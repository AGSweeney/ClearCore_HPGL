/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#include "Transport.h"

#include "ClearCore.h"
#include "Console.h"
#include "EthernetTcpClient.h"
#include "EthernetTcpServer.h"
#include "EthernetUdp.h"
#include "HpglChannel.h"
#include "Plotter.h"
#include "PlotterConfig.h"
#include "Settings.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define UsbPort ConnectorUsb

enum ChannelId {
    CH_USB = 0,
    CH_COM = 1,
    CH_TCP = 2,
    CH_COUNT = 3,
};

static const char *const kChannelName[CH_COUNT] = {"usb", "com", "tcp"};

static uint8_t g_ringUsb[CHPGL_RING_USB];
static uint8_t g_ringCom[CHPGL_RING_COM];
static uint8_t g_ringTcp[CHPGL_RING_TCP];
static HpglChannel g_ch[CH_COUNT];
static uint32_t g_lastRxMs[CH_COUNT];
/* After a fault the rest of that job is dropped until the source goes quiet. */
static bool g_discard[CH_COUNT];
static int g_active = -1;
static bool g_resyncPending = false;
static HpglInterpreter g_hpgl;

static SerialDriver *g_com = nullptr;

/* Ethernet. */
static EthernetTcpServer g_hpglServer(CHPGL_TCP_HPGL_PORT);
static EthernetTcpServer g_consoleServer(CHPGL_TCP_CONSOLE_PORT);
static EthernetTcpClient g_tcpClient;
static EthernetTcpClient g_consoleClient;
static EthernetUdp g_discoveryUdp;
static bool g_ethernetReady = false;
static bool g_tcpConnected = false;
static bool g_consoleConnected = false;
static uint32_t g_tcpJobs = 0;
static uint32_t g_tcpRead = 0;
static char g_consoleLine[CHPGL_MAX_LINE];
/* "tel <ms>" on the TCP console: push a TEL line every period (0 = off). */
static uint32_t g_telPeriodMs = 0;
static uint32_t g_telLastMs = 0;
static uint16_t g_consoleLen = 0;

/* USB console and the "menu" escape. */
static bool g_usbConsole = false;
static char g_usbLine[CHPGL_MAX_LINE];
static uint16_t g_usbLen = 0;
static bool g_usbPrevCr = false;
static char g_held[4];
static uint8_t g_heldLen = 0;
static uint32_t g_heldMs = 0;
static bool g_usbLineStart = true;

/* ------------------------------------------------------------------------- */
/* Channel I/O                                                                */
/* ------------------------------------------------------------------------- */

class UsbIo : public HpglChannelIo {
public:
    void ChannelSend(const uint8_t *d, uint16_t n) override {
        for (uint16_t i = 0; i < n; i++) {
            UsbPort.SendChar(d[i]);
        }
    }
};

class ComIo : public HpglChannelIo {
public:
    void ChannelSend(const uint8_t *d, uint16_t n) override {
        if (!g_com) {
            return;
        }
        for (uint16_t i = 0; i < n; i++) {
            g_com->SendChar(d[i]);
        }
    }
};

class TcpIo : public HpglChannelIo {
public:
    void ChannelSend(const uint8_t *d, uint16_t n) override {
        if (g_tcpConnected) {
            g_tcpClient.Send(d, n);
        }
    }
};

static UsbIo g_usbIo;
static ComIo g_comIo;
static TcpIo g_tcpIo;

static void OnReply(const char *text) {
    g_ch[g_active >= 0 ? g_active : CH_USB].SendReply(text);
}

static void Ingest(int ch, uint8_t b) {
    g_lastRxMs[ch] = Milliseconds();
    if (g_discard[ch]) {
        return;
    }
    g_ch[ch].Ingest(b);
}

/* ------------------------------------------------------------------------- */
/* USB                                                                        */
/* ------------------------------------------------------------------------- */

void TransportUsbSendLine(const char *line) {
    UsbPort.SendLine(line);
}

