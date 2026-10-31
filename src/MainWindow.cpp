#include "MainWindow.h"

#include "DxfImport.h"
#include "HpglPreview.h"
#include "PathGapBridge.h"
#include "PathVizIssues.h"
#include "PlotterSettingsDialog.h"
#include "PositionView3D.h"

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSerialPortInfo>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QEvent>
#include <QTimer>
#include <algorithm>
#include <QTextBlock>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWidget>

#include <cfloat>
#include <utility>

namespace {
constexpr int kMaxLogBlocks = 3000;
constexpr int kMaxPreviewChars = 200000;
constexpr float kPenUpVizZ = 2.0f;

QSettings AppSettings() {
    return QSettings("ClearHPGL", "ClearHPGL");
}

// Display only: one instruction per line (drivers such as AutoCAD's write the whole job
// without line breaks) and visible control characters. The job itself is sent unchanged.
// lineBytes (optional) receives the job byte offset at which each displayed line starts.
QString FormatHpglForDisplay(const QByteArray &hpgl, int maxChars, QVector<int> *lineBytes = nullptr) {
    QString out;
    out.reserve(qMin(int(hpgl.size() + hpgl.size() / 8), maxChars + 64));
    if (lineBytes) {
        lineBytes->clear();
        lineBytes->append(0);
    }
    auto newLine = [&](int nextByte) {
        out += QLatin1Char('\n');
        if (lineBytes) {
            lineBytes->append(nextByte);
        }
    };
    for (int i = 0; i < hpgl.size(); i++) {
        if (out.size() >= maxChars) {
            out += QStringLiteral("\n... (%1 more bytes not shown)").arg(hpgl.size() - i);
            break;
        }
        const uchar c = uchar(hpgl.at(i));
        if (c == ';') {
            out += QLatin1Char(';');
            const uchar next = (i + 1 < hpgl.size()) ? uchar(hpgl.at(i + 1)) : 0;
            if (next != '\r' && next != '\n') {
                newLine(i + 1);
            }
        } else if (c == '\r') {
            if (i + 1 >= hpgl.size() || hpgl.at(i + 1) != '\n') {
                newLine(i + 1);
            }
        } else if (c == '\n') {
            newLine(i + 1);
        } else if (c == '\t' || c >= 0x20) {
            out += QChar(c);
        } else if (c == 0x1b) {
            out += QStringLiteral("<ESC>");
        } else if (c == 0x03) {
            out += QStringLiteral("<ETX>");
        } else {
            out += QStringLiteral("<%1>").arg(c, 2, 16, QLatin1Char('0'));
        }
    }
    return out;
}

// setStyleSheet() re-polishes the widget; the status handler runs ~3x/s, so only apply real changes.
void SetSheet(QWidget *w, const QString &sheet) {
    if (w->styleSheet() != sheet) {
        w->setStyleSheet(sheet);
    }
}

double ParseDoubleLine(const QLineEdit *e, double defaultValue) {
    if (!e) {
        return defaultValue;
    }
    bool ok = false;
    const double v = e->text().trimmed().toDouble(&ok);
    return ok ? v : defaultValue;
}

const char *const kIdleButton =
    "QPushButton { background: #3a3f46; color: #dce1e8; border: 1px solid #23272d; border-radius: 2px; font-weight: 600; }"
    "QPushButton:hover { border-color: #586473; background: #424850; }"
    "QPushButton:disabled { background: #2d3238; color: #7b8490; border-color: #252a31; }";
const char *const kGreenButton =
    "QPushButton { background: #1e4a28; color: #e8f8ec; border: 1px solid #3a9d55; border-radius: 2px; font-weight: 700; }"
    "QPushButton:hover { background: #255a32; border-color: #4fc06e; }"
    "QPushButton:disabled { background: #2a332e; color: #7a9080; border-color: #3a4a3f; }";
const char *const kOrangeButton =
    "QPushButton { background: #5a3a18; color: #fff0e0; border: 1px solid #c97a2c; border-radius: 2px; font-weight: 700; }"
    "QPushButton:hover { background: #6d4720; border-color: #e89640; }"
    "QPushButton:disabled { background: #3a3028; color: #a89888; border-color: #5a4a3a; }";
const char *const kRedButton =
    "QPushButton { background: #8e1d1d; color: #ffebee; border: 1px solid #e57373; border-radius: 2px; font-weight: 800; }"
    "QPushButton:hover { background: #a82424; border-color: #ffcdd2; }"
    "QPushButton:disabled { background: #4a2a2a; color: #b09090; border-color: #5d3a3a; }";
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    m_plotWidthMm = AppSettings().value("plotter/widthMm", 400.0).toDouble();
    m_plotHeightMm = AppSettings().value("plotter/heightMm", 300.0).toDouble();

    BuildUi();
    RefreshPorts();
    SetConnectedUi(false);

    m_cycleTick.setInterval(100);
    connect(&m_cycleTick, &QTimer::timeout, this, &MainWindow::ShowCycleTime);
    connect(&m_link, &PlotterLink::statusUpdated, this, &MainWindow::OnStatus);
    connect(&m_link, &PlotterLink::telemetryUpdated, this, &MainWindow::OnTelemetry);
    connect(&m_link, &PlotterLink::commandFinished, this, &MainWindow::OnCommandFinished);
    connect(&m_link, &PlotterLink::jobProgress, this, &MainWindow::OnJobProgress);
    connect(&m_link, &PlotterLink::jobSent, this, &MainWindow::OnJobSent);
    connect(&m_link, &PlotterLink::jobFailed, this, &MainWindow::OnJobFailed);
    connect(&m_link, &PlotterLink::linkLost, this, &MainWindow::OnLinkLost);
    connect(&m_link, &PlotterLink::logLine, this, &MainWindow::AppendLog);
    connect(&m_link, &PlotterLink::deviceDiscovered, this, [this](const QString &host, const QString &info) {
        if (m_deviceCombo->findData(host) < 0) {
            m_deviceCombo->addItem(QStringLiteral("%1  (%2)").arg(host, info.section(QLatin1Char(' '), 1, 1)), host);
        }
        m_deviceCombo->setCurrentIndex(m_deviceCombo->findData(host));
        if (!m_discoverySeen.contains(host)) {
            m_discoverySeen.insert(host);
            AppendLog(QStringLiteral("RX UDP %1: %2").arg(host, info));
        }
    });
}

MainWindow::~MainWindow() {
    m_link.disconnectLink();
}

void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    if (m_didWorkAreaInitialSplit || !m_workAreaSplitter) {
        return;
    }
    // After the first layout pass, widths are real (sync showEvent is often 0 on first show).
    QTimer::singleShot(0, this, [this]() {
        if (m_didWorkAreaInitialSplit || !m_workAreaSplitter) {
            return;
        }
        int w = m_workAreaSplitter->width();
        if (w < 8) {
            w = qMax(8, (centralWidget() ? centralWidget()->width() : width()) - 24);
        }
        if (w < 8) {
            return;
        }
        // Left column: the width it was last dragged to (default 430 px), never wider than 45%
        // of the window, so opening maximized does not start with a wide column.
        const int savedLeft = AppSettings().value("ui/leftColumnWidth", 430).toInt();
        const int leftW = qBound(m_workAreaSplitter->widget(0)->minimumSizeHint().width(), savedLeft, (w * 45) / 100);
        m_workAreaSplitter->setSizes({leftW, w - leftW});
        // Left column: plotter panel at its natural height, log ~28% (or the remembered
        // height if smaller, never under 150), the drawing / HP-GL tab gets the rest.
        if (m_mainVSplitter && m_logGroup && m_logGroup->isVisible()) {
            const int total = m_mainVSplitter->height();
            const int ctrlH = m_mainVSplitter->widget(0)->sizeHint().height();
            const int logH = qBound(150, qMin(m_savedLogPaneHeight, (total * 28) / 100), qMax(150, total / 2));
            m_mainVSplitter->setSizes({ctrlH, qMax(200, total - ctrlH - logH), logH});
        }
        m_didWorkAreaInitialSplit = true;
    });
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_link.jobActive()) {
        const auto r = QMessageBox::question(
            this, QStringLiteral("ClearHPGL"),
            QStringLiteral("A plot is still being sent. Closing stops sending; the plotter finishes what it has "
                           "buffered. Close anyway?"));
        if (r != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }
    SaveWindowUiSettings();
    m_link.disconnectLink();
    event->accept();
}

/* ------------------------------------------------------------------------- */
/* UI                                                                         */
/* ------------------------------------------------------------------------- */

