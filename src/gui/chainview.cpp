#include "gui/chainview.h"

#include <QDateTime>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QJsonArray>
#include <QLineEdit>
#include <QLineF>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointingDevice>
#include <QRegularExpression>
#include <QStyleHints>
#include <QTransform>
#include <QTouchEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace cue::gui {

namespace {

const QColor kSilentWire{0x6B, 0x6B, 0x73};
const QColor kDot{0xA1, 0xA1, 0xAA};
constexpr double kOffDb = -96;

// A wire's level on its fader: 0 dB three quarters along, +12 dB at the
// end, and below 0 dB a fader's curve down to silent.
double levelToFader(double db) {
    if (db <= kOffDb)
        return 0;
    if (db >= 0)
        return 0.75 + std::min(db, 12.0) / 12.0 * 0.25;
    return 0.75 * std::pow(10.0, db / 60.0);
}

double faderToLevel(double f) {
    if (f >= 0.75)
        return (f - 0.75) / 0.25 * 12.0;
    if (f <= levelToFader(kOffDb + 0.1))
        return kOffDb;
    return 60.0 * std::log10(f / 0.75);
}

QString levelText(double db) {
    if (db <= kOffDb)
        return "off";
    const double r = std::round(db * 10) / 10;
    return QString("%1%2 dB").arg(r > 0 ? "+" : r < 0 ? "−" : "").arg(std::fabs(r), 0, 'f', 1);
}

// The panel says L30; the wire's tag, 30L.
QString panText(double pan, bool tag) {
    const long v = std::lround(std::fabs(pan));
    if (v == 0)
        return "C";
    const QString side = pan < 0 ? "L" : "R";
    return tag ? QString::number(v) + side : side + QString::number(v);
}

// The outputs a port's sound goes on to, by number.
QVector<int> reaches(const ChainInfo& c, const PortInfo& to) {
    QVector<int> out;
    QVector<bool> seen(c.blocks.size(), false);
    QVector<PortInfo> todo{to};
    while (!todo.isEmpty()) {
        const PortInfo p = todo.takeLast();
        if (p.kind == PortInfo::Kind::Output) {
            if (!out.contains(p.index))
                out << p.index;
            continue;
        }
        if (p.kind != PortInfo::Kind::Block || p.index < 0 || p.index >= seen.size() || seen[p.index])
            continue;
        seen[p.index] = true;
        for (const WireInfo& w : c.wires)
            if (w.from.kind == PortInfo::Kind::Block && w.from.index == p.index)
                todo << w.to;
    }
    std::sort(out.begin(), out.end());
    return out;
}

// A name the chain does not use yet: "Delay", else "Delay 2", ...
QString freeLabel(const ChainInfo& c, const QString& label) {
    auto taken = [&](const QString& l) {
        return std::any_of(c.blocks.begin(), c.blocks.end(), [&](const PluginInfo& b) { return b.label.compare(l, Qt::CaseInsensitive) == 0; });
    };
    if (!taken(label))
        return label;
    for (int n = 2;; ++n)
        if (!taken(QString("%1 %2").arg(label).arg(n)))
            return QString("%1 %2").arg(label).arg(n);
}

// What can be added, by section: the favourites the scan found, every
// plug-in the performance has, then the built-ins.
struct Choice {
    QString section;
    PluginInfo plugin;
    QString what; // under its name
};

QVector<Choice> choices(const AppState& s) {
    QVector<Choice> out;
    QSet<QString> seen;
    auto key = [](const PluginInfo& b) { return b.format + "|" + b.path + "|" + b.id + "|" + b.args.join(' '); };
    for (const ScannedPlugin& p : s.catalog.plugins)
        if (p.ready() && s.isFavourite(p)) {
            const PluginInfo b = p.toPlugin();
            seen.insert(key(b));
            out << Choice{"FAVOURITES", b, (p.vendor.isEmpty() ? QString() : p.vendor + " · ") + p.format.toUpper()};
        }
    for (const ChainInfo& c : s.perf.chains)
        for (const PluginInfo& b : c.blocks) {
            if (b.format == "builtin" || seen.contains(key(b)))
                continue;
            // Already a favourite under the scan's name.
            const bool favourite = std::any_of(s.catalog.plugins.begin(), s.catalog.plugins.end(),
                                               [&](const ScannedPlugin& p) { return s.isFavourite(p) && isPlugin(b, p); });
            if (favourite)
                continue;
            seen.insert(key(b));
            out << Choice{"IN THIS PERFORMANCE", b, b.format.toUpper() + " · " + b.path.section('/', -1)};
        }
    const QVector<QPair<QString, QStringList>> builtins = {
        {"Gain", {"gain", "1"}}, {"Drive", {"drive", "3"}}, {"Lowpass", {"lowpass", "2000"}}, {"Delay", {"delay", "400", "0.4", "0.4"}}};
    for (const auto& [label, args] : builtins) {
        PluginInfo b;
        b.format = "builtin";
        b.label = label;
        b.args = args;
        out << Choice{"BUILT IN", b, "built in · " + args.join(' ')};
    }
    return out;
}

void hexagon(QPainter& p, const QRectF& box, double cut) {
    const double cy = box.center().y();
    QPolygonF hex;
    hex << QPointF(box.left() + cut, box.top()) << QPointF(box.right() - cut, box.top()) << QPointF(box.right(), cy)
        << QPointF(box.right() - cut, box.bottom()) << QPointF(box.left() + cut, box.bottom()) << QPointF(box.left(), cy);
    p.drawPolygon(hex);
}

// The insert button: a + in a hexagon drawn as six strokes whose corners do
// not touch.
void insertMark(QPainter& p, const QRectF& b, const QColor& color, const QColor& bg) {
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawEllipse(b.center(), b.width() * 0.55, b.height() * 0.55);
    const double w = b.width(), h = b.height();
    auto pt = [&](double x, double y) { return QPointF(b.left() + x / 24 * w, b.top() + y / 22 * h); };
    p.setPen(QPen(color, std::max(1.2, w / 16), Qt::SolidLine, Qt::RoundCap));
    p.drawLine(pt(1.8, 9.4), pt(5.2, 3.1));
    p.drawLine(pt(7.8, 1.5), pt(16.2, 1.5));
    p.drawLine(pt(18.8, 3.1), pt(22.2, 9.4));
    p.drawLine(pt(22.2, 12.6), pt(18.8, 18.9));
    p.drawLine(pt(16.2, 20.5), pt(7.8, 20.5));
    p.drawLine(pt(5.2, 18.9), pt(1.8, 12.6));
    p.setPen(QPen(color, std::max(1.4, w / 13), Qt::SolidLine, Qt::RoundCap));
    p.drawLine(pt(12, 7.5), pt(12, 14.5));
    p.drawLine(pt(8.5, 11), pt(15.5, 11));
}

QJsonObject placeRequest(int chain, int slot, QPointF at) {
    return QJsonObject{{"cmd", "place"}, {"chain", chain}, {"slot", slot}, {"x", std::round(at.x() * 10) / 10}, {"y", std::round(at.y() * 10) / 10}};
}

QString newToken(int& counter) { return QString("app-%1-%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(++counter); }

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
        // The performance may have come back changed: what was selected may be gone.
        if (const ChainInfo* c = s.chain(chain_)) {
            if (slot_ >= c->numPlugins())
                selectSlot(-1);
            if (wireOn_ && selectedWire() < 0)
                wireOn_ = false;
        }
        update();
    });
    letGo_.setSingleShot(true);
    letGo_.setInterval(QGuiApplication::styleHints()->mouseDoubleClickInterval());
    connect(&letGo_, &QTimer::timeout, this, [this] { selectSlot(-1); });
    // Something undone: show where, with its values now.
    connect(&s, &AppState::undone, this, [this](int chain, int slot) {
        if (!isVisible() || !s.chain(chain))
            return;
        if (chain != chain_) {
            followPlaying_ = chain == s.playing();
            showChain(chain);
        }
        selectSlot(slot);
    });
    connect(&s, &AppState::undoChanged, this, [this] { update(); });
}