static void UsbConsoleOut(const char *line) {
    UsbPort.SendLine(line);
}

static void UsbPrompt() {
    UsbPort.Send("> ");
}

static void EnterUsbConsole() {
    g_usbConsole = true;
    g_usbLen = 0;
    ConsoleBanner(UsbConsoleOut);
    UsbPrompt();
}

static void FlushHeld() {
    for (uint8_t i = 0; i < g_heldLen; i++) {
        Ingest(CH_USB, (uint8_t)g_held[i]);
    }
    g_heldLen = 0;
}

static void UsbDataChar(uint8_t c) {
    static const char kMenu[] = "menu";
    if (g_heldLen == 4) {
        if (c == '\r' || c == '\n') {
            g_heldLen = 0;
            g_usbLineStart = true;
            EnterUsbConsole();
            return;
        }
        FlushHeld();
    } else if ((g_usbLineStart || g_heldLen > 0) && tolower(c) == kMenu[g_heldLen]) {
        if (g_heldLen == 0) {
            g_heldMs = Milliseconds();
        }
        g_held[g_heldLen++] = (char)c;
        g_usbLineStart = false;
        return;
    } else if (g_heldLen > 0) {
        FlushHeld();
    }
    Ingest(CH_USB, c);
    g_usbLineStart = (c == '\r' || c == '\n');
}

static void UsbConsoleChar(uint8_t c) {
    const bool afterCr = g_usbPrevCr;
    g_usbPrevCr = (c == '\r');
    if (c == '\n' && afterCr) {
        return;
    }
    if (c == '\r' || c == '\n') {
        UsbPort.SendLine();
        g_usbLine[g_usbLen] = '\0';
        g_usbLen = 0;
        if (ConsoleHandleLine(g_usbLine, UsbConsoleOut)) {
            g_usbConsole = false;
            UsbPort.SendLine("Console closed. USB is an HP-GL port again.");
            return;
        }
        UsbPrompt();
        return;
    }
    if (c == 0x08 || c == 0x7f) {
        if (g_usbLen > 0) {
            g_usbLen--;
            UsbPort.Send("\b \b");
        }
        return;
    }
    if (c < 0x20 || g_usbLen >= CHPGL_MAX_LINE - 1) {
        return;
    }
    g_usbLine[g_usbLen++] = (char)c;
    UsbPort.SendChar(c);
}

static void PollUsb() {
    for (int budget = 512; budget > 0; budget--) {
        if (!g_usbConsole && g_ch[CH_USB].Free() < 8) {
            break;  /* back-pressure: leave it in the CDC buffer */
        }
        const int16_t ch = UsbPort.CharGet();
        if (ch < 0) {
            break;
        }
        if (g_usbConsole) {
            UsbConsoleChar((uint8_t)ch);
        } else {
            UsbDataChar((uint8_t)ch);
        }
    }
    if (g_heldLen > 0 && Milliseconds() - g_heldMs > 1000u) {
        FlushHeld();
    }
}

void TransportInitUsb() {
    UsbPort.Mode(Connector::USB_CDC);
    UsbPort.Speed(CHPGL_USB_BAUD);
    UsbPort.PortOpen();
    const uint32_t start = Milliseconds();
    while (!UsbPort && Milliseconds() - start < CHPGL_WAIT_USB_MS) {
        continue;
    }
    Delay_ms(100);
}

/* ------------------------------------------------------------------------- */
/* COM                                                                        */
/* ------------------------------------------------------------------------- */

static void InitCom() {
    g_com = nullptr;
    if (g_settings.comPort == 1) {
        g_com = &ConnectorCOM0;
    } else if (g_settings.comPort == 2) {
        g_com = &ConnectorCOM1;
    }
    if (!g_com) {
        return;
    }
    g_com->Mode(g_settings.comRs232 ? Connector::RS232 : Connector::TTL);
    g_com->Speed(g_settings.comBaud);
    g_com->FlowControl(g_settings.comHandshake == HPGL_HS_HARDWARE);
    g_com->PortOpen();
}

