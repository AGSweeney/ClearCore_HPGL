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
    uint32_t checksum;
};
#pragma pack(pop)

extern PlotterSettings g_settings;

void SettingsDefaults();
/* Loads NVM; falls back to defaults when the blob is missing or corrupt. */
void SettingsLoad();
bool SettingsSave();
bool SettingsFromNvm();

/* Console access. Returns nullptr on success or an error message. */
const char *SettingsSet(const char *key, const char *value, bool motorsEnabled, bool *needsRestart);
uint16_t SettingsCount();
/* "key = value    help" for entry i. Returns false past the end. */
bool SettingsFormat(uint16_t i, char *buf, size_t len);

double SettingsStepsPerMm(uint8_t axis);

#endif
