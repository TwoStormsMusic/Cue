#pragma once

// The stage view: the chain playing, big; the levels; the tuner strip; a
// button for each chain of the scene and each control. A tap does what the
// FCB's pedal would, so the screen and the pedals stay in step.

#include "gui/surface.h"

#include <QElapsedTimer>
#include <QHash>

namespace cue::gui {

class StageView : public Surface {
    Q_OBJECT
public:
    explicit StageView(AppState& state, QWidget* parent = nullptr);

protected:
    void paint(QPainter& p) override;

private:
    void hero(QPainter& p, const QRectF& r);
    void levels(QPainter& p, const QRectF& r);
    void tunerStrip(QPainter& p, const QRectF& r);
    void chainButtons(QPainter& p, const QRectF& r, const QVector<int>& chains, int rows);
    void controlButtons(QPainter& p, const QRectF& r, int rows);
    void onStatus();

    // Meters fall back slowly and hold their peak, as meters do.
    struct Meter {
        double shown = -120, peak = -120;
        qint64 peakAt = 0, clipAt = -10000;
    };
    QHash<QString, Meter> meters_;
    QElapsedTimer clock_;
    qint64 lastStatus_ = 0, lastDrag_ = 0;
};

} // namespace cue::gui
