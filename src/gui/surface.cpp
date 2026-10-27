#include "gui/surface.h"

#include <QFile>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace cue::gui {

Surface::Surface(AppState& state, QWidget* parent) : QWidget(parent), s(state) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(false);
}

QRectF Surface::hitRect(const QString& id) const {
    for (const Hit& h : hits_)
        if (h.id == id)
            return h.r;
    return {};
}

QStringList Surface::hitIds() const {
    QStringList out;
    for (const Hit& h : hits_)
        out << h.id;
    return out;
}

void Surface::hit(const QString& id, const QRectF& r, std::function<void()> tap, Drag drag) {
    hits_.append({id, r, std::move(tap), std::move(drag)});
}

void Surface::paintEvent(QPaintEvent*) {
    hits_.clear();
    u = std::clamp(std::min(width() / 1280.0, height() / 860.0), 0.4, 4.0) * s.settings.scale;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.fillRect(rect(), color::bg);
    headerHeight_ = 0;
    paint(p);
    if (!s.connected())
        offline(p);
}

// Without the engine: what was last shown stays, dimmed, under a note saying
// what the app is waiting for. The header still works.
void Surface::offline(QPainter& p) {
    const QRectF area(0, headerHeight_, width(), height() - headerHeight_);
    p.fillRect(area, QColor(10, 10, 11, 215));
    // When the app started the engine, the engine's last words say why it is not here.
    QStringList said;
    if (!s.engineLog.isEmpty()) {
        QFile f(s.engineLog);
        if (f.open(QIODevice::ReadOnly) && f.seek(std::min(s.engineLogFrom, f.size()))) {
            const QStringList lines = QString::fromLocal8Bit(f.readAll()).split('\n', Qt::SkipEmptyParts);
            // Its errors if it gave any, else its last lines.
            for (const QString& line : lines)
                if (said.size() < 3 && (line.startsWith("error") || line.startsWith("audio:") || line.startsWith("config:")))
                    said << line.trimmed();
            if (said.isEmpty())
                for (int i = std::max(0, static_cast<int>(lines.size()) - 3); i < lines.size(); ++i)
                    said << lines[i].trimmed();
        }
    }
    const double w = std::min(area.width() - 40 * u, 760 * u), h = (190 + (said.isEmpty() ? 0 : 30 + 22 * said.size())) * u;
    const QRectF r(area.center().x() - w / 2, area.center().y() - h / 2, w, h);
    draw::card(p, r, 16 * u, color::panel, color::orange);
    p.setPen(color::orange);
    p.setFont(typeface(Face::Display, 40 * u, QFont::Bold));
    p.drawText(r.adjusted(28 * u, 24 * u, -28 * u, 0), Qt::AlignLeft | Qt::AlignTop, "WAITING FOR CUE");
    p.setPen(color::text2);
    p.setFont(typeface(Face::Text, 16 * u));
    const QString text = s.engineLog.isEmpty()
                             ? QString("The engine is not answering at %1. Start it with cue run and a performance; the app connects on its "
                                       "own, and the sound never depends on the app.")
                                   .arg(s.link.socketPath())
                             : QString("The app started the engine. It answers once its plug-ins are loaded; what it says is in %1.")
                                   .arg(s.engineLog);
    p.drawText(QRectF(r.left() + 28 * u, r.top() + 84 * u, w - 56 * u, 80 * u), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
    p.setFont(typeface(Face::Mono, 13 * u));
    double y = r.top() + 176 * u;
    for (const QString& line : said) {
        const bool bad = line.contains("error") || line.contains("audio:") || line.contains("config:");
        p.setPen(bad ? color::red : color::muted);
        draw::text(p, QRectF(r.left() + 28 * u, y, w - 56 * u, 20 * u), Qt::AlignLeft | Qt::AlignVCenter, line);
        y += 22 * u;
    }
    hit("offline", area, {});
}

void Surface::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton)
        pointerPress(e->position());
}

void Surface::mouseMoveEvent(QMouseEvent* e) { pointerMove(e->position()); }

void Surface::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton)
        pointerRelease(e->position());
}

