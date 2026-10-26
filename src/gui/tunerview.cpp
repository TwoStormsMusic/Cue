#include "gui/tunerview.h"

#include <QFontMetricsF>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace cue::gui {

namespace {

// The TwoStorms Tuner's colours (TwoStorms Plugins, src/gui/theme.h and the
// tuner's own), which are a little warmer than the app's.
namespace ts {
const QColor text{0xF2, 0xED, 0xE6};
const QColor dim{0xA6, 0x9E, 0x96};
const QColor faint{0x6A, 0x64, 0x5F};
const QColor held{0x55, 0x50, 0x4B};      // the note and numbers once the note has gone
const QColor heldNeedle{0x4A, 0x46, 0x42};
const QColor heldStrobe{0x2C, 0x2A, 0x2E};
const QColor unlit{0x1F, 0x1D, 0x22};     // a chevron not lit
const QColor tickMajor{0x5A, 0x55, 0x50};
const QColor tickMinor{0x34, 0x31, 0x3A};
const QColor green{0x3D, 0xDC, 0x84};
const QColor orange{0xFF, 0x94, 0x24};
const QColor red{0xFF, 0x4F, 0x5E};
const QColor purple{0xA7, 0x8B, 0xFF};
const QColor amber{0xFF, 0xBF, 0x47};
const QColor well{0x08, 0x08, 0x0A};
const QColor border{0x28, 0x26, 0x2A};
} // namespace ts

QColor alpha(QColor c, double a) {
    c.setAlphaF(static_cast<float>(c.alphaF() * a));
    return c;
}

QLinearGradient vertical(const QRectF& r, const QColor& top, const QColor& bottom) {
    QLinearGradient g(r.topLeft(), r.bottomLeft());
    g.setColorAt(0, top);
    g.setColorAt(1, bottom);
    return g;
}

void fill(QPainter& p, const QRectF& r, const QBrush& b, double radius) {
    p.setPen(Qt::NoPen);
    p.setBrush(b);
    if (radius > 0)
        p.drawRoundedRect(r, radius, radius);
    else
        p.drawRect(r);
}

// A soft glow the shape of a rounded rectangle, as the TwoStorms canvas
// draws a shadow: full inside, fading out (smoothstep) over blur either side
// of its edge. Drawn once into a picture for each size and colour, then
// reused, since the needle's moves every frame.
void glow(QPainter& p, const QRectF& r, double radius, double blur, const QColor& c) {
    struct Picture {
        QSize size;
        double radius = 0, blur = 0;
        QRgb color = 0;
        QImage image;
    };
    static Picture kept[4];
    static int next = 0;
    const QSize size(static_cast<int>(std::ceil(r.width() + 2 * blur)), static_cast<int>(std::ceil(r.height() + 2 * blur)));
    if (size.isEmpty() || blur < 0.5)
        return;
    Picture* pic = nullptr;
    for (Picture& k : kept)
        if (k.size == size && k.radius == radius && k.blur == blur && k.color == c.rgba())
            pic = &k;
    if (!pic) {
        pic = &kept[next];
        next = (next + 1) % 4;
        *pic = {size, radius, blur, c.rgba(), QImage(size, QImage::Format_ARGB32_Premultiplied)};
        const double hw = r.width() / 2, hh = r.height() / 2, cx = blur + hw, cy = blur + hh;
        const double rr = std::min(radius, std::min(hw, hh));
        for (int y = 0; y < size.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(pic->image.scanLine(y));
            for (int x = 0; x < size.width(); ++x) {
                // The signed distance to the rounded rectangle's edge.
                const double qx = std::abs(x + 0.5 - cx) - (hw - rr), qy = std::abs(y + 0.5 - cy) - (hh - rr);
                const double sd = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - rr;
                const double t = std::clamp((sd + blur) / (2 * blur), 0.0, 1.0);
                const double a = c.alphaF() * (1 - t * t * (3 - 2 * t));
                line[x] = qPremultiply(qRgba(c.red(), c.green(), c.blue(), static_cast<int>(std::lround(a * 255))));
            }
        }
    }
    p.drawImage(QPointF(std::round(r.left() - blur), std::round(r.top() - blur)), pic->image);
}

// A sharp or a flat, drawn rather than set: top left x, y and height h.
void sharp(QPainter& p, double x, double y, double h, const QColor& c) {
    const double w = h * 0.62, sw = h * 0.085, bt = h * 0.13, sl = h * 0.10;
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    const double sx[2] = {x + w * 0.33, x + w * 0.67};
    for (int k = 0; k < 2; ++k) {
        const double y0 = y + (k == 0 ? h * 0.06 : 0), y1 = y + h - (k == 0 ? 0 : h * 0.06);
        p.drawRect(QRectF(sx[k] - sw / 2, y0, sw, y1 - y0));
    }
    for (int k = 0; k < 2; ++k) {
        const double yc = y + h * (k == 0 ? 0.36 : 0.64);
        QPainterPath bar;
        bar.moveTo(x, yc + sl / 2);
        bar.lineTo(x + w, yc - sl / 2);
        bar.lineTo(x + w, yc - sl / 2 + bt);
        bar.lineTo(x, yc + sl / 2 + bt);
        bar.closeSubpath();
        p.drawPath(bar);
    }
}

void flat(QPainter& p, double x, double y, double h, const QColor& c) {
    auto at = [&](double a, double b) { return QPointF(x + a * h, y + b * h); };
    // The stem and the bowl as one outline, the bowl's hole as a second.
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    path.moveTo(at(0, 0));
    path.lineTo(at(0.09, 0));
    path.lineTo(at(0.09, 0.5));
    path.cubicTo(at(0.25, 0.38), at(0.52, 0.42), at(0.47, 0.6));
    path.cubicTo(at(0.43, 0.76), at(0.22, 0.88), at(0.09, 1.0));
    path.lineTo(at(0, 1.0));
    path.closeSubpath();
    path.moveTo(at(0.09, 0.62));
    path.lineTo(at(0.09, 0.89));
    path.cubicTo(at(0.18, 0.82), at(0.31, 0.72), at(0.33, 0.62));
    path.cubicTo(at(0.36, 0.52), at(0.18, 0.53), at(0.09, 0.62));
    path.closeSubpath();
    p.fillPath(path, c);
}

// A chevron pointing right (dir 1) or left (-1), centred at cx, cy.
void chevron(QPainter& p, double cx, double cy, double w, double h, int dir, double lw, const QColor& c) {
    p.setPen(QPen(c, lw, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    const QPointF pts[3] = {{cx - dir * w / 2, cy - h / 2}, {cx + dir * w / 2, cy}, {cx - dir * w / 2, cy + h / 2}};
    p.drawPolyline(pts, 3);
}

// Inter Bold's cap height, as a part of its size.
double capRatio() {
    static const double r = QFontMetricsF(typeface(Face::Text, 100, QFont::Bold)).capHeight() / 100;
    return r;
}

// The note's size and place in its area: as big as the area allows the note
// with its sign, the chevrons either side and the line of numbers under it.
struct NoteGeom {
    double C = 0;  // the letter's cap height
    double fs = 0; // its font size
    double R1 = 0; // the cents' font size
    double yb = 0; // the letter's baseline
    double cx = 0;
    double ly = 0; // the numbers' baseline
};

NoteGeom noteGeom(const QRectF& na, double u) {
    NoteGeom g;
    const double capR = capRatio();
    const double C = std::max(20 * u, std::min(na.height() * 0.92 / 1.36, na.width() * 0.94 / 3.4));
    g.fs = std::max(8.0, std::round(C / capR)); // whole pixels, as the font takes them
    g.C = g.fs * capR;
    g.R1 = std::clamp(g.C * 0.24, 15 * u, 76 * u);
    const double rcap = capR * g.R1;
    const double blockH = g.C + g.C * 0.17 + rcap;
    g.yb = na.top() + (na.height() - blockH) / 2 + g.C;
    g.cx = na.center().x();
    g.ly = g.yb + g.C * 0.17 + rcap;
    return g;
}

// The width text takes with every digit a 0, so numbers that change do not
// move what is centred with them.
double steadyWidth(const QFont& f, QString text) {
    for (QChar& c : text)
        if (c.isDigit())
            c = '0';
    return QFontMetricsF(f).horizontalAdvance(text);
}

bool hasNote(const TunerReading& t) { return t.valid && !t.note.isEmpty(); }
bool inTune(const TunerReading& t) { return hasNote(t) && !t.held && t.inTune; }

} // namespace

namespace tunerdraw {

QColor noteColor(const TunerReading& t) {
    if (!hasNote(t) || t.held)
        return ts::held;
    return t.inTune ? ts::green : ts::text;
}

QString centsText(const TunerReading& t) {
    if (!hasNote(t))
        return {};
    const double c = std::abs(t.cents) < 0.05 ? 0.0 : t.cents;
    if (c > 0)
        return QString("+%1¢").arg(c, 0, 'f', 1);
    if (c < 0)
        return QString("−%1¢").arg(-c, 0, 'f', 1);
    return "0.0¢";
}

void pane(QPainter& p, const QRectF& r, bool muted, double u) {
    p.save();
    const double radius = 14 * u;
    fill(p, r, vertical(r, QColor(0x11, 0x10, 0x13), QColor(0x0B, 0x0B, 0x0D)), radius);
    const double w = muted ? 3 * u : 1;
    p.setPen(QPen(muted ? ts::red : ts::border, w));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(w / 2, w / 2, -w / 2, -w / 2), radius, radius);
    p.restore();
}

void note(QPainter& p, const QRectF& na, const TunerReading& t, double u) {
    p.save();
    const NoteGeom g = noteGeom(na, u);
    const double C = g.C;
    // What is wrong, if anything.
    QString msg;
    if (!t.valid)
        msg = "Tuner off";
    else if (!t.signal)
        msg = "No audio is coming in";
    if (!hasNote(t)) {
        const QString text = msg.isEmpty() ? "Play a note" : msg;
        p.setPen(msg.isEmpty() ? ts::faint : ts::amber);
        p.setFont(draw::fitted(Face::Text, QFont::DemiBold, text, std::clamp(C * 0.2, 14 * u, 46 * u), 9 * u, na.width()));
        p.drawText(na, Qt::AlignCenter, text);
        p.restore();
        return;
    }
    // Green in tune, white out of it (the chevrons orange), dark once the note has gone.
    const bool held = t.held, tune = inTune(t);
    const QColor nc = noteColor(t);
    const QString letter = t.note.left(1);
    const int sign = t.note.size() < 2 ? 0 : t.note[1] == '#' ? 1 : t.note[1] == 'b' ? -1 : 0;
    const QFont nf = typeface(Face::Text, g.fs, QFont::Bold), of = typeface(Face::Text, g.fs * 0.34, QFont::DemiBold);
    const QString octave = QString::number(t.octave);
    const double lw = QFontMetricsF(nf).horizontalAdvance(letter), ow = QFontMetricsF(of).horizontalAdvance(octave);
    const double accH = C * 0.48, accW = sign > 0 ? accH * 0.62 : sign < 0 ? accH * 0.5 : 0;
    const double gap = C * 0.06, gw = lw + gap + std::max(accW, ow), gx = g.cx - gw / 2;
    if (tune)
        glow(p, QRectF(gx - C * 0.08, g.yb - C, gw + C * 0.16, C), C * 0.25, C * 0.4, alpha(ts::green, 0.12));
    p.setFont(nf);
    p.setPen(nc);
    p.drawText(QPointF(gx, g.yb), letter);
    const double ax = gx + lw + gap;
    if (sign > 0)
        sharp(p, ax, g.yb - C, accH, nc);
    else if (sign < 0)
        flat(p, ax, g.yb - C, accH, nc);
    p.setFont(of);
    p.setPen(held ? ts::held : ts::dim);
    p.drawText(QPointF(ax, g.yb), octave);

    // The chevrons: lit on the side it is off to, more of them the further
    // off; both inner ones green in tune.
    const double chH = C * 0.42, chW = C * 0.2, chS = C * 0.22, cy = g.yb - C * 0.5;
    const double off = std::abs(t.cents);
    const int lit = held || off <= t.tolerance ? 0 : off > 20 ? 3 : off > 8 ? 2 : 1;
    for (int side = 0; side < 2; ++side) {
        const bool on = t.cents < 0 ? side == 0 : side == 1;
        for (int k = 0; k < 3; ++k) {
            const double x = side == 0 ? gx - C * 0.33 - chW / 2 - k * chS : gx + gw + C * 0.33 + chW / 2 + k * chS;
            QColor c = ts::unlit;
            if (tune && k == 0)
                c = ts::green;
            else if (on && k < lit)
                c = k == 2 ? ts::red : ts::orange;
            chevron(p, x, cy, chW, chH, side == 0 ? 1 : -1, C * 0.065, c);
        }
    }
    if (!msg.isEmpty() && g.yb - C - na.top() >= 20 * u) {
        // Over the note, where there is room (not in the stage's strip).
        p.setPen(ts::amber);
        p.setFont(typeface(Face::Text, 13 * u, QFont::Medium));
        p.drawText(QRectF(na.left(), na.top() + 4 * u, na.width(), 18 * u), Qt::AlignCenter, msg);
    }

    // The numbers: the cents, big and coloured, and the frequency.
    const QString cents = centsText(t), hz = QString("%1 Hz").arg(t.hz, 0, 'f', t.hz < 1000 ? 2 : 1);
    const QFont cf = typeface(Face::Text, g.R1, QFont::Bold), hf = typeface(Face::Text, g.R1 * 0.6, QFont::Medium);
    const double w1 = steadyWidth(cf, cents), w2 = steadyWidth(hf, hz), sp = g.R1 * 0.9;
    const double lx = g.cx - (w1 + sp + w2) / 2;
    p.setFont(cf);
    p.setPen(held ? ts::held : tune ? ts::green : ts::orange);
    p.drawText(QPointF(lx, g.ly), cents);
    p.setFont(hf);
    p.setPen(held ? ts::held : ts::dim);
    p.drawText(QPointF(lx + w1 + sp, g.ly), hz);
    p.restore();
}

void meter(QPainter& p, const QRectF& mr, const TunerReading& t, double u, bool labels) {
    p.save();
    const double pad = std::max(18 * u, mr.height() * 0.28);
    // The track; with the numbers over it, lower down.
    const QRectF tr = labels ? QRectF(mr.left() + pad, mr.top() + mr.height() * 0.36, mr.width() - 2 * pad, mr.height() * 0.42)
                             : QRectF(mr.left() + pad, mr.top() + mr.height() * 0.24, mr.width() - 2 * pad, mr.height() * 0.52);
    const double cx = tr.center().x(), hw = tr.width() / 2;
    fill(p, mr, vertical(mr, QColor(0x0D, 0x0C, 0x0F), QColor(0x09, 0x09, 0x0B)), 10 * u);
    draw::card(p, mr, 10 * u, QColor(), ts::border);
    fill(p, tr, ts::well, tr.height() * 0.25);
    // The tolerance, green.
    const double zw = (hasNote(t) || t.valid ? t.tolerance : 2) / 50 * hw;
    fill(p, QRectF(cx - zw, tr.top(), 2 * zw, tr.height()), alpha(ts::green, 0.18), 2 * u);
    // A tick every 5 cents, a number every 10.
    const double ls = std::clamp(mr.height() * 0.17, 10 * u, 30 * u);
    const QFont lf = typeface(Face::Text, ls, QFont::DemiBold);
    for (int k = -50; k <= 50; k += 5) {
        const double x = cx + k / 50.0 * hw;
        const bool major = k % 10 == 0;
        const double th = major ? tr.height() * 0.55 : tr.height() * 0.3;
        const double w = (k == 0 ? 3 : 1.5) * u;
        fill(p, QRectF(x - w / 2, tr.bottom() - th, w, th), k == 0 ? ts::green : major ? ts::tickMajor : ts::tickMinor, 0);
        if (labels && major) {
            const QString s = k > 0 ? QString("+%1").arg(k) : k < 0 ? QString("−%1").arg(-k) : QString("0");
            p.setPen(k == 0 ? ts::green : ts::faint);
            p.setFont(lf);
            p.drawText(QRectF(x - 40 * u, mr.top() + mr.height() * 0.06 - ls * 0.2, 80 * u, ls * 1.4), Qt::AlignCenter, s);
        }
    }
    // Flat and sharp at the ends.
    const double gh = std::min(tr.height() * 0.9, pad * 0.9), gy = tr.top() + (tr.height() - gh) / 2;
    flat(p, mr.left() + pad / 2 - gh * 0.22, gy, gh, ts::faint);
    sharp(p, mr.right() - pad / 2 - gh * 0.31, gy, gh, ts::faint);
    if (!hasNote(t)) {
        p.restore();
        return;
    }
    const bool held = t.held, tune = inTune(t);
    const double x = cx + std::clamp(t.cents, -50.0, 50.0) / 50 * hw;
    const QColor col = held ? ts::heldNeedle : tune ? ts::green : ts::orange;
    // The trail from the middle, then the needle.
    if (!held)
        fill(p, QRectF(std::min(cx, x), tr.top() + tr.height() * 0.2, std::abs(x - cx), tr.height() * 0.6), alpha(col, 0.35), 2 * u);
    const double nw = std::max(4 * u, mr.height() * 0.07);
    const QRectF nr = labels ? QRectF(x - nw / 2, mr.top() + mr.height() * 0.26, nw, mr.height() * 0.64)
                             : QRectF(x - nw / 2, mr.top() + mr.height() * 0.12, nw, mr.height() * 0.76);
    if (!held)
        glow(p, nr, nw / 2, nw * 2.2, alpha(col, 0.55));
    fill(p, nr, col, nw / 2);
    if (std::abs(t.cents) > 50 && !held) {
        // Off the scale: an arrow at the end.
        const double ex = t.cents > 0 ? tr.right() : tr.left(), d = t.cents > 0 ? 1 : -1;
        QPainterPath arrow;
        arrow.moveTo(ex + d * pad * 0.1, tr.top() - tr.height() * 0.25);
        arrow.lineTo(ex + d * pad * 0.75, tr.center().y());
        arrow.lineTo(ex + d * pad * 0.1, tr.bottom() + tr.height() * 0.25);
        arrow.closeSubpath();
        p.fillPath(arrow, ts::red);
    }
    p.restore();
}

void strobe(QPainter& p, const QRectF& sr, const TunerReading& t, double phase, double u) {
    p.save();
    fill(p, sr, ts::well, 8 * u);
    draw::card(p, sr, 8 * u, QColor(), ts::border);
    if (hasNote(t)) {
        const QColor col = t.held ? ts::heldStrobe : inTune(t) ? ts::green : ts::purple;
        const QRectF in = sr.adjusted(3 * u, 3 * u, -3 * u, -3 * u);
        p.setClipRect(in);
        // Two rows, the lower at twice the speed (the second partial, as a
        // strobe tuner's second ring).
        for (int row = 0; row < 2; ++row) {
            const double rh = in.height() / 2, ry = in.top() + row * rh, per = rh * 1.6;
            double ph = phase * (row + 1);
            ph -= std::floor(ph);
            const QColor rc = row == 0 ? col : alpha(col, 0.75);
            for (double x = in.left() - per + ph * per; x < in.right(); x += per)
                fill(p, QRectF(x, ry + (row == 0 ? 0 : 1 * u), per / 2, rh - 1 * u), rc, 0);
        }
        p.setClipping(false);
        // The middle mark.
        fill(p, QRectF(sr.center().x() - 1 * u, sr.top() - 4 * u, 2 * u, sr.height() + 8 * u), QColor(255, 255, 255, 128), 0);
    }
    p.restore();
}

void level(QPainter& p, const QRectF& bar, const TunerReading& t, double u) {
    p.save();
    fill(p, bar, ts::well, bar.height() / 2);
    const double f = std::clamp((t.levelDb + 66) / 60, 0.0, 1.0);
    if (t.valid && f > 0)
        fill(p, QRectF(bar.left(), bar.top(), bar.width() * f, bar.height()), t.signal ? alpha(ts::green, 0.8) : ts::faint,
             std::min(bar.height(), bar.width() * f) / 2);
    if (t.valid && t.signal) {
        p.setPen(ts::faint);
        p.setFont(typeface(Face::Text, 11 * u));
        p.drawText(QRectF(bar.left() - 140 * u, bar.center().y() - 8 * u, 132 * u, 16 * u), Qt::AlignRight | Qt::AlignVCenter,
                   t.levelDb > -66 ? "signal" : "quiet");
    }
    p.restore();
}

} // namespace tunerdraw

TunerView::TunerView(AppState& state, QWidget* parent) : Surface(state, parent) {
    connect(&s, &AppState::tunerChanged, this, [this] {
        if (isVisible())
            update();
    });
    connect(&s, &AppState::statusChanged, this, [this] {
        if (isVisible())
            update();
    });
    connect(&s, &AppState::changed, this, qOverload<>(&QWidget::update));
}

void TunerView::paint(QPainter& p) {
    const TunerReading& t = s.tuner;
    const bool muted = s.status.valid && s.status.muted;
    const QString input = t.input.isEmpty() ? s.perf.tunerInput : t.input;

    // As the TwoStorms Tuner's header: what it tunes to, then mute and
    // settings; and the way back to the stage.
    QString subtitle;
    if (t.valid) {
        const QString range = t.range == "guitar" ? "Guitar" : t.range == "bass" ? "Bass" : t.range == "vocals" ? "Vocals" : "Any instrument";
        subtitle = QString("A4 = %1 Hz  ·  %2  ·  ±%3¢").arg(t.reference, 0, 'g', 5).arg(range).arg(t.tolerance, 0, 'g', 3);
    }
    if (!input.isEmpty())
        subtitle += (subtitle.isEmpty() ? "" : "  ·  ") + QString("listening to %1").arg(input);
    QVector<Pill> pills;
    if (s.status.valid)
        pills.append({QString("Xruns %1").arg(s.status.xruns), s.status.xruns ? color::red : color::green});
    else
        pills = enginePills();
    const ControlInfo* muter = nullptr;
    for (const ControlInfo& ci : s.perf.controls)
        if (ci.mutes)
            muter = &ci;
    Button mute{"mute", muted ? "MUTED" : "MUTE",
                [this, muter, muted] {
                    // Through the control that mutes, so its pedal and LED agree.
                    const ControlStatus* cs = muter ? s.status.control(muter->name) : nullptr;
                    if (muter && !(muted && cs && cs->value == 0))
                        s.run(muter->name + " = toggle");
                    else
                        s.run(muted ? "mute off" : "mute on");
                }};
    if (muted) {
        mute.textColor = Qt::white;
        mute.stroke = ts::red.lighter(130);
        mute.fill = ts::red;
    }
    if (muter && muter->pedal)
        mute.tag = QString("Pedal %1").arg(muter->pedal);
    const double top = header(p, "Tuner", subtitle, pills,
                              {mute, {"settings", "Settings", [this] { emit navigate("settings"); }},
                               {"stage", "Back to stage", [this] { emit navigate("stage"); }}});

    // The pane: the note, then the needle and the strobe across the width.
    const double M = 16 * u, W = width(), H = height();
    const QRectF pane(M, top + 12 * u, W - 2 * M, H - top - 12 * u - M);
    const double CH = pane.height() - 24 * u;
    const double mh = std::clamp(CH * 0.22, 84 * u, 190 * u), sh = std::clamp(CH * 0.11, 34 * u, 90 * u);
    QRectF na(pane.left() + 12 * u, pane.top() + 12 * u, pane.width() - 24 * u, std::max(0.0, CH - mh - sh - 24 * u));
    {
        // A tall window: the note as big as the width allows, and the whole
        // stack in the middle.
        const double cw = na.width() * 0.94 / 3.4, chh = na.height() * 0.92 / 1.36;
        if (cw < chh) {
            const double need = cw * 1.36 / 0.92 + 24 * u;
            na.translate(0, (na.height() - need) / 2);
            na.setHeight(need);
        }
    }
    tunerdraw::pane(p, pane, muted, u);
    if (muted) {
        // From across the stage: a red frame and a red tag.
        const QString text = "INPUTS MUTED";
        const QFont f = typeface(Face::Text, 15 * u, QFont::Bold);
        const QRectF tag(pane.left() + 14 * u, pane.top() + 12 * u, QFontMetricsF(f).horizontalAdvance(text) + 24 * u, 28 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(ts::red);
        p.drawRoundedRect(tag, 6 * u, 6 * u);
        p.setPen(Qt::white);
        p.setFont(f);
        p.drawText(tag, Qt::AlignCenter, text);
    }
    tunerdraw::level(p, QRectF(pane.right() - 14 * u - 90 * u, pane.top() + 18 * u, 90 * u, 8 * u), t, u);
    tunerdraw::note(p, na, t, u);
    const QRectF meter(pane.left() + 12 * u, na.bottom() + 12 * u, pane.width() - 24 * u, mh);
    tunerdraw::meter(p, meter, t, u, true);
    tunerdraw::strobe(p, QRectF(meter.left(), meter.bottom() + 12 * u, meter.width(), sh), t, s.strobePhase, u);
}

} // namespace cue::gui
