#pragma once

#include "PlotterLink.h"

#include <QByteArray>
#include <QMainWindow>
#include <QMap>
#include <QSet>
#include <QString>
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
    void AppendLog(const QString &line);
    void Command(const QString &line);

    void LoadDxf(const QString &path);
    void LoadHpgl(const QString &path);
    QByteArray JobHpgl() const;
    QVector<QVector<QVector3D>> PlacedDxfStrips() const;
    QVector3D DxfPlacementOffset() const;
    void RebuildPreview();
    void UpdatePlotInfo();
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

    // Plotter (from "config"; defaults match the firmware)
    QMap<QString, QString> m_deviceConfig;
    double m_plotWidthMm = 400.0;
    double m_plotHeightMm = 300.0;
    bool m_settingsDialogPending = false;
    QSet<QString> m_discoverySeen;

    // Job
    bool m_jobSending = false;
    bool m_jobAwaitingFinish = false;
    bool m_jobSawBusy = false;
};
