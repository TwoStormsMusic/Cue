#pragma once

// What the app knows about the engine, read from its messages
// (docs/control.md): the performance as loaded, the latest status and the
// tuner's reading. Plain data, so the views and the tests can share it.

#include <QJsonObject>
#include <QJsonValue>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

namespace cue::gui {

// One plug-in of a chain, from performance.json: a block. slot is its
// place in the chain's list of blocks, from 0, as the engine numbers them.
// One output of a plug-in or of a chain's input: channels 1-2, 3-4... Each
// has its level (-96 dB and below is silent), width in % (0 = mono), a
// balance, -100 (left) to 100, and mute; every wire leaving it carries it so.
struct OutputInfo {
    double levelDb = 0, width = 100, pan = 0;
    bool muted = false;
    bool neutral() const { return levelDb == 0 && width == 100 && pan == 0 && !muted; }
    bool operator==(const OutputInfo& o) const { return levelDb == o.levelDb && width == o.width && pan == o.pan && muted == o.muted; }
    bool operator!=(const OutputInfo& o) const { return !(*this == o); }
    static OutputInfo fromJson(const QJsonObject& j);
    // level_db, width, pan and muted, as the output request takes them.
    QJsonObject json() const;
    // What it is called: its channels, "1-2", "3-4"...
    static QString name(int out) { return QString("%1-%2").arg(2 * out + 1).arg(2 * out + 2); }
};
constexpr int kMaxOuts = 8; // as the engine has: channels 1-2 to 15-16

struct PluginInfo {
    int slot = 0;
    QString label, format, path, id;
    QStringList args;      // a built-in's name and numbers
    bool bypassed = false; // as the file starts it
    bool muteOnBypass = false;
    bool hasKey = false;   // it has a side-chain input to wire into
    bool placed = false;   // the routing board draws it where it was put...
    QPointF at;            // ...its centre, in board units
    QVector<OutputInfo> outs{OutputInfo{}};
};

// An end of a wire: the chain's input or a plug-in (where a wire starts,
// one of its outputs; where it ends, its input or its key) or one of the
// performance's outputs.
struct PortInfo {
    enum class Kind { Input, Block, Output };
    Kind kind = Kind::Input;
    int index = 0; // the plug-in's slot, or the output's number
    bool key = false;
    int out = 0; // which output of the input or plug-in a wire leaves, from 0
    static PortInfo input(int out = 0) { return {Kind::Input, 0, false, out}; }
    static PortInfo block(int slot, bool key = false) { return {Kind::Block, slot, key, 0}; }
    static PortInfo blockOut(int slot, int out) { return {Kind::Block, slot, false, out}; }
    static PortInfo output(int o) { return {Kind::Output, o, false, 0}; }
    bool operator==(const PortInfo& o) const { return kind == o.kind && index == o.index && key == o.key && out == o.out; }
    bool operator!=(const PortInfo& o) const { return !(*this == o); }
    // The same input or plug-in, whichever of its outputs.
    bool same(const PortInfo& o) const { return kind == o.kind && (kind == Kind::Input || index == o.index); }
    PortInfo withOut(int o) const {
        PortInfo p = *this;
        p.out = o;
        return p;
    }
    // As a request names it: a slot, "in", or the output by number (out1,
    // out2...), which stays when outputs are renamed, so undo still finds it.
    QJsonValue json(const QStringList& outputs) const;
};

// A wire: it carries one output of the input or a plug-in (from.out) as
// that output has it, to a plug-in or an output. It has no gains of its own.
struct WireInfo {
    PortInfo from, to;
    bool feedback = false; // heard a block late, so it may lead back to its own plug-in
    bool jump = false;     // drawn as a jump cut: out at its source and in at its end, marked by a letter
    QPointF jumpOut{34, 0}, jumpIn{-34, 0}; // ...where the letters are, from the ports, in board units
    // jump, jump_out and jump_in, as the engine sends them.
    void readJump(const QJsonObject& wj);
    bool same(const WireInfo& o) const { return jump == o.jump && jumpOut == o.jumpOut && jumpIn == o.jumpIn; }
    // The request that makes it so: {"cmd": "wire", "chain", "from", "out", "to", ...}.
    QJsonObject request(int chain, const QStringList& outputs, const QString& cmd = "wire") const;
};

// The request that sets one output: {"cmd": "output", "chain", "from", "out", gains}.
QJsonObject outputRequest(int chain, const PortInfo& from, const OutputInfo& gains);

// A wire attached at one end only, left where a plug-in was pulled out:
// it carries nothing until its loose end is plugged into something. from:
// it leaves port, so its loose end wants an input; else it arrives at port.
struct LooseInfo {
    PortInfo port;
    bool from = true;
    QPointF at; // the loose end, in board units
    bool feedback = false;
    // As the wires request lists it.
    QJsonObject json(const QStringList& outputs) const;
    // The wire it makes, plugged into other; and the loose one a wire leaves
    // when its end at side (from or to) is pulled out, lying at at.
    WireInfo plugged(const PortInfo& other) const;
    static LooseInfo of(const WireInfo& w, bool keepFrom, QPointF at);
};

struct ChainInfo {
    // The engine's number for it, which stays while the engine plays,
    // whatever is added, moved or taken out around it.
    int index = 0;
    int scene = 0;
    QString name, display, warm, input;
    int pedal = 0, program = -1;
    QStringList onSelect; // actions, as a config writes them
    QVector<PluginInfo> blocks;
    QVector<WireInfo> wires;
    QVector<LooseInfo> loose;
    QVector<OutputInfo> inputOuts{OutputInfo{}}; // the input's outputs
    // The outputs of the input or a plug-in (none for anything else), and
    // the one a wire from there leaves (as it starts if there is none).
    const QVector<OutputInfo>* outsOf(const PortInfo& from) const;
    QVector<OutputInfo>* outsOf(const PortInfo& from);
    int numOuts(const PortInfo& from) const;
    OutputInfo gainsOf(const PortInfo& from) const;
    // The wires leaving that output, by their place in wires.
    QVector<int> wiresFrom(const PortInfo& from) const;
    // Where the board draws its input and each output (by number), once
    // they are put somewhere.
    std::optional<QPointF> inputAt;
    QVector<std::optional<QPointF>> outputsAt;
    int numPlugins() const { return blocks.size(); }
    const PluginInfo* plugin(int slot) const;
    // The wire between two ports, by its place in wires, or -1.
    int wire(const PortInfo& from, const PortInfo& to) const;
    // The outputs it has a wire to, by number.
    QVector<int> outputsUsed() const;
    // Whether a wire from -> to would bring a plug-in's signal back to it:
    // into itself (its input or its key), or into one its signal goes
    // through on the way there. Only a feedback wire can.
    bool wouldLoop(const PortInfo& from, const PortInfo& to) const;
};

// What a chain would do if it played (whether it does or not). A wire is
// green when sound from the input would pass through it on its way to an
// output: not after a plug-in bypassed to silence, not on a wire muted, not
// where nothing leads on to an output. A plug-in is played when the input
// reaches it and an output hears it (through it or its key); the engine
// processes only those.
struct Flow {
    QVector<bool> wires;  // by wire
    QVector<bool> played; // by slot
    QVector<bool> heard;  // by output: some sound would reach it
};
Flow flowOf(const ChainInfo& c, const std::function<bool(int slot)>& bypassed, bool inputsMuted, int numOutputs);

struct SceneInfo {
    QString name, display;
    int pedal = 0, program = -1;
    QVector<int> chains; // engine numbers, in order
};

struct TriggerInfo {
    QString name, scene; // scene: only there; empty: everywhere
};

struct ControlInfo {
    QString name, type; // type: toggle, discrete or continuous
    QStringList values;
    double min = 0, max = 1;
    int pedal = 0;
    bool mutes = false; // has a mute line: muting the outputs is what it is for
};

// Auto, as the Gain Stage does it: what it measures, the level it aims for
// and how long it listens.
struct AutoInfo {
    QString measure = "peak"; // peak, rms, lufs
    double targetDb = -12, seconds = 5;
    static AutoInfo fromJson(const QJsonObject& j);
    QJsonObject json() const;
};

// The performance's levels, the same whichever scene plays: an input's gain
// before every chain, an output's level, mute and limiter after them all.
// -100 dB is silence.
struct InputLevel {
    double gainDb = 0;
    AutoInfo autoSet;
};
struct OutputLevel {
    double levelDb = 0;
    bool muted = false, limiter = false;
    double ceilingDb = -1, releaseMs = 100;
    AutoInfo autoSet;
};
struct IoLevels {
    QVector<InputLevel> inputs;
    QVector<OutputLevel> outputs;
    // Auto on a block's outputs, in every chain: matching what the block is
    // fed, or to the target.
    AutoInfo wireAuto;
    bool wireMatch = true;
    static IoLevels fromJson(const QJsonObject& io);
};

struct Performance {
    bool valid = false;
    QString name;
    QStringList inputs, outputs;
    IoLevels levels; // one for each input and output
    QVector<SceneInfo> scenes;
    QVector<ChainInfo> chains; // scene by scene, in order
    QVector<ControlInfo> controls;
    QVector<TriggerInfo> triggers;
    QString tunerInput;

