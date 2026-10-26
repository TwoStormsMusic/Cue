#include "gui/stageview.h"

#include "gui/tunerview.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace cue::gui {

namespace {

// The levels' scale: -60 dB at the bottom, 0 dB (full scale) at the top.
double meterFraction(double db) { return std::clamp((db + 60) / 60, 0.0, 1.0); }

// How wide a control's button is, against a plain switch.
double controlWeight(const ControlInfo& c) {
    if (c.type == "continuous")
        return 2.0;
    if (c.values.size() > 2)
        return 0.5 + 0.55 * c.values.size();
    return 1.0;
}

} // namespace

StageView::StageView(AppState& state, QWidget* parent) : Surface(state, parent) {
    clock_.start();
    connect(&s, &AppState::statusChanged, this, &StageView::onStatus);
    connect(&s, &AppState::tunerChanged, this, [this] {
        if (isVisible())
            update();
    });
    connect(&s, &AppState::changed, this, qOverload<>(&QWidget::update));
}

void StageView::onStatus() {
    const qint64 now = clock_.elapsed();
    const double dt = std::min(0.5, (now - lastStatus_) / 1000.0);
    lastStatus_ = now;
    auto follow = [&](const QString& key, double db) {
        Meter& m = meters_[key];
        m.shown = std::max(db, m.shown - 24 * dt);
        if (db >= m.peak || now - m.peakAt > 1500) {
            m.peak = db;
            m.peakAt = now;
        }
        if (db > -0.3)
            m.clipAt = now;
    };
    for (const Level& l : s.status.inputs)
        follow("i:" + l.name, l.peakDb);
    for (const Level& l : s.status.outputs)
        follow("o:" + l.name, l.peakDb);
    if (isVisible())
        update();
}

void StageView::paint(QPainter& p) {
    const Performance& perf = s.perf;
    QString subtitle;
    if (s.status.valid && s.status.scene < perf.scenes.size())
        subtitle = QString("Scene %1 of %2 · %3").arg(s.status.scene + 1).arg(perf.scenes.size()).arg(perf.scenes[s.status.scene].name);
    const double top = header(p, perf.valid ? perf.name : "Cue", subtitle, enginePills(),
                              {{"tuner", "Tuner", [this] { emit navigate("tuner"); }, color::green, color::greenDark, color::panel},
                               {"chains", "Chains", [this] { emit navigate("chains"); }},
                               {"settings", "Settings", [this] { emit navigate("settings"); }}});

    const double m = 20 * u, gap = 18 * u, W = width();
    const QVector<int> chains = s.sceneChains();
    const int chainRows = chains.size() <= 6 ? 1 : chains.size() <= 12 ? 2 : 3;
    const double chainsH = chains.isEmpty() ? 0 : chainRows == 1 ? 128 * u : chainRows * 90 * u + (chainRows - 1) * 12 * u;
    double weight = 0;
    for (const ControlInfo& c : perf.controls)
        weight += controlWeight(c);
    const int controlRows = perf.controls.isEmpty() ? 0 : weight > 7.5 ? 2 : 1;
    const double controlsH = controlRows == 0 ? 0 : controlRows == 1 ? 122 * u : 2 * 96 * u + 12 * u;
    const double stripH = 90 * u;
    double y = top + m;
    const double heroH = std::max(120 * u, height() - y - m - stripH - chainsH - controlsH - gap * (1 + (chainsH > 0) + (controlsH > 0)));

    const int meters = s.status.inputs.size() + s.status.outputs.size();
    const double levelsW = std::clamp(80 * u + 78 * u * std::max(meters, 2), 220 * u, W * 0.36);
    hero(p, QRectF(m, y, W - 2 * m - levelsW - gap, heroH));
    levels(p, QRectF(W - m - levelsW, y, levelsW, heroH));
    y += heroH + gap;
    tunerStrip(p, QRectF(m, y, W - 2 * m, stripH));
    y += stripH + gap;
    if (chainsH > 0) {
        chainButtons(p, QRectF(m, y, W - 2 * m, chainsH), chains, chainRows);
        y += chainsH + gap;
    }
    if (controlsH > 0)
        controlButtons(p, QRectF(m, y, W - 2 * m, controlsH), controlRows);
}

