#include "gui/chainview.h"

#include <QFontMetricsF>
#include <QJsonArray>
#include <QLineEdit>
#include <QLineF>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointingDevice>
#include <QRegularExpression>
#include <QTouchEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace cue::gui {

namespace {

enum class Shape { Hexagon, GainStage, Act };

Shape shapeOf(const PluginInfo& p) {
    const QString id = p.id.toLower(), label = p.label.toLower();
    const QString file = p.path.section('/', -1).toLower();
    if (id == "com.twostorms.gainstage" || label.contains("gain stage") || file.contains("gain stage"))
        return Shape::GainStage;
    static const QRegularExpression act("\\bact\\b");
    if (id == "com.twostorms.act" || act.match(label).hasMatch() || (id.isEmpty() && act.match(file).hasMatch()))
        return Shape::Act;
    return Shape::Hexagon;
}

// Where everything of a chain sits, in units before fitting it to the canvas:
// lines are rows (the main line first), plug-ins columns, the input at the
// left and the outputs at the right.
struct Layout {
    static constexpr double colPitch = 128, rowPitch = 150, edge = 34, blockW = 100, blockH = 74, ioW = 72, ioH = 56;
    QVector<int> startCol, row;
    QVector<int> outputRow;
    int cols = 2, rows = 1;

    explicit Layout(const ChainInfo& c) {
        const int n = c.lines.size();
        startCol.resize(n);
        row.resize(n);
        int lastCol = 1;
        for (int l = 0; l < n; ++l) {
            const LineInfo& line = c.lines[l];
            // Branches start under the main line, as the routing board draws
            // them; the wire from where they leave loops back to them.
            startCol[l] = 1;
            row[l] = l;
            lastCol = std::max(lastCol, startCol[l] + std::max<int>(1, line.plugins.size()) - 1);
        }
        cols = lastCol + 2;
        rows = n;
        // Each output beside the line it takes, unless that row is taken.
        QVector<bool> used(n + c.taps.size(), false);
        for (const TapInfo& t : c.taps) {
            int r = row.value(t.line, 0);
            while (used[r])
                ++r;
            used[r] = true;
            outputRow << r;
            rows = std::max(rows, r + 1);
        }
    }
    double width() const { return edge * 2 + cols * colPitch; }
    double height() const { return edge * 2 + rows * rowPitch; }
    QPointF center(int col, int r) const { return {edge + (col + 0.5) * colPitch, edge + (r + 0.5) * rowPitch}; }
};

} // namespace

ChainView::ChainView(AppState& state, QWidget* parent) : Surface(state, parent) {
    // Touch comes as touches here, not as a mouse, so two fingers can zoom.
    setAttribute(Qt::WA_AcceptTouchEvents);
    clock_.start();
    refresh_.setInterval(700);
    connect(&refresh_, &QTimer::timeout, this, &ChainView::requestPlugin);
    // The chain playing, until another is picked here.
    connect(&s, &AppState::statusChanged, this, [this] {
        if (followPlaying_ && s.playing() != chain_ && s.chain(s.playing()))
            showChain(s.playing());
        if (isVisible())
            update();
    });
    connect(&s, &AppState::changed, this, [this] {
        if (!s.chain(chain_))
            showChain(s.playing());
        update();
    });
}

void ChainView::showEvent(QShowEvent*) {
    followPlaying_ = true;
    showChain(s.playing());
    refresh_.start();
    requestPlugin();
}

void ChainView::hideEvent(QHideEvent*) { refresh_.stop(); }

void ChainView::showChain(int chain) {
    if (chain != chain_) {
        chain_ = chain;
        selectSlot(-1);
        fit();
    }
    update();
}

void ChainView::selectSlot(int slot) {
    if (slot != slot_) {
        slot_ = slot;
        plugin_ = {};
        scroll_ = 0;
        draggingParam_ = -1;
        dragRow_ = -1;
        shownPending_ = 0;
        setShowAll(false);
    }
    requestPlugin();
    update();
}

void ChainView::requestPlugin() {
    if (slot_ < 0 || chain_ < 0 || !s.connected() || draggingParam_ >= 0)
        return;
    const int chain = chain_, slot = slot_;
    s.link.request({{"cmd", "plugin"}, {"chain", chain}, {"slot", slot}}, [this, chain, slot](const QJsonObject& r) {
        if (chain != chain_ || slot != slot_ || !r.value("ok").toBool() || draggingParam_ >= 0)
            return;
        plugin_ = r.value("plugin").toObject();
        update();
    });
}

void ChainView::setParam(const QJsonObject& param, double value, bool done) {
    const int id = param.value("id").toInt();
    const double lo = param.value("min").toDouble(), hi = param.value("max").toDouble(1);
    const int steps = param.value("steps").toInt();
    value = std::clamp(value, lo, hi);
    if (steps > 0 && hi > lo)
        value = lo + std::round((value - lo) / (hi - lo) * steps) * (hi - lo) / steps;
    draggingParam_ = done ? -1 : id;
    draggingValue_ = value;
    // Into the list as the value now, so the slider stays where it was let go.
    QJsonArray params = plugin_.value("params").toArray();
    for (int i = 0; i < params.size(); ++i)
        if (params[i].toObject().value("id").toInt() == id) {
            QJsonObject q = params[i].toObject();
            q["value"] = value;
            params[i] = q;
        }
    plugin_["params"] = params;
    const qint64 now = clock_.elapsed();
    if (done || now - lastSend_ >= 40) {
        lastSend_ = now;
        const int chain = chain_, slot = slot_;
        s.link.request({{"cmd", "param"}, {"chain", chain}, {"slot", slot}, {"param", id}, {"value", value}},
                       [this, chain, slot, id](const QJsonObject& r) {
                           if (!s.check(r, "The plug-in did not take it") || chain != chain_ || slot != slot_)
                               return;
                           QJsonArray params = plugin_.value("params").toArray();
                           for (int i = 0; i < params.size(); ++i)
                               if (params[i].toObject().value("id").toInt() == id) {
                                   QJsonObject q = params[i].toObject();
                                   q["text"] = r.value("text").toString();
                                   params[i] = q;
                               }
                           plugin_["params"] = params;
                           update();
                       });
    }
    update();
}

void ChainView::wheelEvent(QWheelEvent* e) {
    if (listRect_.contains(e->position())) {
        scroll_ = std::clamp(scroll_ - e->angleDelta().y() / 120.0 * 60 * u, 0.0, scrollMax_);
        update();
    } else if (canvasRect_.contains(e->position())) {
        // A wheel zooms where it points; two fingers on a touchpad pan,
        // and zoom with Ctrl.
        const bool pad = e->device() && e->device()->type() == QInputDevice::DeviceType::TouchPad && !e->pixelDelta().isNull();
        if (pad && !(e->modifiers() & Qt::ControlModifier)) {
            pan_ += e->pixelDelta();
            clampPan();
            update();
        } else {
            const double steps = (pad ? e->pixelDelta().y() / 50.0 : e->angleDelta().y() / 120.0);
            zoomAt(e->position(), std::pow(1.2, steps));
        }
    }
}

QPointF ChainView::origin(double k) const {
    return QPointF(canvasRect_.center().x() - layoutSize_.width() * k / 2, canvasRect_.center().y() - layoutSize_.height() * k / 2) + pan_;
}

