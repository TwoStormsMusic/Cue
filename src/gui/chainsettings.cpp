// The chain view's chains and scenes: the column that lists a scene's chains
// and makes new ones, the settings under the board, the menus their fields
// open and the line typed in for a name, a display or an action. Every
// change goes to the engine as a request and is kept for undo.

#include "gui/chainview.h"

#include <QDateTime>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cue::gui {

namespace {

QString newToken(int& counter) { return QString("app-%1-%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(++counter); }

QJsonObject batch(const QJsonObject& first, const QJsonObject& then) {
    return {{"cmd", "batch"}, {"requests", QJsonArray{first, then}}};
}

// A line to type in, which Esc lets go of without keeping what was typed.
class Entry : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
    std::function<void()> escaped;

protected:
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Escape && escaped) {
            escaped();
            return;
        }
        QLineEdit::keyPressEvent(e);
    }
};

// What a set-chain or set-scene changes, as an undo says it.
QString describe(const QJsonObject& settings) {
    if (settings.contains("name"))
        return "name";
    if (settings.contains("pedal"))
        return "pedal";
    if (settings.contains("display"))
        return "display";
    if (settings.contains("warm"))
        return "chain kept warm";
    if (settings.contains("on_select"))
        return "what selecting it does";
    return "settings";
}

QString pedalText(int pedal) { return pedal > 0 ? QString("FCB pedal %1").arg(pedal) : QString("None"); }

// Chips flowed in rows across area: where each of texts goes, and the
// last, the + that adds one.
QVector<QRectF> chipRects(const QStringList& texts, const QRectF& area, double u, const QFont& font) {
    QVector<QRectF> out;
    const double h = 34 * u, gap = 8 * u;
    double x = area.left(), y = area.top();
    QFontMetricsF fm(font);
    for (int i = 0; i <= texts.size(); ++i) {
        const QString t = i < texts.size() ? texts[i] + "  ×" : "+ Action";
        const double w = std::min(area.width(), fm.horizontalAdvance(t) + 26 * u);
        if (x + w > area.right() + 0.5 && x > area.left()) {
            x = area.left();
            y += h + gap;
        }
        out << QRectF(x, y, w, h);
        x += w + gap;
    }
    return out;
}

QFont chipFont(double u) { return typeface(Face::Text, 14 * u); }

QFont labelFont(double u) {
    QFont f = typeface(Face::Text, 11.5 * u, QFont::DemiBold);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.3 * u);
    return f;
}

// The settings are a row of fields, then a row of actions after their label.
const char* kActionsLabel = "WHEN SELECTED, DO";
double actionsLabelWidth(double u) { return QFontMetricsF(labelFont(u)).horizontalAdvance(kActionsLabel) + 18 * u; }

// Columns across r, each a share of it, with gaps between.
class Columns {
public:
    Columns(const QRectF& r, double gap) : r_(r), gap_(gap), x_(r.left()) {}
    QRectF next(double share) {
        const QRectF col(x_, r_.top(), share * (r_.width() + gap_) - gap_, r_.height());
        x_ += share * (r_.width() + gap_);
        return col;
    }

private:
    QRectF r_;
    double gap_, x_;
};

} // namespace

// ------------------------------------------------------------ the column

int ChainView::shownScene() const {
    if (const ChainInfo* c = s.chain(chain_))
        return c->scene;
    return s.status.valid && s.status.scene < s.perf.scenes.size() ? s.status.scene : (s.perf.scenes.isEmpty() ? -1 : 0);
}

void ChainView::showScene(int scene) {
    if (scene < 0 || scene >= s.perf.scenes.size() || s.perf.scenes[scene].chains.isEmpty())
        return;
    const QVector<int>& chains = s.perf.scenes[scene].chains;
    const int chain = chains.contains(s.playing()) ? s.playing() : chains.first();
    showChain(chain);
}

void ChainView::setStrip(Strip what) {
    strip_ = what;
    endEntry();
    closeMenu();
    update();
}

