#pragma once

// What the app knows about the engine, read from its messages
// (docs/control.md): the performance as loaded, the latest status and the
// tuner's reading. Plain data, so the views and the tests can share it.

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace cue::gui {

// One plug-in of a chain, from performance.json. slot is its place in the
// chain counted line by line, as the engine numbers them.
struct PluginInfo {
    int slot = 0;
    QString label, format, path, id;
    bool bypassed = false; // as the file starts it
    bool muteOnBypass = false;
};

struct LineInfo {
    QString name;        // empty for the main line
    QString from;        // the line it leaves ("" = the main line)
    int after = 0;       // after this many of that line's plug-ins
    int parent = -1;     // index of that line
    QVector<PluginInfo> plugins;
};

// Where a chain feeds one output: a line, after so many of its plug-ins.
struct TapInfo {
    QString output;
    int line = 0;
    int after = 0;
};

struct ChainInfo {
    int index = 0; // the engine's number: chains are numbered scene by scene
    int scene = 0;
    QString name, display, warm, input;
    int pedal = 0;
    QVector<LineInfo> lines;
    QVector<TapInfo> taps; // one per output
    int numPlugins() const;
    const PluginInfo* plugin(int slot) const;
};

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
};

// "M2 · 48k · 64×3" from the hello's audio_settings.
QString audioSummary(const QJsonObject& hello);
// The controller, as the MIDI pill says it: "FCB1010", the port, or empty.
QString controllerName(const QJsonObject& hello);

} // namespace cue::gui