// At least a corner of the chain stays in the canvas.
void ChainView::clampPan() {
    if (layoutSize_.isEmpty() || canvasRect_.isEmpty())
        return;
    const double k = scaleNow(), w = layoutSize_.width() * k, h = layoutSize_.height() * k;
    const double keepX = std::min(90 * u, w), keepY = std::min(90 * u, h);
    const QPointF o = origin(k);
    const double x = std::clamp(o.x(), canvasRect_.left() + keepX - w, canvasRect_.right() - keepX);
    const double y = std::clamp(o.y(), canvasRect_.top() + keepY - h, canvasRect_.bottom() - keepY);
    pan_ += QPointF(x, y) - o;
}

void ChainView::zoomAt(QPointF pos, double factor) {
    if (layoutSize_.isEmpty())
        return;
    const double k0 = scaleNow();
    const QPointF at = (pos - origin(k0)) / k0; // stays under pos
    const double most = std::max(4.0, 3 * u / fitK_);
    zoom_ = std::clamp(zoom_ * factor, 0.5, most);
    const double k = scaleNow();
    pan_ += pos - at * k - origin(k);
    clampPan();
    update();
}

void ChainView::fit() {
    zoom_ = 1;
    pan_ = {};
    update();
}

bool ChainView::event(QEvent* e) {
    switch (e->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel: {
        auto* te = static_cast<QTouchEvent*>(e);
        if (e->type() == QEvent::TouchBegin)
            pinching_ = touchDone_ = false;
        if (e->type() == QEvent::TouchCancel) {
            pointerCancel();
            pinching_ = false;
            return true;
        }
        QVector<QPointF> down;
        for (const QEventPoint& pt : te->points())
            if (pt.state() != QEventPoint::Released)
                down << pt.position();
        if (down.size() >= 2) {
            const QPointF mid = (down[0] + down[1]) / 2;
            const double dist = std::max(1.0, QLineF(down[0], down[1]).length());
            if (!pinching_ && canvasRect_.contains(mid)) {
                // A second finger: whatever the first was doing stops, and the two zoom and pan.
                pinching_ = touchDone_ = true;
                pointerCancel();
                panning_ = false;
                pinchDist_ = dist;
                pinchZoom_ = zoom_;
                pinchAt_ = (mid - origin(scaleNow())) / scaleNow();
            } else if (pinching_) {
                zoom_ = std::clamp(pinchZoom_ * dist / pinchDist_, 0.5, std::max(4.0, 3 * u / fitK_));
                const double k = scaleNow();
                pan_ += mid - pinchAt_ * k - origin(k);
                clampPan();
                update();
            }
            return true;
        }
        if (touchDone_) {
            // After two fingers, nothing until all are lifted.
            if (e->type() == QEvent::TouchEnd)
                pinching_ = false;
            return true;
        }
        if (te->points().isEmpty())
            return true;
        const QPointF pos = te->points().first().position();
        if (e->type() == QEvent::TouchBegin)
            pointerPress(pos);
        else if (e->type() == QEvent::TouchUpdate)
            pointerMove(pos);
        else
            pointerRelease(pos);
        return true;
    }
    case QEvent::NativeGesture: {
        // A touchpad's pinch.
        auto* ge = static_cast<QNativeGestureEvent*>(e);
        if (ge->gestureType() == Qt::ZoomNativeGesture && canvasRect_.contains(ge->position())) {
            zoomAt(ge->position(), 1 + ge->value());
            return true;
        }
        break;
    }
    default:
        break;
    }
    return Surface::event(e);
}

void ChainView::paint(QPainter& p) {
    const Performance& perf = s.perf;
    QString subtitle;
    if (const ChainInfo* c = s.chain(chain_); c && c->scene < perf.scenes.size())
        subtitle = "Scene " + perf.scenes[c->scene].name;
    const double top = header(p, perf.valid ? perf.name : "Cue", subtitle, enginePills(),
                              {{"stage", "Stage", [this] { emit navigate("stage"); }},
                               {"tuner", "Tuner", [this] { emit navigate("tuner"); }, color::green, color::greenDark, color::panel},
                               {"settings", "Settings", [this] { emit navigate("settings"); }},
                               {"save", "Save", [this] { s.save(); }, color::onOrange, color::orange, color::orange}});
    const double W = width(), H = height();
    const double sideW = std::clamp(230 * u, 150.0, W * 0.22), panelW = std::clamp(310 * u, 220.0, W * 0.3);
    p.fillRect(QRectF(0, top, sideW, H - top), color::bg);
    p.setPen(QPen(color::line, 1));
    p.drawLine(QPointF(sideW - 0.5, top), QPointF(sideW - 0.5, H));
    p.drawLine(QPointF(W - panelW + 0.5, top), QPointF(W - panelW + 0.5, H));
    sidebar(p, QRectF(0, top, sideW, H - top));

    if (!s.chain(chain_) && followPlaying_)
        chain_ = s.playing();
    const ChainInfo* c = s.chain(chain_);
    const double m = 20 * u;
    const QRectF mid(sideW + m, top + m, W - sideW - panelW - 2 * m, H - top - 2 * m);
    if (!c) {
        p.setPen(color::muted);
        p.setFont(typeface(Face::Text, 16 * u));
        p.drawText(mid, Qt::AlignCenter, s.perf.valid ? "No chain playing yet" : "Loading the performance…");
        return;
    }
    // Title, then the canvas, then the legend.
    p.setPen(color::text);
    p.setFont(typeface(Face::Display, 30 * u, QFont::DemiBold));
    const QString title = "Chain: " + c->name;
    const double titleW = std::min(mid.width() * 0.6, QFontMetricsF(p.font()).horizontalAdvance(title) + 2);
    draw::text(p, QRectF(mid.left(), mid.top(), titleW, 40 * u), Qt::AlignLeft | Qt::AlignVCenter, title);
    p.setPen(color::muted);
    p.setFont(typeface(Face::Text, 14 * u));
    const int n = c->numPlugins();
    QString what = QString("%1 plug-in%2 · %3 output%4").arg(n).arg(n == 1 ? "" : "s").arg(c->taps.size()).arg(c->taps.size() == 1 ? "" : "s");
    if (perf.inputs.size() > 1)
        what = "plays " + c->input + " · " + what;
    double right = mid.right();
    if (c->index != s.playing()) {
        const double bw = 150 * u;
        button(p, "play", QRectF(right - bw, mid.top() + 2 * u, bw, 38 * u), "Play this chain", [this] { s.select(chain_); }, color::text,
               color::border2, color::panel, 15 * u);
        right -= bw + 12 * u;
    }
    draw::text(p, QRectF(mid.left() + titleW + 14 * u, mid.top(), std::max(0.0, right - mid.left() - titleW - 14 * u), 40 * u),
               Qt::AlignLeft | Qt::AlignVCenter, what);
    const double legendH = 64 * u;
    canvas(p, QRectF(mid.left(), mid.top() + 54 * u, mid.width(), mid.height() - 54 * u - legendH - 10 * u), *c);
    legend(p, QRectF(mid.left(), mid.bottom() - legendH, mid.width(), legendH));
    panel(p, QRectF(W - panelW, top, panelW, H - top), *c);
}

