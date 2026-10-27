#pragma once

// The plug-ins view: every plug-in the engine's scan found on its machine,
// as the plug-in browser mockup draws it. Vendors and collections
// (favourites, those in the performance, those skipped) on the left; a
// search and the formats over the list; the one selected on the right,
// with what is known of it and the button that adds it to the chain the
// chain view shows (or onto its selected wire). A double tap on a row adds
// it too. Rescan opens every file again; Folders adds folders to look in.
// The scan runs in the engine, so the folders are the engine's machine's.

#include "gui/surface.h"

#include <QElapsedTimer>
#include <functional>

class QLineEdit;

namespace cue::gui {

class PluginsView : public Surface {
    Q_OBJECT
public:
    explicit PluginsView(AppState& state, QWidget* parent = nullptr);

    // Where the add button puts a plug-in, as it says it ("Add to Slap"),
    // and what does it. Set by the window, from the chain view.
    std::function<QString()> target;
    std::function<void(const PluginInfo&)> add;

    enum class Collection { All, Favourites, Used, Skipped };
    enum class Kind { All, Clap, Vst3, Linux, Windows };
    void setSearch(const QString& text);
    void setVendor(const QString& vendor); // empty: all
    void setCollection(Collection c);
    void setKind(Kind k);
    // The plug-ins listed, as places in the catalog, in their order.
    QVector<int> listed() const;
    // The one selected, as a place in the catalog, or -1.
    int selected() const;
    void select(int index);
    bool showingFolders() const { return folders_; }
    // Where a listed plug-in's row was last drawn (empty if it was not).
    QRectF rowRect(int index) const;

protected:
    void paint(QPainter& p) override;
    void showEvent(QShowEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    void sidebar(QPainter& p, const QRectF& r);
    void list(QPainter& p, const QRectF& r);
    void details(QPainter& p, const QRectF& r);
    void foldersPanel(QPainter& p, const QRectF& r);
    void addSelected();
    bool doubleTap(int index);
    QLineEdit* lineEdit(QLineEdit*& edit, const QString& placeholder);
    void place(QLineEdit* edit, const QRectF& r);
    // A list that scrolls when dragged and taps where it is let go
    // without moving.
    void scrolling(const QString& id, const QRectF& r, double& scroll, double max, std::function<void(QPointF)> tap);

    QString vendor_;
    Collection collection_ = Collection::All;
    Kind kind_ = Kind::All;
    QString selectedKey_; // by key and name, so a rescan keeps it
    bool folders_ = false;
    QLineEdit* search_ = nullptr;
    QLineEdit* folder_ = nullptr;
    double scroll_ = 0, scrollMax_ = 0, sideScroll_ = 0, sideMax_ = 0;
    QRectF listRect_, sideRect_;
    QVector<QPair<int, QRectF>> rows_;              // listed plug-in, its row
    QVector<QPair<QRectF, std::function<void()>>> sideItems_;
    QPointF pressAt_;
    double scrollFrom_ = 0;
    bool pressing_ = false, moved_ = false;
    QElapsedTimer clock_;
    int lastTap_ = -1;
    qint64 lastTapAt_ = 0;
};

} // namespace cue::gui
