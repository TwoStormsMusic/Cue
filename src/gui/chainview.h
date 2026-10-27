#pragma once

// The chain view: the routing board. A chain's plug-ins are blocks and its
// wires join them, as the routing mockup draws them. The canvas zooms
// (wheel, pinch, the buttons in its corner) and pans (drag open space, two
// fingers); a double tap on open space fits the chain again.
//
// Tapping a block shows its picked parameters as sliders, in the order
// picked; a double tap switches it on or off, and one on a chain in the
// list plays it. "Show all parameters" lists every one to pick from. A
// block is dragged to move it; a drag from one of its dots draws a wire.
// Tapping a wire shows its level, width and pan; its + puts a plug-in on
// it. Delete takes away what is selected. Undo takes back what was changed
// here, wiring and plug-ins added or taken out too. Save keeps it all in
// the .cue file.

#include "gui/board.h"
#include "gui/surface.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainterPath>
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
    // The next time it is shown, it stays on the chain and what is selected
    // (back from the plug-ins view), rather than going to the chain playing.
    void resume() { resume_ = true; }
    // The wire selected, by its place in the chain's wires, or -1.
    int selectedWire() const;
    void selectWire(int wire);
    // Takes away the selected wire or plug-in; false if there is none.
    bool deleteSelected();
    // Lets go of what is selected; false if nothing was.
    bool deselect();
    // Where the plug-ins view adds one, said on its button: on the selected
    // wire, else to the chain shown, unwired. Empty with no chain.
    QString addTarget() const;
    void addFromBrowser(const PluginInfo& plugin);
    // Where a board point is on the screen, as last drawn, and back.
    QPointF toScreen(QPointF board) const;
    QPointF toBoard(QPointF screen) const;

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
    // The routing board.
    void pin(const ChainInfo& c);
    void tapBlock(int slot);
    void dragBlock(int slot, QPointF pos, bool done);
    void dragWire(const PortInfo& port, bool source, QPointF pos, bool done);
    bool portAt(QPointF pos, bool input, PortInfo& out) const;
    void connectPorts(PortInfo from, PortInfo to);
    void removeWire(int wire);
    void removePlugin(int slot);
    void addPlugin(const PluginInfo& what);
    void setGains(const WireInfo& gains, bool done);
    WireInfo wireNow(const ChainInfo& c, int wire) const;
    QString portName(const ChainInfo& c, const PortInfo& p) const;
    void wirePanel(QPainter& p, const QRectF& r, const ChainInfo& c);
    void pickerPanel(QPainter& p, const QRectF& r, const ChainInfo& c);
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
    bool resume_ = false;
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
    QPointF boardTopLeft_; // the board point at the layout's corner
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
    // A block being dragged, and blocks where they are until the engine has them.
    int dragBlock_ = -1;
    bool dragMoved_ = false;
    QPointF dragStart_, dragFrom_;
    QHash<int, QPointF> moving_;
    // A wire being drawn from a dot: the port, whether it is where sound
    // comes out, and where the finger is.
    bool wiring_ = false, wiringSource_ = false;
    PortInfo wiringPort_;
    QPointF wiringAt_;
    struct PortSpot {
        PortInfo port;
        bool input; // where sound goes in
        QRectF r;
    };
    QVector<PortSpot> ports_;     // as last drawn
    QVector<QPainterPath> wireShapes_; // where each wire can be tapped, as last drawn
    // The wire selected, by its ends, so it is found again when the
    // performance comes back changed.
    bool wireOn_ = false;
    PortInfo wireFrom_, wireTo_;
    // Its gains while a slider moves, and as they were before.
    int gainDrag_ = -1; // 0 level, 1 width, 2 pan
    bool gainReset_ = false;
    std::optional<WireInfo> gainsNow_;
    WireInfo gainsFrom_;
    qint64 lastWireSend_ = 0;
    // Adding a plug-in: where (a wire, or a free place on the board).
    enum class Picker { None, Add, Insert } picker_ = Picker::None;
    QPointF addAt_;
    int token_ = 0; // numbers what undo keeps of a plug-in taken out
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
