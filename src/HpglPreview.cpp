#include "HpglPreview.h"

#include "Hpgl.h"
#include "HpglChannel.h"

namespace {

class PreviewSink : public HpglSink {
public:
    explicit PreviewSink(HpglPreviewResult *out) : m_out(out) {}

    int byte = 0;  // job byte being fed when the interpreter emits a move

    void PenUp() override {
        m_down = false;
        m_stripOpen = false;
    }
    void PenDown() override {
        if (!m_down) {
            m_out->penLifts++;
        }
        m_down = true;
    }
    void MoveTo(double xPu, double yPu) override {
        const QVector3D p(float(xPu / HPGL_PU_PER_MM), float(yPu / HPGL_PU_PER_MM), 0.f);
        const float len = (p - m_pos).length();
        if (m_down) {
            if (!m_stripOpen) {
                m_out->cutStripsMm.append(QVector<QVector3D>{m_pos});
                m_out->cutStripBytes.append(QVector<int>{byte});
                m_stripOpen = true;
            }
            m_out->cutStripsMm.last().append(p);
            m_out->cutStripBytes.last().append(byte);
            m_out->drawLengthMm += len;
        } else {
            m_out->rapidSegmentsMm.append(m_pos);
            m_out->rapidSegmentsMm.append(p);
            m_out->travelLengthMm += len;
        }
        m_pos = p;
    }
    void SetVelocity(double) override {}
    void SelectPen(int) override {}
    void Reply(const char *) override {}
    void ActualPosition(double *xPu, double *yPu, bool *penDown) override {
        *xPu = double(m_pos.x()) * HPGL_PU_PER_MM;
        *yPu = double(m_pos.y()) * HPGL_PU_PER_MM;
        *penDown = m_down;
    }
    void PageEnd() override {}
    uint32_t Room() override { return 1u << 30; }

private:
    HpglPreviewResult *m_out;
    QVector3D m_pos;
    bool m_down = false;
    bool m_stripOpen = false;
};

/* Replies (ESC.B, OI, ...) go nowhere in a preview. */
class NullIo : public HpglChannelIo {
public:
    void ChannelSend(const uint8_t *, uint16_t) override {}
};

} // namespace

HpglPreviewResult PreviewHpgl(const QByteArray &hpgl, double plotWidthMm, double plotHeightMm, const char *ident) {
    HpglPreviewResult result;
    PreviewSink sink(&result);
    HpglInterpreter interp;
    interp.Init(&sink, plotWidthMm * HPGL_PU_PER_MM, plotHeightMm * HPGL_PU_PER_MM, ident);
    /* Through the board's input channel, as on the board: it strips PCL/PJL escapes (an HP-GL/2
     * job from a DesignJet-style driver starts with ESC%-1B) and answers ESC. device control.
     * Each byte goes straight through, so the vertex-to-byte mapping stays exact. */
    NullIo io;
    uint8_t ring[64];
    HpglChannel channel;
    channel.Init(&io, ring, sizeof(ring), 0);
    for (const char c : hpgl) {
        channel.Ingest(uint8_t(c));
        sink.byte++;  /* moves record the offset just past the byte that completed them */
        uint8_t b;
        while (channel.Pop(&b)) {
            interp.Feed(b);
            while (interp.Busy()) {
                interp.Pump();
            }
        }
    }
    interp.Feed(';');  /* end of file ends the last instruction, as on the board */
    while (interp.Busy()) {
        interp.Pump();
    }
    result.labelsSkipped = interp.LabelsSkipped();
    result.unknownInstructions = interp.UnknownCount();
    result.lastError = interp.Error();
    return result;
}
