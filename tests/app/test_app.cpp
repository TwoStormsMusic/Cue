// The app's tests: what it reads from the engine, its line to the engine,
// every view at every screen size, what taps ask for, and the app against a
// real `cue run --audio none`. Run offscreen: QT_QPA_PLATFORM=offscreen.

#include "gui/chainview.h"
#include "gui/link.h"
#include "gui/mainwindow.h"
#include "gui/model.h"
#include "gui/settingsview.h"
#include "gui/stageview.h"
#include "gui/state.h"
#include "gui/theme.h"
#include "gui/tunerview.h"

#include <QApplication>
#include <QFile>
#include <QLineF>
#include <QWheelEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

using namespace cue::gui;

namespace {

// Built-in paths, or where an unpacked package keeps them (scripts/run-tests.sh).
QString fromEnv(const char* name, const char* built) {
    const QByteArray v = qgetenv(name);
    return v.isEmpty() ? QString(built) : QString::fromLocal8Bit(v);
}

QJsonObject parse(const char* json) { return QJsonDocument::fromJson(json).object(); }

// Shaped like configs/app-demo.ini: a branch to FOH, five chains, a wah, a tuner pedal.
const char* kPerformance = R"({
  "cue": 1, "name": "App demo", "inputs": ["Bass"], "outputs": ["FOH", "Amp"],
  "tuner": {"input": "Bass", "reference": 440, "range": "bass", "tolerance": 2, "names": "sharps"},
  "scenes": [{"name": "Cue Template", "chains": [
    {"name": "Punk", "display": "PU", "pedal": 6, "warm": "next", "lines": [{"plugins": [
       {"format": "clap", "path": "~/x/Test.clap", "id": "cue.test.gain", "label": "Gain Stage", "bypassed": false, "bypass_mode": "thru"},
       {"format": "builtin", "args": ["drive", "3"], "label": "Darkglass Ultra", "bypassed": false, "bypass_mode": "thru"}]}],
     "outputs": {"FOH": {"line": "", "after": 2}, "Amp": {"line": "", "after": 2}}},
    {"name": "Slap", "display": "SL", "pedal": 7, "warm": "next", "lines": [
      {"plugins": [
       {"format": "clap", "path": "~/x/Test.clap", "id": "cue.test.gain", "label": "Gain Stage", "bypassed": false, "bypass_mode": "thru"},
       {"format": "builtin", "args": ["lowpass", "2500"], "label": "Wah", "bypassed": true, "bypass_mode": "thru"},
       {"format": "builtin", "args": ["gain", "1"], "label": "Act", "bypassed": false, "bypass_mode": "thru"}]},
      {"name": "FOH", "from": "", "after": 3, "plugins": [
       {"format": "builtin", "args": ["lowpass", "6000"], "label": "IR Loader", "bypassed": false, "bypass_mode": "mute"},
       {"format": "clap", "path": "~/x/Test.clap", "id": "cue.test.delay", "label": "Chorus", "bypassed": true, "bypass_mode": "thru"}]}],
     "outputs": {"FOH": {"line": "FOH", "after": 2}, "Amp": {"line": "", "after": 3}}},
    {"name": "2000s", "pedal": 8, "warm": "next", "lines": [{"plugins": []}], "outputs": {}},
    {"name": "Motown", "pedal": 9, "warm": "next", "lines": [{"plugins": []}], "outputs": {}},
    {"name": "Djent", "pedal": 10, "warm": "next", "lines": [{"plugins": []}], "outputs": {}}]}],
  "controls": [
    {"name": "Effect 1", "type": "toggle", "pedal": 1, "mute": []},
    {"name": "Wah", "type": "discrete", "values": ["Off", "Auto", "Pedal"], "pedal": 4, "mute": []},
    {"name": "Mix", "type": "continuous", "min": 0, "max": 1, "mute": []},
    {"name": "Tuner", "type": "toggle", "pedal": 5, "mute": ["on"]}]
})";

const char* kStatus = R"({
  "event": "status", "dsp": {"avg": 34, "max": 60}, "margin_ms": 1.2, "xruns": 0, "period_ms": 1.3333, "buffer_ms": 4.0,
  "scene": {"index": 0, "name": "Cue Template"}, "selected": {"index": 1, "name": "Slap"}, "muted": false, "display": "SL",
  "leds": [true, false, false, false, false, false, true, false, false, false],
  "controls": [{"name": "Effect 1", "value": 1, "text": "on"}, {"name": "Wah", "value": 0, "text": "Off"},
               {"name": "Mix", "value": 0.4, "text": "0.4"}, {"name": "Tuner", "value": 0, "text": "off"}],
  "chains": [{"name": "Punk", "state": "idle", "bypassed": []},
             {"name": "Slap", "state": "active", "bypassed": [1, 4]},
             {"name": "2000s", "state": "warm", "bypassed": []},
             {"name": "Motown", "state": "idle", "bypassed": []},
             {"name": "Djent", "state": "idle", "bypassed": []}],
  "levels": {"inputs": [{"name": "Bass", "peak_db": -12}], "outputs": [{"name": "FOH", "peak_db": -3}, {"name": "Amp", "peak_db": -60}]}
})";

const char* kTuner = R"({"event": "tuner", "input": "Bass", "signal": true, "level_db": -20, "note": "E", "octave": 1, "midi": 28,
  "cents": 2.2, "hz": 41.26, "held": false, "in_tune": false, "reference": 440, "range": "bass", "tolerance": 2, "names": "sharps"})";

