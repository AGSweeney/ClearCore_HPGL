/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#include "Settings.h"

#include "ClearCore.h"
#include "HpglChannel.h"
#include "NvmManager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* User-page blob. Magic differs from ClearAI ('CAIC') and ClearCoreROS ('CROS')
 * so a blob left by either is ignored instead of misread. */
static const uint32_t CHPGL_NVM_MAGIC = 0x47504843u; /* 'CHPG' */
static const uint16_t CHPGL_NVM_VERSION = 3;

static_assert(sizeof(PlotterSettings) <= ClearCore::NvmManager::NVM_LOC_RESERVED_TEKNIC,
              "settings blob must fit below the Teknic reserved area");

PlotterSettings g_settings;
static bool g_fromNvm = false;

static ClearCore::NvmManager &Nvm() {
    return ClearCore::NvmManager::Instance();
}

static uint32_t Checksum(const PlotterSettings *s) {
    /* FNV-1a over everything before the checksum field. */
    const uint8_t *p = (const uint8_t *)s;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(PlotterSettings, checksum); i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

void SettingsDefaults() {
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.magic = CHPGL_NVM_MAGIC;
    g_settings.version = CHPGL_NVM_VERSION;
    g_settings.size = sizeof(PlotterSettings);
    for (int a = 0; a < 2; a++) {
        g_settings.stepsPerRev[a] = 800;
        g_settings.mmPerRev[a] = 5.0f;
        g_settings.dir[a] = 1;
    }
    g_settings.areaMm[0] = 400.0f;
    g_settings.areaMm[1] = 300.0f;
    g_settings.drawMmS = 100.0f;
    g_settings.travelMmS = 200.0f;
    g_settings.maxMmS = 250.0f;
    g_settings.accelMmS2 = 2000.0f;
    g_settings.cornerMmS = 20.0f;
    g_settings.penMode = PEN_MODE_OUTPUT;
    g_settings.penIo = 0;
    g_settings.penActiveHigh = 1;
    g_settings.penDownMs = 150;
    g_settings.penUpMs = 100;
    g_settings.zUpSteps = 0;
    g_settings.zDownSteps = 800;
    g_settings.zVelSteps = 20000;
    g_settings.zAccelSteps = 400000;
    g_settings.estopDi6 = 1;
    g_settings.autoEnable = 1;
    g_settings.optimize = 3;  /* USB and COM: jobs from CAD drivers; the app orders its own */
    g_settings.jobSpeed = 1;
    g_settings.hlfbCheck = 1;
    g_settings.comPort = 0;
    g_settings.comBaud = 9600;
    g_settings.comHandshake = HPGL_HS_XONXOFF;
    g_settings.comRs232 = 1;
    g_settings.netMode = 0;
    strncpy(g_settings.ident, "7475A", sizeof(g_settings.ident) - 1);
    g_settings.checksum = Checksum(&g_settings);
}

/* Network settings left by another firmware on this board. Only the address is taken;
 * motion settings from other firmwares mean different things and are never applied.
 * Offsets are fixed by those firmwares' packed blob layouts:
 *   ClearCoreROS 'CROS', every version: netMode at 62, then ip, netmask, gateway.
 *   ClearAI 'CAIC' version 7 (210 bytes): netMode at 194, then ip, netmask, gateway. */
static const uint32_t ROS_NVM_MAGIC = 0x534F5243u;    /* 'CROS' */
static const uint32_t CLEARAI_NVM_MAGIC = 0x43414943u; /* 'CAIC' */
static const char *g_importedFrom = nullptr;

static bool ImportNetwork(const uint8_t *blob, size_t len) {
    uint32_t magic;
    uint16_t version, size;
    memcpy(&magic, blob, 4);
    memcpy(&version, blob + 4, 2);
    memcpy(&size, blob + 6, 2);
    size_t at = 0;
    const char *from = nullptr;
    if (magic == ROS_NVM_MAGIC && version >= 1 && version <= 4 && size >= 75) {
        at = 62;
        from = "ClearCoreROS";
    } else if (magic == CLEARAI_NVM_MAGIC && version == 7 && size == 210) {
        at = 194;
        from = "ClearAI";
    } else {
        return false;
    }
    if (at + 13 > len) {
        return false;
    }
    const uint8_t mode = blob[at];
    const uint8_t *ip = blob + at + 1;
    if (mode > 1) {
        return false;
    }
    if (mode == 1 && ((ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0) || ip[0] == 255)) {
        return false;
    }
    g_settings.netMode = mode;
    memcpy(g_settings.ip, ip, 4);
    memcpy(g_settings.netmask, blob + at + 5, 4);
    memcpy(g_settings.gateway, blob + at + 9, 4);
    g_settings.checksum = Checksum(&g_settings);
    g_importedFrom = from;
    return true;
}

const char *SettingsImportedFrom() {
    return g_importedFrom;
}

void SettingsLoad() {
    uint8_t raw[ClearCore::NvmManager::NVM_LOC_RESERVED_TEKNIC];
    Nvm().BlockRead(ClearCore::NvmManager::NVM_LOC_USER_START, (int)sizeof(raw), raw);
    PlotterSettings s;
    memcpy(&s, raw, sizeof(s));
    /* Older versions are a prefix of this layout (fields are only appended), each ending with
     * its checksum: keep their settings and default the newer fields. Saved as the current
     * version on the next "set". */
    static const size_t kFieldsEnd[] = {
        0,
        offsetof(PlotterSettings, optimize),  /* version 1 */
        offsetof(PlotterSettings, jobSpeed),  /* version 2 */
    };
    if (s.magic == CHPGL_NVM_MAGIC && s.version >= 1 && s.version < CHPGL_NVM_VERSION &&
        s.size == kFieldsEnd[s.version] + 4) {
        const size_t end = kFieldsEnd[s.version];
        uint32_t oldSum;
        memcpy(&oldSum, raw + end, 4);
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < end; i++) {
            h ^= raw[i];
            h *= 16777619u;
        }
        if (h == oldSum) {
            if (s.version < 2) {
                s.optimize = 3;
            }
            s.jobSpeed = 1;
            s.version = CHPGL_NVM_VERSION;
            s.size = sizeof(PlotterSettings);
            s.checksum = Checksum(&s);
        }
    }
    if (s.magic != CHPGL_NVM_MAGIC || s.version != CHPGL_NVM_VERSION ||
        s.size != sizeof(PlotterSettings) || s.checksum != Checksum(&s)) {
        SettingsDefaults();
        g_fromNvm = false;
        /* Not written back: the other firmware's blob stays intact until the first save. */
        ImportNetwork(raw, sizeof(raw));
        return;
    }
    s.ident[sizeof(s.ident) - 1] = '\0';
    g_settings = s;
    g_fromNvm = true;
}

