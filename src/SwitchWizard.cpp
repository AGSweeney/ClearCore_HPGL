/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#include "SwitchWizard.h"

#include "Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

const char *const kStepNames[] = {"Axis", "Find each switch", "Contact", "Direction", "Limits", "Home", "Review"};
// The inputs a switch can use, in the order the plotter's board shows them (DI-6 is the estop's).
const int kInputs[] = {7, 8, 9, 10, 11, 12, 0, 1, 2, 3, 4, 5};

QLabel *Help(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    l->setObjectName(QStringLiteral("ECamHelp"));
    l->setWordWrap(true);
    return l;
}

QString Mm(double v) {
    return QString::number(v, 'f', 3);
}

}  // namespace

QString SwitchWizard::InputName(int code) {
    if (code >= 0 && code <= 5) return QStringLiteral("IO-%1").arg(code);
    if (code == 7 || code == 8) return QStringLiteral("DI-%1").arg(code);
    if (code >= 9 && code <= 12) return QStringLiteral("A-%1").arg(code);
    return QStringLiteral("none");
}

int SwitchWizard::ParseInput(const QString &name) {
    for (int c : kInputs) {
        if (InputName(c).compare(name.trimmed(), Qt::CaseInsensitive) == 0) return c;
    }
    return -1;
}

SwitchWizard::Verdict SwitchWizard::Judge(int dir, int reached) {
    if (reached == kHome) return Verdict::Home;
    if (reached == kPlus) return dir > 0 ? Verdict::Right : Verdict::Ask;
    if (reached == kMinus) return dir < 0 ? Verdict::Right : Verdict::Ask;
    return Verdict::Unknown;
}

int SwitchWizard::CfgInt(const QMap<QString, QString> &c, const QString &key, int def) {
    bool ok = false;
    const int v = c.value(key).toInt(&ok);
    return ok ? v : def;
}

SwitchWizard::SwitchWizard(PlotterLink *link, const QMap<QString, QString> &config, QWidget *parent)
    : QDialog(parent), m_link(link), m_config(config) {
    setWindowTitle(QStringLiteral("Set up limits and homing"));
    setStyleSheet(theme::DialogSheet());
    theme::ApplyAccent(this);
    resize(860, 600);
    build();
    m_poll = new QTimer(this);
    m_poll->setInterval(300);
    connect(m_poll, &QTimer::timeout, this, &SwitchWizard::poll);
    m_jogTimer = new QTimer(this);
    m_jogTimer->setInterval(200);  // the plotter's jog deadman is 0.5 s
    connect(m_jogTimer, &QTimer::timeout, this, &SwitchWizard::jogTick);
    if (m_link) {
        connect(m_link, &PlotterLink::statusUpdated, this, &SwitchWizard::onStatus);
        connect(m_link, &PlotterLink::commandFinished, this, &SwitchWizard::onCommand);
        m_poll->start();
        onStatus(m_link->status());
    }
    go(kStepAxis);
}

SwitchWizard::~SwitchWizard() = default;

int SwitchWizard::axis() const {
    return m_axis ? std::max(0, m_axis->currentIndex()) : 0;
}

QString SwitchWizard::axisKey() const {
    return axis() ? QStringLiteral("y") : QStringLiteral("x");
}

QString SwitchWizard::switchName(int s) const {
    const QString a = axis() ? QStringLiteral("Y") : QStringLiteral("X");
    if (s == kPlus) return QStringLiteral("%1+ limit").arg(a);
    if (s == kMinus) return QStringLiteral("%1− limit").arg(a);
    return QStringLiteral("%1 home").arg(a);
}

int SwitchWizard::homeShare() const {
    return m_share ? m_share->currentIndex() : 0;
}

int SwitchWizard::homeSide() const {
    if (homeShare() == 1) return -1;
    if (homeShare() == 2) return 1;
    return m_seenHomeSide;
}

QString SwitchWizard::message() const {
    return m_note ? m_note->text() : QString();
}

/* ------------------------------------------------------------------------- */
/* Layout                                                                     */
/* ------------------------------------------------------------------------- */

