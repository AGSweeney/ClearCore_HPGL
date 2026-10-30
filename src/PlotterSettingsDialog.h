#pragma once

// Editor for the board's settings, built from the console's "config" reply
// (lines "key = value  help [disabled|restart]"). Settings are grouped by topic;
// fixed choices are drop-downs. changedCommands() returns the "set key value"
// commands for whatever the user changed.

#include <QDialog>
#include <QMap>
#include <QString>
#include <QStringList>

class QComboBox;
class QLineEdit;

class PlotterSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit PlotterSettingsDialog(const QStringList &configLines, QWidget *parent = nullptr);

    QStringList changedCommands() const;

private:
    struct Field {
        QString original;
        QLineEdit *edit = nullptr;
        QComboBox *combo = nullptr;
    };
    QMap<QString, Field> m_fields;
};
