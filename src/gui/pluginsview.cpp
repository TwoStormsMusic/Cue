#include "gui/pluginsview.h"

#include <QDateTime>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLineEdit>
#include <QMap>
#include <QPainter>
#include <QStyleHints>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>

namespace cue::gui {

namespace {

const QColor kSelectedRow{0x1F, 0x16, 0x0D};

QString formatText(const ScannedPlugin& p) {
    return p.format.toUpper() + (p.bridged ? " · Wine" : "");
}

QString ago(qint64 at) {
    if (at <= 0)
        return "never";
    const qint64 s = QDateTime::currentSecsSinceEpoch() - at;
    if (s < 90)
        return "just now";
    if (s < 90 * 60)
        return QString("%1 min ago").arg((s + 30) / 60);
    if (s < 36 * 3600)
        return QString("%1 h ago").arg((s + 1800) / 3600);
    return QDateTime::fromSecsSinceEpoch(at).toString("d MMM yyyy");
}

QFont spaced(double px, double u) {
    QFont f = typeface(Face::Text, px, QFont::DemiBold);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * u);
    return f;
}

} // namespace

PluginsView::PluginsView(AppState& state, QWidget* parent) : Surface(state, parent) {
    clock_.start();
    connect(&s, &AppState::catalogChanged, this, qOverload<>(&QWidget::update));
    connect(&s, &AppState::changed, this, qOverload<>(&QWidget::update));
}

void PluginsView::showEvent(QShowEvent*) {
    // What is new or changed since the last scan; nothing, most often.
    if (s.connected())
        s.loadPlugins("changed");
}

void PluginsView::setSearch(const QString& text) {
    lineEdit(search_, "Search plug-ins")->setText(text);
    scroll_ = 0;
    update();
}

void PluginsView::setVendor(const QString& vendor) {
    vendor_ = vendor;
    scroll_ = 0;
    update();
}

void PluginsView::setCollection(Collection c) {
    collection_ = c;
    scroll_ = 0;
    update();
}

void PluginsView::setKind(Kind k) {
    kind_ = k;
    scroll_ = 0;
    update();
}

QVector<int> PluginsView::listed() const {
    const QString q = search_ ? search_->text().trimmed() : QString();
    QVector<int> out;
    const auto& all = s.catalog.plugins;
    for (int i = 0; i < all.size(); ++i) {
        const ScannedPlugin& p = all[i];
        if (!vendor_.isEmpty() && p.vendor != vendor_)
            continue;
        if (collection_ == Collection::Favourites && !s.isFavourite(p))
            continue;
        if (collection_ == Collection::Used && chainsWith(s.perf, p).isEmpty())
            continue;
        if (collection_ == Collection::Skipped && p.ready())
            continue;
        if ((kind_ == Kind::Clap && p.format != "clap") || (kind_ == Kind::Vst3 && p.format != "vst3") || (kind_ == Kind::Linux && p.bridged) ||
            (kind_ == Kind::Windows && !p.bridged))
            continue;
        if (!q.isEmpty() && !p.name.contains(q, Qt::CaseInsensitive) && !p.vendor.contains(q, Qt::CaseInsensitive) &&
            !p.categories.join(' ').contains(q, Qt::CaseInsensitive) && !p.path.section('/', -1).contains(q, Qt::CaseInsensitive))
            continue;
        out << i;
    }
    return out;
}

int PluginsView::selected() const {
    const auto& all = s.catalog.plugins;
    for (int i = 0; i < all.size(); ++i)
        if (all[i].key() + "|" + all[i].name == selectedKey_)
            return i;
    return -1;
}

void PluginsView::select(int index) {
    const auto& all = s.catalog.plugins;
    selectedKey_ = index >= 0 && index < all.size() ? all[index].key() + "|" + all[index].name : QString();
    folders_ = false;
    update();
}

QRectF PluginsView::rowRect(int index) const {
    for (const auto& [i, r] : rows_)
        if (i == index)
            return r;
    return {};
}