void SwitchWizard::build() {
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *side = new QWidget(this);
    side->setObjectName(QStringLiteral("ECamSide"));
    side->setFixedWidth(200);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 18, 0, 12);
    sideLayout->setSpacing(2);
    for (int s = 0; s < kSteps; ++s) {
        auto *l = new QLabel(QString::fromUtf8(kStepNames[s]), side);
        l->setObjectName(QStringLiteral("ECamStep"));
        sideLayout->addWidget(l);
        m_stepLabels << l;
    }
    sideLayout->addStretch(1);
    outer->addWidget(side);

    auto *main = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(main);
    mainLayout->setContentsMargins(20, 16, 20, 14);
    mainLayout->setSpacing(10);
    m_title = new QLabel(main);
    m_title->setObjectName(QStringLiteral("ECamTitle"));
    mainLayout->addWidget(m_title);
    m_pages = new QStackedWidget(main);
    mainLayout->addWidget(m_pages, 1);
    m_note = new QLabel(main);
    m_note->setObjectName(QStringLiteral("ECamWarn"));
    m_note->setWordWrap(true);
    mainLayout->addWidget(m_note);
    auto *buttons = new QHBoxLayout();
    m_cancel = new QPushButton(QStringLiteral("Close"), main);
    m_back = new QPushButton(QStringLiteral("Back"), main);
    m_next = new QPushButton(QStringLiteral("Next"), main);
    m_next->setProperty("role", "primary");
    buttons->addWidget(m_cancel);
    buttons->addStretch(1);
    buttons->addWidget(m_back);
    buttons->addWidget(m_next);
    mainLayout->addLayout(buttons);
    outer->addWidget(main, 1);
    connect(m_cancel, &QPushButton::clicked, this, &SwitchWizard::reject);
    connect(m_back, &QPushButton::clicked, this, [this]() { go(nextStep(m_step, -1)); });
    connect(m_next, &QPushButton::clicked, this, [this]() {
        if (m_step == kStepReview) {
            sendAll(applyCommands());
            emit settingsWritten();
            accept();
            return;
        }
        go(nextStep(m_step, +1));
    });

    // Axis
    {
        auto *page = new QWidget(m_pages);
        auto *v = new QVBoxLayout(page);
        v->addWidget(Help(QStringLiteral("One axis at a time. Say which switches it has; each one is found, its contact read, "
                                         "and its job proved by driving the axis to it. Limits stop motion toward them "
                                         "(a job that reaches one is stopped and dropped); a home switch sets where the axis is."),
                          page));
        auto *form = new QFormLayout();
        m_axis = new QComboBox(page);
        m_axis->addItems({QStringLiteral("X"), QStringLiteral("Y")});
        form->addRow(QStringLiteral("Axis"), m_axis);
        static const char *const kWant[] = {"+ limit switch (the far end of the axis)", "− limit switch (the 0 end)",
                                            "Home switch"};
        for (int s = 0; s < kSwitches; ++s) {
            m_want[s] = new QCheckBox(QString::fromUtf8(kWant[s]), page);
            form->addRow(s == 0 ? QStringLiteral("Switches") : QString(), m_want[s]);
            connect(m_want[s], &QCheckBox::toggled, this, &SwitchWizard::refresh);
        }
        m_share = new QComboBox(page);
        m_share->addItems({QStringLiteral("its own input"), QStringLiteral("the − limit switch (homes on it)"),
                           QStringLiteral("the + limit switch (homes on it)")});
        form->addRow(QStringLiteral("Home switch is"), m_share);
        connect(m_share, &QComboBox::currentIndexChanged, this, &SwitchWizard::refresh);
        m_speed = new QDoubleSpinBox(page);
        m_speed->setRange(1.0, 100.0);
        m_speed->setDecimals(1);
        m_speed->setValue(10.0);
        m_speed->setSuffix(QStringLiteral(" mm/s"));
        form->addRow(QStringLiteral("Setup jog speed"), m_speed);
        v->addLayout(form);
        v->addWidget(Help(QStringLiteral("Start slow: the axis stops in about v² / (2 × 3 × acceleration) after a limit "
                                         "trips, plus three samples (0.6 ms) of filtering."),
                          page));
        v->addStretch(1);
        connect(m_axis, &QComboBox::currentIndexChanged, this, [this]() {
            for (Found &f : m_found) f = Found();
            m_dir = CfgInt(m_config, QStringLiteral("dir_") + axisKey(), 1) < 0 ? -1 : 1;
            m_seenHomeSide = 0;
            loadHomeDefaults();
            refresh();
        });
        m_pages->addWidget(page);
    }
    // Find
    {
        auto *page = new QWidget(m_pages);
        auto *v = new QVBoxLayout(page);
        m_findHelp = Help(QStringLiteral("Press Find, then trip that switch by hand and let it go (hold it about a second: "
                                         "the inputs are read three times a second). The input that changes is the "
                                         "switch's."),
                          page);
        v->addWidget(m_findHelp);
        auto *grid = new QGridLayout();
        for (int s = 0; s < kSwitches; ++s) {
            m_findName[s] = new QLabel(page);
            m_findInput[s] = new QLabel(page);
            m_findInput[s]->setObjectName(QStringLiteral("ECamDerived"));
            m_findButton[s] = new QPushButton(QStringLiteral("Find"), page);
            grid->addWidget(m_findName[s], s, 0);
            grid->addWidget(m_findInput[s], s, 1);
            grid->addWidget(m_findButton[s], s, 2);
            connect(m_findButton[s], &QPushButton::clicked, this, [this, s]() { findSwitch(s); });
        }
        grid->setColumnStretch(1, 1);
        v->addLayout(grid);
        m_inputsLabel = new QLabel(page);
        m_inputsLabel->setObjectName(QStringLiteral("ECamCard"));
        m_inputsLabel->setTextFormat(Qt::RichText);
        v->addWidget(m_inputsLabel);
        v->addStretch(1);
        m_pages->addWidget(page);
    }
    // Contact
    {
        auto *page = new QWidget(m_pages);
        auto *v = new QVBoxLayout(page);
        v->addWidget(Help(QStringLiteral("A switch that reads ON at rest is normally closed: a broken wire reads as tripped, "
                                         "which is the safe way for a limit. One that reads OFF at rest is normally open."),
                          page));
        auto *grid = new QGridLayout();
        for (int s = 0; s < kSwitches; ++s) {
            m_contactName[s] = new QLabel(page);
            m_contactRest[s] = new QLabel(page);
            m_contactRest[s]->setObjectName(QStringLiteral("ECamDerived"));
            m_contactOpen[s] = new QCheckBox(QStringLiteral("Normally open"), page);
            grid->addWidget(m_contactName[s], s, 0);
            grid->addWidget(m_contactRest[s], s, 1);
            grid->addWidget(m_contactOpen[s], s, 2);
            connect(m_contactOpen[s], &QCheckBox::toggled, this, [this, s](bool on) { m_found[s].normallyOpen = on; });
        }
        grid->setColumnStretch(1, 1);
        v->addLayout(grid);
        v->addStretch(1);
        m_pages->addWidget(page);
    }
    // Direction and Limits share a jog row.
    auto jogRow = [this](QWidget *page, int i) {
        auto *row = new QHBoxLayout();
        m_enable[i] = new QPushButton(QStringLiteral("Enable motors"), page);
        m_jogMinus[i] = new QPushButton(QStringLiteral("◀  Hold: jog −"), page);
        m_jogPlus[i] = new QPushButton(QStringLiteral("Hold: jog +  ▶"), page);
        for (QPushButton *b : {m_jogMinus[i], m_jogPlus[i]}) b->setMinimumHeight(40);
        row->addWidget(m_enable[i]);
        row->addStretch(1);
        row->addWidget(m_jogMinus[i]);
        row->addWidget(m_jogPlus[i]);
        connect(m_enable[i], &QPushButton::clicked, this, [this]() { send(m_motorOn ? QStringLiteral("disable") : QStringLiteral("enable")); });
        connect(m_jogMinus[i], &QPushButton::pressed, this, [this]() { startJog(-1); });
        connect(m_jogPlus[i], &QPushButton::pressed, this, [this]() { startJog(+1); });
        connect(m_jogMinus[i], &QPushButton::released, this, &SwitchWizard::stopJog);
        connect(m_jogPlus[i], &QPushButton::released, this, &SwitchWizard::stopJog);
        return row;
    };
    // Direction
    {
        auto *page = new QWidget(m_pages);
        auto *v = new QVBoxLayout(page);
        v->addWidget(Help(QStringLiteral("Hold Jog + until the axis stops on a switch, then Jog −. The plotter stops at the "
                                         "first of this axis's switches it reaches (at rest when the jog started); which one "
                                         "proves its job, and the direction the motor runs."),
                          page));
        v->addLayout(jogRow(page, 0));
        m_dirText = new QLabel(page);
        m_dirText->setObjectName(QStringLiteral("ECamCard"));
        m_dirText->setWordWrap(true);
        v->addWidget(m_dirText);
        m_ask = new QFrame(page);
        auto *askLayout = new QVBoxLayout(m_ask);
        m_askText = new QLabel(m_ask);
        m_askText->setWordWrap(true);
        askLayout->addWidget(m_askText);
        auto *askRow = new QHBoxLayout();
        auto *reversed = new QPushButton(QStringLiteral("The head went the other way: reverse the axis"), m_ask);
        auto *swapped = new QPushButton(QStringLiteral("I tripped them the wrong way round: swap them"), m_ask);
        askRow->addWidget(reversed);
        askRow->addWidget(swapped);
        askLayout->addLayout(askRow);
        connect(reversed, &QPushButton::clicked, this, [this]() { answer(true); });
        connect(swapped, &QPushButton::clicked, this, [this]() { answer(false); });
        v->addWidget(m_ask);
        v->addStretch(1);
        m_pages->addWidget(page);
    }
    // Limits
    {
        auto *page = new QWidget(m_pages);
        auto *v = new QVBoxLayout(page);
        v->addWidget(Help(QStringLiteral("The limits are on the plotter now. Hold Jog toward each limit until it stops the axis "
                                         "(a plain jog: only the limit stops it), then jog back off it."),
                          page));
        v->addLayout(jogRow(page, 1));
        m_limText = new QLabel(page);
        m_limText->setObjectName(QStringLiteral("ECamCard"));
        m_limText->setWordWrap(true);
        v->addWidget(m_limText);
        v->addStretch(1);
        m_pages->addWidget(page);
    }
    // Home
    {
        auto *page = new QWidget(m_pages);
        auto *v = new QVBoxLayout(page);
        v->addWidget(Help(QStringLiteral("Homing: fast toward the switch until it trips, back off until it lets go and a little "
                                         "more, then slowly back onto it. Where it trips becomes the position below. "
                                         "Home a few times: the spread of the latched positions is how well it repeats."),
                          page));
        auto *form = new QFormLayout();
        m_homeDir = new QComboBox(page);
        m_homeDir->addItems({QStringLiteral("toward −"), QStringLiteral("toward +")});
        form->addRow(QStringLiteral("Home direction"), m_homeDir);
        auto spin = [page](double lo, double hi, double val, const QString &suffix, int decimals) {
            auto *s = new QDoubleSpinBox(page);
            s->setRange(lo, hi);
            s->setDecimals(decimals);
            s->setValue(val);
            s->setSuffix(suffix);
            return s;
        };
        m_homeFast = spin(0.1, 1000, 20, QStringLiteral(" mm/s"), 1);
        m_homeSlow = spin(0.05, 100, 2, QStringLiteral(" mm/s"), 2);
        m_homeBack = spin(0.1, 100, 3, QStringLiteral(" mm"), 1);
        m_homeAt = spin(-100000, 100000, 0, QStringLiteral(" mm"), 3);
        form->addRow(QStringLiteral("Search speed"), m_homeFast);
        form->addRow(QStringLiteral("Latch speed"), m_homeSlow);
        form->addRow(QStringLiteral("Back-off"), m_homeBack);
        form->addRow(QStringLiteral("Position at the switch"), m_homeAt);
        m_runs = new QSpinBox(page);
        m_runs->setRange(1, 20);
        m_runs->setValue(3);
        auto *runRow = new QHBoxLayout();
        runRow->addWidget(m_runs);
        m_homeButton = new QPushButton(QStringLiteral("Write and home"), page);
        runRow->addWidget(m_homeButton);
        runRow->addStretch(1);
        form->addRow(QStringLiteral("Times"), runRow);
        v->addLayout(form);
        m_homeText = new QLabel(page);
        m_homeText->setObjectName(QStringLiteral("ECamCard"));
        m_homeText->setWordWrap(true);
        v->addWidget(m_homeText);
        v->addStretch(1);
        connect(m_homeButton, &QPushButton::clicked, this, [this]() {
            if (m_homeRunning) {
                send(QStringLiteral("home stop"));
                return;
            }
            startHoming();
        });
        m_pages->addWidget(page);
    }
    // Review
    {
        auto *page = new QWidget(m_pages);
        auto *v = new QVBoxLayout(page);
        m_review = new QLabel(page);
        m_review->setObjectName(QStringLiteral("ECamCard"));
        m_review->setWordWrap(true);
        m_review->setTextFormat(Qt::RichText);
        v->addWidget(m_review);
        m_required = new QCheckBox(QStringLiteral("Home before plotting (the plotter homes by itself when a job arrives)"), page);
        m_required->setChecked(CfgInt(m_config, QStringLiteral("home_required"), 0) == 1);
        v->addWidget(m_required);
        v->addStretch(1);
        m_pages->addWidget(page);
    }
    m_dir = CfgInt(m_config, QStringLiteral("dir_x"), 1) < 0 ? -1 : 1;
    loadHomeDefaults();
}

