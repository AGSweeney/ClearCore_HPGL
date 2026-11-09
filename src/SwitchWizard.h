/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#pragma once

// Plotter > Set up limits and homing: one axis's switches, from wiring to homing proved by motion.
// Adapted from the Electronic CAM Workbench (ECamUI) wizard for the plotter's two HP-GL axes.
//   Axis       the axis (X or Y), which switches it has, the setup speed
//   Find       trip each switch by hand: the input that changes is the switch's
//   Contact    each switch's level at rest says normally closed (on) or open (off); confirm it
//   Direction  hold Jog + / Jog -: the plotter stops the axis at the first switch it reaches (jog ... stopon),
//              and which switch that is proves its job. The - limit reached going + means the motor runs the
//              other way (dir_x / dir_y), or the switches were tripped the wrong way round: one question tells
//              which. The home switch's side is seen on the way.
//   Limits     (an axis with limits) the limits go to the plotter, and they stop the axis: hold Jog until each
//              limit stops it, then off it again; how far past the switch it ran
//   Home       (an axis with a home switch) the homing settings, from what was seen; home it N times and see
//              the spread of the latched positions
//   Review     what was found and proved; Apply writes it to the plotter
// Nothing is taken on trust from a hand: every limit is proved by driving the axis to it.

#include "PlotterLink.h"

#include <QDialog>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimer;

class SwitchWizard : public QDialog {
    Q_OBJECT
public:
    // config: the plotter's settings (key -> value, from "config"); link: its console.
    SwitchWizard(PlotterLink *link, const QMap<QString, QString> &config, QWidget *parent = nullptr);
    ~SwitchWizard() override;

    enum Switch { kPlus = 0, kMinus, kHome, kSwitches };
    enum Step { kStepAxis, kStepFind, kStepContact, kStepDirection, kStepLimits, kStepHome, kStepReview, kSteps };
    struct Found {
        bool wanted = false;
        int input = -1;          // board input code (0..5 IO, 7..8 DI, 9..12 A), -1 none
        bool restHigh = true;    // its level at rest
        bool normallyOpen = false;
        bool proved = false;     // reached by motion where its job says
    };

    // What a stop on a switch says: the switch reached going dir.
    enum class Verdict { Right, Ask, Home, Unknown };
    static Verdict Judge(int dir, int reached);  // reached: kPlus / kMinus / kHome, -1 none of them
    static QString InputName(int code);
    static int ParseInput(const QString &name);  // -1 none / unknown

    int step() const { return m_step; }
    int axis() const;
    const Found &found(int s) const { return m_found[s]; }
    int homeSide() const;  // +1 / -1, 0 not known
    QString message() const;
    // The settings Apply writes (also what the Limits and Home steps send).
    QStringList limitCommands() const;
    QStringList homeCommands() const;
    QStringList applyCommands() const;

    // For tests: drive it without a plotter.
    void setStepForTest(int step);
    void setWantedForTest(bool plus, bool minus, bool home);
    void findSwitch(int s);  // the Find button: the next input that changes and comes back is this switch's
    void feedLevels(int levels);
    void feedStop(int dir, const QString &input);
    void answer(bool motorReversed);
    void feedSwinfo(const QStringList &lines);
    void startHomeRunsForTest(int runs);
    bool limitStopped(int side) const { return m_limStopped[side]; }
    bool limitCleared(int side) const { return m_limCleared[side]; }
    double overtravelMm(int side) const { return m_overMm[side]; }
    int homeRuns() const { return m_runsDone; }
    double homeSpreadMm() const;

