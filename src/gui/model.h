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
struct PluginInfo {
    int slot = 0;
    QString label, format, path, id;
    QStringList args;      // a built-in's name and numbers
    bool bypassed = false; // as the file starts it
    bool muteOnBypass = false;
    bool hasKey = false;   // it has a side-chain input to wire into
    bool placed = false;   // the routing board draws it where it was put...
    QPointF at;            // ...its centre, in board units
};

// An end of a wire: the chain's input, a plug-in (its input, or its key)
// or one of the performance's outputs.
struct PortInfo {
    enum class Kind { Input, Block, Output };
    Kind kind = Kind::Input;
    int index = 0; // the plug-in's slot, or the output's number
    bool key = false;
    static PortInfo input() { return {}; }
    static PortInfo block(int slot, bool key = false) { return {Kind::Block, slot, key}; }
    static PortInfo output(int o) { return {Kind::Output, o, false}; }
    bool operator==(const PortInfo& o) const { return kind == o.kind && index == o.index && key == o.key; }
    bool operator!=(const PortInfo& o) const { return !(*this == o); }
    // As a request names it: a slot, "in", or the output's name.
    QJsonValue json(const QStringList& outputs) const;
};

// A wire and what it does to the sound: its level (-96 dB and below is
// silent), width in % (0 = mono), then a balance, -100 (left) to 100.
struct WireInfo {
    PortInfo from, to;
    double levelDb = 0, width = 100, pan = 0;
    bool muted = false;
    bool feedback = false; // heard a block late, so it may lead back to its own plug-in
    bool jump = false;     // drawn as a jump: out at its source and in at its end, marked by a letter
    bool neutral() const { return levelDb == 0 && width == 100 && pan == 0 && !muted; }
    bool same(const WireInfo& o) const { return levelDb == o.levelDb && width == o.width && pan == o.pan && muted == o.muted && jump == o.jump; }
    // The request that makes it so: {"cmd": "wire", "chain", "from", "to", ...}.
    QJsonObject request(int chain, const QStringList& outputs, const QString& cmd = "wire") const;
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

struct Performance {
    bool valid = false;
    QString name;
    QStringList inputs, outputs;
    QVector<SceneInfo> scenes;
    QVector<ChainInfo> chains; // scene by scene, in order
    QVector<ControlInfo> controls;
    QVector<TriggerInfo> triggers;
    QString tunerInput;

    static Performance fromJson(const QJsonObject& performance);
    const ControlInfo* control(const QString& name) const;
    // A chain by its number, or nullptr.
    const ChainInfo* chain(int index) const;
    ChainInfo* chain(int index);
};

struct ChainStatus {
    QString name, state; // idle, pre-roll, warm, active, tail, stopping
    double avgUs = 0, maxUs = 0;
    QVector<int> bypassed; // slots
};

struct ControlStatus {
    QString name, text;
    double value = 0;
};

struct Level {
    QString name;
    double peakDb = -120;
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
