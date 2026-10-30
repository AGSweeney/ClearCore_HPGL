#include "PlotterLink.h"

#include <QDateTime>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSet>

namespace {
constexpr int kEthernetPollMs = 300;
// Position telemetry period requested from the board ("tel <ms>"); the 300 ms status poll
// alone moved the pen marker and progress only ~3 times a second.
constexpr int kTelemetryMs = 25;
constexpr int kSerialPollMs = 1000;
constexpr int kCommandTimeoutMs = 4000;
constexpr qint64 kJobChunk = 4096;
constexpr qint64 kJobSocketBacklog = 64 * 1024;
constexpr qint64 kSerialChunk = 512;
constexpr qint64 kSerialBacklog = 2048;
// Serial jobs: ask for the pen position roughly this often (at an instruction boundary).
constexpr qint64 kSerialQueryEvery = 4096;
const QByteArray kDiscoveryRequest("CLEARHPGL_DISCOVER?");

qint64 NowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

bool IsStatusCommand(const QString &cmd) {
    return cmd == QLatin1String("status");
}
} // namespace

PlotterLink::PlotterLink(QObject *parent) : QObject(parent) {
    qRegisterMetaType<PlotterLink::Status>("PlotterLink::Status");
    connect(&m_console, &QTcpSocket::readyRead, this, &PlotterLink::onConsoleReadyRead);
    connect(&m_console, &QTcpSocket::disconnected, this, &PlotterLink::onConsoleDisconnected);
    connect(&m_job, &QTcpSocket::bytesWritten, this, &PlotterLink::onJobBytesWritten);
    connect(&m_job, &QTcpSocket::disconnected, this, &PlotterLink::onJobDisconnected);
    connect(&m_job, &QTcpSocket::errorOccurred, this, &PlotterLink::onJobError);
    connect(&m_serial, &QSerialPort::readyRead, this, &PlotterLink::onSerialReadyRead);
    connect(&m_serial, &QSerialPort::bytesWritten, this, &PlotterLink::onSerialBytesWritten);
    connect(&m_serial, &QSerialPort::errorOccurred, this, &PlotterLink::onSerialError);
    connect(&m_udp, &QUdpSocket::readyRead, this, &PlotterLink::onUdpReadyRead);
    connect(&m_pollTimer, &QTimer::timeout, this, &PlotterLink::onPollTimer);
}

PlotterLink::~PlotterLink() {
    disconnectLink();
}

/* ------------------------------------------------------------------------- */
/* Connection                                                                 */
/* ------------------------------------------------------------------------- */

bool PlotterLink::connectEthernet(const QString &host, QString *error) {
    disconnectLink();
    m_console.connectToHost(host, kConsolePort);
    if (!m_console.waitForConnected(3000)) {
        if (error) {
            *error = QStringLiteral("Console %1:%2: %3").arg(host).arg(kConsolePort).arg(m_console.errorString());
        }
        m_console.abort();
        return false;
    }
    m_transport = Transport::Ethernet;
    m_host = host;
    m_status = Status();
    m_pollTimer.start(kEthernetPollMs);
    sendCommand(QStringLiteral("status"));
    sendCommand(QStringLiteral("tel %1").arg(kTelemetryMs));  /* older firmware: harmless error */
    return true;
}

bool PlotterLink::connectSerial(const QString &port, int baud, QSerialPort::FlowControl flow, QString *error) {
    disconnectLink();
    m_serial.setPortName(port);
    m_serial.setBaudRate(baud);
    m_serial.setDataBits(QSerialPort::Data8);
    m_serial.setParity(QSerialPort::NoParity);
    m_serial.setStopBits(QSerialPort::OneStop);
    m_serial.setFlowControl(flow);
    if (!m_serial.open(QIODevice::ReadWrite)) {
        if (error) {
            *error = QStringLiteral("Open %1: %2").arg(port, m_serial.errorString());
        }
        return false;
    }
    m_serial.setDataTerminalReady(true);
    if (flow != QSerialPort::HardwareControl) {
        m_serial.setRequestToSend(true);
    }
    m_transport = Transport::Serial;
    m_host = port;
    m_status = Status();
    m_pollTimer.start(kSerialPollMs);
    sendCommand(QStringLiteral("status"));
    return true;
}