    static Performance fromJson(const QJsonObject& performance);
    // An output by its name or as out1, out2...; -1 if there is none.
    int output(const QString& name) const;
    const ControlInfo* control(const QString& name) const;
    // A chain by its number, or nullptr.
    const ChainInfo* chain(int index) const;
    ChainInfo* chain(int index);
};

struct ChainStatus {
    QString name, state; // idle, pre-roll, warm, active, tail, stopping
    double avgUs = 0, maxUs = 0;
    QVector<int> bypassed; // slots
    QVector<double> wirePeaks; // a playing chain's wires, in order: each one's peak after its gains
};

struct ControlStatus {
    QString name, text;
    double value = 0;
};

struct Level {
    QString name;
    double peakDb = -120;  // an input after its gain, an output as it goes out
    double limitDb = 0;    // how far an output's limiter turned it down
};

struct Status {
    bool valid = false;
    double dspAvg = 0, dspMax = 0;  // % of a block's time
    std::optional<double> marginMs; // none until measured
    qint64 xruns = 0;
    double periodMs = 0, bufferMs = 0;
    int scene = 0;
    int selected = -1;
    QString selectedName;
    bool muted = false;
    QString display;
    QVector<bool> leds;
    QVector<ControlStatus> controls;
    QVector<ChainStatus> chains;
    QVector<Level> inputs, outputs;

