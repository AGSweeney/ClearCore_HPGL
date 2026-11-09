/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#pragma once

// Plotter > Update board firmware: picks the ClearCore on USB, says what's on it and what will happen, and
// runs fw::Updater off the UI thread. From the Electronic CAM Workbench (ECamUI).

#include "FirmwareUpdate.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

class FirmwareDialog : public QDialog {
    Q_OBJECT
public:
    explicit FirmwareDialog(QWidget *parent = nullptr);

    bool busy() const { return m_busy; }
    bool finishedOk() const { return m_done && m_result.ok; }
    const fw::Result &result() const { return m_result; }
    void reject() override;  // not while it runs

signals:
    // Just before flashing: anything holding the board's links lets go.
    void aboutToUpdate();
    void updated();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void refreshPorts();
    void survey();
    void showSurvey();
    void startUpdate();
    void setBusy(bool on);

    fw::Package m_package;
    QList<fw::Port> m_ports;
    fw::Survey m_survey;
    fw::Result m_result;
    bool m_surveyed = false;
    bool m_busy = false;
    bool m_done = false;
    int m_surveyId = 0;

    QLabel *m_carried = nullptr;
    QComboBox *m_port = nullptr;
    QPushButton *m_refresh = nullptr;
    QLabel *m_board = nullptr;
    QLabel *m_step = nullptr;
    QProgressBar *m_progress = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPushButton *m_update = nullptr;
    QPushButton *m_close = nullptr;
};