void ChainView::sidebar(QPainter& p, const QRectF& r) {
    const double pad = 14 * u;
    double y = r.top() + 18 * u;
    QFont label = typeface(Face::Text, 12 * u, QFont::DemiBold);
    label.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * u);
    p.setFont(label);
    p.setPen(color::muted);
    p.drawText(QRectF(r.left() + pad, y, r.width(), 18 * u), Qt::AlignLeft | Qt::AlignVCenter, "CHAINS");
    y += 28 * u;
    const int playing = s.playing(), warm = s.status.valid ? s.status.warmChain() : -1;
    const double rowH = 44 * u;
    for (int sc = 0; sc < s.perf.scenes.size(); ++sc) {
        const SceneInfo& scene = s.perf.scenes[sc];
        if (s.perf.scenes.size() > 1) {
            p.setPen(color::text2);
            p.setFont(typeface(Face::Text, 14 * u, QFont::DemiBold));
            draw::text(p, QRectF(r.left() + pad, y, r.width() - 2 * pad, 30 * u), Qt::AlignLeft | Qt::AlignVCenter, "Scene: " + scene.name);
            y += 32 * u;
        }
        for (int index : scene.chains) {
            const ChainInfo* c = s.chain(index);
            if (!c || y > r.bottom() - rowH)
                continue;
            const QRectF row(r.left() + 10 * u, y, r.width() - 20 * u, rowH - 4 * u);
            const QString id = QString("show %1").arg(index);
            const bool shown = index == chain_;
            if (shown || isPressed(id))
                draw::card(p, row, 8 * u, shown ? QColor(0x2A, 0x1A, 0x0A) : color::raised, QColor());
            p.setPen(shown ? color::orange : color::text);
            p.setFont(typeface(Face::Text, 16 * u, shown ? QFont::DemiBold : QFont::Normal));
            const QString tag = index == playing ? "playing" : index == warm ? "warm" : QString();
            const double tagW = tag.isEmpty() ? 0 : 64 * u;
            draw::text(p, row.adjusted(14 * u, 0, -tagW - 8 * u, 0), Qt::AlignLeft | Qt::AlignVCenter, c->name);
            if (!tag.isEmpty()) {
                p.setPen(index == playing ? color::orange : color::muted);
                p.setFont(typeface(Face::Text, 12 * u));
                p.drawText(row.adjusted(0, 0, -10 * u, 0), Qt::AlignRight | Qt::AlignVCenter, tag);
            }
            hit(id, row, [this, index] {
                followPlaying_ = index == s.playing();
                showChain(index);
            });
            y += rowH;
        }
    }
}