void ChainView::sidebar(QPainter& p, const QRectF& r) {
    const double pad = 14 * u;
    double y = r.top() + 16 * u;
    QFont label = typeface(Face::Text, 12 * u, QFont::DemiBold);
    label.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * u);
    auto heading = [&](const QString& text) {
        p.setFont(label);
        p.setPen(color::muted);
        p.drawText(QRectF(r.left() + pad, y, r.width() - 2 * pad, 18 * u), Qt::AlignLeft | Qt::AlignVCenter, text);
        y += 26 * u;
    };
    const Performance& perf = s.perf;
    const int scene = shownScene();
    if (scene < 0)
        return;

    // The scene listed; a tap lists the others to pick from.
    heading("SCENE");
    {
        const QRectF box(r.left() + 10 * u, y, r.width() - 20 * u, 42 * u);
        const bool open = menu_ && menu_->id == "scenes";
        draw::card(p, box, 8 * u, isPressed("scenes") || open ? color::raised : color::panel2, open ? color::orange : color::border);
        const bool playingHere = s.status.valid && s.status.scene == scene;
        p.setPen(Qt::NoPen);
        p.setBrush(playingHere ? color::green : color::dim);
        p.drawEllipse(QPointF(box.left() + 16 * u, box.center().y()), 4 * u, 4 * u);
        p.setPen(color::text);
        p.setFont(typeface(Face::Text, 15 * u, QFont::DemiBold));
        draw::text(p, box.adjusted(28 * u, 0, -28 * u, 0), Qt::AlignLeft | Qt::AlignVCenter, perf.scenes[scene].name);
        p.setPen(color::muted);
        p.drawText(box.adjusted(0, 0, -12 * u, 0), Qt::AlignRight | Qt::AlignVCenter, "▾");
        hit("scenes", box, [this, box] {
            QVector<MenuItem> items;
            for (int i = 0; i < s.perf.scenes.size(); ++i) {
                const bool playing = s.status.valid && s.status.scene == i;
                items.append({s.perf.scenes[i].name, playing ? "playing" : QString(), i == shownScene(), [this, i] { showScene(i); }});
            }
            items.append({"+ New scene", {}, false, [this] { newScene(); }});
            openMenu("scenes", box, items);
        });
        y += 54 * u;
    }

    heading("CHAINS");
    const int playing = s.playing(), warm = s.status.valid ? s.status.warmChain() : -1;
    const double rowH = 44 * u, btnH = 40 * u, gap = 8 * u;
    const double buttonsTop = r.bottom() - 3 * btnH - 3 * gap - 6 * u;
    for (int index : perf.scenes[scene].chains) {
        const ChainInfo* c = s.chain(index);
        if (!c || y > buttonsTop - rowH)
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
        // A tap shows the chain; a double tap plays it too.
        hit(id, row, [this, id, index] {
            if (doubleTap(id) && index != s.playing()) {
                s.select(index);
                followPlaying_ = true;
            } else {
                followPlaying_ = index == s.playing();
            }
            showChain(index);
        });
        y += rowH;
    }

    // New chains, copies from other scenes, new scenes.
    const QRectF b1(r.left() + 10 * u, buttonsTop, r.width() - 20 * u, btnH);
    button(p, "new chain", b1, "+ Chain", [this] { newChain(); }, color::text, color::border2, color::panel2, 15 * u);
    const QRectF b2 = b1.translated(0, btnH + gap);
    button(p, "copy chain", b2, "Copy from scene…", [this, b2] {
        QVector<MenuItem> items;
        const int here = shownScene();
        for (int sc = 0; sc < s.perf.scenes.size(); ++sc) {
            if (sc == here)
                continue;
            for (int index : s.perf.scenes[sc].chains)
                if (const ChainInfo* c = s.chain(index))
                    items.append({c->name, "from " + s.perf.scenes[sc].name, false, [this, index] { newChain(index); }});
        }
        if (items.isEmpty())
            items.append({"No other scene yet", "+ Scene makes one", false, {}});
        openMenu("copy chain", b2, items);
    },
           color::text, color::border2, color::panel2, 15 * u);
    const QRectF b3 = b2.translated(0, btnH + gap + 6 * u);
    button(p, "new scene", b3, "+ Scene", [this] { newScene(); }, color::text, color::border2, color::panel2, 15 * u);
}

// ------------------------------------------------------------ the settings

double ChainView::stripHeight(const ChainInfo& c, double width) const {
    // The title, the fields, one row of actions; the same for a scene, so
    // the board stays still between the two.
    const double base = 182 * u;
    if (strip_ != Strip::Chain)
        return base;
    // The actions' chips may need more rows than one.
    const double actionsW = width - 32 * u - actionsLabelWidth(u);
    const QVector<QRectF> chips = chipRects(c.onSelect, QRectF(0, 0, actionsW, 1000 * u), u, chipFont(u));
    const double rows = std::round(chips.last().top() / (42 * u)) + 1;
    return base + std::clamp(rows - 1, 0.0, 3.0) * 42 * u;
}