void StageView::hero(QPainter& p, const QRectF& r) {
    draw::card(p, r, 16 * u, color::panel, color::line);
    const double pad = 32 * u;
    const int playing = s.playing();
    const ChainInfo* chain = s.chain(playing);
    const QVector<int> scene = s.sceneChains();

    p.setPen(color::text2);
    QFont label = typeface(Face::Text, 15 * u, QFont::DemiBold);
    label.setLetterSpacing(QFont::AbsoluteSpacing, 2 * u);
    p.setFont(label);
    const QString position = chain ? QString("CHAIN %1 OF %2").arg(scene.indexOf(playing) + 1).arg(scene.size()) : QString("NO CHAIN YET");
    p.drawText(QRectF(r.left() + pad, r.top() + pad - 4 * u, r.width() - 2 * pad, 24 * u), Qt::AlignLeft | Qt::AlignVCenter, position);

    if (s.status.valid && s.status.muted) {
        const double h = 34 * u, w = draw::pillWidth("INPUTS MUTED", h, true);
        draw::pill(p, QRectF(r.right() - pad - w, r.top() + pad - 9 * u, w, h), "INPUTS MUTED", color::red, color::red, color::red, color::redBg);
    }

    // The FCB's display, beside the name.
    const QString display = s.status.valid ? s.status.display : QString();
    const bool showDisplay = !display.isEmpty();
    const double boxW = 92 * u, boxH = 68 * u;
    const double nameTop = r.top() + pad + 30 * u, nameBottom = r.bottom() - pad - 34 * u;
    const double nameW = r.width() - 2 * pad - (showDisplay ? boxW + 40 * u : 0);
    const QString name = chain ? chain->name.toUpper() : QString("–");
    const double nameH = std::max(20 * u, nameBottom - nameTop);
    const QFont nameFont = draw::fitted(Face::Display, QFont::Bold, name, std::min(nameH * 1.12, 190 * u), 24 * u, nameW);
    p.setFont(nameFont);
    p.setPen(color::orange);
    draw::text(p, QRectF(r.left() + pad, nameTop, nameW, nameH), Qt::AlignLeft | Qt::AlignVCenter, name);
    if (showDisplay) {
        const double nameEnd = r.left() + pad + std::min(nameW, QFontMetricsF(nameFont).horizontalAdvance(name));
        const QRectF box(nameEnd + 36 * u, nameTop + nameH / 2 - boxH / 2 + 8 * u, boxW, boxH);
        QFont small = typeface(Face::Text, 12 * u, QFont::DemiBold);
        small.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * u);
        p.setFont(small);
        p.setPen(color::muted);
        p.drawText(QRectF(box.left() - 20 * u, box.top() - 26 * u, box.width() + 40 * u, 20 * u), Qt::AlignLeft | Qt::AlignBottom, "FCB DISPLAY");
        draw::card(p, box, 8 * u, color::purpleBg, color::purpleBorder);
        p.setPen(color::purple);
        p.setFont(draw::fitted(Face::Text, QFont::Medium, display, 40 * u, 16 * u, boxW - 12 * u));
        p.drawText(box, Qt::AlignCenter, display);
    }

    // Previous and next, as the FCB's up and down pedals step.
    QStringList around;
    if (const ChainInfo* prev = s.chain(playing - 1))
        around << "Previous: " + prev->name;
    if (const ChainInfo* next = s.chain(playing + 1))
        around << "Next: " + next->name;
    p.setPen(color::muted);
    p.setFont(typeface(Face::Text, 16 * u));
    draw::text(p, QRectF(r.left() + pad, r.bottom() - pad - 24 * u, r.width() - 2 * pad, 28 * u), Qt::AlignLeft | Qt::AlignVCenter,
               around.join("      "));
}