void MainWindow::BuildUi() {
    resize(1280, 820);
    setMinimumSize(1080, 760);
    setStyleSheet(
        "QMainWindow { background: #25292f; border: none; }"
        "QMenuBar { background: #32363c; color: #d6d9de; border: none; border-bottom: 1px solid #20242a; padding: 4px 8px; }"
        "QMenuBar::item { padding: 6px 14px; border-radius: 2px; }"
        "QMenuBar::item:selected { background: #444b54; color: #f0f2f5; }"
        "QMenu { background: #2f343a; color: #d8dce2; border: 1px solid #20242a; padding: 4px 0; }"
        "QMenu::item { padding: 8px 32px 8px 16px; }"
        "QMenu::item:selected { background: #49525e; color: #f1f4f8; }"
        "QMenu::item:disabled { color: #6f7884; }"
        "QWidget { color: #dde2e8; font-size: 12px; }"
        "QGroupBox { background: #2f343a; border: 1px solid #1a1e24; border-radius: 3px; margin-top: 20px; padding-top: 2px; font-weight: 600; color: #e6ebf3; }"
        "QGroupBox::title {"
        " subcontrol-origin: margin; subcontrol-position: top left; left: 14px; top: 2px; padding: 0 6px; margin-top: 0px;"
        " color: #c9d1dc; background: transparent; border: none; font-weight: 600; }"
        "QPushButton { background: #3a3f46; color: #dce1e8; border: 1px solid #23272d; border-radius: 2px; min-height: 28px; padding: 4px 12px; font-weight: 600; }"
        "QPushButton:hover { border-color: #586473; background: #424850; }"
        "QPushButton:pressed { background: #30353b; border-color: #6d7a8c; }"
        "QPushButton:disabled { background: #2d3238; color: #7b8490; border-color: #252a31; }"
        "QLineEdit, QComboBox, QDoubleSpinBox { background: #282d34; color: #e5e9ef; border: 1px solid #20242a; border-radius: 2px; min-height: 26px; padding: 3px 8px; selection-background-color: #5a82c2; }"
        "QLineEdit:disabled, QComboBox:disabled, QDoubleSpinBox:disabled { background: #242930; color: #aeb7c4; border-color: #20242a; }"
        "QLineEdit:read-only { color: #dbe2ec; background: #242930; }"
        "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: right center; border: none; width: 28px; background: #2d3239; border-left: 1px solid #20242a; border-top-right-radius: 2px; border-bottom-right-radius: 2px; }"
        "QComboBox::drop-down:hover { background: #353b44; }"
        "QComboBox::down-arrow { image: url(:/icons/combo-down.svg); width: 12px; height: 8px; border: none; }"
        "QComboBox:hover { background: #2c3138; border-color: #3d4650; }"
        "QComboBox:focus { border-color: #5a82c2; }"
        "QComboBox QAbstractItemView { background: #282d34; color: #e5e9ef; border: 1px solid #1a1e24; selection-background-color: #3d5a8c; selection-color: #f0f4f8; outline: 0; padding: 2px; }"
        "QComboBox QAbstractItemView::item { min-height: 24px; padding: 4px 8px; }"
        "QComboBox QAbstractItemView::item:selected { background: #3d5a8c; }"
        "QPushButton[role=\"primary\"] { background: #274874; color: #eef5ff; border: 1px solid #5a8fd4; font-weight: 700; }"
        "QPushButton[role=\"primary\"]:hover { background: #2e5385; border-color: #6aa0e0; }"
        "QPushButton[role=\"primary\"]:pressed { background: #1f3d5f; border-color: #4a7ec4; }"
        "QPushButton[role=\"primary\"]:disabled { background: #2d3238; color: #7a8595; border-color: #2f3540; }"
        "QTextEdit { background: #1f242b; color: #e6ebf3; border: 1px solid #20242a; border-radius: 2px; selection-background-color: #5a82c2; font-family: Consolas, 'Courier New', monospace; }"
        "QProgressBar { background: #1f242b; border: 1px solid #20242a; border-radius: 2px; min-height: 18px; text-align: center; color: #e6ebf3; }"
        "QProgressBar::chunk { background: #2e5385; }"
        "QCheckBox::indicator { width: 14px; height: 14px; }"
        "QTabWidget::pane { border: none; background: #25292f; margin-top: 8px; }"
        "QTabBar { background: #2f343a; border: 1px solid #20242a; border-radius: 2px; padding: 2px; }"
        "QTabBar::tab { background: transparent; color: #aeb6c2; border: none; padding: 8px 16px; min-width: 72px; font-weight: 700; border-radius: 2px; }"
        "QTabBar::tab:selected { color: #f0f4f8; background: #3a3f46; border: 1px solid #4b5563; }"
        "QTabBar::tab:hover:!selected { background: #363c44; color: #d1d7df; }"
        "QSplitter { border: none; }"
        "QSplitter::handle:vertical { background: #20242a; height: 3px; }"
        "QSplitter::handle:vertical:hover { background: #3f4853; }"
        "QSplitter::handle:horizontal { background: #20242a; width: 3px; }"
        "QSplitter::handle:horizontal:hover { background: #3f4853; }"
        "QFrame#ClearHpglControlSep { color: #4a5563; }"
        "QLabel#ClearHpglState { background: rgba(20, 23, 28, 200); color: #e6ebf3; border: 1px solid #3a414c; border-radius: 3px; padding: 4px 10px; font-family: Consolas, 'Courier New', monospace; }"
        "QStatusBar { background: #2c3036; color: #9fa6af; border-top: 1px solid #20242a; padding: 6px 14px; }"
        "QStatusBar::item { border: none; }");

    // Menus
    auto *fileMenu = menuBar()->addMenu("&File");
    auto *openAction = fileMenu->addAction("Open drawing (DXF / HP-GL)...");
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::OpenDrawing);
    m_exportHpglAction = fileMenu->addAction("Export HP-GL...");
    m_exportHpglAction->setEnabled(false);
    connect(m_exportHpglAction, &QAction::triggered, this, &MainWindow::ExportHpgl);
    m_saveDxfGeometryAction = fileMenu->addAction(QStringLiteral("Save DXF geometry…"));
    m_saveDxfGeometryAction->setEnabled(false);
    connect(m_saveDxfGeometryAction, &QAction::triggered, this, &MainWindow::SaveDxfGeometryAs);

    auto *setupMenu = menuBar()->addMenu("&Plotter");
    auto *settingsAction = setupMenu->addAction("Plotter settings...");
    connect(settingsAction, &QAction::triggered, this, &MainWindow::ShowPlotterSettings);

    auto *viewMenu = menuBar()->addMenu("&View");
    m_toggleLogAction = new QAction("Command &log", this);
    m_toggleLogAction->setCheckable(true);
    m_toggleLogAction->setChecked(true);
    m_toggleLogAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    viewMenu->addAction(m_toggleLogAction);
    connect(m_toggleLogAction, &QAction::toggled, this, [this](bool on) { SetLogPanelVisible(on); });

    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    // Connection
    auto *connGroup = new QGroupBox("Connection", central);
    connGroup->setMinimumHeight(88);
    auto *connLayout = new QHBoxLayout(connGroup);
    connLayout->setContentsMargins(14, 18, 14, 12);
    connLayout->setSpacing(10);
    m_transportCombo = new QComboBox(connGroup);
    m_transportCombo->addItems({"Ethernet", "Serial"});
    const int savedTransportIdx =
        m_transportCombo->findText(AppSettings().value("connection/transport", "Ethernet").toString());
    if (savedTransportIdx >= 0) {
        m_transportCombo->setCurrentIndex(savedTransportIdx);
    }
    m_portCombo = new QComboBox(connGroup);
    m_baudCombo = new QComboBox(connGroup);
    m_baudCombo->addItems({"9600", "19200", "38400", "57600", "115200"});
    const int baudIdx = m_baudCombo->findText(AppSettings().value("connection/baud", "115200").toString());
    m_baudCombo->setCurrentIndex(baudIdx >= 0 ? baudIdx : m_baudCombo->count() - 1);
    m_baudCombo->setToolTip("USB ignores the baud rate. COM-0/1 must match the com_baud setting.");
    m_flowCombo = new QComboBox(connGroup);
    m_flowCombo->addItems({"No flow control", "XON/XOFF", "RTS/CTS"});
    m_flowCombo->setCurrentIndex(qBound(0, AppSettings().value("connection/flow", 0).toInt(), 2));
    m_flowCombo->setToolTip("USB needs none. A COM port needs the same handshake as com_handshake.");
    m_deviceCombo = new QComboBox(connGroup);
    m_deviceCombo->setMinimumWidth(210);
    m_deviceCombo->setEditable(true);
    m_deviceCombo->setToolTip("Discover, or type the plotter's IP address.");
    const QString savedHost = AppSettings().value("connection/ethernetHost").toString();
    if (!savedHost.isEmpty()) {
        m_deviceCombo->addItem(savedHost, savedHost);
    }
    m_refreshPortsButton = new QPushButton("Refresh", connGroup);
    m_discoverButton = new QPushButton("Discover", connGroup);
    m_connectButton = new QPushButton("Connect", connGroup);
    m_connectButton->setProperty("role", "primary");

    m_serialSettingsWidget = new QWidget(connGroup);
    auto *serialLayout = new QHBoxLayout(m_serialSettingsWidget);
    serialLayout->setContentsMargins(0, 0, 0, 0);
    serialLayout->setSpacing(10);
    serialLayout->addWidget(new QLabel("Port:", m_serialSettingsWidget));
    serialLayout->addWidget(m_portCombo, 2);
    serialLayout->addWidget(new QLabel("Baud:", m_serialSettingsWidget));
    serialLayout->addWidget(m_baudCombo, 1);
    serialLayout->addWidget(m_flowCombo, 1);
    serialLayout->addWidget(m_refreshPortsButton);

    m_ethernetSettingsWidget = new QWidget(connGroup);
    auto *ethLayout = new QHBoxLayout(m_ethernetSettingsWidget);
    ethLayout->setContentsMargins(0, 0, 0, 0);
    ethLayout->setSpacing(10);
    ethLayout->addWidget(new QLabel("Plotter:", m_ethernetSettingsWidget));
    ethLayout->addWidget(m_deviceCombo, 2);
    ethLayout->addWidget(m_discoverButton);

    connLayout->addWidget(new QLabel("Transport:", connGroup));
    connLayout->addWidget(m_transportCombo);
    connLayout->addWidget(m_serialSettingsWidget, 3);
    connLayout->addWidget(m_ethernetSettingsWidget, 3);
    connLayout->addWidget(m_connectButton);
    root->addWidget(connGroup);

    auto *workH = new QSplitter(Qt::Horizontal, central);
    workH->setHandleWidth(3);
    workH->setChildrenCollapsible(false);
    m_workAreaSplitter = workH;

    auto *mainSplitter = new QSplitter(Qt::Vertical, central);
    mainSplitter->setHandleWidth(3);
    mainSplitter->setChildrenCollapsible(false);
    m_mainVSplitter = mainSplitter;

    // Machine control
    auto *controlGroup = new QGroupBox("Plotter", mainSplitter);
    controlGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    auto *controlLayout = new QVBoxLayout(controlGroup);
    controlLayout->setContentsMargins(14, 16, 14, 10);
    controlLayout->setSpacing(8);
    m_enableButton = new QPushButton("Enable", controlGroup);
    m_disableButton = new QPushButton("Disable", controlGroup);
    m_abortButton = new QPushButton("Abort", controlGroup);
    m_abortButton->setToolTip("Stop now, drop the current job and raise the pen (console: abort).");
    m_abortButton->setStyleSheet(kRedButton);
    m_pauseButton = new QPushButton("Pause", controlGroup);
    m_pauseButton->setToolTip("Hold at the next segment boundary with the pen raised.");
    m_originButton = new QPushButton("Set origin", controlGroup);
    m_originButton->setToolTip("The current pen position becomes plotter 0,0 (console: origin).");
    m_clearFaultButton = new QPushButton("Clear fault", controlGroup);
    for (auto *b : {m_enableButton, m_disableButton, m_abortButton, m_pauseButton, m_originButton,
                    m_clearFaultButton}) {
        b->setMinimumHeight(30);
        b->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    }
    // Two rows keep the left column narrow enough for the 3D view to get most of the width.
    auto *controlRow = new QHBoxLayout();
    controlRow->setSpacing(6);
    controlRow->addWidget(m_enableButton);
    controlRow->addWidget(m_disableButton);
    controlRow->addWidget(m_abortButton);
    controlRow->addWidget(m_pauseButton);
    controlRow->addStretch(1);
    controlLayout->addLayout(controlRow);
    auto *controlRow2 = new QHBoxLayout();
    controlRow2->setSpacing(6);
    controlRow2->addWidget(m_originButton);
    controlRow2->addWidget(m_clearFaultButton);
    controlRow2->addStretch(1);
    controlLayout->addLayout(controlRow2);

    auto *workTabs = new QTabWidget(mainSplitter);

    // Plot tab
    auto *plotTab = new QWidget(workTabs);
    auto *plotTabLayout = new QVBoxLayout(plotTab);
    plotTabLayout->setContentsMargins(10, 10, 10, 10);
    plotTabLayout->setSpacing(10);
    auto *plotGroup = new QGroupBox("Drawing (DXF or HP-GL)", plotTab);
    auto *plotLayout = new QVBoxLayout(plotGroup);
    plotLayout->setContentsMargins(14, 18, 14, 12);
    auto *plotControls = new QHBoxLayout();
    m_openButton = new QPushButton("Open...", plotGroup);
    m_placeAtOriginCheck = new QCheckBox("Place DXF at origin", plotGroup);
    m_placeAtOriginCheck->setToolTip(
        "Shift the DXF so its lower-left corner sits at plotter 0,0. Geometry with negative coordinates "
        "would otherwise be clipped at the plot-area edge. HP-GL files are always sent as-is.");
    m_placeAtOriginCheck->setChecked(AppSettings().value("plot/placeAtOrigin", true).toBool());
    m_plotButton = new QPushButton("Plot", plotGroup);
    m_plotButton->setProperty("role", "primary");
    m_stopButton = new QPushButton("Stop", plotGroup);
    m_stopButton->setToolTip("Stop sending and abort the plot on the board.");
    plotControls->addWidget(m_openButton);
    plotControls->addStretch(1);
    plotControls->addWidget(m_plotButton);
    plotControls->addWidget(m_stopButton);
    m_progress = new QProgressBar(plotGroup);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setFormat("%p% sent");
    m_plotInfoLabel = new QLabel("No drawing loaded", plotGroup);
    m_plotInfoLabel->setWordWrap(true);
    m_preview = new QTextEdit(plotGroup);
    m_preview->setReadOnly(true);
    m_preview->setLineWrapMode(QTextEdit::NoWrap);
    m_preview->setPlaceholderText(
        "Open an ASCII DXF (converted to HP-GL) or an HP-GL file (.plt, .hpgl, .hgl). The 3D view shows what "
        "the plotter will draw, using the firmware's own HP-GL interpreter.");
    plotLayout->addLayout(plotControls);
    plotLayout->addWidget(m_placeAtOriginCheck);
    m_cycleLabel = new QLabel("Cycle  -:--.-", plotGroup);
    m_cycleLabel->setToolTip("Run time from pressing Plot until the plot completes or stops.");
    m_cycleLabel->setMinimumWidth(130);
    m_cycleLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_cycleLabel->setStyleSheet("QLabel { font-family: Consolas, 'Courier New', monospace; font-size: 14px; "
                                "font-weight: 700; color: #e6ebf3; }");
    auto *progressRow = new QHBoxLayout();
    progressRow->addWidget(m_progress, 1);
    progressRow->addWidget(m_cycleLabel);
    plotLayout->addLayout(progressRow);
    plotLayout->addWidget(m_plotInfoLabel);
    plotLayout->addWidget(m_preview, 1);
    plotTabLayout->addWidget(plotGroup, 1);

    // Jog tab
    auto *jogTab = new QWidget(workTabs);
    auto *jogTabLayout = new QVBoxLayout(jogTab);
    jogTabLayout->setContentsMargins(10, 10, 10, 10);
    jogTabLayout->setSpacing(10);
    auto *gotoGroup = new QGroupBox("Go to (pen up)", jogTab);
    auto *gotoLayout = new QGridLayout(gotoGroup);
    gotoLayout->setContentsMargins(14, 18, 14, 12);
    gotoLayout->setHorizontalSpacing(8);
    m_gotoXEdit = new QLineEdit("0.000", gotoGroup);
    m_gotoYEdit = new QLineEdit("0.000", gotoGroup);
    m_gotoButton = new QPushButton("Go", gotoGroup);
    m_gotoButton->setProperty("role", "primary");
    gotoLayout->addWidget(new QLabel("X (mm):", gotoGroup), 0, 0);
    gotoLayout->addWidget(m_gotoXEdit, 0, 1);
    gotoLayout->addWidget(new QLabel("Y (mm):", gotoGroup), 0, 2);
    gotoLayout->addWidget(m_gotoYEdit, 0, 3);
    gotoLayout->addWidget(m_gotoButton, 0, 4);
    gotoLayout->setColumnStretch(1, 1);
    gotoLayout->setColumnStretch(3, 1);
    jogTabLayout->addWidget(gotoGroup);

    auto *jogGroup = new QGroupBox("Jog", jogTab);
    auto *jogLayout = new QGridLayout(jogGroup);
    jogLayout->setContentsMargins(14, 18, 14, 12);
    jogLayout->setHorizontalSpacing(8);
    m_jogStepEdit = new QLineEdit("10.000", jogGroup);
    auto *stepRow = new QHBoxLayout();
    stepRow->addWidget(new QLabel("Step (mm):", jogGroup));
    stepRow->addWidget(m_jogStepEdit, 1);
    jogLayout->addLayout(stepRow, 0, 0, 1, 4);
    const struct {
        const char *text;
        double dx;
        double dy;
    } jogs[] = {{"X-", -1, 0}, {"X+", 1, 0}, {"Y-", 0, -1}, {"Y+", 0, 1}};
    for (int i = 0; i < 4; i++) {
        auto *b = new QPushButton(jogs[i].text, jogGroup);
        b->setMinimumHeight(32);
        jogLayout->addWidget(b, 1, i);
        jogLayout->setColumnStretch(i, 1);
        const double dx = jogs[i].dx;
        const double dy = jogs[i].dy;
        connect(b, &QPushButton::clicked, this, [this, dx, dy]() {
            const double step = ParseDoubleLine(m_jogStepEdit, 1.0);
            Jog(dx * step, dy * step);
        });
        m_jogButtons << b;
    }
    m_penUpButton = new QPushButton("Pen up", jogGroup);
    m_penDownButton = new QPushButton("Pen down", jogGroup);
    jogLayout->addWidget(m_penUpButton, 2, 0, 1, 2);
    jogLayout->addWidget(m_penDownButton, 2, 2, 1, 2);
    jogTabLayout->addWidget(jogGroup);
    jogTabLayout->addStretch(1);

    m_workTabs = workTabs;
    m_plotTab = plotTab;
    workTabs->addTab(plotTab, "Plot");
    workTabs->addTab(jogTab, "Jog");

    // Log
    auto *logGroup = new QGroupBox("Log", mainSplitter);
    m_logGroup = logGroup;
    logGroup->setMinimumHeight(150);
    auto *logLayout = new QVBoxLayout(logGroup);
    logLayout->setContentsMargins(14, 18, 14, 12);
    auto *logHeader = new QHBoxLayout();
    auto *minimizeLog = new QPushButton("Minimize", logGroup);
    auto *clearLog = new QPushButton("Clear", logGroup);
    logHeader->addWidget(new QLabel("Activity", logGroup));
    logHeader->addStretch();
    logHeader->addWidget(minimizeLog);
    logHeader->addWidget(clearLog);
    m_logEdit = new QTextEdit(logGroup);
    m_logEdit->setReadOnly(true);
    m_logEdit->document()->setMaximumBlockCount(kMaxLogBlocks);
    QFont logFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    logFont.setPointSize(logFont.pointSize() - 1);
    m_logEdit->setFont(logFont);
    logLayout->addLayout(logHeader);
    logLayout->addWidget(m_logEdit);
    mainSplitter->setCollapsible(0, false);
    mainSplitter->setCollapsible(1, false);
    mainSplitter->setCollapsible(2, true);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 5);  // the drawing / HP-GL view gets most of the height
    mainSplitter->setStretchFactor(2, 1);
    mainSplitter->setSizes({140, 560, 180});
    workH->addWidget(mainSplitter);

    // 3D view
    auto *pos3dGroup = new QGroupBox("Plot area", central);
    pos3dGroup->setMinimumWidth(300);
    auto *pos3dOuter = new QVBoxLayout(pos3dGroup);
    pos3dOuter->setContentsMargins(12, 18, 12, 12);
    pos3dOuter->setSpacing(6);
    auto *pos3dHeader = new QHBoxLayout();
    // The hint may be clipped; it must not set the header's minimum width (that would push the
    // left column wider).
    auto *viewHint = new QLabel("Shift/middle/right = pan · wheel = zoom", pos3dGroup);
    viewHint->setToolTip(viewHint->text());
    viewHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    viewHint->setMinimumWidth(0);
    pos3dHeader->addWidget(viewHint, 1);
    pos3dHeader->addStretch();
    m_pathCheck = new QCheckBox("Path", pos3dGroup);
    m_pathCheck->setToolTip(
        "Yellow = pen down, gray = pen up.");
    m_pathCheck->setChecked(true);
    pos3dHeader->addWidget(m_pathCheck);
    m_issuesCheck = new QCheckBox("Issues", pos3dGroup);
    m_issuesCheck->setToolTip(
        "Mark drawing-check findings with red crosses: unjoined line ends, overlaps and tight corners. "
        "Right-click a cross for DXF quick fixes. Line ends are normal in plots, so this is off by default.");
    m_issuesCheck->setChecked(AppSettings().value("ui/showPathIssues", false).toBool());
    pos3dHeader->addWidget(m_issuesCheck);
    m_lockTopDownViewCheck = new QCheckBox("Top-down", pos3dGroup);
    pos3dHeader->addWidget(m_lockTopDownViewCheck);
    m_leftDragOrbitCheck = new QCheckBox("L-drag rotate", pos3dGroup);
    pos3dHeader->addWidget(m_leftDragOrbitCheck);
    m_followPenCheck = new QCheckBox("Follow pen", pos3dGroup);
    m_followPenCheck->setToolTip(
        "On: the view follows the pen and the drawing slides under it. "
        "Off: the drawing stays still and the pen moves across it; Reset view centres the drawing.");
    pos3dHeader->addWidget(m_followPenCheck);
    auto *reset3d = new QPushButton("Reset view", pos3dGroup);
    pos3dHeader->addWidget(reset3d);
    pos3dOuter->addLayout(pos3dHeader);
    m_positionView3D = new PositionView3D(pos3dGroup);
    m_positionView3D->setDroInches(false);
    m_positionView3D->setBedSize(float(m_plotWidthMm), float(m_plotHeightMm));
    m_positionView3D->resetView();
    // Plotter state overlaid on the viewport (bottom-left) so its changing width never resizes a panel.
    m_stateLabel = new QLabel("Not connected", m_positionView3D);
    m_stateLabel->setObjectName("ClearHpglState");
    m_stateLabel->setAttribute(Qt::WA_TransparentForMouseEvents);  // orbit / pan / issue hover pass through
    m_positionView3D->installEventFilter(this);
    pos3dOuter->addWidget(m_positionView3D, 1);
    workH->addWidget(pos3dGroup);
    // Resizing / maximizing gives all extra width to the 3D view; the left column keeps its width.
    workH->setStretchFactor(0, 0);
    workH->setStretchFactor(1, 1);
    root->addWidget(workH, 1);
    setCentralWidget(central);

    connect(reset3d, &QPushButton::clicked, m_positionView3D, &PositionView3D::resetView);
    connect(m_pathCheck, &QCheckBox::toggled, m_positionView3D, &PositionView3D::setPathTraceEnabled);
    connect(m_issuesCheck, &QCheckBox::toggled, this, [this](bool on) {
        AppSettings().setValue("ui/showPathIssues", on);
        m_positionView3D->setPathIssues(on ? m_pathIssueMarkers : QVector<PathIssueMarker>());
    });
    connect(m_lockTopDownViewCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_positionView3D->setTopDownLocked(on);
        AppSettings().setValue("ui/viewportTopDownLocked", on);
    });
    connect(m_followPenCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_positionView3D->setFollowTool(on);
        AppSettings().setValue("ui/viewportFollowPen", on);
    });
    connect(m_leftDragOrbitCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_positionView3D->setLeftDragOrbitEnabled(on);
        AppSettings().setValue("ui/viewportLeftDragOrbitEnabled", on);
    });
    m_positionView3D->setPathTraceEnabled(true);
    connect(m_positionView3D, &PositionView3D::pathIssueQuickFixCloseContour, this,
            &MainWindow::OnPathIssueQuickFixCloseContour);
    connect(m_positionView3D, &PositionView3D::pathIssueQuickFixMergeAcrossRapid, this,
            &MainWindow::OnPathIssueQuickFixMergeAcrossRapid);

    statusBar()->showMessage("Ready");
    m_showLogButton = new QPushButton("Show log", this);
    m_showLogButton->setVisible(false);
    statusBar()->addPermanentWidget(m_showLogButton);
    connect(m_showLogButton, &QPushButton::clicked, this, [this]() { m_toggleLogAction->setChecked(true); });
    connect(minimizeLog, &QPushButton::clicked, this, [this]() { m_toggleLogAction->setChecked(false); });
    connect(clearLog, &QPushButton::clicked, m_logEdit, &QTextEdit::clear);

    connect(m_refreshPortsButton, &QPushButton::clicked, this, &MainWindow::RefreshPorts);
    connect(m_discoverButton, &QPushButton::clicked, this, &MainWindow::Discover);
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::ConnectOrDisconnect);
    connect(m_transportCombo, &QComboBox::currentTextChanged, this, [this](const QString &t) {
        AppSettings().setValue("connection/transport", t);
        UpdateTransportUi();
    });
    connect(m_enableButton, &QPushButton::clicked, this, [this]() { Command("enable"); });
    connect(m_disableButton, &QPushButton::clicked, this, [this]() { Command("disable"); });
    connect(m_abortButton, &QPushButton::clicked, this, &MainWindow::StopPlot);
    connect(m_pauseButton, &QPushButton::clicked, this, &MainWindow::TogglePause);
    connect(m_originButton, &QPushButton::clicked, this, [this]() { Command("origin"); });
    connect(m_clearFaultButton, &QPushButton::clicked, this, [this]() { Command("clear"); });
    connect(m_gotoButton, &QPushButton::clicked, this, &MainWindow::GotoTarget);
    connect(m_penUpButton, &QPushButton::clicked, this, [this]() { Command("pen up"); });
    connect(m_penDownButton, &QPushButton::clicked, this, [this]() { Command("pen down"); });
    connect(m_openButton, &QPushButton::clicked, this, &MainWindow::OpenDrawing);
    connect(m_plotButton, &QPushButton::clicked, this, &MainWindow::StartPlot);
    connect(m_stopButton, &QPushButton::clicked, this, &MainWindow::StopPlot);
    connect(m_placeAtOriginCheck, &QCheckBox::toggled, this, [this](bool on) {
        AppSettings().setValue("plot/placeAtOrigin", on);
        if (m_source == SourceKind::Dxf) {
            SetPreviewText(JobHpgl());
            RebuildPreview();
        }
    });

    UpdateTransportUi();
    LoadWindowUiSettings();
    m_positionView3D->setDroOverlayText(QStringLiteral("Not connected"));
}

