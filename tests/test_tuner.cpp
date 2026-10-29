// The built-in tuner: notes found, cents settled, a new note taken only when
// it holds, and its settings kept with the performance.

#include "check.h"

#include "app/config.h"
#include "app/cuefile.h"
#include "tuner/tuner.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <unistd.h>

using namespace cue;

namespace {

constexpr double kRate = 48000.0;
constexpr float kPi = 3.14159265f;

// A plucked string: a fundamental and a few partials, dying away slowly.
struct Note {
    double hz;
    double phase = 0, t = 0;
    float operator()() {
        float v = 0;
        for (int k = 1; k <= 4; ++k)
            v += static_cast<float>(std::sin(phase * k) / k);
        phase += 2.0 * kPi * hz / kRate;
        t += 1.0 / kRate;
        return 0.3f * v * static_cast<float>(std::exp(-t / 4.0));
    }
};

double centsAbove(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

// Plays a note into the tuner for a while, looking 30 times a second.
TunerReading play(Tuner& tuner, double hz, double seconds, double& now) {
    Note note{hz};
    std::vector<float> block(64);
    double nextLook = now;
    const int blocks = static_cast<int>(seconds * kRate / 64);
    for (int b = 0; b < blocks; ++b) {
        for (auto& v : block)
            v = note();
        tuner.write(block.data(), 64);
        now += 64 / kRate;
        if (now >= nextLook) {
            tuner.step(now, 0.03);
            nextLook += 0.03;
        }
    }
    return tuner.reading();
}

} // namespace

TEST(tuner_finds_bass_and_guitar_notes) {
    Tuner tuner(kRate);
    tuner.setListening(true, false);
    double now = 0;
    // Low E of a bass, in tune.
    TunerReading r = play(tuner, 41.2034, 1.0, now);
    CHECK_MSG(r.note && r.name == "E" && r.octave == 1, "%s%d", r.name.c_str(), r.octave);
    CHECK_MSG(std::fabs(r.cents) < 0.5 && r.inTune && !r.held, "%.2f cents", r.cents);
    // A five-string's low B, 7 cents sharp: found, and not in tune within 2 cents.
    const double sharpB = 30.8677 * std::pow(2.0, 7.0 / 1200);
    r = play(tuner, sharpB, 1.5, now);
    CHECK_MSG(r.name == "B" && r.octave == 0 && std::fabs(r.cents - 7.0) < 1.0 && !r.inTune, "%s%d %.2f", r.name.c_str(), r.octave, r.cents);
    // A guitar's G string, 12 cents flat, named in flats when asked.
    TunerSettings s = tuner.settings();
    s.flats = true;
    tuner.setSettings(s);
    const double flatG = 196.0 * std::pow(2.0, -12.0 / 1200);
    r = play(tuner, flatG, 1.0, now);
    CHECK_MSG(r.name == "G" && r.octave == 3 && std::fabs(r.cents + 12.0) < 1.0, "%s%d %.2f", r.name.c_str(), r.octave, r.cents);
    CHECK(std::fabs(centsAbove(r.hz, flatG)) < 1.0);
    r = play(tuner, 233.08, 1.0, now); // B flat
    CHECK_MSG(r.name == "Bb", "%s", r.name.c_str());
}

TEST(tuner_follows_the_reference_and_holds_the_last_note) {
    Tuner tuner(kRate);
    TunerSettings s;
    s.reference = 442;
    tuner.setSettings(s);
    tuner.setListening(true, false);
    double now = 0;
    TunerReading r = play(tuner, 110.0, 1.0, now); // A at 440 is 7.85 cents flat of A at 442
    CHECK_MSG(r.name == "A" && std::fabs(r.cents + 7.85) < 1.0, "%s %.2f", r.name.c_str(), r.cents);

    // Silence: the note is held, then nothing is in tune.
    std::vector<float> silence(64, 0.0f);
    for (int b = 0; b < 750; ++b) {
        tuner.write(silence.data(), 64);
        now += 64 / kRate;
        if (b % 22 == 0)
            tuner.step(now, 0.03);
    }
    r = tuner.reading();
    CHECK(r.note && r.held && !r.inTune && !r.signal && r.name == "A");

    // Not listening: the audio thread writes nothing, and a new start forgets the last note.
    tuner.setListening(false);
    tuner.setListening(true, false);
    CHECK(!tuner.reading().note);
}

TEST(tuner_settings_are_kept_with_the_performance) {
    char name[] = "/tmp/cue-tuner-XXXXXX";
    const std::string dir = mkdtemp(name) ? name : "/tmp";
    Config ini;
    std::string err;
    CHECK_MSG(parseConfigText("[performance]\ninputs = Bass, Mic\n[tuner]\ninput = Mic\nreference = 442\nrange = vocals\ntolerance = 5\nnames = flats\n"
                              "[chain A]\nplugin = builtin:gain 1\n",
                              dir + "/t.ini", ini, &err),
              "%s", err.c_str());
    CHECK(ini.tuner.inputIndex == 1 && ini.tuner.settings.reference == 442 && ini.tuner.settings.range == tuner::PitchDetector::R_VOCALS);
    CHECK(ini.tuner.settings.tolerance == 5 && ini.tuner.settings.flats);
    CHECK_MSG(saveCue(ini, dir + "/t.cue", &err), "%s", err.c_str());
    Config c;
    CHECK_MSG(loadCue(dir + "/t.cue", c, &err), "%s", err.c_str());
    CHECK(c.tuner.inputIndex == 1 && c.tuner.input == "Mic" && c.tuner.settings.reference == 442);
    CHECK(c.tuner.settings.range == tuner::PitchDetector::R_VOCALS && c.tuner.settings.tolerance == 5 && c.tuner.settings.flats);

    Config bad;
    CHECK(!parseConfigText("[tuner]\ninput = Guitar\n[chain A]\nplugin = builtin:gain 1\n", dir + "/b.ini", bad, &err));
    CHECK_MSG(err.find("no such input") != std::string::npos, "%s", err.c_str());
    CHECK(!parseConfigText("[tuner]\nreference = 500\n[chain A]\nplugin = builtin:gain 1\n", dir + "/b.ini", bad, &err));
}