bool ChainView::doubleTap(const QString& id) {
    const qint64 now = clock_.elapsed();
    const bool twice = id == lastTapOn_ && now - lastTapOnAt_ < QGuiApplication::styleHints()->mouseDoubleClickInterval();
    lastTapOn_ = twice ? QString() : id;
    lastTapOnAt_ = now;
    return twice;
}

bool ChainView::bypassed(int chain, int slot) const {
    if (s.status.valid)
        return s.status.bypassed(chain, slot);
    const ChainInfo* c = s.chain(chain);
    const PluginInfo* pi = c ? c->plugin(slot) : nullptr;
    return pi && pi->bypassed;
}

void ChainView::setBypass(int chain, int slot, bool bypassed) {
    const ChainInfo* c = s.chain(chain);
    const PluginInfo* pi = c ? c->plugin(slot) : nullptr;
    auto request = [&](bool b) { return QJsonObject{{"cmd", "bypass"}, {"chain", chain}, {"slot", slot}, {"bypassed", b}}; };
    s.link.request(request(bypassed), [this](const QJsonObject& r) { s.check(r, "Not switched"); });
    s.edited({QString("%1 %2").arg(pi ? pi->label : "plug-in", bypassed ? "bypassed" : "on"), chain, slot, request(!bypassed),
              request(bypassed)});
}

void ChainView::showEvent(QShowEvent*) {
    if (std::exchange(resume_, false)) {
        refresh_.start();
        requestPlugin();
        return;
    }
    followPlaying_ = true;
    showChain(s.playing());
    refresh_.start();
    requestPlugin();
}

void ChainView::hideEvent(QHideEvent*) { refresh_.stop(); }

void ChainView::showChain(int chain) {
    // Another than the one playing stays shown when the playing one changes.
    followPlaying_ = chain == s.playing();
    if (chain != chain_) {
        commitEntry();
        closeMenu();
        chain_ = chain;
        selectSlot(-1);
        fit();
    }
    update();
}