void MainWindow::UpdateTransportUi() {
    const bool ethernet = m_transportCombo->currentText() == "Ethernet";
    m_serialSettingsWidget->setVisible(!ethernet);
    m_ethernetSettingsWidget->setVisible(ethernet);
}

void MainWindow::SetConnectedUi(bool connected) {
    m_connectButton->setText(connected ? "Disconnect" : "Connect");
    m_transportCombo->setEnabled(!connected);
    m_serialSettingsWidget->setEnabled(!connected);
    m_ethernetSettingsWidget->setEnabled(!connected);
    setWindowTitle(connected ? QStringLiteral("ClearHPGL [%1]").arg(m_link.host())
                             : QStringLiteral("ClearHPGL [Disconnected]"));
    if (!connected) {
        m_stateLabel->setText("Not connected");
        SetSheet(m_stateLabel, QStringLiteral("QLabel { color: #e6ebf3; border-color: #3a414c; }"));
        PlaceStateOverlay();
        m_positionView3D->setDroOverlayText(QStringLiteral("Not connected"));
    }
    statusBar()->showMessage(connected ? "Connected" : "Disconnected");
    UpdateMachineButtons();
}

void MainWindow::UpdateMachineButtons() {
    const bool conn = m_link.isConnected();
    const PlotterLink::Status &s = m_link.status();
    const bool jobBusy = m_jobSending || m_jobAwaitingFinish;
    const bool serialStreaming = m_link.transport() == PlotterLink::Transport::Serial && m_link.jobActive();
    const bool manualOk = conn && !jobBusy && !serialStreaming;

    for (auto *b : {m_enableButton, m_disableButton, m_abortButton, m_pauseButton, m_originButton,
                    m_clearFaultButton}) {
        b->setEnabled(conn && !serialStreaming);
    }
    m_abortButton->setEnabled(conn);
    m_gotoButton->setEnabled(manualOk);
    m_penUpButton->setEnabled(manualOk);
    m_penDownButton->setEnabled(manualOk);
    for (auto *b : m_jogButtons) {
        b->setEnabled(manualOk && s.valid);
    }
    m_plotButton->setEnabled(conn && !jobBusy && m_source != SourceKind::None);
    m_stopButton->setEnabled(conn && jobBusy);
    m_exportHpglAction->setEnabled(m_source != SourceKind::None);
    m_saveDxfGeometryAction->setEnabled(m_source == SourceKind::Dxf && !m_dxfPolylinesMm.isEmpty());

    if (!conn || !s.valid) {
        SetSheet(m_enableButton, kIdleButton);
        SetSheet(m_disableButton, kIdleButton);
    } else if (s.enabled) {
        SetSheet(m_enableButton, kGreenButton);
        SetSheet(m_disableButton, kIdleButton);
    } else {
        SetSheet(m_enableButton, kIdleButton);
        SetSheet(m_disableButton, kOrangeButton);
    }
    m_pauseButton->setText(s.state == QLatin1String("paused") ? "Resume" : "Pause");
    const bool faulted = s.valid && !s.fault.isEmpty() && s.fault != QLatin1String("none");
    SetSheet(m_clearFaultButton, faulted ? kOrangeButton : kIdleButton);
}