void ChainView::canvas(QPainter& p, const QRectF& r, const ChainInfo& c) {
    draw::card(p, r, 14 * u, color::panel, color::line);
    const Layout L(c);
    // Fitted, the chain fills the canvas but is never larger than the
    // design; the zoom and pan go on top.
    canvasRect_ = r;
    layoutSize_ = QSizeF(L.width(), L.height());
    fitK_ = std::min({u, (r.width() - 8) / L.width(), (r.height() - 8) / L.height()});
    clampPan();
    const double k = scaleNow();
    const QPointF origin = this->origin(k);
    auto at = [&](QPointF pt) { return origin + pt * k; };

    // A dot grid, as the routing board has, moving with the chain.
    p.save();
    p.setClipRect(r.adjusted(2, 2, -2, -2));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x22, 0x22, 0x26));
    double step = 22 * k;
    while (step < 11 * u)
        step *= 2;
    const double x0 = r.left() + std::fmod(std::fmod(origin.x() - r.left(), step) + step, step);
    const double y0 = r.top() + std::fmod(std::fmod(origin.y() - r.top(), step) + step, step);
    for (double y = y0; y < r.bottom(); y += step)
        for (double x = x0; x < r.right(); x += step)
            p.drawEllipse(QPointF(x, y), 0.9 * u, 0.9 * u);
    p.restore();

    // Open space: a drag pans, a double tap fits the chain again.
    hit("canvas", r, {}, [this](QPointF pos, bool done) {
        if (!panning_) {
            panning_ = true;
            panMoved_ = false;
            panStart_ = pos;
            panFrom_ = pan_;
            return;
        }
        if (!done) {
            if (!panMoved_ && QLineF(pos, panStart_).length() < 6 * u)
                return;
            panMoved_ = true;
            pan_ = panFrom_ + (pos - panStart_);
            clampPan();
            update();
            return;
        }
        panning_ = false;
        if (panMoved_ || pinching_)
            return;
        const qint64 now = clock_.elapsed();
        if (now - lastTapAt_ < 400 && QLineF(pos, lastTapPos_).length() < 30 * u) {
            fit();
            lastTapAt_ = -1000;
        } else {
            lastTapAt_ = now;
            lastTapPos_ = pos;
        }
    });

    const bool playing = c.index == s.playing() && s.status.valid;
    const bool live = playing && !s.status.muted;
    auto bypassed = [&](int slot) {
        const PluginInfo* pi = c.plugin(slot);
        if (s.status.valid)
            return s.status.bypassed(c.index, slot);
        return pi && pi->bypassed;
    };
    auto mutes = [&](const PluginInfo& pi) { return bypassed(pi.slot) && pi.muteOnBypass; };
    // Is the signal at a point of a line silenced by a muting bypass before it?
    std::function<bool(int, int)> silent = [&](int l, int pos) -> bool {
        const LineInfo& line = c.lines[l];
        for (int i = 0; i < pos && i < line.plugins.size(); ++i)
            if (mutes(line.plugins[i]))
                return true;
        return l > 0 && silent(std::max(0, line.parent), line.after);
    };
    // Does the signal at a point of a line reach an output?
    std::function<bool(int, int)> reaches = [&](int l, int pos) -> bool {
        const LineInfo& line = c.lines[l];
        auto clear = [&](int a) {
            for (int i = pos; i < a && i < line.plugins.size(); ++i)
                if (mutes(line.plugins[i]))
                    return false;
            return true;
        };
        for (const TapInfo& t : c.taps)
            if (t.line == l && t.after >= pos && clear(t.after))
                return true;
        for (int b = 1; b < c.lines.size(); ++b)
            if (c.lines[b].parent == l && c.lines[b].after >= pos && clear(c.lines[b].after) && reaches(b, 0))
                return true;
        return false;
    };
    auto green = [&](int l, int pos) { return live && !silent(l, pos) && reaches(l, pos); };

    // Port positions, in layout units.
    auto blockCenter = [&](int l, int i) { return L.center(L.startCol[l] + i, L.row[l]); };
    auto halfWidth = [&](const PluginInfo& pi) {
        const Shape sh = shapeOf(pi);
        return sh == Shape::GainStage ? 34.0 : sh == Shape::Act ? 15.0 : Layout::blockW / 2;
    };
    const QPointF inputCenter = L.center(0, 0);
    std::function<QPointF(int, int)> point = [&](int l, int pos) -> QPointF {
        const LineInfo& line = c.lines[l];
        if (pos > 0 && pos <= line.plugins.size()) {
            const QPointF b = blockCenter(l, pos - 1);
            return {b.x() + halfWidth(line.plugins[pos - 1]), b.y()};
        }
        if (l == 0)
            return {inputCenter.x() + Layout::ioW / 2, inputCenter.y()};
        return point(std::max(0, line.parent), line.after);
    };
    auto inPort = [&](int l, int i) {
        const QPointF b = blockCenter(l, i);
        return QPointF(b.x() - halfWidth(c.lines[l].plugins[i]), b.y());
    };

    p.save();
    p.setClipRect(r.adjusted(1, 1, -1, -1));
    // Wires first, under the blocks.
    // A wire: straight along a row; an S-curve to a row below or above; by
    // the lane between rows when blocks are in the way (viaLane) or when it
    // has to go back left (a branch starts under the line it leaves).
    auto wire = [&](QPointF a, QPointF b, bool on, bool viaLane) {
        const QPointF A = at(a), B = at(b);
        const double rr = 18 * k;
        QPainterPath path(A);
        const bool back = B.x() < A.x() - 1;
        if (!viaLane && !back) {
            if (std::fabs(A.y() - B.y()) < 0.5) {
                path.lineTo(B);
            } else {
                const double dx = std::max(30 * k, (B.x() - A.x()) / 2);
                path.cubicTo(A + QPointF(dx, 0), B - QPointF(dx, 0), B);
            }
        } else {
            const double lane = A.y() + (B.y() >= A.y() ? 1 : -1) * Layout::rowPitch * k / 2;
            if (back) {
                path.cubicTo(QPointF(A.x() + 2 * rr, A.y()), QPointF(A.x() + 2 * rr, lane), QPointF(A.x() + rr, lane));
                path.lineTo(B.x() - rr, lane);
                path.cubicTo(QPointF(B.x() - 2 * rr, lane), QPointF(B.x() - 2 * rr, B.y()), B);
            } else {
                path.cubicTo(QPointF(A.x() + rr, A.y()), QPointF(A.x() + rr, lane), QPointF(A.x() + 2 * rr, lane));
                path.lineTo(B.x() - 2 * rr, lane);
                path.cubicTo(QPointF(B.x() - rr, lane), QPointF(B.x() - rr, B.y()), B);
            }
        }
        p.setPen(QPen(on ? color::green : color::border2, std::max(1.5, 2.4 * k)));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setPen(Qt::NoPen);
        p.setBrush(on ? color::green : color::muted);
        for (const QPointF& d : {A, B})
            p.drawEllipse(d, 3.2 * k, 3.2 * k);
    };
    for (int l = 0; l < c.lines.size(); ++l) {
        const LineInfo& line = c.lines[l];
        for (int i = 0; i < line.plugins.size(); ++i)
            wire(point(l, i), inPort(l, i), green(l, i), false);
    }
    for (int o = 0; o < c.taps.size(); ++o) {
        const TapInfo& t = c.taps[o];
        const QPointF out = L.center(L.cols - 1, L.outputRow[o]) - QPointF(Layout::ioW / 2, 0);
        const bool more = t.after < c.lines[t.line].plugins.size();
        wire(point(t.line, t.after), out, live && !silent(t.line, t.after), more);
    }

    // Then the blocks.
    auto ioBox = [&](QPointF center, const QString& name, const QString& sub, bool on) {
        const QRectF b(at(center - QPointF(Layout::ioW / 2, Layout::ioH / 2)), QSizeF(Layout::ioW * k, Layout::ioH * k));
        draw::card(p, b, 8 * k, color::panel2, on ? color::green : color::border2);
        p.setPen(color::text);
        p.setFont(typeface(Face::Text, 14 * k, QFont::DemiBold));
        draw::text(p, b.adjusted(4 * k, 6 * k, -4 * k, -b.height() / 2), Qt::AlignHCenter | Qt::AlignBottom, name);
        p.setPen(color::muted);
        p.setFont(typeface(Face::Mono, 11 * k));
        p.drawText(b.adjusted(0, b.height() / 2 + 2 * k, 0, 0), Qt::AlignHCenter | Qt::AlignTop, sub);
    };
    const int inputIndex = std::max(0, static_cast<int>(s.perf.inputs.indexOf(c.input)));
    ioBox(inputCenter, c.input, QString("in %1").arg(inputIndex + 1), true);
    for (int o = 0; o < c.taps.size(); ++o) {
        const int outputIndex = std::max(0, static_cast<int>(s.perf.outputs.indexOf(c.taps[o].output)));
        const bool muted = s.status.valid && s.status.muted;
        ioBox(L.center(L.cols - 1, L.outputRow[o]), c.taps[o].output, muted ? "muted" : QString("out %1").arg(outputIndex + 1), !muted);
    }
    for (int l = 0; l < c.lines.size(); ++l)
        for (int i = 0; i < c.lines[l].plugins.size(); ++i) {
            const PluginInfo& pi = c.lines[l].plugins[i];
            const QPointF center = at(blockCenter(l, i));
            const bool on = !bypassed(pi.slot), selected = pi.slot == slot_;
            const QString id = QString("block %1").arg(pi.slot);
            const QColor stroke = selected ? color::orange : on ? color::green : color::border2;
            const QColor fill = selected ? QColor(0x2A, 0x1A, 0x0A) : on ? color::greenBg : color::panel2;
            const double sw = selected ? std::max(2.0, 3 * k) : std::max(1.2, 1.6 * k);
            const Shape sh = shapeOf(pi);
            QRectF box;
            if (sh == Shape::Hexagon) {
                const double w = Layout::blockW * k, h = Layout::blockH * k, cut = 14 * k;
                box = QRectF(center.x() - w / 2, center.y() - h / 2, w, h);
                QPolygonF hex;
                hex << QPointF(box.left() + cut, box.top()) << QPointF(box.right() - cut, box.top()) << QPointF(box.right(), center.y())
                    << QPointF(box.right() - cut, box.bottom()) << QPointF(box.left() + cut, box.bottom()) << QPointF(box.left(), center.y());
                p.setPen(QPen(stroke, sw));
                p.setBrush(fill);
                p.drawPolygon(hex);
                // On or bypassed and the format, the name, then nothing yet where the time will go.
                const QString state = on ? pi.format.toUpper() : "bypassed";
                p.setFont(typeface(Face::Mono, 10.5 * k));
                const double tw = QFontMetricsF(p.font()).horizontalAdvance(state);
                const double dotX = center.x() - tw / 2 - 6 * k, ty = box.top() + 14 * k;
                p.setPen(Qt::NoPen);
                p.setBrush(on ? color::green : color::dim);
                p.drawEllipse(QPointF(dotX, ty), 3.4 * k, 3.4 * k);
                p.setPen(on ? color::text2 : color::muted);
                p.drawText(QRectF(dotX + 6 * k, ty - 8 * k, tw + 4 * k, 16 * k), Qt::AlignLeft | Qt::AlignVCenter, state);
                p.setPen(on ? color::text : color::muted);
                p.setFont(draw::fitted(Face::Text, QFont::DemiBold, pi.label, 13.5 * k, 8 * k, w - 2 * cut + 6 * k));
                p.drawText(QRectF(box.left() + cut - 4 * k, box.top() + 22 * k, w - 2 * cut + 8 * k, h - 30 * k),
                           Qt::AlignCenter | Qt::TextWordWrap, pi.label);
            } else {
                const double rad = (sh == Shape::GainStage ? 34 : 15) * k;
                box = QRectF(center.x() - rad, center.y() - rad, 2 * rad, 2 * rad);
                p.setPen(QPen(stroke, sw));
                p.setBrush(fill);
                p.drawEllipse(box);
                p.setPen(on ? color::green : color::muted);
                if (sh == Shape::GainStage) {
                    auto line = [&](double dy, const QString& text) {
                        p.drawText(QRectF(box.left(), center.y() + dy * rad - 8 * k, box.width(), 16 * k), Qt::AlignCenter, text);
                    };
                    p.setFont(typeface(Face::Text, 11 * k, QFont::Bold));
                    line(-0.3, "GAIN");
                    line(0.06, "STAGE");
                    p.setFont(typeface(Face::Text, 9.5 * k));
                    p.setPen(color::muted);
                    line(0.5, on ? "on" : "off");
                } else {
                    p.setFont(typeface(Face::Text, 9.5 * k, QFont::DemiBold));
                    p.drawText(box, Qt::AlignCenter, "Act");
                }
            }
            const QRectF tap = box.adjusted(-4, -4, 4, 4).intersected(r);
            if (!tap.isEmpty())
                hit(id, tap, [this, slot = pi.slot] { selectSlot(slot == slot_ ? -1 : slot); });
        }
    p.restore();

    // Zoom out, fit, zoom in, at the bottom right; how big it is beside them.
    const double bh = 38 * u, bw = 44 * u, gap = 6 * u, fitW = 56 * u;
    double x = r.right() - 10 * u - bw;
    const double y = r.bottom() - 10 * u - bh;
    button(p, "zoom in", QRectF(x, y, bw, bh), "+", [this] { zoomAt(canvasRect_.center(), 1.25); }, color::text, color::border2, color::panel2, 20 * u);
    x -= gap + fitW;
    button(p, "fit", QRectF(x, y, fitW, bh), "Fit", [this] { fit(); }, color::text, color::border2, color::panel2, 14 * u);
    x -= gap + bw;
    button(p, "zoom out", QRectF(x, y, bw, bh), "−", [this] { zoomAt(canvasRect_.center(), 1 / 1.25); }, color::text, color::border2, color::panel2,
           20 * u);
    p.setPen(color::muted);
    p.setFont(typeface(Face::Mono, 12 * u));
    p.drawText(QRectF(x - 70 * u, y, 62 * u, bh), Qt::AlignRight | Qt::AlignVCenter, QString("%1%").arg(std::lround(k / u * 100)));
}

