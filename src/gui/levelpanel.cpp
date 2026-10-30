// The Gain Stage in Cue: the panel of the input box. Its tabs show this
// chain, every input and every output, each as a card that sets its level
// for the whole performance, whichever chain plays; every chain's input box
// shows the same cards. An output box's panel is that output's card. A card
// has the name (a tap renames it), the level (a tap types one), a meter, a
// fader, Auto as the Gain Stage plug-in has it and, on an output, mute and
// a limiter. Every change goes to the engine and is kept for undo.

#include "gui/board.h"
#include "gui/chainview.h"
#include "gui/levels.h"

#include <QJsonArray>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace cue::gui {

namespace {

double meterFraction(double db) { return std::clamp((db + 60) / 60, 0.0, 1.0); }

QFont smallLabel(double u) {
    QFont f = typeface(Face::Text, 12 * u, QFont::DemiBold);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * u);
    return f;
}

QString measureText(const QString& m) { return m == "rms" ? "RMS" : m == "lufs" ? "LUFS" : "Peak"; }

QString secondsText(double s) { return QString("%1 s").arg(s, 0, 'g', 3); }

const double kTimes[] = {1, 2, 3, 5, 10, 20, 30};

} // namespace

void ChainView::followMeters() {
    const qint64 now = clock_.elapsed();
    const double dt = std::min(0.5, (now - lastMeters_) / 1000.0);
    lastMeters_ = now;
    auto follow = [&](const QString& key, double db, double limit) {
        IoMeter& m = ioMeters_[key];
        m.shown = std::max(db, m.shown - 24 * dt);
        if (db >= m.peak || now - m.peakAt > 1500) {
            m.peak = db;
            m.peakAt = now;
        }
        if (db > -0.3)
            m.clipAt = now;
        m.limit = std::min(limit, m.limit + 24 * dt);
    };
    for (int i = 0; i < s.status.inputs.size(); ++i)
        follow(AppState::autoKey(true, i), s.status.inputs[i].peakDb, 0);
    for (int o = 0; o < s.status.outputs.size(); ++o)
        follow(AppState::autoKey(false, o), s.status.outputs[o].peakDb, s.status.outputs[o].limitDb);
    // The wires of the chain shown, after their gains (none while it is idle).
    if (const ChainInfo* c = s.chain(chain_)) {
        const QVector<double> peaks = chain_ < s.status.chains.size() ? s.status.chains[chain_].wirePeaks : QVector<double>();
        for (int i = 0; i < c->wires.size(); ++i)
            follow(QString("w %1 %2").arg(chain_).arg(i), peaks.value(i, -120), 0);
    }
}

// Green to -6 dB, orange above, a peak line held for a moment, red at the
// end after a clip. An output's limiter shows as an orange bar under it,
// from the right, as far as it turns the output down.
void ChainView::meterBar(QPainter& p, const QRectF& r, const QString& key, bool output) {
    if (!ioMeters_.contains(key))
        followMeters();
    const IoMeter m = ioMeters_.value(key);
    const qint64 now = clock_.elapsed();
    p.setPen(Qt::NoPen);
    p.setBrush(color::line);
    p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
    const double f = meterFraction(m.shown), f6 = meterFraction(-6);
    if (f > 0) {
        p.save();
        QPainterPath clip;
        clip.addRoundedRect(r, r.height() / 2, r.height() / 2);
        p.setClipPath(clip);
        const double x = r.left() + r.width() * f, x6 = r.left() + r.width() * f6;
        p.fillRect(QRectF(r.topLeft(), QPointF(std::min(x, x6), r.bottom())), color::green);
        if (x > x6)
            p.fillRect(QRectF(QPointF(x6, r.top()), QPointF(x, r.bottom())), color::orange);
        p.restore();
    }
    if (m.peak > -60) {
        const double px = r.left() + r.width() * meterFraction(m.peak);
        p.fillRect(QRectF(px - 1 * u, r.top(), 2 * u, r.height()), color::text2);
    }
    if (now - m.clipAt < 2000)
        p.fillRect(QRectF(r.right() - 5 * u, r.top(), 5 * u, r.height()), color::red);
    if (output && m.limit < -0.1) {
        const double w = r.width() * std::min(1.0, -m.limit / 12.0);
        p.fillRect(QRectF(r.right() - w, r.bottom() + 2 * u, w, 3 * u), color::orange);
    }
}