static void PollCom() {
    if (!g_com) {
        return;
    }
    /* Always drain the UART; the handshake is what keeps the host from overrunning. */
    for (int budget = 512; budget > 0 && g_com->AvailableForRead() > 0; budget--) {
        const int16_t ch = g_com->CharGet();
        if (ch < 0) {
            break;
        }
        Ingest(CH_COM, (uint8_t)ch);
    }
}

/* ------------------------------------------------------------------------- */
/* Ethernet                                                                   */
/* ------------------------------------------------------------------------- */

void TransportInitEthernet() {
    if (g_ethernetReady || !EthernetMgr.PhyLinkActive()) {
        return;
    }
    EthernetMgr.Setup();
    if (g_settings.netMode == 1) {
        const uint8_t *ip = g_settings.ip;
        const uint8_t *nm = g_settings.netmask;
        const uint8_t *gw = g_settings.gateway;
        EthernetMgr.LocalIp(IpAddress(ip[0], ip[1], ip[2], ip[3]));
        EthernetMgr.NetmaskIp(IpAddress(nm[0], nm[1], nm[2], nm[3]));
        EthernetMgr.GatewayIp(IpAddress(gw[0], gw[1], gw[2], gw[3]));
    } else if (!EthernetMgr.DhcpBegin()) {
        EthernetMgr.LocalIp(IpAddress(CHPGL_FALLBACK_IP));
    }
    g_hpglServer.Begin();
    g_consoleServer.Begin();
    g_discoveryUdp.Begin(CHPGL_UDP_DISCOVERY_PORT);
    g_ethernetReady = true;
    UsbPort.Send("ClearHPGL IP=");
    UsbPort.SendLine(EthernetMgr.LocalIp().StringValue());
}

const char *TransportIpString() {
    return g_ethernetReady ? EthernetMgr.LocalIp().StringValue() : "not up (no link)";
}

static void CloseTcpClient() {
    /* Remote FIN leaves TcpData allocated until Close() (ClearCoreROS finding). */
    g_tcpClient.Close();
    g_tcpConnected = false;
}

static void PollTcpHpgl() {
    if (!g_tcpConnected) {
        EthernetTcpClient next = g_hpglServer.Accept();
        if (!next.Connected()) {
            return;
        }
        g_tcpClient = next;
        g_tcpConnected = true;
        g_tcpJobs++;
        g_discard[CH_TCP] = false;
        g_ch[CH_TCP].ResetDeviceControl();
    }
    uint8_t buf[256];
    while (true) {
        const int16_t avail = g_tcpClient.BytesAvailable();
        if (avail <= 0) {
            break;
        }
        uint32_t want = (uint32_t)avail;
        if (want > sizeof(buf)) {
            want = sizeof(buf);
        }
        if (!g_discard[CH_TCP]) {
            const uint16_t room = g_ch[CH_TCP].Free();
            if (room == 0) {
                break;  /* back-pressure: TCP window closes */
            }
            if (want > room) {
                want = room;
            }
        }
        const int16_t n = g_tcpClient.Read(buf, want);
        if (n <= 0) {
            break;
        }
        g_tcpRead += (uint32_t)n;
        for (int16_t i = 0; i < n; i++) {
            Ingest(CH_TCP, buf[i]);
        }
    }
    if (!g_tcpClient.Connected() && g_tcpClient.BytesAvailable() <= 0) {
        CloseTcpClient();
    }
}

static void ConsoleTcpOut(const char *line) {
    if (g_consoleConnected) {
        g_consoleClient.Send(line);
        g_consoleClient.Send("\r\n");
    }
}

