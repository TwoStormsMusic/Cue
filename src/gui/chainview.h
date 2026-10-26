#pragma once

// The chain view: a chain's plug-ins as blocks and wires, as the routing
// mockup draws them, laid out from the performance. The canvas zooms (wheel,
// pinch, the buttons in its corner) and pans (drag open space, two fingers);
// a double tap on open space fits the chain again. Tapping a block shows its
// picked parameters as sliders, in the order picked; a double tap switches
// it on or off, and one on a chain in the list plays it. "Show all
// parameters" lists every one to pick from. Undo takes back what was changed
// here. Save keeps it all in the .cue file. Moving and wiring blocks comes
// with the routing editor.

#include "gui/surface.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTimer>

class QLineEdit;

namespace cue::gui {

class ChainView : public Surface {
    Q_OBJECT
public:
    explicit ChainView(AppState& state, QWidget* parent = nullptr);

    // The chain shown (the one playing when the view opens) and the
    // plug-in selected in it, -1 for none.
    int shownChain() const { return chain_; }
    int selectedSlot() const { return slot_; }
    void showChain(int chain);
    void selectSlot(int slot);

    // The canvas: 1 fits the chain; more is closer. pan is in pixels from
    // where fitting puts it.
    double zoom() const { return zoom_; }
    QPointF pan() const { return pan_; }
    void zoomAt(QPointF pos, double factor);
    void fit();

    // The selected plug-in's parameters as the panel shows them: the picked
    // ones in their order, then any a pedal or MIDI moves; and whether the
    // panel lists them all.
    QVector<int> shownParams() const;
    bool showingAll() const { return showAll_; }
    void setShowAll(bool all);
    void setShown(const QVector<int>& ids);

protected:
    void paint(QPainter& p) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    bool event(QEvent* e) override;

private:
    void sidebar(QPainter& p, const QRectF& r);
    void canvas(QPainter& p, const QRectF& r, const ChainInfo& c);
    void legend(QPainter& p, const QRectF& r);
    void panel(QPainter& p, const QRectF& r, const ChainInfo& c);
    void requestPlugin();
    void setParam(const QJsonObject& param, double value, bool done);
    void setBypass(int chain, int slot, bool bypassed);
    bool bypassed(int chain, int slot) const;
    // Whether this tap, on what is called id, makes a double tap of the last.
    bool doubleTap(const QString& id);
    void paramRow(QPainter& p, const QRectF& row, const QRectF& list, const QJsonObject& q, const QString& format, bool all);
    void allParams(QPainter& p, const QRectF& r, const QJsonArray& params, const QString& format);
    void shownList(QPainter& p, const QRectF& r, const QJsonArray& params, const QString& format);
    void scrollable(const QRectF& list, double contentH);
    bool tied(int id) const;
    // Where the chain sits: its size in layout units, and the canvas.
    QPointF origin(double k) const;
    double scaleNow() const { return fitK_ * zoom_; }
    void clampPan();

    int chain_ = -1, slot_ = -1;
    bool followPlaying_ = true;
    QJsonObject plugin_; // the selected plug-in as the engine last described it
    QTimer refresh_;
    double scroll_ = 0, scrollMax_ = 0;
    QRectF listRect_;
    // A slider being dragged shows where the finger is, not the engine.
    int draggingParam_ = -1;
    double draggingValue_ = 0, draggingFrom_ = 0;
    QElapsedTimer clock_;
    qint64 lastSend_ = 0;
    double scrollFrom_ = 0, scrollStartY_ = -1;

    // The canvas, as last drawn, and how it is zoomed and panned.
    QRectF canvasRect_;
    QSizeF layoutSize_;
    double fitK_ = 1, zoom_ = 1;
    QPointF pan_;
    // A drag on open space: where it started; a tap there, for a double tap.
    QPointF panStart_, panFrom_;
    bool panning_ = false, panMoved_ = false;
    qint64 lastTapAt_ = -1000;
    QPointF lastTapPos_;
    // The last tap on a chain or a block, for a double tap; a tap on the
    // selected block lets it go only once it is clear it was not the first
    // of two.
    QString lastTapOn_;
    qint64 lastTapOnAt_ = -1000;
    QTimer letGo_;
    // Two fingers: the zoom and where the point between them was at the start.
    bool pinching_ = false, touchDone_ = false;
    double pinchDist_ = 0, pinchZoom_ = 1;
    QPointF pinchAt_; // in layout units

    // The parameters panel.
    bool showAll_ = false;
    QJsonArray pendingShown_; // the picks last sent
    int shownPending_ = 0; // picks sent, not yet answered
    int dragRow_ = -1; // being moved by its grip, as an index into shownParams()
    double dragY_ = 0, dragGrab_ = 0;
    QSet<QString> closed_, opened_; // sections folded or unfolded by hand
    QLineEdit* search_ = nullptr;
};

} // namespace cue::gui