bool PluginsView::doubleTap(int index) {
    const qint64 now = clock_.elapsed();
    const bool twice = index == lastTap_ && now - lastTapAt_ < QGuiApplication::styleHints()->mouseDoubleClickInterval();
    lastTap_ = twice ? -1 : index;
    lastTapAt_ = now;
    return twice;
}

void PluginsView::addSelected() {
    const int i = selected();
    if (i < 0 || !add || !target || target().isEmpty())
        return;
    const ScannedPlugin& p = s.catalog.plugins[i];
    if (!p.ready())
        return;
    add(p.toPlugin());
}

QLineEdit* PluginsView::lineEdit(QLineEdit*& edit, const QString& placeholder) {
    if (!edit) {
        edit = new QLineEdit(this);
        edit->setPlaceholderText(placeholder);
        edit->setClearButtonEnabled(true);
        edit->hide();
        connect(edit, &QLineEdit::textChanged, this, [this] {
            scroll_ = 0;
            update();
        });
    }
    return edit;
}

void PluginsView::place(QLineEdit* edit, const QRectF& r) {
    const QRect g = r.toRect();
    if (edit->geometry() != g || edit->font().pixelSize() != std::lround(15 * u)) {
        edit->setGeometry(g);
        edit->setFont(typeface(Face::Text, 15 * u));
        edit->setStyleSheet(QString("QLineEdit { background: %1; color: %2; border: 1px solid %3; border-radius: %4px; padding: 0 %5px; }"
                                    "QLineEdit:focus { border-color: %6; }")
                                .arg(color::panel.name(), color::text.name(), color::border2.name())
                                .arg(std::lround(10 * u))
                                .arg(std::lround(12 * u))
                                .arg(color::orange.name()));
    }
    edit->show();
}

void PluginsView::scrolling(const QString& id, const QRectF& r, double& scroll, double max, std::function<void(QPointF)> tap) {
    hit(id, r, {}, [this, &scroll, max, tap](QPointF pos, bool done) {
        if (!pressing_) {
            pressing_ = true;
            moved_ = false;
            pressAt_ = pos;
            scrollFrom_ = scroll;
        }
        if (!moved_ && std::fabs(pos.y() - pressAt_.y()) + std::fabs(pos.x() - pressAt_.x()) > 10 * u)
            moved_ = true;
        if (moved_) {
            scroll = std::clamp(scrollFrom_ - (pos.y() - pressAt_.y()), 0.0, max);
            update();
        }
        if (done) {
            pressing_ = false;
            if (!moved_ && tap)
                tap(pos);
        }
    });
}

void PluginsView::wheelEvent(QWheelEvent* e) {
    const double step = e->angleDelta().y() / 120.0 * 60 * u;
    if (listRect_.contains(e->position()))
        scroll_ = std::clamp(scroll_ - step, 0.0, scrollMax_);
    else if (sideRect_.contains(e->position()))
        sideScroll_ = std::clamp(sideScroll_ - step, 0.0, sideMax_);
    update();
}