void Surface::pointerPress(QPointF pos) {
    lastPos_ = pos;
    // The last drawn is on top.
    for (int i = hits_.size() - 1; i >= 0; --i) {
        if (!hits_[i].r.contains(pos))
            continue;
        pressed_ = hits_[i].id;
        pressedInside_ = true;
        dragging_ = hits_[i].drag;
        if (dragging_)
            dragging_(pos, false);
        update();
        return;
    }
}

void Surface::pointerMove(QPointF pos) {
    if (pressed_.isEmpty())
        return;
    lastPos_ = pos;
    if (dragging_) {
        dragging_(pos, false);
        return;
    }
    // A finger that slides off a button lets it go.
    const bool inside = hitRect(pressed_).adjusted(-8, -8, 8, 8).contains(pos);
    if (inside != pressedInside_) {
        pressedInside_ = inside;
        update();
    }
}

void Surface::pointerRelease(QPointF pos) {
    if (pressed_.isEmpty())
        return;
    const QString id = std::exchange(pressed_, {});
    const Drag drag = std::exchange(dragging_, {});
    if (drag) {
        drag(pos, true);
    } else if (hitRect(id).adjusted(-8, -8, 8, 8).contains(pos)) {
        for (const Hit& h : hits_)
            if (h.id == id && h.tap) {
                const auto tap = h.tap; // the tap may repaint, which clears the hits
                tap();
                break;
            }
    }
    update();
}

void Surface::pointerCancel() {
    if (pressed_.isEmpty())
        return;
    pressed_.clear();
    if (const Drag drag = std::exchange(dragging_, {}))
        drag(lastPos_, true);
    update();
}

void Surface::button(QPainter& p, const QString& id, const QRectF& r, const QString& text, std::function<void()> tap, const QColor& textColor,
                     const QColor& stroke, const QColor& fill, double px) {
    const bool down = isPressed(id) && pressedInside_;
    draw::card(p, r, 10 * u, down ? fill.lighter(160) : fill, stroke);
    p.setPen(textColor);
    p.setFont(typeface(Face::Text, px, QFont::DemiBold));
    draw::text(p, r.adjusted(6 * u, 0, -6 * u, 0), Qt::AlignCenter, text);
    hit(id, r, std::move(tap));
}

