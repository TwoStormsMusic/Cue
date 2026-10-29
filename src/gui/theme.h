#pragma once

// The look of the mockups: black, orange for what is selected, green for
// what plays and what is in tune, purple for the controller. Inter for text,
// Barlow Condensed for chain names and notes, JetBrains Mono for numbers;
// all three ship inside the app (third_party/fonts, SIL Open Font License).

#include <QColor>
#include <QFont>
#include <QRectF>
#include <QString>

class QPainter;

namespace cue::gui {

namespace color {
inline const QColor bg{0x0A, 0x0A, 0x0B};
inline const QColor panel{0x11, 0x11, 0x13};
inline const QColor panel2{0x14, 0x14, 0x16};
inline const QColor raised{0x1C, 0x1C, 0x1F};
inline const QColor line{0x22, 0x22, 0x26};
inline const QColor border{0x2A, 0x2A, 0x2E};
inline const QColor border2{0x3A, 0x3A, 0x40};
inline const QColor text{0xF4, 0xF4, 0xF5};
inline const QColor text2{0xD4, 0xD4, 0xD8};
inline const QColor muted{0xA1, 0xA1, 0xAA};
inline const QColor dim{0x71, 0x71, 0x7A};
inline const QColor orange{0xFF, 0x8A, 0x1F};
inline const QColor orangeLight{0xFF, 0xB0, 0x66};
inline const QColor onOrange{0x1A, 0x0E, 0x02};
inline const QColor green{0x4A, 0xDE, 0x80};
inline const QColor greenDark{0x2F, 0x6B, 0x45};
inline const QColor greenBg{0x0F, 0x1A, 0x13};
inline const QColor purple{0xC4, 0xB5, 0xFD};
inline const QColor purpleBorder{0x5B, 0x4A, 0x92};
inline const QColor purpleBg{0x15, 0x12, 0x1F};
inline const QColor red{0xF8, 0x71, 0x71};
inline const QColor redBg{0x2A, 0x10, 0x10};
} // namespace color

enum class Face { Text, Display, Mono };

// Registers the fonts that ship inside the app. Once, before any drawing.
void loadFonts();
// A font at a pixel size (fractional sizes round), weight as QFont::Weight.
QFont typeface(Face face, double px, int weight = QFont::Normal);

// Drawing helpers shared by the views. Sizes are in pixels.
namespace draw {
// A rounded card with a 1 px border.
void card(QPainter& p, const QRectF& r, double radius, const QColor& fill, const QColor& stroke);
// A pill: optional dot, then the text. Returns its width for a given height.
double pillWidth(const QString& text, double h, bool dot);
void pill(QPainter& p, const QRectF& r, const QString& text, const QColor& dot, const QColor& textColor, const QColor& stroke,
          const QColor& fill);
// The purple "Pedal 6" tag, or the dashed orange "warm" one.
double tagWidth(const QString& text, double h);
void tag(QPainter& p, const QRectF& r, const QString& text, const QColor& stroke, const QColor& textColor, const QColor& fill,
         bool dashed = false);
// The largest font, from px down to minPx, at which text fits in width.
QFont fitted(Face face, int weight, const QString& text, double px, double minPx, double width);
// Text, elided at the right if it does not fit.
void text(QPainter& p, const QRectF& r, int flags, const QString& text);
} // namespace draw

} // namespace cue::gui