void StageView::levels(QPainter& p, const QRectF& r) {
    draw::card(p, r, 16 * u, color::panel, color::line);
    const double pad = 20 * u;
    QFont label = typeface(Face::Text, 13 * u, QFont::DemiBold);
    label.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * u);
    p.setFont(label);
    p.setPen(color::muted);
    p.drawText(QRectF(r.left() + pad, r.top() + pad - 4 * u, r.width(), 20 * u), Qt::AlignLeft | Qt::AlignVCenter, "LEVELS");

    struct Column {
        QString key, name;
        bool output;
    };
    QVector<Column> cols;
    for (const Level& l : s.status.inputs)
        cols.append({"i:" + l.name, l.name, false});
    for (const Level& l : s.status.outputs)
        cols.append({"o:" + l.name, l.name, true});
    if (cols.isEmpty())
        return;
    const double top = r.top() + 46 * u, bottom = r.bottom() - 40 * u;
    const double cw = (r.width() - 2 * pad) / cols.size();
    const double bw = std::min(26 * u, cw * 0.45);
    const qint64 now = clock_.elapsed();
    for (int i = 0; i < cols.size(); ++i) {
        const Column& c = cols[i];
        const Meter m = meters_.value(c.key);
        const double cx = r.left() + pad + cw * (i + 0.5);
        const QRectF track(cx - bw / 2, top, bw, bottom - top);
        // Inputs and outputs apart.
        if (i > 0 && c.output && !cols[i - 1].output) {
            p.setPen(QPen(color::line, 1));
            p.drawLine(QPointF(cx - cw / 2, top), QPointF(cx - cw / 2, bottom + 26 * u));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(color::line);
        p.drawRoundedRect(track, 5 * u, 5 * u);
        const double f = meterFraction(m.shown), f6 = meterFraction(-6);
        if (f > 0) {
            p.save();
            QPainterPath clip;
            clip.addRoundedRect(track, 5 * u, 5 * u);
            p.setClipPath(clip);
            const double yTop = track.bottom() - track.height() * f, y6 = track.bottom() - track.height() * f6;
            p.fillRect(QRectF(QPointF(track.left(), std::max(yTop, y6)), track.bottomRight()), color::green);
            if (yTop < y6)
                p.fillRect(QRectF(QPointF(track.left(), yTop), QPointF(track.right(), y6)), color::orange);
            p.restore();
        }
        if (m.peak > -60) {
            const double py = track.bottom() - track.height() * meterFraction(m.peak);
            p.fillRect(QRectF(track.left(), py - 1 * u, bw, 2 * u), color::text2);
        }
        if (now - m.clipAt < 2000)
            p.fillRect(QRectF(track.left(), track.top(), bw, 6 * u), color::red);
        p.setPen(c.output ? color::text : color::text2);
        p.setFont(typeface(Face::Text, 14 * u, QFont::Medium));
        draw::text(p, QRectF(cx - cw / 2, bottom + 6 * u, cw, 24 * u), Qt::AlignHCenter | Qt::AlignVCenter, c.name);
    }
}

void StageView::tunerStrip(QPainter& p, const QRectF& r) {
    // The TwoStorms Tuner, small: the note with its chevrons at the left and
    // the needle across the rest, nothing else, so it reads at a glance;
    // red-framed while the inputs are muted.
    const TunerReading& t = s.tuner;
    tunerdraw::pane(p, r, s.status.valid && s.status.muted, u);
    if (isPressed("tuner strip"))
        draw::card(p, r, 14 * u, QColor(255, 255, 255, 12), QColor());
    hit("tuner strip", r, [this] { emit navigate("tuner"); });
    const double pad = 10 * u, h = r.height() - 2 * pad;
    const double noteW = std::min(r.width() * 0.36, h * 3.4 + 8 * u);
    tunerdraw::note(p, QRectF(r.left() + pad, r.top() + pad, noteW, h), t, u, false);
    const double x = r.left() + pad + noteW + 8 * u, w = r.right() - pad - x;
    tunerdraw::meter(p, QRectF(x, r.top() + pad, w, h), t, u, false);
}