void ChainView::ioPanel(QPainter& p, const QRectF& r, const ChainInfo& c) {
    const PortInfo port = *ioSel_;
    const bool in = port.kind == PortInfo::Kind::Input;
    const int own = in ? static_cast<int>(s.perf.inputs.indexOf(c.input)) : port.index;
    if (own < 0 || own >= (in ? s.perf.inputs.size() : s.perf.outputs.size()))
        return;
    const double pad = 18 * u, w = r.width() - 2 * pad, th = 36 * u;
    const double top = r.top() + 20 * u, bodyTop = in ? top + 30 * u + th + 14 * u : top + 30 * u;
    const QRectF body(r.left() + pad, bodyTop, w, r.bottom() - bodyTop - 8 * u);

    // The cards, scrolled. A drag where no card takes it scrolls them; it
    // comes first, so theirs come before it.
    listRect_ = body;
    hit("io scroll", body, {}, [this](QPointF pos, bool done) {
        if (scrollStartY_ < 0 || done) {
            scrollStartY_ = done ? -1 : pos.y();
            scrollFrom_ = scroll_;
            return;
        }
        scroll_ = std::clamp(scrollFrom_ - (pos.y() - scrollStartY_), 0.0, scrollMax_);
        update();
    });
    p.save();
    p.setClipRect(QRectF(r.left(), body.top(), r.width(), r.bottom() - body.top()));
    const double h = ioBody(p, body, c, own);
    p.restore();
    scrollMax_ = std::max(0.0, h - body.height());
    scroll_ = std::clamp(scroll_, 0.0, scrollMax_);

    // Over them: what is scrolled out of sight above cannot be tapped.
    hit("io head", QRectF(r.left(), r.top(), r.width(), body.top() - r.top()), [] {});
    p.setFont(smallLabel(u));
    p.setPen(color::muted);
    p.drawText(QRectF(r.left() + pad, top, w, 18 * u), Qt::AlignLeft | Qt::AlignVCenter, in ? "GAIN STAGE" : QString("OUTPUT %1").arg(own + 1));
    if (!in)
        return;
    const double gap = 6 * u, tw = (w - 2 * gap) / 3;
    const QPair<IoTab, QString> tabs[] = {{IoTab::Chain, "Chain"}, {IoTab::Inputs, "Inputs"}, {IoTab::Outputs, "Outputs"}};
    for (int t = 0; t < 3; ++t) {
        const bool on = ioTab_ == tabs[t].first;
        button(p, "io tab " + tabs[t].second.toLower(), QRectF(r.left() + pad + t * (tw + gap), top + 30 * u, tw, th), tabs[t].second,
               [this, tab = tabs[t].first] {
                   if (!entryFor_.isEmpty())
                       commitEntry();
                   ioTab_ = tab;
                   scroll_ = 0;
                   update();
               },
               on ? color::onOrange : color::text, on ? color::orange : color::border2, on ? color::orange : color::panel2, 14 * u);
    }
}