static void PollTelemetry() {
    if (!g_consoleConnected || g_telPeriodMs == 0) {
        return;
    }
    const uint32_t now = Milliseconds();
    if (now - g_telLastMs < g_telPeriodMs) {
        return;
    }
    /* Only send when it fits: a PC that stops reading must never stall the motion loop. */
    volatile const EthernetTcp::TcpData *st = g_consoleClient.ConnectionState();
    if (st == nullptr || st->pcb == nullptr || tcp_sndbuf(st->pcb) < 128) {
        return;
    }
    g_telLastMs = now;
    char line[96];
    PlotterTelemetry(line, sizeof(line));
    ConsoleTcpOut(line);
}

const char *TransportSetTelemetry(void (*out)(const char *), uint32_t periodMs) {
    if (out != ConsoleTcpOut) {
        return "telemetry is available on the TCP console (9300) only";
    }
    g_telPeriodMs = (periodMs == 0) ? 0 : (periodMs < 10 ? 10 : periodMs);
    g_telLastMs = 0;
    return nullptr;
}

static void PollTcpConsole() {
    if (!g_consoleConnected || !g_consoleClient.Connected()) {
        if (g_consoleConnected) {
            g_consoleClient.Close();
            g_consoleConnected = false;
            g_telPeriodMs = 0;  /* a new client starts without telemetry */
        }
        EthernetTcpClient next = g_consoleServer.Accept();
        if (!next.Connected()) {
            return;
        }
        g_consoleClient = next;
        g_consoleConnected = true;
        g_consoleLen = 0;
    }
    while (g_consoleClient.BytesAvailable() > 0) {
        const int16_t ch = g_consoleClient.Read();
        if (ch < 0) {
            break;
        }
        if (ch == '\r' || ch == '\n') {
            if (g_consoleLen == 0) {
                continue;
            }
            g_consoleLine[g_consoleLen] = '\0';
            g_consoleLen = 0;
            if (ConsoleHandleLine(g_consoleLine, ConsoleTcpOut)) {
                g_consoleClient.Close();
                g_consoleConnected = false;
                return;
            }
            continue;
        }
        if (g_consoleLen < CHPGL_MAX_LINE - 1) {
            g_consoleLine[g_consoleLen++] = (char)ch;
        }
    }
}

static void PollDiscovery() {
    const uint16_t size = g_discoveryUdp.PacketParse();
    if (size == 0) {
        return;
    }
    unsigned char packet[96];
    const int32_t n = g_discoveryUdp.PacketRead(packet, sizeof(packet) - 1);
    if (n <= 0) {
        return;
    }
    packet[n] = '\0';
    if (strncmp((const char *)packet, CHPGL_DISCOVERY_REQUEST, strlen(CHPGL_DISCOVERY_REQUEST)) != 0) {
        return;
    }
    char reply[160];
    snprintf(reply, sizeof(reply), "CLEARHPGL %s IP=%s HPGL=%u CONSOLE=%u FW=%s IDENT=%s",
             CHPGL_FIRMWARE_NAME, EthernetMgr.LocalIp().StringValue(), (unsigned)CHPGL_TCP_HPGL_PORT,
             (unsigned)CHPGL_TCP_CONSOLE_PORT, CHPGL_FIRMWARE_VERSION, g_settings.ident);
    g_discoveryUdp.Connect(g_discoveryUdp.RemoteIp(), g_discoveryUdp.RemotePort());
    g_discoveryUdp.PacketWrite(reply);
    g_discoveryUdp.PacketSend();
}

/* ------------------------------------------------------------------------- */
/* Job arbitration and interpreter feed                                       */
/* ------------------------------------------------------------------------- */

static bool SourceFinished(int ch) {
    if (g_ch[ch].Count() > 0) {
        return false;
    }
    if (ch == CH_TCP) {
        return !g_tcpConnected;
    }
    return Milliseconds() - g_lastRxMs[ch] > CHPGL_JOB_IDLE_MS;
}