void PlotterLink::disconnectLink() {
    m_pollTimer.stop();
    cancelJob();
    m_cmdQueue.clear();
    m_cmdActive = false;
    m_consoleRx.clear();
    m_serialRx.clear();
    /* Clear the transport first so the disconnected() that abort() emits is ignored. */
    m_transport = Transport::None;
    if (m_console.state() != QAbstractSocket::UnconnectedState) {
        m_console.abort();
    }
    if (m_serial.isOpen()) {
        m_serial.close();
    }
    m_status = Status();
}

bool PlotterLink::isConnected() const {
    if (m_transport == Transport::Ethernet) {
        return m_console.state() == QAbstractSocket::ConnectedState;
    }
    if (m_transport == Transport::Serial) {
        return m_serial.isOpen();
    }
    return false;
}

void PlotterLink::onConsoleDisconnected() {
    if (m_transport != Transport::Ethernet) {
        return;
    }
    disconnectLink();
    emit linkLost(QStringLiteral("console connection closed"));
}

void PlotterLink::onSerialError(QSerialPort::SerialPortError err) {
    if (err == QSerialPort::NoError || m_transport != Transport::Serial) {
        return;
    }
    if (err == QSerialPort::ResourceError || err == QSerialPort::PermissionError) {
        const QString why = m_serial.errorString();
        disconnectLink();
        emit linkLost(why);
    }
}

/* ------------------------------------------------------------------------- */
/* Discovery                                                                  */
/* ------------------------------------------------------------------------- */

void PlotterLink::discover(const QStringList &directHosts) {
    if (m_udp.state() != QAbstractSocket::BoundState) {
        m_udp.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    }
    sendDiscoveryRound(directHosts, true);
    QTimer::singleShot(500, this, [this, directHosts]() { sendDiscoveryRound(directHosts, false); });
}

void PlotterLink::sendDiscoveryRound(const QStringList &directHosts, bool log) {
    QSet<QString> targets;
    for (const QString &h : directHosts) {
        if (!QHostAddress(h).isNull()) {
            targets.insert(h);
        }
    }
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning) ||
            flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.broadcast().isNull()) {
                targets.insert(entry.broadcast().toString());
            }
        }
    }
    if (targets.size() == directHosts.size()) {
        targets.insert(QHostAddress(QHostAddress::Broadcast).toString());  /* no subnet broadcast found */
    }
    for (const QString &t : targets) {
        const qint64 n = m_udp.writeDatagram(kDiscoveryRequest, QHostAddress(t), kDiscoveryPort);
        if (log || n < 0) {
            emit logLine(QStringLiteral("TX UDP: CLEARHPGL_DISCOVER? -> %1:%2%3")
                             .arg(t)
                             .arg(kDiscoveryPort)
                             .arg(n < 0 ? QStringLiteral(" (failed: %1)").arg(m_udp.errorString()) : QString()));
        }
    }
}