void MainWindow::AppendLog(const QString &line) {
    const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    m_logEdit->append(QStringLiteral("[%1] %2").arg(ts, line));
}

void MainWindow::Command(const QString &line) {
    if (!m_link.isConnected()) {
        statusBar()->showMessage("Not connected");
        return;
    }
    m_link.sendCommand(line);
}

/* ------------------------------------------------------------------------- */
/* Connection                                                                 */
/* ------------------------------------------------------------------------- */

void MainWindow::RefreshPorts() {
    const QString selected = m_portCombo->currentText().isEmpty()
                                 ? AppSettings().value("connection/port").toString()
                                 : m_portCombo->currentText();
    m_portCombo->clear();
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
        m_portCombo->addItem(info.portName());
    }
    const int idx = m_portCombo->findText(selected);
    if (idx >= 0) {
        m_portCombo->setCurrentIndex(idx);
    }
}

void MainWindow::Discover() {
    m_discoverySeen.clear();
    QStringList direct;
    const QString typed = m_deviceCombo->currentText().section(QLatin1Char(' '), 0, 0).trimmed();
    if (!typed.isEmpty()) {
        direct << typed;
    }
    const QString saved = AppSettings().value("connection/ethernetHost").toString();
    if (!saved.isEmpty() && saved != typed) {
        direct << saved;
    }
    m_link.discover(direct);
    QTimer::singleShot(1500, this, [this]() {
        if (m_discoverySeen.isEmpty()) {
            AppendLog("Discover: no plotter answered on UDP 9302. Check the cable and that the PC has an "
                      "address on the plotter's subnet, or type the plotter's IP and Connect.");
            statusBar()->showMessage("No plotter found", 6000);
        } else {
            statusBar()->showMessage(QStringLiteral("Found %1 plotter(s)").arg(m_discoverySeen.size()), 6000);
        }
    });
}

