#pragma once

// The tuner view: the TwoStorms Tuner's reading as big as the screen allows,
// for tuning on stage. The stage view draws the same reading as a strip.

#include "gui/surface.h"

namespace cue::gui {

namespace tunerdraw {
// Green in tune, orange out of tune, grey for a held note or none.
QColor noteColor(const TunerReading& t);
// "+2 ¢", or empty without a note.
QString centsText(const TunerReading& t);
// The bar from -50 to +50 cents: the in-tune zone, the centre, the needle.
void centsBar(QPainter& p, const QRectF& r, const TunerReading& t, double u, bool scale);
} // namespace tunerdraw

class TunerView : public Surface {
    Q_OBJECT
public:
    explicit TunerView(AppState& state, QWidget* parent = nullptr);

protected:
    void paint(QPainter& p) override;
};

} // namespace cue::gui
