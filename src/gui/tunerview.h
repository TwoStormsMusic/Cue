#pragma once

// The tuner view: the TwoStorms Tuner's reading as big as the screen allows,
// drawn as the TwoStorms Tuner draws it, for tuning on stage. The stage view
// draws the same reading as a strip.

#include "gui/surface.h"

namespace cue::gui {

namespace tunerdraw {
// Green in tune, white out of tune, dark grey for a held note or none.
QColor noteColor(const TunerReading& t);
// "+2.2¢", "−0.4¢" or "0.0¢"; empty without a note.
QString centsText(const TunerReading& t);
// The panel the reading sits on, red-framed while the inputs are muted.
void pane(QPainter& p, const QRectF& r, bool muted, double u);
// The note as big as the area allows: the letter, its sharp or flat, the
// octave, the chevrons either side (more of them the further off) and the
// cents and hertz under it (unless numbers is false). Without a note, what
// the tuner is waiting for.
void note(QPainter& p, const QRectF& area, const TunerReading& t, double u, bool numbers = true);
// The needle from -50 to +50 cents over the in-tune zone, with a tick every
// five cents; labels puts the numbers over them.
void meter(QPainter& p, const QRectF& r, const TunerReading& t, double u, bool labels);
// The strobe: two rows of stripes that drift with the error, the lower at
// twice the speed, and stand still in tune. phase is AppState::strobePhase.
void strobe(QPainter& p, const QRectF& r, const TunerReading& t, double phase, double u);
// How loud the input is, top right of the pane: a short bar.
void level(QPainter& p, const QRectF& bar, const TunerReading& t, double u);
} // namespace tunerdraw

class TunerView : public Surface {
    Q_OBJECT
public:
    explicit TunerView(AppState& state, QWidget* parent = nullptr);

protected:
    void paint(QPainter& p) override;
};

} // namespace cue::gui