void ChainView::settingsStrip(QPainter& p, const QRectF& r, const ChainInfo& c) {
    draw::card(p, r, 12 * u, color::panel, color::border);
    const double pad = 16 * u;
    const QRectF in = r.adjusted(pad, 12 * u, -pad, -14 * u);
    const int scene = c.scene;
    const QString sceneName = scene < s.perf.scenes.size() ? s.perf.scenes[scene].name : QString();

    // Whose settings: the chain's or its scene's.
    const double titleH = 34 * u;
    double x = in.left();
    auto tab = [&](const QString& id, const QString& text, Strip what) {
        const bool on = strip_ == what;
        const QFont f = typeface(Face::Display, (on ? 21 : 17) * u, QFont::DemiBold);
        const double w = std::min(in.width() * 0.4, QFontMetricsF(f).horizontalAdvance(text) + 4 * u);
        const QRectF tr(x, in.top(), w, titleH);
        p.setFont(f);
        p.setPen(on ? color::orange : isPressed(id) ? color::text : color::muted);
        draw::text(p, tr, Qt::AlignLeft | Qt::AlignVCenter, text);
        hit(id, tr, [this, what] { setStrip(what); });
        x += w + 24 * u;
    };
    tab("strip chain", c.name + ": chain settings", Strip::Chain);
    tab("strip scene", "Scene: " + sceneName, Strip::Scene);

    // What can be done with it: copied, moved, taken out.
    const double bh = 32 * u;
    double right = in.right();
    auto action = [&](const QString& id, const QString& text, const QColor& color, std::function<void(QRectF)> tap) {
        const QFont f = typeface(Face::Text, 13.5 * u, QFont::DemiBold);
        const double w = QFontMetricsF(f).horizontalAdvance(text) + 28 * u;
        const QRectF br(right - w, in.top() + (titleH - bh) / 2, w, bh);
        if (br.left() < x)
            return;
        button(p, id, br, text, [tap, br] { tap(br); }, color, color::border2, color::panel2, 13.5 * u);
        right -= w + 8 * u;
    };
    if (strip_ == Strip::Chain) {
        const int id = c.index;
        action("remove chain", "Take out", color::red, [this, id](QRectF) { removeChain(id); });
        action("move chain", "Move ▾", color::text, [this, id](QRectF under) {
            const ChainInfo* ch = s.chain(id);
            if (!ch)
                return;
            const QVector<int>& chains = s.perf.scenes[ch->scene].chains;
            const int at = chains.indexOf(id), sc = ch->scene;
            QVector<MenuItem> items;
            if (at > 0)
                items.append({"Up", {}, false, [this, id, sc, at] { moveChain(id, sc, at - 1); }});
            if (at + 1 < chains.size())
                items.append({"Down", {}, false, [this, id, sc, at] { moveChain(id, sc, at + 1); }});
            for (int other = 0; other < s.perf.scenes.size(); ++other)
                if (other != sc)
                    items.append({"To scene " + s.perf.scenes[other].name, {}, false, [this, id, other] { moveChain(id, other, -1); }});
            if (items.isEmpty())
                items.append({"Nowhere to move it", "it is the only chain", false, {}});
            openMenu("move chain", under, items);
        });
        action("copy this chain", "Duplicate", color::text, [this, id](QRectF) { newChain(id); });
    } else {
        action("remove scene", "Take out", color::red, [this, scene](QRectF) { removeScene(scene); });
        action("move scene", "Move ▾", color::text, [this, scene](QRectF under) {
            const int n = s.perf.scenes.size();
            QVector<MenuItem> items;
            if (scene > 0) {
                items.append({"Earlier", {}, false, [this, scene] { moveScene(scene, scene - 1); }});
                items.append({"First", {}, false, [this, scene] { moveScene(scene, 0); }});
            }
            if (scene + 1 < n) {
                items.append({"Later", {}, false, [this, scene] { moveScene(scene, scene + 1); }});
                items.append({"Last", {}, false, [this, scene, n] { moveScene(scene, n - 1); }});
            }
            if (items.isEmpty())
                items.append({"Nowhere to move it", "it is the only scene", false, {}});
            openMenu("move scene", under, items);
        });
        action("copy this scene", "Duplicate", color::text, [this, scene](QRectF) { newScene(scene); });
    }

    const QRectF body(in.left(), in.top() + titleH + 10 * u, in.width(), in.bottom() - in.top() - titleH - 10 * u);
    if (strip_ == Strip::Chain)
        chainFields(p, body, c);
    else
        sceneFields(p, body, scene);
}

void ChainView::field(QPainter& p, const QString& id, const QRectF& r, const QString& label, const QString& value, bool menu,
                      std::function<void()> tap) {
    p.setFont(labelFont(u));
    p.setPen(color::muted);
    draw::text(p, QRectF(r.left(), r.top(), r.width(), 18 * u), Qt::AlignLeft | Qt::AlignVCenter, label);
    const QRectF box(r.left(), r.top() + 24 * u, r.width(), 40 * u);
    if (entryFor_ == id) {
        placeEntry(id, box);
        return;
    }
    const bool open = menu_ && menu_->id == id;
    draw::card(p, box, 8 * u, isPressed(id) || open ? color::raised : color::panel2, open ? color::orange : color::border2);
    p.setPen(value.isEmpty() ? color::dim : color::text);
    p.setFont(typeface(Face::Text, 15 * u));
    draw::text(p, box.adjusted(12 * u, 0, -(menu ? 28 : 12) * u, 0), Qt::AlignLeft | Qt::AlignVCenter, value.isEmpty() ? "none" : value);
    if (menu) {
        p.setPen(color::muted);
        p.drawText(box.adjusted(0, 0, -10 * u, 0), Qt::AlignRight | Qt::AlignVCenter, "▾");
    }
    hit(id, box, std::move(tap));
}

