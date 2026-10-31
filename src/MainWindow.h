/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#pragma once

#include "PathVizIssues.h"
#include "PlotterLink.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QMap>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QVector3D>

class QAction;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QSplitter;
class QTabWidget;
class QTextEdit;
class QWidget;
class PositionView3D;

// Host for ClearHPGL_Firmware: open a DXF or HP-GL file, preview it, and plot it
// over Ethernet (TCP 9100 + console 9300) or a serial port. See Firmware/PROTOCOL.md.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void RefreshPorts();
    void Discover();
    void ConnectOrDisconnect();
    void UpdateTransportUi();
    void OpenDrawing();
    void StartPlot();
    void StopPlot();
    void TogglePause();
    void GotoTarget();
    void Jog(double dxMm, double dyMm);
    void ShowPlotterSettings();
    void ExportHpgl();
    void SaveDxfGeometryAs();
    void OnPathIssueQuickFixCloseContour(int stripIndex);
    void OnPathIssueQuickFixMergeAcrossRapid(int rapidPairIndex);

    void OnStatus(const PlotterLink::Status &status);
    void OnTelemetry(const PlotterLink::Status &status);
    void OnCommandFinished(const QString &command, bool ok, const QStringList &lines);
    void OnJobProgress(qint64 sent, qint64 total);
    void OnJobSent();
    void OnJobFailed(const QString &message);
    void OnLinkLost(const QString &reason);