void MainWindow::ConnectOrDisconnect() {
    if (m_link.isConnected()) {
        if (m_link.jobActive()) {
            const auto r = QMessageBox::question(
                this, "Disconnect",
                "A plot is still being sent. Disconnecting stops sending; the plotter finishes what it has "
                "buffered. Disconnect anyway?");
            if (r != QMessageBox::Yes) {
                return;
            }
        }
        m_link.disconnectLink();
        m_jobSending = false;
        m_jobAwaitingFinish = false;
        AppendLog("Disconnected");
        SetConnectedUi(false);
        return;
    }

    QString err;
    QSettings settings = AppSettings();
    if (m_transportCombo->currentText() == "Ethernet") {
        QString host = m_deviceCombo->currentData().toString();
        if (host.isEmpty() || m_deviceCombo->currentText().trimmed() != m_deviceCombo->itemText(m_deviceCombo->currentIndex())) {
            host = m_deviceCombo->currentText().section(QLatin1Char(' '), 0, 0).trimmed();
        }
        if (host.isEmpty()) {
            QMessageBox::warning(this, "ClearHPGL", "No plotter selected. Click Discover or type its IP address.");
            return;
        }
        if (!m_link.connectEthernet(host, &err)) {
            QMessageBox::critical(this, "ClearHPGL", err);
            return;
        }
        settings.setValue("connection/ethernetHost", host);
        AppendLog(QStringLiteral("Connected to %1 (console %2, HP-GL %3)")
                      .arg(host)
                      .arg(PlotterLink::kConsolePort)
                      .arg(PlotterLink::kHpglPort));
    } else {
        const QString port = m_portCombo->currentText();
        if (port.isEmpty()) {
            QMessageBox::warning(this, "ClearHPGL", "No serial port selected.");
            return;
        }
        static const QSerialPort::FlowControl kFlows[] = {QSerialPort::NoFlowControl, QSerialPort::SoftwareControl,
                                                          QSerialPort::HardwareControl};
        if (!m_link.connectSerial(port, m_baudCombo->currentText().toInt(), kFlows[m_flowCombo->currentIndex()],
                                  &err)) {
            QMessageBox::critical(this, "ClearHPGL", err);
            return;
        }
        settings.setValue("connection/port", port);
        settings.setValue("connection/baud", m_baudCombo->currentText());
        settings.setValue("connection/flow", m_flowCombo->currentIndex());
        AppendLog(QStringLiteral("Opened %1 (HP-GL port; console via \"menu\" on USB)").arg(port));
    }
    SetConnectedUi(true);
    Command("config");
}

void MainWindow::OnLinkLost(const QString &reason) {
    AppendLog(QStringLiteral("Connection lost: %1").arg(reason));
    m_jobSending = false;
    m_jobAwaitingFinish = false;
    StopCycleTimer(QStringLiteral("Plot interrupted (connection lost)"));
    SetConnectedUi(false);
}

/* ------------------------------------------------------------------------- */
/* Status and console replies                                                 */
/* ------------------------------------------------------------------------- */

void MainWindow::OnStatus(const PlotterLink::Status &s) {
    const bool faulted = !s.fault.isEmpty() && s.fault != QLatin1String("none");
    // Pen and position are in the viewport's readout (top right); this line is machine state only.
    QString text = QStringLiteral("%1 · %2")
                       .arg(s.state.isEmpty() ? QStringLiteral("streaming") : s.state.toUpper(),
                            s.enabled ? QStringLiteral("enabled") : QStringLiteral("disabled"));
    if (s.queue > 0) {
        text += QStringLiteral(" · queue %1").arg(s.queue);
    }
    if (s.estop == QLatin1String("open")) {
        text += QStringLiteral(" · ESTOP OPEN");
    }
    if (faulted) {
        text += QStringLiteral(" · FAULT: %1").arg(s.fault);
    }
    m_stateLabel->setText(text);
    // Both states set explicitly: clearing the sheet to "" left the fault colours on the overlay.
    SetSheet(m_stateLabel, faulted || s.estop == QLatin1String("open")
                                    ? QStringLiteral("QLabel { color: #ffcdd2; border-color: #c62828; }")
                                    : QStringLiteral("QLabel { color: #e6ebf3; border-color: #3a414c; }"));
    PlaceStateOverlay();
    UpdatePenView(s);

    if (m_jobSending || m_jobAwaitingFinish) {
        UpdateProgress(s);
    }
    if (m_jobAwaitingFinish) {
        if (s.state == QLatin1String("plotting") || s.state == QLatin1String("paused") || s.queue > 0) {
            m_jobSawBusy = true;
        }
        if (faulted) {
            m_jobAwaitingFinish = false;
            StopCycleTimer(QStringLiteral("Plot stopped (%1)").arg(s.fault));
        } else if (s.state == QLatin1String("idle") && s.queue == 0 &&
                   (m_jobSawBusy || QDateTime::currentMSecsSinceEpoch() - m_jobSentMs > 3000)) {
            /* A small job is "sent" as soon as the OS buffers it, often before the board has
             * started; an idle sample then is not the end. Require having seen this job busy
             * (or a few seconds of idle for jobs too short to catch). */
            m_jobAwaitingFinish = false;
            PushProgressToView(true);
            StopCycleTimer(QStringLiteral("Plot complete"));
        }
    }
    UpdateMachineButtons();
}

void MainWindow::UpdatePenView(const PlotterLink::Status &s) {
    m_positionView3D->setDroOverlayText(QStringLiteral("X  %1 mm\nY  %2 mm\nPen %3")
                                            .arg(s.xMm, 0, 'f', 3)
                                            .arg(s.yMm, 0, 'f', 3)
                                            .arg(s.penDown ? "down" : "up"));
    m_positionView3D->setToolPosition(float(s.xMm), float(s.yMm), s.penDown ? 0.f : kPenUpVizZ);
}

// Fast path (~40/s): pen marker, readout and progress only. Buttons, faults and job
// completion stay on the full status poll.
void MainWindow::OnTelemetry(const PlotterLink::Status &s) {
    UpdatePenView(s);
    if (m_jobSending || m_jobAwaitingFinish) {
        UpdateProgress(s);
    }
}

void MainWindow::OnCommandFinished(const QString &command, bool ok, const QStringList &lines) {
    if (command == QLatin1String("status") || command.startsWith(QLatin1String("tel "))) {
        return;  /* background poll / optional telemetry (older firmware rejects it quietly) */
    }
    if (command == QLatin1String("config")) {
        if (ok) {
            ApplyDeviceConfig(lines);
            if (m_settingsDialogPending) {
                m_settingsDialogPending = false;
                ShowSettingsDialog(lines);
            }
        } else {
            m_settingsDialogPending = false;
        }
        return;
    }
    const QString last = lines.isEmpty() ? QString() : lines.last();
    statusBar()->showMessage(QStringLiteral("%1: %2").arg(command, ok ? QStringLiteral("ok") : last), 6000);
    if (!ok && !command.startsWith(QLatin1String("set "))) {
        QMessageBox::warning(this, "Plotter", QStringLiteral("%1\n\n%2").arg(command, last));
    }
}

void MainWindow::ApplyDeviceConfig(const QStringList &lines) {
    static const QRegularExpression kLine(QStringLiteral("^(\\S+)\\s+=\\s+(\\S*)"));
    for (const QString &l : lines) {
        const QRegularExpressionMatch m = kLine.match(l);
        if (m.hasMatch()) {
            m_deviceConfig.insert(m.captured(1), m.captured(2));
        }
    }
    const double w = m_deviceConfig.value("width_mm").toDouble();
    const double h = m_deviceConfig.value("height_mm").toDouble();
    if (w > 0 && h > 0 && (w != m_plotWidthMm || h != m_plotHeightMm)) {
        m_plotWidthMm = w;
        m_plotHeightMm = h;
        AppSettings().setValue("plotter/widthMm", w);
        AppSettings().setValue("plotter/heightMm", h);
        AppendLog(QStringLiteral("Plot area from plotter: %1 x %2 mm").arg(w).arg(h));
    }
    m_positionView3D->setBedSize(float(m_plotWidthMm), float(m_plotHeightMm));
    RebuildPreview();
}

/* ------------------------------------------------------------------------- */
/* Machine actions                                                            */
/* ------------------------------------------------------------------------- */

void MainWindow::TogglePause() {
    Command(m_link.status().state == QLatin1String("paused") ? "resume" : "pause");
}

void MainWindow::GotoTarget() {
    Command(QStringLiteral("goto %1 %2")
                .arg(ParseDoubleLine(m_gotoXEdit, 0.0), 0, 'f', 3)
                .arg(ParseDoubleLine(m_gotoYEdit, 0.0), 0, 'f', 3));
}

void MainWindow::Jog(double dxMm, double dyMm) {
    const PlotterLink::Status &s = m_link.status();
    const double x = qBound(0.0, s.xMm + dxMm, m_plotWidthMm);
    const double y = qBound(0.0, s.yMm + dyMm, m_plotHeightMm);
    Command(QStringLiteral("goto %1 %2").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3));
}

void MainWindow::ShowPlotterSettings() {
    if (!m_link.isConnected()) {
        QMessageBox::information(this, "Plotter settings", "Connect to the plotter first.");
        return;
    }
    m_settingsDialogPending = true;
    Command("config");
}