void PluginsView::paint(QPainter& p) {
    const Catalog& cat = s.catalog;
    QString subtitle;
    if (!cat.valid)
        subtitle = "Asking the engine…";
    else if (cat.scanning)
        subtitle = cat.total > 0 ? QString("Scanning %1 of %2").arg(std::min(cat.done + 1, cat.total)).arg(cat.total) : "Scanning";
    else
        subtitle = QString("%1 plug-ins%2 · scanned %3")
                       .arg(cat.ready())
                       .arg(cat.skipped() > 0 ? QString(" · %1 skipped").arg(cat.skipped()) : QString())
                       .arg(ago(cat.scannedAt));
    const double top = header(p, "Plug-ins", subtitle, enginePills(),
                              {{"back", "Back to chains", [this] { emit navigate("chains"); }, color::onOrange, color::orange, color::orange}});
    const double W = width(), H = height();
    const double sideW = std::clamp(220 * u, 140.0, W * 0.2), panelW = std::clamp(310 * u, 200.0, W * 0.3);
    p.fillRect(QRectF(0, top, sideW, H - top), color::panel);
    p.fillRect(QRectF(W - panelW, top, panelW, H - top), color::panel);
    p.setPen(QPen(color::line, 1));
    p.drawLine(QPointF(sideW - 0.5, top), QPointF(sideW - 0.5, H));
    p.drawLine(QPointF(W - panelW + 0.5, top), QPointF(W - panelW + 0.5, H));
    sidebar(p, QRectF(0, top, sideW, H - top));
    list(p, QRectF(sideW, top, W - sideW - panelW, H - top));
    if (folders_) {
        foldersPanel(p, QRectF(W - panelW, top, panelW, H - top));
    } else {
        if (folder_)
            folder_->hide();
        details(p, QRectF(W - panelW, top, panelW, H - top));
    }
}

// Vendors and collections, each with how many it has; Rescan and Folders
// under them.
void PluginsView::sidebar(QPainter& p, const QRectF& r) {
    const Catalog& cat = s.catalog;
    const double pad = 12 * u, rowH = 36 * u, headH = 34 * u, btnH = 40 * u, gap = 8 * u;
    QMap<QString, int> vendors; // sorted, ignoring case
    QMap<QString, QString> names;
    int favourites = 0, used = 0;
    for (const ScannedPlugin& sp : cat.plugins) {
        if (!sp.vendor.isEmpty()) {
            ++vendors[sp.vendor.toLower()];
            names[sp.vendor.toLower()] = sp.vendor;
        }
        favourites += s.isFavourite(sp);
        used += !chainsWith(s.perf, sp).isEmpty();
    }
    struct Item {
        QString head, text;
        int count = -1;
        bool on = false;
        std::function<void()> tap;
    };
    QVector<Item> items;
    items << Item{"VENDOR", {}, -1, false, {}};
    items << Item{{}, "All", static_cast<int>(cat.plugins.size()), vendor_.isEmpty(), [this] { setVendor({}); }};
    for (auto it = vendors.begin(); it != vendors.end(); ++it) {
        const QString v = names[it.key()];
        items << Item{{}, v, it.value(), vendor_ == v, [this, v] { setVendor(v); }};
    }
    items << Item{"COLLECTIONS", {}, -1, false, {}};
    items << Item{{}, "Everything", -1, collection_ == Collection::All, [this] { setCollection(Collection::All); }};
    items << Item{{}, "Favourites", favourites, collection_ == Collection::Favourites, [this] { setCollection(Collection::Favourites); }};
    items << Item{{}, "In this performance", used, collection_ == Collection::Used, [this] { setCollection(Collection::Used); }};
    items << Item{{}, "Skipped", cat.skipped(), collection_ == Collection::Skipped, [this] { setCollection(Collection::Skipped); }};

    const QRectF area(r.left(), r.top() + 10 * u, r.width(), r.height() - 2 * btnH - gap - 34 * u);
    double contentH = 0;
    for (const Item& it : items)
        contentH += it.head.isEmpty() ? rowH : headH;
    sideRect_ = area;
    sideMax_ = std::max(0.0, contentH - area.height());
    sideScroll_ = std::clamp(sideScroll_, 0.0, sideMax_);
    sideItems_.clear();
    p.save();
    p.setClipRect(area);
    double y = area.top() - sideScroll_;
    for (const Item& it : items) {
        if (!it.head.isEmpty()) {
            p.setFont(spaced(12 * u, u));
            p.setPen(color::muted);
            p.drawText(QRectF(r.left() + pad + 10 * u, y + 6 * u, r.width() - 2 * pad, headH - 6 * u), Qt::AlignLeft | Qt::AlignVCenter, it.head);
            y += headH;
            continue;
        }
        const QRectF row(r.left() + pad, y + 2 * u, r.width() - 2 * pad, rowH - 4 * u);
        if (it.on)
            draw::card(p, row, 8 * u, color::raised, QColor());
        p.setPen(it.on ? color::text : color::text2);
        p.setFont(typeface(Face::Text, 14 * u, it.on ? QFont::DemiBold : QFont::Medium));
        const double countW = it.count >= 0 ? 40 * u : 0;
        draw::text(p, row.adjusted(10 * u, 0, -10 * u - countW, 0), Qt::AlignLeft | Qt::AlignVCenter, it.text);
        if (it.count >= 0) {
            p.setPen(color::dim);
            p.setFont(typeface(Face::Mono, 12 * u));
            p.drawText(row.adjusted(0, 0, -10 * u, 0), Qt::AlignRight | Qt::AlignVCenter, QString::number(it.count));
        }
        sideItems_.append({row, it.tap});
        y += rowH;
    }
    p.restore();
    scrolling("side", area, sideScroll_, sideMax_, [this](QPointF pos) {
        for (const auto& [rect, tap] : sideItems_)
            if (rect.contains(pos) && tap) {
                const auto run = tap;
                run();
                return;
            }
    });
    const double bx = r.left() + pad, bw = r.width() - 2 * pad;
    double by = r.bottom() - 2 * btnH - gap - 14 * u;
    button(p, "rescan", QRectF(bx, by, bw, btnH), cat.scanning ? "Scanning…" : "Rescan",
           [this] {
               if (!s.catalog.scanning)
                   s.loadPlugins("all");
           },
           cat.scanning ? color::dim : color::text, color::border2, color::panel2, 14 * u);
    by += btnH + gap;
    button(p, "folders", QRectF(bx, by, bw, btnH), "Folders…",
           [this] {
               folders_ = !folders_;
               update();
           },
           folders_ ? color::onOrange : color::text, folders_ ? color::orange : color::border2, folders_ ? color::orange : color::panel2, 14 * u);
}