static void FeedInterpreter() {
    if (g_resyncPending) {
        return;
    }
    if (g_active < 0) {
        for (int ch = 0; ch < CH_COUNT; ch++) {
            if (g_ch[ch].Count() > 0) {
                g_active = ch;
                break;
            }
        }
    }
    if (g_active < 0) {
        if (g_hpgl.Busy()) {
            g_hpgl.Pump();
        }
        return;
    }
    HpglChannel &c = g_ch[g_active];
    for (int i = 0; i < CHPGL_FEED_BURST; i++) {
        if (PlotterRoom() <= 2 * HpglInterpreter::kRoomReserve) {
            break;
        }
        if (g_hpgl.Busy()) {
            g_hpgl.Pump();
            if (g_hpgl.Busy()) {
                break;
            }
            continue;
        }
        uint8_t b;
        if (!c.Pop(&b)) {
            break;
        }
        g_hpgl.Feed(b);
    }
    if (!g_hpgl.Busy() && SourceFinished(g_active)) {
        if (!g_hpgl.AtInstructionBoundary()) {
            g_hpgl.Feed(';');  /* end of job ends the last instruction */
            if (g_hpgl.Busy()) {
                return;
            }
            if (!g_hpgl.AtInstructionBoundary()) {
                g_hpgl.AbortInstruction();  /* unterminated label or string */
            }
        }
        g_active = -1;
    }
}

static void HandleAborts() {
    for (int ch = 0; ch < CH_COUNT; ch++) {
        if (g_ch[ch].TakeAbortRequest() && ch == g_active) {
            /* ESC.K: drop buffered graphics; the host keeps sending. */
            g_hpgl.AbortInstruction();
            PlotterAbort();
            g_resyncPending = true;
        }
        if (g_discard[ch] && ch != CH_TCP && Milliseconds() - g_lastRxMs[ch] > CHPGL_JOB_IDLE_MS) {
            g_discard[ch] = false;
        }
    }
    if (PlotterTakeJobAbort() && g_active >= 0) {
        const int ch = g_active;
        g_ch[ch].ClearData();
        g_discard[ch] = true;
        if (ch == CH_TCP) {
            CloseTcpClient();
        }
        g_hpgl.AbortInstruction();
        g_active = -1;
        g_resyncPending = true;
    }
    if (g_resyncPending && PlotterIdle()) {
        g_hpgl.SyncPositionFromSink();
        g_resyncPending = false;
    }
}

/* ------------------------------------------------------------------------- */
/* Public                                                                     */
/* ------------------------------------------------------------------------- */

void TransportInit() {
    g_ch[CH_USB].Init(&g_usbIo, g_ringUsb, sizeof(g_ringUsb), HPGL_HS_NONE);
    g_ch[CH_COM].Init(&g_comIo, g_ringCom, sizeof(g_ringCom), g_settings.comHandshake);
    g_ch[CH_TCP].Init(&g_tcpIo, g_ringTcp, sizeof(g_ringTcp), HPGL_HS_NONE);
    g_hpgl.Init(PlotterSink(), g_settings.areaMm[0] * HPGL_PU_PER_MM,
                g_settings.areaMm[1] * HPGL_PU_PER_MM, g_settings.ident);
    PlotterSetReplyHandler(OnReply);
    InitCom();
}

void TransportApplySettings() {
    PlotterApplySettings();
    g_hpgl.SetHardClip(g_settings.areaMm[0] * HPGL_PU_PER_MM, g_settings.areaMm[1] * HPGL_PU_PER_MM);
    g_hpgl.SetIdentification(g_settings.ident);
    g_ch[CH_COM].SetHandshake(g_settings.comHandshake);
    /* Mechanics only change while disabled (no job); live speed changes keep the stream going. */
    if (!TransportJobActive()) {
        g_resyncPending = true;
    }
}

HpglInterpreter &TransportInterpreter() {
    return g_hpgl;
}

void TransportPoll() {
    PollUsb();
    PollCom();
    if (!g_ethernetReady) {
        TransportInitEthernet();
    } else {
        EthernetMgr.Refresh();
        PollTcpHpgl();
        PollTcpConsole();
        PollTelemetry();
        PollDiscovery();
    }
    HandleAborts();
    FeedInterpreter();
    for (int ch = 0; ch < CH_COUNT; ch++) {
        g_ch[ch].Poll();
    }
}

