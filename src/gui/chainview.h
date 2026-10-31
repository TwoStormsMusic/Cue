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
// Every plug-in, and the chain's input, has outputs (1-2, 3-4...), each a
// dot on its right with a level, width, pan and mute of its own; a wire
// carries the output it leaves. Tapping a wire shows its output's; its +
// puts a plug-in on it. Delete takes away what is selected. Undo takes back what was changed
// here, wiring and plug-ins added or taken out too. Save keeps it all in
// the .cue file.
//
// The column on the left lists the chains of one scene (picked at its top)
// and makes new chains and scenes, or copies chains from other scenes. Under
// the board are the shown chain's settings (its name, pedal, display, the
// chain kept warm while it plays and what selecting it does) or its scene's;
// each can be copied, moved or taken out there (chainsettings.cpp).

#include "gui/board.h"
#include "gui/surface.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainterPath>
#include <QSet>
#include <QTimer>

#include <optional>

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
    // Plug-ins selected together (Shift, or Select), to move at once.
    const QSet<int>& group() const { return group_; }
    void showChain(int chain);
    void selectSlot(int slot);
    // The next time it is shown, it stays on the chain and what is selected
    // (back from the plug-ins view), rather than going to the chain playing.
    void resume() { resume_ = true; }
    // The wire selected, by its place in the chain's wires, or -1.
    int selectedWire() const;
    // While a wire is being drawn: what it would join where the finger is.
    bool wiringTarget(PortInfo& to) const { return wiring_ && portAt(wiringAt_, wiringSource_, to); }
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

    // The scene the column lists: the shown chain's. Showing another shows
    // its chain playing, else its first.
    int shownScene() const;
    void showScene(int scene);
    // What the settings under the board are for.
    enum class Strip { Chain, Scene };
    Strip strip() const { return strip_; }
    void setStrip(Strip what);
    // Chains and scenes, asked of the engine and kept for undo. A new chain
    // goes into the shown scene (copy: a chain to copy, from any scene); a
    // new scene after the shown one (copy: a scene to copy). settings are
    // as set-chain and set-scene take them (docs/control.md).
    void newChain(int copy = -1);
    void removeChain(int chain);
    void setChain(int chain, const QJsonObject& settings);
    void moveChain(int chain, int scene, int at);
    void newScene(int copy = -1);
    void removeScene(int scene);
    void setScene(int scene, const QJsonObject& settings);
    void moveScene(int scene, int at);
    // The menu open (a field's choices), its items as listed, and picking one.
    bool menuOpen() const { return menu_.has_value(); }
    QStringList menuItems() const;
    void pickMenu(int item);
    // A field being typed in (a name, a display, an action): where, and
    // what typing there and pressing Enter does.
    QString entryField() const;
    void typeEntry(const QString& text);

protected:
    void paint(QPainter& p) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    bool event(QEvent* e) override;

