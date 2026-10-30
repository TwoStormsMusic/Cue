#pragma once

// What every view shares: the line to the engine, what it last said, the
// app's settings, and the things a tap can ask of the engine.

#include "gui/link.h"
#include "gui/model.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

namespace cue::gui {

// The app's own settings, in ~/.config/cue/app.ini: the machine's, not the
// performance's.
struct Settings {
    QString startView = "stage"; // stage, tuner or chains
    bool percent = false;        // times as % of the budget rather than ms
    double scale = 1.0;          // size of everything, 0.5 to 2.5
    bool tunerWhenMuted = true;  // the tuner view comes up while the inputs are muted
    bool fullScreen = false;
    bool wireTags = true;        // a wire shows its level, width and pan on the board when they are changed
    QStringList favourites;      // plug-ins, by ScannedPlugin::key()
    // What the stage view's header shows: cpu, margin, xruns, audio, controller.
    QStringList stageShows = {"cpu", "margin", "xruns", "audio", "controller"};
    bool chainColumn = true;     // the chain view shows its column of scenes and chains
    bool feedbackWires = false;  // a wire may bring a plug-in's sound back to it, a block late
    bool takeCard = true;        // an engine the app starts takes the sound card from what else has it

    static QString path();
    void load();
    void save() const;
};

class AppState : public QObject {
    Q_OBJECT
public:
    explicit AppState(EngineLink& link, QObject* parent = nullptr);

    EngineLink& link;
    Settings settings;
    // When the app started the engine (--start): where its messages go, and
    // how long that file was before.
    QString engineLog;
    qint64 engineLogFrom = 0;

    bool connected() const { return link.connected(); }
    QJsonObject hello;
    Performance perf;
    Status status;
    TunerReading tuner;
    double strobePhase = 0; // the tuner's strobe, turned by each reading
    struct LogLine {
        QString level, text;
        QDateTime at;
    };
    QVector<LogLine> log; // the newest last, at most 200

    // The chain playing, or -1 before the engine has said.
    int playing() const { return status.valid ? status.selected : -1; }
    const ChainInfo* chain(int index) const;
    // The chains of the scene playing, in order.
    QVector<int> sceneChains() const;

    // Asks the engine. Failures come back as a message.
    void select(int chain);
    void step(int delta); // the next or previous chain, as the FCB's up and down
    void run(const QString& action);
    void save();
    void reloadPerformance();
    // A reply's "ok", or a message saying why not.
    bool check(const QJsonObject& reply, const QString& what);
    // The plug-ins on the engine's machine. Asks for them, and for a scan:
    // "changed" (new and changed files), "all", or one file by its path.
    Catalog catalog;
    void loadPlugins(const QString& scan = {});
    void setPluginFolders(const QStringList& extra);
    bool isFavourite(const ScannedPlugin& p) const { return settings.favourites.contains(p.key()); }
    void setFavourite(const ScannedPlugin& p, bool on);

    // Auto running on an input ("in 0") or an output ("out 1"): waiting for
    // signal or listening, and for how long of how long.
    struct AutoNow {
        bool listening = false;
        double seconds = 0, of = 0;
    };
    QHash<QString, AutoNow> autoNow;
    static QString autoKey(bool input, int index) { return QString(input ? "in %1" : "out %1").arg(index); }
    // An input's gain or an output's level, mute, limiter or Auto settings
    // (the fields of a level request); Auto started or stopped.
    void setLevel(bool input, int index, const QJsonObject& fields, std::function<void(const QJsonObject&)> then = {});
    void runAuto(bool input, int index, bool cancel);

    // Asks the engine for a change to the routing (add, remove, restore,
    // wire, unwire, place). The performance follows its answer, which then
    // goes to then.
    void change(const QJsonObject& request, std::function<void(const QJsonObject&)> then = {});

    // What was changed here, so it can be taken back (Ctrl+Z) and made again
    // (Ctrl+Y or Ctrl+Shift+Z): a parameter, a bypass, the parameters picked
    // to show, the routing.
    // Each is the request that undoes it and the one that does it again.
    // What a pedal or a plug-in's own editor changes is not in it. A new
    // engine or performance starts it afresh.
    struct Edit {
        QString what; // said when it is undone: "Delay: Output"
        int chain = -1, slot = -1; // where it is shown once undone (-1: no plug-in)
        QJsonObject undo, redo; // each a request, or {"cmd": "batch", "requests": [...]}
        int redoSlot = -2; // ...and once redone, if not slot
    };
    void edited(const Edit& e);
    bool canUndo() const { return !undo_.isEmpty(); }
    bool canRedo() const { return !redo_.isEmpty(); }
    void undo();
    void redo();

signals:
    // The connection, the hello or the performance changed.
    void changed();
    void statusChanged();
    void tunerChanged();
    // For a moment at the bottom of the screen.
    void message(const QString& text, bool error);
    // An edit was undone or redone: the plug-in it was on.
    void undone(int chain, int slot);
    void undoChanged();
    // The scan went on, or the plug-ins changed.
    void catalogChanged();

private:
    void onConnection(bool up);
    void onEvent(const QString& name, const QJsonObject& m);
    // What a change's answer (or an event) says the performance is now.
    void follow(const QJsonObject& request, const QJsonObject& reply);
    void setWire(int chain, const QJsonObject& wire);
    void setLevels(const QJsonObject& io);

    void takeBack(QVector<Edit>& from, QVector<Edit>& to, bool undoing);

    QElapsedTimer tunerClock_; // since the last reading
    QVector<Edit> undo_, redo_; // the newest last
};

} // namespace cue::gui
