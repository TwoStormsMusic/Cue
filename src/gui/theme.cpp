#include "gui/theme.h"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>

#include <cmath>
#include <cstdio>

// The fonts are compiled into the library; a static library's resources
// are registered by hand, outside any namespace.
static void initFontResources() { Q_INIT_RESOURCE(fonts); }

namespace cue::gui {

void loadFonts() {
    static bool loaded = false;
    if (loaded)
        return;
    loaded = true;
    initFontResources();
    for (const char* f : {"Inter-Regular.otf", "Inter-Medium.otf", "Inter-SemiBold.otf", "Inter-Bold.otf", "BarlowCondensed-SemiBold.ttf",
                          "BarlowCondensed-Bold.ttf", "JetBrainsMono-Regular.ttf", "JetBrainsMono-Medium.ttf"})
        if (QFontDatabase::addApplicationFont(QString(":/fonts/") + f) < 0)
            std::fprintf(stderr, "cue-app: could not load the font %s\n", f);
}

QFont typeface(Face face, double px, int weight) {
    QFont f(face == Face::Display ? "Barlow Condensed" : face == Face::Mono ? "JetBrains Mono" : "Inter");
    f.setPixelSize(std::max(1, static_cast<int>(std::lround(px))));
    f.setWeight(static_cast<QFont::Weight>(weight));
    f.setHintingPreference(QFont::PreferNoHinting);
    return f;
}

namespace draw {

void card(QPainter& p, const QRectF& r, double radius, const QColor& fill, const QColor& stroke) {
    p.save();
    p.setPen(stroke.isValid() ? QPen(stroke, 1) : Qt::NoPen);
    p.setBrush(fill.isValid() ? QBrush(fill) : Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    p.restore();
}

static QFont pillFont(double h) { return typeface(Face::Mono, h * 0.42, QFont::Medium); }

double pillWidth(const QString& text, double h, bool dot) {
    const double pad = h * 0.4;
    return pad * 2 + (dot ? h * 0.38 : 0) + QFontMetricsF(pillFont(h)).horizontalAdvance(text);
}

void pill(QPainter& p, const QRectF& r, const QString& text, const QColor& dot, const QColor& textColor, const QColor& stroke,
          const QColor& fill) {
    const double h = r.height();
    card(p, r, h / 2, fill, stroke);
    double x = r.left() + h * 0.4;
    p.save();
    if (dot.isValid()) {
        p.setPen(Qt::NoPen);
        p.setBrush(dot);
        const double d = h * 0.2;
        p.drawEllipse(QPointF(x + d / 2, r.center().y()), d / 2, d / 2);
        x += h * 0.38;
    }
    p.setPen(textColor);
    p.setFont(pillFont(h));
    p.drawText(QRectF(x, r.top(), r.right() - x, h), Qt::AlignVCenter | Qt::AlignLeft, text);
    p.restore();
}

static QFont tagFont(double h) { return typeface(Face::Text, h * 0.58, QFont::Medium); }

double tagWidth(const QString& text, double h) { return h * 0.9 + QFontMetricsF(tagFont(h)).horizontalAdvance(text); }

void tag(QPainter& p, const QRectF& r, const QString& text, const QColor& stroke, const QColor& textColor, const QColor& fill, bool dashed) {
    p.save();
    QPen pen(stroke, 1);
    if (dashed)
        pen.setDashPattern({3, 2});
    p.setPen(pen);
    p.setBrush(fill.isValid() ? QBrush(fill) : Qt::NoBrush);
    const double radius = r.height() * 0.22;
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    p.setPen(textColor);
    p.setFont(tagFont(r.height()));
    p.drawText(r, Qt::AlignCenter, text);
    p.restore();
}

QFont fitted(Face face, int weight, const QString& text, double px, double minPx, double width) {
    QFont f = typeface(face, px, weight);
    const double w = QFontMetricsF(f).horizontalAdvance(text);
    if (w <= width || w <= 0)
        return f;
    return typeface(face, std::max(minPx, px * width / w), weight);
}

void text(QPainter& p, const QRectF& r, int flags, const QString& s) {
    const QFontMetricsF fm(p.font());
    p.drawText(r, flags, fm.elidedText(s, Qt::ElideRight, r.width()));
}

} // namespace draw

} // namespace cue::gui