// The search and the formats, then the plug-ins: name, vendor, format and
// whether each is ready.
void PluginsView::list(QPainter& p, const QRectF& r) {
    const Catalog& cat = s.catalog;
    const double pad = 18 * u, chipH = 36 * u;
    double y = r.top() + 16 * u;
    const double w = r.width() - 2 * pad;
    // The chips beside the search, or under it when there is no room.
    const QStringList chips = {"All", "CLAP", "VST3", "Linux", "Windows (yabridge)"};
    const QFont chipFont = typeface(Face::Text, 13 * u, QFont::Medium);
    const QFontMetricsF cfm(chipFont);
    double chipsW = 0;
    for (const QString& c : chips)
        chipsW += cfm.horizontalAdvance(c) + 26 * u + 8 * u;
    const bool beside = w - chipsW > 240 * u;
    const double searchW = beside ? w - chipsW - 6 * u : w;
    place(lineEdit(search_, "Search plug-ins"), QRectF(r.left() + pad, y, searchW, 42 * u));
    double cx = beside ? r.left() + pad + searchW + 10 * u : r.left() + pad;
    double cy = beside ? y + 3 * u : y + 52 * u;
    for (int i = 0; i < chips.size(); ++i) {
        const double cw = cfm.horizontalAdvance(chips[i]) + 26 * u;
        if (!beside && cx + cw > r.right() - pad) {
            cx = r.left() + pad;
            cy += chipH + 6 * u;
        }
        const QRectF chip(cx, cy, cw, chipH);
        const bool on = static_cast<int>(kind_) == i;
        draw::card(p, chip, chipH / 2, on ? color::text : (isPressed("kind " + chips[i]) ? color::raised : QColor(Qt::transparent)),
                   on ? color::text : color::border2);
        p.setPen(on ? color::bg : color::text2);
        p.setFont(chipFont);
        p.drawText(chip, Qt::AlignCenter, chips[i]);
        hit("kind " + chips[i], chip, [this, i] { setKind(static_cast<Kind>(i)); });
        cx += cw + 8 * u;
    }
    y = (beside ? y + 42 * u : cy + chipH) + 14 * u;

    // The table, in a card, its header fixed and its rows scrolling.
    const double hintH = 56 * u;
    const QRectF box(r.left() + pad, y, w, r.bottom() - y - hintH - 8 * u);
    draw::card(p, box, 14 * u, color::panel, color::line);
    const double rowH = 46 * u, headH = 38 * u, cellPad = 12 * u;
    const double nameW = box.width() * 0.38, vendorW = box.width() * 0.22, formatW = box.width() * 0.16;
    auto columns = [&](double top, double h) {
        return std::array<QRectF, 4>{QRectF(box.left() + cellPad, top, nameW - cellPad, h), QRectF(box.left() + nameW + cellPad, top, vendorW - cellPad, h),
                                     QRectF(box.left() + nameW + vendorW + cellPad, top, formatW - cellPad, h),
                                     QRectF(box.left() + nameW + vendorW + formatW + cellPad, top, box.width() - nameW - vendorW - formatW - 2 * cellPad, h)};
    };
    p.setFont(spaced(12 * u, u));
    p.setPen(color::muted);
    const auto head = columns(box.top(), headH);
    const char* titles[] = {"NAME", "VENDOR", "FORMAT", "STATUS"};
    for (int c = 0; c < 4; ++c)
        p.drawText(head[c], Qt::AlignLeft | Qt::AlignVCenter, titles[c]);
    p.setPen(QPen(color::line, 1));
    p.drawLine(QPointF(box.left(), box.top() + headH), QPointF(box.right(), box.top() + headH));

    const QVector<int> shown = listed();
    const QRectF rowsArea(box.left() + 1, box.top() + headH + 1, box.width() - 2, box.height() - headH - 2);
    listRect_ = rowsArea;
    scrollMax_ = std::max(0.0, shown.size() * rowH - rowsArea.height());
    scroll_ = std::clamp(scroll_, 0.0, scrollMax_);
    rows_.clear();
    const int sel = selected();
    p.save();
    p.setClipRect(rowsArea);
    for (int k = 0; k < shown.size(); ++k) {
        const QRectF row(rowsArea.left(), rowsArea.top() + k * rowH - scroll_, rowsArea.width(), rowH);
        if (row.bottom() < rowsArea.top() || row.top() > rowsArea.bottom())
            continue;
        const int i = shown[k];
        const ScannedPlugin& sp = cat.plugins[i];
        rows_.append({i, row.intersected(rowsArea)});
        if (i == sel)
            p.fillRect(row, kSelectedRow);
        const auto cells = columns(row.top(), rowH);
        p.setFont(typeface(Face::Text, 14.5 * u, i == sel ? QFont::DemiBold : QFont::Medium));
        p.setPen(i == sel ? color::orange : sp.ready() ? color::text : color::muted);
        draw::text(p, cells[0], Qt::AlignLeft | Qt::AlignVCenter, sp.name);
        p.setFont(typeface(Face::Text, 14 * u));
        p.setPen(color::text2);
        draw::text(p, cells[1], Qt::AlignLeft | Qt::AlignVCenter, sp.vendor);
        p.setFont(typeface(Face::Mono, 12 * u));
        p.setPen(color::muted);
        draw::text(p, cells[2], Qt::AlignLeft | Qt::AlignVCenter, formatText(sp));
        p.setFont(typeface(Face::Text, 13 * u));
        p.setPen(sp.ready() ? color::green : color::red);
        draw::text(p, cells[3], Qt::AlignLeft | Qt::AlignVCenter, sp.ready() ? (s.isFavourite(sp) ? "ready · favourite" : "ready") : "skipped: " + sp.error);
        p.setPen(QPen(color::line, 1));
        p.drawLine(QPointF(row.left(), row.bottom()), QPointF(row.right(), row.bottom()));
    }
    p.restore();
    if (scrollMax_ > 0) {
        const double bh = rowsArea.height() * rowsArea.height() / (rowsArea.height() + scrollMax_);
        const double by = rowsArea.top() + (rowsArea.height() - bh) * scroll_ / scrollMax_;
        p.fillRect(QRectF(rowsArea.right() - 5 * u, by, 3 * u, bh), color::border2);
    }
    if (shown.isEmpty()) {
        QString why;
        if (!cat.valid)
            why = s.connected() ? "Asking the engine for its plug-ins…" : "The engine is not running.";
        else if (cat.plugins.isEmpty())
            why = cat.scanning ? "Looking for plug-ins…"
                               : "No plug-ins found. Cue looks in ~/.clap, ~/.vst3 and the system's plug-in folders on the computer running the "
                                 "engine. Add a folder with Folders…, then Rescan.";
        else
            why = "Nothing here matches.";
        p.setPen(color::muted);
        p.setFont(typeface(Face::Text, 15 * u));
        p.drawText(rowsArea.adjusted(24 * u, 24 * u, -24 * u, -24 * u), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, why);
    }
    scrolling("list", rowsArea, scroll_, scrollMax_, [this](QPointF pos) {
        for (const auto& [i, rect] : rows_)
            if (rect.contains(pos)) {
                const bool twice = doubleTap(i);
                select(i);
                if (twice)
                    addSelected();
                return;
            }
    });

    // Under it, how it scans, or how far the scan is.
    const QRectF hint(r.left() + pad, box.bottom() + 8 * u, w, hintH);
    if (cat.scanning) {
        const QRectF bar(hint.left(), hint.top() + 10 * u, hint.width(), 6 * u);
        draw::card(p, bar, 3 * u, color::line, QColor());
        const double f = cat.total > 0 ? std::clamp(double(cat.done) / cat.total, 0.0, 1.0) : 0;
        draw::card(p, QRectF(bar.left(), bar.top(), std::max(bar.height(), bar.width() * f), bar.height()), 3 * u, color::orange, QColor());
        p.setPen(color::muted);
        p.setFont(typeface(Face::Mono, 12 * u));
        draw::text(p, QRectF(hint.left(), bar.bottom() + 6 * u, hint.width(), 22 * u), Qt::AlignLeft | Qt::AlignVCenter,
                   cat.current.isEmpty() ? QString("Scanning…") : "Opening " + cat.current.section('/', -1));
    } else {
        p.setPen(color::muted);
        p.setFont(typeface(Face::Text, 13 * u));
        p.drawText(hint, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                   "Each plug-in file is opened by a helper of its own, one at a time, so one that hangs or crashes is skipped and "
                   "listed, never taking Cue down. Double-tap a plug-in to add it.");
    }
}