void SwitchWizard::loadHomeDefaults() {
    if (!m_homeFast) return;
    const QString a = axisKey() + QStringLiteral("_home_");
    auto val = [this](const QString &k, double def) {
        bool ok = false;
        const double v = m_config.value(k).toDouble(&ok);
        return ok ? v : def;
    };
    m_homeFast->setValue(val(a + QStringLiteral("fast"), 20));
    m_homeSlow->setValue(val(a + QStringLiteral("slow"), 2));
    m_homeBack->setValue(val(a + QStringLiteral("back"), 3));
    m_homeAt->setValue(val(a + QStringLiteral("at"), 0));
    m_homeDir->setCurrentIndex(val(a + QStringLiteral("dir"), -1) > 0 ? 1 : 0);
}

/* ------------------------------------------------------------------------- */
/* Steps                                                                      */
/* ------------------------------------------------------------------------- */

bool SwitchWizard::applies(int step) const {
    const bool limits = m_want[kPlus]->isChecked() || m_want[kMinus]->isChecked();
    if (step == kStepFind || step == kStepContact || step == kStepDirection) {
        return limits || (m_want[kHome]->isChecked() && homeShare() == 0);
    }
    if (step == kStepLimits) return limits;
    if (step == kStepHome) return m_want[kHome]->isChecked();
    return step >= 0 && step < kSteps;
}

int SwitchWizard::nextStep(int from, int dir) const {
    int s = from + dir;
    while (s > 0 && s < kSteps - 1 && !applies(s)) s += dir;
    return std::clamp(s, 0, int(kSteps) - 1);
}

