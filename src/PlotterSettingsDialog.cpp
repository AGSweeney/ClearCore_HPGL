#include "PlotterSettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QVBoxLayout>

namespace {

struct Choice {
    const char *value;
    const char *text;
};

struct KeyInfo {
    const char *key;
    const char *label;
    const char *unit;
    QList<Choice> choices;
};

struct Section {
    const char *title;
    QList<KeyInfo> keys;
};

const QList<Choice> kOnOff = {{"1", "On"}, {"0", "Off"}};
const QList<Choice> kDir = {{"1", "Normal (+1)"}, {"-1", "Reversed (-1)"}};

const QList<Section> &Sections() {
    static const QList<Section> sections = {
        {"Machine",
         {{"steps_rev_x", "X steps / rev", "steps", {}},
          {"mm_rev_x", "X travel / rev", "mm", {}},
          {"dir_x", "X direction", "", kDir},
          {"steps_rev_y", "Y steps / rev", "steps", {}},
          {"mm_rev_y", "Y travel / rev", "mm", {}},
          {"dir_y", "Y direction", "", kDir},
          {"width_mm", "Plot area width", "mm", {}},
          {"height_mm", "Plot area height", "mm", {}}}},
        {"Motion",
         {{"draw_mm_s", "Pen-down speed", "mm/s", {}},
          {"job_speed", "Pen speed from jobs (VS)", "", {{"1", "Use the job's speed"}, {"0", "Ignore: always the pen-down speed"}}},
          {"travel_mm_s", "Pen-up speed", "mm/s", {}},
          {"max_mm_s", "Max speed (VS limit)", "mm/s", {}},
          {"accel_mm_s2", "Acceleration", "mm/s²", {}},
          {"corner_mm_s", "Corner speed change", "mm/s", {}}}},
        {"Pen",
         {{"pen_mode", "Pen actuator", "", {{"0", "IO output (solenoid / relay)"}, {"1", "Z axis on M2"}}},
          {"pen_io", "Pen output", "",
           {{"0", "IO-0"}, {"1", "IO-1"}, {"2", "IO-2"}, {"3", "IO-3"}, {"4", "IO-4"}, {"5", "IO-5"}}},
          {"pen_active_high", "Output on means", "", {{"1", "Pen down"}, {"0", "Pen up"}}},
          {"pen_down_ms", "Settle after lowering", "ms", {}},
          {"pen_up_ms", "Settle after raising", "ms", {}},
          {"z_up_steps", "Z pen-up position", "steps", {}},
          {"z_down_steps", "Z pen-down position", "steps", {}},
          {"z_vel_steps", "Z velocity", "steps/s", {}},
          {"z_accel_steps", "Z acceleration", "steps/s²", {}}}},
        {"Safety",
         {{"estop_di6", "Estop input DI-6", "",
           {{"0", "Not used"}, {"1", "Fault when DI-6 is low"}, {"2", "Fault when DI-6 is high"}}},
          {"auto_enable", "Enable motors when a job arrives", "", kOnOff},
          {"hlfb_check", "Require HLFB on enable", "", kOnOff}}},
        {"Serial port (COM)",
         {{"com_port", "HP-GL serial port", "", {{"0", "Off"}, {"1", "COM-0"}, {"2", "COM-1"}}},
          {"com_baud", "Baud rate", "", {}},
          {"com_handshake", "Handshake", "", {{"0", "None"}, {"1", "XON/XOFF"}, {"2", "RTS/CTS"}}},
          {"com_rs232", "Signal levels", "", {{"1", "RS-232"}, {"0", "TTL"}}}}},
        {"Network",
         {{"net_mode", "Address", "", {{"0", "DHCP"}, {"1", "Static"}}},
          {"ip", "Static IP", "", {}},
          {"netmask", "Netmask", "", {}},
          {"gateway", "Gateway", "", {}}}},
        {"Identity", {{"ident", "OI reply (model)", "", {}}}},
    };
    return sections;
}

const char *const kStyle =
    "QDialog { background: #2a2f36; }"
    "QWidget#SettingsPage { background: #2a2f36; }"
    "QScrollArea { background: #2a2f36; border: none; }"
    "QLabel { color: #e6ebf3; background: transparent; }"
    "QLabel#Note { color: #aeb7c4; }"
    "QLabel#Unit { color: #aeb7c4; min-width: 52px; }"
    "QLabel#Badge { color: #f0c15d; font-size: 11px; }"
    "QGroupBox { background: #2f343a; border: 1px solid #1a1e24; border-radius: 3px; margin-top: 18px;"
    " padding: 6px 10px 8px 10px; color: #e6ebf3; font-weight: 600; }"
    "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 6px; color: #c9d1dc; }"
    "QLineEdit, QComboBox { background: #22272e; color: #e8edf3; border: 1px solid #3d4650; border-radius: 2px;"
    " min-height: 24px; padding: 2px 8px; selection-background-color: #5a82c2; }"
    "QLineEdit:focus, QComboBox:focus { border-color: #5a82c2; }"
    "QComboBox::drop-down { border: none; width: 24px; }"
    "QComboBox::down-arrow { image: url(:/icons/combo-down.svg); width: 12px; height: 8px; }"
    "QComboBox QAbstractItemView { background: #22272e; color: #e8edf3; selection-background-color: #3d5a8c;"
    " border: 1px solid #3d4650; }"
    "QPushButton { background: #3d4450; color: #f2f6fb; border: 1px solid #5a6575; border-radius: 2px;"
    " min-width: 88px; min-height: 26px; padding: 4px 14px; font-weight: 600; }"
    "QPushButton:hover { background: #4c5565; }"
    "QScrollBar:vertical { background: #2a2f36; width: 12px; }"
    "QScrollBar::handle:vertical { background: #4a5260; border-radius: 4px; min-height: 30px; }"
    "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }";

} // namespace