void PlotterLink::onUdpReadyRead() {
    while (m_udp.hasPendingDatagrams()) {
        QByteArray d;
        d.resize(int(m_udp.pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort = 0;
        m_udp.readDatagram(d.data(), d.size(), &sender, &senderPort);
        const QString text = QString::fromLatin1(d).trimmed();
        if (!text.startsWith(QLatin1String("CLEARHPGL "))) {
            continue;
        }
        QString ip = sender.toString();
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("IP=(\\S+)")).match(text);
        if (m.hasMatch()) {
            ip = m.captured(1);
        }
        emit deviceDiscovered(ip, text);
    }
}

/* ------------------------------------------------------------------------- */
/* Console commands                                                           */
/* ------------------------------------------------------------------------- */

void PlotterLink::sendCommand(const QString &line) {
    const QString cmd = line.trimmed();
    if (cmd.isEmpty() || !isConnected()) {
        return;
    }
    if (IsStatusCommand(cmd) && (m_cmdQueue.contains(cmd) || (m_cmdActive && m_cmdCurrent == cmd))) {
        return;
    }
    m_cmdQueue.enqueue(cmd);
    pumpCommands();
}

void PlotterLink::pumpCommands() {
    if (m_cmdActive || m_cmdQueue.isEmpty() || !isConnected()) {
        return;
    }
    if (m_transport == Transport::Serial && m_jobActive) {
        return;  /* the port is busy with HP-GL; console waits for the job to finish */
    }
    m_cmdCurrent = m_cmdQueue.dequeue();
    m_cmdLines.clear();
    m_cmdActive = true;
    m_cmdSeenEcho = false;
    m_cmdStartMs = NowMs();
    if (!IsStatusCommand(m_cmdCurrent)) {
        emit logLine(QStringLiteral("TX: %1").arg(m_cmdCurrent));
    }
    if (m_transport == Transport::Ethernet) {
        m_console.write(m_cmdCurrent.toUtf8() + "\r\n");
    } else {
        /* CR puts the board at a line start so "menu" opens the console. */
        m_serial.write("\rmenu\r" + m_cmdCurrent.toUtf8() + "\r");
    }
}

void PlotterLink::finishCommand(bool ok) {
    const QString cmd = m_cmdCurrent;
    const QStringList lines = m_cmdLines;
    m_cmdActive = false;
    m_cmdCurrent.clear();
    m_cmdLines.clear();
    if (m_transport == Transport::Serial) {
        m_serial.write("exit\r");
    }
    /* Background polls stay out of the log, including timeouts while a USB buffer is full. */
    if (!IsStatusCommand(cmd)) {
        for (const QString &l : lines) {
            emit logLine(QStringLiteral("RX: %1").arg(l));
        }
    }
    emit commandFinished(cmd, ok, lines);
    pumpCommands();
}

void PlotterLink::onConsoleReadyRead() {
    m_consoleRx += m_console.readAll();
    int nl;
    while ((nl = m_consoleRx.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_consoleRx.left(nl)).trimmed();
        m_consoleRx.remove(0, nl + 1);
        if (!line.isEmpty()) {
            handleConsoleLine(line);
        }
    }
}

void PlotterLink::handleConsoleLine(const QString &line) {
    if (line.startsWith(QLatin1String("TEL "))) {
        parseTelemetryLine(line);  /* pushed by the board; may arrive inside a command reply */
        return;
    }
    if (line.contains(QLatin1String("state=")) || line.contains(QLatin1String("source="))) {
        parseStatusLine(line);
    }
    if (!m_cmdActive) {
        return;
    }
    m_cmdLines << line;
    if (line.startsWith(QLatin1String("ok"))) {
        finishCommand(true);
    } else if (line.startsWith(QLatin1String("error:"))) {
        finishCommand(false);
    }
}

void PlotterLink::onPollTimer() {
    if (m_cmdActive && NowMs() - m_cmdStartMs > kCommandTimeoutMs) {
        m_cmdLines << QStringLiteral("error: no reply (timeout)%1")
                          .arg(m_transport == Transport::Serial
                                   ? QStringLiteral("; the console is on the USB port only, not COM-0/1")
                                   : QString());
        finishCommand(false);
    }
    if (m_transport == Transport::Serial && m_jobActive) {
        return;
    }
    sendCommand(QStringLiteral("status"));
}

static QString TokenValue(const QString &line, const QString &key) {
    const QString k = key + QLatin1Char('=');
    int at = 0;
    while ((at = line.indexOf(k, at)) >= 0) {
        if (at == 0 || line.at(at - 1).isSpace()) {
            const int start = at + k.size();
            int end = line.indexOf(QLatin1Char(' '), start);
            if (end < 0) {
                end = line.size();
            }
            return line.mid(start, end - start);
        }
        at += k.size();
    }
    return QString();
}

void PlotterLink::parseTelemetryLine(const QString &line) {
    bool okX = false;
    bool okY = false;
    const double x = TokenValue(line, QStringLiteral("x")).toDouble(&okX);
    const double y = TokenValue(line, QStringLiteral("y")).toDouble(&okY);
    if (!okX || !okY) {
        return;
    }
    m_status.xMm = x;
    m_status.yMm = y;
    m_status.penDown = TokenValue(line, QStringLiteral("pen")) == QLatin1String("1");
    m_status.state = TokenValue(line, QStringLiteral("state"));
    m_status.queue = TokenValue(line, QStringLiteral("queue")).toInt();
    m_status.valid = true;
    emit telemetryUpdated(m_status);
}

void PlotterLink::parseStatusLine(const QString &line) {
    if (line.contains(QLatin1String("state="))) {
        m_status.valid = true;
        m_status.state = TokenValue(line, QStringLiteral("state"));
        m_status.enabled = TokenValue(line, QStringLiteral("enabled")) == QLatin1String("1");
        m_status.xMm = TokenValue(line, QStringLiteral("x_mm")).toDouble();
        m_status.yMm = TokenValue(line, QStringLiteral("y_mm")).toDouble();
        m_status.penDown = TokenValue(line, QStringLiteral("pen")) == QLatin1String("down");
        m_status.queue = TokenValue(line, QStringLiteral("queue")).toInt();
        m_status.estop = TokenValue(line, QStringLiteral("estop"));
        const int f = line.indexOf(QLatin1String(" fault="));
        m_status.fault = (f >= 0) ? line.mid(f + 7).trimmed() : QStringLiteral("none");
        emit statusUpdated(m_status);
    } else {
        m_status.source = TokenValue(line, QStringLiteral("source"));
    }
}

/* ------------------------------------------------------------------------- */
/* Serial receive: console text and HP-GL replies                             */
/* ------------------------------------------------------------------------- */

void PlotterLink::onSerialReadyRead() {
    m_serialRx += m_serial.readAll();
    while (true) {
        int end = -1;
        for (int i = 0; i < m_serialRx.size(); i++) {
            if (m_serialRx.at(i) == '\r' || m_serialRx.at(i) == '\n') {
                end = i;
                break;
            }
        }
        if (end < 0) {
            break;
        }
        QByteArray raw = m_serialRx.left(end);
        m_serialRx.remove(0, end + 1);
        raw.replace('\x11', "").replace('\x13', "");  /* XON / XOFF */
        QString line = QString::fromLatin1(raw).trimmed();
        if (line.startsWith(QLatin1String("> "))) {
            line = line.mid(2).trimmed();
        } else if (line == QLatin1String(">")) {
            continue;
        }
        if (!line.isEmpty()) {
            parseSerialReply(line);
        }
    }
}

void PlotterLink::parseSerialReply(const QString &line) {
    static const QRegularExpression kOa(QStringLiteral("^(-?\\d+),(-?\\d+),([01])$"));
    const QRegularExpressionMatch m = kOa.match(line);
    if (m.hasMatch()) {
        m_status.valid = true;
        m_status.xMm = m.captured(1).toDouble() / 40.0;
        m_status.yMm = m.captured(2).toDouble() / 40.0;
        m_status.penDown = m.captured(3) == QLatin1String("1");
        if (m_jobActive) {
            m_status.state = QStringLiteral("plotting");
        }
        emit statusUpdated(m_status);
        return;
    }
    if (!m_cmdActive) {
        return;  /* banner, prompts, "Console closed" */
    }
    if (!m_cmdSeenEcho) {
        if (line == m_cmdCurrent) {
            m_cmdSeenEcho = true;
        }
        return;
    }
    handleConsoleLine(line);
}

/* ------------------------------------------------------------------------- */
/* Jobs                                                                       */
/* ------------------------------------------------------------------------- */

bool PlotterLink::startJob(const QByteArray &hpgl, QString *error) {
    if (!isConnected()) {
        if (error) *error = QStringLiteral("not connected");
        return false;
    }
    if (m_jobActive) {
        if (error) *error = QStringLiteral("a job is already being sent");
        return false;
    }
    if (hpgl.isEmpty()) {
        if (error) *error = QStringLiteral("nothing to plot");
        return false;
    }
    m_jobData = hpgl;
    m_jobOffset = 0;
    m_jobConfirmed = 0;
    m_lastQueryAt = 0;
    if (m_transport == Transport::Ethernet) {
        m_job.connectToHost(m_host, kHpglPort);
        if (!m_job.waitForConnected(3000)) {
            if (error) {
                *error = QStringLiteral("HP-GL port %1:%2: %3").arg(m_host).arg(kHpglPort).arg(m_job.errorString());
            }
            m_job.abort();
            return false;
        }
        m_jobActive = true;
        emit logLine(QStringLiteral("Job: %1 bytes -> %2:%3").arg(hpgl.size()).arg(m_host).arg(kHpglPort));
        writeJobChunk();
    } else {
        if (m_cmdActive) {
            if (error) *error = QStringLiteral("console command in progress; try again");
            return false;
        }
        m_jobActive = true;
        emit logLine(QStringLiteral("Job: %1 bytes -> %2").arg(hpgl.size()).arg(m_host));
        writeSerialJobChunk();
    }
    emit jobProgress(0, m_jobData.size());
    return true;
}

void PlotterLink::writeJobChunk() {
    while (m_jobOffset < m_jobData.size() && m_job.bytesToWrite() < kJobSocketBacklog) {
        const qint64 n = qMin(kJobChunk, m_jobData.size() - m_jobOffset);
        m_job.write(m_jobData.constData() + m_jobOffset, n);
        m_jobOffset += n;
    }
}

void PlotterLink::onJobBytesWritten(qint64 bytes) {
    if (!m_jobActive) {
        return;
    }
    m_jobConfirmed += bytes;
    emit jobProgress(m_jobConfirmed, m_jobData.size());
    writeJobChunk();
    if (m_jobConfirmed >= m_jobData.size()) {
        m_jobActive = false;
        m_job.disconnectFromHost();  /* end of job: the board finishes from its buffer */
        emit jobSent();
    }
}

void PlotterLink::onJobDisconnected() {
    if (m_jobActive) {
        failJob(QStringLiteral("the plotter closed the job connection (fault, estop or abort)"));
    }
}

void PlotterLink::onJobError(QAbstractSocket::SocketError err) {
    if (m_jobActive && err != QAbstractSocket::RemoteHostClosedError) {
        failJob(m_job.errorString());
    }
}

void PlotterLink::writeSerialJobChunk() {
    while (m_jobOffset < m_jobData.size() && m_serial.bytesToWrite() < kSerialBacklog) {
        qint64 n = qMin(kSerialChunk, m_jobData.size() - m_jobOffset);
        bool query = false;
        if (m_jobOffset + n - m_lastQueryAt >= kSerialQueryEvery) {
            /* End the chunk on a ';' so the OA; lands between instructions. */
            const int semi = m_jobData.indexOf(';', int(m_jobOffset));
            if (semi >= 0 && semi < m_jobOffset + n) {
                n = semi + 1 - m_jobOffset;
                query = true;
            }
        }
        m_serial.write(m_jobData.constData() + m_jobOffset, n);
        m_jobOffset += n;
        if (query) {
            m_serial.write("OA;");
            m_lastQueryAt = m_jobOffset;
        }
    }
}

void PlotterLink::onSerialBytesWritten(qint64) {
    if (!m_jobActive) {
        return;
    }
    const qint64 sentEstimate = m_jobOffset - qMin<qint64>(m_jobOffset, m_serial.bytesToWrite());
    emit jobProgress(sentEstimate, m_jobData.size());
    writeSerialJobChunk();
    if (m_jobOffset >= m_jobData.size() && m_serial.bytesToWrite() == 0) {
        m_jobActive = false;
        emit jobSent();
        pumpCommands();
    }
}

void PlotterLink::cancelJob() {
    if (!m_jobActive) {
        return;
    }
    m_jobActive = false;
    if (m_transport == Transport::Ethernet) {
        m_job.abort();
    } else if (m_serial.isOpen()) {
        m_serial.clear(QSerialPort::Output);
    }
    m_jobData.clear();
    pumpCommands();
}

void PlotterLink::failJob(const QString &message) {
    m_jobActive = false;
    m_job.abort();
    m_jobData.clear();
    emit jobFailed(message);
    pumpCommands();
}
