#pragma once

// Settings: the app's own (which view it starts in, how times show, its size,
// full screen) and the built-in tuner's, which belong to the performance and
// are saved with it.

#include "gui/surface.h"

namespace cue::gui {

class SettingsView : public Surface {
    Q_OBJECT
public:
    explicit SettingsView(AppState& state, QWidget* parent = nullptr);

signals:
    // The app's settings changed (and are saved).
    void settingsChanged();

protected:
    void paint(QPainter& p) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    // A row: a label, a hint, and choices; the one in force is marked.
    double row(QPainter& p, double y, const QString& label, const QString& hint, const QStringList& choices, int current,
               const std::function<void(int)>& pick);
    void changed();
    void tuner(const QJsonObject& change);

    // Below the header the page scrolls, by wheel or by dragging.
    QRectF body_;
    double scroll_ = 0, scrollMax_ = 0, dragFrom_ = 0, dragY_ = -1;
};

} // namespace cue::gui
