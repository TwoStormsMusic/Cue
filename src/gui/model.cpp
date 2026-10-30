#include "gui/model.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <utility>

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

QJsonValue PortInfo::json(const QStringList& outputs) const {
    switch (kind) {
    case Kind::Input: return "in";
    case Kind::Block: return index;
    case Kind::Output: break;
    }
    Q_UNUSED(outputs);
    return QString("out%1").arg(index + 1);
}

void WireInfo::readJump(const QJsonObject& wj) {
    jump = wj.value("jump").toBool();
    auto point = [&](const char* key, QPointF def) {
        const QJsonArray a = wj.value(key).toArray();
        return a.size() == 2 ? QPointF(a[0].toDouble(), a[1].toDouble()) : def;
    };
    jumpOut = point("jump_out", {34, 0});
    jumpIn = point("jump_in", {-34, 0});
}

QJsonObject WireInfo::request(int chain, const QStringList& outputs, const QString& cmd) const {
    QJsonObject r{{"cmd", cmd}, {"chain", chain}, {"from", from.json(outputs)}, {"to", to.json(outputs)}};
    if (to.key)
        r["key"] = true;
    if (cmd == "wire") {
        r["level_db"] = levelDb;
        r["width"] = width;
        r["pan"] = pan;
        r["muted"] = muted;
        if (feedback)
            r["feedback"] = true;
        r["jump"] = jump;
        if (jump) {
            r["jump_out"] = QJsonArray{jumpOut.x(), jumpOut.y()};
            r["jump_in"] = QJsonArray{jumpIn.x(), jumpIn.y()};
        }
    }
    return r;
}

QJsonObject LooseInfo::json(const QStringList& outputs) const {
    QJsonObject r{{from ? "from" : "to", port.json(outputs)}, {"x", at.x()}, {"y", at.y()}};
    if (!from && port.key)
        r["key"] = true;
    if (levelDb != 0)
        r["level_db"] = levelDb;
    if (width != 100)
        r["width"] = width;
    if (pan != 0)
        r["pan"] = pan;
    if (muted)
        r["muted"] = true;
    if (feedback)
        r["feedback"] = true;
    return r;
}

WireInfo LooseInfo::plugged(const PortInfo& other) const {
    WireInfo w;
    w.from = from ? port : other;
    w.to = from ? other : port;
    w.levelDb = levelDb;
    w.width = width;
    w.pan = pan;
    w.muted = muted;
    w.feedback = feedback;
    return w;
}

LooseInfo LooseInfo::of(const WireInfo& w, bool keepFrom, QPointF at) {
    LooseInfo l;
    l.port = keepFrom ? w.from : w.to;
    l.from = keepFrom;
    l.at = at;
    l.levelDb = w.levelDb;
    l.width = w.width;
    l.pan = w.pan;
    l.muted = w.muted;
    l.feedback = w.feedback;
    return l;
}

const PluginInfo* ChainInfo::plugin(int slot) const { return slot >= 0 && slot < blocks.size() ? &blocks[slot] : nullptr; }

int ChainInfo::wire(const PortInfo& from, const PortInfo& to) const {
    for (int i = 0; i < wires.size(); ++i)
        if (wires[i].from == from && wires[i].to == to)
            return i;
    return -1;
}

QVector<int> ChainInfo::outputsUsed() const {
    QVector<int> out;
    for (const WireInfo& w : wires)
        if (w.to.kind == PortInfo::Kind::Output && !out.contains(w.to.index))
            out << w.to.index;
    std::sort(out.begin(), out.end());
    return out;
}

bool ChainInfo::wouldLoop(const PortInfo& from, const PortInfo& to) const {
    if (from.kind != PortInfo::Kind::Block || to.kind != PortInfo::Kind::Block)
        return false;
    // Where to's signal goes, by wires other than feedback ones.
    QVector<bool> reached(blocks.size(), false);
    QVector<int> todo{to.index};
    while (!todo.isEmpty()) {
        const int b = todo.takeLast();
        if (b == from.index)
            return true;
        if (b < 0 || b >= reached.size() || reached[b])
            continue;
        reached[b] = true;
        for (const WireInfo& w : wires)
            if (!w.feedback && w.from.kind == PortInfo::Kind::Block && w.from.index == b && w.to.kind == PortInfo::Kind::Block)
                todo << w.to.index;
    }
    return false;
}

