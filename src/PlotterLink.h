#pragma once

// Host side of ClearHPGL_Firmware (Firmware/PROTOCOL.md).
//
// Ethernet: console on TCP 9300 (commands + status polling), one TCP 9100
//           connection per plot job, UDP 9302 discovery.
// Serial:   the USB / COM port is an HP-GL port. Jobs are streamed on it. Console
//           commands are wrapped in "menu" ... "exit"; while a job streams, position
//           comes from OA; queries woven into the stream.

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QQueue>
#include <QSerialPort>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

class PlotterLink : public QObject {
    Q_OBJECT
public:
    enum class Transport { None, Ethernet, Serial };

    struct Status {
        bool valid = false;
        QString state;  // idle, plotting, paused, disabled, waiting_enable, fault
        bool enabled = false;
        double xMm = 0.0;
        double yMm = 0.0;
        bool penDown = false;
        int queue = 0;
        QString fault;  // "none" when clear
        QString estop;  // "ok" / "open"
        QString source; // usb, com, tcp, none
    };

    static constexpr quint16 kHpglPort = 9100;
    static constexpr quint16 kConsolePort = 9300;
    static constexpr quint16 kDiscoveryPort = 9302;

    explicit PlotterLink(QObject *parent = nullptr);
    ~PlotterLink() override;

    bool connectEthernet(const QString &host, QString *error);
    bool connectSerial(const QString &port, int baud, QSerialPort::FlowControl flow, QString *error);
    void disconnectLink();
    bool isConnected() const;
    Transport transport() const { return m_transport; }
    QString host() const { return m_host; }

    // Broadcast on every IPv4 subnet, plus a direct query to each of directHosts, sent twice
    // 0.5 s apart: the board's first reply can be lost while it resolves this PC's MAC (ARP).
    void discover(const QStringList &directHosts = QStringList());

    // Console command (status, enable, set k v, ...). Replies come back through
    // commandFinished in the order sent. Polling "status" is added automatically.
    void sendCommand(const QString &line);
    bool hasPendingCommands() const { return !m_cmdQueue.isEmpty() || m_cmdActive; }

    bool startJob(const QByteArray &hpgl, QString *error);
    // Stops sending the rest of the job (the board-side abort is a separate "abort").
    void cancelJob();
    bool jobActive() const { return m_jobActive; }

    const Status &status() const { return m_status; }

signals:
    void deviceDiscovered(const QString &host, const QString &info);
    void statusUpdated(const PlotterLink::Status &status);
    void commandFinished(const QString &command, bool ok, const QStringList &lines);
    void jobProgress(qint64 sent, qint64 total);
    void jobSent();
    void jobFailed(const QString &message);
    void logLine(const QString &line);
    void linkLost(const QString &reason);

private slots:
    void onConsoleReadyRead();
    void onConsoleDisconnected();
    void onJobBytesWritten(qint64 bytes);
    void onJobDisconnected();
    void onJobError(QAbstractSocket::SocketError err);
    void onSerialReadyRead();
    void onSerialBytesWritten(qint64 bytes);
    void onSerialError(QSerialPort::SerialPortError err);
    void onUdpReadyRead();
    void onPollTimer();

private:
    void pumpCommands();
    void handleConsoleLine(const QString &line);
    void finishCommand(bool ok);
    void parseStatusLine(const QString &line);
    void parseSerialReply(const QString &line);
    void sendDiscoveryRound(const QStringList &directHosts, bool log);
    void writeJobChunk();
    void writeSerialJobChunk();
    void failJob(const QString &message);

    Transport m_transport = Transport::None;
    QString m_host;
    QTcpSocket m_console;
    QTcpSocket m_job;
    QSerialPort m_serial;
    QUdpSocket m_udp;
    QTimer m_pollTimer;

    QByteArray m_consoleRx;
    QByteArray m_serialRx;

    QQueue<QString> m_cmdQueue;
    bool m_cmdActive = false;
    QString m_cmdCurrent;
    QStringList m_cmdLines;
    bool m_cmdSeenEcho = false;  // serial console: our command echoed back
    qint64 m_cmdStartMs = 0;

    bool m_jobActive = false;
    QByteArray m_jobData;
    qint64 m_jobOffset = 0;      // bytes handed to the socket / port
    qint64 m_jobConfirmed = 0;   // bytes the OS has written
    qint64 m_lastQueryAt = 0;    // serial: stream offset of the last OA; query

    Status m_status;
};

Q_DECLARE_METATYPE(PlotterLink::Status)