double Surface::header(QPainter& p, const QString& title, const QString& subtitle, const QVector<Pill>& pills, const QVector<Button>& buttons) {
    const double h = 72 * u, m = 20 * u, cy = h / 2;
    // The buttons, from the right.
    double right = width() - m;
    const double bh = 46 * u, bpx = 16 * u;
    const QFont bfont = typeface(Face::Text, bpx, QFont::DemiBold);
    const double th = 22 * u;
    for (int i = buttons.size() - 1; i >= 0; --i) {
        const Button& b = buttons[i];
        const double textW = QFontMetricsF(bfont).horizontalAdvance(b.text);
        const double tw = b.tag.isEmpty() ? 0 : draw::tagWidth(b.tag, th) + 10 * u;
        const double bw = tw + textW + 38 * u;
        const QRectF r(right - bw, cy - bh / 2, bw, bh);
        if (b.tag.isEmpty()) {
            button(p, b.id, r, b.text, b.tap, b.textColor, b.stroke, b.fill, bpx);
        } else {
            // The tag and the words, centred together.
            button(p, b.id, r, QString(), b.tap, b.textColor, b.stroke, b.fill, bpx);
            const bool plain = b.fill == color::panel;
            const double x = r.left() + 19 * u;
            draw::tag(p, QRectF(x, cy - th / 2, tw - 10 * u, th), b.tag, plain ? color::purpleBorder : b.textColor,
                      plain ? color::purple : b.textColor, plain ? color::purpleBg : QColor());
            p.setPen(b.textColor);
            p.setFont(bfont);
            p.drawText(QRectF(x + tw, r.top(), textW + 1, bh), Qt::AlignLeft | Qt::AlignVCenter, b.text);
        }
        right -= bw + 10 * u;
    }
    // The logo and the titles.
    double x = m;
    p.setPen(color::orange);
    p.setFont(typeface(Face::Display, 30 * u, QFont::Bold));
    p.drawText(QRectF(x, 0, 80 * u, h), Qt::AlignVCenter | Qt::AlignLeft, "CUE");
    x += QFontMetricsF(p.font()).horizontalAdvance("CUE") + 18 * u;
    const QFont titleFont = typeface(Face::Text, 16 * u, QFont::DemiBold), subFont = typeface(Face::Text, 14 * u);
    const double titleW = std::min(QFontMetricsF(titleFont).horizontalAdvance(title), (right - x) * 0.45);
    p.setFont(titleFont);
    p.setPen(color::text);
    draw::text(p, QRectF(x, 0, titleW + 1, h), Qt::AlignVCenter | Qt::AlignLeft, title);
    x += titleW + 16 * u;
    // Pills sit right, before the buttons, as many as fit beside the subtitle.
    const double ph = 32 * u, gap = 10 * u;
    const double subW = std::min(QFontMetricsF(subFont).horizontalAdvance(subtitle), std::max(0.0, (right - x) * 0.5));
    double room = right - 6 * u - (x + subW + 16 * u);
    int n = 0;
    double used = 0;
    for (; n < pills.size(); ++n) {
        const double w = draw::pillWidth(pills[n].text, ph, pills[n].dot.isValid()) + gap;
        if (used + w > room)
            break;
        used += w;
    }
    double px = right - 6 * u - used;
    for (int i = 0; i < n; ++i) {
        const Pill& pl = pills[i];
        const double w = draw::pillWidth(pl.text, ph, pl.dot.isValid());
        draw::pill(p, QRectF(px + gap, cy - ph / 2, w, ph), pl.text, pl.dot, pl.textColor, pl.stroke, pl.fill);
        px += w + gap;
    }
    p.setFont(subFont);
    p.setPen(color::muted);
    draw::text(p, QRectF(x, 0, std::max(0.0, right - used - x - 16 * u), h), Qt::AlignVCenter | Qt::AlignLeft, subtitle);
    p.setPen(QPen(color::line, 1));
    p.drawLine(QPointF(0, h - 0.5), QPointF(width(), h - 0.5));
    headerHeight_ = h;
    return h;
}

QVector<Surface::Pill> Surface::enginePills(const QStringList* only) const {
    QVector<Pill> pills;
    auto want = [only](const char* what) { return !only || only->contains(what); };
    if (!s.connected()) {
        pills.append({"Waiting for Cue", color::orange, color::orange, color::orange, color::bg});
        return pills;
    }
    const Status& st = s.status;
    if (!st.valid)
        return pills;
    const QColor load = st.dspAvg < 60 ? color::green : st.dspAvg < 85 ? color::orange : color::red;
    if (want("cpu"))
        pills.append({s.settings.percent ? QString("CPU %1%").arg(std::lround(st.dspAvg))
                                         : QString("CPU %1 ms").arg(st.dspAvg / 100 * st.periodMs, 0, 'f', 2),
                      load});
    QString margin = "Margin -";
    QColor marginColor = color::text2;
    if (st.marginMs) {
        const auto pc = st.marginPercent();
        margin = s.settings.percent && pc ? QString("Margin %1%").arg(std::lround(*pc)) : QString("Margin %1 ms").arg(*st.marginMs, 0, 'f', 2);
        if (pc && *pc < 15)
            marginColor = color::red;
    }
    if (want("margin"))
        pills.append({margin, {}, marginColor});
    // Xruns are always shown once there are any.
    if (want("xruns") || st.xruns)
        pills.append({QString("Xruns %1").arg(st.xruns), st.xruns ? color::red : color::green, st.xruns ? color::red : color::text2});
    const QString audio = audioSummary(s.hello);
    if (!audio.isEmpty() && want("audio"))
        pills.append({audio, {}});
    const QString controller = controllerName(s.hello);
    // A controller not found is always shown.
    const bool controllerUp = s.hello.value("midi").toObject().value("ports").toInt() > 0;
    if (!controller.isEmpty() && (want("controller") || !controllerUp)) {
        const bool up = controllerUp;
        pills.append({up ? controller : controller + " not found", up ? color::purple : color::red, up ? color::purple : color::red,
                      up ? color::purpleBorder : color::red, up ? color::purpleBg : color::bg});
    }
    return pills;
}

} // namespace cue::gui