void SwitchWizard::go(int step) {
    if (m_jogDir != 0) stopJog();
    const int was = m_step;
    m_step = std::clamp(step, 0, int(kSteps) - 1);
    if (was == kStepAxis && m_step != kStepAxis) {
        for (int s = 0; s < kSwitches; ++s) {
            m_found[s].wanted = m_want[s]->isChecked() && !(s == kHome && homeShare() != 0);
        }
    }
    m_pages->setCurrentIndex(m_step);
    int index = 0, count = 0;
    for (int s = 0; s < kSteps; ++s) {
        if (!applies(s)) {
            m_stepLabels[s]->setVisible(false);
            continue;
        }
        m_stepLabels[s]->setVisible(true);
        ++count;
        if (s <= m_step) index = count;
        m_stepLabels[s]->setProperty("current", s == m_step);
        m_stepLabels[s]->setProperty("done", s < m_step);
        m_stepLabels[s]->style()->unpolish(m_stepLabels[s]);
        m_stepLabels[s]->style()->polish(m_stepLabels[s]);
    }
    m_title->setText(QStringLiteral("%1 of %2: %3").arg(index).arg(count).arg(QString::fromUtf8(kStepNames[m_step])));
    if (m_step == kStepContact) {
        for (int s = 0; s < kSwitches; ++s) {
            const Found &f = m_found[s];
            if (f.wanted && f.input >= 0) m_contactOpen[s]->setChecked(!f.restHigh);
        }
    }
    if (m_step == kStepLimits && !m_limitsWritten) writeLimits();
    if (m_step == kStepHome && m_homeDir && homeSide() != 0) {
        m_homeDir->setCurrentIndex(homeSide() > 0 ? 1 : 0);
        // Homing at the far end: the switch is about where the plot area ends.
        const double size = m_config.value(axis() ? QStringLiteral("height_mm") : QStringLiteral("width_mm")).toDouble();
        if (homeSide() > 0 && m_homeAt->value() == 0.0 && size > 0) m_homeAt->setValue(size);
    }
    refresh();
}

void SwitchWizard::setStepForTest(int step) {
    m_step = step;
    go(step);
}

void SwitchWizard::findSwitch(int s) {
    m_finding = s;
    m_baseLevels = m_levels;
    m_candidate = -1;
    m_findNote = QStringLiteral("Trip the %1 switch now, then let it go.").arg(switchName(s));
    refresh();
}

void SwitchWizard::setWantedForTest(bool plus, bool minus, bool home) {
    m_want[kPlus]->setChecked(plus);
    m_want[kMinus]->setChecked(minus);
    m_want[kHome]->setChecked(home);
    for (int s = 0; s < kSwitches; ++s) m_found[s].wanted = m_want[s]->isChecked();
}

/* ------------------------------------------------------------------------- */
/* The plotter                                                                */
/* ------------------------------------------------------------------------- */

void SwitchWizard::send(const QString &line) {
    if (m_link && m_link->isConnected()) m_link->sendCommand(line);
}

void SwitchWizard::sendAll(const QStringList &lines) {
    for (const QString &l : lines) send(l);
}

void SwitchWizard::poll() {
    if (!m_link || !m_link->isConnected() || m_swinfoBusy) return;
    if (m_step == kStepLimits || m_step == kStepHome || m_step == kStepDirection) {
        m_swinfoBusy = true;
        send(QStringLiteral("swinfo"));
    }
}

void SwitchWizard::onStatus(const PlotterLink::Status &s) {
    if (!s.valid) return;
    m_motorOn = s.enabled;
    m_state = s.state;
    m_fault = s.fault;
    if (s.inputs >= 0) {
        feedLevels(s.inputs);
    } else {
        refresh();
    }
}

void SwitchWizard::onCommand(const QString &command, bool ok, const QStringList &lines) {
    const QString last = lines.isEmpty() ? QString() : lines.last();
    const QString why = last.startsWith(QLatin1String("error: ")) ? last.mid(7) : last;
    if (command == QLatin1String("swinfo")) {
        m_swinfoBusy = false;
        if (ok) feedSwinfo(lines);
        return;
    }
    if (command == QLatin1String("config")) {
        static const QRegularExpression kLine(QStringLiteral("^(\\S+)\\s+=\\s+(\\S*)"));
        for (const QString &l : lines) {
            const QRegularExpressionMatch m = kLine.match(l);
            if (m.hasMatch()) m_config.insert(m.captured(1), m.captured(2));
        }
        return;
    }
    if (command.startsWith(QLatin1String("jog ")) && command != QLatin1String("jog stop")) {
        m_jogBusy = false;
        if (ok || m_jogDir == 0) return;
        static const QRegularExpression kStopped(QStringLiteral("stopped on a switch: (\\S+)"));
        const QRegularExpressionMatch m = kStopped.match(why);
        const int dir = m_jogDir;
        m_jogDir = 0;
        m_jogTimer->stop();
        if (m.hasMatch()) {
            Q_UNUSED(dir);
            send(QStringLiteral("jog stop"));
            send(QStringLiteral("swinfo"));  // the stop is read from its laststop line
            refresh();
            return;
        }
        if (m_step == kStepLimits) m_limNote = why;
        else m_dirNote = why;
        refresh();
        return;
    }
    if (command.startsWith(QLatin1String("home"))) {
        if (!ok && command != QLatin1String("home stop")) {
            m_homeRunning = false;
            m_homeFailed = true;
            m_homeNote = why;
            refresh();
        }
        return;
    }
    if (!ok && (command.startsWith(QLatin1String("set ")) || command == QLatin1String("enable") ||
                command == QLatin1String("disable"))) {
        const QString text = QStringLiteral("%1: %2").arg(command, why);
        if (m_step == kStepDirection) m_dirNote = text;
        else if (m_step == kStepLimits) m_limNote = text;
        else if (m_step == kStepHome) m_homeNote = text;
        else m_note->setText(text);
        refresh();
    }
}