void StageView::chainButtons(QPainter& p, const QRectF& r, const QVector<int>& chains, int rows) {
    const int perRow = (chains.size() + rows - 1) / rows;
    const double gap = 14 * u, rowGap = 12 * u;
    const double w = (r.width() - gap * (perRow - 1)) / perRow, h = (r.height() - rowGap * (rows - 1)) / rows;
    const int playing = s.playing(), warm = s.status.valid ? s.status.warmChain() : -1;
    for (int i = 0; i < chains.size(); ++i) {
        const ChainInfo* c = s.chain(chains[i]);
        if (!c)
            continue;
        const QRectF b(r.left() + (i % perRow) * (w + gap), r.top() + (i / perRow) * (h + rowGap), w, h);
        const QString id = QString("chain %1").arg(c->index);
        const bool on = c->index == playing, down = isPressed(id);
        draw::card(p, b, 12 * u, on ? (down ? color::orangeLight : color::orange) : (down ? color::raised : color::panel2),
                   on ? color::orange : color::border);
        const double th = 22 * u, pad = 16 * u;
        if (c->pedal) {
            const QString tag = QString("Pedal %1").arg(c->pedal);
            draw::tag(p, QRectF(b.left() + pad, b.top() + 14 * u, draw::tagWidth(tag, th), th), tag, on ? color::onOrange : color::purpleBorder,
                      on ? color::onOrange : color::purple, on ? QColor() : color::purpleBg);
        }
        if (c->index == warm) {
            const double tw = draw::tagWidth("warm", th);
            draw::tag(p, QRectF(b.right() - pad - tw, b.top() + 14 * u, tw, th), "warm", on ? color::onOrange : color::orange,
                      on ? color::onOrange : color::orange, QColor(), true);
        }
        const QString name = c->name.toUpper();
        p.setPen(on ? color::onOrange : color::text);
        p.setFont(draw::fitted(Face::Display, QFont::Bold, name, std::min(36 * u, h * 0.34), 14 * u, b.width() - 2 * pad));
        draw::text(p, QRectF(b.left() + pad, b.top() + th + 18 * u, b.width() - 2 * pad, b.height() - th - 30 * u),
                   Qt::AlignLeft | Qt::AlignBottom, name);
        hit(id, b, [this, index = c->index] { s.select(index); });
    }
}