void MainWindow::ShowSettingsDialog(const QStringList &configLines) {
    PlotterSettingsDialog dlg(configLines, this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    const QStringList cmds = dlg.changedCommands();
    for (const QString &cmd : cmds) {
        Command(cmd);
    }
    if (!cmds.isEmpty()) {
        Command("config");
    }
}

/* ------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* ------------------------------------------------------------------------- */

void MainWindow::OpenDrawing() {
    const QString path = QFileDialog::getOpenFileName(
        this, "Open drawing", AppSettings().value("program/lastDirectory").toString(),
        "Drawings (*.dxf *.plt *.hpgl *.hgl *.hpg *.prn);;DXF (*.dxf);;HP-GL (*.plt *.hpgl *.hgl *.hpg *.prn);;"
        "All Files (*.*)");
    if (path.isEmpty()) {
        return;
    }
    AppSettings().setValue("program/lastDirectory", QFileInfo(path).absolutePath());
    if (path.endsWith(QLatin1String(".dxf"), Qt::CaseInsensitive)) {
        LoadDxf(path);
    } else {
        LoadHpgl(path);
    }
    if (m_workTabs && m_plotTab) {
        m_workTabs->setCurrentWidget(m_plotTab);
    }
}

void MainWindow::LoadDxf(const QString &path) {
    DxfImportOutput imported;
    if (!ImportDxfFile(path, &imported)) {
        QMessageBox::warning(this, "Open DXF",
                             imported.errorMessage.isEmpty() ? QStringLiteral("Import failed.") : imported.errorMessage);
        return;
    }
    m_source = SourceKind::Dxf;
    m_sourcePath = path;
    m_hpglFile.clear();
    m_dxfInsunits = imported.insunits;
    m_dxfDrawingToMmScale = imported.drawingToMmScale;
    m_dxfPolylinesMm = std::move(imported.polylinesMm);
    m_dxfRapidSegmentsMm = std::move(imported.rapidSegmentsMm);
    SetPreviewText(JobHpgl());
    AppendLog(QStringLiteral("Loaded DXF: %1 (%2 polylines)").arg(path).arg(m_dxfPolylinesMm.size()));
    RebuildPreview();
    if (!m_followPenCheck->isChecked()) {
        m_positionView3D->resetView();  /* fixed view: centre the new drawing */
    }
}

void MainWindow::LoadHpgl(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Open HP-GL", QStringLiteral("Could not open %1").arg(path));
        return;
    }
    m_source = SourceKind::Hpgl;
    m_sourcePath = path;
    m_hpglFile = f.readAll();
    m_dxfPolylinesMm.clear();
    m_dxfRapidSegmentsMm.clear();
    SetPreviewText(m_hpglFile);
    AppendLog(QStringLiteral("Loaded HP-GL: %1 (%2 bytes)").arg(path).arg(m_hpglFile.size()));
    RebuildPreview();
    if (!m_followPenCheck->isChecked()) {
        m_positionView3D->resetView();
    }
}

QVector3D MainWindow::DxfPlacementOffset() const {
    if (!m_placeAtOriginCheck->isChecked() || m_dxfPolylinesMm.isEmpty()) {
        return QVector3D();
    }
    float minX = FLT_MAX;
    float minY = FLT_MAX;
    for (const auto &strip : m_dxfPolylinesMm) {
        for (const QVector3D &p : strip) {
            minX = qMin(minX, p.x());
            minY = qMin(minY, p.y());
        }
    }
    return (minX == FLT_MAX) ? QVector3D() : QVector3D(-minX, -minY, 0.f);
}

QVector<QVector<QVector3D>> MainWindow::PlacedDxfStrips() const {
    const QVector3D off = DxfPlacementOffset();
    QVector<QVector<QVector3D>> out = m_dxfPolylinesMm;
    for (auto &strip : out) {
        for (QVector3D &p : strip) {
            p += off;
        }
    }
    return out;
}

QByteArray MainWindow::JobHpgl() const {
    if (m_source == SourceKind::Hpgl) {
        return m_hpglFile;
    }
    if (m_source == SourceKind::Dxf) {
        /* End with the pen raised and put away, as plotter drivers do. */
        return BuildHpglFromPolylinesMm(PlacedDxfStrips()).toLatin1() + "\nPU;SP0;\n";
    }
    return QByteArray();
}

void MainWindow::RebuildPreview() {
    m_previewStripsMm.clear();
    m_previewRapidsMm.clear();
    m_previewVertexBytes.clear();
    QVector<PathIssueMarker> markers;
    QString extra;

    const double spmm = m_deviceConfig.value("mm_rev_x").toDouble() > 0
                            ? m_deviceConfig.value("steps_rev_x").toDouble() / m_deviceConfig.value("mm_rev_x").toDouble()
                            : 160.0;
    const double cornerMmS = m_deviceConfig.contains("corner_mm_s") ? m_deviceConfig.value("corner_mm_s").toDouble() : 20.0;
    const double drawMmS = m_deviceConfig.contains("draw_mm_s") ? m_deviceConfig.value("draw_mm_s").toDouble() : 100.0;

    if (m_source == SourceKind::Dxf) {
        const QVector3D off = DxfPlacementOffset();
        m_previewStripsMm = PlacedDxfStrips();
        m_previewRapidsMm = m_dxfRapidSegmentsMm;
        for (QVector3D &p : m_previewRapidsMm) {
            p += off;
        }
        BuildPathIssueMarkers(m_previewStripsMm, true, false, cornerMmS * spmm, spmm, spmm, drawMmS * 60.0,
                              &m_previewRapidsMm, &markers);
        /* JobHpgl() starts with exactly this text: one PD instruction per strip. */
        QVector<int> pdOffsets;
        BuildHpglFromPolylinesMm(m_previewStripsMm, &pdOffsets);
        for (int s = 0; s < m_previewStripsMm.size(); s++) {
            m_previewVertexBytes.append(QVector<int>(m_previewStripsMm[s].size(), pdOffsets.value(s, -1)));
        }
    } else if (m_source == SourceKind::Hpgl) {
        const HpglPreviewResult r = PreviewHpgl(m_hpglFile, m_plotWidthMm, m_plotHeightMm);
        m_previewStripsMm = r.cutStripsMm;
        m_previewRapidsMm = r.rapidSegmentsMm;
        m_previewVertexBytes = r.cutStripBytes;
        BuildPathIssueMarkers(m_previewStripsMm, false, false, cornerMmS * spmm, spmm, spmm, drawMmS * 60.0,
                              &m_previewRapidsMm, &markers);
        if (r.labelsSkipped > 0) {
            extra += QStringLiteral(" · %1 text label(s) will not be drawn (LB is not implemented)").arg(r.labelsSkipped);
        }
        if (r.unknownInstructions > 0) {
            extra += QStringLiteral(" · %1 unknown instruction(s) skipped").arg(r.unknownInstructions);
        }
    }
    m_positionView3D->setProgramPathSplit(m_previewStripsMm, m_previewRapidsMm);
    m_pathIssueMarkers = markers;
    m_positionView3D->setPathIssues(m_issuesCheck->isChecked() ? markers : QVector<PathIssueMarker>());
    BuildProgressPlan();
    m_pathCheck->setEnabled(m_source != SourceKind::None);
    UpdatePlotInfo();
    if (!extra.isEmpty()) {
        m_plotInfoLabel->setText(m_plotInfoLabel->text() + extra);
    }
    UpdateMachineButtons();
}

void MainWindow::UpdatePlotInfo() {
    if (m_source == SourceKind::None) {
        m_plotInfoLabel->setText("No drawing loaded");
        return;
    }
    float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
    double drawLen = 0.0;
    for (const auto &strip : m_previewStripsMm) {
        for (int i = 0; i < strip.size(); i++) {
            const QVector3D &p = strip.at(i);
            minX = qMin(minX, p.x());
            minY = qMin(minY, p.y());
            maxX = qMax(maxX, p.x());
            maxY = qMax(maxY, p.y());
            if (i > 0) {
                drawLen += double((p - strip.at(i - 1)).length());
            }
        }
    }
    if (minX == FLT_MAX) {
        m_plotInfoLabel->setText(QStringLiteral("%1 — nothing is drawn with the pen down")
                                     .arg(QFileInfo(m_sourcePath).fileName()));
        m_plotInfoLabel->setStyleSheet("QLabel { color: #ffcc80; }");
        return;
    }
    QString text = QStringLiteral("%1 — %2 strokes, %3 m drawn, extent X %4…%5  Y %6…%7 mm (plot area %8 × %9 mm)")
                       .arg(QFileInfo(m_sourcePath).fileName())
                       .arg(m_previewStripsMm.size())
                       .arg(drawLen / 1000.0, 0, 'f', 2)
                       .arg(minX, 0, 'f', 1)
                       .arg(maxX, 0, 'f', 1)
                       .arg(minY, 0, 'f', 1)
                       .arg(maxY, 0, 'f', 1)
                       .arg(m_plotWidthMm)
                       .arg(m_plotHeightMm);
    const bool outside = minX < -0.01f || minY < -0.01f || maxX > m_plotWidthMm + 0.01 || maxY > m_plotHeightMm + 0.01;
    if (outside) {
        text += QStringLiteral(" · extends beyond the plot area and will be clipped");
    }
    m_plotInfoLabel->setText(text);
    SetSheet(m_plotInfoLabel, outside ? "QLabel { color: #ffcc80; }" : "");
}

/* ------------------------------------------------------------------------- */
/* Plot progress                                                              */
/* ------------------------------------------------------------------------- */

void MainWindow::BuildProgressPlan() {
    m_plan.clear();
    double cum = 0.0;
    bool havePrev = false;
    QVector3D prevEnd;
    for (int s = 0; s < m_previewStripsMm.size(); s++) {
        const QVector<QVector3D> &strip = m_previewStripsMm[s];
        if (strip.size() < 2) {
            continue;
        }
        if (havePrev) {
            PlanSeg hop;
            hop.a = prevEnd;
            hop.b = strip.first();
            hop.strip = s;
            hop.cumStart = cum;
            m_plan.append(hop);
            cum += double((hop.b - hop.a).length());
        }
        for (int i = 0; i + 1 < strip.size(); i++) {
            PlanSeg seg;
            seg.a = strip[i];
            seg.b = strip[i + 1];
            seg.strip = s;
            seg.vertex = i;
            seg.cut = true;
            seg.cumStart = cum;
            m_plan.append(seg);
            cum += double((seg.b - seg.a).length());
        }
        prevEnd = strip.last();
        havePrev = true;
    }
    ResetProgress();
}

void MainWindow::SetPreviewText(const QByteArray &job) {
    m_activeLine = -1;
    m_preview->setExtraSelections({});
    m_preview->setPlainText(FormatHpglForDisplay(job, kMaxPreviewChars, &m_previewLineBytes));
}

void MainWindow::ShowActiveLine(int line) {
    if (line == m_activeLine) {
        return;
    }
    m_activeLine = line;
    const QTextBlock block = line >= 0 ? m_preview->document()->findBlockByNumber(line) : QTextBlock();
    if (!block.isValid()) {  // cleared, or past the truncated end of the preview
        m_preview->setExtraSelections({});
        return;
    }
    QTextEdit::ExtraSelection sel;
    sel.cursor = QTextCursor(block);
    sel.format.setBackground(QColor(0x2e, 0x6b, 0x45));
    sel.format.setForeground(QColor(0xff, 0xff, 0xff));
    sel.format.setProperty(QTextFormat::FullWidthSelection, true);
    m_preview->setExtraSelections({sel});
    // Centre the line vertically; keep the horizontal scroll at the line start.
    QScrollBar *v = m_preview->verticalScrollBar();
    const QRect r = m_preview->cursorRect(sel.cursor);
    v->setValue(v->value() + r.center().y() - m_preview->viewport()->height() / 2);
    m_preview->horizontalScrollBar()->setValue(0);
}

