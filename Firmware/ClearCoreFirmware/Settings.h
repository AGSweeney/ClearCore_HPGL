/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#ifndef __CLEARHPGL_SETTINGS_H__
#define __CLEARHPGL_SETTINGS_H__

#include <stddef.h>
#include <stdint.h>

enum PenMode {
    PEN_MODE_OUTPUT = 0,  /* solenoid / relay / laser gate on IO-n */
    PEN_MODE_Z_AXIS = 1,  /* ClearPath on M2 between two step positions */
};

#pragma pack(push, 1)
struct PlotterSettings {
    uint32_t magic;
    uint16_t version;
    uint16_t size;

    /* Mechanics (M0 = X, M1 = Y). */
    uint32_t stepsPerRev[2];
    float mmPerRev[2];
    int8_t dir[2];
    float areaMm[2];

    /* Motion. */
    float drawMmS;       /* default pen-down speed (VS; restores this) */
    float travelMmS;     /* pen-up speed */
    float maxMmS;        /* VS is clamped to this */
    float accelMmS2;
    float cornerMmS;     /* junction dVmax */

    /* Pen. */
    uint8_t penMode;
    uint8_t penIo;       /* 0..5 = IO-0..IO-5 */
    uint8_t penActiveHigh;
    uint16_t penDownMs;
    uint16_t penUpMs;
    int32_t zUpSteps;
    int32_t zDownSteps;
    uint32_t zVelSteps;
    uint32_t zAccelSteps;

    /* Safety and power-up. */
    uint8_t estopDi6;    /* 0 off, 1 fault when DI-6 low, 2 fault when high */
    uint8_t autoEnable;
    uint8_t hlfbCheck;

    /* Serial plotter port. */
    uint8_t comPort;     /* 0 off, 1 COM-0, 2 COM-1 */
    uint32_t comBaud;
    uint8_t comHandshake;  /* HpglHandshake */
    uint8_t comRs232;    /* 1 RS-232 levels, 0 TTL */

    /* Network. */
    uint8_t netMode;     /* 0 DHCP, 1 static */
    uint8_t ip[4];
    uint8_t netmask[4];
    uint8_t gateway[4];

    char ident[12];      /* OI reply */

    /* Added in version 2. */
    uint8_t optimize;    /* reorder jobs before plotting: bit 0 USB, bit 1 COM, bit 2 network */

    /* Added in version 3. */
    uint8_t jobSpeed;    /* 1 honour VS from jobs, 0 always draw at drawMmS */

    /* Added in version 4. */
    uint8_t kinematics;  /* 0 Cartesian (M0 = X, M1 = Y), 1 CoreXY / H-bot (M0 = A, M1 = B) */
    uint8_t swapXy;      /* 1 exchange the X and Y axes (with dir: any belt or motor layout) */
    float motorMaxMmS;   /* highest motor speed as belt travel (mm/s); 0 = no limit */

    /* Added in version 5. */
    float pulleyDiaMm;   /* CoreXY: pulley pitch diameter, both motors (travel per rev = pi x this) */

    /* Added in version 6: limit and home switches, homing (Switches.h). */
    int8_t swPin[6];     /* input code per switch (X+, X-, X home, Y+, Y-, Y home), -1 none */
    uint8_t swOpen;      /* bit per switch: normally open (tripped when high) */
    uint8_t homeMode;    /* 0 off, 1 board homing on the home switches, 2 ClearPath hard-stop homing */
    uint8_t homeRequired;  /* plots wait for homing (run automatically) after enable */
    uint8_t homeOrder;   /* 0 X then Y, 1 Y then X */
    int8_t homeDir[2];   /* toward the home switch, per axis */
    float homeFast[2];   /* search speed, mm/s */
    float homeSlow[2];   /* latch speed, mm/s */
    float homeBack[2];   /* back-off, mm */
    float homeAt[2];     /* the axis's position at the switch (mm) */
    float homeTravel[2]; /* longest search, mm; 0 = from the plot area */
    uint32_t checksum;
};
#pragma pack(pop)

extern PlotterSettings g_settings;

void SettingsDefaults();
/* Loads NVM; falls back to defaults when the blob is missing or corrupt. */
void SettingsLoad();
bool SettingsSave();
bool SettingsFromNvm();
/* "ClearCoreROS" / "ClearAI" when the network settings came from that firmware's
 * NVM blob (no ClearHPGL settings saved yet); nullptr otherwise. */
const char *SettingsImportedFrom();

/* Console access. Returns nullptr on success or an error message. Keys marked [disabled] need
 * motorsEnabled false; keys marked [idle] need jobActive false. */
const char *SettingsSet(const char *key, const char *value, bool motorsEnabled, bool jobActive,
                        bool *needsRestart);
uint16_t SettingsCount();
/* "key = value    help" for entry i. Returns false past the end. */
bool SettingsFormat(uint16_t i, char *buf, size_t len);

double SettingsStepsPerMm(uint8_t axis);

#endif