void StageView::controlButtons(QPainter& p, const QRectF& r, int rows) {
    const QVector<ControlInfo>& controls = s.perf.controls;
    double total = 0;
    for (const ControlInfo& c : controls)
        total += controlWeight(c);
    // Rows of about equal weight.
    QVector<QVector<int>> lines(rows);
    double acc = 0;
    for (int i = 0; i < controls.size(); ++i) {
        const int row = std::min(rows - 1, static_cast<int>(acc / (total / rows) + 0.001));
        lines[row] << i;
        acc += controlWeight(controls[i]);
    }
    const double gap = 14 * u, rowGap = 12 * u, h = (r.height() - rowGap * (rows - 1)) / rows;
    for (int row = 0; row < rows; ++row) {
        double rowWeight = 0;
        for (int i : lines[row])
            rowWeight += controlWeight(controls[i]);
        const double unit = (r.width() - gap * (lines[row].size() - 1)) / std::max(rowWeight, 1.0);
        double x = r.left();
        for (int i : lines[row]) {
            const ControlInfo& c = controls[i];
            const ControlStatus* cs = s.status.valid ? s.status.control(c.name) : nullptr;
            const double value = cs ? cs->value : 0;
            const QRectF b(x, r.top() + row * (h + rowGap), unit * controlWeight(c), h);
            x += b.width() + gap;
            const QString id = "control " + c.name;
            const double pad = 14 * u, th = 22 * u;
            const bool toggle = c.type != "continuous" && c.values.size() <= 2;
            const bool on = value != 0;
            const bool muting = c.mutes && on;
            const bool down = isPressed(id);
            QColor fill = color::panel2, stroke = color::border;
            if (toggle && on) {
                fill = muting ? color::redBg : color::greenBg;
                stroke = muting ? color::red : color::greenDark;
            }
            if (down)
                fill = fill.lighter(150);
            draw::card(p, b, 12 * u, fill, stroke);
            if (c.pedal) {
                const QString tag = QString("Pedal %1").arg(c.pedal);
                draw::tag(p, QRectF(b.left() + pad, b.top() + 12 * u, draw::tagWidth(tag, th), th), tag, color::purpleBorder, color::purple,
                          color::purpleBg);
            }
            const QRectF topRight(b.left() + pad + 90 * u, b.top() + 12 * u, b.width() - 2 * pad - 90 * u, th);
            if (toggle) {
                QString state = cs ? cs->text.toUpper() : QString();
                if (c.mutes)
                    state = on ? "MUTED" : "mutes";
                p.setPen(muting ? color::red : on ? color::green : color::muted);
                p.setFont(typeface(Face::Text, (c.mutes && !on ? 13 : 14) * u, c.mutes && !on ? QFont::Normal : QFont::DemiBold));
                draw::text(p, topRight, Qt::AlignRight | Qt::AlignVCenter, state);
                p.setPen(on ? color::text : color::muted);
                p.setFont(draw::fitted(Face::Text, QFont::Medium, c.name, 20 * u, 12 * u, b.width() - 2 * pad));
                draw::text(p, QRectF(b.left() + pad, b.top(), b.width() - 2 * pad, b.height() - 14 * u), Qt::AlignLeft | Qt::AlignBottom, c.name);
                hit(id, b, [this, name = c.name] { s.run(name + " = toggle"); });
                continue;
            }
            // A name at the top right, then the values or a slider.
            p.setPen(on ? color::text : color::text2);
            p.setFont(typeface(Face::Text, 19 * u, QFont::DemiBold));
            draw::text(p, topRight.adjusted(0, -2 * u, 0, 2 * u), Qt::AlignRight | Qt::AlignVCenter, c.name);
            const QRectF lower(b.left() + pad, b.top() + th + 22 * u, b.width() - 2 * pad, b.height() - th - 22 * u - pad);
            draw::card(p, lower, 10 * u, color::bg, QColor());
            if (c.type == "continuous") {
                const double f = c.max > c.min ? std::clamp((value - c.min) / (c.max - c.min), 0.0, 1.0) : 0;
                p.save();
                p.setPen(Qt::NoPen);
                p.setBrush(color::greenDark);
                p.drawRoundedRect(QRectF(lower.left(), lower.top(), std::max(lower.height() * 0.3, lower.width() * f), lower.height()), 10 * u, 10 * u);
                p.restore();
                p.setPen(color::text);
                p.setFont(typeface(Face::Mono, 16 * u, QFont::Medium));
                p.drawText(lower.adjusted(12 * u, 0, -12 * u, 0), Qt::AlignRight | Qt::AlignVCenter, cs ? cs->text : QString());
                // Dragging sets it, about 20 times a second and once more where the finger lifts.
                hit(id, lower, {}, [this, c, lower](QPointF pos, bool done) {
                    const double f = std::clamp((pos.x() - lower.left()) / lower.width(), 0.0, 1.0);
                    const qint64 t = clock_.elapsed();
                    if (!done && t - lastDrag_ < 50)
                        return;
                    lastDrag_ = t;
                    s.run(QString("%1 = %2").arg(c.name).arg(c.min + f * (c.max - c.min), 0, 'g', 4));
                });
                continue;
            }
            const int n = c.values.size();
            const double sw = (lower.width() - 8 * u) / std::max(n, 1);
            for (int k = 0; k < n; ++k) {
                const QRectF seg(lower.left() + 4 * u + k * sw, lower.top() + 4 * u, sw, lower.height() - 8 * u);
                const QString sid = QString("control %1 = %2").arg(c.name, c.values[k]);
                const bool here = std::lround(value) == k;
                if (here || isPressed(sid))
                    draw::card(p, seg, 8 * u, isPressed(sid) ? color::border2 : color::border, QColor());
                p.setPen(here ? (k == 0 ? color::text : color::green) : color::muted);
                p.setFont(typeface(Face::Text, 16 * u, here ? QFont::DemiBold : QFont::Medium));
                draw::text(p, seg, Qt::AlignCenter, c.values[k]);
                hit(sid, seg, [this, name = c.name, v = c.values[k]] { s.run(name + " = " + v); });
            }
        }
    }
}

} // namespace cue::gui