void ChainView::selectSlot(int slot) {
    if (slot >= 0) {
        wireOn_ = false;
        picker_ = Picker::None;
    }
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
    // Where a drag started, to undo it as one change.
    if (draggingParam_ != id)
        draggingFrom_ = param.value("value").toDouble(param.value("default").toDouble());
    if (done && value != draggingFrom_) {
        const ChainInfo* c = s.chain(chain_);
        const PluginInfo* pi = c ? c->plugin(slot_) : nullptr;
        auto request = [&](double v) {
            return QJsonObject{{"cmd", "param"}, {"chain", chain_}, {"slot", slot_}, {"param", id}, {"value", v}};
        };
        s.edited({QString("%1: %2").arg(pi ? pi->label : "plug-in", param.value("name").toString()), chain_, slot_, request(draggingFrom_),
                  request(value)});
    }
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
    if (menu_) {
        menuScroll_ = std::clamp(menuScroll_ - e->angleDelta().y() / 120.0 * 42 * u, 0.0, menuMax_);
        update();
    } else if (listRect_.contains(e->position())) {
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
    if (e->type() == QEvent::TouchBegin) {
        // A finger on a line being typed in is the line's, as a mouse.
        auto* te = static_cast<QTouchEvent*>(e);
        if (!te->points().isEmpty() && qobject_cast<QLineEdit*>(childAt(te->points().first().position().toPoint()))) {
            e->ignore();
            return false;
        }
    }
    // A tap anywhere else keeps what was typed.
    if ((e->type() == QEvent::MouseButtonPress || e->type() == QEvent::TouchBegin) && !entryFor_.isEmpty())
        commitEntry();
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
    entryPlaced_ = false;
    QString subtitle;
    if (const ChainInfo* c = s.chain(chain_); c && c->scene < perf.scenes.size())
        subtitle = "Scene: " + perf.scenes[c->scene].name;
    const double top = header(p, perf.valid ? perf.name : "Cue", subtitle, enginePills(),
                              {{"undo", "Undo", [this] { s.undo(); }, s.canUndo() ? color::text : color::dim},
                               {"redo", "Redo", [this] { s.redo(); }, s.canRedo() ? color::text : color::dim},
                               {"stage", "Stage", [this] { emit navigate("stage"); }},
                               {"tuner", "Tuner", [this] { emit navigate("tuner"); }, color::green, color::greenDark, color::panel},
                               {"plugins", "Plug-ins", [this] { emit navigate("plugins"); }},
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
        drawMenu(p);
        return;
    }
    // Title, then the canvas, then the chain's settings.
    p.setPen(color::text);
    p.setFont(typeface(Face::Display, 30 * u, QFont::DemiBold));
    const QString title = "Chain: " + c->name;
    const double titleW = std::min(mid.width() * 0.6, QFontMetricsF(p.font()).horizontalAdvance(title) + 2);
    draw::text(p, QRectF(mid.left(), mid.top(), titleW, 40 * u), Qt::AlignLeft | Qt::AlignVCenter, title);
    p.setPen(color::muted);
    p.setFont(typeface(Face::Text, 14 * u));
    const int n = c->numPlugins(), outs = c->outputsUsed().size();
    QString what = QString("%1 plug-in%2 · %3 output%4").arg(n).arg(n == 1 ? "" : "s").arg(outs).arg(outs == 1 ? "" : "s");
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
    const double stripH = stripHeight(*c, mid.width());
    canvas(p, QRectF(mid.left(), mid.top() + 54 * u, mid.width(), mid.height() - 54 * u - stripH - 14 * u), *c);
    settingsStrip(p, QRectF(mid.left(), mid.bottom() - stripH, mid.width(), stripH), *c);
    panel(p, QRectF(W - panelW, top, panelW, H - top), *c);
    // A menu goes over everything; a line being typed in whose field is no
    // longer drawn goes.
    drawMenu(p);
    if (!entryFor_.isEmpty() && !entryPlaced_)
        QTimer::singleShot(0, this, [this] {
            if (!entryPlaced_)
                endEntry();
        });
}

QPointF ChainView::toScreen(QPointF board) const { return origin(scaleNow()) + (board - boardTopLeft_) * scaleNow(); }

QPointF ChainView::toBoard(QPointF screen) const { return (screen - origin(scaleNow())) / scaleNow() + boardTopLeft_; }

void ChainView::canvas(QPainter& p, const QRectF& r, const ChainInfo& c) {
    draw::card(p, r, 14 * u, color::panel, color::line);
    const int numOutputs = s.perf.outputs.size();
    const Board B(c, numOutputs, moving_);
    // Fitted, the chain fills the canvas but is never larger than the
    // design; the zoom and pan go on top. While something is dragged the
    // board keeps its size, so it stays still under the finger.
    canvasRect_ = r;
    if (dragBlock_ < 0 && !wiring_) {
        layoutSize_ = B.bounds.size();
        boardTopLeft_ = B.bounds.topLeft();
        fitK_ = std::min({u, (r.width() - 8) / layoutSize_.width(), (r.height() - 8) / layoutSize_.height()});
    }
    clampPan();
    const double k = scaleNow();
    const QPointF origin = this->origin(k);
    auto at = [&](QPointF pt) { return origin + (pt - boardTopLeft_) * k; };
    QTransform screen;
    screen.translate(origin.x(), origin.y());
    screen.scale(k, k);
    screen.translate(-boardTopLeft_.x(), -boardTopLeft_.y());

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

    // Open space: a drag pans; a tap selects the wire under it, or lets go
    // of what is selected; a double tap fits the chain again.
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
        for (int i = wireShapes_.size() - 1; i >= 0; --i)
            if (wireShapes_[i].contains(pos)) {
                selectWire(i);
                lastTapAt_ = -1000;
                return;
            }
        deselect();
        const qint64 now = clock_.elapsed();
        if (now - lastTapAt_ < 400 && QLineF(pos, lastTapPos_).length() < 30 * u) {
            fit();
            lastTapAt_ = -1000;
        } else {
            lastTapAt_ = now;
            lastTapPos_ = pos;
        }
    });

    const Flow flow = flowOf(c, [&](int slot) { return bypassed(c.index, slot); }, s.status.valid && s.status.muted, numOutputs);
    const int selWire = selectedWire();
    p.save();
    p.setClipRect(r.adjusted(1, 1, -1, -1));

    // Wires first, under the blocks: green where sound would pass if the
    // chain played, grey where it would not, orange when selected; thick
    // when its level, width or pan is changed.
    wireShapes_.clear();
    QPainterPathStroker stroker;
    stroker.setWidth(std::max(16 * u, 10 * k));
    stroker.setCapStyle(Qt::RoundCap);
    struct Tag {
        int wire;
        QPointF mid;
        double room;
        QColor stroke;
    };
    QVector<Tag> tags;
    for (int i = 0; i < c.wires.size(); ++i) {
        const WireInfo w = wireNow(c, i);
        const QPainterPath path = screen.map(B.path(w));
        wireShapes_ << stroker.createStroke(path);
        const bool selected = i == selWire;
        const QColor col = selected ? color::orange : flow.wires.value(i) ? color::green : kSilentWire;
        p.setPen(QPen(col, w.neutral() ? std::max(1.5, 2.2 * k) : std::max(3.0, 4.4 * k), Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        if (selected || (!w.neutral() && s.settings.wireTags))
            tags << Tag{i, path.pointAtPercent(0.5), path.length(), col};
    }

    // Then the blocks. A plug-in that the input does not reach, or no
    // output hears, is loaded but not processed: grey, "not connected".
    ports_.clear();
    for (int b = 0; b < c.blocks.size(); ++b) {
        const PluginInfo& pi = c.blocks[b];
        const QPointF center = at(B.block[b]);
        const bool connected = flow.played.value(b);
        const bool on = !bypassed(c.index, b), selected = b == slot_ && !wireOn_ && picker_ == Picker::None;
        const bool lit = on && connected;
        const QString id = QString("block %1").arg(b);
        const QColor stroke = selected ? color::orange : lit ? color::green : color::border2;
        const QColor fill = selected ? QColor(0x2A, 0x1A, 0x0A) : lit ? color::greenBg : color::panel2;
        const double sw = selected ? std::max(2.0, 3 * k) : std::max(1.2, 1.6 * k);
        const Shape sh = shapeOf(pi);
        const QRectF box(at(B.box(b).topLeft()), B.box(b).size() * k);
        if (sh == Shape::Hexagon) {
            const double w = box.width(), h = box.height(), cut = 14 * k;
            p.setPen(QPen(stroke, sw));
            p.setBrush(fill);
            hexagon(p, box, cut);
            // On, bypassed or not connected and the format, then the name.
            const QString state = !connected ? "not connected" : on ? pi.format.toUpper() : "bypassed";
            p.setFont(draw::fitted(Face::Mono, QFont::Normal, state, 10.5 * k, 6 * k, w - 2 * cut - 6 * k));
            const double tw = QFontMetricsF(p.font()).horizontalAdvance(state);
            const double dotX = center.x() - tw / 2 - 6 * k, ty = box.top() + 14 * k;
            p.setPen(Qt::NoPen);
            p.setBrush(lit ? color::green : color::dim);
            p.drawEllipse(QPointF(dotX, ty), 3.4 * k, 3.4 * k);
            p.setPen(lit ? color::text2 : color::muted);
            p.drawText(QRectF(dotX + 6 * k, ty - 8 * k, tw + 4 * k, 16 * k), Qt::AlignLeft | Qt::AlignVCenter, state);
            p.setPen(lit ? color::text : color::muted);
            p.setFont(draw::fitted(Face::Text, QFont::DemiBold, pi.label, 13.5 * k, 8 * k, w - 2 * cut + 6 * k));
            p.drawText(QRectF(box.left() + cut - 4 * k, box.top() + 22 * k, w - 2 * cut + 8 * k, h - 30 * k), Qt::AlignCenter | Qt::TextWordWrap,
                       pi.label);
        } else {
            const double rad = box.width() / 2;
            p.setPen(QPen(stroke, sw));
            p.setBrush(fill);
            p.drawEllipse(box);
            p.setPen(lit ? color::green : color::muted);
            if (sh == Shape::GainStage) {
                auto line = [&](double dy, const QString& text) {
                    p.drawText(QRectF(box.left(), center.y() + dy * rad - 8 * k, box.width(), 16 * k), Qt::AlignCenter, text);
                };
                p.setFont(typeface(Face::Text, 11 * k, QFont::Bold));
                line(-0.3, "GAIN");
                line(0.06, "STAGE");
                p.setFont(typeface(Face::Text, 9.5 * k));
                p.setPen(color::muted);
                line(0.5, !connected ? "unwired" : on ? "on" : "off");
            } else {
                p.setFont(typeface(Face::Text, 9.5 * k, QFont::DemiBold));
                p.drawText(box, Qt::AlignCenter, "Act");
            }
        }
        // A tap selects it (or lets it go), a double tap switches it on or
        // off; a drag moves it.
        const QRectF tap = box.adjusted(-4, -4, 4, 4).intersected(r);
        if (!tap.isEmpty())
            hit(id, tap, {}, [this, b](QPointF pos, bool done) { dragBlock(b, pos, done); });
    }

    // The input and the outputs: green while sound would reach them.
    auto ioBox = [&](QPointF center, const QString& name, const QString& sub, bool on) {
        const QRectF b(at(center - QPointF(Board::ioW / 2, Board::ioH / 2)), QSizeF(Board::ioW * k, Board::ioH * k));
        draw::card(p, b, 8 * k, on ? color::greenBg : color::panel2, on ? color::green : color::border2);
        p.setPen(on ? color::text : color::muted);
        p.setFont(draw::fitted(Face::Text, QFont::DemiBold, name, 14 * k, 8 * k, b.width() - 8 * k));
        draw::text(p, b.adjusted(4 * k, 6 * k, -4 * k, -b.height() / 2), Qt::AlignHCenter | Qt::AlignBottom, name);
        p.setPen(color::muted);
        p.setFont(typeface(Face::Mono, 11 * k));
        p.drawText(b.adjusted(0, b.height() / 2 + 2 * k, 0, 0), Qt::AlignHCenter | Qt::AlignTop, sub);
    };
    const bool muted = s.status.valid && s.status.muted;
    const int inputIndex = std::max(0, static_cast<int>(s.perf.inputs.indexOf(c.input)));
    ioBox(B.input, c.input, muted ? "muted" : QString("in %1").arg(inputIndex + 1), !muted);
    for (int o = 0; o < numOutputs; ++o)
        ioBox(B.output[o], s.perf.outputs[o], muted ? "muted" : QString("out %1").arg(o + 1), flow.heard.value(o));

    // The dots: where wires leave (right) and arrive (left), and a
    // plug-in's key, a small triangle. A drag from one draws a wire.
    const double dotR = std::max(3.0, 4.6 * k), grab = std::max(26 * u, 22 * k);
    auto dot = [&](const PortInfo& port, bool input, const QString& id) {
        const QPointF d = at(input ? B.inPoint(port) : B.outPoint(port));
        const QRectF spot(d.x() - grab / 2, d.y() - grab / 2, grab, grab);
        if (port.key) {
            const double t = dotR * 2;
            QPolygonF tri;
            tri << QPointF(d.x(), d.y() - t / 2) << QPointF(d.x() + t * 0.85, d.y()) << QPointF(d.x(), d.y() + t / 2);
            p.setPen(QPen(kDot, std::max(1.0, 1.4 * k), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(color::panel);
            p.drawPolygon(tri);
        } else {
            p.setPen(QPen(color::panel, std::max(1.0, 2 * k)));
            p.setBrush(kDot);
            p.drawEllipse(d, dotR, dotR);
        }
        ports_ << PortSpot{port, input, spot};
        if (spot.intersects(r))
            hit(id, spot.intersected(r), {}, [this, port, input](QPointF pos, bool done) { dragWire(port, !input, pos, done); });
    };
    dot(PortInfo::input(), false, "port input");
    for (int b = 0; b < c.blocks.size(); ++b) {
        dot(PortInfo::block(b), true, QString("port in %1").arg(b));
        dot(PortInfo::block(b), false, QString("port out %1").arg(b));
        if (c.blocks[b].hasKey)
            dot(PortInfo::block(b, true), true, QString("port key %1").arg(b));
    }
    for (int o = 0; o < numOutputs; ++o)
        dot(PortInfo::output(o), true, QString("port output %1").arg(o));

    // A wire being drawn, to the finger, and what it would join.
    if (wiring_) {
        const QPointF a = at(wiringSource_ ? B.outPoint(wiringPort_) : B.inPoint(wiringPort_));
        PortInfo target;
        const bool over = portAt(wiringAt_, wiringSource_, target);
        QPointF b = wiringAt_;
        if (over)
            b = at(wiringSource_ ? B.inPoint(target) : B.outPoint(target));
        const QPainterPath path = wiringSource_ ? B.path(toBoard(a), toBoard(b)) : B.path(toBoard(b), toBoard(a));
        p.setPen(QPen(color::orange, std::max(2.0, 3 * k), Qt::DashLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawPath(screen.map(path));
        if (over) {
            p.setPen(QPen(color::orange, std::max(1.5, 2 * k)));
            p.drawEllipse(b, dotR * 2.2, dotR * 2.2);
        }
    }

    // On top, the tags: what is changed on a wire, as room allows (level,
    // then pan, then width); and on the selected wire, the + that puts a
    // plug-in on it.
    for (const Tag& t : tags) {
        const WireInfo w = wireNow(c, t.wire);
        const bool selected = t.wire == selWire;
        QString text;
        bool above = false;
        if (!w.neutral() && s.settings.wireTags) {
            p.setFont(typeface(Face::Mono, std::max(8.0, 10.5 * k), QFont::Medium));
            const QString level = w.muted ? "muted" : w.levelDb != 0 ? levelText(w.levelDb) : QString();
            const QString width = !w.muted && w.width != 100 ? QString("W %1%").arg(std::lround(w.width)) : QString();
            const QString pan = !w.muted && w.pan != 0 ? panText(w.pan, true) : QString();
            const QFontMetricsF fm(p.font());
            for (const QStringList& tryParts : {QStringList{level, width, pan}, QStringList{level, pan}, QStringList{level}}) {
                QStringList kept;
                for (const QString& part : tryParts)
                    if (!part.isEmpty())
                        kept << part;
                const QString candidate = kept.join("  ");
                if (!candidate.isEmpty() && fm.horizontalAdvance(candidate) + 16 * k < t.room - 24 * k) {
                    text = candidate;
                    break;
                }
            }
            // Too short a wire for any of it: all of it above the wire,
            // clear of the blocks it joins.
            if (text.isEmpty()) {
                QStringList kept;
                for (const QString& part : {level, width, pan})
                    if (!part.isEmpty())
                        kept << part;
                text = kept.join("  ");
                above = true;
            }
        }
        const double th = std::max(14.0, 17 * k);
        double right = t.mid.x();
        if (!text.isEmpty()) {
            const double tw = QFontMetricsF(p.font()).horizontalAdvance(text) + 14 * k;
            const double ty = above ? t.mid.y() - (Board::blockH / 2 + 8) * k - th / 2 : t.mid.y();
            const QRectF tag(t.mid.x() - tw / 2, ty - th / 2, tw, th);
            if (above) {
                p.setPen(QPen(t.stroke, std::max(1.0, 1.2 * k)));
                p.drawLine(QPointF(t.mid.x(), tag.bottom()), t.mid);
            }
            draw::card(p, tag, th / 2, color::panel, t.stroke);
            p.setPen(selected ? color::orangeLight : QColor(0xE4, 0xE4, 0xE7));
            p.drawText(tag, Qt::AlignCenter, text);
            hit(QString("tag %1").arg(t.wire), tag.intersected(r), [this, i = t.wire] { selectWire(i); });
            right = above ? t.mid.x() : tag.right() + 6 * k;
        }
        if (selected) {
            const double bw = std::max(26 * u, 24 * k), bh = bw * 22 / 24;
            const QRectF plus(text.isEmpty() || above ? t.mid.x() - bw / 2 : right, t.mid.y() - bh / 2, bw, bh);
            insertMark(p, plus, color::orange, color::panel);
            hit("insert", plus.adjusted(-4, -4, 4, 4).intersected(r), [this] {
                picker_ = Picker::Insert;
                scroll_ = 0;
                update();
            });
        }
    }
    p.restore();

    // A plug-in, at the bottom left; zoom out, fit, zoom in, at the bottom
    // right, and how big it is beside them.
    const double bh = 38 * u, bw = 44 * u, gap = 6 * u, fitW = 56 * u;
    button(p, "add", QRectF(r.left() + 10 * u, r.bottom() - 10 * u - bh, 120 * u, bh), "+ Plug-in",
           [this] {
               selectSlot(-1);
               wireOn_ = false;
               picker_ = Picker::Add;
               addAt_ = toBoard(canvasRect_.center());
               scroll_ = 0;
               update();
           },
           picker_ == Picker::Add ? color::onOrange : color::text, picker_ == Picker::Add ? color::orange : color::border2,
           picker_ == Picker::Add ? color::orange : color::panel2, 15 * u);
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
    p.setFont(typeface(Face::Text, 12 * u));
    const double h = 18 * u;
    double x = r.left(), y = r.top() + 6 * u;
    auto item = [&](const QString& text, const std::function<void(const QRectF&)>& icon, double iconW) {
        const double w = iconW + 7 * u + QFontMetricsF(p.font()).horizontalAdvance(text) + 18 * u;
        if (x + w > r.right() && x > r.left()) {
            x = r.left();
            y += h + 10 * u;
        }
        icon(QRectF(x, y, iconW, h));
        p.setPen(color::muted);
        p.drawText(QRectF(x + iconW + 7 * u, y - 2 * u, w, h + 4 * u), Qt::AlignLeft | Qt::AlignVCenter, text);
        x += w;
    };
    auto hex = [&](const QRectF& b, const QColor& stroke, const QColor& fill) {
        p.setPen(QPen(stroke, 1.5));
        p.setBrush(fill);
        hexagon(p, b, b.height() * 0.3);
    };
    auto wire = [&](const QColor& c, double w) {
        return [&p, c, w, this](const QRectF& b) {
            p.setPen(QPen(c, w * u, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(b.left() + 2 * u, b.center().y()), QPointF(b.right() - 2 * u, b.center().y()));
        };
    };
    item("inputs and outputs", [&](const QRectF& b) { draw::card(p, b, 4 * u, QColor(), color::green); }, 26 * u);
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
    item("plug-in on", [&](const QRectF& b) { hex(b, color::green, color::greenBg); }, 26 * u);
    item("bypassed or not connected", [&](const QRectF& b) { hex(b, color::border2, color::panel2); }, 26 * u);
    item("key (side-chain input)", [&](const QRectF& b) {
        const QPointF c = b.center();
        QPolygonF tri;
        tri << c + QPointF(-4 * u, -4.5 * u) << c + QPointF(4 * u, 0) << c + QPointF(-4 * u, 4.5 * u);
        p.setPen(QPen(kDot, 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(tri);
    }, 10 * u);
    item("carries sound", wire(color::green, 2), 26 * u);
    item("silent", wire(kSilentWire, 2), 26 * u);
    item("level, width or pan changed", wire(color::green, 4), 26 * u);
    item("selected", [&](const QRectF& b) { draw::card(p, QRectF(b.left(), b.top() + 2 * u, b.height() - 4 * u, b.height() - 4 * u), 3 * u, color::orange, QColor()); },
         h);
    p.restore();
}

int ChainView::selectedWire() const {
    const ChainInfo* c = s.chain(chain_);
    return wireOn_ && c ? c->wire(wireFrom_, wireTo_) : -1;
}

void ChainView::selectWire(int wire) {
    const ChainInfo* c = s.chain(chain_);
    letGo_.stop();
    selectSlot(-1);
    picker_ = Picker::None;
    gainDrag_ = -1;
    gainsNow_.reset();
    wireOn_ = c && wire >= 0 && wire < c->wires.size();
    if (wireOn_) {
        wireFrom_ = c->wires[wire].from;
        wireTo_ = c->wires[wire].to;
    }
    update();
}

bool ChainView::deselect() {
    // A menu open, or a line being typed in, goes first.
    if (menu_ || !entryFor_.isEmpty()) {
        closeMenu();
        entryCancelled_ = true;
        endEntry();
        return true;
    }
    const bool any = slot_ >= 0 || wireOn_ || picker_ != Picker::None;
    letGo_.stop();
    selectSlot(-1);
    wireOn_ = false;
    picker_ = Picker::None;
    update();
    return any;
}

bool ChainView::deleteSelected() {
    if (const int w = selectedWire(); w >= 0) {
        removeWire(w);
        return true;
    }
    if (slot_ >= 0 && s.chain(chain_) && slot_ < s.chain(chain_)->numPlugins()) {
        removePlugin(slot_);
        return true;
    }
    return false;
}

WireInfo ChainView::wireNow(const ChainInfo& c, int wire) const {
    WireInfo w = c.wires.value(wire);
    if (gainsNow_ && gainsNow_->from == w.from && gainsNow_->to == w.to && c.index == chain_)
        w = *gainsNow_;
    return w;
}

QString ChainView::portName(const ChainInfo& c, const PortInfo& port) const {
    switch (port.kind) {
    case PortInfo::Kind::Input: return c.input;
    case PortInfo::Kind::Block: break;
    case PortInfo::Kind::Output: return s.perf.outputs.value(port.index, "an output");
    }
    const PluginInfo* b = c.plugin(port.index);
    const QString name = b ? b->label : "a plug-in";
    return port.key ? name + " (key)" : name;
}

// The first change to a chain's routing puts every plug-in where it is
// drawn, so the board does not lay itself out again under the hand.
void ChainView::pin(const ChainInfo& c) {
    const Board B(c, s.perf.outputs.size(), moving_);
    for (int b = 0; b < c.numPlugins(); ++b)
        if (!c.blocks[b].placed)
            s.change(placeRequest(c.index, b, B.block[b]));
}

void ChainView::tapBlock(int slot) {
    const QString id = QString("block %1").arg(slot);
    if (doubleTap(id)) {
        letGo_.stop();
        selectSlot(slot);
        setBypass(chain_, slot, !bypassed(chain_, slot));
    } else if (slot == slot_ && !wireOn_ && picker_ == Picker::None) {
        letGo_.start();
    } else {
        letGo_.stop();
        selectSlot(slot);
    }
}

void ChainView::dragBlock(int slot, QPointF pos, bool done) {
    const ChainInfo* c = s.chain(chain_);
    if (!c || slot >= c->numPlugins()) {
        dragBlock_ = -1;
        return;
    }
    const Board B(*c, s.perf.outputs.size(), moving_);
    if (dragBlock_ != slot) {
        // Pressed: a tap or a move, it is not yet clear which.
        dragBlock_ = slot;
        dragMoved_ = false;
        dragStart_ = pos;
        dragFrom_ = B.block[slot];
        return;
    }
    if (!done) {
        if (!dragMoved_ && QLineF(pos, dragStart_).length() < 8 * u)
            return;
        dragMoved_ = true;
        QPointF to = dragFrom_ + (pos - dragStart_) / scaleNow();
        // Near the height of another plug-in or of the input it takes it,
        // so rows stay straight.
        double best = 10;
        auto snap = [&](double y) {
            if (std::fabs(y - to.y()) < best) {
                best = std::fabs(y - to.y());
                to.setY(y);
            }
        };
        for (int b = 0; b < B.block.size(); ++b)
            if (b != slot)
                snap(B.block[b].y());
        snap(B.input.y());
        moving_[slot] = to;
        update();
        return;
    }
    dragBlock_ = -1;
    if (pinching_) {
        moving_.remove(slot);
        update();
        return;
    }
    if (!dragMoved_) {
        moving_.remove(slot);
        tapBlock(slot);
        return;
    }
    const QPointF to = moving_.value(slot, dragFrom_);
    const int chain = c->index;
    pin(*c);
    const QString what = "Moved " + c->blocks[slot].label;
    const QJsonObject before = placeRequest(chain, slot, dragFrom_), after = placeRequest(chain, slot, to);
    s.change(after, [this, slot](const QJsonObject& r) {
        moving_.remove(slot);
        s.check(r, "Not moved");
        update();
    });
    s.edited({what, chain, slot, before, after});
}

void ChainView::dragWire(const PortInfo& port, bool source, QPointF pos, bool done) {
    if (!wiring_) {
        wiring_ = true;
        wiringPort_ = port;
        wiringSource_ = source;
        wiringAt_ = dragStart_ = pos;
        update();
        return;
    }
    wiringAt_ = pos;
    if (!done) {
        update();
        return;
    }
    wiring_ = false;
    PortInfo other;
    if (pinching_) {
        // Two fingers: no wire.
    } else if (QLineF(pos, dragStart_).length() < 8 * u) {
        // A tap on a plug-in's dot is a tap on the plug-in.
        if (port.kind == PortInfo::Kind::Block)
            tapBlock(port.index);
    } else if (portAt(pos, source, other)) {
        if (source)
            connectPorts(port, other);
        else
            connectPorts(other, port);
    }
    update();
}

// What a wire being drawn would join at pos: a dot of the kind wanted (an
// input, or where sound comes out), else a plug-in, the input or an output
// as a whole.
bool ChainView::portAt(QPointF pos, bool input, PortInfo& out) const {
    for (int i = ports_.size() - 1; i >= 0; --i)
        if (ports_[i].input == input && ports_[i].r.contains(pos) && !(ports_[i].port == wiringPort_)) {
            out = ports_[i].port;
            return true;
        }
    const ChainInfo* c = s.chain(chain_);
    if (!c)
        return false;
    const Board B(*c, s.perf.outputs.size(), moving_);
    const QPointF at = toBoard(pos);
    for (int b = 0; b < B.block.size(); ++b)
        if (B.box(b).adjusted(-6, -6, 6, 6).contains(at)) {
            out = PortInfo::block(b);
            return true;
        }
    if (input) {
        for (int o = 0; o < B.output.size(); ++o)
            if (B.ioBox(B.output[o]).contains(at)) {
                out = PortInfo::output(o);
                return true;
            }
    } else if (B.ioBox(B.input).contains(at)) {
        out = PortInfo::input();
        return true;
    }
    return false;
}

void ChainView::connectPorts(PortInfo from, PortInfo to) {
    const ChainInfo* c = s.chain(chain_);
    if (!c || from.kind == PortInfo::Kind::Output || to.kind == PortInfo::Kind::Input)
        return;
    if (from.kind == PortInfo::Kind::Block && to.kind == PortInfo::Kind::Block && from.index == to.index) {
        emit s.message("A plug-in cannot feed itself", true);
        return;
    }
    if (const int i = c->wire(from, to); i >= 0) {
        selectWire(i);
        return;
    }
    WireInfo w;
    w.from = from;
    w.to = to;
    const int chain = c->index;
    const QStringList outs = s.perf.outputs;
    const QJsonObject make = w.request(chain, outs), take = w.request(chain, outs, "unwire");
    const QString what = portName(*c, from) + " → " + portName(*c, to);
    pin(*c);
    s.change(make, [this, chain, from, to, what, make, take](const QJsonObject& r) {
        if (!s.check(r, "Not wired"))
            return;
        s.edited({"Wired " + what, chain, -1, take, make});
        if (chain == chain_) {
            selectSlot(-1);
            picker_ = Picker::None;
            wireOn_ = true;
            wireFrom_ = from;
            wireTo_ = to;
        }
        update();
    });
}

void ChainView::removeWire(int wire) {
    const ChainInfo* c = s.chain(chain_);
    if (!c || wire < 0 || wire >= c->wires.size())
        return;
    const WireInfo w = wireNow(*c, wire);
    const int chain = c->index;
    const QJsonObject take = w.request(chain, s.perf.outputs, "unwire"), make = w.request(chain, s.perf.outputs);
    const QString what = "the wire " + portName(*c, w.from) + " → " + portName(*c, w.to);
    pin(*c);
    wireOn_ = false;
    gainsNow_.reset();
    s.change(take, [this, chain, what, take, make](const QJsonObject& r) {
        if (s.check(r, "Not taken away"))
            s.edited({"Took away " + what, chain, -1, make, take});
    });
    update();
}

void ChainView::removePlugin(int slot) {
    const ChainInfo* c = s.chain(chain_);
    if (!c || slot < 0 || slot >= c->numPlugins())
        return;
    const int chain = c->index;
    const QString label = c->blocks[slot].label, token = newToken(token_);
    const QJsonObject take{{"cmd", "remove"}, {"chain", chain}, {"slot", slot}, {"token", token}};
    const QJsonObject back{{"cmd", "restore"}, {"token", token}};
    pin(*c);
    s.change(take, [this, chain, slot, label, take, back](const QJsonObject& r) {
        if (!s.check(r, "Not taken out"))
            return;
        if (chain == chain_ && slot_ == slot)
            selectSlot(-1);
        s.edited({"Took out " + label, chain, slot, back, take, -1});
        update();
    });
}

QString ChainView::addTarget() const {
    const ChainInfo* c = s.chain(chain_);
    if (!c)
        return {};
    if (const int w = selectedWire(); w >= 0)
        return "Insert on " + portName(*c, c->wires[w].from) + " → " + portName(*c, c->wires[w].to);
    return "Add to " + c->name;
}

void ChainView::addFromBrowser(const PluginInfo& what) {
    if (!s.chain(chain_))
        return;
    picker_ = selectedWire() >= 0 ? Picker::Insert : Picker::Add;
    if (picker_ == Picker::Add)
        addAt_ = toBoard(canvasRect_.center());
    addPlugin(what);
}

void ChainView::addPlugin(const PluginInfo& what) {
    const ChainInfo* c = s.chain(chain_);
    if (!c)
        return;
    const QStringList outs = s.perf.outputs;
    QJsonObject plugin{{"format", what.format}};
    if (what.format == "builtin") {
        plugin["args"] = QJsonArray::fromStringList(what.args);
    } else {
        plugin["path"] = what.path;
        if (!what.id.isEmpty())
            plugin["id"] = what.id;
    }
    const QString label = freeLabel(*c, what.label);
    QJsonObject req{{"cmd", "add"}, {"chain", c->index}, {"plugin", plugin}, {"label", label}};
    const int chain = c->index;
    const Board B(*c, outs.size(), moving_);
    pin(*c);
    // Anywhere: the free place nearest. On a wire: where what it feeds is
    // (or just after what feeds an output), and what is in the way on that
    // row moves along to make room.
    QPointF at = B.freeNear(addAt_);
    QJsonArray pushed, pushedBack;
    if (const int w = selectedWire(); picker_ == Picker::Insert && w >= 0) {
        const WireInfo& wire = c->wires[w];
        QJsonObject on{{"from", wire.from.json(outs)}, {"to", wire.to.json(outs)}};
        if (wire.to.key)
            on["key"] = true;
        req["on"] = on;
        if (wire.to.kind == PortInfo::Kind::Block)
            at = B.block[wire.to.index];
        else
            at = QPointF((wire.from.kind == PortInfo::Kind::Block ? B.block[wire.from.index].x() : B.input.x()) + Board::colPitch,
                         B.output.value(wire.to.index).y());
        const QRectF spot(at.x() - Board::blockW / 2, at.y() - Board::blockH / 2, Board::blockW, Board::blockH);
        bool taken = false;
        for (int b = 0; b < c->numPlugins(); ++b)
            taken = taken || B.box(b).intersects(spot);
        for (int b = 0; taken && b < c->numPlugins(); ++b)
            if (std::fabs(B.block[b].y() - at.y()) < Board::rowPitch / 2 && B.block[b].x() > at.x() - Board::colPitch / 2) {
                const QJsonObject move = placeRequest(chain, b, B.block[b] + QPointF(Board::colPitch, 0));
                s.change(move);
                pushed.append(move);
                pushedBack.append(placeRequest(chain, b, B.block[b]));
            }
    }
    req["x"] = std::round(at.x() * 10) / 10;
    req["y"] = std::round(at.y() * 10) / 10;
    picker_ = Picker::None;
    wireOn_ = false;
    s.change(req, [this, chain, label, pushed, pushedBack](const QJsonObject& r) {
        if (!s.check(r, "Not added"))
            return;
        const int slot = r.value("slot").toInt(-1);
        const QString token = newToken(token_);
        // Undone, it is taken out and what moved along goes back.
        QJsonArray undo{QJsonObject{{"cmd", "remove"}, {"chain", chain}, {"slot", slot}, {"token", token}}};
        QJsonArray redo{QJsonObject{{"cmd", "restore"}, {"token", token}}};
        for (const QJsonValue& v : pushedBack)
            undo.append(v);
        for (const QJsonValue& v : pushed)
            redo.append(v);
        s.edited({"Added " + label, chain, -1, {{"cmd", "batch"}, {"requests", undo}}, {{"cmd", "batch"}, {"requests", redo}}, slot});
        if (chain == chain_)
            selectSlot(slot);
    });
    update();
}

void ChainView::setGains(const WireInfo& w, bool done) {
    const ChainInfo* c = s.chain(chain_);
    if (!c || c->wire(w.from, w.to) < 0)
        return;
    gainsNow_ = w;
    const int chain = c->index;
    const QStringList outs = s.perf.outputs;
    const qint64 now = clock_.elapsed();
    if (done || now - lastWireSend_ >= 40) {
        lastWireSend_ = now;
        s.change(w.request(chain, outs), [this, done](const QJsonObject& r) {
            s.check(r, "The wire did not take it");
            if (done && gainDrag_ < 0)
                gainsNow_.reset();
            update();
        });
    }
    if (done) {
        const WireInfo& was = gainsFrom_;
        if (was.levelDb != w.levelDb || was.width != w.width || was.pan != w.pan || was.muted != w.muted) {
            const QString what = w.muted != was.muted ? (w.muted ? "Muted " : "Unmuted ") : was.levelDb != w.levelDb ? "Level of "
                                 : was.width != w.width                        ? "Width of "
                                                                                : "Pan of ";
            s.edited({what + portName(*c, w.from) + " → " + portName(*c, w.to), chain, -1, was.request(chain, outs), w.request(chain, outs)});
        }
    }
    update();
}

void ChainView::panel(QPainter& p, const QRectF& r, const ChainInfo& c) {
    if (picker_ != Picker::None) {
        if (search_)
            search_->hide();
        pickerPanel(p, r, c);
        return;
    }
    if (selectedWire() >= 0) {
        if (search_)
            search_->hide();
        listRect_ = {};
        wirePanel(p, r, c);
        return;
    }
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
        // How the board works, then what its shapes and colours mean.
        p.setPen(color::muted);
        p.setFont(typeface(Face::Text, 15 * u));
        const QRectF help(r.left() + pad, y, w, r.bottom() - y - 20 * u);
        QRectF used;
        p.drawText(help, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                   "Tap a plug-in to see its parameters; tap it twice to switch it on or off.\n\n"
                   "Drag a plug-in to move it. Drag from a dot to draw a wire: inputs are on the left, outputs on the right, and the "
                   "small triangle is a side-chain input (key).\n\n"
                   "Tap a wire to set its level, width and pan; its + puts a plug-in on it. + Plug-in adds one to the board.\n\n"
                   "Delete takes away what is selected, and undo puts it back.",
                   &used);
        y = used.bottom() + 26 * u;
        if (y + 120 * u < r.bottom()) {
            p.setFont(label);
            p.setPen(color::muted);
            p.drawText(QRectF(r.left() + pad, y, w, 18 * u), Qt::AlignLeft | Qt::AlignVCenter, "KEY");
            legend(p, QRectF(r.left() + pad, y + 24 * u, w, r.bottom() - y - 44 * u));
        }
        if (search_)
            search_->hide();
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

    const bool off = bypassed(c.index, slot_);
    const double half = (w - 10 * u) / 2;
    button(p, "bypass", QRectF(r.left() + pad, y, half, 50 * u), off ? "Turn on" : "Bypass",
           [this, chain = c.index, slot = slot_, off] { setBypass(chain, slot, !off); }, off ? color::green : color::text,
           off ? color::greenDark : color::border2, color::panel2, 16 * u);
    button(p, "remove", QRectF(r.right() - pad - half, y, half, 50 * u), "Remove", [this, slot = slot_] { removePlugin(slot); }, color::red,
           color::border2, color::panel2, 16 * u);
    y += 62 * u;
    // The plug-in's own editor, when it has one. It opens on the computer
    // running the engine, which on stage is this one.
    if (plugin_.value("editor").toBool()) {
        const bool open = plugin_.value("editor_open").toBool();
        const QRectF eb(r.left() + pad, y, w, 50 * u);
        button(p, "editor", eb, open ? "Close editor" : "Open editor",
               [this, chain = c.index, slot = slot_, open] {
                   QJsonObject req{{"cmd", "editor"}, {"chain", chain}, {"slot", slot}, {"open", !open}};
                   // This window, so the editor stays over it and leaves it the focus.
                   if (QGuiApplication::platformName() == "xcb") {
                       req["above"] = static_cast<double>(window()->winId());
                       req["display"] = QString::fromLocal8Bit(qgetenv("DISPLAY"));
                   }
                   s.link.request(req, [this](const QJsonObject& r) {
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

// The selected wire: where it goes, and its level, width and pan as
// faders, in signal order; then mute, insert and delete.
void ChainView::wirePanel(QPainter& p, const QRectF& r, const ChainInfo& c) {
    const double pad = 18 * u, w = r.width() - 2 * pad;
    const int i = selectedWire();
    const WireInfo wire = wireNow(c, i);
    double y = r.top() + 20 * u;
    QFont label = typeface(Face::Text, 12 * u, QFont::DemiBold);
    label.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * u);
    p.setFont(label);
    p.setPen(color::muted);
    p.drawText(QRectF(r.left() + pad, y, w, 18 * u), Qt::AlignLeft | Qt::AlignVCenter, "SELECTED WIRE");
    y += 30 * u;
    const QString title = portName(c, wire.from) + " → " + portName(c, wire.to);
    p.setPen(color::text);
    p.setFont(draw::fitted(Face::Text, QFont::DemiBold, title, 21 * u, 13 * u, w));
    draw::text(p, QRectF(r.left() + pad, y, w, 30 * u), Qt::AlignLeft | Qt::AlignVCenter, title);
    y += 32 * u;
    QStringList to;
    for (int o : reaches(c, wire.to))
        to << s.perf.outputs.value(o);
    QString sub = to.isEmpty() ? "Leads to no output" : "Leads to " + (to.size() > 1 ? to.mid(0, to.size() - 1).join(", ") + " and " + to.last() : to.first());
    if (wire.to.key)
        sub += " · side-chain";
    if (wire.muted)
        sub += " · muted";
    p.setPen(wire.muted ? color::orangeLight : color::muted);
    p.setFont(typeface(Face::Text, 14 * u));
    draw::text(p, QRectF(r.left() + pad, y, w, 22 * u), Qt::AlignLeft | Qt::AlignVCenter, sub);
    y += 36 * u;

    // A fader: a double tap sets it back; a drag (or a tap) moves it.
    enum { Level, Width, Pan };
    auto fader = [&](int which, const QString& name, double f, const QString& value, const QString& hint, double tick) {
        p.setFont(label);
        p.setPen(color::muted);
        p.drawText(QRectF(r.left() + pad, y, w, 18 * u), Qt::AlignLeft | Qt::AlignVCenter, name);
        y += 24 * u;
        const double valueW = 70 * u;
        const QRectF track(r.left() + pad + 10 * u, y + 14 * u - 3 * u, w - valueW - 20 * u, 6 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(color::line);
        p.drawRoundedRect(track, 3 * u, 3 * u);
        p.setBrush(color::orange);
        // Pan fills from the middle, the others from the left.
        const double from = which == Pan ? 0.5 : 0.0;
        const double a = track.left() + track.width() * std::min(from, f), b = track.left() + track.width() * std::max(from, f);
        p.drawRoundedRect(QRectF(a, track.top(), std::max(track.height(), b - a), track.height()), 3 * u, 3 * u);
        if (tick >= 0) {
            p.setPen(QPen(color::dim, 1));
            const double tx = track.left() + track.width() * tick;
            p.drawLine(QPointF(tx, track.top() - 7 * u), QPointF(tx, track.bottom() + 7 * u));
        }
        p.setPen(QPen(color::orange, 2 * u));
        p.setBrush(color::text);
        p.drawEllipse(QPointF(track.left() + track.width() * f, track.center().y()), 9 * u, 9 * u);
        p.setPen(color::text);
        p.setFont(typeface(Face::Mono, 13 * u));
        p.drawText(QRectF(track.right() + 8 * u, y, valueW - 4 * u, 28 * u), Qt::AlignRight | Qt::AlignVCenter, value);
        const QString id = QString("wire %1").arg(name.toLower());
        const QRectF grab(track.left() - 12 * u, y - 6 * u, track.width() + 24 * u, 40 * u);
        hit(id, grab, {}, [this, which, id, track](QPointF pos, bool done) {
            const ChainInfo* c = s.chain(chain_);
            const int i = selectedWire();
            if (!c || i < 0)
                return;
            WireInfo w = wireNow(*c, i);
            if (gainDrag_ != which) {
                // Pressed: where this change starts, and whether it is the second tap of two.
                gainDrag_ = which;
                gainsFrom_ = w;
                gainReset_ = doubleTap(id);
            }
            if (done)
                gainDrag_ = -1;
            const double f = std::clamp((pos.x() - track.left()) / track.width(), 0.0, 1.0);
            if (gainReset_) {
                if (which == Level)
                    w.levelDb = 0;
                else if (which == Width)
                    w.width = 100;
                else
                    w.pan = 0;
            } else if (which == Level) {
                const double db = faderToLevel(f);
                w.levelDb = std::fabs(db) < 0.25 ? 0 : db <= kOffDb ? kOffDb : std::round(db * 10) / 10;
            } else if (which == Width) {
                w.width = std::lround(f * 100);
            } else {
                const long pan = std::lround(f * 200 - 100);
                w.pan = std::abs(pan) < 3 ? 0 : pan;
            }
            setGains(w, done);
        });
        y += 40 * u;
        p.setPen(color::dim);
        p.setFont(typeface(Face::Text, 12 * u));
        const QRectF hr(r.left() + pad, y, w, 60 * u);
        const QRectF used = p.boundingRect(hr, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, hint);
        p.drawText(hr, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, hint);
        y += used.height() + 16 * u;
    };
    fader(Level, "LEVEL", levelToFader(wire.levelDb), levelText(wire.levelDb),
          "From silent to +12 dB. The tick is 0 dB; double-tap the fader to go back to it.", 0.75);
    fader(Width, "WIDTH", wire.width / 100, QString("%1%").arg(std::lround(wire.width)),
          "Comes before pan. 0% folds left and right into one mono sound, which pan can then place anywhere. Double-tap for 100%.", -1);
    fader(Pan, "PAN", (wire.pan + 100) / 200, panText(wire.pan, false),
          "Like a balance knob: it turns one side down and never moves sound across. Double-tap for centre.", 0.5);

    const double gap = 8 * u, bw = (w - 2 * gap) / 3, bh = 46 * u;
    button(p, "wire mute", QRectF(r.left() + pad, y, bw, bh), wire.muted ? "Unmute" : "Mute",
           [this] {
               const ChainInfo* c = s.chain(chain_);
               if (!c || selectedWire() < 0)
                   return;
               WireInfo w = wireNow(*c, selectedWire());
               gainsFrom_ = w;
               w.muted = !w.muted;
               setGains(w, true);
           },
           wire.muted ? color::onOrange : color::text, wire.muted ? color::orange : color::border2, wire.muted ? color::orange : color::panel2, 14 * u);
    button(p, "wire insert", QRectF(r.left() + pad + bw + gap, y, bw, bh), "Insert",
           [this] {
               picker_ = Picker::Insert;
               scroll_ = 0;
               update();
           },
           color::text, color::border2, color::panel2, 14 * u);
    button(p, "wire delete", QRectF(r.left() + pad + 2 * (bw + gap), y, bw, bh), "Delete", [this] { removeWire(selectedWire()); }, color::red,
           color::border2, color::panel2, 14 * u);
    y += bh + 16 * u;
    p.setPen(color::muted);
    p.setFont(typeface(Face::Text, 13 * u));
    p.drawText(QRectF(r.left() + pad, y, w, r.bottom() - y - 10 * u), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
               "Two wires from one dot are copies of the same sound, each with its own level, width and pan. Wires into one input add "
               "together. Delete takes the wire away.");
}

// What can be added, to the board or onto the selected wire: the plug-ins
// the performance has, then the built-ins.
void ChainView::pickerPanel(QPainter& p, const QRectF& r, const ChainInfo& c) {
    const double pad = 18 * u, w = r.width() - 2 * pad;
    double y = r.top() + 20 * u;
    QFont label = typeface(Face::Text, 12 * u, QFont::DemiBold);
    label.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * u);
    p.setFont(label);
    p.setPen(color::muted);
    const bool onWire = picker_ == Picker::Insert && selectedWire() >= 0;
    p.drawText(QRectF(r.left() + pad, y, w, 18 * u), Qt::AlignLeft | Qt::AlignVCenter, onWire ? "INSERT A PLUG-IN" : "ADD A PLUG-IN");
    y += 30 * u;
    QString sub = "It goes on the board unwired: drag from its dots to wire it.";
    if (onWire) {
        const WireInfo& wire = c.wires[selectedWire()];
        sub = "On the wire " + portName(c, wire.from) + " → " + portName(c, wire.to) + ".";
    }
    p.setPen(color::text2);
    p.setFont(typeface(Face::Text, 14 * u));
    const QRectF sr(r.left() + pad, y, w, 60 * u);
    const QRectF used = p.boundingRect(sr, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, sub);
    p.drawText(sr, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, sub);
    y += used.height() + 18 * u;

    const QVector<Choice> all = choices(s);
    const double rowH = 54 * u, headH = 34 * u, btnH = 46 * u, gap = 10 * u;
    const QRectF list(r.left(), y, r.width(), r.bottom() - y - 2 * btnH - gap - 24 * u);
    int sections = 0;
    for (int i = 0; i < all.size(); ++i)
        sections += i == 0 || all[i].section != all[i - 1].section;
    scrollable(list, all.size() * rowH + sections * headH);
    p.save();
    p.setClipRect(list);
    double ry = list.top() - scroll_;
    auto head = [&](const QString& text) {
        p.setFont(label);
        p.setPen(color::muted);
        p.drawText(QRectF(r.left() + pad, ry, w, headH), Qt::AlignLeft | Qt::AlignVCenter, text);
        ry += headH;
    };
    for (int i = 0; i < all.size(); ++i) {
        if (i == 0 || all[i].section != all[i - 1].section)
            head(all[i].section);
        const PluginInfo& b = all[i].plugin;
        const QRectF row(r.left() + pad, ry, w, rowH - 6 * u);
        ry += rowH;
        if (row.bottom() < list.top() || row.top() > list.bottom())
            continue;
        const QString id = QString("choice %1").arg(i);
        draw::card(p, row, 10 * u, isPressed(id) ? color::raised : color::panel2, color::border);
        p.setPen(color::text);
        p.setFont(typeface(Face::Text, 15 * u, QFont::DemiBold));
        draw::text(p, row.adjusted(14 * u, 4 * u, -14 * u, -row.height() / 2), Qt::AlignLeft | Qt::AlignBottom, b.label);
        p.setPen(color::muted);
        p.setFont(typeface(Face::Mono, 11.5 * u));
        draw::text(p, row.adjusted(14 * u, row.height() / 2 + 2 * u, -14 * u, -4 * u), Qt::AlignLeft | Qt::AlignTop, all[i].what);
        const QRectF tap = row.intersected(list);
        if (!tap.isEmpty())
            hit(id, tap, [this, b] { addPlugin(b); });
    }
    p.restore();
    // Everything the scan found, to pick from in the plug-ins view.
    button(p, "browse", QRectF(r.left() + pad, r.bottom() - 2 * btnH - gap - 14 * u, w, btnH), "Browse all plug-ins",
           [this] { emit navigate("plugins"); }, color::orange, color::border2, color::panel2, 15 * u);
    button(p, "cancel", QRectF(r.left() + pad, r.bottom() - btnH - 14 * u, w, btnH), "Cancel",
           [this] {
               picker_ = Picker::None;
               update();
           },
           color::text, color::border2, color::panel2, 15 * u);
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
    // The picks as they were, for undo: none yet shows the first few.
    QJsonArray before;
    const QJsonValue was = shownPending_ ? QJsonValue(pendingShown_) : plugin_.value("shown");
    if (was.isArray()) {
        before = was.toArray();
    } else {
        const QJsonArray params = plugin_.value("params").toArray();
        for (int i = 0; i < params.size() && i < 8; ++i)
            before.append(params[i].toObject().value("id").toInt());
    }
    pendingShown_ = {};
    for (int id : ids)
        pendingShown_.append(id);
    ++shownPending_;
    const int chain = chain_, slot = slot_;
    const ChainInfo* c = s.chain(chain);
    const PluginInfo* pi = c ? c->plugin(slot) : nullptr;
    auto request = [&](const QJsonArray& params) {
        return QJsonObject{{"cmd", "shown"}, {"chain", chain}, {"slot", slot}, {"params", params}};
    };
    s.edited({QString("%1: the parameters shown").arg(pi ? pi->label : "plug-in"), chain, slot, request(before), request(pendingShown_)});
    s.link.request(request(pendingShown_), [this, chain, slot](const QJsonObject& r) {
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
