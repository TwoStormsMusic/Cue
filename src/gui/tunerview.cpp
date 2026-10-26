#include "gui/tunerview.h"

#include <QFontMetricsF>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace cue::gui {

namespace tunerdraw {

QColor noteColor(const TunerReading& t) {
    if (!t.valid || t.note.isEmpty() || t.held)
        return color::dim;
    return t.inTune ? color::green : color::text;
}

QString centsText(const TunerReading& t) {
    if (!t.valid || t.note.isEmpty())
        return {};
    const long c = std::lround(t.cents);
    return QString("%1%2 ¢").arg(c > 0 ? "+" : c < 0 ? "−" : "±").arg(std::abs(c));
}

void centsBar(QPainter& p, const QRectF& r, const TunerReading& t, double u, bool scale) {
    const double radius = std::min(12 * u, r.height() / 4);
    draw::card(p, r, radius, color::panel2, color::border);
    auto xOf = [&](double cents) { return r.left() + r.width() * (std::clamp(cents, -50.0, 50.0) + 50) / 100; };
    p.save();
    // The in-tune zone, then a line every ten cents and the centre.
    const double tol = t.valid ? t.tolerance : 2;
    p.fillRect(QRectF(QPointF(xOf(-tol * 1.5), r.top() + 1), QPointF(xOf(tol * 1.5), r.bottom() - 1)),
               t.valid && t.inTune && !t.held ? color::greenDark : color::greenBg);
    p.setPen(QPen(color::border, 1));
    for (int c = -40; c <= 40; c += 10)
        if (c)
            p.drawLine(QPointF(xOf(c), r.top() + 1), QPointF(xOf(c), r.bottom() - 1));
    p.setPen(QPen(color::green, std::max(1.0, 2 * u)));
    p.drawLine(QPointF(xOf(0), r.top() + 1), QPointF(xOf(0), r.bottom() - 1));
    if (t.valid && !t.note.isEmpty()) {
        const double w = std::max(4.0, 8 * u), x = xOf(t.cents);
        p.setPen(Qt::NoPen);
        p.setBrush(t.held ? color::dim : color::text);
        p.drawRoundedRect(QRectF(x - w / 2, r.top() + r.height() * 0.1, w, r.height() * 0.8), w / 2, w / 2);
    }
    if (scale) {
        p.setFont(typeface(Face::Mono, 14 * u));
        for (int c : {-50, -25, 0, 25, 50}) {
            p.setPen(c ? color::muted : color::green);
            const QString s = c > 0 ? QString("+%1").arg(c) : QString::number(c);
            const double w = QFontMetricsF(p.font()).horizontalAdvance(s);
            const double x = std::clamp(xOf(c) - w / 2, r.left() + 6 * u, r.right() - w - 6 * u);
            p.drawText(QPointF(x, r.bottom() + 26 * u), s);
        }
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
    const int playing = s.playing();
    const ChainInfo* chain = s.chain(playing);
    const QVector<int> scene = s.sceneChains();
    QVector<Pill> pills;
    if (s.status.valid) {
        pills.append(s.status.muted ? Pill{"Outputs muted", color::red, color::red, color::red, color::redBg}
                                    : Pill{"Outputs live", color::green});
        pills.append({QString("Xruns %1").arg(s.status.xruns), s.status.xruns ? color::red : color::green});
    } else {
        pills = enginePills();
    }
    const double top = header(p, chain ? chain->name.toUpper() : "Tuner",
                              chain ? QString("chain %1 of %2").arg(scene.indexOf(playing) + 1).arg(scene.size()) : QString(), pills,
                              {{"settings", "Settings", [this] { emit navigate("settings"); }},
                               {"stage", "Back to stage", [this] { emit navigate("stage"); }}});

    // Note and cents, then the bar, then the buttons; all from the room there is.
    const double m = 40 * u, W = width(), H = height() - top;
    const double barW = std::min(W - 2 * m, 1100 * u), barH = std::clamp(H * 0.12, 40 * u, 110 * u);
    const double buttonsH = 72 * u, gap = 30 * u;
    const double noteH = std::max(80 * u, H - barH - buttonsH - 3 * gap - 60 * u - 50 * u);
    const double y0 = top + 30 * u;

    const QColor c = tunerdraw::noteColor(t);
    const QString input = t.input.isEmpty() ? s.perf.tunerInput : t.input;
    if (!t.valid || t.note.isEmpty()) {
        // No note yet: what the tuner is doing, where the note will be.
        const QString what = !t.valid ? "TUNER OFF" : t.signal ? "LISTENING" : "NO SIGNAL";
        p.setPen(color::dim);
        p.setFont(draw::fitted(Face::Display, QFont::Bold, what, std::min(noteH * 0.5, 160 * u), 20 * u, W - 2 * m));
        p.drawText(QRectF(m, y0, W - 2 * m, noteH * 0.8), Qt::AlignCenter, what);
        p.setPen(color::muted);
        p.setFont(typeface(Face::Text, std::clamp(noteH * 0.085, 13 * u, 24 * u)));
        p.drawText(QRectF(m, y0 + noteH * 0.8, W - 2 * m, noteH * 0.2), Qt::AlignHCenter | Qt::AlignTop,
                   t.valid ? "on the " + input + " input" : "waiting for the engine");
    } else {
        const QFont noteFont = typeface(Face::Display, std::min(noteH * 1.18, 330 * u), QFont::Bold);
        const QString letter = t.note.left(1), sharp = t.note.mid(1);
        const double letterW = QFontMetricsF(noteFont).horizontalAdvance(letter);
        const QFont sideFont = typeface(Face::Display, std::min(noteH * 0.36, 100 * u), QFont::DemiBold);
        const QFont detailFont = typeface(Face::Text, std::clamp(noteH * 0.085, 13 * u, 24 * u));
        const QString cents = tunerdraw::centsText(t);
        const QString detail = QString("%1 Hz%2").arg(t.hz, 0, 'f', 1).arg(t.string().isEmpty() ? QString() : " · " + t.string());
        const double sideW = std::max({QFontMetricsF(sideFont).horizontalAdvance("+00 ¢"), QFontMetricsF(sideFont).horizontalAdvance(cents),
                                       QFontMetricsF(detailFont).horizontalAdvance(detail)});
        // The letter centred, the sharp, cents and hertz beside it.
        const double x0 = W / 2 - letterW / 2;
        p.setPen(c);
        p.setFont(noteFont);
        p.drawText(QRectF(x0, y0, letterW, noteH), Qt::AlignLeft | Qt::AlignBottom, letter);
        const double sx = x0 + letterW + 30 * u;
        p.setFont(sideFont);
        if (!sharp.isEmpty())
            p.drawText(QRectF(sx, y0, sideW, noteH * 0.45), Qt::AlignLeft | Qt::AlignTop, sharp);
        p.drawText(QRectF(sx, y0, sideW + 40 * u, noteH * 0.82), Qt::AlignLeft | Qt::AlignBottom, cents);
        p.setFont(detailFont);
        p.setPen(color::muted);
        p.drawText(QRectF(sx, y0 + noteH * 0.84, W - sx - m, noteH * 0.16 + 4 * u), Qt::AlignLeft | Qt::AlignTop, detail);
    }

    const QRectF bar(W / 2 - barW / 2, y0 + noteH + gap, barW, barH);
    tunerdraw::centsBar(p, bar, t, u, true);

    // Mute (with the pedal of the control that mutes, if there is one) and back.
    const double by = bar.bottom() + 50 * u, bw = std::min(260 * u, (W - 2 * m - 16 * u) / 2);
    const ControlInfo* muter = nullptr;
    for (const ControlInfo& ci : s.perf.controls)
        if (ci.mutes)
            muter = &ci;
    const bool muted = s.status.valid && s.status.muted;
    const QRectF mute(W / 2 - bw - 8 * u, by, bw, buttonsH), back(W / 2 + 8 * u, by, bw, buttonsH);
    const QString muteText = muted ? "Unmute outputs" : "Mute outputs";
    button(p, "mute", mute, QString(), [this, muter, muted] {
               // Through the control that mutes, so its pedal and LED agree.
               const ControlStatus* cs = muter ? s.status.control(muter->name) : nullptr;
               if (muter && !(muted && cs && cs->value == 0))
                   s.run(muter->name + " = toggle");
               else
                   s.run(muted ? "mute off" : "mute on");
           },
           muted ? color::red : color::text, muted ? color::red : color::border2, muted ? color::redBg : color::panel2, 19 * u);
    {
        // The pedal's tag and the words, centred together.
        const QString tag = muter && muter->pedal ? QString("Pedal %1").arg(muter->pedal) : QString();
        const QFont f = typeface(Face::Text, 19 * u, QFont::DemiBold);
        const double th = 22 * u, tw = tag.isEmpty() ? 0 : draw::tagWidth(tag, th) + 12 * u;
        const double x = mute.center().x() - (tw + QFontMetricsF(f).horizontalAdvance(muteText)) / 2;
        if (!tag.isEmpty())
            draw::tag(p, QRectF(x, mute.center().y() - th / 2, tw - 12 * u, th), tag, color::purpleBorder, color::purple, color::purpleBg);
        p.setPen(muted ? color::red : color::text);
        p.setFont(f);
        p.drawText(QRectF(x + tw, mute.top(), mute.right() - x - tw, mute.height()), Qt::AlignLeft | Qt::AlignVCenter, muteText);
    }
    button(p, "back", back, "Back to stage", [this] { emit navigate("stage"); }, color::onOrange, color::orange, color::orange, 19 * u);

    p.setPen(color::muted);
    p.setFont(typeface(Face::Text, 14 * u));
    draw::text(p, QRectF(m, back.bottom() + 24 * u, W - 2 * m, 30 * u), Qt::AlignHCenter | Qt::AlignTop,
               QString("The tuner listens to the %1 input before any plug-in, so you can tune with the outputs live or muted.").arg(input));
}

} // namespace cue::gui