private:
    enum class SourceKind { None, Dxf, Hpgl };

    void BuildUi();
    void SetConnectedUi(bool connected);
    void UpdateMachineButtons();
    void UpdatePenView(const PlotterLink::Status &status);
    void StartCycleTimer();
    void StopCycleTimer(const QString &outcome);
    void ShowCycleTime();
    void PlaceStateOverlay();
    void AppendLog(const QString &line);
    void Command(const QString &line);

    void LoadDxf(const QString &path);
    void LoadHpgl(const QString &path);
    QByteArray JobHpgl() const;
    QVector<QVector<QVector3D>> PlacedDxfStrips() const;
    // The board's model ("ident") and whether it puts plotter 0,0 at the bed centre (DraftMaster).
    QByteArray DeviceIdent() const;
    bool DeviceCenterOrigin() const;
    // DXF strips in the plotter's coordinate frame (bed coordinates, shifted for a centre origin).
    QVector<QVector<QVector3D>> JobDxfStrips() const;
    QVector3D DxfPlacementOffset() const;
    void RebuildPreview();
    void UpdatePlotInfo();
    void BuildProgressPlan();
    void ResetProgress();
    void UpdateProgress(const PlotterLink::Status &status);
    void PushProgressToView(bool allDone = false);
    void SetPreviewText(const QByteArray &job);
    void ShowActiveLine(int line);
    void TrackExternalJob(const PlotterLink::Status &s, bool faulted);
    void ApplyDeviceConfig(const QStringList &lines);
    void ShowSettingsDialog(const QStringList &configLines);
    void PromptSaveDxfAfterGeometryFix();
    QString SuggestedDxfSavePath() const;

    void SetLogPanelVisible(bool visible);
    void LoadWindowUiSettings();
    void SaveWindowUiSettings();

    PlotterLink m_link;

    // Connection
    QComboBox *m_transportCombo = nullptr;
    QComboBox *m_portCombo = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QComboBox *m_flowCombo = nullptr;
    QComboBox *m_deviceCombo = nullptr;
    QPushButton *m_refreshPortsButton = nullptr;
    QPushButton *m_discoverButton = nullptr;
    QPushButton *m_connectButton = nullptr;
    QWidget *m_serialSettingsWidget = nullptr;
    QWidget *m_ethernetSettingsWidget = nullptr;

    // Machine control
    QPushButton *m_enableButton = nullptr;
    QPushButton *m_disableButton = nullptr;
    QPushButton *m_abortButton = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_originButton = nullptr;
    QPushButton *m_clearFaultButton = nullptr;
    QLabel *m_stateLabel = nullptr;

    // Jog tab
    QLineEdit *m_gotoXEdit = nullptr;
    QLineEdit *m_gotoYEdit = nullptr;
    QPushButton *m_gotoButton = nullptr;
    QLineEdit *m_jogStepEdit = nullptr;
    QList<QPushButton *> m_jogButtons;
    QPushButton *m_penUpButton = nullptr;
    QPushButton *m_penDownButton = nullptr;

    // Plot tab
    QPushButton *m_openButton = nullptr;
    QPushButton *m_plotButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QCheckBox *m_placeAtOriginCheck = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_cycleLabel = nullptr;
    QLabel *m_plotInfoLabel = nullptr;
    QTextEdit *m_preview = nullptr;
    QAction *m_saveDxfGeometryAction = nullptr;
    QAction *m_exportHpglAction = nullptr;

    // Log, layout, 3D
    QTextEdit *m_logEdit = nullptr;
    QGroupBox *m_logGroup = nullptr;
    QAction *m_toggleLogAction = nullptr;
    QPushButton *m_showLogButton = nullptr;
    QSplitter *m_workAreaSplitter = nullptr;
    QSplitter *m_mainVSplitter = nullptr;
    QTabWidget *m_workTabs = nullptr;
    QWidget *m_plotTab = nullptr;
    PositionView3D *m_positionView3D = nullptr;
    QCheckBox *m_pathCheck = nullptr;
    QCheckBox *m_lockTopDownViewCheck = nullptr;
    QCheckBox *m_leftDragOrbitCheck = nullptr;
    QCheckBox *m_followPenCheck = nullptr;
    QCheckBox *m_issuesCheck = nullptr;
    QVector<PathIssueMarker> m_pathIssueMarkers;  // drawing-check results; shown only with Issues on
    bool m_didWorkAreaInitialSplit = false;
    int m_savedLogPaneHeight = 320;

    // Drawing
    SourceKind m_source = SourceKind::None;
    QString m_sourcePath;
    QVector<QVector<QVector3D>> m_dxfPolylinesMm;
    QVector<QVector3D> m_dxfRapidSegmentsMm;
    int m_dxfInsunits = 0;
    double m_dxfDrawingToMmScale = 25.4;
    QByteArray m_hpglFile;
    QVector<QVector<QVector3D>> m_previewStripsMm;
    QVector<QVector3D> m_previewRapidsMm;
    // Active-line tracking in the HP-GL box: per preview vertex, the job byte of the instruction
    // that draws it; per displayed line, the job byte it starts at.
    QVector<QVector<int>> m_previewVertexBytes;
    QVector<int> m_previewLineBytes;
    int m_activeLine = -1;

    // Plotter (from "config"; defaults match the firmware)
    QMap<QString, QString> m_deviceConfig;
    double m_plotWidthMm = 400.0;
    double m_plotHeightMm = 300.0;
    bool m_settingsDialogPending = false;
    QSet<QString> m_discoverySeen;

    // Plot progress: the planned path as ordered segments (cuts, plus the pen-up hop to each
    // strip start), and how far along it the reported pen position has been matched.
    struct PlanSeg {
        QVector3D a;
        QVector3D b;
        int strip = -1;   // cut strip index; for a pen-up hop, the strip it leads to
        int vertex = 0;   // cut: index of a within the strip
        bool cut = false;
        double cumStart = 0.0;
    };
    QVector<PlanSeg> m_plan;
    int m_progressSeg = -1;
    float m_progressT = 0.f;
    qint64 m_progressMs = 0;

    // Cycle timer: Plot pressed -> run finished (complete, stopped, fault or failure).
    QElapsedTimer m_cycleClock;
    QTimer m_cycleTick;
    bool m_cycleRunning = false;

    // Job
    bool m_jobSending = false;
    bool m_jobAwaitingFinish = false;
    bool m_jobSawBusy = false;
    // A job another source (USB or COM, e.g. "copy /b file.plt COM3") is plotting while this app
    // is connected over Ethernet: progress, highlight and cycle timer follow it as if sent here,
    // assuming it is the drawing loaded in the app.
    bool m_externalJob = false;
    QString m_externalSource;
    qint64 m_jobSentMs = 0;
    int m_progressShapeShown = -1;
};
