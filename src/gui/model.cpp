#include "gui/model.h"

#include <QFileInfo>
#include <QJsonArray>

#include <cmath>

namespace cue::gui {

namespace {

QStringList strings(const QJsonValue& v) {
    QStringList out;
    for (const QJsonValue& s : v.toArray())
        out << s.toString();
    return out;
}

// What a block is called without a label: the plug-in's file, or the
// built-in's name.
QString pluginName(const QJsonObject& p) {
    if (p.value("format").toString() == "builtin") {
        const QJsonArray args = p.value("args").toArray();
        return args.isEmpty() ? "builtin" : args.first().toString();
    }
    QString name = QFileInfo(p.value("path").toString()).completeBaseName();
    const QString id = p.value("id").toString();
    if (p.value("format").toString() == "vst3" && !id.isEmpty() && id.size() != 32)
        name = id; // an effect name inside a shell bundle; a class ID says less than the file
    return name;
}

} // namespace

int ChainInfo::numPlugins() const {
    int n = 0;
    for (const LineInfo& l : lines)
        n += l.plugins.size();
    return n;
}

const PluginInfo* ChainInfo::plugin(int slot) const {
    for (const LineInfo& l : lines)
        for (const PluginInfo& p : l.plugins)
            if (p.slot == slot)
                return &p;
    return nullptr;
}

Performance Performance::fromJson(const QJsonObject& j) {
    Performance perf;
    perf.valid = true;
    perf.name = j.value("name").toString();
    perf.inputs = strings(j.value("inputs"));
    perf.outputs = strings(j.value("outputs"));
    if (perf.inputs.isEmpty())
        perf.inputs << "In 1";
    perf.tunerInput = j.value("tuner").toObject().value("input").toString(perf.inputs.first());
    int index = 0;
    const QJsonArray scenes = j.value("scenes").toArray();
    for (int s = 0; s < scenes.size(); ++s) {
        const QJsonObject sj = scenes[s].toObject();
        SceneInfo scene;
        scene.name = sj.value("name").toString();
        for (const QJsonValue& cv : sj.value("chains").toArray()) {
            const QJsonObject cj = cv.toObject();
            ChainInfo c;
            c.index = index++;
            c.scene = s;
            c.name = cj.value("name").toString();
            c.display = cj.value("display").toString();
            c.warm = cj.value("warm").toString("next");
            c.input = cj.value("input").toString(perf.inputs.first());
            c.pedal = cj.value("pedal").toInt();
            int slot = 0;
            for (const QJsonValue& lv : cj.value("lines").toArray()) {
                const QJsonObject lj = lv.toObject();
                LineInfo line;
                line.name = lj.value("name").toString();
                line.from = lj.value("from").toString();
                line.after = lj.value("after").toInt();
                if (!c.lines.isEmpty()) {
                    line.parent = 0;
                    for (int k = 0; k < c.lines.size(); ++k)
                        if (c.lines[k].name == line.from)
                            line.parent = k;
                }
                for (const QJsonValue& pv : lj.value("plugins").toArray()) {
                    const QJsonObject pj = pv.toObject();
                    PluginInfo p;
                    p.slot = slot++;
                    p.format = pj.value("format").toString();
                    p.path = pj.value("path").toString();
                    p.id = pj.value("id").toString();
                    p.label = pj.value("label").toString(pluginName(pj));
                    p.bypassed = pj.value("bypassed").toBool();
                    p.muteOnBypass = pj.value("bypass_mode").toString() == "mute";
                    line.plugins << p;
                }
                c.lines << line;
            }
            if (c.lines.isEmpty())
                c.lines << LineInfo{};
            // Every output: where the file says, else the end of the main line.
            const QJsonObject outs = cj.value("outputs").toObject();
            for (const QString& o : perf.outputs) {
                TapInfo t;
                t.output = o;
                t.after = c.lines[0].plugins.size();
                if (outs.contains(o)) {
                    const QJsonObject tj = outs.value(o).toObject();
                    const QString line = tj.value("line").toString();
                    for (int k = 0; k < c.lines.size(); ++k)
                        if (c.lines[k].name == line)
                            t.line = k;
                    t.after = std::min<int>(tj.value("after").toInt(), c.lines[t.line].plugins.size());
                }
                c.taps << t;
            }
            scene.chains << c.index;
            perf.chains << c;
        }
        perf.scenes << scene;
    }
    for (const QJsonValue& v : j.value("controls").toArray()) {
        const QJsonObject cj = v.toObject();
        ControlInfo c;
        c.name = cj.value("name").toString();
        c.type = cj.value("type").toString("discrete");
        c.values = strings(cj.value("values"));
        if (c.type == "toggle" && c.values.isEmpty())
            c.values = {"off", "on"};
        c.min = cj.value("min").toDouble(0);
        c.max = cj.value("max").toDouble(c.type == "continuous" ? 1 : std::max<int>(0, c.values.size() - 1));
        c.pedal = cj.value("pedal").toInt();
        c.mutes = !cj.value("mute").toArray().isEmpty();
        perf.controls << c;
    }
    return perf;
}

const ControlInfo* Performance::control(const QString& name) const {
    for (const ControlInfo& c : controls)
        if (c.name.compare(name, Qt::CaseInsensitive) == 0)
            return &c;
    return nullptr;
}

Status Status::fromJson(const QJsonObject& j) {
    Status s;
    s.valid = true;
    const QJsonObject dsp = j.value("dsp").toObject();
    s.dspAvg = dsp.value("avg").toDouble();
    s.dspMax = dsp.value("max").toDouble();
    if (j.value("margin_ms").isDouble())
        s.marginMs = j.value("margin_ms").toDouble();
    s.xruns = j.value("xruns").toInteger();
    s.periodMs = j.value("period_ms").toDouble();
    s.bufferMs = j.value("buffer_ms").toDouble();
    s.scene = j.value("scene").toObject().value("index").toInt();
    s.selected = j.value("selected").toObject().value("index").toInt(-1);
    s.selectedName = j.value("selected").toObject().value("name").toString();
    s.muted = j.value("muted").toBool();
    s.display = j.value("display").toString();
    for (const QJsonValue& v : j.value("leds").toArray())
        s.leds << v.toBool();
    for (const QJsonValue& v : j.value("controls").toArray()) {
        const QJsonObject c = v.toObject();
        s.controls << ControlStatus{c.value("name").toString(), c.value("text").toString(), c.value("value").toDouble()};
    }
    for (const QJsonValue& v : j.value("chains").toArray()) {
        const QJsonObject c = v.toObject();
        ChainStatus cs{c.value("name").toString(), c.value("state").toString(), c.value("avg_us").toDouble(), c.value("max_us").toDouble(), {}};
        for (const QJsonValue& b : c.value("bypassed").toArray())
            cs.bypassed << b.toInt();
        s.chains << cs;
    }
    const QJsonObject levels = j.value("levels").toObject();
    for (const QJsonValue& v : levels.value("inputs").toArray())
        s.inputs << Level{v.toObject().value("name").toString(), v.toObject().value("peak_db").toDouble(-120)};
    for (const QJsonValue& v : levels.value("outputs").toArray())
        s.outputs << Level{v.toObject().value("name").toString(), v.toObject().value("peak_db").toDouble(-120)};
    return s;
}

int Status::warmChain() const {
    for (int i = 0; i < chains.size(); ++i)
        if (chains[i].state == "warm")
            return i;
    return -1;
}

const ControlStatus* Status::control(const QString& name) const {
    for (const ControlStatus& c : controls)
        if (c.name.compare(name, Qt::CaseInsensitive) == 0)
            return &c;
    return nullptr;
}

bool Status::bypassed(int chain, int slot) const {
    return chain >= 0 && chain < chains.size() && chains[chain].bypassed.contains(slot);
}

std::optional<double> Status::marginPercent() const {
    const double room = bufferMs - periodMs;
    if (!marginMs || room <= 0)
        return std::nullopt;
    return 100.0 * *marginMs / room;
}

TunerReading TunerReading::fromJson(const QJsonObject& j) {
    TunerReading t;
    t.valid = true;
    t.signal = j.value("signal").toBool();
    t.held = j.value("held").toBool(true);
    t.inTune = j.value("in_tune").toBool();
    t.note = j.value("note").toString();
    t.octave = j.value("octave").toInt(4);
    t.midi = j.value("midi").toInt(69);
    t.cents = j.value("cents").toDouble();
    t.hz = j.value("hz").toDouble();
    t.levelDb = j.value("level_db").toDouble(-200);
    t.reference = j.value("reference").toDouble(440);
    t.tolerance = j.value("tolerance").toDouble(2);
    t.range = j.value("range").toString();
    t.input = j.value("input").toString();
    t.names = j.value("names").toString();
    return t;
}

QString TunerReading::string() const {
    if (note.isEmpty())
        return {};
    // Highest string first: string 1 is the thinnest.
    static const int bass[] = {43, 38, 33, 28, 23};      // G D A E B
    static const int guitar[] = {64, 59, 55, 50, 45, 40}; // E B G D A E
    const int* strings = range == "guitar" ? guitar : range == "bass" ? bass : nullptr;
    const int n = range == "guitar" ? 6 : 5;
    if (!strings)
        return {};
    for (int i = 0; i < n; ++i)
        if (strings[i] == midi)
            return QString("string %1").arg(i + 1);
    return {};
}

QString audioSummary(const QJsonObject& hello) {
    const QJsonObject a = hello.value("audio_settings").toObject();
    if (a.isEmpty())
        return {};
    QString device = a.value("device").toString();
    if (device == "none")
        device = "no sound card";
    else if (device.startsWith("hw:") || device.startsWith("plughw:"))
        device = device.section(':', 1).section(',', 0, 0);
    const int rate = a.value("rate").toInt();
    const QString khz = rate % 1000 == 0 ? QString::number(rate / 1000) : QString::number(rate / 1000.0, 'f', 1);
    return QString("%1 · %2k · %3×%4").arg(device, khz).arg(a.value("period").toInt()).arg(a.value("periods").toInt());
}

QString controllerName(const QJsonObject& hello) {
    const QJsonObject m = hello.value("midi").toObject();
    if (!m.value("enabled").toBool())
        return {};
    if (m.value("profile").toString() == "eurekaprom")
        return "FCB1010";
    return m.value("in").toString();
}

} // namespace cue::gui
