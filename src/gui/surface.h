#pragma once

// A view drawn by hand, as the mockups are, that works the same by touch and
// by mouse: what can be tapped is registered while drawing, a tap acts when
// the finger lifts inside it, and a drag (a slider) follows from the first
// touch. Sizes are in units of the 1280-pixel-wide design, so the view fits
// any screen; the size setting scales on top.

#include "gui/state.h"
#include "gui/theme.h"

#include <QColor>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

namespace cue::gui {

class Surface : public QWidget {
    Q_OBJECT
public:
    explicit Surface(AppState& state, QWidget* parent = nullptr);

    // Where something tappable was last drawn (empty if it was not), for
    // the tests.
    QRectF hitRect(const QString& id) const;
    QStringList hitIds() const;

signals:
    // Go to another view: "stage", "tuner", "chains" or "settings".
    void navigate(const QString& view);

protected:
    void paintEvent(QPaintEvent*) override;
    virtual void paint(QPainter& p) = 0;

    using Drag = std::function<void(QPointF pos, bool done)>;
    void hit(const QString& id, const QRectF& r, std::function<void()> tap, Drag drag = {});
    bool isPressed(const QString& id) const { return pressed_ == id; }

    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

    // The header every view has: the logo, a title and a subtitle, then
    // pills, then buttons at the right. Pills that do not fit are left out,
    // the last first. Returns the header's height.
    struct Pill {
        QString text;
        QColor dot, textColor = color::text2, stroke = color::border, fill = color::panel;
    };
    struct Button {
        QString id, text;
        std::function<void()> tap;
        QColor textColor = color::text, stroke = color::border2, fill = color::panel;
    };
    double header(QPainter& p, const QString& title, const QString& subtitle, const QVector<Pill>& pills, const QVector<Button>& buttons);

    // Pills for the engine's numbers, as the settings say to show times.
    QVector<Pill> enginePills() const;

    // A button: a rounded card with centred text.
    void button(QPainter& p, const QString& id, const QRectF& r, const QString& text, std::function<void()> tap, const QColor& textColor,
                const QColor& stroke, const QColor& fill, double px);

    AppState& s;
    double u = 1; // pixels per unit, set before paint()

private:
    void offline(QPainter& p);

    struct Hit {
        QString id;
        QRectF r;
        std::function<void()> tap;
        Drag drag;
    };
    QVector<Hit> hits_;
    QString pressed_;
    Drag dragging_;
    bool pressedInside_ = false;
    double headerHeight_ = 0;
};

} // namespace cue::gui