double ChainView::ioBody(QPainter& p, const QRectF& body, const ChainInfo& c, int own) {
    const bool in = ioSel_->kind == PortInfo::Kind::Input;
    const double w = body.width(), y0 = body.top() - scroll_;
    double y = y0;
    auto label = [&](const QString& text) {
        p.setFont(smallLabel(u));
        p.setPen(color::muted);
        p.drawText(QRectF(body.left(), y, w, 18 * u), Qt::AlignLeft | Qt::AlignVCenter, text);
        y += 26 * u;
    };
    auto help = [&](const QString& text) {
        p.setPen(color::dim);
        p.setFont(typeface(Face::Text, 13 * u));
        const QRectF hr(body.left(), y + 4 * u, w, 400 * u);
        const QRectF used = p.boundingRect(hr, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
        p.drawText(hr, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
        y += used.height() + 12 * u;
    };

    if (!in) {
        y += levelCard(p, QRectF(body.left(), y, w, 0), false, own, true) + 14 * u;
        help("This output's level, mute and limiter are the performance's: the same whichever chain plays, and on every input box's "
             "Outputs tab. The name too. Drag the box to move it.");
        return y - y0;
    }

    if (ioTab_ == IoTab::Chain) {
        // This chain: the input it plays, then every output in it. A
        // block's outputs are the wires that leave it, the input's too.
        label("PLAYS");
        y += levelCard(p, QRectF(body.left(), y, w, 0), true, own, true) + 18 * u;
        label("OUTPUTS IN THIS CHAIN");
        y += wireAutoRow(p, QRectF(body.left(), y, w, 0)) + 14 * u;
        QVector<int> order;
        for (int b = 0; b < c.numPlugins(); ++b)
            order << b;
        const Board B(c, s.perf.outputs.size());
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const QPointF pa = B.block.value(a), pb = B.block.value(b);
            return std::fabs(pa.y() - pb.y()) > 1 ? pa.y() < pb.y() : pa.x() < pb.x();
        });
        order.prepend(-1);
        int cards = 0;
        for (int b : order) {
            const PortInfo from = b < 0 ? PortInfo::input() : PortInfo::block(b);
            QVector<int> wires;
            for (int i = 0; i < c.wires.size(); ++i)
                if (c.wires[i].from == from)
                    wires << i;
            if (wires.isEmpty())
                continue;
            const PluginInfo* pi = b < 0 ? nullptr : c.plugin(b);
            const QString name = b < 0 ? c.input : pi ? pi->label : QString("Block %1").arg(b + 1);
            p.setPen(color::text2);
            p.setFont(typeface(Face::Text, 15 * u, QFont::DemiBold));
            draw::text(p, QRectF(body.left(), y, w * 0.7, 24 * u), Qt::AlignLeft | Qt::AlignVCenter, name);
            p.setPen(color::dim);
            p.setFont(typeface(Face::Text, 12 * u));
            p.drawText(QRectF(body.left(), y, w, 24 * u), Qt::AlignRight | Qt::AlignVCenter,
                       b < 0 ? "input" : bypassed(c.index, b) ? "bypassed" : QString("%1 output%2").arg(wires.size()).arg(wires.size() > 1 ? "s" : ""));
            y += 30 * u;
            for (int i : wires)
                y += outputCard(p, QRectF(body.left(), y, w, 0), c, i) + 8 * u;
            y += 10 * u;
            ++cards;
        }
        help(QString(cards ? "" : "Nothing leaves a plug-in yet. ") +
             "Each wire leaving a plug-in is one of its outputs: draw another from its dot for another output. Its level, mute and mono "
             "are this chain's; the Inputs and Outputs tabs set the whole performance's. Auto listens while the chain plays: Match sets "
             "the level so the plug-in plays as loud as it is fed, Target so it reaches the target (from the input, always the target).");
        return y - y0;
    }

    const bool inputs = ioTab_ == IoTab::Inputs;
    const int n = inputs ? s.perf.inputs.size() : s.perf.outputs.size();
    for (int k = 0; k < n; ++k)
        y += levelCard(p, QRectF(body.left(), y, w, 0), inputs, k, inputs && k == own) + 12 * u;
    help(inputs ? "Each input's gain comes before every chain that plays it: its calibration. Every chain's Gain Stage shows "
                  "and sets the same gains. Auto listens once you play, then sets the gain so the input reaches the target."
                : "Each output's level comes after every chain, then its limiter, which turns down any peak over its ceiling at "
                  "once, with no added latency. Every chain's Gain Stage shows and sets the same levels.");
    return y - y0;
}

