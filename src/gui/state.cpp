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
}

AppState::AppState(EngineLink& l, QObject* parent) : QObject(parent), link(l) {
    connect(&link, &EngineLink::connectionChanged, this, &AppState::onConnection);
    connect(&link, &EngineLink::event, this, &AppState::onEvent);
    link.setEvents({"status", "log", "tuner", "performance", "wire", "scan", "plugins", "edited"});
}

void AppState::onConnection(bool up) {
    // What could be undone was for the engine that was here.
    undo_.clear();
    redo_.clear();
    emit undoChanged();
    if (up) {
        link.request({{"cmd", "hello"}}, [this](const QJsonObject& r) {
            if (r.value("ok").toBool()) {
                hello = r;
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
    } else if (r.contains("wire")) {
        setWire(request.value("chain").toInt(-1), r.value("wire").toObject());
    } else if (request.value("cmd").toString() == "place") {
        ChainInfo* c = perf.chain(request.value("chain").toInt(-1));
        const int slot = request.value("slot").toInt(-1);
        if (c && slot >= 0 && slot < c->blocks.size()) {
            PluginInfo& b = c->blocks[slot];
            b.placed = true;
            b.at = QPointF(request.value("x").toDouble(), request.value("y").toDouble());
            emit changed();
        }
    }
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
        return from ? PortInfo::input() : PortInfo::output(perf.outputs.indexOf(v.toString()));
    };
    const int i = c.wire(port(wj.value("from"), true), port(wj.value("to"), false));
    if (i < 0)
        return;
    WireInfo& w = c.wires[i];
    w.levelDb = wj.value("level_db").toDouble(0);
    w.width = wj.value("width").toDouble(100);
    w.pan = wj.value("pan").toDouble(0);
    w.muted = wj.value("muted").toBool();
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
