#pragma once

// The app's window: one view at a time (stage, tuner, chains, settings), a
// message line at the bottom, and the keys that work everywhere.

#include "gui/state.h"

#include <QHash>
#include <QString>
#include <QTimer>
#include <QWidget>

class QStackedLayout;

namespace cue::gui {

class Surface;

class MainWindow : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(AppState& state, QWidget* parent = nullptr);

    void showView(const QString& view);
    QString current() const { return current_; }
    Surface* view(const QString& name) const { return views_.value(name); }
    void applySettings();

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void onStatus();
    void onMessage(const QString& text, bool error);

    AppState& s_;
    QStackedLayout* stack_ = nullptr;
    QHash<QString, Surface*> views_;
    QString current_, beforeTuner_ = "stage";
    bool autoTuner_ = false, wasMuted_ = false;
    QWidget* toast_ = nullptr;
    QString toastText_;
    bool toastError_ = false;
    QTimer toastTimer_;
};

} // namespace cue::gui
