#include "gui/settingsview.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace cue::gui {

SettingsView::SettingsView(AppState& state, QWidget* parent) : Surface(state, parent) {
    connect(&s, &AppState::changed, this, qOverload<>(&QWidget::update));
    connect(&s, &AppState::tunerChanged, this, [this] {
        if (isVisible())
            update();
    });
}

void SettingsView::changed() {
    s.settings.save();
    emit settingsChanged();
    update();
}

void SettingsView::tuner(const QJsonObject& change) {
    QJsonObject req = change;
    req["cmd"] = "tuner";
    s.link.request(req, [this](const QJsonObject& r) {
        if (s.check(r, "The tuner did not take it")) {
            s.tuner = TunerReading::fromJson(r.value("tuner").toObject());
            update();
        }
    });
}

double SettingsView::row(QPainter& p, double y, const QString& label, const QString& hint, const QStringList& choices, int current,
                         const std::function<void(int)>& pick) {
    const double m = 40 * u, h = 64 * u, W = std::min<double>(width(), 1200 * u + 2 * m);
    const double x0 = (width() - W) / 2 + m, labelW = std::min(360 * u, (W - 2 * m) * 0.42);
    p.setPen(color::text);
    p.setFont(typeface(Face::Text, 17 * u, QFont::DemiBold));
    draw::text(p, QRectF(x0, y + 6 * u, labelW, 26 * u), Qt::AlignLeft | Qt::AlignVCenter, label);
    p.setPen(color::muted);
    p.setFont(typeface(Face::Text, 13 * u));
    draw::text(p, QRectF(x0, y + 32 * u, labelW, 22 * u), Qt::AlignLeft | Qt::AlignVCenter, hint);
    // The choices as one segmented control, as wide as their text.
    const QFont f = typeface(Face::Text, 16 * u, QFont::Medium);
    const QFontMetricsF fm(f);
    double total = 8 * u;
    for (const QString& c : choices)
        total += fm.horizontalAdvance(c) + 44 * u;
    const double avail = W - 2 * m - labelW - 20 * u;
    const double k = std::min(1.0, avail / total);
    const QRectF box(x0 + labelW + 20 * u, y + 4 * u, total * k, h - 8 * u);
    draw::card(p, box, 12 * u, color::bg, color::border);
    double x = box.left() + 4 * u * k;
    for (int i = 0; i < choices.size(); ++i) {
        const double sw = (fm.horizontalAdvance(choices[i]) + 44 * u) * k;
        const QRectF seg(x, box.top() + 4 * u, sw, box.height() - 8 * u);
        const QString id = label + ": " + choices[i];
        if (i == current || isPressed(id))
            draw::card(p, seg, 9 * u, i == current ? QColor(0x2A, 0x1A, 0x0A) : color::raised, i == current ? color::orange : QColor());
        p.setPen(i == current ? color::orange : color::text2);
        p.setFont(f);
        draw::text(p, seg, Qt::AlignCenter, choices[i]);
        if (seg.intersects(body_))
            hit(id, seg.intersected(body_), [pick, i] { pick(i); });
        x += sw;
    }
    return y + h + 14 * u;
}