// A link that is always up and records what is asked; replies come from
// answer(), or ok.
class FakeLink : public EngineLink {
public:
    FakeLink() : EngineLink("/nonexistent/engine.sock") {}
    bool connected() const override { return up; }
    void request(QJsonObject r, Reply reply) override {
        sent << r;
        if (reply)
            reply(answer ? answer(r) : QJsonObject{{"ok", true}});
    }
    bool up = true;
    QList<QJsonObject> sent;
    std::function<QJsonObject(const QJsonObject&)> answer;
    QJsonObject last(const QString& cmd) const {
        for (int i = sent.size() - 1; i >= 0; --i)
            if (sent[i].value("cmd").toString() == cmd)
                return sent[i];
        return {};
    }
};

void fill(AppState& s, bool muted = false) {
    s.perf = Performance::fromJson(parse(kPerformance));
    QJsonObject st = parse(kStatus);
    st["muted"] = muted;
    s.status = Status::fromJson(st);
    s.tuner = TunerReading::fromJson(parse(kTuner));
    s.hello = parse(R"({"ok": true, "cue": "0.5.0", "audio_settings": {"device": "hw:M2", "rate": 48000, "period": 64, "periods": 3},
                        "midi": {"enabled": true, "in": "M2", "profile": "eurekaprom", "ports": 1}})");
}

// With CUE_APP_SHOTS set, a picture of the view for a person to look at.
void shot(QWidget* w, const QString& name) {
    if (const QByteArray dir = qgetenv("CUE_APP_SHOTS"); !dir.isEmpty())
        w->grab().save(QString("%1/%2.png").arg(QString(dir), name));
}

void click(QWidget* w, const QRectF& r) {
    QVERIFY2(!r.isEmpty(), "nothing to tap there");
    QTest::mouseClick(w, Qt::LeftButton, {}, r.center().toPoint());
}

void writeFrame(QLocalSocket* c, const QJsonObject& m) {
    const QByteArray body = QJsonDocument(m).toJson(QJsonDocument::Compact);
    char head[4];
    qToBigEndian<quint32>(body.size(), head);
    c->write(head, 4);
    c->write(body);
    c->flush();
}

} // namespace

class AppTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        loadFonts();
    }

    void performance_reads_chains_lines_and_taps() {
        const Performance p = Performance::fromJson(parse(kPerformance));
        QCOMPARE(p.name, QString("App demo"));
        QCOMPARE(p.chains.size(), 5);
        QCOMPARE(p.scenes[0].chains, (QVector<int>{0, 1, 2, 3, 4}));
        const ChainInfo& slap = p.chains[1];
        QCOMPARE(slap.lines.size(), 2);
        QCOMPARE(slap.lines[1].parent, 0);
        QCOMPARE(slap.lines[1].after, 3);
        // Slots count line by line, as the engine numbers them.
        QCOMPARE(slap.plugin(3)->label, QString("IR Loader"));
        QVERIFY(slap.plugin(3)->muteOnBypass);
        QCOMPARE(slap.numPlugins(), 5);
        // A tap per output, in the performance's order.
        QCOMPARE(slap.taps.size(), 2);
        QCOMPARE(slap.taps[0].output, QString("FOH"));
        QCOMPARE(slap.taps[0].line, 1);
        QCOMPARE(slap.taps[1].after, 3);
        // An output the file leaves out takes the end of the main line.
        QCOMPARE(p.chains[2].taps[0].after, 0);
        QCOMPARE(p.control("wah")->values.size(), 3);
        QCOMPARE(p.control("Effect 1")->values, (QStringList{"off", "on"}));
        QVERIFY(p.control("Tuner")->mutes);
        QCOMPARE(p.chains[0].plugin(1)->label, QString("Darkglass Ultra"));
    }

    void status_and_tuner_read_what_the_engine_sends() {
        const Status s = Status::fromJson(parse(kStatus));
        QCOMPARE(s.selected, 1);
        QCOMPARE(s.warmChain(), 2);
        QVERIFY(s.bypassed(1, 4));
        QVERIFY(!s.bypassed(1, 0));
        QCOMPARE(s.outputs.size(), 2);
        QCOMPARE(s.outputs[0].peakDb, -3.0);
        QCOMPARE(s.control("wah")->text, QString("Off"));
        // 1.2 ms of the 2.67 ms the third period leaves.
        QVERIFY(std::abs(*s.marginPercent() - 45.0) < 0.1);
        const TunerReading t = TunerReading::fromJson(parse(kTuner));
        QCOMPARE(t.note, QString("E"));
        QCOMPARE(t.string(), QString("string 4"));
        QCOMPARE(tunerdraw::centsText(t), QString("+2.2¢"));
        QCOMPARE(audioSummary(parse(R"({"audio_settings": {"device": "hw:M2,0", "rate": 48000, "period": 64, "periods": 3}})")),
                 QString("M2 · 48k · 64×3"));
    }

    void link_connects_when_the_engine_comes_and_again_after_it_goes() {
        QTemporaryDir dir;
        const QString path = dir.filePath("engine.sock");
        EngineLink link(path);
        link.setEvents({"status"});
        QSignalSpy up(&link, &EngineLink::connectionChanged);
        link.start();
        QTest::qWait(300);
        QVERIFY(!link.connected());

        // The engine starts: the link finds it within a second or so.
        QLocalServer server;
        QVERIFY(server.listen(path));
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000);
        QLocalSocket* c = server.nextPendingConnection();
        QTRY_VERIFY_WITH_TIMEOUT(link.connected(), 3000);
        // It subscribes on its own.
        QTRY_VERIFY_WITH_TIMEOUT(c->bytesAvailable() >= 4, 2000);
        QByteArray in = c->readAll();
        QVERIFY(in.contains("\"subscribe\""));

        // A reply finds its request; an event comes out as one.
        QJsonObject got;
        link.request({{"cmd", "hello"}}, [&](const QJsonObject& r) { got = r; });
        QTRY_VERIFY_WITH_TIMEOUT(c->bytesAvailable() >= 4 || in.contains("hello"), 2000);
        in += c->readAll();
        const int id = QJsonDocument::fromJson(in.mid(in.lastIndexOf('{'))).object().value("id").toInt();
        QSignalSpy events(&link, &EngineLink::event);
        writeFrame(c, {{"id", id}, {"ok", true}, {"cue", "test"}});
        writeFrame(c, parse(kStatus));
        QTRY_COMPARE_WITH_TIMEOUT(got.value("cue").toString(), QString("test"), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(events.size(), 1, 2000);
        QCOMPARE(events[0][0].toString(), QString("status"));

        // The engine goes: a request waiting fails, and the link keeps trying.
        QJsonObject failed;
        link.request({{"cmd", "status"}}, [&](const QJsonObject& r) { failed = r; });
        QTest::qWait(50);
        c->abort();
        server.close();
        QTRY_VERIFY_WITH_TIMEOUT(!link.connected(), 2000);
        QCOMPARE(failed.value("ok").toBool(true), false);
        QVERIFY(server.listen(path));
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(link.connected(), 3000);
    }

    void every_view_draws_at_every_size() {
        FakeLink link;
        AppState s(link);
        fill(s);
        MainWindow w(s);
        for (const QSize size : {QSize(800, 480), QSize(1024, 600), QSize(1280, 800), QSize(1920, 1080), QSize(2560, 1440), QSize(600, 1024)}) {
            w.resize(size);
            for (const char* view : {"stage", "tuner", "chains", "settings"}) {
                w.showView(view);
                Surface* v = w.view(view);
                v->resize(size);
                const QImage img = v->grab().toImage();
                QCOMPARE(img.size(), size);
                shot(v, QString("%1-%2x%3").arg(view).arg(size.width()).arg(size.height()));
                // Everything tappable is on the screen.
                for (const QString& id : v->hitIds()) {
                    const QRectF r = v->hitRect(id);
                    if (id == "params")
                        continue;
                    QVERIFY2(QRectF(0, 0, size.width(), size.height()).adjusted(-1, -1, 1, 1).contains(r),
                             qPrintable(QString("%1 at %2x%3 is off the screen").arg(id).arg(size.width()).arg(size.height())));
                }
            }
            // The chain buttons never overlap.
            Surface* stage = w.view("stage");
            w.showView("stage");
            stage->grab();
            for (int a = 0; a < 5; ++a)
                for (int b = a + 1; b < 5; ++b)
                    QVERIFY(!stage->hitRect(QString("chain %1").arg(a)).intersects(stage->hitRect(QString("chain %1").arg(b))));
        }
        w.resize(1280, 860);
        for (const char* view : {"stage", "tuner", "chains", "settings"}) {
            w.showView(view);
            w.view(view)->resize(1280, 860);
            shot(w.view(view), view);
        }
    }

    void the_tuner_draws_every_reading() {
        FakeLink link;
        AppState s(link);
        fill(s);
        MainWindow w(s);
        w.resize(1280, 800);
        struct Case {
            const char* name;
            QJsonObject change;
            bool muted;
        };
        const QJsonValue none(QJsonValue::Null);
        const Case cases[] = {
            {"out", {}, false},
            {"in-tune", {{"cents", -0.6}, {"in_tune", true}}, false},
            {"sharp-name", {{"note", "F#"}, {"octave", 2}, {"cents", -14.5}, {"hz", 91.71}}, false},
            {"flat-name", {{"note", "Bb"}, {"octave", 1}, {"cents", 31}, {"hz", 59.33}, {"names", "flats"}}, false},
            {"off-scale", {{"cents", -63}}, false},
            {"held", {{"held", true}, {"signal", false}, {"level_db", -90}}, false},
            {"no-note", {{"note", none}, {"signal", true}}, false},
            {"no-audio", {{"note", none}, {"signal", false}, {"level_db", -200}}, true},
        };
        for (const Case& c : cases) {
            QJsonObject t = parse(kTuner);
            for (auto it = c.change.begin(); it != c.change.end(); ++it)
                t[it.key()] = it.value();
            fill(s, c.muted);
            s.tuner = TunerReading::fromJson(t);
            // The strobe turns with the error and stands still on a held note.
            const double turned = s.tuner.turn(0.5, 0.1);
            QVERIFY(turned >= 0 && turned < 1);
            if (s.tuner.held || s.tuner.note.isEmpty())
                QCOMPARE(turned, 0.5);
            else
                QVERIFY(std::abs(turned - 0.5) > 1e-6);
            for (const char* view : {"tuner", "stage"}) {
                w.showView(view);
                w.view(view)->resize(1280, 800);
                QVERIFY(!w.view(view)->grab().isNull());
                shot(w.view(view), QString("%1-%2").arg(view, c.name));
            }
        }
        QCOMPARE(tunerdraw::centsText(s.tuner), QString());
        s.tuner.note = "A";
        s.tuner.cents = -0.04;
        QCOMPARE(tunerdraw::centsText(s.tuner), QString("0.0¢"));
        s.tuner.cents = -3.26;
        QCOMPARE(tunerdraw::centsText(s.tuner), QString("−3.3¢"));
    }

    void stage_taps_ask_what_the_pedals_would() {
        FakeLink link;
        AppState s(link);
        fill(s);
        MainWindow w(s);
        w.resize(1280, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        Surface* v = w.view("stage");
        v->grab();
        click(v, v->hitRect("chain 3"));
        QCOMPARE(link.last("select").value("index").toInt(), 3);
        click(v, v->hitRect("control Effect 1"));
        QCOMPARE(link.last("do").value("action").toString(), QString("Effect 1 = toggle"));
        click(v, v->hitRect("control Wah = Auto"));
        QCOMPARE(link.last("do").value("action").toString(), QString("Wah = Auto"));
        // A slider sets the control where the finger lifts.
        const QRectF mix = v->hitRect("control Mix");
        QTest::mousePress(v, Qt::LeftButton, {}, QPoint(mix.left() + 2, mix.center().y()));
        QTest::mouseRelease(v, Qt::LeftButton, {}, QPoint(mix.left() + mix.width() * 3 / 4, mix.center().y()));
        const QString set = link.last("do").value("action").toString();
        QVERIFY2(set.startsWith("Mix = 0.7"), qPrintable(set));
        // The strip opens the tuner.
        click(v, v->hitRect("tuner strip"));
        QCOMPARE(w.current(), QString("tuner"));
    }

    void the_tuner_comes_up_while_the_inputs_are_muted() {
        FakeLink link;
        AppState s(link);
        fill(s);
        MainWindow w(s);
        w.resize(1280, 800);
        w.showView("chains");
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        fill(s, true);
        emit s.statusChanged();
        QCOMPARE(w.current(), QString("tuner"));
        fill(s, false);
        emit s.statusChanged();
        QCOMPARE(w.current(), QString("chains"));
        // Not when the setting is off.
        s.settings.tunerWhenMuted = false;
        fill(s, true);
        emit s.statusChanged();
        QCOMPARE(w.current(), QString("chains"));
        // The mute button goes through the control that mutes.
        w.showView("tuner");
        w.view("tuner")->resize(1280, 800);
        w.view("tuner")->grab();
        fill(s, false);
        w.view("tuner")->grab();
        click(w.view("tuner"), w.view("tuner")->hitRect("mute"));
        QCOMPARE(link.last("do").value("action").toString(), QString("Tuner = toggle"));
    }

    void chain_view_switches_plugins_and_moves_their_parameters() {
        FakeLink link;
        link.answer = [](const QJsonObject& r) -> QJsonObject {
            if (r.value("cmd") == "plugin")
                return parse(R"({"ok": true, "plugin": {"chain": 1, "slot": 0, "label": "Gain Stage", "name": "Test Gain", "format": "clap",
                                  "bypassed": false, "params": [{"id": 0, "name": "Gain", "module": "", "min": 0, "max": 2, "default": 1,
                                  "steps": 0, "value": 1, "text": "1.000"}]}})");
            if (r.value("cmd") == "param")
                return {{"ok", true}, {"text", "ok"}};
            return {{"ok", true}};
        };
        AppState s(link);
        fill(s);
        MainWindow w(s);
        w.resize(1280, 800);
        w.showView("chains");
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* v = static_cast<ChainView*>(w.view("chains"));
        QCOMPARE(v->shownChain(), 1); // the chain playing
        v->grab();
        // Every plug-in of the chain is a block.
        for (int slot = 0; slot < 5; ++slot)
            QVERIFY2(!v->hitRect(QString("block %1").arg(slot)).isEmpty(), qPrintable(QString("block %1").arg(slot)));
        click(v, v->hitRect("block 0"));
        QCOMPARE(v->selectedSlot(), 0);
        QCOMPARE(link.last("plugin").value("slot").toInt(), 0);
        v->grab();
        const QRectF slider = v->hitRect("param 0");
        QVERIFY(!slider.isEmpty());
        QTest::mousePress(v, Qt::LeftButton, {}, slider.center().toPoint());
        QTest::mouseRelease(v, Qt::LeftButton, {}, QPoint(slider.right() - 2, slider.center().y()));
        const QJsonObject param = link.last("param");
        QCOMPARE(param.value("chain").toInt(), 1);
        QCOMPARE(param.value("param").toInt(), 0);
        QVERIFY(param.value("value").toDouble() > 1.8 && param.value("value").toDouble() <= 2.0);
        shot(v, "chains-selected");
        // A plug-in with no editor of its own offers no button for one.
        QVERIFY(v->hitRect("editor").isEmpty());
        click(v, v->hitRect("bypass"));
        const QJsonObject bypass = link.last("bypass");
        QCOMPARE(bypass.value("slot").toInt(), 0);
        QCOMPARE(bypass.value("bypassed").toBool(), true);
        // Another chain from the list, and play it.
        v->grab();
        click(v, v->hitRect("show 0"));
        QCOMPARE(v->shownChain(), 0);
        v->grab();
        click(v, v->hitRect("play"));
        QCOMPARE(link.last("select").value("index").toInt(), 0);
        click(v, v->hitRect("save"));
        QVERIFY(!link.last("save").isEmpty());
    }

    void the_chain_view_opens_a_plugins_own_editor() {
        FakeLink link;
        bool open = false;
        link.answer = [&open](const QJsonObject& r) -> QJsonObject {
            if (r.value("cmd") == "editor")
                open = r.value("open").toBool();
            if (r.value("cmd") == "plugin" || r.value("cmd") == "editor")
                return QJsonObject{{"ok", true},
                                   {"plugin", QJsonObject{{"chain", 1},
                                                          {"slot", 0},
                                                          {"label", "Parallax"},
                                                          {"name", "Parallax"},
                                                          {"format", "vst3"},
                                                          {"bypassed", false},
                                                          {"editor", true},
                                                          {"editor_open", open},
                                                          {"params", QJsonArray{}}}}};
            return {{"ok", true}};
        };
        AppState s(link);
        fill(s);
        MainWindow w(s);
        w.resize(1280, 800);
        w.showView("chains");
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* v = static_cast<ChainView*>(w.view("chains"));
        v->grab();
        click(v, v->hitRect("block 0"));
        v->grab();
        const QRectF button = v->hitRect("editor");
        QVERIFY2(!button.isEmpty(), "no button to open the editor");
        click(v, button);
        QCOMPARE(link.last("editor").value("chain").toInt(), 1);
        QCOMPARE(link.last("editor").value("slot").toInt(), 0);
        QCOMPARE(link.last("editor").value("open").toBool(), true);
        // Open, the same button closes it again.
        v->grab();
        click(v, v->hitRect("editor"));
        QCOMPARE(link.last("editor").value("open").toBool(), false);
    }

    void chain_view_zooms_and_pans() {
        FakeLink link;
        AppState s(link);
        fill(s);
        MainWindow w(s);
        w.resize(1280, 800);
        w.showView("chains");
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* v = static_cast<ChainView*>(w.view("chains"));
        v->grab();
        const QRectF canvas = v->hitRect("canvas");
        QVERIFY(!canvas.isEmpty());
        const QRectF before = v->hitRect("block 0");
        // The wheel zooms in where it points: what is there stays there.
        const QPointF at = before.center();
        QWheelEvent wheel(at, v->mapToGlobal(at), {}, QPoint(0, 240), Qt::NoButton, {}, Qt::NoScrollPhase, false);
        QApplication::sendEvent(v, &wheel);
        QVERIFY(v->zoom() > 1.3);
        v->grab();
        const QRectF after = v->hitRect("block 0");
        QVERIFY(after.width() > before.width() * 1.2);
        QVERIFY(QLineF(after.center(), before.center()).length() < 3);
        shot(v, "chains-zoomed");
        // A drag on open space pans.
        const QPoint from = (canvas.topLeft() + QPointF(30, 30)).toPoint();
        QTest::mousePress(v, Qt::LeftButton, {}, from);
        QTest::mouseMove(v, from + QPoint(80, 40));
        QTest::mouseRelease(v, Qt::LeftButton, {}, from + QPoint(80, 40));
        QVERIFY(QLineF(v->pan(), QPointF()).length() > 10);
        v->grab();
        QVERIFY(v->hitRect("block 0").center().x() > after.center().x() + 40);
        // A double tap there fits it again; so does Fit.
        QTest::mouseClick(v, Qt::LeftButton, {}, from);
        QTest::mouseClick(v, Qt::LeftButton, {}, from);
        QCOMPARE(v->zoom(), 1.0);
        QCOMPARE(v->pan(), QPointF());
        v->grab();
        click(v, v->hitRect("zoom in"));
        QVERIFY(v->zoom() > 1.2);
        v->grab();
        click(v, v->hitRect("fit"));
        QCOMPARE(v->zoom(), 1.0);
        // Two fingers pinch it bigger.
        v->grab();
        const QPointF c = canvas.center();
        QPointingDevice* screen = QTest::createTouchDevice();
        QTest::touchEvent(v, screen).press(0, (c - QPointF(40, 0)).toPoint()).press(1, (c + QPointF(40, 0)).toPoint());
        QTest::touchEvent(v, screen).move(0, (c - QPointF(80, 0)).toPoint()).move(1, (c + QPointF(80, 0)).toPoint());
        QTest::touchEvent(v, screen).release(0, (c - QPointF(80, 0)).toPoint()).release(1, (c + QPointF(80, 0)).toPoint());
        QVERIFY2(v->zoom() > 1.8 && v->zoom() < 2.2, qPrintable(QString::number(v->zoom())));
        // One finger on a block still taps it.
        v->grab();
        const QPoint b = v->hitRect("block 1").center().toPoint();
        QTest::touchEvent(v, screen).press(0, b);
        QTest::touchEvent(v, screen).release(0, b);
        QCOMPARE(v->selectedSlot(), 1);
    }

    void chain_view_shows_the_picked_parameters() {
        // A plug-in with sections and more parameters than fit, one moved by a pedal.
        QJsonArray params;
        for (int i = 0; i < 20; ++i) {
            QJsonObject q{{"id", 100 + i}, {"name", QString("Knob %1").arg(i)}, {"module", i < 10 ? "Amp" : "Cab"}, {"min", 0}, {"max", 1},
                          {"default", 0.5}, {"steps", 0}, {"value", 0.5}, {"text", "50%"}};
            if (i == 15)
                q["tied"] = QJsonArray{QJsonObject{{"control", "Grunt"}, {"follows", "expression 1"}, {"pedal", 0}}};
            params.append(q);
        }
        QJsonValue shown; // as the engine keeps it
        FakeLink link;
        link.answer = [&](const QJsonObject& r) -> QJsonObject {
            if (r.value("cmd") == "shown")
                shown = r.value("params");
            if (r.value("cmd") == "plugin" || r.value("cmd") == "shown") {
                QJsonObject plugin{{"chain", 1}, {"slot", 1}, {"label", "Darkglass Ultra"}, {"name", "Darkglass Ultra"}, {"format", "vst3"},
                                   {"bypassed", false}, {"editor", true}, {"params", params}};
                if (shown.isArray())
                    plugin["shown"] = shown;
                return {{"ok", true}, {"plugin", plugin}};
            }
            return {{"ok", true}};
        };
        AppState s(link);
        fill(s);
        MainWindow w(s);
        w.resize(1280, 800);
        w.showView("chains");
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* v = static_cast<ChainView*>(w.view("chains"));
        v->grab();
        click(v, v->hitRect("block 1"));
        v->grab();
        // Nothing picked yet: the first eight, and the one the pedal moves.
        QCOMPARE(v->shownParams(), (QVector<int>{100, 101, 102, 103, 104, 105, 106, 107, 115}));
        QVERIFY(!v->hitRect("param 100").isEmpty());
        QVERIFY(v->hitRect("param 110").isEmpty());
        shot(v, "chains-params");
        // All of them: uncheck one, check another; the pedal's stays.
        click(v, v->hitRect("show all"));
        QVERIFY(v->showingAll());
        v->grab();
        QVERIFY(!v->hitRect("collapse").isEmpty());
        QVERIFY(!v->hitRect("section Amp").isEmpty());
        click(v, v->hitRect("pick 101"));
        v->grab();
        QCOMPARE(link.last("shown").value("params").toArray().size(), 8);
        QCOMPARE(v->shownParams().contains(101), false);
        // Fold the first section, so the second's come up.
        click(v, v->hitRect("section Amp"));
        v->grab();
        QVERIFY(v->hitRect("pick 100").isEmpty());
        click(v, v->hitRect("pick 112"));
        v->grab();
        QCOMPARE(v->shownParams().last(), 112);
        click(v, v->hitRect("pick 115"));
        QVERIFY(v->shownParams().contains(115));
        shot(v, "chains-all-params");
        click(v, v->hitRect("collapse"));
        QVERIFY(!v->showingAll());
        // The third moved to the top by its grip.
        v->grab();
        const QRectF grip = v->hitRect("move 102");
        const QRectF first = v->hitRect("move 100");
        QVERIFY(!grip.isEmpty() && !first.isEmpty());
        QTest::mousePress(v, Qt::LeftButton, {}, grip.center().toPoint());
        QTest::mouseMove(v, QPoint(grip.center().x(), first.top() + 4));
        v->grab();
        shot(v, "chains-moving-param");
        QTest::mouseRelease(v, Qt::LeftButton, {}, QPoint(grip.center().x(), first.top() + 4));
        QCOMPARE(v->shownParams().first(), 102);
        QCOMPARE(link.last("shown").value("params").toArray().first().toInt(), 102);
        QCOMPARE(v->shownParams().last(), 112);
        QCOMPARE(link.last("shown").value("slot").toInt(), 1);
        // Undo puts it back.
        QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(link.last("shown").value("params").toArray().first().toInt(), 100);
        QCOMPARE(v->shownParams().first(), 100);
    }

    void chain_view_double_taps_play_chains_and_switch_plugins() {
        FakeLink link;
        auto count = [&link](const QString& cmd) {
            int n = 0;
            for (const QJsonObject& r : link.sent)
                n += r.value("cmd").toString() == cmd;
            return n;
        };
        AppState s(link);
        fill(s);
        MainWindow w(s);
        w.resize(1280, 800);
        w.showView("chains");
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* v = static_cast<ChainView*>(w.view("chains"));
        v->grab();
        // A chain in the list: one tap shows it, two play it.
        click(v, v->hitRect("show 2"));
        QCOMPARE(v->shownChain(), 2);
        QCOMPARE(count("select"), 0);
        v->grab();
        click(v, v->hitRect("show 0"));
        v->grab();
        click(v, v->hitRect("show 0"));
        QCOMPARE(v->shownChain(), 0);
        QCOMPARE(count("select"), 1);
        QCOMPARE(link.last("select").value("index").toInt(), 0);

        // Not playing yet (the engine has not said so), its wires are green
        // all the same: they would carry sound if it played.
        auto greenBetween = [v](const QString& a, const QString& b) {
            const QImage img = v->grab().toImage();
            const QRectF ra = v->hitRect(a), rb = v->hitRect(b);
            const int x = static_cast<int>((ra.right() + rb.left()) / 2) * img.devicePixelRatio();
            for (int dy = -3; dy <= 3; ++dy) {
                const QColor c = img.pixelColor(x, static_cast<int>(ra.center().y() + dy) * img.devicePixelRatio());
                if (c.green() > 150 && c.red() < 150)
                    return true;
            }
            return false;
        };
        QVERIFY(s.playing() != 0);
        QVERIFY2(greenBetween("block 0", "block 1"), "the wire of a chain not playing is not green");
        shot(v, "chains-not-playing");
        // With the inputs muted, nothing carries sound.
        fill(s, true);
        QVERIFY(!greenBetween("block 0", "block 1"));
        fill(s);

        // A plug-in: two taps switch it off and leave it selected.
        click(v, v->hitRect("show 1"));
        v->grab();
        click(v, v->hitRect("block 0"));
        QCOMPARE(v->selectedSlot(), 0);
        QCOMPARE(count("bypass"), 0);
        v->grab();
        click(v, v->hitRect("block 0"));
        QCOMPARE(count("bypass"), 1);
        QCOMPARE(link.last("bypass").value("slot").toInt(), 0);
        QCOMPARE(link.last("bypass").value("bypassed").toBool(), true);
        QCOMPARE(v->selectedSlot(), 0);
        // The Wah is bypassed: two taps switch it on.
        v->grab();
        click(v, v->hitRect("block 1"));
        v->grab();
        click(v, v->hitRect("block 1"));
        QCOMPARE(link.last("bypass").value("slot").toInt(), 1);
        QCOMPARE(link.last("bypass").value("bypassed").toBool(), false);
        // One tap on the selected one lets it go, once it is clear no second is coming.
        v->grab();
        click(v, v->hitRect("block 1"));
        QCOMPARE(v->selectedSlot(), 1);
        QTRY_COMPARE(v->selectedSlot(), -1);
        QCOMPARE(count("bypass"), 2);
    }

    void undo_takes_back_what_was_changed_here() {
        FakeLink link;
        link.answer = [](const QJsonObject& r) -> QJsonObject {
            if (r.value("cmd") == "plugin")
                return parse(R"({"ok": true, "plugin": {"chain": 1, "slot": 0, "label": "Gain Stage", "name": "Test Gain", "format": "clap",
                                  "bypassed": false, "params": [{"id": 0, "name": "Gain", "module": "", "min": 0, "max": 2, "default": 1,
                                  "steps": 0, "value": 1, "text": "1.000"}]}})");
            return {{"ok", true}};
        };
        AppState s(link);
        fill(s);
        MainWindow w(s);
        w.resize(1280, 800);
        w.showView("chains");
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto* v = static_cast<ChainView*>(w.view("chains"));
        QSignalSpy said(&s, &AppState::message);
        v->grab();
        QVERIFY(!s.canUndo());
        click(v, v->hitRect("undo"));
        QCOMPARE(said.last().at(0).toString(), QString("Nothing to undo"));

        // A slider dragged is one change, whatever it sent on the way.
        click(v, v->hitRect("block 0"));
        v->grab();
        const QRectF slider = v->hitRect("param 0");
        QTest::mousePress(v, Qt::LeftButton, {}, slider.center().toPoint());
        QTest::mouseMove(v, QPoint(slider.center().x() + 20, slider.center().y()));
        QTest::mouseRelease(v, Qt::LeftButton, {}, QPoint(slider.right() - 2, slider.center().y()));
        const double moved = link.last("param").value("value").toDouble();
        QVERIFY(moved > 1.8);
        QVERIFY(s.canUndo());
        QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(link.last("param").value("value").toDouble(), 1.0);
        QCOMPARE(link.last("param").value("chain").toInt(), 1);
        QCOMPARE(said.last().at(0).toString(), QString("Undone: Gain Stage: Gain"));
        QVERIFY(s.canRedo());
        QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(link.last("param").value("value").toDouble(), moved);

        // A bypass, undone by the button; a new change leaves nothing to redo.
        v->grab();
        click(v, v->hitRect("bypass"));
        QCOMPARE(link.last("bypass").value("bypassed").toBool(), true);
        v->grab();
        click(v, v->hitRect("undo"));
        QCOMPARE(link.last("bypass").value("bypassed").toBool(), false);
        QVERIFY(s.canRedo());
        v->grab();
        click(v, v->hitRect("redo"));
        QCOMPARE(link.last("bypass").value("bypassed").toBool(), true);
        click(v, v->hitRect("undo"));
        v->grab();
        click(v, v->hitRect("bypass"));
        QVERIFY(!s.canRedo());
        click(v, v->hitRect("undo"));

        // Undone from another chain, it shows where.
        v->grab();
        click(v, v->hitRect("show 0"));
        QCOMPARE(v->shownChain(), 0);
        QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(link.last("param").value("value").toDouble(), 1.0);
        QCOMPARE(v->shownChain(), 1);
        QCOMPARE(v->selectedSlot(), 0);
        QVERIFY(!s.canUndo());
        shot(v, "chains-undone");
    }

    void settings_are_kept() {
        FakeLink link;
        AppState s(link);
        fill(s);
        MainWindow w(s);
        w.resize(1280, 800);
        w.showView("settings");
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        Surface* v = w.view("settings");
        v->grab();
        click(v, v->hitRect("Start in: Tuner"));
        v->grab();
        click(v, v->hitRect("Show times in: %"));
        v->grab();
        click(v, v->hitRect("Range: Guitar"));
        QCOMPARE(link.last("tuner").value("range").toString(), QString("guitar"));
        Settings again;
        again.load();
        QCOMPARE(again.startView, QString("tuner"));
        QVERIFY(again.percent);
    }

    // The app against the real engine: it plays, switches, reads plug-ins,
    // and finds the engine again after it was stopped and started.
    void with_the_engine() {
        QTemporaryDir dir;
        QFile demo(fromEnv("CUE_CONFIGS", CUE_SOURCE_DIR "/configs") + "/app-demo.ini");
        QVERIFY(demo.open(QIODevice::ReadOnly));
        QByteArray text = demo.readAll();
        text.replace("../plugins/Cue Test Plugins.clap", fromEnv("CUE_TEST_CLAP", CUE_TEST_CLAP).toLocal8Bit());
        QFile ini(dir.filePath("demo.ini"));
        QVERIFY(ini.open(QIODevice::WriteOnly));
        ini.write(text);
        ini.close();
        const QString socket = dir.filePath("engine.sock");
        auto startEngine = [&](QProcess& engine) {
            engine.setProcessChannelMode(QProcess::MergedChannels);
            engine.start(fromEnv("CUE_BIN", CUE_BIN), {"run", ini.fileName(), "--audio", "none", "--socket", socket});
            QVERIFY(engine.waitForStarted());
        };
        QProcess engine;
        startEngine(engine);

        EngineLink link(socket);
        AppState s(link);
        link.start();
        QTRY_VERIFY_WITH_TIMEOUT(s.perf.valid && s.status.valid, 10000);
        QCOMPARE(s.perf.chains.size(), 5);
        QCOMPARE(s.perf.name, QString("App demo"));
        QCOMPARE(s.hello.value("audio_settings").toObject().value("device").toString(), QString("none"));
        QCOMPARE(s.status.outputs.size(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(s.tuner.valid, 3000);

        s.select(3);
        QTRY_COMPARE_WITH_TIMEOUT(s.status.selected, 3, 3000);
        s.run("Effect 2 = on");
        QTRY_COMPARE_WITH_TIMEOUT(s.status.control("Effect 2")->value, 1.0, 3000);
        // Djent's Delay is enabled by Effect 2... in Djent; Slap's Chorus by Effect 1.
        QJsonObject plugin;
        link.request({{"cmd", "plugin"}, {"chain", 1}, {"slot", 0}}, [&](const QJsonObject& r) { plugin = r; });
        QTRY_VERIFY_WITH_TIMEOUT(!plugin.isEmpty(), 3000);
        QVERIFY(plugin.value("ok").toBool());
        const QJsonArray params = plugin.value("plugin").toObject().value("params").toArray();
        QCOMPARE(params.size(), 1);
        QCOMPARE(params[0].toObject().value("name").toString(), QString("Gain"));
        // The plug-in itself takes the value, on the chain playing (3) and on one
        // that is not (1): read back, it stays where it was set.
        for (int chain : {3, 1}) {
            QJsonObject set;
            link.request({{"cmd", "param"}, {"chain", chain}, {"slot", 0}, {"param", 0}, {"value", 0.5}}, [&](const QJsonObject& r) { set = r; });
            QTRY_VERIFY_WITH_TIMEOUT(!set.isEmpty(), 3000);
            QCOMPARE(set.value("text").toString(), QString("0.500"));
            double now = -1;
            for (int tries = 0; tries < 30 && now != 0.5; ++tries) {
                QJsonObject r;
                link.request({{"cmd", "plugin"}, {"chain", chain}, {"slot", 0}}, [&](const QJsonObject& reply) { r = reply; });
                QTRY_VERIFY_WITH_TIMEOUT(!r.isEmpty(), 3000);
                now = r.value("plugin").toObject().value("params").toArray().at(0).toObject().value("value").toDouble();
                if (now != 0.5)
                    QTest::qWait(100);
            }
            QCOMPARE(now, 0.5);
        }
        // The picked parameters are kept, and come back with the plug-in.
        QJsonObject picked;
        link.request({{"cmd", "shown"}, {"chain", 1}, {"slot", 0}, {"params", QJsonArray{0}}}, [&](const QJsonObject& r) { picked = r; });
        QTRY_VERIFY_WITH_TIMEOUT(!picked.isEmpty(), 3000);
        QCOMPARE(picked.value("plugin").toObject().value("shown").toArray(), QJsonArray{0});
        link.request({{"cmd", "bypass"}, {"chain", 1}, {"slot", 2}, {"bypassed", true}});
        QTRY_VERIFY_WITH_TIMEOUT(s.status.bypassed(1, 2), 3000);

        // The engine stops: the app says so and waits; it starts again: the app is back.
        link.request({{"cmd", "quit"}});
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), QProcess::NotRunning, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!s.connected(), 3000);
        QProcess again;
        startEngine(again);
        QTRY_VERIFY_WITH_TIMEOUT(s.connected() && s.status.valid, 10000);
        QCOMPARE(s.perf.chains.size(), 5);
        link.request({{"cmd", "quit"}});
        QTRY_COMPARE_WITH_TIMEOUT(again.state(), QProcess::NotRunning, 5000);
    }
};

QTEST_MAIN(AppTests)
#include "test_app.moc"
