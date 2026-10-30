/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#ifndef __CLEARHPGL_HPGL_CHANNEL_H__
#define __CLEARHPGL_HPGL_CHANNEL_H__

/*
 * One host link (USB, COM, TCP). Portable C++ for the host tests.
 *
 * Raw bytes from the wire go through Ingest(). RS-232 device-control sequences
 * (ESC . x) are answered at once, the way a 7475A answers them ahead of its
 * buffer, so ESC.B reports real free space. PCL/PJL escapes (ESC % ..., ESC E,
 * ESC & ..., ESC * ...) are dropped. Everything else lands in the ring buffer
 * that the interpreter drains with Pop().
 *
 * Flow control: XON/XOFF (configured, or enabled by ESC.N), ENQ/ACK (ESC.H /
 * ESC.I), or none. TCP and USB apply back-pressure by not reading, so they use
 * none.
 */

#include <stdbool.h>
#include <stdint.h>

enum HpglHandshake {
    HPGL_HS_NONE = 0,
    HPGL_HS_XONXOFF = 1,
    HPGL_HS_HARDWARE = 2,  /* RTS/CTS on the UART; nothing to do here */
};

/* ESC.E extended errors (HP 7475A). */
enum HpglExtError {
    HPGL_EXT_NONE = 0,
    HPGL_EXT_BAD_ESC_BYTE = 11,
    HPGL_EXT_BAD_DC_PARAM = 12,
    HPGL_EXT_TOO_MANY_PARAMS = 14,
    HPGL_EXT_BUFFER_OVERFLOW = 16,
};

class HpglChannelIo {
public:
    virtual ~HpglChannelIo() {}
    virtual void ChannelSend(const uint8_t *data, uint16_t len) = 0;
};

class HpglChannel {
public:
    static const uint8_t kXon = 0x11;
    static const uint8_t kXoff = 0x13;

    HpglChannel();
    void Init(HpglChannelIo *io, uint8_t *mem, uint16_t size, uint8_t handshake);
    void SetHandshake(uint8_t handshake);

    /* Returns false when a data byte was dropped because the buffer was full. */
    bool Ingest(uint8_t c);
    bool Pop(uint8_t *c);
    uint16_t Count() const { return m_count; }
    uint16_t Free() const { return (uint16_t)(m_size - m_count); }
    uint16_t Size() const { return m_size; }

    /* XON/XOFF hysteresis and deferred ENQ/ACK. Call after draining. */
    void Poll();
    /* Drop buffered data (abort). Device-control state is kept. */
    void ClearData();
    /* Power-on handshake and output settings. */
    void ResetDeviceControl();

    bool PlotterOn() const { return m_on; }
    /* Reply body followed by the ESC.M output terminator. */
    void SendReply(const char *body);
    void SendRaw(const char *text);
    /* ESC.K was received since the last call. */
    bool TakeAbortRequest();

    uint32_t Overflows() const { return m_overflows; }
    uint32_t BytesIn() const { return m_bytesIn; }
    bool XoffSent() const { return m_xoffSent; }

private:
    enum DcState {
        DC_DATA,
        DC_ESC,
        DC_DOT,
        DC_PARAMS,
        DC_AFTER,
        DC_PCL,
    };

    void ExecuteDeviceControl(char cmd);
    void Push(uint8_t c);
    void SendByte(uint8_t c);
    void SendNumber(int32_t v);
    int32_t Param(uint8_t i, int32_t dflt) const;

    HpglChannelIo *m_io;
    uint8_t *m_mem;
    uint16_t m_size;
    uint16_t m_head;
    uint16_t m_count;

    DcState m_dc;
    char m_dcCmd;
    int32_t m_dcParams[8];
    uint8_t m_dcParamCount;
    bool m_dcHaveDigit;

    bool m_on;
    uint8_t m_configHandshake;
    bool m_xonxoff;
    bool m_xoffSent;
    uint8_t m_xoffChar;
    uint8_t m_xonChars[3];
    uint8_t m_xonLen;
    uint8_t m_enq;
    uint16_t m_enqThreshold;
    uint8_t m_ack[3];
    uint8_t m_ackLen;
    bool m_ackPending;
    uint8_t m_term[2];
    uint8_t m_termLen;
    int m_extError;
    bool m_abortRequested;
    uint32_t m_overflows;
    uint32_t m_bytesIn;
};

#endif