QVector<ChainView::MenuItem> ChainView::pedalItems(int now, const std::function<void(int)>& pick, const QString& self, bool scene) const {
    const Performance& perf = s.perf;
    const int shown = shownScene();
    QVector<MenuItem> items;
    items.append({"None", {}, now == 0, [pick] { pick(0); }});
    for (int pedal = 1; pedal <= 10; ++pedal) {
        // What else that pedal does here.
        QStringList others;
        if (!scene && shown >= 0)
            for (int index : perf.scenes[shown].chains)
                if (const ChainInfo* c = s.chain(index); c && c->pedal == pedal && c->name != self)
                    others << c->name;
        for (const SceneInfo& sc : perf.scenes)
            if (sc.pedal == pedal && (!scene || sc.name != self))
                others << "scene " + sc.name;
        for (const ControlInfo& k : perf.controls)
            if (k.pedal == pedal)
                others << k.name;
        items.append({pedalText(pedal), others.join(", "), pedal == now, [pick, pedal] { pick(pedal); }});
    }
    return items;
}

void ChainView::chainFields(QPainter& p, const QRectF& r, const ChainInfo& c) {
    const int id = c.index;
    const QString name = c.name;
    Columns columns(QRectF(r.left(), r.top(), r.width(), 64 * u), 16 * u);
    const QRectF nameR = columns.next(0.22), pedalR = columns.next(0.22), displayR = columns.next(0.16), warmR = columns.next(0.40);

    field(p, "chain name", nameR, "NAME", name, false, [this, id, name] {
        startEntry("chain name", name, "Its name", 40, [this, id, name](const QString& text) {
            if (!text.trimmed().isEmpty() && text.trimmed() != name)
                setChain(id, {{"name", text.trimmed()}});
        });
    });
    field(p, "chain pedal", pedalR, "SELECTED BY", pedalText(c.pedal), true, [this, id, pedalR, name, now = c.pedal] {
        openMenu("chain pedal", pedalR.adjusted(0, 24 * u, 0, 0),
                 pedalItems(now, [this, id](int pedal) { setChain(id, {{"pedal", pedal}}); }, name, false));
    });
    field(p, "chain display", displayR, "FCB DISPLAY", c.display, false, [this, id, display = c.display] {
        startEntry("chain display", display, "3 letters", 3, [this, id, display](const QString& text) {
            if (text.trimmed() != display)
                setChain(id, {{"display", text.trimmed()}});
        });
    });

    // The chain kept warm while it plays: the next, none, or one of the scene.
    const QVector<int> chains = c.scene < s.perf.scenes.size() ? s.perf.scenes[c.scene].chains : QVector<int>();
    QString next;
    for (int k = 1; k <= chains.size() && next.isEmpty(); ++k)
        if (const ChainInfo* o = s.chain(chains[(chains.indexOf(id) + k) % chains.size()]); o && o->index != id)
            next = o->name;
    const QString warm = c.warm.compare("next", Qt::CaseInsensitive) == 0 ? (next.isEmpty() ? "Next (none)" : "Next: " + next)
                         : c.warm.compare("none", Qt::CaseInsensitive) == 0 ? "None"
                                                                             : c.warm;
    field(p, "chain warm", warmR, "KEEP WARM WHILE " + name.toUpper() + " PLAYS", warm, true, [this, id, warmR, chains, next, now = c.warm] {
        QVector<MenuItem> items;
        items.append({"The next chain", next, now.compare("next", Qt::CaseInsensitive) == 0, [this, id] { setChain(id, {{"warm", "next"}}); }});
        items.append({"None", "switching takes a moment longer", now.compare("none", Qt::CaseInsensitive) == 0,
                      [this, id] { setChain(id, {{"warm", "none"}}); }});
        for (int other : chains)
            if (const ChainInfo* o = s.chain(other); o && other != id)
                items.append({o->name, {}, now.compare(o->name, Qt::CaseInsensitive) == 0, [this, id, n = o->name] { setChain(id, {{"warm", n}}); }});
        openMenu("chain warm", warmR.adjusted(0, 24 * u, 0, 0), items);
    });

    // What selecting it does: each action a chip; + Action adds one.
    const double rowTop = r.top() + 64 * u + 14 * u, labelW = actionsLabelWidth(u);
    p.setFont(labelFont(u));
    p.setPen(color::muted);
    draw::text(p, QRectF(r.left(), rowTop, labelW, 34 * u), Qt::AlignLeft | Qt::AlignVCenter, kActionsLabel);
    const QRectF chipsArea(r.left() + labelW, rowTop, r.width() - labelW, r.bottom() - rowTop);
    const QStringList actions = c.onSelect;
    if (entryFor_ == "add action") {
        placeEntry("add action", QRectF(chipsArea.left(), chipsArea.top(), chipsArea.width(), 34 * u));
        return;
    }
    const QFont cf = chipFont(u);
    const QVector<QRectF> chips = chipRects(actions, chipsArea, u, cf);
    for (int i = 0; i < chips.size(); ++i) {
        const QRectF chip = chips[i];
        if (chip.bottom() > r.bottom() + 1)
            break;
        const bool add = i == actions.size();
        const QString hid = add ? QString("add action") : QString("action %1").arg(i);
        const bool open = menu_ && menu_->id == hid;
        draw::card(p, chip, 8 * u, isPressed(hid) || open ? color::raised : color::panel2, open ? color::orange : color::border2);
        p.setFont(cf);
        p.setPen(add ? color::text2 : color::text);
        draw::text(p, chip.adjusted(12 * u, 0, -10 * u, 0), Qt::AlignLeft | Qt::AlignVCenter, add ? "+ Action" : actions[i] + "  ×");
        if (add) {
            hit(hid, chip, [this, id, chip, actions] {
                // Typed, or one of the controls' values.
                QVector<MenuItem> items;
                auto adding = [this, id, actions](const QString& action) {
                    QStringList now = actions;
                    now << action;
                    setChain(id, {{"on_select", QJsonArray::fromStringList(now)}});
                };
                items.append({"Type an action…", "Wah = on, group Reset", false, [this, adding] {
                                  startEntry("add action", {}, "An action, as Wah = on or group Reset", 200, [adding](const QString& text) {
                                      if (!text.trimmed().isEmpty())
                                          adding(text.trimmed());
                                  });
                              }});
                for (const ControlInfo& k : s.perf.controls) {
                    if (k.type == "continuous")
                        continue;
                    for (const QString& v : k.values)
                        items.append({k.name + " = " + v, {}, false, [adding, a = k.name + " = " + v] { adding(a); }});
                }
                openMenu("add action", chip, items);
            });
        } else {
            hit(hid, chip, [this, id, chip, actions, i, hid] {
                auto setTo = [this, id](const QStringList& now) { setChain(id, {{"on_select", QJsonArray::fromStringList(now)}}); };
                QVector<MenuItem> items;
                items.append({"Take it out", actions[i], false, [setTo, actions, i] {
                                  QStringList now = actions;
                                  now.removeAt(i);
                                  setTo(now);
                              }});
                if (i > 0)
                    items.append({"Do it earlier", {}, false, [setTo, actions, i] {
                                      QStringList now = actions;
                                      now.swapItemsAt(i, i - 1);
                                      setTo(now);
                                  }});
                if (i + 1 < actions.size())
                    items.append({"Do it later", {}, false, [setTo, actions, i] {
                                      QStringList now = actions;
                                      now.swapItemsAt(i, i + 1);
                                      setTo(now);
                                  }});
                openMenu(hid, chip, items);
            });
        }
    }
}