void SwitchWizard::feedLevels(int levels) {
    m_levels = levels;
    if (m_step == kStepFind && m_finding >= 0 && m_baseLevels >= 0) {
        const int changed = (levels ^ m_baseLevels);
        if (m_candidate < 0) {
            for (int c : kInputs) {
                if (!((changed >> c) & 1)) continue;
                const QString blocked = blockedWhy(c);
                if (!blocked.isEmpty()) {
                    m_findNote = blocked;
                    continue;
                }
                m_candidate = c;
                m_findNote = QStringLiteral("%1 changed: now let the switch go.").arg(InputName(c));
                break;
            }
        } else if (!((changed >> m_candidate) & 1)) {
            Found &f = m_found[m_finding];
            f.input = m_candidate;
            f.restHigh = ((m_baseLevels >> m_candidate) & 1) != 0;
            f.normallyOpen = !f.restHigh;
            f.proved = false;
            m_findNote = QStringLiteral("%1 is on %2.").arg(switchName(m_finding), InputName(m_candidate));
            m_finding = -1;
            m_candidate = -1;
        }
    }
    refresh();
}

// Why this input can't be one of this axis's switches, empty when it can.
QString SwitchWizard::blockedWhy(int code) const {
    const QString name = InputName(code);
    for (int s = 0; s < kSwitches; ++s) {
        if (s != m_finding && m_found[s].wanted && m_found[s].input == code) {
            return QStringLiteral("%1 is already the %2 switch.").arg(name, switchName(s));
        }
    }
    if (CfgInt(m_config, QStringLiteral("pen_mode"), 0) == 0 && CfgInt(m_config, QStringLiteral("pen_io"), -1) == code) {
        return QStringLiteral("%1 is the pen's output.").arg(name);
    }
    const QString other = axis() ? QStringLiteral("x") : QStringLiteral("y");
    for (const char *k : {"_limit_plus", "_limit_minus", "_home"}) {
        if (ParseInput(m_config.value(other + QLatin1String(k))) == code) {
            return QStringLiteral("%1 is the other axis's %2 switch.").arg(name, other + QLatin1String(k));
        }
    }
    return QString();
}

QString SwitchWizard::stopList() const {
    QStringList names;
    for (int s = 0; s < kSwitches; ++s) {
        const Found &f = m_found[s];
        if (!f.wanted || f.input < 0) continue;
        const int now = m_levels >= 0 ? ((m_levels >> f.input) & 1) : -1;
        if (now >= 0 && now != (f.restHigh ? 1 : 0)) continue;  // tripped now: it can't stop this jog
        names << InputName(f.input);
    }
    return names.join(QLatin1Char(','));
}

void SwitchWizard::startJog(int dir) {
    if (m_askDir != 0) return;
    if (!m_motorOn) {
        (m_step == kStepLimits ? m_limNote : m_dirNote) = QStringLiteral("Enable the motors first.");
        refresh();
        return;
    }
    if (m_step == kStepDirection && stopList().isEmpty()) {
        m_dirNote = QStringLiteral("Every switch reads tripped: nothing could stop the axis.");
        refresh();
        return;
    }
    // No limit is on the plotter yet in this step: a limit that reads tripped is not a stop for this jog,
    // so never jog further into it. Off it is the way that did not reach it.
    if (m_step == kStepDirection) {
        for (int s : {int(kPlus), int(kMinus)}) {
            const Found &f = m_found[s];
            if (!f.wanted || f.input < 0 || m_levels < 0) continue;
            const bool tripped = ((m_levels >> f.input) & 1) != (f.restHigh ? 1 : 0);
            if (!tripped) continue;
            if (m_limitHitDir[s] == 0) {
                m_dirNote = QStringLiteral("The %1 switch (%2) reads tripped, and it is not known which way is off it. "
                                           "Disable the motors and move the axis off it by hand.")
                                .arg(switchName(s), InputName(f.input));
                refresh();
                return;
            }
            if (dir == m_limitHitDir[s]) {
                m_dirNote = QStringLiteral("The %1 switch (%2) is tripped: jog the other way.").arg(switchName(s), InputName(f.input));
                refresh();
                return;
            }
        }
    }
    m_lastJogDir = dir;
    m_jogDir = dir;
    m_dirNote.clear();
    m_limNote.clear();
    jogTick();
    m_jogTimer->start();
    refresh();
}

void SwitchWizard::jogTick() {
    if (m_jogDir == 0 || m_jogBusy) return;
    m_jogBusy = true;
    const double v = m_jogDir * m_speed->value();
    // Direction: a stop on a switch (no limit is on the plotter yet). Limits: a plain jog, so the limit stops it.
    QString line = QStringLiteral("jog %1 %2").arg(axisKey()).arg(v, 0, 'g', 10);
    if (m_step == kStepDirection) line += QStringLiteral(" stopon %1").arg(stopList());
    send(line);
}

void SwitchWizard::stopJog() {
    m_jogTimer->stop();
    if (m_jogDir == 0) return;
    m_jogDir = 0;
    send(QStringLiteral("jog stop"));
    refresh();
}

void SwitchWizard::feedStop(int dir, const QString &input) {
    int reached = -1;
    for (int s = 0; s < kSwitches; ++s) {
        if (m_found[s].wanted && InputName(m_found[s].input) == input) reached = s;
    }
    const QString way = dir > 0 ? QStringLiteral("+") : QStringLiteral("−");
    if (reached == kPlus || reached == kMinus) m_limitHitDir[reached] = dir;
    switch (Judge(dir, reached)) {
        case Verdict::Right:
            m_found[reached].proved = true;
            m_dirNote = QStringLiteral("%1 reached going %2: right.").arg(switchName(reached), way);
            break;
        case Verdict::Home:
            m_seenHomeSide = dir;
            m_found[kHome].proved = true;
            m_dirNote = QStringLiteral("Home is on the %1 side. Hold the same jog again for the limit beyond it.").arg(way);
            break;
        case Verdict::Ask:
            m_askDir = dir;
            m_dirNote = QStringLiteral("%1 reached going %2.").arg(switchName(reached), way);
            break;
        case Verdict::Unknown:
            m_dirNote = QStringLiteral("Stopped at %1, which isn't one of this axis's switches.").arg(input);
            break;
    }
    refresh();
}

