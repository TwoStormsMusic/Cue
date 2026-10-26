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
    bool neutral() const { return levelDb == 0 && width == 100 && pan == 0 && !muted; }
    // The request that makes it so: {"cmd": "wire", "chain", "from", "to", ...}.
    QJsonObject request(int chain, const QStringList& outputs, const QString& cmd = "wire") const;
};

struct ChainInfo {
    int index = 0; // the engine's number: chains are numbered scene by scene
    int scene = 0;
    QString name, display, warm, input;
    int pedal = 0;
    QVector<PluginInfo> blocks;
    QVector<WireInfo> wires;
    int numPlugins() const { return blocks.size(); }
    const PluginInfo* plugin(int slot) const;
    // The wire between two ports, by its place in wires, or -1.
    int wire(const PortInfo& from, const PortInfo& to) const;
    // The outputs it has a wire to, by number.
    QVector<int> outputsUsed() const;
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
    QString name;
    QVector<int> chains; // engine numbers
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
    QVector<ChainInfo> chains;
    QVector<ControlInfo> controls;
    QString tunerInput;

    static Performance fromJson(const QJsonObject& performance);
    const ControlInfo* control(const QString& name) const;
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

// "M2 · 48k · 64×3" from the hello's audio_settings.
QString audioSummary(const QJsonObject& hello);
// The controller, as the MIDI pill says it: "FCB1010", the port, or empty.
QString controllerName(const QJsonObject& hello);

} // namespace cue::gui