Flow flowOf(const ChainInfo& c, const std::function<bool(int)>& bypassed, bool inputsMuted, int numOutputs) {
    const int n = c.blocks.size(), m = c.wires.size();
    Flow f;
    f.wires.fill(false, m);
    f.played.fill(false, n);
    f.heard.fill(false, numOutputs);
    auto isBlock = [n](const PortInfo& p) { return p.kind == PortInfo::Kind::Block && p.index >= 0 && p.index < n; };
    auto silences = [&](int slot) { return c.blocks[slot].muteOnBypass && bypassed && bypassed(slot); };
    auto passes = [](const WireInfo& w) { return !w.muted && w.levelDb > -96; };
    // What a pass finds stays found, so n passes settle it, loops or not.
    // Fed: the input reaches its main input (whatever the gains); sounding:
    // sound comes out of it.
    QVector<bool> fed(n, false), sounding(n, false);
    for (int pass = 0; pass <= n; ++pass)
        for (const WireInfo& w : c.wires) {
            if (!isBlock(w.to) || w.to.key)
                continue;
            const bool from = w.from.kind == PortInfo::Kind::Input || (isBlock(w.from) && fed[w.from.index]);
            const bool sound = w.from.kind == PortInfo::Kind::Input ? !inputsMuted : isBlock(w.from) && sounding[w.from.index];
            if (from)
                fed[w.to.index] = true;
            if (sound && passes(w) && !silences(w.to.index))
                sounding[w.to.index] = true;
        }
    // Heard: an output hears it, through wires or a key (whatever the
    // gains); leads on: its sound goes on to an output.
    QVector<bool> heard(n, false), leads(n, false);
    for (int pass = 0; pass <= n; ++pass)
        for (const WireInfo& w : c.wires) {
            if (!isBlock(w.from))
                continue;
            const bool out = w.to.kind == PortInfo::Kind::Output;
            if (out || (isBlock(w.to) && heard[w.to.index]))
                heard[w.from.index] = true;
            if (passes(w) && (out || (isBlock(w.to) && leads[w.to.index] && !silences(w.to.index))))
                leads[w.from.index] = true;
        }
    for (int b = 0; b < n; ++b)
        f.played[b] = fed[b] && heard[b];
    for (int i = 0; i < m; ++i) {
        const WireInfo& w = c.wires[i];
        const bool sound = w.from.kind == PortInfo::Kind::Input ? !inputsMuted : isBlock(w.from) && sounding[w.from.index];
        const bool on = w.to.kind == PortInfo::Kind::Output || (isBlock(w.to) && leads[w.to.index] && !silences(w.to.index));
        f.wires[i] = sound && passes(w) && on && (w.from.kind == PortInfo::Kind::Input || f.played[w.from.index]);
        if (f.wires[i] && w.to.kind == PortInfo::Kind::Output && w.to.index >= 0 && w.to.index < numOutputs)
            f.heard[w.to.index] = true;
    }
    return f;
}

AutoInfo AutoInfo::fromJson(const QJsonObject& j) {
    AutoInfo a;
    a.measure = j.value("measure").toString(a.measure);
    a.targetDb = j.value("target_db").toDouble(a.targetDb);
    a.seconds = j.value("seconds").toDouble(a.seconds);
    return a;
}

QJsonObject AutoInfo::json() const { return {{"measure", measure}, {"target_db", targetDb}, {"seconds", seconds}}; }