PlotterSettingsDialog::PlotterSettingsDialog(const QStringList &configLines, QWidget *parent) : QDialog(parent) {
    setWindowTitle("Plotter settings");
    setStyleSheet(kStyle);
    resize(640, 720);

    // Parse "key = value  help [flag]".
    static const QRegularExpression kLine(QStringLiteral("^(\\S+)\\s+=\\s+(\\S*)\\s*(.*)$"));
    struct Entry {
        QString value;
        QString help;
        QString flag;
    };
    QMap<QString, Entry> entries;
    QStringList order;
    for (const QString &l : configLines) {
        const QRegularExpressionMatch m = kLine.match(l);
        if (!m.hasMatch()) {
            continue;
        }
        Entry e;
        e.value = m.captured(2);
        e.help = m.captured(3).trimmed();
        if (e.help.endsWith(QLatin1String("[disabled]"))) {
            e.flag = QStringLiteral("disabled");
        } else if (e.help.endsWith(QLatin1String("[restart]"))) {
            e.flag = QStringLiteral("restart");
        }
        e.help.remove(QRegularExpression(QStringLiteral("\\s*\\[(disabled|restart)\\]$")));
        entries.insert(m.captured(1), e);
        order << m.captured(1);
    }

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 14, 14, 12);
    auto *note = new QLabel(
        "Changes are saved to the plotter when you press OK. Settings marked “motors disabled” need the "
        "motors disabled first; “after restart” settings take effect when the plotter restarts.",
        this);
    note->setObjectName("Note");
    note->setWordWrap(true);
    outer->addWidget(note);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *page = new QWidget(scroll);
    page->setObjectName("SettingsPage");
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 6, 0);
    pageLayout->setSpacing(4);

    auto addRow = [&](QFormLayout *form, const QString &key, const QString &label, const QString &unit,
                      const QList<Choice> &choices) {
        const Entry e = entries.value(key);
        Field f;
        f.original = e.value;
        auto *row = new QWidget(page);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        bool known = choices.isEmpty();
        if (!choices.isEmpty()) {
            f.combo = new QComboBox(row);
            for (const Choice &c : choices) {
                f.combo->addItem(QString::fromUtf8(c.text), QString::fromLatin1(c.value));
                if (QString::fromLatin1(c.value) == e.value) {
                    f.combo->setCurrentIndex(f.combo->count() - 1);
                    known = true;
                }
            }
            if (!known) {  /* value outside the list: show it rather than hide it */
                f.combo->addItem(e.value, e.value);
                f.combo->setCurrentIndex(f.combo->count() - 1);
            }
            f.combo->setToolTip(e.help);
            rowLayout->addWidget(f.combo, 1);
        } else {
            f.edit = new QLineEdit(e.value, row);
            f.edit->setToolTip(e.help);
            rowLayout->addWidget(f.edit, 1);
            auto *u = new QLabel(unit, row);
            u->setObjectName("Unit");
            rowLayout->addWidget(u);
        }
        if (!e.flag.isEmpty()) {
            auto *badge = new QLabel(e.flag == QLatin1String("disabled") ? "motors disabled" : "after restart", row);
            badge->setObjectName("Badge");
            badge->setMinimumWidth(96);
            rowLayout->addWidget(badge);
        } else {
            rowLayout->addSpacing(104);
        }
        auto *lbl = new QLabel(label, page);
        lbl->setToolTip(e.help);
        lbl->setMinimumWidth(200);  /* same label column in every section */
        lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->addRow(lbl, row);
        m_fields.insert(key, f);
    };

    QSet<QString> placed;
    for (const Section &s : Sections()) {
        auto *box = new QGroupBox(QString::fromUtf8(s.title), page);
        auto *form = new QFormLayout(box);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setHorizontalSpacing(12);
        form->setVerticalSpacing(6);
        int rows = 0;
        for (const KeyInfo &k : s.keys) {
            const QString key = QString::fromLatin1(k.key);
            if (!entries.contains(key)) {
                continue;
            }
            addRow(form, key, QString::fromUtf8(k.label), QString::fromUtf8(k.unit), k.choices);
            placed.insert(key);
            rows++;
        }
        if (rows > 0) {
            pageLayout->addWidget(box);
        } else {
            delete box;
        }
    }
    // Keys this app does not know yet (newer firmware) still get a plain row.
    QFormLayout *otherForm = nullptr;
    for (const QString &key : order) {
        if (placed.contains(key)) {
            continue;
        }
        if (!otherForm) {
            auto *box = new QGroupBox("Other", page);
            otherForm = new QFormLayout(box);
            pageLayout->addWidget(box);
        }
        addRow(otherForm, key, key, QString(), {});
    }
    pageLayout->addStretch(1);
    scroll->setWidget(page);
    outer->addWidget(scroll, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QStringList PlotterSettingsDialog::changedCommands() const {
    QStringList cmds;
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it) {
        const Field &f = it.value();
        const QString v = f.combo ? f.combo->currentData().toString() : f.edit->text().trimmed();
        if (!v.isEmpty() && v != f.original) {
            cmds << QStringLiteral("set %1 %2").arg(it.key(), v);
        }
    }
    return cmds;
}