void ChainView::sceneFields(QPainter& p, const QRectF& r, int scene) {
    if (scene < 0 || scene >= s.perf.scenes.size())
        return;
    const SceneInfo& sc = s.perf.scenes[scene];
    Columns columns(QRectF(r.left(), r.top(), r.width(), 64 * u), 16 * u);
    const QRectF nameR = columns.next(0.30), pedalR = columns.next(0.26), displayR = columns.next(0.16);
    const QString name = sc.name;
    field(p, "scene name", nameR, "NAME", name, false, [this, scene, name] {
        startEntry("scene name", name, "Its name", 40, [this, scene, name](const QString& text) {
            if (!text.trimmed().isEmpty() && text.trimmed() != name)
                setScene(scene, {{"name", text.trimmed()}});
        });
    });
    field(p, "scene pedal", pedalR, "SELECTED BY", pedalText(sc.pedal), true, [this, scene, pedalR, name, now = sc.pedal] {
        openMenu("scene pedal", pedalR.adjusted(0, 24 * u, 0, 0),
                 pedalItems(now, [this, scene](int pedal) { setScene(scene, {{"pedal", pedal}}); }, name, true));
    });
    field(p, "scene display", displayR, "FCB DISPLAY", sc.display, false, [this, scene, display = sc.display] {
        startEntry("scene display", display, "3 letters", 3, [this, scene, display](const QString& text) {
            if (text.trimmed() != display)
                setScene(scene, {{"display", text.trimmed()}});
        });
    });
    int triggers = 0;
    for (const TriggerInfo& t : s.perf.triggers)
        triggers += t.scene.compare(name, Qt::CaseInsensitive) == 0;
    const int n = sc.chains.size();
    QString info = QString("%1 chain%2").arg(n).arg(n == 1 ? "" : "s");
    if (triggers)
        info += QString(" · %1 MIDI trigger%2 that work only here").arg(triggers).arg(triggers == 1 ? "" : "s");
    info += ". Duplicate copies all of it, plug-in settings, pedals and triggers too.";
    p.setPen(color::muted);
    p.setFont(typeface(Face::Text, 14 * u));
    p.drawText(QRectF(r.left(), r.top() + 64 * u + 14 * u, r.width(), r.bottom() - r.top() - 78 * u), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
               info);
}

// ------------------------------------------------------------ menus

void ChainView::openMenu(const QString& id, const QRectF& under, QVector<MenuItem> items) {
    endEntry();
    menu_ = Menu{id, under, std::move(items)};
    menuScroll_ = 0;
    menuPressing_ = false;
    update();
}

void ChainView::closeMenu() {
    if (!menu_)
        return;
    menu_.reset();
    menuPressing_ = false;
    update();
}