bool SettingsSave() {
    g_settings.magic = CHPGL_NVM_MAGIC;
    g_settings.version = CHPGL_NVM_VERSION;
    g_settings.size = sizeof(PlotterSettings);
    g_settings.checksum = Checksum(&g_settings);
    PlotterSettings existing;
    Nvm().BlockRead(ClearCore::NvmManager::NVM_LOC_USER_START, (int)sizeof(existing),
                    (uint8_t *)&existing);
    /* BlockWrite reports "unchanged" and "write failed" the same way (ClearCoreROS
     * finding): an unchanged cache only counts once the page write has finished. */
    if (memcmp(&existing, &g_settings, sizeof(existing)) == 0) {
        if (Nvm().Synchonized()) {
            g_fromNvm = true;
            return true;
        }
        PlotterSettings nudge;
        memset(&nudge, 0, sizeof(nudge));
        nudge.magic = 1;
        if (!Nvm().BlockWrite(ClearCore::NvmManager::NVM_LOC_USER_START, (int)sizeof(nudge),
                              (const uint8_t *)&nudge)) {
            return false;
        }
    }
    if (!Nvm().BlockWrite(ClearCore::NvmManager::NVM_LOC_USER_START, (int)sizeof(g_settings),
                          (const uint8_t *)&g_settings)) {
        return false;
    }
    g_fromNvm = Nvm().Synchonized();
    return g_fromNvm;
}