// The plug-in selected: what it is, what is known of it, and the buttons
// that add it and keep it as a favourite.
void PluginsView::details(QPainter& p, const QRectF& r) {
    const double pad = 18 * u, w = r.width() - 2 * pad;
    double y = r.top() + 20 * u;
    p.setFont(spaced(12 * u, u));
    p.setPen(color::muted);
    p.drawText(QRectF(r.left() + pad, y, w, 18 * u), Qt::AlignLeft | Qt::AlignVCenter, "PLUG-IN");
    y += 30 * u;
    const int i = selected();
    if (i < 0) {
        p.setPen(color::muted);
        p.setFont(typeface(Face::Text, 15 * u));
        p.drawText(QRectF(r.left() + pad, y, w, r.bottom() - y - 20 * u), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                   "Tap a plug-in to see it here. Add puts it in the chain the chain view shows, or on its selected wire.");
        return;
    }
    const ScannedPlugin& sp = s.catalog.plugins[i];
    p.setPen(color::text);
    p.setFont(draw::fitted(Face::Text, QFont::Bold, sp.name, 21 * u, 14 * u, w));
    draw::text(p, QRectF(r.left() + pad, y, w, 30 * u), Qt::AlignLeft | Qt::AlignVCenter, sp.name);
    y += 32 * u;
    QStringList what;
    if (!sp.vendor.isEmpty())
        what << sp.vendor;
    what << sp.format.toUpper() << (sp.bridged ? "Windows, through yabridge" : "native Linux");
    p.setPen(color::muted);
    p.setFont(typeface(Face::Text, 13.5 * u));
    const QRectF wr(r.left() + pad, y, w, 60 * u);
    const QRectF wrote = p.boundingRect(wr, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, what.join(" · "));
    p.drawText(wr, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, what.join(" · "));
    y += wrote.height() + 16 * u;

    if (!sp.ready()) {
        const QRectF card(r.left() + pad, y, w, 0);
        p.setFont(typeface(Face::Text, 14 * u));
        const QRectF used = p.boundingRect(card.adjusted(14 * u, 12 * u, -14 * u, 400 * u), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                                           "Skipped: " + sp.error);
        const QRectF box(card.left(), y, w, used.height() + 24 * u);
        draw::card(p, box, 10 * u, color::redBg, color::red);
        p.setPen(color::red);
        p.drawText(box.adjusted(14 * u, 12 * u, -14 * u, -12 * u), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, "Skipped: " + sp.error);
        y = box.bottom() + 14 * u;
    }

    // What is known, as a column of names and values.
    QVector<QPair<QString, QString>> kv;
    if (!sp.id.isEmpty())
        kv.append({"ID", sp.id});
    kv.append({"File", sp.path});
    if (!sp.version.isEmpty())
        kv.append({"Version", sp.version});
    if (sp.ready()) {
        const QString unknown = sp.bridged ? "known once added" : "not known";
        kv.append({"Inputs", sp.opened ? (sp.sidechain ? "stereo + side-chain" : "stereo") : unknown});
        kv.append({"Own editor", sp.opened ? (sp.editor ? "yes" : "no") : unknown});
        kv.append({"MIDI in", sp.opened ? (sp.midi ? "yes" : "no") : unknown});
    }
    if (!sp.categories.isEmpty())
        kv.append({"Kind", sp.categories.join(", ")});
    const QStringList chains = chainsWith(s.perf, sp);
    kv.append({"Used in", chains.isEmpty() ? "no chain yet" : chains.join(", ")});
    const double keyW = 92 * u;
    const double buttonsH = 2 * 46 * u + 10 * u + 20 * u;
    for (const auto& [k, v] : kv) {
        if (y > r.bottom() - buttonsH - 24 * u)
            break;
        p.setPen(color::muted);
        p.setFont(typeface(Face::Text, 13 * u));
        p.drawText(QRectF(r.left() + pad, y, keyW, 20 * u), Qt::AlignLeft | Qt::AlignTop, k);
        const bool mono = k == "ID" || k == "File";
        p.setFont(mono ? typeface(Face::Mono, 11.5 * u) : typeface(Face::Text, 13 * u, QFont::Medium));
        p.setPen(color::text2);
        const QRectF vr(r.left() + pad + keyW, y, w - keyW, r.bottom() - buttonsH - y);
        const QRectF used = p.boundingRect(vr, Qt::AlignLeft | Qt::AlignTop | Qt::TextWrapAnywhere, v);
        p.drawText(vr, Qt::AlignLeft | Qt::AlignTop | Qt::TextWrapAnywhere, v);
        y += std::max(20 * u, used.height()) + 8 * u;
    }

    const double bh = 46 * u;
    const QString where = target ? target() : QString();
    const bool can = sp.ready() && !where.isEmpty();
    double by = r.bottom() - 2 * bh - 10 * u - 20 * u;
    if (!sp.ready()) {
        // Opened again by itself: it may have been mended.
        button(p, "try again", QRectF(r.left() + pad, by, w, bh), "Try again",
               [this, path = sp.path] {
                   if (!s.catalog.scanning)
                       s.loadPlugins(path);
               },
               color::text, color::border2, color::panel2, 15 * u);
    } else {
        button(p, "add", QRectF(r.left() + pad, by, w, bh), where.isEmpty() ? "No chain to add it to" : where, [this, can] {
            if (can)
                addSelected();
        },
               can ? color::onOrange : color::dim, can ? color::orange : color::border2, can ? color::orange : color::panel2, 15 * u);
    }
    by += bh + 10 * u;
    const bool fav = s.isFavourite(sp);
    button(p, "favourite", QRectF(r.left() + pad, by, w, bh), fav ? "Remove from favourites" : "Add to favourites",
           [this, sp, fav] { s.setFavourite(sp, !fav); }, fav ? color::orange : color::text, color::border2, color::panel2, 15 * u);
}