void ChainView::legend(QPainter& p, const QRectF& r) {
    p.save();
    p.setFont(typeface(Face::Text, 13 * u));
    const double h = 18 * u;
    double x = r.left(), y = r.top() + 6 * u;
    auto item = [&](const QString& text, const std::function<void(const QRectF&)>& icon, double iconW) {
        const double w = iconW + 8 * u + QFontMetricsF(p.font()).horizontalAdvance(text) + 22 * u;
        if (x + w > r.right() && x > r.left()) {
            x = r.left();
            y += h + 12 * u;
        }
        icon(QRectF(x, y, iconW, h));
        p.setPen(color::muted);
        p.drawText(QRectF(x + iconW + 8 * u, y - 2 * u, w, h + 4 * u), Qt::AlignLeft | Qt::AlignVCenter, text);
        x += w;
    };
    auto hex = [&](const QRectF& b, const QColor& stroke, const QColor& fill) {
        const double cut = b.height() * 0.3;
        QPolygonF poly;
        poly << QPointF(b.left() + cut, b.top()) << QPointF(b.right() - cut, b.top()) << QPointF(b.right(), b.center().y())
             << QPointF(b.right() - cut, b.bottom()) << QPointF(b.left() + cut, b.bottom()) << QPointF(b.left(), b.center().y());
        p.setPen(QPen(stroke, 1.5));
        p.setBrush(fill);
        p.drawPolygon(poly);
    };
    item("inputs and outputs", [&](const QRectF& b) { draw::card(p, b, 4 * u, QColor(), color::green); }, 30 * u);
    item("Gain Stage", [&](const QRectF& b) {
        p.setPen(QPen(color::green, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(b.left(), b.top(), b.height(), b.height()));
    }, h);
    item("Act", [&](const QRectF& b) {
        p.setPen(QPen(color::green, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(b.left() + 3 * u, b.top() + 3 * u, b.height() - 6 * u, b.height() - 6 * u));
    }, h);
    item("plug-in on", [&](const QRectF& b) { hex(b, color::green, color::greenBg); }, 30 * u);
    item("bypassed", [&](const QRectF& b) { hex(b, color::border2, color::panel2); }, 30 * u);
    item("carries sound to an output", [&](const QRectF& b) {
        p.setPen(QPen(color::green, 2));
        p.drawLine(QPointF(b.left(), b.center().y()), QPointF(b.right(), b.center().y()));
    }, 30 * u);
    item("silent", [&](const QRectF& b) {
        p.setPen(QPen(color::border2, 2));
        p.drawLine(QPointF(b.left(), b.center().y()), QPointF(b.right(), b.center().y()));
    }, 30 * u);
    item("selected", [&](const QRectF& b) { draw::card(p, QRectF(b.left(), b.top() + 2 * u, b.height() - 4 * u, b.height() - 4 * u), 3 * u, color::orange, QColor()); },
         h);
    p.restore();
}

void ChainView::panel(QPainter& p, const QRectF& r, const ChainInfo& c) {
    const double pad = 18 * u;
    double y = r.top() + 20 * u;
    QFont label = typeface(Face::Text, 12 * u, QFont::DemiBold);
    label.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * u);
    p.setFont(label);
    p.setPen(color::muted);
    p.drawText(QRectF(r.left() + pad, y, r.width(), 18 * u), Qt::AlignLeft | Qt::AlignVCenter, "SELECTED BLOCK");
    y += 30 * u;
    const PluginInfo* pi = c.plugin(slot_);
    const double w = r.width() - 2 * pad;
    listRect_ = {};
    if (!pi) {
        p.setPen(color::muted);
        p.setFont(typeface(Face::Text, 15 * u));
        p.drawText(QRectF(r.left() + pad, y, w, 120 * u), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                   "Tap a plug-in to switch it on or off and see its parameters.");
        return;
    }
    p.setPen(color::text);
    p.setFont(draw::fitted(Face::Text, QFont::DemiBold, pi->label, 21 * u, 13 * u, w));
    draw::text(p, QRectF(r.left() + pad, y, w, 30 * u), Qt::AlignLeft | Qt::AlignVCenter, pi->label);
    y += 32 * u;
    const QString name = plugin_.value("name").toString();
    QString sub = pi->format.toUpper();
    if (pi->format == "vst3" && pi->path.contains("yabridge"))
        sub += " through yabridge";
    if (!name.isEmpty() && name != pi->label)
        sub = name + " · " + sub;
    p.setPen(color::muted);
    p.setFont(typeface(Face::Text, 14 * u));
    draw::text(p, QRectF(r.left() + pad, y, w, 22 * u), Qt::AlignLeft | Qt::AlignVCenter, sub);
    y += 34 * u;

    const bool bypassed = s.status.valid ? s.status.bypassed(c.index, slot_) : pi->bypassed;
    const QRectF bb(r.left() + pad, y, w, 50 * u);
    button(p, "bypass", bb, bypassed ? "Turn on" : "Bypass", [this, chain = c.index, slot = slot_, bypassed] {
               s.link.request({{"cmd", "bypass"}, {"chain", chain}, {"slot", slot}, {"bypassed", !bypassed}},
                              [this](const QJsonObject& r) { s.check(r, "Not switched"); });
           },
           bypassed ? color::green : color::text, bypassed ? color::greenDark : color::border2, color::panel2, 16 * u);
    y += 62 * u;
    // The plug-in's own editor, when it has one. It opens on the computer
    // running the engine, which on stage is this one.
    if (plugin_.value("editor").toBool()) {
        const bool open = plugin_.value("editor_open").toBool();
        const QRectF eb(r.left() + pad, y, w, 50 * u);
        button(p, "editor", eb, open ? "Close editor" : "Open editor",
               [this, chain = c.index, slot = slot_, open] {
                   s.link.request({{"cmd", "editor"}, {"chain", chain}, {"slot", slot}, {"open", !open}},
                                  [this](const QJsonObject& r) {
                                      if (s.check(r, "Could not open the editor"))
                                          requestPlugin();
                                  });
               },
               open ? color::onOrange : color::text, open ? color::orange : color::border2, open ? color::orange : color::panel2, 16 * u);
        y += 62 * u;
    }
    p.setPen(color::muted);
    p.setFont(typeface(Face::Text, 13 * u));
    draw::text(p, QRectF(r.left() + pad, y, w, 20 * u), Qt::AlignLeft | Qt::AlignVCenter,
               pi->muteOnBypass ? "When bypassed, it passes silence." : "When bypassed, it passes its input on.");
    y += 34 * u;

    p.setFont(label);
    p.setPen(color::muted);
    p.drawText(QRectF(r.left() + pad, y, w, 18 * u), Qt::AlignLeft | Qt::AlignVCenter, showAll_ ? "ALL PARAMETERS" : "PARAMETERS");
    y += 28 * u;

    const QJsonArray params = plugin_.value("params").toArray();
    if (search_ && !showAll_)
        search_->hide();
    if (plugin_.isEmpty() || params.isEmpty()) {
        p.setPen(color::muted);
        p.setFont(typeface(Face::Text, 14 * u));
        p.drawText(QRectF(r.left() + pad, y, w, r.bottom() - y), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                   plugin_.isEmpty() ? "Asking the plug-in…" : "This plug-in shows no parameters.");
        return;
    }
    const QRectF area(r.left(), y, r.width(), r.bottom() - y - 14 * u);
    if (showAll_)
        allParams(p, area, params, pi->format);
    else
        shownList(p, area, params, pi->format);
}

namespace {

QHash<int, QJsonObject> byId(const QJsonArray& params) {
    QHash<int, QJsonObject> out;
    for (const QJsonValue& v : params)
        out.insert(v.toObject().value("id").toInt(), v.toObject());
    return out;
}

// What moves a parameter from outside, as the panel says it.
QString tieText(const QJsonObject& q) {
    const QJsonArray tied = q.value("tied").toArray();
    if (tied.isEmpty())
        return {};
    const QJsonObject t = tied[0].toObject();
    if (t.contains("trigger"))
        return "set by " + t.value("trigger").toString();
    const QString control = t.value("control").toString(), follows = t.value("follows").toString();
    if (!follows.isEmpty())
        return "follows " + follows;
    if (t.value("pedal").toInt() > 0)
        return QString("%1, pedal %2").arg(control).arg(t.value("pedal").toInt());
    return "moved by " + control;
}

} // namespace

bool ChainView::tied(int id) const {
    for (const QJsonValue& v : plugin_.value("params").toArray())
        if (v.toObject().value("id").toInt() == id)
            return !v.toObject().value("tied").toArray().isEmpty();
    return false;
}

QVector<int> ChainView::shownParams() const {
    const QJsonArray params = plugin_.value("params").toArray();
    QSet<int> have;
    for (const QJsonValue& v : params)
        have.insert(v.toObject().value("id").toInt());
    QVector<int> ids;
    const QJsonValue shown = shownPending_ ? QJsonValue(pendingShown_) : plugin_.value("shown");
    if (shown.isArray()) {
        for (const QJsonValue& v : shown.toArray())
            if (have.contains(v.toInt()) && !ids.contains(v.toInt()))
                ids << v.toInt();
    } else {
        // Nothing picked yet: the first few, in the plug-in's order.
        for (int i = 0; i < params.size() && ids.size() < 8; ++i)
            ids << params[i].toObject().value("id").toInt();
    }
    // What a pedal or MIDI moves always shows.
    for (const QJsonValue& v : params) {
        const QJsonObject q = v.toObject();
        if (!q.value("tied").toArray().isEmpty() && !ids.contains(q.value("id").toInt()))
            ids << q.value("id").toInt();
    }
    return ids;
}

void ChainView::setShown(const QVector<int>& ids) {
    if (slot_ < 0 || chain_ < 0)
        return;
    pendingShown_ = {};
    for (int id : ids)
        pendingShown_.append(id);
    ++shownPending_;
    const int chain = chain_, slot = slot_;
    s.link.request({{"cmd", "shown"}, {"chain", chain}, {"slot", slot}, {"params", pendingShown_}}, [this, chain, slot](const QJsonObject& r) {
        if (chain != chain_ || slot != slot_)
            return;
        s.check(r, "The picks were not kept");
        if (--shownPending_ <= 0) {
            shownPending_ = 0;
            plugin_["shown"] = pendingShown_;
        }
        update();
    });
    update();
}

void ChainView::setShowAll(bool all) {
    if (all == showAll_)
        return;
    showAll_ = all;
    scroll_ = 0;
    dragRow_ = -1;
    if (search_ && !all) {
        search_->clear();
        search_->hide();
    }
    update();
}

// The list scrolls by a drag anywhere but on a slider or a grip (touch has
// no wheel), and by the wheel.
void ChainView::scrollable(const QRectF& list, double contentH) {
    listRect_ = list;
    scrollMax_ = std::max(0.0, contentH - list.height());
    scroll_ = std::clamp(scroll_, 0.0, scrollMax_);
    hit("params", list, {}, [this](QPointF pos, bool done) {
        if (scrollStartY_ < 0 || done) {
            scrollStartY_ = done ? -1 : pos.y();
            scrollFrom_ = scroll_;
            return;
        }
        scroll_ = std::clamp(scrollFrom_ - (pos.y() - scrollStartY_), 0.0, scrollMax_);
        update();
    });
}

// One parameter: its name (and what moves it, purple), its slider and its
// value. The row's first 40 units are left for a grip or a checkbox.
void ChainView::paramRow(QPainter& p, const QRectF& row, const QRectF& list, const QJsonObject& q, const QString& format, bool all) {
    Q_UNUSED(all);
    const double pad = 18 * u, lead = 40 * u, valueW = 58 * u;
    const double w = row.width() - 2 * pad - lead;
    const double left = row.left() + pad + lead, nameW = w * 0.4;
    const int id = q.value("id").toInt();
    const double lo = q.value("min").toDouble(), hi = q.value("max").toDouble(1);
    const double value = id == draggingParam_ ? draggingValue_ : q.value("value").toDouble(q.value("default").toDouble());
    const double f = hi > lo ? std::clamp((value - lo) / (hi - lo), 0.0, 1.0) : 0;
    QString text = q.value("text").toString();
    if (text.isEmpty() || id == draggingParam_)
        text = format == "vst3" ? QString("%1%").arg(std::lround(value * 100)) : QString::number(value, 'g', 3);
    // What moves it, purple, on a line of its own under the rest.
    const QString tie = tieText(q);
    const double cy = tie.isEmpty() ? row.center().y() : row.top() + row.height() * 0.38;
    p.setPen(color::text);
    p.setFont(typeface(Face::Text, 14 * u));
    draw::text(p, QRectF(left, cy - 12 * u, nameW - 8 * u, 24 * u), Qt::AlignLeft | Qt::AlignVCenter, q.value("name").toString());
    if (!tie.isEmpty()) {
        p.setPen(color::purple);
        p.setFont(typeface(Face::Text, 11.5 * u));
        draw::text(p, QRectF(left, cy + 9 * u, w, 18 * u), Qt::AlignLeft | Qt::AlignVCenter, tie);
    }
    const QRectF track(left + nameW, cy - 3 * u, w - nameW - valueW - 8 * u, 6 * u);
    p.setPen(Qt::NoPen);
    p.setBrush(color::line);
    p.drawRoundedRect(track, 3 * u, 3 * u);
    p.setBrush(tie.isEmpty() ? color::orange : color::purple);
    p.drawRoundedRect(QRectF(track.left(), track.top(), std::max(track.height(), track.width() * f), track.height()), 3 * u, 3 * u);
    p.setPen(color::text2);
    p.setFont(typeface(Face::Mono, 12 * u));
    draw::text(p, QRectF(track.right() + 8 * u, cy - 12 * u, valueW, 24 * u), Qt::AlignRight | Qt::AlignVCenter, text);
    // The slider takes the whole row's height, so a finger finds it.
    const QRectF grab(track.left() - 6 * u, row.top() + 4 * u, track.width() + 12 * u, row.height() - 8 * u);
    if (grab.intersects(list) && dragRow_ < 0)
        hit(QString("param %1").arg(id), grab.intersected(list), {}, [this, q, track, lo, hi](QPointF pos, bool done) {
            const double f = std::clamp((pos.x() - track.left()) / track.width(), 0.0, 1.0);
            setParam(q, lo + f * (hi - lo), done);
        });
}

// The picked parameters, each with a grip to move it up or down, then the
// way to all of them.
void ChainView::shownList(QPainter& p, const QRectF& area, const QJsonArray& params, const QString& format) {
    const double pad = 18 * u, btnH = 46 * u, rowH = 54 * u;
    const QRectF list(area.left(), area.top(), area.width(), area.height() - btnH - 12 * u);
    const QVector<int> ids = shownParams();
    const QHash<int, QJsonObject> all = byId(params);
    scrollable(list, ids.size() * rowH);
    if (ids.isEmpty()) {
        p.setPen(color::muted);
        p.setFont(typeface(Face::Text, 14 * u));
        p.drawText(list.adjusted(pad, 0, -pad, 0), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                   "None picked. Show all parameters and check the ones to see here.");
    }
    const int n = ids.size();
    auto targetOf = [this, list, rowH, n](double y) {
        return std::clamp(static_cast<int>(std::lround((y - dragGrab_ - list.top() + scroll_) / rowH)), 0, std::max(0, n - 1));
    };
    const int target = dragRow_ >= 0 && dragRow_ < n ? targetOf(dragY_) : -1;
    p.save();
    p.setClipRect(list);
    auto grip = [&](const QRectF& row, bool lifted) {
        p.setPen(QPen(lifted ? color::orange : color::dim, 2 * u, Qt::SolidLine, Qt::RoundCap));
        const double gx = row.left() + pad + 4 * u, gy = row.center().y();
        for (int k = -1; k <= 1; ++k)
            p.drawLine(QPointF(gx, gy + k * 6 * u), QPointF(gx + 18 * u, gy + k * 6 * u));
    };
    int place = 0;
    for (int i = 0; i < n; ++i) {
        if (i == dragRow_)
            continue;
        if (place == target)
            ++place;
        const QRectF row(area.left(), list.top() + place * rowH - scroll_, area.width(), rowH);
        ++place;
        if (row.bottom() < list.top() || row.top() > list.bottom())
            continue;
        paramRow(p, row, list, all.value(ids[i]), format, false);
        grip(row, false);
        // The grip moves the row: lift it, drag it to its place, let go.
        const QRectF gripRect = QRectF(row.left(), row.top(), pad + 40 * u, row.height()).intersected(list);
        if (!gripRect.isEmpty())
            hit(QString("move %1").arg(ids[i]), gripRect, {}, [this, i, row, ids, targetOf, list](QPointF pos, bool done) {
                if (dragRow_ < 0) {
                    dragRow_ = i;
                    dragGrab_ = pos.y() - row.top();
                    dragY_ = pos.y();
                    update();
                    return;
                }
                dragY_ = pos.y();
                // Near an end of the list, it scrolls.
                if (pos.y() < list.top() + 24 * u)
                    scroll_ = std::max(0.0, scroll_ - 10 * u);
                else if (pos.y() > list.bottom() - 24 * u)
                    scroll_ = std::min(scrollMax_, scroll_ + 10 * u);
                if (done) {
                    QVector<int> order = ids;
                    const int from = dragRow_, to = targetOf(dragY_);
                    dragRow_ = -1;
                    if (from != to) {
                        order.move(from, to);
                        setShown(order);
                    }
                }
                update();
            });
    }
    if (target >= 0) {
        // The row being moved, lifted, under the finger.
        const QRectF row(area.left(), dragY_ - dragGrab_, area.width(), rowH);
        draw::card(p, row.adjusted(8 * u, 2 * u, -8 * u, -2 * u), 10 * u, color::raised, color::orange);
        paramRow(p, row, list, all.value(ids[dragRow_]), format, false);
        grip(row, true);
    }
    p.restore();
    if (scrollMax_ > 0) {
        const double bh = list.height() * list.height() / (list.height() + scrollMax_);
        const double by = list.top() + (list.height() - bh) * scroll_ / scrollMax_;
        p.fillRect(QRectF(area.right() - 5 * u, by, 3 * u, bh), color::border2);
    }
    button(p, "show all", QRectF(area.left() + pad, area.bottom() - btnH, area.width() - 2 * pad, btnH),
           QString("Show all parameters (%1)").arg(params.size()), [this] { setShowAll(true); }, color::orange, color::border2, color::panel2, 15 * u);
}

// Every parameter in the plug-in's order, a checkmark on those shown;
// sections (a plug-in's own groups) fold, and a search narrows a long list.
void ChainView::allParams(QPainter& p, const QRectF& area, const QJsonArray& params, const QString& format) {
    const double pad = 18 * u, rowH = 54 * u, headH = 40 * u;
    double y = area.top();
    button(p, "collapse", QRectF(area.left() + pad, y, area.width() - 2 * pad, 44 * u), "Collapse all parameters", [this] { setShowAll(false); },
           color::orange, color::orange, color::panel2, 15 * u);
    y += 54 * u;
    QString filter;
    if (params.size() > 16) {
        if (!search_) {
            search_ = new QLineEdit(this);
            search_->setPlaceholderText("Search parameters");
            search_->setClearButtonEnabled(true);
            connect(search_, &QLineEdit::textChanged, this, [this] {
                scroll_ = 0;
                update();
            });
        }
        const QRect g = QRectF(area.left() + pad, y, area.width() - 2 * pad, 40 * u).toRect();
        if (search_->geometry() != g) {
            search_->setGeometry(g);
            search_->setFont(typeface(Face::Text, 15 * u));
            search_->setStyleSheet(QString("QLineEdit { background: %1; color: %2; border: 1px solid %3; border-radius: %4px; padding: 0 %5px; }"
                                           "QLineEdit:focus { border-color: %6; }")
                                       .arg(color::panel2.name(), color::text.name(), color::border2.name())
                                       .arg(std::lround(8 * u))
                                       .arg(std::lround(10 * u))
                                       .arg(color::orange.name()));
        }
        search_->show();
        filter = search_->text().trimmed();
        y += 50 * u;
    } else if (search_) {
        search_->hide();
    }
    const QRectF list(area.left(), y, area.width(), area.bottom() - y);
    const QVector<int> ids = shownParams();

    // Sections, when the plug-in has more than one.
    QSet<QString> modules;
    for (const QJsonValue& v : params)
        modules.insert(v.toObject().value("module").toString());
    const bool sections = modules.size() > 1;
    struct Item {
        QString section; // a header when q is empty
        QJsonObject q;
        int count = 0;
    };
    QVector<Item> items;
    auto folded = [&](const QString& m) {
        if (!filter.isEmpty() || closed_.contains(m))
            return !filter.isEmpty() ? false : true;
        if (opened_.contains(m) || params.size() <= 40)
            return false;
        // A long list starts folded, but for the sections with something shown.
        for (const QJsonValue& v : params)
            if (v.toObject().value("module").toString() == m && ids.contains(v.toObject().value("id").toInt()))
                return false;
        return true;
    };
    int header = -1;
    QString current;
    for (int i = 0; i < params.size(); ++i) {
        const QJsonObject q = params[i].toObject();
        const QString m = q.value("module").toString();
        if (!filter.isEmpty() && !q.value("name").toString().contains(filter, Qt::CaseInsensitive))
            continue;
        if (sections && (header < 0 || m != current)) {
            current = m;
            header = items.size();
            items.append({m, {}, 0});
        }
        if (header >= 0)
            ++items[header].count;
        if (!sections || !folded(m))
            items.append({m, q, 0});
    }
    double contentH = 0;
    for (const Item& it : items)
        contentH += it.q.isEmpty() ? headH : rowH;
    scrollable(list, contentH);
    if (items.isEmpty()) {
        p.setPen(color::muted);
        p.setFont(typeface(Face::Text, 14 * u));
        p.drawText(list.adjusted(pad, 0, -pad, 0), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, "No parameter is called that.");
    }
    p.save();
    p.setClipRect(list);
    double ry = list.top() - scroll_;
    for (const Item& it : items) {
        const double h = it.q.isEmpty() ? headH : rowH;
        const QRectF row(area.left(), ry, area.width(), h);
        ry += h;
        if (row.bottom() < list.top() || row.top() > list.bottom())
            continue;
        if (it.q.isEmpty()) {
            // A section: tap to fold or unfold it.
            const bool f = folded(it.section);
            const QString id = "section " + it.section;
            p.setPen(color::text2);
            p.setFont(typeface(Face::Text, 13 * u, QFont::DemiBold));
            const QString name = it.section.isEmpty() ? "Other" : it.section;
            draw::text(p, row.adjusted(pad + 22 * u, 0, -pad - 40 * u, 0), Qt::AlignLeft | Qt::AlignVCenter, name);
            p.setPen(color::muted);
            p.setFont(typeface(Face::Mono, 12 * u));
            p.drawText(row.adjusted(0, 0, -pad, 0), Qt::AlignRight | Qt::AlignVCenter, QString::number(it.count));
            // A triangle: right when folded, down when open.
            QPolygonF tri;
            const QPointF c(row.left() + pad + 7 * u, row.center().y());
            if (f)
                tri << c + QPointF(-3 * u, -6 * u) << c + QPointF(5 * u, 0) << c + QPointF(-3 * u, 6 * u);
            else
                tri << c + QPointF(-6 * u, -3 * u) << c + QPointF(6 * u, -3 * u) << c + QPointF(0, 5 * u);
            p.setPen(Qt::NoPen);
            p.setBrush(color::muted);
            p.drawPolygon(tri);
            p.setPen(QPen(color::line, 1));
            p.drawLine(QPointF(row.left() + pad, row.bottom() - 0.5), QPointF(row.right() - pad, row.bottom() - 0.5));
            const QRectF tap = row.intersected(list);
            if (!tap.isEmpty() && filter.isEmpty())
                hit(id, tap, [this, m = it.section, f] {
                    if (f) {
                        closed_.remove(m);
                        opened_.insert(m);
                    } else {
                        opened_.remove(m);
                        closed_.insert(m);
                    }
                    update();
                });
            continue;
        }
        const int pid = it.q.value("id").toInt();
        const bool on = ids.contains(pid), locked = !it.q.value("tied").toArray().isEmpty();
        paramRow(p, row, list, it.q, format, true);
        // The checkbox: orange when shown, purple and fixed when a pedal or MIDI moves it.
        const double bs = 22 * u;
        const QRectF box(row.left() + pad + 2 * u, row.center().y() - bs / 2, bs, bs);
        const QColor fill = locked ? color::purple : on ? color::orange : QColor();
        draw::card(p, box, 5 * u, fill, locked ? color::purple : on ? color::orange : color::border2);
        if (on || locked) {
            QPainterPath tick;
            tick.moveTo(box.left() + bs * 0.24, box.top() + bs * 0.52);
            tick.lineTo(box.left() + bs * 0.43, box.top() + bs * 0.7);
            tick.lineTo(box.left() + bs * 0.78, box.top() + bs * 0.32);
            p.setPen(QPen(locked ? color::purpleBg : color::onOrange, 2.4 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPath(tick);
        }
        const QRectF tap = QRectF(row.left(), row.top(), pad + 40 * u, row.height()).intersected(list);
        if (!tap.isEmpty())
            hit(QString("pick %1").arg(pid), tap, [this, pid, locked, name = it.q.value("name").toString()] {
                if (locked) {
                    emit s.message(name + " is moved by a pedal or MIDI, so it always shows", false);
                    return;
                }
                QVector<int> order = shownParams();
                if (order.contains(pid))
                    order.removeAll(pid);
                else
                    order.append(pid);
                setShown(order);
            });
    }
    p.restore();
    if (scrollMax_ > 0) {
        const double bh = list.height() * list.height() / (list.height() + scrollMax_);
        const double by = list.top() + (list.height() - bh) * scroll_ / scrollMax_;
        p.fillRect(QRectF(area.right() - 5 * u, by, 3 * u, bh), color::border2);
    }
}

} // namespace cue::gui