    static Status fromJson(const QJsonObject& status);
    // The chain kept warm, or -1.
    int warmChain() const;
    const ControlStatus* control(const QString& name) const;
    bool bypassed(int chain, int slot) const;
    // The margin as % of what the buffer can hold beyond the block being
    // played: 0% is an xrun.
    std::optional<double> marginPercent() const;
};

struct TunerReading {
    bool valid = false;
    bool signal = false, held = true, inTune = false;
    QString note; // empty before a note is heard
    int octave = 4, midi = 69;
    double cents = 0, hz = 0, levelDb = -200;
    double reference = 440, tolerance = 2;
    QString range, input, names;

    static TunerReading fromJson(const QJsonObject& tuner);
    // "string 4" when the note is an open string of a bass or guitar in
    // standard tuning (the range says which), else empty.
    QString string() const;
    // Where the strobe is (in stripes, 0 to 1) seconds after phase, as the
    // TwoStorms Tuner turns it: a quarter stripe a second per cent off, at
    // most 12 a second; still for a held note.
    double turn(double phase, double seconds) const;
};

// A plug-in the engine's scan found on its machine, or a file it skipped
// (then name is the file's, and error says why).
struct ScannedPlugin {
    QString format, path, id, name, vendor, version;
    QStringList categories;
    bool bridged = false; // a Windows plug-in, through yabridge
    bool opened = false;  // the scan made it, so the next three are known
    bool sidechain = false, editor = false, midi = false;
    QString error; // why it is skipped; empty when it is ready
    bool ready() const { return error.isEmpty(); }
    // The same plug-in wherever it is listed: for favourites.
    QString key() const { return format + "|" + path + "|" + id; }
    // As a block would have it, to be added.
    PluginInfo toPlugin() const;
};

// What the engine's scan found, where it looked, and how far it is.
struct Catalog {
    bool valid = false, scanning = false;
    int done = 0, total = 0;
    QString current;     // the file being opened
    qint64 scannedAt = 0; // seconds since 1970; 0: never
    QStringList usual;   // the folders always looked in
    QStringList extra;   // the user's
    QVector<ScannedPlugin> plugins; // by name, skipped ones too

    static Catalog fromJson(const QJsonObject& plugins);
    // A "scan" event: how far it is.
    void progress(const QJsonObject& scan);
    int ready() const;
    int skipped() const { return plugins.size() - ready(); }
};

// Whether a block of the performance is this plug-in. The performance
// writes paths under the home folder with ~, the scan does not.
bool isPlugin(const PluginInfo& block, const ScannedPlugin& p);
// The chains that have it, by name.
QStringList chainsWith(const Performance& perf, const ScannedPlugin& p);

// "M2 · 48k · 64×3" from the hello's audio_settings.
QString audioSummary(const QJsonObject& hello);
// The controller, as the MIDI pill says it: "FCB1010", the port, or empty.
QString controllerName(const QJsonObject& hello);

} // namespace cue::gui