bool SettingsFromNvm() {
    return g_fromNvm;
}

double SettingsStepsPerMm(uint8_t axis) {
    if (axis > 1 || g_settings.mmPerRev[axis] <= 0.0f) {
        return 1.0;
    }
    return (double)g_settings.stepsPerRev[axis] / (double)g_settings.mmPerRev[axis];
}

/* ------------------------------------------------------------------------- */
/* Key table                                                                  */
/* ------------------------------------------------------------------------- */

enum KeyType { K_U8, K_I8, K_U16, K_U32, K_I32, K_F32, K_IP, K_STR };
enum KeyFlags {
    KF_LIVE = 0,
    KF_DISABLED = 1,  /* only while motors are disabled */
    KF_RESTART = 2,   /* saved now, used after restart */
    KF_IDLE = 4,      /* only between jobs (motors may stay enabled) */
};

struct KeyDef {
    const char *name;
    uint8_t type;
    uint16_t offset;
    double min;
    double max;
    uint8_t flags;
    const char *help;
};

#define OFS(f) ((uint16_t)offsetof(PlotterSettings, f))

static const KeyDef kKeys[] = {
    {"steps_rev_x", K_U32, OFS(stepsPerRev[0]), 1, 1000000, KF_DISABLED, "X motor steps per revolution"},
    {"steps_rev_y", K_U32, OFS(stepsPerRev[1]), 1, 1000000, KF_DISABLED, "Y motor steps per revolution"},
    {"mm_rev_x", K_F32, OFS(mmPerRev[0]), 0.001, 10000, KF_DISABLED, "X travel per motor revolution (mm)"},
    {"mm_rev_y", K_F32, OFS(mmPerRev[1]), 0.001, 10000, KF_DISABLED, "Y travel per motor revolution (mm)"},
    {"dir_x", K_I8, OFS(dir[0]), -1, 1, KF_DISABLED, "1 or -1: flip X"},
    {"dir_y", K_I8, OFS(dir[1]), -1, 1, KF_DISABLED, "1 or -1: flip Y"},
    /* The plot area is only the hard clip: it can change with the motors enabled, but not mid-job. */
    {"width_mm", K_F32, OFS(areaMm[0]), 1, 100000, KF_IDLE, "plot area X (hard clip)"},
    {"height_mm", K_F32, OFS(areaMm[1]), 1, 100000, KF_IDLE, "plot area Y (hard clip)"},
    {"draw_mm_s", K_F32, OFS(drawMmS), 0.1, 10000, KF_LIVE, "default pen-down speed (VS; restores it)"},
    {"travel_mm_s", K_F32, OFS(travelMmS), 0.1, 10000, KF_LIVE, "pen-up speed"},
    {"max_mm_s", K_F32, OFS(maxMmS), 0.1, 10000, KF_LIVE, "upper limit for VS"},
    {"accel_mm_s2", K_F32, OFS(accelMmS2), 1, 1000000, KF_LIVE, "path acceleration"},
    {"corner_mm_s", K_F32, OFS(cornerMmS), 0, 10000, KF_LIVE, "max per-axis speed change at a corner"},
    {"pen_mode", K_U8, OFS(penMode), 0, 1, KF_DISABLED, "0 = IO output, 1 = Z axis on M2"},
    {"pen_io", K_U8, OFS(penIo), 0, 5, KF_DISABLED, "pen output IO-0..IO-5 (pen_mode 0)"},
    {"pen_active_high", K_U8, OFS(penActiveHigh), 0, 1, KF_DISABLED, "1 = output on means pen down"},
    {"pen_down_ms", K_U16, OFS(penDownMs), 0, 10000, KF_LIVE, "settle time after lowering"},
    {"pen_up_ms", K_U16, OFS(penUpMs), 0, 10000, KF_LIVE, "settle time after raising"},
    {"z_up_steps", K_I32, OFS(zUpSteps), -100000000, 100000000, KF_DISABLED, "M2 pen-up position"},
    {"z_down_steps", K_I32, OFS(zDownSteps), -100000000, 100000000, KF_DISABLED, "M2 pen-down position"},
    {"z_vel_steps", K_U32, OFS(zVelSteps), 1, 10000000, KF_DISABLED, "M2 velocity (steps/s)"},
    {"z_accel_steps", K_U32, OFS(zAccelSteps), 1, 100000000, KF_DISABLED, "M2 acceleration (steps/s^2)"},
    {"estop_di6", K_U8, OFS(estopDi6), 0, 2, KF_LIVE, "0 off, 1 fault when DI-6 low, 2 fault when high"},
    {"auto_enable", K_U8, OFS(autoEnable), 0, 1, KF_LIVE, "enable motors when a job arrives"},
    {"optimize", K_U8, OFS(optimize), 0, 7, KF_LIVE, "reorder jobs before plotting: 1 USB + 2 COM + 4 network"},
    {"job_speed", K_U8, OFS(jobSpeed), 0, 1, KF_LIVE, "1 = jobs set pen speed (VS), 0 = always draw_mm_s"},
    {"hlfb_check", K_U8, OFS(hlfbCheck), 0, 1, KF_LIVE, "require HLFB asserted on enable"},
    {"com_port", K_U8, OFS(comPort), 0, 2, KF_RESTART, "serial plotter port: 0 off, 1 COM-0, 2 COM-1"},
    {"com_baud", K_U32, OFS(comBaud), 300, 1000000, KF_RESTART, "serial baud"},
    {"com_handshake", K_U8, OFS(comHandshake), 0, 2, KF_RESTART, "0 none, 1 XON/XOFF, 2 RTS/CTS"},
    {"com_rs232", K_U8, OFS(comRs232), 0, 1, KF_RESTART, "1 RS-232 levels, 0 TTL"},
    {"net_mode", K_U8, OFS(netMode), 0, 1, KF_RESTART, "0 DHCP, 1 static"},
    {"ip", K_IP, OFS(ip), 0, 0, KF_RESTART, "static address"},
    {"netmask", K_IP, OFS(netmask), 0, 0, KF_RESTART, "static netmask"},
    {"gateway", K_IP, OFS(gateway), 0, 0, KF_RESTART, "static gateway"},
    {"ident", K_STR, OFS(ident), 0, 0, KF_LIVE, "OI reply; 758x/759x (DraftMaster) put 0,0 at bed centre"},
};

