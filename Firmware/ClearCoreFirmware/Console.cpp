/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#include "Console.h"

#include "Plotter.h"
#include "PlotterConfig.h"
#include "Settings.h"
#include "Transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const kHelp[] = {
    "  status               plotter state, position, link counters",
    "  config               all settings",
    "  set <key> <value>    change and save a setting",
    "  defaults             restore and save factory settings (motors disabled)",
    "  enable | disable     motor enable",
    "  abort                stop now, drop the current job",
    "  pause | resume       hold at the next segment boundary, pen raised",
    "  origin               current position becomes plotter 0,0",
    "  goto <x_mm> <y_mm>   pen-up move (no job running)",
    "  pen up | pen down    pen test (no job running)",
    "  clear                clear a latched fault",
    "  tel <ms>             TCP console: push 'TEL x= y= pen= state= queue=' every <ms> (0 = off)",
    "  restart              reboot (network and COM settings apply)",
    "  version",
    "  exit                 leave the console",
};

static void Reply(ConsoleOut out, const char *err) {
    if (err) {
        char buf[CHPGL_MAX_LINE];
        snprintf(buf, sizeof(buf), "error: %s", err);
        out(buf);
    } else {
        out("ok");
    }
}

void ConsoleBanner(ConsoleOut out) {
    char buf[CHPGL_MAX_LINE];
    snprintf(buf, sizeof(buf), "=== %s %s (HP-GL plotter, ident %s) ===", CHPGL_FIRMWARE_NAME,
             CHPGL_FIRMWARE_VERSION, g_settings.ident);
    out(buf);
    snprintf(buf, sizeof(buf), "IP %s   HP-GL on USB, TCP %u%s   settings: %s", TransportIpString(),
             (unsigned)CHPGL_TCP_HPGL_PORT, g_settings.comPort ? ", COM" : "",
             SettingsFromNvm() ? "NVM"
             : SettingsImportedFrom() ? "defaults, network from previous firmware"
                                      : "defaults");
    out(buf);
    out("Type help for commands.");
}

static uint8_t Tokenize(char *line, char *tok[], uint8_t max) {
    uint8_t n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p) {
            *p++ = '\0';
        }
    }
    return n;
}

static bool Eq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

bool ConsoleHandleLine(const char *lineIn, ConsoleOut out) {
    char line[CHPGL_MAX_LINE];
    strncpy(line, lineIn, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    char *tok[4];
    const uint8_t n = Tokenize(line, tok, 4);
    if (n == 0) {
        return false;
    }
    /* Command words and keys are case-insensitive; values (ident) keep their case. */
    for (uint8_t t = 0; t < n && t < 2; t++) {
        for (char *p = tok[t]; *p; p++) {
            if (*p >= 'A' && *p <= 'Z') {
                *p = (char)(*p - 'A' + 'a');
            }
        }
    }
    const char *cmd = tok[0];
    char buf[400];

    if (Eq(cmd, "help") || Eq(cmd, "?") || Eq(cmd, "menu")) {
        out("Commands:");
        for (size_t i = 0; i < sizeof(kHelp) / sizeof(kHelp[0]); i++) {
            out(kHelp[i]);
        }
        out("ok");
    } else if (Eq(cmd, "status")) {
        PlotterStatus(buf, sizeof(buf));
        out(buf);
        TransportStats(buf, sizeof(buf));
        out(buf);
        out("ok");
    } else if (Eq(cmd, "config")) {
        for (uint16_t i = 0; SettingsFormat(i, buf, sizeof(buf)); i++) {
            out(buf);
        }
        if (SettingsFromNvm()) {
            out("(stored in NVM)");
        } else if (SettingsImportedFrom()) {
            snprintf(buf, sizeof(buf), "(defaults; network imported from %s NVM; saved on first set)",
                     SettingsImportedFrom());
            out(buf);
        } else {
            out("(defaults; not stored yet)");
        }
        out("ok");
    } else if (Eq(cmd, "set")) {
        if (n < 3) {
            Reply(out, "usage: set <key> <value>");
            return false;
        }
        bool restart = false;
        const char *err = SettingsSet(tok[1], tok[2], PlotterEnabled(), &restart);
        if (err) {
            Reply(out, err);
            return false;
        }
        TransportApplySettings();
        if (!SettingsSave()) {
            Reply(out, "applied but NVM write failed (supply above undervoltage lockout?)");
            return false;
        }
        out(restart ? "ok (saved; takes effect after restart)" : "ok");
    } else if (Eq(cmd, "defaults")) {
        if (PlotterEnabled()) {
            Reply(out, "disable motors first");
            return false;
        }
        SettingsDefaults();
        TransportApplySettings();
        Reply(out, SettingsSave() ? nullptr : "NVM write failed");
    } else if (Eq(cmd, "enable")) {
        PlotterClearFault();
        Reply(out, PlotterEnable());
    } else if (Eq(cmd, "disable")) {
        Reply(out, PlotterDisable());
    } else if (Eq(cmd, "abort")) {
        TransportAbortJob();
        Reply(out, nullptr);
    } else if (Eq(cmd, "pause")) {
        PlotterPause(true);
        Reply(out, nullptr);
    } else if (Eq(cmd, "resume")) {
        PlotterPause(false);
        Reply(out, nullptr);
    } else if (Eq(cmd, "origin")) {
        Reply(out, TransportOrigin());
    } else if (Eq(cmd, "goto")) {
        if (n < 3) {
            Reply(out, "usage: goto <x_mm> <y_mm>");
            return false;
        }
        Reply(out, TransportGoto(atof(tok[1]), atof(tok[2])));
    } else if (Eq(cmd, "pen")) {
        if (n < 2 || (!Eq(tok[1], "up") && !Eq(tok[1], "down"))) {
            Reply(out, "usage: pen up|down");
            return false;
        }
        Reply(out, TransportPen(Eq(tok[1], "down")));
    } else if (Eq(cmd, "diag")) {
        PlotterDiag(out);
        TransportOptimizerDiag(out);
        TransportUsbDiag(out);
        out("ok");
    } else if (Eq(cmd, "tel")) {
        if (n < 2) {
            Reply(out, "usage: tel <ms> (0 = off)");
            return false;
        }
        Reply(out, TransportSetTelemetry(out, (uint32_t)atoi(tok[1])));
    } else if (Eq(cmd, "clear")) {
        PlotterClearFault();
        Reply(out, nullptr);
    } else if (Eq(cmd, "restart")) {
        out("ok (restarting)");
        TransportRestart();
    } else if (Eq(cmd, "version")) {
        snprintf(buf, sizeof(buf), "%s %s", CHPGL_FIRMWARE_NAME, CHPGL_FIRMWARE_VERSION);
        out(buf);
        out("ok");
    } else if (Eq(cmd, "exit") || Eq(cmd, "quit") || Eq(cmd, "q")) {
        out("ok");
        return true;
    } else {
        Reply(out, "unknown command (try: help)");
    }
    return false;
}