void MainWindow::ResetProgress() {
    m_progressShapeShown = -1;
    m_progressSeg = -1;
    m_progressT = 0.f;
    m_progressMs = QDateTime::currentMSecsSinceEpoch();
    m_positionView3D->setProgramProgress({});
    ShowActiveLine(-1);
}

// Match the reported pen position to the plan, searching forward only (plotting is sequential)
// and only as far as the plotter could have travelled since the last match, so crossings and
// repeated passes over the same spot do not make progress jump ahead.
void MainWindow::UpdateProgress(const PlotterLink::Status &s) {
    if (m_plan.isEmpty() || !s.valid) {
        return;
    }
    if (!s.penDown) {
        /* Pen-up samples never match ahead (they cannot tell which shape is next, and matching
         * a later pen-up hop jumped past undrawn shapes). But a lift while plotting means the
         * current pen-down run is finished: complete it now, otherwise the tail of a short move
         * after the last pen-down sample stayed yellow until the next shape started. Pause also
         * lifts the pen, mid-shape, so only "plotting" counts. */
        if (s.state == QLatin1String("plotting") && m_progressSeg >= 0 && m_plan[m_progressSeg].cut) {
            const int strip = m_plan[m_progressSeg].strip;
            int last = m_progressSeg;
            while (last + 1 < m_plan.size() && m_plan[last + 1].cut && m_plan[last + 1].strip == strip) {
                last++;
            }
            if (last != m_progressSeg || m_progressT < 1.f) {
                m_progressSeg = last;
                m_progressT = 1.f;
                m_progressMs = QDateTime::currentMSecsSinceEpoch();
                PushProgressToView();
            }
        }
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double dt = qMax<qint64>(now - m_progressMs, 100) / 1000.0;
    const double vMax = qMax(m_deviceConfig.value("max_mm_s", "250").toDouble(),
                             m_deviceConfig.value("travel_mm_s", "200").toDouble());
    const int from = qMax(0, m_progressSeg);
    const double reach = (m_progressSeg < 0 ? 0.0 : m_plan[from].cumStart) + vMax * dt * 2.0 + 20.0;
    const QVector3D p(float(s.xMm), float(s.yMm), 0.f);
    constexpr float kTolMm = 1.0f;

    int best = -1;
    float bestT = 0.f;
    constexpr float kTieMm = 0.05f;  /* overlapping shapes: prefer the earliest */
    float bestDist = kTolMm + kTieMm;
    for (int i = from; i < m_plan.size() && m_plan[i].cumStart <= reach; i++) {
        const PlanSeg &seg = m_plan[i];
        if (!seg.cut) {
            continue;
        }
        const QVector3D ab = seg.b - seg.a;
        const float len2 = ab.lengthSquared();
        float t = len2 > 1e-12f ? QVector3D::dotProduct(p - seg.a, ab) / len2 : 0.f;
        t = qBound(0.f, t, 1.f);
        if (i == m_progressSeg) {
            t = qMax(t, m_progressT);  // never move backwards within a segment
        }
        const float d = (seg.a + ab * t - p).length();
        if (d + kTieMm < bestDist) {
            bestDist = d;
            best = i;
            bestT = t;
        }
    }
    if (best < 0 || (best == m_progressSeg && bestT <= m_progressT)) {
        /* No match: keep the time of the last match so the search window keeps growing.
         * Resetting it here capped the window at one poll interval, and after a few missed
         * samples the pen was out of reach for good (progress stopped after a few shapes). */
        return;
    }
    m_progressMs = now;
    m_progressSeg = best;
    m_progressT = bestT;
    PushProgressToView();
    const int shape = m_plan[best].strip;
    if (shape != m_progressShapeShown) {
        m_progressShapeShown = shape;
        statusBar()->showMessage(QStringLiteral("Drawing shape %1 of %2").arg(shape + 1).arg(m_previewStripsMm.size()));
    }
}

void MainWindow::PushProgressToView(bool allDone) {
    QVector<QVector<QVector3D>> done;
    if (allDone) {
        done = m_previewStripsMm;
    } else if (m_progressSeg >= 0 && m_progressSeg < m_plan.size()) {
        const PlanSeg &cur = m_plan[m_progressSeg];
        for (int s = 0; s < cur.strip && s < m_previewStripsMm.size(); s++) {
            done.append(m_previewStripsMm[s]);
        }
        if (cur.cut) {
            const QVector<QVector3D> &strip = m_previewStripsMm[cur.strip];
            QVector<QVector3D> part = strip.mid(0, cur.vertex + 1);
            part.append(cur.a + (cur.b - cur.a) * m_progressT);
            done.append(part);
        }
    }
    m_positionView3D->setProgramProgress(done);

    // HP-GL box: highlight and scroll to the instruction drawing the current segment.
    int line = -1;
    if (!allDone && m_progressSeg >= 0 && m_progressSeg < m_plan.size() && m_plan[m_progressSeg].cut) {
        const PlanSeg &cur = m_plan[m_progressSeg];
        const int byte = m_previewVertexBytes.value(cur.strip).value(cur.vertex + 1, -1);
        if (byte >= 0 && !m_previewLineBytes.isEmpty()) {
            /* Last displayed line starting at or before the byte (the move is emitted once its
             * instruction's terminator or next separator has been read, so step back one). */
            const int at = qMax(0, byte - 1);
            line = int(std::upper_bound(m_previewLineBytes.cbegin(), m_previewLineBytes.cend(), at) -
                       m_previewLineBytes.cbegin()) - 1;
        }
    }
    ShowActiveLine(line);
}

void MainWindow::ExportHpgl() {
    if (m_source == SourceKind::None) {
        return;
    }
    const QFileInfo fi(m_sourcePath);
    const QString out = QFileDialog::getSaveFileName(this, "Export HP-GL",
                                                     fi.absolutePath() + "/" + fi.completeBaseName() + ".plt",
                                                     "HP-GL (*.plt *.hpgl);;All Files (*.*)");
    if (out.isEmpty()) {
        return;
    }
    QFile f(out);
    if (!f.open(QIODevice::WriteOnly) || f.write(JobHpgl()) < 0) {
        QMessageBox::warning(this, "Export HP-GL", QStringLiteral("Could not write %1").arg(out));
        return;
    }
    AppendLog(QStringLiteral("Wrote HP-GL: %1").arg(out));
}

/* ------------------------------------------------------------------------- */
/* Plot job                                                                   */
/* ------------------------------------------------------------------------- */

void MainWindow::StartPlot() {
    const QByteArray job = JobHpgl();
    if (job.isEmpty()) {
        QMessageBox::information(this, "Plot", "Open a drawing first.");
        return;
    }
    const PlotterLink::Status &s = m_link.status();
    if (s.valid && s.estop == QLatin1String("open")) {
        QMessageBox::warning(this, "Plot",
                             "The estop input (DI-6) is open. Close it, or set estop_di6 to 0 in Plotter settings "
                             "if no estop is wired.");
        return;
    }
    if (m_source == SourceKind::Dxf) {
        int partly = 0;
        int entirely = 0;
        for (const auto &strip : m_previewStripsMm) {
            int outside = 0;
            for (const QVector3D &p : strip) {
                if (p.x() < -0.01f || p.y() < -0.01f || p.x() > m_plotWidthMm + 0.01 || p.y() > m_plotHeightMm + 0.01) {
                    outside++;
                }
            }
            if (outside == strip.size()) {
                entirely++;
            } else if (outside > 0) {
                partly++;
            }
        }
        if (entirely + partly > 0) {
            const auto r = QMessageBox::question(
                this, "Plot",
                QStringLiteral("The drawing does not fit the %1 x %2 mm plot area: %3 shape(s) lie entirely outside "
                               "and will not be drawn, %4 will be cut off at the edge.\n\nPlot anyway?")
                    .arg(m_plotWidthMm)
                    .arg(m_plotHeightMm)
                    .arg(entirely)
                    .arg(partly));
            if (r != QMessageBox::Yes) {
                return;
            }
        }
    }
    QString err;
    if (!m_link.startJob(job, &err)) {
        QMessageBox::warning(this, "Plot", err);
        return;
    }
    ResetProgress();
    StartCycleTimer();
    m_jobSending = true;
    m_jobAwaitingFinish = false;
    m_jobSawBusy = false;
    m_progress->setValue(0);
    statusBar()->showMessage("Sending plot...");
    UpdateMachineButtons();
}

void MainWindow::StopPlot() {
    if (m_link.jobActive()) {
        m_link.cancelJob();
        AppendLog("Stopped sending the job");
    }
    m_jobSending = false;
    m_jobAwaitingFinish = false;
    StopCycleTimer(QStringLiteral("Plot aborted"));
    if (m_link.isConnected()) {
        Command("abort");
    }
    UpdateMachineButtons();
}

void MainWindow::OnJobProgress(qint64 sent, qint64 total) {
    m_progress->setValue(total > 0 ? int((sent * 100) / total) : 0);
}

void MainWindow::OnJobSent() {
    m_jobSentMs = QDateTime::currentMSecsSinceEpoch();
    m_jobSending = false;
    m_jobAwaitingFinish = true;
    m_progress->setValue(100);
    AppendLog("Job sent; the plotter is finishing from its buffer");
    statusBar()->showMessage("Job sent; plotting...");
    UpdateMachineButtons();
}

static QString FormatCycle(qint64 ms) {
    const qint64 tenths = ms / 100;
    const qint64 h = tenths / 36000;
    const qint64 m = (tenths / 600) % 60;
    const qint64 s = (tenths / 10) % 60;
    const qint64 t = tenths % 10;
    return h > 0 ? QStringLiteral("%1:%2:%3.%4").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0')).arg(t)
                 : QStringLiteral("%1:%2.%3").arg(m).arg(s, 2, 10, QLatin1Char('0')).arg(t);
}

void MainWindow::StartCycleTimer() {
    m_cycleClock.start();
    m_cycleRunning = true;
    m_cycleLabel->setStyleSheet("QLabel { font-family: Consolas, 'Courier New', monospace; font-size: 14px; "
                                "font-weight: 700; color: #7fe3a0; }");
    ShowCycleTime();
    m_cycleTick.start();
}

void MainWindow::StopCycleTimer(const QString &outcome) {
    if (!m_cycleRunning) {
        return;
    }
    m_cycleRunning = false;
    m_cycleTick.stop();
    ShowCycleTime();
    m_cycleLabel->setStyleSheet("QLabel { font-family: Consolas, 'Courier New', monospace; font-size: 14px; "
                                "font-weight: 700; color: #e6ebf3; }");
    const QString line = QStringLiteral("%1 in %2").arg(outcome, FormatCycle(m_cycleClock.elapsed()));
    AppendLog(line);
    statusBar()->showMessage(line);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_positionView3D && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        PlaceStateOverlay();
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::PlaceStateOverlay() {
    if (!m_stateLabel || !m_positionView3D) {
        return;
    }
    m_stateLabel->adjustSize();
    const int margin = 10;
    m_stateLabel->move(margin, qMax(0, m_positionView3D->height() - m_stateLabel->height() - margin));
    m_stateLabel->raise();
}

void MainWindow::ShowCycleTime() {
    m_cycleLabel->setText(QStringLiteral("Cycle  %1").arg(FormatCycle(m_cycleClock.elapsed())));
}

void MainWindow::OnJobFailed(const QString &message) {
    m_jobSending = false;
    m_jobAwaitingFinish = false;
    StopCycleTimer(QStringLiteral("Job failed (%1)").arg(message));
    UpdateMachineButtons();
}

/* ------------------------------------------------------------------------- */
/* DXF quick fixes                                                            */
/* ------------------------------------------------------------------------- */

void MainWindow::OnPathIssueQuickFixCloseContour(int stripIndex) {
    if (m_source != SourceKind::Dxf || stripIndex < 0 || stripIndex >= m_dxfPolylinesMm.size()) {
        return;
    }
    QVector<QVector3D> &strip = m_dxfPolylinesMm[stripIndex];
    if (strip.size() < 3) {
        return;
    }
    constexpr float tolMm = 0.09f;
    if ((strip.first() - strip.last()).length() <= tolMm) {
        AppendLog(QStringLiteral("Quick fix: contour on strip %1 already closed (skipped).").arg(stripIndex));
        return;
    }
    const QVector3D endPrev = strip.at(strip.size() - 2);
    const QVector3D endP = strip.last();
    const QVector3D startP = strip.first();
    const QVector3D startNext = strip.at(1);
    const QVector<QVector3D> mid = gapBezierBridgeInterior(endPrev, endP, startP, startNext, 26);
    if (!mid.isEmpty()) {
        for (const QVector3D &p : mid) {
            strip.append(p);
        }
    } else {
        const int n = 12;
        for (int k = 1; k < n; ++k) {
            const float t = float(k) / float(n);
            strip.append(endP * (1.f - t) + startP * t);
        }
    }
    strip.append(startP);
    OrderStripsForCutting(&m_dxfPolylinesMm, &m_dxfRapidSegmentsMm);
    SetPreviewText(JobHpgl());
    AppendLog(QStringLiteral("DXF quick fix: closed strip %1 with a tangent bridge to its start.").arg(stripIndex));
    RebuildPreview();
    PromptSaveDxfAfterGeometryFix();
}

void MainWindow::OnPathIssueQuickFixMergeAcrossRapid(int rapidPairIndex) {
    if (m_source != SourceKind::Dxf || rapidPairIndex < 0 || rapidPairIndex + 1 >= m_dxfPolylinesMm.size()) {
        return;
    }
    const int flatIdx = rapidPairIndex * 2;
    if (flatIdx + 1 >= m_dxfRapidSegmentsMm.size()) {
        return;
    }
    QVector<QVector3D> a = m_dxfPolylinesMm[rapidPairIndex];
    const QVector<QVector3D> &b = m_dxfPolylinesMm[rapidPairIndex + 1];
    constexpr float tol = 0.06f;
    const bool meet = (a.last() - b.first()).lengthSquared() <= tol * tol;
    if (!meet) {
        const QVector3D endPrev = a.size() >= 2 ? a.at(a.size() - 2) : a.last();
        const QVector3D endP = a.last();
        const QVector3D startP = b.first();
        const QVector3D startNext = b.size() >= 2 ? b.at(1) : (startP + QVector3D(0.02f, 0.f, 0.f));
        const QVector<QVector3D> mid = gapBezierBridgeInterior(endPrev, endP, startP, startNext, 26);
        if (!mid.isEmpty()) {
            for (const QVector3D &p : mid) {
                a.append(p);
            }
        } else {
            const int n = 12;
            for (int k = 1; k < n; ++k) {
                const float t = float(k) / float(n);
                a.append(endP * (1.f - t) + startP * t);
            }
        }
    }
    for (int i = meet ? 1 : 0; i < b.size(); ++i) {
        a.append(b.at(i));
    }
    m_dxfPolylinesMm[rapidPairIndex] = a;
    m_dxfPolylinesMm.removeAt(rapidPairIndex + 1);
    m_dxfRapidSegmentsMm.remove(flatIdx, 2);
    OrderStripsForCutting(&m_dxfPolylinesMm, &m_dxfRapidSegmentsMm);
    SetPreviewText(JobHpgl());
    AppendLog(QStringLiteral("DXF quick fix: merged strips %1 and %2 across a pen-up move.")
                  .arg(rapidPairIndex)
                  .arg(rapidPairIndex + 1));
    RebuildPreview();
    PromptSaveDxfAfterGeometryFix();
}

QString MainWindow::SuggestedDxfSavePath() const {
    if (!m_sourcePath.isEmpty()) {
        const QFileInfo fi(m_sourcePath);
        return fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral("_fixed.dxf");
    }
    const QString dir = AppSettings().value("program/lastDirectory").toString();
    return dir.isEmpty() ? QStringLiteral("geometry_fixed.dxf") : QDir(dir).filePath(QStringLiteral("geometry_fixed.dxf"));
}

void MainWindow::PromptSaveDxfAfterGeometryFix() {
    const auto r = QMessageBox::question(
        this, QStringLiteral("Save DXF"),
        QStringLiteral("Save the corrected geometry to a new DXF file? Your original file is not overwritten "
                       "until you confirm a path."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (r == QMessageBox::Yes) {
        SaveDxfGeometryAs();
    }
}

void MainWindow::SaveDxfGeometryAs() {
    if (m_source != SourceKind::Dxf || m_dxfPolylinesMm.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Save DXF"), QStringLiteral("Load a DXF first."));
        return;
    }
    const QString outPath = QFileDialog::getSaveFileName(this, QStringLiteral("Save DXF geometry"),
                                                         SuggestedDxfSavePath(),
                                                         QStringLiteral("DXF Files (*.dxf);;All Files (*.*)"));
    if (outPath.isEmpty()) {
        return;
    }
    QString err;
    if (!ExportAsciiDxfMinimal(outPath, m_dxfPolylinesMm, m_dxfInsunits, m_dxfDrawingToMmScale, &err)) {
        QMessageBox::warning(this, QStringLiteral("Save DXF"), err.isEmpty() ? QStringLiteral("Export failed.") : err);
        return;
    }
    m_sourcePath = outPath;
    AppendLog(QStringLiteral("Wrote DXF geometry: %1").arg(outPath));
}

/* ------------------------------------------------------------------------- */
/* Window state                                                               */
/* ------------------------------------------------------------------------- */

void MainWindow::SetLogPanelVisible(bool visible) {
    {
        QSignalBlocker blocker(m_toggleLogAction);
        m_toggleLogAction->setChecked(visible);
    }
    if (!visible) {
        if (m_logGroup->isVisible()) {
            const QList<int> sizes = m_mainVSplitter->sizes();
            if (sizes.size() >= 3 && sizes[2] > 48) {
                m_savedLogPaneHeight = sizes[2];
            }
        }
        m_logGroup->setMinimumHeight(0);
        m_logGroup->hide();
    } else {
        m_logGroup->setMinimumHeight(150);
        m_logGroup->show();
        QTimer::singleShot(0, this, [this]() {
            if (!m_logGroup->isVisible()) {
                return;
            }
            const int total = qMax(400, m_mainVSplitter->height());
            // Remembered height, but never more than ~30% so the drawing view stays the larger.
            const int logH = qBound(120, m_savedLogPaneHeight, qMax(120, (total * 30) / 100));
            const QList<int> cur = m_mainVSplitter->sizes();
            const int ctrlH = (cur.size() >= 3 && cur[0] > 0) ? qBound(64, cur[0], 200) : 110;
            m_mainVSplitter->setSizes({ctrlH, qMax(200, total - ctrlH - logH), logH});
        });
    }
    m_showLogButton->setVisible(!visible);
}

void MainWindow::LoadWindowUiSettings() {
    m_savedLogPaneHeight = qBound(150, AppSettings().value("ui/logPaneHeight", 260).toInt(), 2000);
    SetLogPanelVisible(AppSettings().value("ui/logPanelVisible", true).toBool());
    {
        const bool td = AppSettings().value("ui/viewportTopDownLocked", true).toBool();
        const QSignalBlocker blocker(m_lockTopDownViewCheck);
        m_lockTopDownViewCheck->setChecked(td);
        m_positionView3D->setTopDownLocked(td);
    }
    {
        const bool follow = AppSettings().value("ui/viewportFollowPen", false).toBool();
        const QSignalBlocker blocker(m_followPenCheck);
        m_followPenCheck->setChecked(follow);
        m_positionView3D->setFollowTool(follow);
        m_positionView3D->resetView();
    }
    {
        const bool orbit = AppSettings().value("ui/viewportLeftDragOrbitEnabled", false).toBool();
        const QSignalBlocker blocker(m_leftDragOrbitCheck);
        m_leftDragOrbitCheck->setChecked(orbit);
        m_positionView3D->setLeftDragOrbitEnabled(orbit);
    }
}

void MainWindow::SaveWindowUiSettings() {
    if (m_logGroup->isVisible()) {
        const QList<int> s = m_mainVSplitter->sizes();
        if (s.size() >= 3 && s[2] > 48) {
            m_savedLogPaneHeight = s[2];
        }
    }
    AppSettings().setValue("ui/logPanelVisible", m_logGroup->isVisible());
    if (m_workAreaSplitter && m_didWorkAreaInitialSplit) {
        AppSettings().setValue("ui/leftColumnWidth", m_workAreaSplitter->sizes().value(0, 430));
    }
    AppSettings().setValue("ui/logPaneHeight", m_savedLogPaneHeight);
}