// One output of a plug-in (a wire leaving it): where it goes, its level
// (a tap types one), its meter after the level, a fader, mute and mono.
double ChainView::outputCard(QPainter& p, const QRectF& r, const ChainInfo& c, int i) {
    const WireInfo wire = wireNow(c, i);
    const double pad = 12 * u, w = r.width() - 2 * pad, h = 136 * u;
    const QRectF card(r.left(), r.top(), r.width(), h);
    draw::card(p, card, 10 * u, color::panel2, color::border2);
    double y = card.top() + 10 * u;
    const QString key = QString("w %1 %2").arg(c.index).arg(i), id = QString("io wire %1").arg(i);
    const double valueW = 92 * u;
    const QString to = "→ " + portName(c, wire.to) + (wire.to.key ? " (key)" : "");
    p.setPen(wire.muted ? color::dim : color::text);
    p.setFont(draw::fitted(Face::Text, QFont::Medium, to, 15 * u, 11 * u, w - valueW - 8 * u));
    draw::text(p, QRectF(card.left() + pad, y, w - valueW - 8 * u, 30 * u), Qt::AlignLeft | Qt::AlignVCenter, to);
    const QRectF valueR(card.right() - pad - valueW, y, valueW, 30 * u);
    const QString valueId = id + " value";
    if (entryFor_ == valueId) {
        placeEntry(valueId, valueR);
    } else {
        draw::card(p, valueR, 6 * u, isPressed(valueId) ? color::raised : color::panel, color::border);
        p.setPen(wire.muted ? color::orangeLight : color::text);
        p.setFont(typeface(Face::Mono, 13 * u));
        p.drawText(valueR, Qt::AlignCenter, wire.muted ? "muted" : levelText(wire.levelDb));
        hit(valueId, valueR, [this, valueId, i, db = wire.levelDb] {
            startEntry(valueId, db <= kOffDb ? "off" : QString::number(std::round(db * 10) / 10), "dB", 12, [this, i](const QString& typed) {
                const ChainInfo* c = s.chain(chain_);
                double to = 0;
                if (!c || i >= c->wires.size())
                    return;
                if (!typedLevel(typed, to)) {
                    emit s.message("A level is a number of dB, or off", true);
                    return;
                }
                WireInfo w = wireNow(*c, i);
                gainsFrom_ = w;
                w.levelDb = to <= kOffDb ? kOffDb : std::min(to, 12.0);
                setGains(w, true);
            });
        });
    }
    y += 38 * u;
    meterBar(p, QRectF(card.left() + pad, y, w, 8 * u), key, false);
    y += 16 * u;
    const QRectF track(card.left() + pad + 10 * u, y + 14 * u - 3 * u, w - 20 * u, 6 * u);
    const double f = levelToFader(wire.levelDb);
    p.setPen(Qt::NoPen);
    p.setBrush(color::line);
    p.drawRoundedRect(track, 3 * u, 3 * u);
    p.setBrush(wire.muted ? color::dim : color::orange);
    p.drawRoundedRect(QRectF(track.left(), track.top(), std::max(track.height(), track.width() * f), track.height()), 3 * u, 3 * u);
    p.setPen(QPen(color::dim, 1));
    const double tx = track.left() + track.width() * 0.75;
    p.drawLine(QPointF(tx, track.top() - 7 * u), QPointF(tx, track.bottom() + 7 * u));
    p.setPen(QPen(wire.muted ? color::dim : color::orange, 2 * u));
    p.setBrush(color::text);
    p.drawEllipse(QPointF(track.left() + track.width() * f, track.center().y()), 9 * u, 9 * u);
    const QString faderId = id + " fader";
    hit(faderId, QRectF(track.left() - 12 * u, y - 6 * u, track.width() + 24 * u, 40 * u), {}, [this, i, track, faderId](QPointF pos, bool done) {
        const ChainInfo* c = s.chain(chain_);
        if (!c || i >= c->wires.size())
            return;
        WireInfo w = wireNow(*c, i);
        if (gainDrag_ != 0) {
            // Pressed: where this change starts, and whether it is the second tap of two.
            gainDrag_ = 0;
            gainsFrom_ = w;
            gainReset_ = doubleTap(faderId);
        }
        if (done)
            gainDrag_ = -1;
        if (gainReset_) {
            w.levelDb = 0;
        } else {
            const double db = faderToLevel(std::clamp((pos.x() - track.left()) / track.width(), 0.0, 1.0));
            w.levelDb = std::fabs(db) < 0.25 ? 0 : db <= kOffDb ? kOffDb : std::round(db * 10) / 10;
        }
        setGains(w, done);
    });
    y += 38 * u;
    const double gap = 6 * u, bw = (w - 2 * gap) / 3, bh = 32 * u;
    const bool mono = wire.width == 0;
    button(p, id + " mute", QRectF(card.left() + pad, y, bw, bh), wire.muted ? "Unmute" : "Mute",
           [this, i] {
               const ChainInfo* c = s.chain(chain_);
               if (!c || i >= c->wires.size())
                   return;
               WireInfo w = wireNow(*c, i);
               gainsFrom_ = w;
               w.muted = !w.muted;
               setGains(w, true);
           },
           wire.muted ? color::onOrange : color::text, wire.muted ? color::orange : color::border2, wire.muted ? color::orange : color::panel, 13 * u);
    button(p, id + " mono", QRectF(card.left() + pad + bw + gap, y, bw, bh), "Mono",
           [this, i] {
               const ChainInfo* c = s.chain(chain_);
               if (!c || i >= c->wires.size())
                   return;
               WireInfo w = wireNow(*c, i);
               gainsFrom_ = w;
               w.width = w.width == 0 ? 100 : 0;
               setGains(w, true);
           },
           mono ? color::green : color::text, mono ? color::greenDark : color::border2, mono ? color::greenBg : color::panel, 13 * u);
    // Auto, which stops it while it runs.
    const auto running = s.autoNow.constFind(s.autoKey(c.index, wire));
    const bool busy = running != s.autoNow.constEnd();
    QString autoText = "Auto";
    if (busy)
        autoText = running->listening ? QString("%1 / %2 s").arg(running->seconds, 0, 'f', 1).arg(running->of, 0, 'g', 3) : "Play…";
    button(p, id + " auto", QRectF(card.left() + pad + 2 * (bw + gap), y, bw, bh), autoText,
           [this, i, busy] {
               const ChainInfo* c = s.chain(chain_);
               if (c && i < c->wires.size())
                   s.runAuto(c->index, wireNow(*c, i), busy);
           },
           busy ? color::onOrange : color::text, busy ? color::orange : color::border2, busy ? color::orange : color::panel, 13 * u);
    return h;
}