// Going m_askDir, the axis reached the other limit. The head went toward the other end: the axis's + is the other
// way (dir_x / dir_y). Or it went where it was told, and the switches were tripped the other way round in Find.
void SwitchWizard::answer(bool motorReversed) {
    if (m_askDir == 0) return;
    const int dir = m_askDir;
    m_askDir = 0;
    if (!motorReversed) {
        std::swap(m_found[kPlus].input, m_found[kMinus].input);
        std::swap(m_found[kPlus].restHigh, m_found[kMinus].restHigh);
        std::swap(m_found[kPlus].normallyOpen, m_found[kMinus].normallyOpen);
        std::swap(m_limitHitDir[kPlus], m_limitHitDir[kMinus]);
        m_found[dir > 0 ? kPlus : kMinus].proved = true;
        m_found[dir > 0 ? kMinus : kPlus].proved = false;
        m_dirNote = QStringLiteral("The + and − switches were the other way round: swapped.");
        refresh();
        return;
    }
    m_dir = -m_dir;
    for (Found &f : m_found) f.proved = false;
    for (int &d : m_limitHitDir) d = -d;  // the same physical way is the other sign now
    m_seenHomeSide = 0;
    send(QStringLiteral("disable"));
    send(QStringLiteral("set dir_%1 %2").arg(axisKey()).arg(m_dir));
    m_config.insert(QStringLiteral("dir_") + axisKey(), QString::number(m_dir));
    emit settingsWritten();
    m_dirNote = QStringLiteral("Reversed (dir_%1 = %2). Enable, and jog each way again.").arg(axisKey()).arg(m_dir);
    refresh();
}

QStringList SwitchWizard::limitCommands() const {
    const QString a = axisKey();
    const int base = axis() * 3;
    int open = CfgInt(m_config, QStringLiteral("switch_open"), 0) & ~(7 << base);
    for (int s = 0; s < kSwitches; ++s) {
        const Found &f = m_found[s];
        if (f.wanted && f.input >= 0 && f.normallyOpen) open |= 1 << (base + s);
    }
    // A home switch on a limit has that limit's contact.
    if (m_want[kHome] && m_want[kHome]->isChecked() && homeShare() != 0) {
        const int lim = homeShare() == 1 ? kMinus : kPlus;
        if ((open >> (base + lim)) & 1) open |= 1 << (base + kHome);
    }
    QStringList c;
    // Clear first: the plotter refuses an input two switches would share.
    c << QStringLiteral("set %1_limit_plus none").arg(a) << QStringLiteral("set %1_limit_minus none").arg(a)
      << QStringLiteral("set %1_home none").arg(a) << QStringLiteral("set switch_open %1").arg(open);
    if (m_found[kPlus].wanted && m_found[kPlus].input >= 0)
        c << QStringLiteral("set %1_limit_plus %2").arg(a, InputName(m_found[kPlus].input));
    if (m_found[kMinus].wanted && m_found[kMinus].input >= 0)
        c << QStringLiteral("set %1_limit_minus %2").arg(a, InputName(m_found[kMinus].input));
    return c;
}

QStringList SwitchWizard::homeCommands() const {
    QStringList c;
    if (!m_want[kHome] || !m_want[kHome]->isChecked()) return c;
    const QString a = axisKey();
    int input = m_found[kHome].input;
    if (homeShare() == 1) input = m_found[kMinus].input;
    if (homeShare() == 2) input = m_found[kPlus].input;
    if (input < 0) return c;
    c << QStringLiteral("set %1_home %2").arg(a, InputName(input))
      << QStringLiteral("set %1_home_dir %2").arg(a).arg(m_homeDir->currentIndex() == 1 ? 1 : -1)
      << QStringLiteral("set %1_home_fast %2").arg(a).arg(m_homeFast->value())
      << QStringLiteral("set %1_home_slow %2").arg(a).arg(m_homeSlow->value())
      << QStringLiteral("set %1_home_back %2").arg(a).arg(m_homeBack->value())
      << QStringLiteral("set %1_home_at %2").arg(a).arg(m_homeAt->value(), 0, 'f', 3)
      << QStringLiteral("set home_mode 1");
    return c;
}

QStringList SwitchWizard::applyCommands() const {
    QStringList c = limitCommands() + homeCommands();
    if (m_required) c << QStringLiteral("set home_required %1").arg(m_required->isChecked() ? 1 : 0);
    return c;
}

void SwitchWizard::writeLimits() {
    sendAll(limitCommands());
    m_limitsWritten = true;
    m_tripBaseKnown = false;
    for (int s = 0; s < 2; ++s) {
        m_limStopped[s] = m_limCleared[s] = false;
        m_overMm[s] = -1;
    }
    emit settingsWritten();
}

// Lines of "swinfo": the Limits step's stops and the Home step's runs.
void SwitchWizard::feedSwinfo(const QStringList &lines) {
    static const QRegularExpression kSwitch(QStringLiteral(
        "^switch (\\S+) input=(\\S+) open=(\\d) tripped=(\\d) trips=(\\d+) at=(-?[0-9.]+)"));
    static const QRegularExpression kPos(QStringLiteral("^pos x=(-?[0-9.]+) y=(-?[0-9.]+) moving=(\\d)"));
    static const QRegularExpression kLastStop(QStringLiteral("^laststop n=(\\d+) kind=(\\S+)(?: switch=\\S+ input=(\\S+))?"));
    static const QRegularExpression kHomeLine(QStringLiteral(
        "^home mode=\\d+ state=(\\S+) homed=(\\d+) x_ends=(\\d+) x_latch=(-?[0-9.]+) y_ends=(\\d+) "
        "y_latch=(-?[0-9.]+) last=(.*)$"));
    const QString a = axisKey();
    bool tripped[2] = {false, false};
    quint32 trips[2] = {0, 0};
    double at[2] = {0, 0};
    bool haveLimits = false;
    for (const QString &l : lines) {
        QRegularExpressionMatch m = kSwitch.match(l);
        if (m.hasMatch()) {
            const QString key = m.captured(1);
            for (int s = 0; s < 2; ++s) {
                if (key == a + (s == 0 ? QStringLiteral("+") : QStringLiteral("-"))) {
                    tripped[s] = m.captured(4) == QLatin1String("1");
                    trips[s] = m.captured(5).toUInt();
                    at[s] = m.captured(6).toDouble();
                    haveLimits = true;
                }
            }
            continue;
        }
        m = kLastStop.match(l);
        if (m.hasMatch()) {
            const quint32 n = m.captured(1).toUInt();
            if (!m_stopCountKnown) {
                m_stopCountKnown = true;
                m_stopCount = n;
            } else if (n != m_stopCount) {
                m_stopCount = n;
                // A stop-on input stopped a jog (even one already let go): Direction judges it.
                if (m.captured(2) == QLatin1String("input") && m_step == kStepDirection && m_lastJogDir != 0) {
                    if (m_jogDir != 0) {
                        m_jogDir = 0;
                        m_jogTimer->stop();
                        send(QStringLiteral("jog stop"));
                    }
                    feedStop(m_lastJogDir, m.captured(3));
                }
            }
            continue;
        }
        m = kPos.match(l);
        if (m.hasMatch()) {
            m_pos = m.captured(axis() ? 2 : 1).toDouble();
            m_moving = m.captured(3) == QLatin1String("1");
            continue;
        }
        m = kHomeLine.match(l);
        if (m.hasMatch() && m_homeRunning) {
            const quint32 ends = m.captured(axis() ? 5 : 3).toUInt();
            const double latch = m.captured(axis() ? 6 : 4).toDouble();
            const QString last = m.captured(7).trimmed();
            if (!m_endsKnown) {
                m_endsKnown = true;
                m_endsBase = ends;
            } else if (ends != m_endsBase) {
                m_endsBase = ends;
                if (last != QLatin1String("ok")) {
                    m_homeRunning = false;
                    m_homeFailed = last != QLatin1String("stopped");
                    m_homeNote = last == QLatin1String("stopped") ? QStringLiteral("Stopped.")
                                                                 : QStringLiteral("Homing failed: %1").arg(last);
                } else {
                    // The first homing sets where the switch is; each one after latches against it.
                    m_devMm << (m_runsDone == 0 ? 0.0 : latch - m_homeAt->value());
                    ++m_runsDone;
                    if (m_runsDone < m_runsWanted) {
                        homeRun();
                    } else {
                        m_homeRunning = false;
                    }
                }
            }
        }
    }
    if (haveLimits && m_step == kStepLimits) {
        if (!m_tripBaseKnown) {
            m_tripBaseKnown = true;
            m_tripBase[0] = trips[0];
            m_tripBase[1] = trips[1];
        }
        for (int s = 0; s < 2; ++s) {
            if (trips[s] != m_tripBase[s]) {
                m_tripBase[s] = trips[s];
                m_limStopped[s] = true;
                m_limCleared[s] = false;
                m_tripAt[s] = at[s];
                m_overMm[s] = -1;
                m_found[s].proved = true;
            }
            if (m_limStopped[s] && m_overMm[s] < 0 && !m_moving) m_overMm[s] = std::fabs(m_pos - m_tripAt[s]);
            if (m_limStopped[s] && m_overMm[s] >= 0 && !tripped[s]) m_limCleared[s] = true;
        }
    }
    refresh();
}