QStringList ChainView::menuItems() const {
    QStringList out;
    if (menu_)
        for (const MenuItem& m : menu_->items)
            out << m.text;
    return out;
}

void ChainView::pickMenu(int item) {
    if (!menu_ || item < 0 || item >= menu_->items.size())
        return;
    const auto pick = menu_->items[item].pick;
    if (!pick)
        return;
    closeMenu();
    pick();
}

void ChainView::drawMenu(QPainter& p) {
    if (!menu_)
        return;
    // A tap anywhere else closes it.
    hit("menu away", rect(), [this] { closeMenu(); });
    const double rowH = 42 * u, pad = 6 * u;
    const QRectF under = menu_->under;
    const double w = std::max(under.width(), 300 * u);
    const double want = menu_->items.size() * rowH + 2 * pad;
    const double below = height() - under.bottom() - 12 * u, above = under.top() - 12 * u;
    const bool down = below >= std::min(want, 220 * u) || below >= above;
    const double h = std::min(want, down ? below : above);
    const double x = std::clamp(under.left(), 10 * u, std::max(10 * u, width() - w - 10 * u));
    const QRectF box(x, down ? under.bottom() + 4 * u : under.top() - 4 * u - h, w, h);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 120));
    p.drawRoundedRect(box.translated(0, 4 * u), 12 * u, 12 * u);
    draw::card(p, box, 12 * u, color::raised, color::border2);
    const QRectF list = box.adjusted(0, pad, 0, -pad);
    menuMax_ = std::max(0.0, want - h);
    menuScroll_ = std::clamp(menuScroll_, 0.0, menuMax_);
    p.save();
    p.setClipRect(list);
    for (int i = 0; i < menu_->items.size(); ++i) {
        const MenuItem& m = menu_->items[i];
        const QRectF row(box.left(), list.top() + i * rowH - menuScroll_, w, rowH);
        if (row.bottom() < list.top() || row.top() > list.bottom())
            continue;
        p.setFont(typeface(Face::Text, 15 * u, m.checked ? QFont::DemiBold : QFont::Normal));
        p.setPen(!m.pick ? color::dim : m.checked ? color::orange : color::text);
        const QFontMetricsF fm(p.font());
        const double textW = std::min(fm.horizontalAdvance(m.text) + 2, w * (m.detail.isEmpty() ? 0.9 : 0.6));
        draw::text(p, QRectF(row.left() + 16 * u, row.top(), textW, rowH), Qt::AlignLeft | Qt::AlignVCenter, m.text);
        if (!m.detail.isEmpty()) {
            p.setPen(color::muted);
            p.setFont(typeface(Face::Text, 13 * u));
            draw::text(p, QRectF(row.left() + 24 * u + textW, row.top(), w - textW - 40 * u, rowH), Qt::AlignRight | Qt::AlignVCenter, m.detail);
        }
    }
    p.restore();
    // One hit for the list: a drag scrolls it, a tap picks the row.
    hit("menu", list, {}, [this, list, rowH](QPointF pos, bool done) {
        if (!menuPressing_) {
            menuPressing_ = true;
            menuMoved_ = false;
            menuPress_ = pos;
            menuScrollFrom_ = menuScroll_;
        }
        if (!menuMoved_ && std::fabs(pos.y() - menuPress_.y()) > 10 * u)
            menuMoved_ = true;
        if (menuMoved_) {
            menuScroll_ = std::clamp(menuScrollFrom_ - (pos.y() - menuPress_.y()), 0.0, menuMax_);
            update();
        }
        if (done) {
            menuPressing_ = false;
            if (!menuMoved_ && list.contains(pos))
                pickMenu(static_cast<int>((pos.y() - list.top() + menuScroll_) / rowH));
        }
    });
}

// ------------------------------------------------------------ typing

QString ChainView::entryField() const { return entry_ && entry_->isVisible() ? entryFor_ : QString(); }

void ChainView::startEntry(const QString& id, const QString& text, const QString& placeholder, int maxLength,
                           std::function<void(const QString&)> done) {
    closeMenu();
    if (!entry_) {
        auto* e = new Entry(this);
        e->escaped = [this] {
            entryCancelled_ = true;
            endEntry();
        };
        entry_ = e;
        entry_->hide();
        // Enter keeps it, and so does tapping elsewhere.
        connect(entry_, &QLineEdit::editingFinished, this, &ChainView::commitEntry);
    }
    if (entry_->isVisible())
        endEntry();
    entryFor_ = id;
    entryDone_ = std::move(done);
    entryCancelled_ = false;
    entry_->setMaxLength(maxLength > 0 ? maxLength : 32767);
    entry_->setPlaceholderText(placeholder);
    entry_->setText(text);
    entry_->selectAll();
    update();
}