// Where the engine looks: the usual folders, and the user's, which can be
// added to and taken away.
void PluginsView::foldersPanel(QPainter& p, const QRectF& r) {
    const Catalog& cat = s.catalog;
    const double pad = 18 * u, w = r.width() - 2 * pad;
    double y = r.top() + 20 * u;
    p.setFont(spaced(12 * u, u));
    p.setPen(color::muted);
    p.drawText(QRectF(r.left() + pad, y, w, 18 * u), Qt::AlignLeft | Qt::AlignVCenter, "FOLDERS");
    y += 28 * u;
    auto para = [&](const QString& text, const QColor& c) {
        p.setPen(c);
        p.setFont(typeface(Face::Text, 13.5 * u));
        const QRectF tr(r.left() + pad, y, w, 200 * u);
        const QRectF used = p.boundingRect(tr, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
        p.drawText(tr, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
        y += used.height() + 10 * u;
    };
    para("On the computer running the engine, Cue looks in these and every folder inside them:", color::text2);
    p.setFont(typeface(Face::Mono, 11.5 * u));
    p.setPen(color::muted);
    for (const QString& d : cat.usual) {
        draw::text(p, QRectF(r.left() + pad, y, w, 18 * u), Qt::AlignLeft | Qt::AlignVCenter, d);
        y += 20 * u;
    }
    y += 12 * u;
    para(cat.extra.isEmpty() ? "And in folders you add:" : "And in yours:", color::text2);
    for (int i = 0; i < cat.extra.size(); ++i) {
        const double rbw = 78 * u;
        p.setFont(typeface(Face::Mono, 11.5 * u));
        p.setPen(color::text);
        draw::text(p, QRectF(r.left() + pad, y, w - rbw - 8 * u, 34 * u), Qt::AlignLeft | Qt::AlignVCenter, cat.extra[i]);
        button(p, QString("remove folder %1").arg(i), QRectF(r.right() - pad - rbw, y, rbw, 34 * u), "Remove",
               [this, i] {
                   QStringList extra = s.catalog.extra;
                   if (i < extra.size())
                       extra.removeAt(i);
                   s.setPluginFolders(extra);
               },
               color::red, color::border2, color::panel2, 13 * u);
        y += 42 * u;
    }
    const double addW = 64 * u;
    place(lineEdit(folder_, "/path/to/plugins"), QRectF(r.left() + pad, y, w - addW - 8 * u, 40 * u));
    button(p, "add folder", QRectF(r.right() - pad - addW, y, addW, 40 * u), "Add",
           [this] {
               const QString d = folder_ ? folder_->text().trimmed() : QString();
               if (d.isEmpty())
                   return;
               s.setPluginFolders(s.catalog.extra + QStringList{d});
               folder_->clear();
           },
           color::text, color::border2, color::panel2, 14 * u);
    y += 52 * u;
    para("A folder added is scanned at once; Rescan opens every plug-in again.", color::muted);
    button(p, "done", QRectF(r.left() + pad, r.bottom() - 46 * u - 20 * u, w, 46 * u), "Done",
           [this] {
               folders_ = false;
               update();
           },
           color::text, color::border2, color::panel2, 15 * u);
}

} // namespace cue::gui