void SwitchWizard::startHoming() {
    if (!m_homeWritten || QMessageBox::question(this, windowTitle(),
                                                QStringLiteral("Write these homing settings to the plotter and home %1 %2 times?")
                                                    .arg(axisKey().toUpper())
                                                    .arg(m_runs->value())) == QMessageBox::Yes) {
        sendAll(homeCommands());
        m_homeWritten = true;
        emit settingsWritten();
        m_runsWanted = m_runs->value();
        m_runsDone = 0;
        m_devMm.clear();
        m_homeFailed = false;
        m_homeNote.clear();
        m_endsKnown = false;
        m_homeRunning = true;
        send(QStringLiteral("swinfo"));  // the end count before the first run
        homeRun();
        refresh();
    }
}

void SwitchWizard::homeRun() {
    if (!m_homeRunning || m_homeTest) return;  // a test feeds the results itself
    send(QStringLiteral("home %1").arg(axisKey()));
}

void SwitchWizard::startHomeRunsForTest(int runs) {
    m_runsWanted = runs;
    m_runsDone = 0;
    m_devMm.clear();
    m_homeFailed = false;
    m_homeRunning = true;
    m_endsKnown = false;
    m_homeTest = true;
}

double SwitchWizard::homeSpreadMm() const {
    if (m_devMm.size() < 2) return 0.0;
    const auto mm = std::minmax_element(m_devMm.begin() + 1, m_devMm.end());
    return *mm.second - *mm.first;
}

void SwitchWizard::reject() {
    if (m_jogDir != 0) stopJog();
    if (m_homeRunning) send(QStringLiteral("home stop"));
    m_poll->stop();
    QDialog::reject();
}

/* ------------------------------------------------------------------------- */
/* Display                                                                    */
/* ------------------------------------------------------------------------- */