IoLevels IoLevels::fromJson(const QJsonObject& io) {
    IoLevels l;
    for (const QJsonValue& v : io.value("inputs").toArray()) {
        const QJsonObject j = v.toObject();
        InputLevel in;
        in.gainDb = j.value("gain_db").toDouble(0);
        in.autoSet = AutoInfo::fromJson(j.value("auto").toObject());
        l.inputs << in;
    }
    for (const QJsonValue& v : io.value("outputs").toArray()) {
        const QJsonObject j = v.toObject();
        OutputLevel out;
        out.levelDb = j.value("level_db").toDouble(0);
        out.muted = j.value("muted").toBool(false);
        const QJsonObject lim = j.value("limiter").toObject();
        out.limiter = lim.value("on").toBool(false);
        out.ceilingDb = lim.value("ceiling_db").toDouble(out.ceilingDb);
        out.releaseMs = lim.value("release_ms").toDouble(out.releaseMs);
        out.autoSet = AutoInfo::fromJson(j.value("auto").toObject());
        l.outputs << out;
    }
    const QJsonObject wa = io.value("wires").toObject().value("auto").toObject();
    l.wireAuto = AutoInfo::fromJson(wa);
    l.wireMatch = wa.value("match").toBool(true);
    return l;
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
    perf.levels = IoLevels::fromJson(j.value("io").toObject());
    perf.levels.inputs.resize(perf.inputs.size());
    perf.levels.outputs.resize(perf.outputs.size());
    int index = 0;
    const QJsonArray scenes = j.value("scenes").toArray();
    for (int s = 0; s < scenes.size(); ++s) {
        const QJsonObject sj = scenes[s].toObject();
        SceneInfo scene;
        scene.name = sj.value("name").toString();
        scene.display = sj.value("display").toString();
        scene.pedal = sj.value("pedal").toInt();
        scene.program = sj.value("program").toInt(-1);
        for (const QJsonValue& cv : sj.value("chains").toArray()) {
            const QJsonObject cj = cv.toObject();
            ChainInfo c;
            // A file's chains, before an engine numbers them, by their place.
            c.index = cj.value("index").toInt(index);
            ++index;
            c.scene = s;
            c.name = cj.value("name").toString();
            c.display = cj.value("display").toString();
            c.warm = cj.value("warm").toString("next");
            c.input = cj.value("input").toString(perf.inputs.first());
            c.pedal = cj.value("pedal").toInt();
            c.program = cj.value("program").toInt(-1);
            c.onSelect = strings(cj.value("on_select"));
            for (const QJsonValue& pv : cj.value("blocks").toArray()) {
                const QJsonObject pj = pv.toObject();
                PluginInfo p;
                p.slot = c.blocks.size();
                p.format = pj.value("format").toString();
                p.path = pj.value("path").toString();
                p.id = pj.value("id").toString();
                p.args = strings(pj.value("args"));
                p.label = pj.value("label").toString(pluginName(pj));
                p.bypassed = pj.value("bypassed").toBool();
                p.muteOnBypass = pj.value("bypass_mode").toString() == "mute";
                p.hasKey = pj.value("has_key").toBool();
                p.placed = pj.contains("x") || pj.contains("y");
                p.at = QPointF(pj.value("x").toDouble(), pj.value("y").toDouble());
                c.blocks << p;
            }
            // A wire's ends: a block by number, the input by name, an output by name.
            auto port = [&](const QJsonValue& v, bool from, bool key, PortInfo& out) {
                if (v.isDouble()) {
                    out = PortInfo::block(v.toInt(), !from && key);
                    return v.toInt() >= 0 && v.toInt() < c.blocks.size();
                }
                if (from) {
                    out = PortInfo::input();
                    return true;
                }
                const int o = perf.output(v.toString());
                out = PortInfo::output(o);
                return o >= 0;
            };
            for (const QJsonValue& wv : cj.value("wires").toArray()) {
                const QJsonObject wj = wv.toObject();
                WireInfo w;
                if (!port(wj.value("from"), true, false, w.from) || !port(wj.value("to"), false, wj.value("key").toBool(), w.to))
                    continue;
                w.levelDb = wj.value("level_db").toDouble(0);
                w.width = wj.value("width").toDouble(100);
                w.pan = wj.value("pan").toDouble(0);
                w.muted = wj.value("muted").toBool();
                w.feedback = wj.value("feedback").toBool();
                w.readJump(wj);
                c.wires << w;
            }
            for (const QJsonValue& lv : cj.value("loose").toArray()) {
                const QJsonObject lj = lv.toObject();
                LooseInfo l;
                l.from = lj.contains("from");
                if (!port(lj.value(l.from ? "from" : "to"), l.from, lj.value("key").toBool(), l.port))
                    continue;
                l.at = QPointF(lj.value("x").toDouble(), lj.value("y").toDouble());
                l.levelDb = lj.value("level_db").toDouble(0);
                l.width = lj.value("width").toDouble(100);
                l.pan = lj.value("pan").toDouble(0);
                l.muted = lj.value("muted").toBool();
                l.feedback = lj.value("feedback").toBool();
                c.loose << l;
            }
            const QJsonObject board = cj.value("board").toObject();
            auto place = [](const QJsonValue& v) { return QPointF(v.toObject().value("x").toDouble(), v.toObject().value("y").toDouble()); };
            if (board.contains("input"))
                c.inputAt = place(board.value("input"));
            c.outputsAt.resize(perf.outputs.size());
            const QJsonObject outs = board.value("outputs").toObject();
            for (auto o = outs.begin(); o != outs.end(); ++o)
                if (const int k = perf.output(o.key()); k >= 0)
                    c.outputsAt[k] = place(o.value());
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
    for (const QJsonValue& v : j.value("triggers").toArray())
        perf.triggers << TriggerInfo{v.toObject().value("name").toString(), v.toObject().value("scene").toString()};
    return perf;
}

const ChainInfo* Performance::chain(int index) const {
    for (const ChainInfo& c : chains)
        if (c.index == index)
            return &c;
    return nullptr;
}

ChainInfo* Performance::chain(int index) { return const_cast<ChainInfo*>(std::as_const(*this).chain(index)); }

int Performance::output(const QString& name) const {
    for (int o = 0; o < outputs.size(); ++o)
        if (outputs[o].compare(name, Qt::CaseInsensitive) == 0)
            return o;
    bool ok = false;
    const int n = name.startsWith("out", Qt::CaseInsensitive) ? name.mid(3).toInt(&ok) : 0;
    return ok && n >= 1 && n <= outputs.size() ? n - 1 : -1;
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
    // The chain playing, by its number (an older engine's: the preset's).
    const QJsonObject selected = j.value("selected").toObject();
    s.selected = selected.value("chain").toInt(selected.value("index").toInt(-1));
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
        for (const QJsonValue& v : c.value("wire_peaks").toArray())
            cs.wirePeaks << v.toDouble(-120);
        s.chains << cs;
    }
    const QJsonObject levels = j.value("levels").toObject();
    for (const QJsonValue& v : levels.value("inputs").toArray())
        s.inputs << Level{v.toObject().value("name").toString(), v.toObject().value("peak_db").toDouble(-120)};
    for (const QJsonValue& v : levels.value("outputs").toArray())
        s.outputs << Level{v.toObject().value("name").toString(), v.toObject().value("peak_db").toDouble(-120), v.toObject().value("limit_db").toDouble(0)};
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

double TunerReading::turn(double phase, double seconds) const {
    if (!valid || note.isEmpty() || held)
        return phase;
    phase += std::clamp(cents * 0.25, -12.0, 12.0) * seconds;
    return phase - std::floor(phase);
}

PluginInfo ScannedPlugin::toPlugin() const {
    PluginInfo b;
    b.format = format;
    b.path = path;
    b.id = id;
    b.label = name;
    b.hasKey = sidechain;
    return b;
}

Catalog Catalog::fromJson(const QJsonObject& j) {
    Catalog c;
    c.valid = true;
    c.progress(j);
    const QJsonObject folders = j.value("folders").toObject();
    for (const char* kind : {"clap", "vst3"})
        for (const QJsonValue& d : folders.value(kind).toArray())
            if (!c.usual.contains(d.toString()))
                c.usual << d.toString();
    for (const QJsonValue& d : folders.value("extra").toArray())
        c.extra << d.toString();
    for (const QJsonValue& fv : j.value("files").toArray()) {
        const QJsonObject f = fv.toObject();
        const QString fileError = f.value("error").toString();
        if (!fileError.isEmpty()) {
            ScannedPlugin p;
            p.format = f.value("format").toString();
            p.path = f.value("path").toString();
            p.name = QFileInfo(p.path).completeBaseName();
            p.error = fileError;
            c.plugins << p;
            continue;
        }
        for (const QJsonValue& pv : f.value("plugins").toArray()) {
            const QJsonObject o = pv.toObject();
            ScannedPlugin p;
            p.format = o.value("format").toString();
            p.path = o.value("path").toString();
            p.id = o.value("id").toString();
            p.name = o.value("name").toString();
            p.vendor = o.value("vendor").toString();
            p.version = o.value("version").toString();
            for (const QJsonValue& cat : o.value("categories").toArray())
                p.categories << cat.toString();
            p.bridged = o.value("bridged").toBool();
            p.opened = o.value("opened").toBool();
            p.sidechain = o.value("sidechain").toBool();
            p.editor = o.value("editor").toBool();
            p.midi = o.value("midi").toBool();
            p.error = o.value("error").toString();
            if (p.name.isEmpty())
                p.name = QFileInfo(p.path).completeBaseName();
            c.plugins << p;
        }
    }
    std::stable_sort(c.plugins.begin(), c.plugins.end(), [](const ScannedPlugin& a, const ScannedPlugin& b) {
        return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
    });
    return c;
}

void Catalog::progress(const QJsonObject& j) {
    scanning = j.value("scanning").toBool();
    done = j.value("done").toInt();
    total = j.value("total").toInt();
    current = j.value("current").toString();
    scannedAt = static_cast<qint64>(j.value("scanned_at").toDouble());
}

int Catalog::ready() const {
    return static_cast<int>(std::count_if(plugins.begin(), plugins.end(), [](const ScannedPlugin& p) { return p.ready(); }));
}

bool isPlugin(const PluginInfo& block, const ScannedPlugin& p) {
    if (block.format != p.format || p.path.isEmpty())
        return false;
    const bool samePath = block.path == p.path || (block.path.startsWith("~/") && p.path.endsWith(block.path.mid(1)));
    if (!samePath)
        return false;
    return block.id.isEmpty() || block.id == p.id || block.id.compare(p.name, Qt::CaseInsensitive) == 0 ||
           (p.format == "vst3" && QString(block.id).remove(QRegularExpression("[{}-]")).compare(p.id, Qt::CaseInsensitive) == 0);
}

QStringList chainsWith(const Performance& perf, const ScannedPlugin& p) {
    QStringList out;
    for (const ChainInfo& c : perf.chains)
        for (const PluginInfo& b : c.blocks)
            if (isPlugin(b, p) && !out.contains(c.name)) {
                out << c.name;
                break;
            }
    return out;
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