static const uint16_t kKeyCount = (uint16_t)(sizeof(kKeys) / sizeof(kKeys[0]));

uint16_t SettingsCount() {
    return kKeyCount;
}

static bool ParseIp(const char *s, uint8_t out[4]) {
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }
    out[0] = (uint8_t)a;
    out[1] = (uint8_t)b;
    out[2] = (uint8_t)c;
    out[3] = (uint8_t)d;
    return true;
}

static void FormatValue(const KeyDef &k, char *buf, size_t len) {
    const uint8_t *base = (const uint8_t *)&g_settings + k.offset;
    switch (k.type) {
        case K_U8: snprintf(buf, len, "%u", (unsigned)*(const uint8_t *)base); break;
        case K_I8: snprintf(buf, len, "%d", (int)*(const int8_t *)base); break;
        case K_U16: {
            uint16_t v;
            memcpy(&v, base, sizeof(v));
            snprintf(buf, len, "%u", (unsigned)v);
            break;
        }
        case K_U32: {
            uint32_t v;
            memcpy(&v, base, sizeof(v));
            snprintf(buf, len, "%lu", (unsigned long)v);
            break;
        }
        case K_I32: {
            int32_t v;
            memcpy(&v, base, sizeof(v));
            snprintf(buf, len, "%ld", (long)v);
            break;
        }
        case K_F32: {
            float v;
            memcpy(&v, base, sizeof(v));
            snprintf(buf, len, "%.3f", (double)v);
            break;
        }
        case K_IP:
            snprintf(buf, len, "%u.%u.%u.%u", base[0], base[1], base[2], base[3]);
            break;
        case K_STR:
            snprintf(buf, len, "%s", (const char *)base);
            break;
        default:
            buf[0] = '\0';
            break;
    }
}