// Auto's settings for every block's outputs, the same in every chain: Match
// or Target, what it measures, its target and its time, each a tap.
double ChainView::wireAutoRow(QPainter& p, const QRectF& r) {
    const AutoInfo was = s.perf.levels.wireAuto;
    const bool match = s.perf.levels.wireMatch;
    const double gap = 6 * u, bh = 36 * u, mw = r.width() * 0.3, cw = (r.width() - mw - 3 * gap) / 3;
    auto setAuto = [this, was, match](AutoInfo to, bool toMatch) {
        s.setWireAuto(to, toMatch, [this, was, match, to, toMatch](const QJsonObject& r) {
            auto req = [](const AutoInfo& a, bool m) {
                QJsonObject j = a.json();
                j["match"] = m;
                return QJsonObject{{"cmd", "level"}, {"wires", true}, {"auto", j}};
            };
            if (s.check(r, "Auto"))
                s.edited({"Auto settings of the outputs", -1, -1, req(was, match), req(to, toMatch)});
        });
    };
    double x = r.left();
    button(p, "io wires mode", QRectF(x, r.top(), mw, bh), match ? "Match" : "Target", [setAuto, was, match] { setAuto(was, !match); },
           color::text, color::border2, color::panel, 14 * u);
    x += mw + gap;
    button(p, "io wires measure", QRectF(x, r.top(), cw, bh), measureText(was.measure),
           [setAuto, was, match] {
               AutoInfo to = was;
               to.measure = was.measure == "peak" ? "rms" : was.measure == "rms" ? "lufs" : "peak";
               setAuto(to, match);
           },
           color::text2, color::border2, color::panel, 13 * u);
    x += cw + gap;
    const QString targetId = "io wires target";
    if (entryFor_ == targetId) {
        placeEntry(targetId, QRectF(x, r.top(), cw, bh));
    } else {
        button(p, targetId, QRectF(x, r.top(), cw, bh), QString::number(was.targetDb, 'g', 3).replace("-", "−"),
               [this, targetId, setAuto, was, match] {
                   startEntry(targetId, QString::number(was.targetDb, 'g', 3), "Target dB", 6, [this, setAuto, was, match](const QString& typed) {
                       double to = 0;
                       if (!typedLevel(typed, to) || to < -60 || to > 0) {
                           emit s.message("Auto's target is -60 to 0 dB", true);
                           return;
                       }
                       AutoInfo a = was;
                       a.targetDb = std::round(to * 10) / 10;
                       setAuto(a, match);
                   });
               },
               match ? color::dim : color::text2, color::border2, color::panel, 13 * u);
    }
    x += cw + gap;
    button(p, "io wires time", QRectF(x, r.top(), cw, bh), secondsText(was.seconds),
           [setAuto, was, match] {
               AutoInfo to = was;
               to.seconds = kTimes[0];
               for (double t : kTimes)
                   if (t > was.seconds + 0.01) {
                       to.seconds = t;
                       break;
                   }
               setAuto(to, match);
           },
           color::text2, color::border2, color::panel, 13 * u);
    return bh;
}