void ChainView::placeEntry(const QString& id, const QRectF& r) {
    if (!entry_ || entryFor_ != id)
        return;
    entryPlaced_ = true;
    const QRect g = r.toRect();
    if (entry_->geometry() != g || entry_->font().pixelSize() != std::lround(15 * u)) {
        entry_->setGeometry(g);
        entry_->setFont(typeface(Face::Text, 15 * u));
        entry_->setStyleSheet(QString("QLineEdit { background: %1; color: %2; border: 1px solid %3; border-radius: %4px; padding: 0 %5px;"
                                      " selection-background-color: %3; selection-color: %6; }")
                                  .arg(color::panel2.name(), color::text.name(), color::orange.name())
                                  .arg(std::lround(8 * u))
                                  .arg(std::lround(11 * u))
                                  .arg(color::onOrange.name()));
    }
    if (!entry_->isVisible()) {
        entry_->show();
        entry_->setFocus();
    }
}

void ChainView::commitEntry() {
    if (!entry_ || entryFor_.isEmpty())
        return;
    const auto then = std::exchange(entryDone_, {});
    const QString typed = entry_->text();
    const bool keep = !entryCancelled_;
    endEntry();
    if (keep && then)
        then(typed);
}

void ChainView::endEntry() {
    if (!entry_ || entryFor_.isEmpty())
        return;
    entryFor_.clear();
    entryDone_ = {};
    if (entry_->isVisible()) {
        entry_->hide();
        setFocus();
    }
    update();
}

void ChainView::typeEntry(const QString& text) {
    if (!entry_ || entryFor_.isEmpty())
        return;
    entry_->setText(text);
    emit entry_->editingFinished();
}

// ------------------------------------------------------------ requests

void ChainView::arrange(const QJsonObject& request, const QString& failed, std::function<void(const QJsonObject&)> done) {
    s.change(request, [this, failed, done](const QJsonObject& r) {
        if (!s.check(r, failed))
            return;
        const QJsonArray warnings = r.value("warnings").toArray();
        if (!warnings.isEmpty())
            emit s.message(warnings.first().toString(), false);
        if (done)
            done(r);
        update();
    });
}

QJsonObject ChainView::chainNow(const ChainInfo& c, const QJsonObject& settings) const {
    QJsonObject now{{"cmd", "set-chain"}, {"chain", c.index}};
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        const QString key = it.key();
        if (key == "name")
            now[key] = c.name;
        else if (key == "display")
            now[key] = c.display;
        else if (key == "pedal")
            now[key] = c.pedal;
        else if (key == "program")
            now[key] = c.program;
        else if (key == "warm")
            now[key] = c.warm;
        else if (key == "on_select")
            now[key] = QJsonArray::fromStringList(c.onSelect);
    }
    return now;
}

QJsonObject ChainView::sceneNow(int scene, const QJsonObject& settings) const {
    const SceneInfo& sc = s.perf.scenes[scene];
    QJsonObject now{{"cmd", "set-scene"}, {"scene", scene}};
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        const QString key = it.key();
        if (key == "name")
            now[key] = sc.name;
        else if (key == "display")
            now[key] = sc.display;
        else if (key == "pedal")
            now[key] = sc.pedal;
        else if (key == "program")
            now[key] = sc.program;
    }
    return now;
}

void ChainView::newChain(int copy) {
    const int scene = shownScene();
    if (scene < 0)
        return;
    QJsonObject request{{"cmd", "add-chain"}, {"scene", scene}};
    if (copy >= 0) {
        request["copy"] = copy;
        // A copy in its own scene goes just after it.
        if (const ChainInfo* from = s.chain(copy); from && from->scene == scene)
            request["at"] = s.perf.scenes[scene].chains.indexOf(copy) + 1;
    }
    closeMenu();
    arrange(request, copy >= 0 ? "Not copied" : "No new chain", [this, copy](const QJsonObject& r) {
        const int id = r.value("chain").toInt(-1);
        const ChainInfo* c = s.chain(id);
        if (!c)
            return;
        const QString token = newToken(token_);
        s.edited({(copy >= 0 ? "Copied " : "Added chain ") + c->name, id, -1, {{"cmd", "remove-chain"}, {"chain", id}, {"token", token}},
                  {{"cmd", "restore-chain"}, {"token", token}}});
        strip_ = Strip::Chain;
        showChain(id);
    });
}

void ChainView::removeChain(int chain) {
    const ChainInfo* c = s.chain(chain);
    if (!c)
        return;
    // Then the one beside it is shown.
    const QVector<int> chains = s.perf.scenes[c->scene].chains;
    const int at = chains.indexOf(chain);
    const int beside = chains.size() < 2 ? -1 : chains[at + 1 < chains.size() ? at + 1 : at - 1];
    const QString token = newToken(token_), name = c->name;
    const QJsonObject take{{"cmd", "remove-chain"}, {"chain", chain}, {"token", token}};
    arrange(take, "Not taken out", [this, chain, beside, name, take, token](const QJsonObject&) {
        s.edited({"Took out chain " + name, chain, -1, {{"cmd", "restore-chain"}, {"token", token}}, take});
        if (s.chain(beside))
            showChain(beside);
    });
}

