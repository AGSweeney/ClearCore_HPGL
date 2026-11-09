/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#include "Console.h"

#include "Plotter.h"
#include "PlotterConfig.h"
#include "Settings.h"
#include "Switches.h"
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
    "  home [x|y|all|stop]  home the axes on their switches (home_mode)",
    "  jog <x|y> <mm_s> [stopon <in>,...]  move while renewed (0.5 s); jog stop",
    "  swinfo               inputs, switches, limit stops, homing",
    "  sim <in> low|high|off | sim <in> <x+|x-|y+|y-> <mm> | sim clear   bench switches",
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
    char *tok[6];
    const uint8_t n = Tokenize(line, tok, 6);
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
        const char *err = SettingsSet(tok[1], tok[2], PlotterEnabled(),
                                      TransportJobActive() || !PlotterIdle(), &restart);
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
        if (n >= 2 && Eq(tok[1], "reset")) {
            PlotterDiagReset();
        }
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
    } else if (Eq(cmd, "home")) {
        if (n >= 2 && Eq(tok[1], "stop")) {
            PlotterHomeStop();
            Reply(out, nullptr);
            return false;
        }
        uint8_t axes = 0;
        if (n >= 2) {
            axes = Eq(tok[1], "x") ? 1 : (Eq(tok[1], "y") ? 2 : (Eq(tok[1], "all") ? 0 : 0xFF));
            if (axes == 0xFF) {
                Reply(out, "usage: home [x|y|all|stop]");
                return false;
            }
        }
        Reply(out, TransportJobActive() ? "a job is running" : PlotterHome(axes));
    } else if (Eq(cmd, "jog")) {
        if (n == 1) {
            PlotterSwitchInfo(out);
            out("ok");
            return false;
        }
        if (Eq(tok[1], "stop")) {
            PlotterJogStop();
            Reply(out, nullptr);
            return false;
        }
        const int axis = Eq(tok[1], "x") ? 0 : (Eq(tok[1], "y") ? 1 : -1);
        if (axis < 0 || n < 3) {
            Reply(out, "usage: jog <x|y> <mm_s> [stopon <input>,...] | jog stop");
            return false;
        }
        uint16_t stopOn = 0;
        if (n >= 5 && strcmp(tok[3], "stopon") == 0) {
            char list[CHPGL_MAX_LINE];
            strncpy(list, tok[4], sizeof(list) - 1);
            list[sizeof(list) - 1] = '\0';
            for (char *p = strtok(list, ","); p; p = strtok(nullptr, ",")) {
                const int code = ParseInputName(p);
                if (code < 0) {
                    Reply(out, "stopon: not an input (DI-7, DI-8, A-9..A-12, IO-0..IO-5)");
                    return false;
                }
                stopOn |= (uint16_t)(1u << code);
            }
        }
        Reply(out, TransportJobActive() ? "a job is running" : PlotterJog(axis, atof(tok[2]), stopOn));
    } else if (Eq(cmd, "swinfo")) {
        PlotterSwitchInfo(out);
        out("ok");
    } else if (Eq(cmd, "sim")) {
        if (n == 2 && Eq(tok[1], "clear")) {
            PlotterSimClear();
            Reply(out, nullptr);
            return false;
        }
        const int code = n >= 3 ? ParseInputName(tok[1]) : -2;
        if (code < 0) {
            Reply(out, "usage: sim <input> low|high|off | sim <input> <x+|x-|y+|y-> <mm> | sim clear");
            return false;
        }
        if (Eq(tok[2], "low") || Eq(tok[2], "high") || Eq(tok[2], "off")) {
            PlotterSimLevel(code, Eq(tok[2], "low") ? 1 : (Eq(tok[2], "high") ? 2 : 0));
            Reply(out, nullptr);
            return false;
        }
        const char *s = tok[2];
        if (n < 4 || (s[0] != 'x' && s[0] != 'y') || (s[1] != '+' && s[1] != '-') || s[2]) {
            Reply(out, "usage: sim <input> <x+|x-|y+|y-> <mm>: a switch that far from here on that side");
            return false;
        }
        PlotterSimAt(code, s[0] == 'y' ? 1 : 0, s[1] == '+' ? 1 : -1, atof(tok[3]));
        Reply(out, nullptr);
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