// A plug-in's outputs in its own panel: a row for each wire leaving it,
// with its meter and level; a tap selects the wire.
double ChainView::outputRows(QPainter& p, const QRectF& r, const ChainInfo& c, const PortInfo& from) {
    double y = r.top();
    p.setFont(smallLabel(u));
    p.setPen(color::muted);
    p.drawText(QRectF(r.left(), y, r.width(), 18 * u), Qt::AlignLeft | Qt::AlignVCenter, "OUTPUTS");
    y += 26 * u;
    int shown = 0;
    for (int i = 0; i < c.wires.size(); ++i) {
        if (c.wires[i].from != from)
            continue;
        const WireInfo wire = wireNow(c, i);
        const QRectF row(r.left(), y, r.width(), 36 * u);
        const QString id = QString("output %1").arg(i);
        draw::card(p, row, 8 * u, isPressed(id) ? color::raised : color::panel2, color::border2);
        const double valueW = 70 * u, meterW = std::min(70 * u, row.width() * 0.25);
        p.setPen(wire.muted ? color::dim : color::text);
        p.setFont(typeface(Face::Text, 14 * u));
        draw::text(p, QRectF(row.left() + 10 * u, y, row.width() - valueW - meterW - 28 * u, row.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   "→ " + portName(c, wire.to) + (wire.to.key ? " (key)" : "") + (wire.width == 0 ? " · mono" : ""));
        meterBar(p, QRectF(row.right() - valueW - meterW - 8 * u, row.center().y() - 3 * u, meterW, 6 * u), QString("w %1 %2").arg(c.index).arg(i),
                 false);
        p.setPen(wire.muted ? color::orangeLight : color::muted);
        p.setFont(typeface(Face::Mono, 12 * u));
        p.drawText(QRectF(row.right() - valueW - 4 * u, y, valueW - 6 * u, row.height()), Qt::AlignRight | Qt::AlignVCenter,
                   wire.muted ? "muted" : levelText(wire.levelDb));
        hit(id, row, [this, i] { selectWire(i); });
        y += row.height() + 6 * u;
        ++shown;
    }
    if (!shown) {
        p.setPen(color::dim);
        p.setFont(typeface(Face::Text, 13 * u));
        p.drawText(QRectF(r.left(), y, r.width(), 22 * u), Qt::AlignLeft | Qt::AlignVCenter, "None: drag from its right-hand dot to add one.");
        y += 26 * u;
    }
    return y - r.top();
}

double ChainView::levelCard(QPainter& p, const QRectF& r, bool input, int index, bool own) {
    const QStringList& names = input ? s.perf.inputs : s.perf.outputs;
    if (index < 0 || index >= names.size())
        return 0;
    const QString name = names[index];
    const QString key = AppState::autoKey(input, index);
    const InputLevel inLevel = input ? s.perf.levels.inputs.value(index) : InputLevel{};
    const OutputLevel outLevel = input ? OutputLevel{} : s.perf.levels.outputs.value(index);
    double db = input ? inLevel.gainDb : outLevel.levelDb;
    if (levelDrag_ && levelDrag_->input == input && levelDrag_->index == index)
        db = levelDrag_->nowDb;
    const AutoInfo autoSet = input ? inLevel.autoSet : outLevel.autoSet;
    const double pad = 12 * u, w = r.width() - 2 * pad;
    const double h = (input ? 160 : 202) * u;
    const QRectF card(r.left(), r.top(), r.width(), h);
    draw::card(p, card, 10 * u, color::panel2, own ? color::orange : color::border2);
    double y = card.top() + 10 * u;
    const QString tag = input ? QString("in %1").arg(index + 1) : QString("out %1").arg(index + 1);

    // The name and the level, each a tap to type.
    const QString nameId = QString("io %1 name").arg(key), valueId = QString("io %1 value").arg(key);
    const double valueW = 92 * u;
    const QRectF nameR(card.left() + pad, y, w - valueW - 8 * u, 32 * u), valueR(card.right() - pad - valueW, y, valueW, 32 * u);
    if (entryFor_ == nameId) {
        placeEntry(nameId, nameR);
    } else {
        p.setPen(color::text);
        p.setFont(draw::fitted(Face::Text, QFont::DemiBold, name, 17 * u, 12 * u, nameR.width() - 44 * u));
        const QRectF used = p.boundingRect(nameR, Qt::AlignLeft | Qt::AlignVCenter, name);
        draw::text(p, nameR.adjusted(0, 0, -44 * u, 0), Qt::AlignLeft | Qt::AlignVCenter, name);
        p.setPen(color::dim);
        p.setFont(typeface(Face::Mono, 11 * u));
        p.drawText(QRectF(std::min(used.right(), nameR.right() - 44 * u) + 6 * u, y, 44 * u, 32 * u), Qt::AlignLeft | Qt::AlignVCenter, tag);
        hit(nameId, nameR, [this, nameId, name, input, index] {
            startEntry(nameId, name, "Its name", 40, [this, input, index](const QString& text) { renamePort(input, index, text); });
        });
    }
    if (entryFor_ == valueId) {
        placeEntry(valueId, valueR);
    } else {
        draw::card(p, valueR, 6 * u, isPressed(valueId) ? color::raised : color::panel, color::border);
        const bool muted = !input && outLevel.muted;
        p.setPen(muted ? color::orangeLight : color::text);
        p.setFont(typeface(Face::Mono, 13 * u));
        p.drawText(valueR, Qt::AlignCenter, muted ? "muted" : levelText(db));
        hit(valueId, valueR, [this, valueId, db, input, index] {
            startEntry(valueId, db <= kOffDb ? "off" : QString::number(std::round(db * 10) / 10), "dB", 12, [this, input, index](const QString& typed) {
                double to = 0;
                if (!typedLevel(typed, to)) {
                    emit s.message("A level is a number of dB, or off", true);
                    return;
                }
                const double from = input ? s.perf.levels.inputs.value(index).gainDb : s.perf.levels.outputs.value(index).levelDb;
                to = to <= kOffDb ? -100 : std::clamp(to, -100.0, input ? 60.0 : 24.0);
                sendLevel(input, index, to, true, from);
            });
        });
    }
    y += 40 * u;

    // The meter: an input after its gain, an output as it goes out.
    meterBar(p, QRectF(card.left() + pad, y, w, 8 * u), key, !input);
    y += 18 * u;

    // The fader. A double tap sets it back to 0 dB.
    const QRectF track(card.left() + pad + 10 * u, y + 14 * u - 3 * u, w - 20 * u, 6 * u);
    const double f = input ? gainToFader(db) : levelToFader(db), tick = input ? gainToFader(0) : 0.75;
    p.setPen(Qt::NoPen);
    p.setBrush(color::line);
    p.drawRoundedRect(track, 3 * u, 3 * u);
    p.setBrush(color::orange);
    p.drawRoundedRect(QRectF(track.left(), track.top(), std::max(track.height(), track.width() * f), track.height()), 3 * u, 3 * u);
    p.setPen(QPen(color::dim, 1));
    const double tx = track.left() + track.width() * tick;
    p.drawLine(QPointF(tx, track.top() - 7 * u), QPointF(tx, track.bottom() + 7 * u));
    p.setPen(QPen(color::orange, 2 * u));
    p.setBrush(color::text);
    p.drawEllipse(QPointF(track.left() + track.width() * f, track.center().y()), 9 * u, 9 * u);
    const QString faderId = QString("io %1 fader").arg(key);
    hit(faderId, QRectF(track.left() - 12 * u, y - 6 * u, track.width() + 24 * u, 40 * u), {},
        [this, input, index, track, faderId](QPointF pos, bool done) { dragLevel(input, index, track, faderId, pos, done); });
    y += 40 * u;

    const double gap = 6 * u, bh = 36 * u;
    if (!input) {
        // Mute, and the limiter: on or off, and its ceiling.
        const double bw = (w - 2 * gap) / 3;
        const QRectF muteR(card.left() + pad, y, bw, bh), limR(muteR.right() + gap, y, bw, bh), ceilR(limR.right() + gap, y, bw, bh);
        button(p, QString("io %1 mute").arg(key), muteR, outLevel.muted ? "Unmute" : "Mute",
               [this, index, muted = outLevel.muted, name] {
                   s.setLevel(false, index, {{"muted", !muted}}, [this, index, muted, name](const QJsonObject& r) {
                       if (s.check(r, "Not muted"))
                           s.edited({(muted ? "Unmuted " : "Muted ") + name, -1, -1,
                                     {{"cmd", "level"}, {"output", index}, {"muted", muted}},
                                     {{"cmd", "level"}, {"output", index}, {"muted", !muted}}});
                   });
               },
               outLevel.muted ? color::onOrange : color::text, outLevel.muted ? color::orange : color::border2,
               outLevel.muted ? color::orange : color::panel, 14 * u);
        button(p, QString("io %1 limiter").arg(key), limR, "Limiter",
               [this, index, on = outLevel.limiter, name] {
                   s.setLevel(false, index, {{"limiter", QJsonObject{{"on", !on}}}}, [this, index, on, name](const QJsonObject& r) {
                       if (s.check(r, "Limiter"))
                           s.edited({QString("Limiter ") + (on ? "off on " : "on on ") + name, -1, -1,
                                     {{"cmd", "level"}, {"output", index}, {"limiter", QJsonObject{{"on", on}}}},
                                     {{"cmd", "level"}, {"output", index}, {"limiter", QJsonObject{{"on", !on}}}}});
                   });
               },
               outLevel.limiter ? color::green : color::text, outLevel.limiter ? color::greenDark : color::border2,
               outLevel.limiter ? color::greenBg : color::panel, 14 * u);
        const QString ceilId = QString("io %1 ceiling").arg(key);
        if (entryFor_ == ceilId) {
            placeEntry(ceilId, ceilR);
        } else {
            button(p, ceilId, ceilR, "≤ " + levelText(outLevel.ceilingDb),
                   [this, ceilId, index, was = outLevel.ceilingDb, name] {
                       startEntry(ceilId, QString::number(was), "Ceiling dB", 8, [this, index, was, name](const QString& typed) {
                           double to = 0;
                           if (!typedLevel(typed, to) || to < -60 || to > 0) {
                               emit s.message("The ceiling is -60 to 0 dB", true);
                               return;
                           }
                           s.setLevel(false, index, {{"limiter", QJsonObject{{"ceiling_db", to}}}}, [this, index, was, to, name](const QJsonObject& r) {
                               if (s.check(r, "Ceiling"))
                                   s.edited({"Limiter ceiling of " + name, -1, -1,
                                             {{"cmd", "level"}, {"output", index}, {"limiter", QJsonObject{{"ceiling_db", was}}}},
                                             {{"cmd", "level"}, {"output", index}, {"limiter", QJsonObject{{"ceiling_db", to}}}}});
                           });
                       });
                   },
                   outLevel.limiter ? color::text : color::muted, color::border2, color::panel, 13 * u);
        }
        y += bh + gap;
    }

    // Auto: the button (which stops it while it runs), then what it
    // measures, its target and its time, each a tap to change.
    const auto running = s.autoNow.constFind(key);
    const bool busy = running != s.autoNow.constEnd();
    const double aw = w * 0.38, cw = (w - aw - 3 * gap) / 3;
    QString autoText = "Auto";
    if (busy)
        autoText = running->listening ? QString("%1 / %2 s").arg(running->seconds, 0, 'f', 1).arg(running->of, 0, 'g', 3) : "Play…";
    button(p, QString("io %1 auto").arg(key), QRectF(card.left() + pad, y, aw, bh), autoText,
           [this, input, index, busy] { s.runAuto(input, index, busy); }, busy ? color::onOrange : color::text,
           busy ? color::orange : color::border2, busy ? color::orange : color::panel, 14 * u);
    auto setAuto = [this, input, index, autoSet, name](AutoInfo to) {
        s.setLevel(input, index, {{"auto", to.json()}}, [this, input, index, autoSet, to, name](const QJsonObject& r) {
            const char* which = input ? "input" : "output";
            if (s.check(r, "Auto"))
                s.edited({"Auto settings of " + name, -1, -1, {{"cmd", "level"}, {which, index}, {"auto", autoSet.json()}},
                          {{"cmd", "level"}, {which, index}, {"auto", to.json()}}});
        });
    };
    double x = card.left() + pad + aw + gap;
    button(p, QString("io %1 measure").arg(key), QRectF(x, y, cw, bh), measureText(autoSet.measure),
           [setAuto, autoSet] {
               AutoInfo to = autoSet;
               to.measure = autoSet.measure == "peak" ? "rms" : autoSet.measure == "rms" ? "lufs" : "peak";
               setAuto(to);
           },
           color::text2, color::border2, color::panel, 13 * u);
    x += cw + gap;
    const QString targetId = QString("io %1 target").arg(key);
    if (entryFor_ == targetId) {
        placeEntry(targetId, QRectF(x, y, cw, bh));
    } else {
        button(p, targetId, QRectF(x, y, cw, bh), QString::number(autoSet.targetDb, 'g', 3).replace("-", "−"),
               [this, targetId, setAuto, autoSet] {
                   startEntry(targetId, QString::number(autoSet.targetDb, 'g', 3), "Target dB", 6, [this, setAuto, autoSet](const QString& typed) {
                       double to = 0;
                       if (!typedLevel(typed, to) || to < -60 || to > 0) {
                           emit s.message("Auto's target is -60 to 0 dB", true);
                           return;
                       }
                       AutoInfo a = autoSet;
                       a.targetDb = std::round(to * 10) / 10;
                       setAuto(a);
                   });
               },
               color::text2, color::border2, color::panel, 13 * u);
    }
    x += cw + gap;
    button(p, QString("io %1 time").arg(key), QRectF(x, y, cw, bh), secondsText(autoSet.seconds),
           [setAuto, autoSet] {
               AutoInfo to = autoSet;
               to.seconds = kTimes[0];
               for (double t : kTimes)
                   if (t > autoSet.seconds + 0.01) {
                       to.seconds = t;
                       break;
                   }
               setAuto(to);
           },
           color::text2, color::border2, color::panel, 13 * u);
    return h;
}

void ChainView::dragLevel(bool input, int index, const QRectF& track, const QString& id, QPointF pos, bool done) {
    const double now = input ? s.perf.levels.inputs.value(index).gainDb : s.perf.levels.outputs.value(index).levelDb;
    if (!levelDrag_ || levelDrag_->input != input || levelDrag_->index != index) {
        // Pressed: where this change starts, and whether it is the second tap of two.
        levelDrag_ = LevelDrag{input, index, now, now, doubleTap(id)};
    }
    double db = 0;
    if (!levelDrag_->reset) {
        const double f = std::clamp((pos.x() - track.left()) / track.width(), 0.0, 1.0);
        db = input ? faderToGain(f) : faderToLevel(f);
        db = std::fabs(db) < 0.25 ? 0 : db <= kOffDb ? -100 : std::round(db * 10) / 10;
    }
    levelDrag_->nowDb = db;
    const double from = levelDrag_->fromDb;
    if (done)
        levelDrag_.reset();
    sendLevel(input, index, db, done, from);
    update();
}

void ChainView::sendLevel(bool input, int index, double db, bool done, double fromDb) {
    const qint64 now = clock_.elapsed();
    if (!done && now - lastLevelSend_ < 40)
        return;
    lastLevelSend_ = now;
    const char* field = input ? "gain_db" : "level_db";
    s.setLevel(input, index, {{field, db}}, [this, input, index, db, done, fromDb, field](const QJsonObject& r) {
        if (!s.check(r, input ? "The gain did not take it" : "The level did not take it") || !done || db == fromDb)
            return;
        const char* which = input ? "input" : "output";
        const QString name = (input ? s.perf.inputs : s.perf.outputs).value(index);
        s.edited({(input ? "Gain of " : "Level of ") + name, -1, -1, {{"cmd", "level"}, {which, index}, {field, fromDb}},
                  {{"cmd", "level"}, {which, index}, {field, db}}});
        update();
    });
}

} // namespace cue::gui