void ChainView::setChain(int chain, const QJsonObject& settings) {
    const ChainInfo* c = s.chain(chain);
    if (!c)
        return;
    QJsonObject request = settings;
    request["cmd"] = "set-chain";
    request["chain"] = chain;
    QJsonObject undo = chainNow(*c, settings);
    // A pedal taken from another chain of the scene goes back to it.
    const int pedal = settings.value("pedal").toInt(0);
    if (pedal > 0 && c->scene < s.perf.scenes.size())
        for (int other : s.perf.scenes[c->scene].chains)
            if (const ChainInfo* o = s.chain(other); o && other != chain && o->pedal == pedal)
                undo = batch(undo, {{"cmd", "set-chain"}, {"chain", other}, {"pedal", pedal}});
    const QString what = c->name + ": " + describe(settings);
    arrange(request, "Not changed", [this, what, chain, undo, request](const QJsonObject&) { s.edited({what, chain, -1, undo, request}); });
}

void ChainView::moveChain(int chain, int scene, int at) {
    const ChainInfo* c = s.chain(chain);
    if (!c || scene < 0 || scene >= s.perf.scenes.size())
        return;
    const QJsonObject request{{"cmd", "move-chain"}, {"chain", chain}, {"scene", scene}, {"at", at}};
    const QJsonObject undo{{"cmd", "move-chain"}, {"chain", chain}, {"scene", c->scene}, {"at", s.perf.scenes[c->scene].chains.indexOf(chain)}};
    const QString what = "Moved " + c->name;
    arrange(request, "Not moved", [this, what, chain, undo, request](const QJsonObject&) {
        s.edited({what, chain, -1, undo, request});
        showChain(chain);
    });
}

void ChainView::newScene(int copy) {
    const int at = shownScene() + 1;
    QJsonObject request{{"cmd", "add-scene"}, {"at", at}};
    if (copy >= 0)
        request["copy"] = copy;
    closeMenu();
    arrange(request, copy >= 0 ? "Not copied" : "No new scene", [this, copy](const QJsonObject& r) {
        const int scene = r.value("scene").toInt(-1);
        const QJsonArray chains = r.value("chains").toArray();
        if (scene < 0 || scene >= s.perf.scenes.size() || chains.isEmpty())
            return;
        const int first = chains.first().toInt();
        const QString token = newToken(token_);
        s.edited({(copy >= 0 ? "Copied scene " : "Added scene ") + s.perf.scenes[scene].name, first, -1,
                  {{"cmd", "remove-scene"}, {"scene", scene}, {"token", token}}, {{"cmd", "restore-scene"}, {"token", token}}});
        // Its settings, to name it.
        strip_ = Strip::Scene;
        showChain(first);
    });
}

void ChainView::removeScene(int scene) {
    if (scene < 0 || scene >= s.perf.scenes.size())
        return;
    const SceneInfo& sc = s.perf.scenes[scene];
    const int first = sc.chains.isEmpty() ? -1 : sc.chains.first();
    const QString token = newToken(token_), name = sc.name;
    const QJsonObject take{{"cmd", "remove-scene"}, {"scene", scene}, {"token", token}};
    arrange(take, "Not taken out", [this, first, name, take, token](const QJsonObject&) {
        s.edited({"Took out scene " + name, first, -1, {{"cmd", "restore-scene"}, {"token", token}}, take});
        strip_ = Strip::Chain;
        showChain(s.playing());
    });
}

void ChainView::setScene(int scene, const QJsonObject& settings) {
    if (scene < 0 || scene >= s.perf.scenes.size())
        return;
    QJsonObject request = settings;
    request["cmd"] = "set-scene";
    request["scene"] = scene;
    QJsonObject undo = sceneNow(scene, settings);
    // A pedal taken from another scene goes back to it.
    const int pedal = settings.value("pedal").toInt(0);
    for (int other = 0; pedal > 0 && other < s.perf.scenes.size(); ++other)
        if (other != scene && s.perf.scenes[other].pedal == pedal)
            undo = batch(undo, {{"cmd", "set-scene"}, {"scene", other}, {"pedal", pedal}});
    const QString what = "Scene " + s.perf.scenes[scene].name + ": " + describe(settings);
    const int chain = chain_;
    arrange(request, "Not changed", [this, what, chain, undo, request](const QJsonObject&) { s.edited({what, chain, -1, undo, request}); });
}

void ChainView::moveScene(int scene, int at) {
    const int n = s.perf.scenes.size();
    if (scene < 0 || scene >= n)
        return;
    const int to = at < 0 || at >= n ? n - 1 : at;
    const QJsonObject request{{"cmd", "move-scene"}, {"scene", scene}, {"at", to}};
    const QJsonObject undo{{"cmd", "move-scene"}, {"scene", to}, {"at", scene}};
    const QString what = "Moved scene " + s.perf.scenes[scene].name;
    const int chain = chain_;
    arrange(request, "Not moved", [this, what, chain, undo, request](const QJsonObject&) { s.edited({what, chain, -1, undo, request}); });
}

} // namespace cue::gui