bool TransportJobActive() {
    return g_active >= 0 || g_hpgl.Busy();
}

void TransportAbortJob() {
    if (g_active >= 0) {
        g_ch[g_active].ClearData();
        g_discard[g_active] = true;
        if (g_active == CH_TCP) {
            CloseTcpClient();
        }
        g_active = -1;
    }
    g_hpgl.AbortInstruction();
    PlotterAbort();
    g_resyncPending = true;
}

static const char *ManualAllowed() {
    if (TransportJobActive()) {
        return "a job is running";
    }
    if (!PlotterIdle()) {
        return "plotter busy";
    }
    return nullptr;
}

const char *TransportGoto(double xMm, double yMm) {
    if (const char *err = ManualAllowed()) {
        return err;
    }
    xMm = fmin(fmax(xMm, 0.0), g_settings.areaMm[0]);
    yMm = fmin(fmax(yMm, 0.0), g_settings.areaMm[1]);
    PlotterSink()->PenUp();
    PlotterSink()->MoveTo(xMm * HPGL_PU_PER_MM, yMm * HPGL_PU_PER_MM);
    g_resyncPending = true;
    return nullptr;
}

const char *TransportPen(bool down) {
    if (const char *err = ManualAllowed()) {
        return err;
    }
    if (down) {
        PlotterSink()->PenDown();
    } else {
        PlotterSink()->PenUp();
    }
    g_resyncPending = true;
    return nullptr;
}

const char *TransportOrigin() {
    if (const char *err = ManualAllowed()) {
        return err;
    }
    if (const char *err = PlotterSetOrigin()) {
        return err;
    }
    g_hpgl.SyncPositionFromSink();
    return nullptr;
}

void TransportUsbDiag(void (*out)(const char *line)) {
    char buf[320];
    UsbPort.RxDebugLine(buf, sizeof(buf));
    out(buf);
    snprintf(buf, sizeof(buf), "usb channel: count=%u free=%u held=%u console=%d",
             (unsigned)g_ch[CH_USB].Count(), (unsigned)g_ch[CH_USB].Free(), (unsigned)g_heldLen,
             g_usbConsole ? 1 : 0);
    out(buf);
}

void TransportStats(char *buf, size_t len) {
    snprintf(buf, len,
             "source=%s usb_in=%lu com=%s com_in=%lu com_overflow=%lu tcp_in=%lu tcp_jobs=%lu "
             "tcp_connected=%d tcp_avail=%d tcp_live=%d tcp_read=%lu discard=%d active_hpgl=%d console=%d "
             "instr=%lu unknown=%lu labels_skipped=%lu hpgl_error=%d ip=%s",
             g_active >= 0 ? kChannelName[g_active] : "none", (unsigned long)g_ch[CH_USB].BytesIn(),
             g_com ? (g_com == &ConnectorCOM0 ? "COM-0" : "COM-1") : "off",
             (unsigned long)g_ch[CH_COM].BytesIn(), (unsigned long)g_ch[CH_COM].Overflows(),
             (unsigned long)g_ch[CH_TCP].BytesIn(), (unsigned long)g_tcpJobs, g_tcpConnected ? 1 : 0,
             g_tcpConnected ? (int)g_tcpClient.BytesAvailable() : -1,
             g_tcpConnected ? (g_tcpClient.Connected() ? 1 : 0) : -1, (unsigned long)g_tcpRead,
             g_discard[CH_TCP] ? 1 : 0, g_active, g_consoleConnected ? 1 : 0, (unsigned long)g_hpgl.InstructionCount(),
             (unsigned long)g_hpgl.UnknownCount(), (unsigned long)g_hpgl.LabelsSkipped(), g_hpgl.Error(),
             TransportIpString());
}

void TransportRestart() {
    for (int i = 0; i < 10; i++) {
        EthernetMgr.Refresh();
        Delay_ms(5);
    }
    SysMgr.ResetBoard();
}
