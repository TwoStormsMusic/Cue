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
}

void Settings::save() const {
    QSettings s(path(), QSettings::IniFormat);
    s.setValue("start_view", startView);
    s.setValue("times", percent ? "percent" : "ms");
    s.setValue("scale", scale);
    s.setValue("tuner_when_muted", tunerWhenMuted);
    s.setValue("full_screen", fullScreen);
}

AppState::AppState(EngineLink& l, QObject* parent) : QObject(parent), link(l) {
    connect(&link, &EngineLink::connectionChanged, this, &AppState::onConnection);
    connect(&link, &EngineLink::event, this, &AppState::onEvent);
    link.setEvents({"status", "log", "tuner"});
}

void AppState::onConnection(bool up) {
    if (up) {
        link.request({{"cmd", "hello"}}, [this](const QJsonObject& r) {
            if (r.value("ok").toBool()) {
                hello = r;
                emit changed();
            }
        });
        reloadPerformance();
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
    } else if (name == "log") {
        log.append({m.value("level").toString(), m.value("text").toString(), QDateTime::currentDateTime()});
        if (log.size() > 200)
            log.remove(0, log.size() - 200);
        if (m.value("level").toString() != "info")
            emit message(m.value("text").toString(), true);
    }
}

const ChainInfo* AppState::chain(int index) const {
    return index >= 0 && index < perf.chains.size() ? &perf.chains[index] : nullptr;
}

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

void AppState::select(int chain) {
    link.request({{"cmd", "select"}, {"index", chain}}, [this](const QJsonObject& r) { check(r, "Could not play that chain"); });
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

} // namespace cue::gui