private:
    void sidebar(QPainter& p, const QRectF& r);
    void canvas(QPainter& p, const QRectF& r, const ChainInfo& c);
    void boardTools(QPainter& p, const QRectF& r);
    void dragJump(int wire, bool out, QPointF pos, bool done);
    // Several plug-ins selected, to move together: by Shift, or Select for touch.
    bool adding() const;
    void boxSelect(const QRectF& screenBox);
    void legend(QPainter& p, const QRectF& r);
    // The settings under the board, and their height.
    double stripHeight(const ChainInfo& c, double width) const;
    void settingsStrip(QPainter& p, const QRectF& r, const ChainInfo& c);
    void chainFields(QPainter& p, const QRectF& r, const ChainInfo& c);
    void sceneFields(QPainter& p, const QRectF& r, int scene);
    // A setting: its label, and a box with its value that a tap opens (a
    // menu, with ▾, or typing).
    void field(QPainter& p, const QString& id, const QRectF& r, const QString& label, const QString& value, bool menu,
               std::function<void()> tap);
    struct MenuItem {
        QString text, detail;
        bool checked = false;
        std::function<void()> pick; // none: said, not picked
    };
    void openMenu(const QString& id, const QRectF& under, QVector<MenuItem> items);
    void closeMenu();
    void drawMenu(QPainter& p);
    void startEntry(const QString& id, const QString& text, const QString& placeholder, int maxLength,
                    std::function<void(const QString&)> done);
    void placeEntry(const QString& id, const QRectF& r);
    // Keeps what was typed (Enter, or a tap elsewhere); endEntry lets it go.
    void commitEntry();
    void endEntry();
    // Asks for a change to the chains or scenes; what it warns of is said.
    void arrange(const QJsonObject& request, const QString& failed, std::function<void(const QJsonObject&)> done);
    // The settings named, as they are now: what an undo sets back.
    QJsonObject chainNow(const ChainInfo& c, const QJsonObject& settings) const;
    QJsonObject sceneNow(int scene, const QJsonObject& settings) const;
    QVector<MenuItem> pedalItems(int now, const std::function<void(int)>& pick, const QString& self, bool scene) const;
    void panel(QPainter& p, const QRectF& r, const ChainInfo& c);
    void requestPlugin();
    void setParam(const QJsonObject& param, double value, bool done);
    void setBypass(int chain, int slot, bool bypassed);
    // Thru or mute: what it passes on while bypassed.
    void setBypassMode(int chain, int slot, bool mute);
    // The routing board.
    void pin(const ChainInfo& c);
    // The input or an output: dragged to move it, tapped to select it, and
    // renamed (for the whole performance).
    void dragIo(const PortInfo& port, QPointF pos, bool done);
    void renameIo(const PortInfo& port, const QString& name);
    void renamePort(bool input, int index, const QString& name);
    // The input's panel is the Gain Stage: tabs for this chain, every input
    // and every output, each a card that sets its level for the whole
    // performance. An output's panel is its card.
    void ioPanel(QPainter& p, const QRectF& r, const ChainInfo& c);
    // A card: the name (a tap renames it), the level (a tap types one), a
    // meter, a fader, mute and limiter for an output, and Auto. Returns its
    // height.
    double levelCard(QPainter& p, const QRectF& r, bool input, int index, bool own);
    void dragLevel(bool input, int index, const QRectF& track, const QString& id, QPointF pos, bool done);
    void sendLevel(bool input, int index, double db, bool done, double fromDb);
    void meterBar(QPainter& p, const QRectF& r, const QString& key, bool output);
    // A plug-in's (or the input's) outputs. Its panel lists them, each with
    // a meter, its level and where it goes (a tap opens it in the Gain
    // Stage); the Gain Stage's Chain tab shows each as a card, as the
    // TwoStorms plug-ins have it: + (another output; held or right-clicked,
    // several), width, pan and its … menu, then its level, meter, fader,
    // mute and Auto, under a row of Auto's settings for them all. Each
    // returns its height.
    double outputRows(QPainter& p, const QRectF& r, const ChainInfo& c, const PortInfo& from);
    double outputCard(QPainter& p, const QRectF& r, const ChainInfo& c, const PortInfo& from);
    double wireAutoRow(QPainter& p, const QRectF& r);
    // An output's gains as shown (while a fader moves, where it is), and
    // asking for them, undoably once done.
    OutputInfo outputNow(const ChainInfo& c, const PortInfo& from) const;
    void setOutputGains(const PortInfo& from, const OutputInfo& gains, bool done);
    // A fader for an output's level (0), width (1) or pan (2) on track: a
    // drag moves it, a double tap sets it back.
    void gainFader(const QString& id, const QRectF& grab, const QRectF& track, int which, const PortInfo& from);
    // A small one, its value written in it: a card's width and pan.
    void gainBar(QPainter& p, const QString& id, const QRectF& r, int which, const PortInfo& from, const OutputInfo& g);
    // More outputs (up to count in all), or one taken out with its wires;
    // the menu that adds several.
    void addOutputs(const PortInfo& from, int count);
    void removeOutput(const PortInfo& from);
    void addOutputsMenu(const PortInfo& from, const QRectF& under);
    void outputMenu(const PortInfo& from, const QRectF& under);
    // The level meters' key for an output, which meters its wires.
    QString meterKey(int chain, const PortInfo& from) const;
    // A Gain Stage tab's cards, scrolled; returns their height.
    double ioBody(QPainter& p, const QRectF& body, const ChainInfo& c, int own);
    void followMeters();
    void tapBlock(int slot);
    void dragBlock(int slot, QPointF pos, bool done);
    void dragWire(const PortInfo& port, bool source, QPointF pos, bool done);
    bool portAt(QPointF pos, bool input, PortInfo& out) const;
    void connectPorts(PortInfo from, PortInfo to);
    void removeWire(int wire);
    void removePlugin(int slot);
    // Ejects a plug-in from its wires: they stay, loose, where its dots
    // were, and it moves just above. A plug-in's dot dragged onto a loose
    // end, or a loose end onto a dot, plugs it in again. Remove leaves the
    // wires loose the same way.
    void eject(int slot);
    void unplugged(const ChainInfo& c, int slot, QVector<WireInfo>& wires, QVector<LooseInfo>& loose) const;
    void dragLoose(int loose, QPointF pos, bool done);
    void removeLoose(int loose);
    void loosePanel(QPainter& p, const QRectF& r, const ChainInfo& c);
    // A chain's wires and loose wires set at once, undoably: what pulling
    // out and plugging in do. place, if given, moves a plug-in with them.
    struct Place {
        int slot = -1;
        QPointF from, to;
    };
    void rewire(const ChainInfo& c, const QVector<WireInfo>& wires, const QVector<LooseInfo>& loose, const QString& what, Place place);
    void rewire(const ChainInfo& c, const QVector<WireInfo>& wires, const QVector<LooseInfo>& loose, const QString& what) {
        rewire(c, wires, loose, what, Place());
    }
    QJsonObject wiresRequest(const ChainInfo& c, const QVector<WireInfo>& wires, const QVector<LooseInfo>& loose) const;
    // While a plug-in is dragged: the loose ends its dots would plug into
    // where it is, and the wire it would go onto when it has none.
    QVector<int> pluggedAt(const ChainInfo& c, int slot, QPointF at) const;
    int wireUnder(const ChainInfo& c, int slot, QPointF at) const;
    void addPlugin(const PluginInfo& what);
    // A wire drawn as a jump cut or not, or its letters moved.
    void setJump(const WireInfo& wire, bool done);
    WireInfo wireNow(const ChainInfo& c, int wire) const;
    QString portName(const ChainInfo& c, const PortInfo& p) const;
    void wirePanel(QPainter& p, const QRectF& r, const ChainInfo& c);
    void pickerPanel(QPainter& p, const QRectF& r, const ChainInfo& c);
    bool bypassed(int chain, int slot) const;
    // Whether this tap, on what is called id, makes a double tap of the last.
    bool doubleTap(const QString& id);
    void paramRow(QPainter& p, const QRectF& row, const QRectF& list, const QJsonObject& q, const QString& format, bool all);
    // A parameter's value box: a tap types a value (once it is clear it was
    // not the first of two), a double tap sets the default.
    void tapValue(const QJsonObject& param);
    void typeValue(int chain, int slot, const QJsonObject& param, const QString& typed);
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
    int jumpDrag_ = -1;
    QSet<int> group_;                 // plug-ins selected together, by slot
    QHash<int, QPointF> groupFrom_;   // where each was when a drag of the group began
    bool selectMode_ = false;         // the Select button: taps and drags select
    bool banding_ = false;            // a drag on open space draws a box to select with
    QPointF bandTo_; // a jump cut's letter being dragged: wire * 2, + 1 for where it leaves
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
    // A loose wire selected, by its place in the chain's list; one whose
    // end is being dragged, and where to; where each can be tapped.
    int looseSel_ = -1, looseDrag_ = -1;
    bool looseMoved_ = false;
    QPointF looseStart_, looseFrom_, looseAt_;
    QVector<QPainterPath> looseShapes_;
    // The input or an output selected, and one being dragged.
    std::optional<PortInfo> ioSel_, ioDrag_;
    // The Gain Stage's tab: this chain, the inputs, the outputs.
    enum class IoTab { Chain, Inputs, Outputs } ioTab_ = IoTab::Inputs;
    // A gain or level fader being moved: whose, from what, and to what now.
    struct LevelDrag {
        bool input = true;
        int index = -1;
        double fromDb = 0, nowDb = 0;
        bool reset = false;
    };
    std::optional<LevelDrag> levelDrag_;
    qint64 lastLevelSend_ = 0;
    // The meters: they fall back slowly and hold their peak, as meters do.
    struct IoMeter {
        double shown = -120, peak = -120, limit = 0;
        qint64 peakAt = 0, clipAt = -100000;
    };
    QHash<QString, IoMeter> ioMeters_;
    qint64 lastMeters_ = 0;
    // The wire a dragged plug-in with none would go onto, or -1.
    int insertWire_ = -1;
    // A jump cut's letter while it is dragged, and as it was before.
    std::optional<WireInfo> jumpNow_;
    WireInfo jumpFrom_;
    // An output's gains while a slider moves, and as they were before.
    int gainDrag_ = -1; // 0 level, 1 width, 2 pan
    bool gainReset_ = false;
    struct OutputNow {
        int chain = -1;
        PortInfo from;
        OutputInfo gains;
    };
    std::optional<OutputNow> outNow_;
    OutputInfo outFrom_;
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
    QTimer valueTap_;
    QJsonObject valueParam_; // the parameter whose box was tapped

    // The chains and scenes.
    Strip strip_ = Strip::Chain;
    struct Menu {
        QString id; // the field it hangs from
        QRectF under;
        QVector<MenuItem> items;
    };
    std::optional<Menu> menu_;
    double menuScroll_ = 0, menuMax_ = 0, menuScrollFrom_ = 0;
    QPointF menuPress_;
    bool menuPressing_ = false, menuMoved_ = false;
    QLineEdit* entry_ = nullptr;
    QString entryFor_;
    bool entryPlaced_ = false, entryCancelled_ = false;
    std::function<void(const QString&)> entryDone_;
};

} // namespace cue::gui