void SettingsView::paint(QPainter& p) {
    const double top = header(p, "Settings", s.perf.valid ? s.perf.name : QString(), enginePills(),
                              {{"stage", "Back to stage", [this] { emit navigate("stage"); }, color::onOrange, color::orange, color::orange}});
    Settings& st = s.settings;
    const double m = 40 * u, W = std::min<double>(width(), 1200 * u + 2 * m), x0 = (width() - W) / 2 + m;
    body_ = QRectF(0, top, width(), height() - top);
    hit("scroll", body_, {}, [this](QPointF pos, bool done) {
        if (dragY_ < 0 || done) {
            dragY_ = done ? -1 : pos.y();
            dragFrom_ = scroll_;
            return;
        }
        scroll_ = std::clamp(dragFrom_ - (pos.y() - dragY_), 0.0, scrollMax_);
        update();
    });
    p.save();
    p.setClipRect(body_);
    double y = top + 26 * u - scroll_;
    auto section = [&](const QString& title) {
        QFont label = typeface(Face::Text, 13 * u, QFont::DemiBold);
        label.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * u);
        p.setFont(label);
        p.setPen(color::muted);
        p.drawText(QRectF(x0, y, W, 20 * u), Qt::AlignLeft | Qt::AlignVCenter, title);
        y += 34 * u;
    };

    section("THIS MACHINE");
    const QStringList views = {"stage", "tuner", "chains"};
    y = row(p, y, "Start in", "the view the app opens with", {"Stage", "Tuner", "Chains"}, views.indexOf(st.startView), [this, views](int i) {
        s.settings.startView = views[i];
        changed();
    });
    y = row(p, y, "Show times in", "CPU and margin: ms, or % of what a block allows", {"ms", "%"}, st.percent ? 1 : 0, [this](int i) {
        s.settings.percent = i == 1;
        changed();
    });
    const int size = static_cast<int>(std::lround(st.scale * 100));
    y = row(p, y, "Size", "everything bigger or smaller", {"−", QString("%1%").arg(size), "+"}, 1, [this](int i) {
        if (i != 1)
            s.settings.scale = std::clamp(std::round(s.settings.scale * 10 + (i == 0 ? -1 : 1)) / 10, 0.5, 2.5);
        else
            s.settings.scale = 1.0;
        changed();
    });
    y = row(p, y, "Tuner while muted", "the tuner view comes up while the inputs are muted", {"On", "Off"}, st.tunerWhenMuted ? 0 : 1,
            [this](int i) {
                s.settings.tunerWhenMuted = i == 0;
                changed();
            });
    y = row(p, y, "Full screen", "for the stage computer (F11 switches too)", {"On", "Off"}, st.fullScreen ? 0 : 1, [this](int i) {
        s.settings.fullScreen = i == 0;
        changed();
    });

    y += 10 * u;
    section("TUNER (SAVED WITH THE PERFORMANCE)");
    const TunerReading& t = s.tuner;
    const bool have = t.valid && s.connected();
    if (!have) {
        p.setPen(color::muted);
        p.setFont(typeface(Face::Text, 15 * u));
        p.drawText(QRectF(x0, y, W, 30 * u), Qt::AlignLeft | Qt::AlignVCenter, "Shown once the engine answers.");
        y += 44 * u;
    } else {
        const QStringList ranges = {"guitar", "bass", "vocals", "any"};
        y = row(p, y, "Range", "what the tuner listens for", {"Guitar", "Bass", "Vocals", "Any"}, ranges.indexOf(t.range),
                [this, ranges](int i) { tuner({{"range", ranges[i]}}); });
        y = row(p, y, "Reference", "the A above middle C", {"−", QString("%1 Hz").arg(t.reference, 0, 'f', 0), "+", "Reset"}, 1,
                [this, ref = t.reference](int i) {
                    if (i == 1)
                        return;
                    tuner({{"reference", i == 3 ? 440.0 : std::clamp(ref + (i == 0 ? -1 : 1), 415.0, 466.0)}});
                });
        y = row(p, y, "In tune within", "how close counts as in tune", {"1 ¢", "2 ¢", "3 ¢", "5 ¢"},
                QList<double>{1, 2, 3, 5}.indexOf(t.tolerance), [this](int i) { tuner({{"tolerance", QList<double>{1, 2, 3, 5}[i]}}); });
        y = row(p, y, "Note names", "C# or Db", {"Sharps", "Flats"}, t.names == "flats" ? 1 : 0,
                [this](int i) { tuner({{"names", i == 1 ? "flats" : "sharps"}}); });
    }

    y += 10 * u;
    section("ENGINE");
    p.setPen(color::text2);
    p.setFont(typeface(Face::Text, 14 * u));
    QStringList lines;
    if (s.connected()) {
        lines << QString("Cue %1, playing %2").arg(s.hello.value("cue").toString(), s.hello.value("file").toString());
        lines << s.hello.value("audio").toString();
    } else {
        lines << "Not connected";
    }
    lines << "Control socket: " + s.link.socketPath();
    lines << "App settings: " + Settings::path();
    for (const QString& l : lines) {
        draw::text(p, QRectF(x0, y, W - 2 * m, 24 * u), Qt::AlignLeft | Qt::AlignVCenter, l);
        y += 26 * u;
    }
    p.restore();
    scrollMax_ = std::max(0.0, y + scroll_ + 20 * u - height());
    scroll_ = std::min(scroll_, scrollMax_);
    if (scrollMax_ > 0) {
        const double h = body_.height() * body_.height() / (body_.height() + scrollMax_);
        p.fillRect(QRectF(width() - 6 * u, body_.top() + (body_.height() - h) * scroll_ / scrollMax_, 3 * u, h), color::border2);
    }
}

void SettingsView::wheelEvent(QWheelEvent* e) {
    scroll_ = std::clamp(scroll_ - e->angleDelta().y() / 120.0 * 60 * u, 0.0, scrollMax_);
    update();
}

} // namespace cue::gui
