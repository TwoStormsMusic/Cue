#include "gui/state.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace cue::gui {

QString Settings::path() { return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/cue/app.ini"; }

void Settings::load() {
    QSettings s(path(), QSettings::IniFormat);
    const QString view = s.value("start_view", startView).toString();
    if (view == "stage" || view == "tuner" || view == "chains")
        startView = view;
    percent = s.value("times", percent ? "percent" : "ms").toString() == "percent";
    scale = std::clamp(s.value("scale", scale).toDouble(), 0.5, 2.5);
    tunerWhenMuted = s.value("tuner_when_muted", tunerWhenMuted).toBool();
    fullScreen = s.value("full_screen", fullScreen).toBool();
    wireTags = s.value("wire_tags", wireTags).toBool();
    favourites = s.value("favourites", favourites).toStringList();
    // An empty list is saved as "none": QSettings would read it back as missing.
    const QStringList shows = s.value("stage_shows", stageShows).toStringList();
    stageShows = shows == QStringList{"none"} ? QStringList() : shows;
    chainColumn = s.value("chain_column", chainColumn).toBool();
    feedbackWires = s.value("feedback_wires", feedbackWires).toBool();
    takeCard = s.value("take_sound_card", takeCard).toBool();
}

void Settings::save() const {
    QSettings s(path(), QSettings::IniFormat);
    s.setValue("start_view", startView);
    s.setValue("times", percent ? "percent" : "ms");
    s.setValue("scale", scale);
    s.setValue("tuner_when_muted", tunerWhenMuted);
    s.setValue("full_screen", fullScreen);
    s.setValue("wire_tags", wireTags);
    s.setValue("favourites", favourites);
    s.setValue("stage_shows", stageShows.isEmpty() ? QStringList{"none"} : stageShows);
    s.setValue("chain_column", chainColumn);
    s.setValue("feedback_wires", feedbackWires);
    s.setValue("take_sound_card", takeCard);
}

AppState::AppState(EngineLink& l, QObject* parent) : QObject(parent), link(l) {
    connect(&link, &EngineLink::connectionChanged, this, &AppState::onConnection);
    connect(&link, &EngineLink::event, this, &AppState::onEvent);
    link.setEvents({"status", "log", "tuner", "performance", "wire", "scan", "plugins", "edited", "io", "auto"});
}

void AppState::onConnection(bool up) {
    // What could be undone was for the engine that was here.
    undo_.clear();
    redo_.clear();
    autoNow.clear();
    emit undoChanged();
    if (up) {
        link.request({{"cmd", "hello"}}, [this](const QJsonObject& r) {
            if (r.value("ok").toBool()) {
                hello = r;
                // What the engine did to get the sound card: closed REAPER, say.
                const QJsonArray notes = r.value("audio_notes").toArray();
                if (!notes.isEmpty()) {
                    QString text = notes.first().toString();
                    emit message(text.left(1).toUpper() + text.mid(1), false);
                }
                emit changed();
            }
        });
        reloadPerformance();
        loadPlugins();
    } else {
        status.valid = false;
        tuner.valid = false;
    }
    emit changed();
}

void AppState::reloadPerformance() {
    link.request({{"cmd", "performance"}}, [this](const QJsonObject& r) {
        if (!r.value("ok").toBool())
            return;
        perf = Performance::fromJson(r.value("performance").toObject());
        emit changed();
    });
}

void AppState::onEvent(const QString& name, const QJsonObject& m) {
    if (name == "status") {
        status = Status::fromJson(m);
        emit statusChanged();
    } else if (name == "tuner") {
        double dt = 0;
        if (tunerClock_.isValid())
            dt = std::min(0.2, tunerClock_.restart() / 1000.0);
        else
            tunerClock_.start();
        tuner = TunerReading::fromJson(m);
        strobePhase = tuner.turn(strobePhase, dt);
        emit tunerChanged();
    } else if (name == "performance") {
        perf = Performance::fromJson(m);
        emit changed();
    } else if (name == "wire") {
        setWire(m.value("chain").toInt(-1), m.value("wire").toObject());
    } else if (name == "io") {
        setLevels(m);
    } else if (name == "auto" && m.contains("chain")) {
        // Auto on a block's output.
        const int chain = m.value("chain").toInt(-1);
        const QString key = autoKey(chain, m.value("from"), m.value("to"), m.value("key").toBool()), state = m.value("state").toString();
        if (state == "waiting" || state == "listening") {
            autoNow[key] = {state == "listening", m.value("seconds").toDouble(), m.value("of").toDouble()};
        } else {
            autoNow.remove(key);
            const ChainInfo* c = perf.chain(chain);
            auto end = [&](const QJsonValue& v, bool from) {
                if (v.isDouble()) {
                    const PluginInfo* pi = c ? c->plugin(v.toInt()) : nullptr;
                    return pi ? pi->label : QString("Block %1").arg(v.toInt() + 1);
                }
                const int o = perf.output(v.toString());
                return from ? (c ? c->input : QString("the input")) : perf.outputs.value(o, v.toString());
            };
            const QString what = end(m.value("from"), true) + " → " + end(m.value("to"), false);
            if (state == "done") {
                // Undone like a fader moved by hand.
                auto set = [&](double db) {
                    QJsonObject r{{"cmd", "wire"}, {"chain", chain}, {"from", m.value("from")}, {"to", m.value("to")}, {"level_db", db}};
                    if (m.value("key").toBool())
                        r["key"] = true;
                    return r;
                };
                edited({"Auto on " + what, chain, -1, set(m.value("from_db").toDouble()), set(m.value("to_db").toDouble())});
                const double to = m.value("to_db").toDouble();
                QString heard = QString("it heard %1 dB").arg(m.value("level_db").toDouble(), 0, 'f', 1);
                if (m.contains("fed_db"))
                    heard += QString(", fed %1 dB").arg(m.value("fed_db").toDouble(), 0, 'f', 1);
                emit message(QString("Auto set %1 to %2%3 dB (%4)").arg(what, to > 0 ? "+" : "").arg(to, 0, 'f', 1).arg(heard), false);
            } else if (state == "failed") {
                emit message("Auto on " + what + ": " + m.value("error").toString(), true);
            }
        }
        emit statusChanged();
    } else if (name == "auto") {
        const bool input = m.contains("input");
        const int index = m.value(input ? "input" : "output").toInt(-1);
        const QString key = autoKey(input, index), state = m.value("state").toString();
        const QStringList& names = input ? perf.inputs : perf.outputs;
        const QString what = names.value(index, input ? "the input" : "the output");
        if (state == "waiting" || state == "listening") {
            autoNow[key] = {state == "listening", m.value("seconds").toDouble(), m.value("of").toDouble()};
        } else {
            autoNow.remove(key);
            if (state == "done") {
                // Undone like a fader moved by hand.
                const char* field = input ? "gain_db" : "level_db";
                auto set = [&](double db) { return QJsonObject{{"cmd", "level"}, {input ? "input" : "output", index}, {field, db}}; };
                const QJsonObject undo = set(m.value("from_db").toDouble()), redo = set(m.value("to_db").toDouble());
                edited({"Auto on " + what, -1, -1, undo, redo});
                const double to = m.value("to_db").toDouble();
                emit message(QString("Auto set %1 to %2%3 dB (it heard %4 dB)")
                                 .arg(what, to > 0 ? "+" : "")
                                 .arg(to, 0, 'f', 1)
                                 .arg(m.value("level_db").toDouble(), 0, 'f', 1),
                             false);
            } else if (state == "failed") {
                emit message("Auto on " + what + ": " + m.value("error").toString(), true);
            }
        }
        emit statusChanged();
    } else if (name == "edited") {
        // A knob turned in a plug-in's own editor: undone like a slider here.
        const int chain = m.value("chain").toInt(-1), slot = m.value("slot").toInt(-1), param = m.value("param").toInt();
        auto set = [&](double v) { return QJsonObject{{"cmd", "param"}, {"chain", chain}, {"slot", slot}, {"param", param}, {"value", v}}; };
        const ChainInfo* c = perf.chain(chain);
        const PluginInfo* pi = c ? c->plugin(slot) : nullptr;
        edited({QString("%1: %2").arg(pi ? pi->label : "plug-in", m.value("name").toString()), chain, slot, set(m.value("from").toDouble()),
                set(m.value("to").toDouble())});
    } else if (name == "scan") {
        catalog.progress(m);
        emit catalogChanged();
    } else if (name == "plugins") {
        catalog = Catalog::fromJson(m);
        emit catalogChanged();
    } else if (name == "log") {
        log.append({m.value("level").toString(), m.value("text").toString(), QDateTime::currentDateTime()});
        if (log.size() > 200)
            log.remove(0, log.size() - 200);
        if (m.value("level").toString() != "info")
            emit message(m.value("text").toString(), true);
    }
}

void AppState::loadPlugins(const QString& scan) {
    QJsonObject req{{"cmd", "plugins"}};
    if (!scan.isEmpty())
        req["scan"] = scan;
    link.request(req, [this](const QJsonObject& r) {
        if (!r.value("ok").toBool()) {
            // An engine from before the scan, or one scanning already.
            if (!r.value("error").toString().contains("running"))
                check(r, "Plug-ins");
            return;
        }
        catalog = Catalog::fromJson(r.value("plugins").toObject());
        emit catalogChanged();
    });
}

void AppState::setPluginFolders(const QStringList& extra) {
    link.request({{"cmd", "plugins"}, {"folders", QJsonArray::fromStringList(extra)}, {"scan", "changed"}}, [this](const QJsonObject& r) {
        if (!check(r, "Folders not kept"))
            return;
        catalog = Catalog::fromJson(r.value("plugins").toObject());
        emit catalogChanged();
    });
}

void AppState::setFavourite(const ScannedPlugin& p, bool on) {
    settings.favourites.removeAll(p.key());
    if (on)
        settings.favourites << p.key();
    settings.save();
    emit catalogChanged();
}

const ChainInfo* AppState::chain(int index) const { return perf.chain(index); }

QVector<int> AppState::sceneChains() const {
    const int scene = status.valid ? status.scene : 0;
    if (scene >= 0 && scene < perf.scenes.size())
        return perf.scenes[scene].chains;
    return {};
}

bool AppState::check(const QJsonObject& reply, const QString& what) {
    if (reply.value("ok").toBool())
        return true;
    emit message(what + ": " + reply.value("error").toString("no answer"), true);
    return false;
}

void AppState::change(const QJsonObject& request, std::function<void(const QJsonObject&)> then) {
    link.request(request, [this, request, then](const QJsonObject& r) {
        follow(request, r);
        if (then)
            then(r);
    });
}

void AppState::follow(const QJsonObject& request, const QJsonObject& r) {
    if (!r.value("ok").toBool())
        return;
    if (r.contains("performance")) {
        perf = Performance::fromJson(r.value("performance").toObject());
        emit changed();
    } else if (r.contains("io")) {
        setLevels(r.value("io").toObject());
    } else if (r.contains("wire")) {
        setWire(request.value("chain").toInt(-1), r.value("wire").toObject());
    } else if (request.value("cmd").toString() == "place") {
        ChainInfo* c = perf.chain(request.value("chain").toInt(-1));
        const int slot = request.value("slot").toInt(-1);
        const QPointF at(request.value("x").toDouble(), request.value("y").toDouble());
        if (c && request.contains("port")) {
            // The input, or an output by name.
            const QString port = request.value("port").toString();
            const int o = perf.output(port);
            if (port == "in") {
                c->inputAt = at;
            } else if (o >= 0) {
                c->outputsAt.resize(std::max(static_cast<int>(c->outputsAt.size()), static_cast<int>(perf.outputs.size())));
                c->outputsAt[o] = at;
            }
            emit changed();
        } else if (c && slot >= 0 && slot < c->blocks.size()) {
            PluginInfo& b = c->blocks[slot];
            b.placed = true;
            b.at = QPointF(request.value("x").toDouble(), request.value("y").toDouble());
            emit changed();
        }
    }
}

void AppState::setLevels(const QJsonObject& io) {
    perf.levels = IoLevels::fromJson(io);
    perf.levels.inputs.resize(perf.inputs.size());
    perf.levels.outputs.resize(perf.outputs.size());
    emit changed();
}

void AppState::setLevel(bool input, int index, const QJsonObject& fields, std::function<void(const QJsonObject&)> then) {
    QJsonObject req = fields;
    req["cmd"] = "level";
    req[input ? "input" : "output"] = index;
    change(req, std::move(then));
}

void AppState::runAuto(bool input, int index, bool cancel) {
    QJsonObject req{{"cmd", "auto"}, {input ? "input" : "output", index}};
    if (cancel)
        req["cancel"] = true;
    link.request(req, [this, input, index, cancel](const QJsonObject& r) {
        follow(QJsonObject{{"cmd", "auto"}}, r);
        if (!check(r, "Auto"))
            return;
        if (cancel)
            autoNow.remove(autoKey(input, index));
        else
            autoNow[autoKey(input, index)] = {false, 0, r.value("auto").toObject().value("of").toDouble()};
        emit statusChanged();
    });
}

QString AppState::autoKey(int chain, const QJsonValue& from, const QJsonValue& to, bool key) {
    auto end = [](const QJsonValue& v) { return v.isDouble() ? QString::number(v.toInt()) : v.toString().toLower(); };
    return QString("wire %1 %2 %3%4").arg(chain).arg(end(from), end(to), key ? " key" : "");
}

QString AppState::autoKey(int chain, const WireInfo& w) const {
    return autoKey(chain, w.from.json(perf.outputs), w.to.json(perf.outputs), w.to.key);
}

void AppState::runAuto(int chain, const WireInfo& w, bool cancel) {
    QJsonObject req = w.request(chain, perf.outputs, "auto");
    if (cancel)
        req["cancel"] = true;
    const QString key = autoKey(chain, w);
    link.request(req, [this, key, cancel](const QJsonObject& r) {
        if (!check(r, "Auto"))
            return;
        if (cancel)
            autoNow.remove(key);
        else
            autoNow[key] = {false, 0, r.value("auto").toObject().value("of").toDouble()};
        emit statusChanged();
    });
}

void AppState::setWireAuto(const AutoInfo& to, bool match, std::function<void(const QJsonObject&)> then) {
    QJsonObject a = to.json();
    a["match"] = match;
    change({{"cmd", "level"}, {"wires", true}, {"auto", a}}, std::move(then));
}

// A wire's gains, as the engine has them now.
void AppState::setWire(int chain, const QJsonObject& wj) {
    ChainInfo* found = perf.chain(chain);
    if (!found)
        return;
    ChainInfo& c = *found;
    auto port = [&](const QJsonValue& v, bool from) {
        if (v.isDouble())
            return PortInfo::block(v.toInt(), !from && wj.value("key").toBool());
        return from ? PortInfo::input() : PortInfo::output(perf.output(v.toString()));
    };
    const int i = c.wire(port(wj.value("from"), true), port(wj.value("to"), false));
    if (i < 0)
        return;
    WireInfo& w = c.wires[i];
    w.levelDb = wj.value("level_db").toDouble(0);
    w.width = wj.value("width").toDouble(100);
    w.pan = wj.value("pan").toDouble(0);
    w.muted = wj.value("muted").toBool();
    w.readJump(wj);
    emit changed();
}

void AppState::select(int chain) {
    link.request({{"cmd", "select"}, {"chain", chain}}, [this](const QJsonObject& r) { check(r, "Could not play that chain"); });
}

void AppState::step(int delta) {
    link.request({{"cmd", "select"}, {"step", delta}}, [this](const QJsonObject& r) { check(r, "Could not change chains"); });
}

void AppState::run(const QString& action) {
    link.request({{"cmd", "do"}, {"action", action}}, [this, action](const QJsonObject& r) { check(r, action); });
}

void AppState::save() {
    link.request({{"cmd", "save"}}, [this](const QJsonObject& r) {
        if (!check(r, "Not saved"))
            return;
        QString text = "Saved " + QFileInfo(r.value("path").toString()).fileName();
        const QJsonArray warnings = r.value("warnings").toArray();
        if (!warnings.isEmpty())
            text += ", but " + warnings.first().toString();
        emit message(text, !warnings.isEmpty());
    });
}


void AppState::edited(const Edit& e) {
    undo_.append(e);
    if (undo_.size() > 200)
        undo_.removeFirst();
    redo_.clear();
    emit undoChanged();
}

void AppState::undo() {
    if (undo_.isEmpty())
        emit message("Nothing to undo", false);
    else
        takeBack(undo_, redo_, true);
}

void AppState::redo() {
    if (redo_.isEmpty())
        emit message("Nothing to redo", false);
    else
        takeBack(redo_, undo_, false);
}

void AppState::takeBack(QVector<Edit>& from, QVector<Edit>& to, bool undoing) {
    const Edit e = from.takeLast();
    to.append(e);
    emit undoChanged();
    // One request, or a batch of them, in order: what is said comes with the last.
    const QJsonObject request = undoing ? e.undo : e.redo;
    QVector<QJsonObject> requests;
    if (request.value("cmd").toString() == "batch") {
        for (const QJsonValue& v : request.value("requests").toArray())
            requests << v.toObject();
    } else {
        requests << request;
    }
    for (int i = 0; i + 1 < requests.size(); ++i)
        link.request(requests[i], [this, undoing, one = requests[i]](const QJsonObject& r) {
            follow(one, r);
            check(r, undoing ? "Not undone" : "Not redone");
        });
    const QJsonObject last = requests.isEmpty() ? QJsonObject() : requests.last();
    link.request(last, [this, e, undoing, last](const QJsonObject& r) {
        follow(last, r);
        if (!check(r, undoing ? "Not undone" : "Not redone"))
            return;
        emit message((undoing ? "Undone: " : "Redone: ") + e.what, false);
        emit undone(e.chain, undoing || e.redoSlot == -2 ? e.slot : e.redoSlot);
    });
}

} // namespace cue::gui