    void reject() override;  // stops a jog or homing first

signals:
    // Settings were written: the main window reads the config again.
    void settingsWritten();

private:
    void build();
    void go(int step);
    bool applies(int step) const;
    int nextStep(int from, int dir) const;
    void refresh();
    void poll();
    void onStatus(const PlotterLink::Status &s);
    void onCommand(const QString &command, bool ok, const QStringList &lines);
    void send(const QString &line);
    void sendAll(const QStringList &lines);
    void startJog(int dir);
    void stopJog();
    void jogTick();
    QString stopList() const;
    QString axisKey() const;  // "x" / "y"
    QString switchName(int s) const;
    QString blockedWhy(int code) const;
    int homeShare() const;    // 0 own input, 1 the - limit's, 2 the + limit's
    void writeLimits();
    void startHoming();
    void homeRun();
    void loadHomeDefaults();
    static int CfgInt(const QMap<QString, QString> &c, const QString &key, int def);

    PlotterLink *m_link;
    QMap<QString, QString> m_config;
    QTimer *m_poll = nullptr, *m_jogTimer = nullptr;
    bool m_swinfoBusy = false, m_jogBusy = false;
    int m_step = kStepAxis;
    Found m_found[kSwitches];
    bool m_motorOn = false;
    int m_levels = -1;        // the plotter's input levels, bit per code; -1 not known
    QString m_state, m_fault;

    // Find
    int m_finding = -1, m_baseLevels = -1, m_candidate = -1;
    QString m_findNote;
    // Direction
    int m_jogDir = 0;
    int m_seenHomeSide = 0;
    int m_askDir = 0;
    int m_dir = 1;            // dir_x / dir_y as the plotter has it
    QString m_dirNote;
    // Limits
    bool m_limitsWritten = false;
    bool m_tripBaseKnown = false;
    quint32 m_tripBase[2] = {0, 0};
    bool m_limStopped[2] = {false, false}, m_limCleared[2] = {false, false};
    double m_tripAt[2] = {0, 0}, m_overMm[2] = {-1, -1};
    bool m_moving = false;
    double m_pos = 0.0;
    QString m_limNote;
    // Home
    bool m_homeWritten = false, m_homeRunning = false, m_homeFailed = false, m_homeTest = false;
    bool m_endsKnown = false;
    quint32 m_endsBase = 0;
    int m_runsWanted = 0, m_runsDone = 0;
    QVector<double> m_devMm;
    QString m_homeNote;

    // Widgets
    QLabel *m_title = nullptr, *m_note = nullptr;
    QVector<QLabel *> m_stepLabels;
    QStackedWidget *m_pages = nullptr;
    QPushButton *m_back = nullptr, *m_next = nullptr, *m_cancel = nullptr;
    QComboBox *m_axis = nullptr, *m_share = nullptr;
    QCheckBox *m_want[kSwitches] = {nullptr, nullptr, nullptr};
    QDoubleSpinBox *m_speed = nullptr;
    QLabel *m_inputsLabel = nullptr, *m_findHelp = nullptr;
    QLabel *m_findName[kSwitches] = {nullptr, nullptr, nullptr}, *m_findInput[kSwitches] = {nullptr, nullptr, nullptr};
    QPushButton *m_findButton[kSwitches] = {nullptr, nullptr, nullptr};
    QLabel *m_contactName[kSwitches] = {nullptr, nullptr, nullptr}, *m_contactRest[kSwitches] = {nullptr, nullptr, nullptr};
    QCheckBox *m_contactOpen[kSwitches] = {nullptr, nullptr, nullptr};
    QPushButton *m_enable[2] = {nullptr, nullptr};
    QPushButton *m_jogMinus[2] = {nullptr, nullptr}, *m_jogPlus[2] = {nullptr, nullptr};
    QLabel *m_dirText = nullptr, *m_limText = nullptr;
    QFrame *m_ask = nullptr;
    QLabel *m_askText = nullptr;
    QDoubleSpinBox *m_homeFast = nullptr, *m_homeSlow = nullptr, *m_homeBack = nullptr, *m_homeAt = nullptr;
    QComboBox *m_homeDir = nullptr;
    QSpinBox *m_runs = nullptr;
    QPushButton *m_homeButton = nullptr;
    QLabel *m_homeText = nullptr, *m_review = nullptr;
    QCheckBox *m_required = nullptr;
};