bool SettingsFormat(uint16_t i, char *buf, size_t len) {
    if (i >= kKeyCount) {
        return false;
    }
    char value[32];
    FormatValue(kKeys[i], value, sizeof(value));
    const char *note = (kKeys[i].flags == KF_DISABLED) ? " [disabled]"
                       : (kKeys[i].flags == KF_RESTART) ? " [restart]"
                       : (kKeys[i].flags == KF_IDLE)    ? " [idle]"
                                                        : "";
    snprintf(buf, len, "%-16s = %-16s %s%s", kKeys[i].name, value, kKeys[i].help, note);
    return true;
}

const char *SettingsSet(const char *key, const char *value, bool motorsEnabled, bool jobActive,
                        bool *needsRestart) {
    if (needsRestart) {
        *needsRestart = false;
    }
    const KeyDef *k = nullptr;
    for (uint16_t i = 0; i < kKeyCount; i++) {
        if (strcmp(kKeys[i].name, key) == 0) {
            k = &kKeys[i];
            break;
        }
    }
    if (!k) {
        return "unknown key (try: config)";
    }
    if (!value || !*value) {
        return "missing value";
    }
    if ((k->flags & KF_DISABLED) && motorsEnabled) {
        return "disable motors first";
    }
    if ((k->flags & KF_IDLE) && jobActive) {
        return "a job is running; change it when the plotter is idle";
    }
    uint8_t *base = (uint8_t *)&g_settings + k->offset;
    if (k->type == K_IP) {
        uint8_t ip[4];
        if (!ParseIp(value, ip)) {
            return "expected a.b.c.d";
        }
        memcpy(base, ip, 4);
    } else if (k->type == K_STR) {
        const size_t cap = sizeof(g_settings.ident);
        if (strlen(value) >= cap) {
            return "too long";
        }
        memset(base, 0, cap);
        memcpy(base, value, strlen(value));
    } else {
        char *end = nullptr;
        const double v = strtod(value, &end);
        if (end == value || (end && *end != '\0')) {
            return "not a number";
        }
        if (v < k->min || v > k->max) {
            return "out of range";
        }
        if (k->type == K_I8 && v == 0) {
            return "must be 1 or -1";
        }
        switch (k->type) {
            case K_U8: *base = (uint8_t)v; break;
            case K_I8: *(int8_t *)base = (int8_t)v; break;
            case K_U16: {
                const uint16_t x = (uint16_t)v;
                memcpy(base, &x, sizeof(x));
                break;
            }
            case K_U32: {
                const uint32_t x = (uint32_t)v;
                memcpy(base, &x, sizeof(x));
                break;
            }
            case K_I32: {
                const int32_t x = (int32_t)v;
                memcpy(base, &x, sizeof(x));
                break;
            }
            case K_F32: {
                const float x = (float)v;
                memcpy(base, &x, sizeof(x));
                break;
            }
            default:
                break;
        }
    }
    if (needsRestart) {
        *needsRestart = (k->flags & KF_RESTART) != 0;
    }
    return nullptr;
}
