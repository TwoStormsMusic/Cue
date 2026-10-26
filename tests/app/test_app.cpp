// The app's tests: what it reads from the engine, its line to the engine,
// every view at every screen size, what taps ask for, and the app against a
// real `cue run --audio none`. Run offscreen: QT_QPA_PLATFORM=offscreen.

#include "gui/board.h"
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
#include <QPainterPath>
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
  "cue": 2, "name": "App demo", "inputs": ["Bass"], "outputs": ["FOH", "Amp"],
  "tuner": {"input": "Bass", "reference": 440, "range": "bass", "tolerance": 2, "names": "sharps"},
  "scenes": [{"name": "Cue Template", "chains": [
    {"name": "Punk", "display": "PU", "pedal": 6, "warm": "next", "blocks": [
       {"format": "clap", "path": "~/x/Test.clap", "id": "cue.test.gain", "label": "Gain Stage", "bypassed": false, "bypass_mode": "thru"},
       {"format": "builtin", "args": ["drive", "3"], "label": "Darkglass Ultra", "bypassed": false, "bypass_mode": "thru"}],
     "wires": [{"from": "Bass", "to": 0}, {"from": 0, "to": 1}, {"from": 1, "to": "FOH"}, {"from": 1, "to": "Amp"}]},
    {"name": "Slap", "display": "SL", "pedal": 7, "warm": "next", "blocks": [
       {"format": "clap", "path": "~/x/Test.clap", "id": "cue.test.gain", "label": "Gain Stage", "bypassed": false, "bypass_mode": "thru"},
       {"format": "builtin", "args": ["lowpass", "2500"], "label": "Wah", "bypassed": true, "bypass_mode": "thru"},
       {"format": "builtin", "args": ["gain", "1"], "label": "Act", "bypassed": false, "bypass_mode": "thru"},
       {"format": "builtin", "args": ["lowpass", "6000"], "label": "IR Loader", "bypassed": false, "bypass_mode": "mute"},
       {"format": "clap", "path": "~/x/Test.clap", "id": "cue.test.delay", "label": "Chorus", "bypassed": true, "bypass_mode": "thru", "has_key": true}],
     "wires": [{"from": "Bass", "to": 0}, {"from": 0, "to": 1}, {"from": 1, "to": 2}, {"from": 2, "to": 3}, {"from": 3, "to": 4},
               {"from": 4, "to": "FOH"}, {"from": 2, "to": "Amp", "level_db": -3, "width": 50, "pan": -30}]},
    {"name": "2000s", "pedal": 8, "warm": "next", "blocks": [], "wires": [{"from": "Bass", "to": "FOH"}, {"from": "Bass", "to": "Amp"}]},
    {"name": "Motown", "pedal": 9, "warm": "next", "blocks": [], "wires": [{"from": "Bass", "to": "FOH"}, {"from": "Bass", "to": "Amp"}]},
    {"name": "Djent", "pedal": 10, "warm": "next", "blocks": [], "wires": [{"from": "Bass", "to": "FOH"}, {"from": "Bass", "to": "Amp"}]}]}],
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

    void performance_reads_chains_blocks_and_wires() {
        const Performance p = Performance::fromJson(parse(kPerformance));
        QCOMPARE(p.name, QString("App demo"));
        QCOMPARE(p.chains.size(), 5);
        QCOMPARE(p.scenes[0].chains, (QVector<int>{0, 1, 2, 3, 4}));
        const ChainInfo& slap = p.chains[1];
        QCOMPARE(slap.numPlugins(), 5);
        QCOMPARE(slap.plugin(3)->label, QString("IR Loader"));
        QVERIFY(slap.plugin(3)->muteOnBypass);
        QVERIFY(slap.plugin(4)->hasKey);
        QVERIFY(!slap.plugin(3)->hasKey);
        QCOMPARE(slap.plugin(1)->args, (QStringList{"lowpass", "2500"}));
        QCOMPARE(slap.wires.size(), 7);
        QCOMPARE(slap.wires[0].from, PortInfo::input());
        const int amp = slap.wire(PortInfo::block(2), PortInfo::output(1));
        QVERIFY(amp >= 0);
        QCOMPARE(slap.wires[amp].levelDb, -3.0);
        QCOMPARE(slap.wires[amp].width, 50.0);
        QCOMPARE(slap.wires[amp].pan, -30.0);
        QVERIFY(!slap.wires[amp].neutral());
        QVERIFY(slap.wires[0].neutral());
        QCOMPARE(slap.outputsUsed(), (QVector<int>{0, 1}));
        // A request names the ends as the engine does.
        const QJsonObject r = slap.wires[amp].request(1, p.outputs);
        QCOMPARE(r.value("from").toInt(), 2);
        QCOMPARE(r.value("to").toString(), QString("Amp"));
        QCOMPARE(r.value("pan").toDouble(), -30.0);
        // A chain with no plug-ins: its input wired straight to the outputs.
        QCOMPARE(p.chains[2].wires.size(), 2);
        QCOMPARE(p.chains[2].wires[1].to, PortInfo::output(1));
        QCOMPARE(p.control("wah")->values.size(), 3);
        QCOMPARE(p.control("Effect 1")->values, (QStringList{"off", "on"}));
        QVERIFY(p.control("Tuner")->mutes);
        QCOMPARE(p.chains[0].plugin(1)->label, QString("Darkglass Ultra"));
    }

    // Green: sound from the input would pass on its way to an output, if
    // the chain played.
    void a_wire_is_green_where_sound_would_pass() {
        const Performance p = Performance::fromJson(parse(kPerformance));
        const ChainInfo& slap = p.chains[1];
        auto w = [&slap](PortInfo a, PortInfo b) { return slap.wire(a, b); };
        const auto none = [](int) { return false; };
        Flow f = flowOf(slap, none, false, 2);
        QCOMPARE(f.wires, QVector<bool>(7, true));
        QCOMPARE(f.played, QVector<bool>(5, true));
        // The IR Loader bypassed to silence: nothing after it carries sound,
        // and the wire into it leads nowhere. It is still processed.
        f = flowOf(slap, [](int s) { return s == 3; }, false, 2);
        QVERIFY(!f.wires[w(PortInfo::block(2), PortInfo::block(3))]);
        QVERIFY(!f.wires[w(PortInfo::block(3), PortInfo::block(4))]);
        QVERIFY(!f.wires[w(PortInfo::block(4), PortInfo::output(0))]);
        QVERIFY(f.wires[w(PortInfo::block(2), PortInfo::output(1))]);
        QVERIFY(f.wires[w(PortInfo::block(1), PortInfo::block(2))]);
        QVERIFY(f.played[3]);
        QVERIFY(!f.heard[0]);
        QVERIFY(f.heard[1]);
        // Bypassed to pass its input on, the Wah keeps its wires green.
        f = flowOf(slap, [](int s) { return s == 1; }, false, 2);
        QCOMPARE(f.wires, QVector<bool>(7, true));
        // The inputs muted: all grey.
        f = flowOf(slap, none, true, 2);
        QCOMPARE(f.wires, QVector<bool>(7, false));
        // No way on to an output: grey, and not processed.
        ChainInfo cut = slap;
        cut.wires.remove(cut.wire(PortInfo::block(4), PortInfo::output(0)));
        f = flowOf(cut, none, false, 2);
        QVERIFY(!f.played[4]);
        QVERIFY(!f.played[3]);
        QVERIFY(!f.wires[cut.wire(PortInfo::block(2), PortInfo::block(3))]);
        QVERIFY(f.wires[cut.wire(PortInfo::block(2), PortInfo::output(1))]);
        QVERIFY(!f.heard[0]);
        // A muted wire carries nothing; one into a key carries the sound
        // the plug-in listens to.
        ChainInfo muted = slap;
        muted.wires[muted.wire(PortInfo::block(2), PortInfo::output(1))].muted = true;
        muted.wires.append(WireInfo{PortInfo::block(0), PortInfo::block(4, true)});
        f = flowOf(muted, none, false, 2);
        QVERIFY(!f.wires[muted.wire(PortInfo::block(2), PortInfo::output(1))]);
        QVERIFY(!f.heard[1]);
        QVERIFY(f.wires.last());
    }

    // A chain laid out from its wires: a row for each way it splits into,
    // the input left of it all, the outputs right of it beside what feeds
    // them. What was put somewhere stays there.
    void the_board_lays_chains_out_by_their_wires() {
        const Performance p = Performance::fromJson(parse(kPerformance));
        const ChainInfo& slap = p.chains[1];
        const Board b(slap, 2);
        QCOMPARE(b.block[1].y(), b.block[0].y());
        QCOMPARE(b.block[2].y(), b.block[0].y());
        QVERIFY(b.block[1].x() > b.block[0].x());
        // After Act, which feeds the Amp, the branch to FOH has a row below.
        QVERIFY(b.block[3].y() > b.block[2].y());
        QCOMPARE(b.block[4].y(), b.block[3].y());
        QVERIFY(b.block[4].x() > b.block[3].x());
        QVERIFY(b.input.x() < b.block[0].x());
        QCOMPARE(b.input.y(), b.block[0].y());
        QCOMPARE(b.output[0].y(), b.block[4].y());
        QCOMPARE(b.output[1].y(), b.block[2].y());
        QVERIFY(b.output[0].x() > b.block[4].x());
        for (int i = 0; i < 5; ++i)
            QVERIFY(b.bounds.contains(b.box(i)));
        // The wire to the Amp passes nothing on its row, so it is straight;
        // the one back to the branch's start goes round.
        const QPainterPath amp = b.path(slap.wires[slap.wire(PortInfo::block(2), PortInfo::output(1))]);
        QCOMPARE(amp.pointAtPercent(0.5).y(), b.block[2].y());
        const QPainterPath down = b.path(slap.wires[slap.wire(PortInfo::block(2), PortInfo::block(3))]);
        QVERIFY(down.boundingRect().right() > b.box(2).right());
        // A key is low on the left.
        QVERIFY(b.inPoint(PortInfo::block(4, true)).y() > b.inPoint(PortInfo::block(4)).y());
        // Placed, it stays where it was put; dragged, it is where the finger is.
        ChainInfo moved = slap;
        moved.blocks[3].placed = true;
        moved.blocks[3].at = QPointF(900, -200);
        const Board m(moved, 2, {{4, QPointF(1000, -200)}});
        QCOMPARE(m.block[3], QPointF(900, -200));
        QCOMPARE(m.block[4], QPointF(1000, -200));
        QCOMPARE(m.laidOut[3], b.block[3]);
        QVERIFY(m.output[0].x() > 1000);
        QVERIFY(m.bounds.contains(m.box(3)));
        // A free place for a new one is not on top of another.
        const QPointF free = b.freeNear(b.block[1]);
        for (int i = 0; i < 5; ++i)
            QVERIFY(!b.box(i).contains(free));
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

    // The routing board, tap by tap: a wire selected and its faders moved,
    // a plug-in put on it, a wire drawn and one taken away, a plug-in moved
    // and one taken out; each undone and done again.
    void the_routing_board_edits_wires_and_plugins() {
        FakeLink link;
        link.answer = [](const QJsonObject& r) -> QJsonObject {
            if (r.value("cmd") == "add")
                return {{"ok", true}, {"slot", 5}};
            return {{"ok", true}};
        };
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
        QCOMPARE(v->shownChain(), 1);
        const ChainInfo slap = *s.chain(1);
        const Board b(slap, 2);
        auto tapAt = [v](QPointF board) {
            v->grab();
            QTest::mouseClick(v, Qt::LeftButton, {}, v->toScreen(board).toPoint());
        };
        auto drag = [v](QPointF from, QPointF to) {
            QTest::mousePress(v, Qt::LeftButton, {}, from.toPoint());
            QTest::mouseMove(v, ((from + to) / 2).toPoint());
            QTest::mouseMove(v, to.toPoint());
            QTest::mouseRelease(v, Qt::LeftButton, {}, to.toPoint());
        };

        // A tap on a wire selects it; its level fader moves it.
        const int gw = slap.wire(PortInfo::block(0), PortInfo::block(1));
        tapAt(b.path(slap.wires[gw]).pointAtPercent(0.5));
        QCOMPARE(v->selectedWire(), gw);
        QCOMPARE(v->selectedSlot(), -1);
        v->grab();
        const QRectF level = v->hitRect("wire level");
        QVERIFY(!level.isEmpty());
        QVERIFY(!v->hitRect("wire width").isEmpty());
        QVERIFY(!v->hitRect("wire pan").isEmpty());
        QVERIFY(!v->hitRect("insert").isEmpty());
        drag(level.center(), QPointF(level.right() - 2, level.center().y()));
        QJsonObject wire = link.last("wire");
        QCOMPARE(wire.value("chain").toInt(), 1);
        QCOMPARE(wire.value("from").toInt(), 0);
        QCOMPARE(wire.value("to").toInt(), 1);
        QVERIFY(wire.value("level_db").toDouble() > 10);
        shot(v, "routing-wire");
        QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(link.last("wire").value("level_db").toDouble(), 0.0);
        QTest::keyClick(&w, Qt::Key_Y, Qt::ControlModifier);
        QVERIFY(link.last("wire").value("level_db").toDouble() > 10);
        // Pan from the middle; mute keeps the level.
        v->grab();
        const QRectF pan = v->hitRect("wire pan");
        drag(pan.center(), QPointF(pan.left() + 2, pan.center().y()));
        QCOMPARE(link.last("wire").value("pan").toDouble(), -100.0);
        v->grab();
        click(v, v->hitRect("wire mute"));
        QVERIFY(link.last("wire").value("muted").toBool());

        // Its + puts a plug-in on it: the built-in Gain, after the two this
        // performance has. The first change to the routing keeps every
        // plug-in where it is drawn; the new one goes where the Wah was, and
        // the rest of that row moves along.
        QCOMPARE(count("place"), 0);
        v->grab();
        click(v, v->hitRect("insert"));
        v->grab();
        shot(v, "routing-insert");
        QVERIFY(!v->hitRect("choice 0").isEmpty());
        click(v, v->hitRect("choice 2"));
        const QJsonObject add = link.last("add");
        QCOMPARE(add.value("chain").toInt(), 1);
        QCOMPARE(add.value("plugin").toObject().value("format").toString(), QString("builtin"));
        QCOMPARE(add.value("plugin").toObject().value("args").toArray().first().toString(), QString("gain"));
        QCOMPARE(add.value("label").toString(), QString("Gain"));
        QCOMPARE(add.value("on").toObject().value("from").toInt(), 0);
        QCOMPARE(add.value("on").toObject().value("to").toInt(), 1);
        QCOMPARE(QPointF(add.value("x").toDouble(), add.value("y").toDouble()), b.block[1]);
        QCOMPARE(count("place"), 5 + 2);
        QVERIFY(s.chain(1)->blocks[0].placed);
        QCOMPARE(s.chain(1)->blocks[0].at, b.block[0]);
        QCOMPARE(s.chain(1)->blocks[1].at, b.block[1] + QPointF(Board::colPitch, 0));
        QCOMPARE(s.chain(1)->blocks[2].at, b.block[2] + QPointF(Board::colPitch, 0));
        QCOMPARE(s.chain(1)->blocks[3].at, b.block[3]);
        QCOMPARE(v->selectedSlot(), 5);
        // Undone, it is taken out and kept, to be put back when redone, and
        // what moved along goes back.
        QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
        const QJsonObject removed = link.last("remove");
        QCOMPARE(removed.value("slot").toInt(), 5);
        const QString token = removed.value("token").toString();
        QVERIFY(!token.isEmpty());
        QCOMPARE(s.chain(1)->blocks[1].at, b.block[1]);
        QCOMPARE(s.chain(1)->blocks[2].at, b.block[2]);
        QTest::keyClick(&w, Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(link.last("restore").value("token").toString(), token);
        QCOMPARE(s.chain(1)->blocks[2].at, b.block[2] + QPointF(Board::colPitch, 0));

        // A wire drawn from a plug-in's dot to an output.
        v->grab();
        drag(v->hitRect("port out 0").center(), v->hitRect("port output 1").center());
        wire = link.last("wire");
        QCOMPARE(wire.value("from").toInt(), 0);
        QCOMPARE(wire.value("to").toString(), QString("Amp"));
        QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(link.last("unwire").value("from").toInt(), 0);
        QCOMPARE(link.last("unwire").value("to").toString(), QString("Amp"));
        // Drawn backwards, from an input dot to where sound comes out, it is the same wire.
        v->grab();
        drag(v->hitRect("port key 4").center(), v->hitRect("port input").center());
        wire = link.last("wire");
        QCOMPARE(wire.value("from").toString(), QString("in"));
        QCOMPARE(wire.value("to").toInt(), 4);
        QVERIFY(wire.value("key").toBool());

        // Delete takes the selected wire away; undo puts it back as it was.
        const int aw = slap.wire(PortInfo::block(2), PortInfo::output(1));
        tapAt(Board(*s.chain(1), 2).path(slap.wires[aw]).pointAtPercent(0.5));
        QCOMPARE(v->selectedWire(), aw);
        shot(v, "routing-thick-wire");
        QTest::keyClick(&w, Qt::Key_Delete);
        QCOMPARE(link.last("unwire").value("from").toInt(), 2);
        QCOMPARE(link.last("unwire").value("to").toString(), QString("Amp"));
        QCOMPARE(v->selectedWire(), -1);
        QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
        wire = link.last("wire");
        QCOMPARE(wire.value("from").toInt(), 2);
        QCOMPARE(wire.value("level_db").toDouble(), -3.0);
        QCOMPARE(wire.value("width").toDouble(), 50.0);
        QCOMPARE(wire.value("pan").toDouble(), -30.0);

        // A plug-in dragged goes where it was let go; undo moves it back.
        v->grab();
        const QPointF chorus = v->hitRect("block 4").center();
        const int before = count("place");
        drag(chorus, chorus + QPointF(30, 90));
        QCOMPARE(count("place"), before + 1);
        const QJsonObject place = link.last("place");
        QCOMPARE(place.value("slot").toInt(), 4);
        QVERIFY(place.value("y").toDouble() > b.block[4].y() + 40);
        QVERIFY(s.chain(1)->blocks[4].at.y() > b.block[4].y() + 40);
        shot(v, "routing-moved");
        QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(link.last("place").value("y").toDouble(), b.block[4].y());
        QCOMPARE(s.chain(1)->blocks[4].at, b.block[4]);

        // A plug-in taken out with its button; undo puts it back.
        v->grab();
        click(v, v->hitRect("block 3"));
        QCOMPARE(v->selectedSlot(), 3);
        v->grab();
        click(v, v->hitRect("remove"));
        const QJsonObject out = link.last("remove");
        QCOMPARE(out.value("slot").toInt(), 3);
        QCOMPARE(v->selectedSlot(), -1);
        QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(link.last("restore").value("token").toString(), out.value("token").toString());
        QCOMPARE(v->selectedSlot(), 3);

        // + Plug-in adds one to the board, unwired, in a free place.
        v->grab();
        click(v, v->hitRect("add"));
        v->grab();
        click(v, v->hitRect("choice 0"));
        const QJsonObject free = link.last("add");
        QCOMPARE(free.value("plugin").toObject().value("id").toString(), QString("cue.test.gain"));
        QCOMPARE(free.value("label").toString(), QString("Gain Stage 2"));
        QVERIFY(!free.contains("on"));
        QVERIFY(free.contains("x") && free.contains("y"));
        // Esc lets go of what is selected before it leaves the view.
        QTest::keyClick(&w, Qt::Key_Escape);
        QCOMPARE(v->selectedSlot(), -1);
        QVERIFY(v->isVisible());
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
        click(v, v->hitRect("Show modified wire parameters: Off"));
        v->grab();
        click(v, v->hitRect("Range: Guitar"));
        QCOMPARE(link.last("tuner").value("range").toString(), QString("guitar"));
        Settings again;
        again.load();
        QCOMPARE(again.startView, QString("tuner"));
        QVERIFY(again.percent);
        QVERIFY(!again.wireTags);
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

        // The routing, live: a plug-in put on a wire while the chain plays,
        // a wire's gains, the plug-in taken out and put back, a loop refused.
        auto ask = [&](const QJsonObject& request) {
            QJsonObject reply;
            s.change(request, [&reply](const QJsonObject& r) { reply = r; });
            [&] { QTRY_VERIFY_WITH_TIMEOUT(!reply.isEmpty(), 5000); }();
            return reply;
        };
        s.select(1);
        QTRY_COMPARE_WITH_TIMEOUT(s.status.selected, 1, 3000);
        const int had = s.perf.chains[1].numPlugins();
        QJsonObject r = ask({{"cmd", "add"}, {"chain", 1}, {"plugin", QJsonObject{{"format", "builtin"}, {"args", QJsonArray{"gain", "0.5"}}}},
                             {"label", "Trim"}, {"on", QJsonObject{{"from", 0}, {"to", 1}}}});
        QVERIFY2(r.value("ok").toBool(), qPrintable(r.value("error").toString()));
        const int trim = r.value("slot").toInt();
        QCOMPARE(trim, had);
        QCOMPARE(s.perf.chains[1].numPlugins(), had + 1);
        QCOMPARE(s.perf.chains[1].plugin(trim)->label, QString("Trim"));
        QVERIFY(s.perf.chains[1].wire(PortInfo::block(0), PortInfo::block(trim)) >= 0);
        QVERIFY(s.perf.chains[1].wire(PortInfo::block(trim), PortInfo::block(1)) >= 0);
        QVERIFY(s.perf.chains[1].wire(PortInfo::block(0), PortInfo::block(1)) < 0);
        r = ask({{"cmd", "wire"}, {"chain", 1}, {"from", trim}, {"to", 1}, {"level_db", -6}, {"pan", -50}});
        QVERIFY(r.value("ok").toBool());
        QCOMPARE(r.value("wire").toObject().value("level_db").toDouble(), -6.0);
        const ChainInfo& live = s.perf.chains[1];
        QCOMPARE(live.wires[live.wire(PortInfo::block(trim), PortInfo::block(1))].pan, -50.0);
        r = ask({{"cmd", "remove"}, {"chain", 1}, {"slot", trim}, {"token", "test"}});
        QVERIFY2(r.value("ok").toBool(), qPrintable(r.value("error").toString()));
        QCOMPARE(s.perf.chains[1].numPlugins(), had);
        QVERIFY(s.perf.chains[1].wire(PortInfo::block(0), PortInfo::block(1)) >= 0);
        r = ask({{"cmd", "restore"}, {"token", "test"}});
        QVERIFY2(r.value("ok").toBool(), qPrintable(r.value("error").toString()));
        QCOMPARE(r.value("slot").toInt(), trim);
        QCOMPARE(s.perf.chains[1].numPlugins(), had + 1);
        QCOMPARE(s.perf.chains[1].wires[s.perf.chains[1].wire(PortInfo::block(trim), PortInfo::block(1))].pan, -50.0);
        r = ask({{"cmd", "wire"}, {"chain", 1}, {"from", 1}, {"to", 0}});
        QVERIFY(!r.value("ok").toBool());
        QVERIFY2(r.value("error").toString().contains("loop"), qPrintable(r.value("error").toString()));
        r = ask({{"cmd", "place"}, {"chain", 1}, {"slot", trim}, {"x", 500}, {"y", 300}});
        QVERIFY(r.value("ok").toBool());
        QCOMPARE(s.perf.chains[1].plugin(trim)->at, QPointF(500, 300));
        QVERIFY(s.status.valid);
        QCOMPARE(s.status.selected, 1);

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