void SwitchWizard::refresh() {
    if (!m_pages) return;
    const bool conn = m_link && m_link->isConnected();
    m_share->setEnabled(m_want[kHome]->isChecked() && (m_want[kPlus]->isChecked() || m_want[kMinus]->isChecked()));
    if (!m_share->isEnabled() && m_share->currentIndex() != 0) m_share->setCurrentIndex(0);
    if (m_share->currentIndex() == 1 && !m_want[kMinus]->isChecked()) m_share->setCurrentIndex(0);
    if (m_share->currentIndex() == 2 && !m_want[kPlus]->isChecked()) m_share->setCurrentIndex(0);

    QString note;
    bool canNext = true;
    switch (m_step) {
        case kStepAxis:
            canNext = m_want[kPlus]->isChecked() || m_want[kMinus]->isChecked() || m_want[kHome]->isChecked();
            if (!canNext) note = QStringLiteral("Pick at least one switch.");
            break;
        case kStepFind: {
            for (int s = 0; s < kSwitches; ++s) {
                const Found &f = m_found[s];
                m_findName[s]->setVisible(f.wanted);
                m_findInput[s]->setVisible(f.wanted);
                m_findButton[s]->setVisible(f.wanted);
                m_findName[s]->setText(switchName(s));
                m_findInput[s]->setText(m_finding == s ? QStringLiteral("trip it now…")
                                                       : (f.input >= 0 ? InputName(f.input) : QStringLiteral("not found")));
                m_findButton[s]->setEnabled(conn && m_levels >= 0);
                if (f.wanted && f.input < 0) canNext = false;
            }
            QStringList cells;
            for (int c : kInputs) {
                const bool on = m_levels >= 0 && ((m_levels >> c) & 1);
                cells << QStringLiteral("<span style='color:%1'>%2 %3</span>")
                             .arg(on ? QStringLiteral("#7fe3a0") : QStringLiteral("#7b8490"), InputName(c),
                                  on ? QStringLiteral("●") : QStringLiteral("○"));
            }
            m_inputsLabel->setText(m_levels >= 0 ? cells.join(QStringLiteral("&nbsp;&nbsp; "))
                                                 : QStringLiteral("Inputs: not read yet (connect to the plotter; "
                                                                  "this needs its limits-and-homing firmware)."));
            note = m_findNote;
            break;
        }
        case kStepContact:
            for (int s = 0; s < kSwitches; ++s) {
                const Found &f = m_found[s];
                const bool show = f.wanted && f.input >= 0;
                m_contactName[s]->setVisible(show);
                m_contactRest[s]->setVisible(show);
                m_contactOpen[s]->setVisible(show);
                m_contactName[s]->setText(QStringLiteral("%1 (%2)").arg(switchName(s), InputName(f.input)));
                m_contactRest[s]->setText(f.restHigh ? QStringLiteral("ON at rest: normally closed")
                                                     : QStringLiteral("OFF at rest: normally open"));
            }
            break;
        case kStepDirection:
        case kStepLimits: {
            const int i = m_step == kStepDirection ? 0 : 1;
            m_enable[i]->setText(m_motorOn ? QStringLiteral("Disable motors") : QStringLiteral("Enable motors"));
            m_enable[i]->setEnabled(conn);
            for (QPushButton *b : {m_jogMinus[i], m_jogPlus[i]}) b->setEnabled(conn && m_motorOn && m_askDir == 0);
            if (m_step == kStepDirection) {
                QStringList rows;
                for (int s = 0; s < kSwitches; ++s) {
                    const Found &f = m_found[s];
                    if (!f.wanted) continue;
                    rows << QStringLiteral("%1 (%2): %3").arg(switchName(s), InputName(f.input),
                                                              f.proved ? QStringLiteral("proved") : QStringLiteral("not reached yet"));
                    if (!f.proved) canNext = false;
                }
                if (m_seenHomeSide != 0)
                    rows << QStringLiteral("Home is on the %1 side.").arg(m_seenHomeSide > 0 ? QStringLiteral("+") : QStringLiteral("−"));
                m_dirText->setText(rows.join(QLatin1Char('\n')));
                m_ask->setVisible(m_askDir != 0);
                m_askText->setText(QStringLiteral("The axis was jogged %1 and stopped on the switch found as the %2 limit. "
                                                  "Which end did the head go to?")
                                       .arg(m_askDir > 0 ? QStringLiteral("+") : QStringLiteral("−"),
                                            m_askDir > 0 ? QStringLiteral("−") : QStringLiteral("+")));
                note = m_dirNote;
            } else {
                QStringList rows;
                for (int s = 0; s < 2; ++s) {
                    if (!m_found[s].wanted) continue;
                    QString r = QStringLiteral("%1 (%2): ").arg(switchName(s), InputName(m_found[s].input));
                    if (!m_limStopped[s]) {
                        r += QStringLiteral("jog toward it until it stops the axis");
                        canNext = false;
                    } else {
                        r += QStringLiteral("stopped it at %1 mm").arg(Mm(m_tripAt[s]));
                        if (m_overMm[s] >= 0) r += QStringLiteral(", ran %1 mm past").arg(Mm(m_overMm[s]));
                        r += m_limCleared[s] ? QStringLiteral(", and off it again") : QStringLiteral("; now jog back off it");
                        if (!m_limCleared[s]) canNext = false;
                    }
                    rows << r;
                }
                rows << QStringLiteral("Position %1 mm%2").arg(Mm(m_pos), m_moving ? QStringLiteral(" (moving)") : QString());
                m_limText->setText(rows.join(QLatin1Char('\n')));
                note = m_limNote;
            }
            break;
        }
        case kStepHome: {
            m_homeButton->setText(m_homeRunning ? QStringLiteral("Stop") : QStringLiteral("Write and home"));
            m_homeButton->setEnabled(conn);
            QString t;
            if (m_runsDone > 0 || m_homeRunning) {
                t = QStringLiteral("Homed %1 of %2.").arg(m_runsDone).arg(m_runsWanted);
                if (m_runsDone >= 2) {
                    t += QStringLiteral(" Spread of the latched positions: %1 mm").arg(Mm(homeSpreadMm()));
                }
            } else {
                t = QStringLiteral("Not homed yet. The plotter lifts the pen first.");
            }
            m_homeText->setText(t);
            note = m_homeNote;
            canNext = !m_homeRunning;
            break;
        }
        case kStepReview: {
            QStringList rows;
            rows << QStringLiteral("<b>%1 axis</b>").arg(axisKey().toUpper());
            for (int s = 0; s < kSwitches; ++s) {
                const Found &f = m_found[s];
                if (s == kHome && m_want[kHome]->isChecked() && homeShare() != 0) {
                    rows << QStringLiteral("%1: on the %2 limit switch").arg(switchName(s),
                                                                            homeShare() == 1 ? QStringLiteral("−") : QStringLiteral("+"));
                    continue;
                }
                if (!f.wanted) continue;
                rows << QStringLiteral("%1: %2, normally %3, %4")
                            .arg(switchName(s), InputName(f.input), f.normallyOpen ? QStringLiteral("open") : QStringLiteral("closed"),
                                 f.proved ? QStringLiteral("proved") : QStringLiteral("<span style='color:#ffb07a'>not proved</span>"));
            }
            for (int s = 0; s < 2; ++s) {
                if (m_found[s].wanted && m_overMm[s] >= 0)
                    rows << QStringLiteral("%1 ran %2 mm past its switch at %3 mm/s").arg(switchName(s), Mm(m_overMm[s])).arg(m_speed->value());
            }
            if (m_want[kHome]->isChecked()) {
                rows << QStringLiteral("Homing %1 at %2 / %3 mm/s, back-off %4 mm; the switch is at %5 mm")
                            .arg(m_homeDir->currentIndex() == 1 ? QStringLiteral("toward +") : QStringLiteral("toward −"))
                            .arg(m_homeFast->value())
                            .arg(m_homeSlow->value())
                            .arg(m_homeBack->value())
                            .arg(Mm(m_homeAt->value()));
                if (m_runsDone >= 2) rows << QStringLiteral("Repeats within %1 mm over %2 homings").arg(Mm(homeSpreadMm())).arg(m_runsDone);
            }
            rows << QString() << QStringLiteral("Apply writes these settings to the plotter (they are saved there).");
            m_review->setText(rows.join(QStringLiteral("<br>")));
            break;
        }
        default:
            break;
    }
    if (!conn) note = QStringLiteral("Not connected to the plotter.");
    else if (!m_fault.isEmpty() && m_fault != QLatin1String("none") && note.isEmpty()) note = QStringLiteral("Plotter fault: %1").arg(m_fault);
    m_note->setText(note);
    m_back->setEnabled(m_step > 0 && m_jogDir == 0 && !m_homeRunning);
    m_next->setEnabled(canNext && m_jogDir == 0);
    m_next->setText(m_step == kStepReview ? QStringLiteral("Apply") : QStringLiteral("Next"));
}
