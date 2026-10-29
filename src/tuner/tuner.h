#pragma once

// The built-in tuner: the TwoStorms Tuner listening to one of the
// performance's inputs, before any plug-in. The audio thread only copies that
// input into a ring while someone is looking; the pitch is found on a thread
// of the tuner's own, about 30 times a second, with the TwoStorms Tuner's
// detector and its way of settling the reading (a new note needs two looks in
// a row, the cents are the median of the last three, smoothed over 90 ms).

#include "tuner/pitch.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cue {

struct TunerSettings {
    double reference = 440.0;                          // A4 in Hz, 415 to 466
    int range = tuner::PitchDetector::R_WIDE;          // R_GUITAR, R_BASS, R_VOCALS or R_WIDE
    double tolerance = 2.0;                            // in tune within this many cents
    bool flats = false;                                // Db rather than C#
};

// "guitar", "bass", "vocals", "any": -1 for anything else.
int tunerRange(const std::string& name);
const char* tunerRangeName(int range);

struct TunerReading {
    bool signal = false;  // the input is above the tuner's threshold (-66 dBFS)
    bool note = false;    // a note has been heard since the tuner started listening
    bool held = true;     // no pitch for a quarter second: the last note, held
    bool inTune = false;
    int midi = 69;
    double cents = 0;     // against midi, settled
    double hz = 0;        // what the note and cents come to, against the reference
    double levelDb = -200;
    std::string name;     // "E", "F#" or "Gb"
    int octave = 4;
};

class Tuner {
public:
    explicit Tuner(double sampleRate);
    ~Tuner();
    Tuner(const Tuner&) = delete;
    Tuner& operator=(const Tuner&) = delete;

    void setSettings(const TunerSettings& settings);
    TunerSettings settings() const;

    // Main thread: listens (and runs its thread) while on. The tests listen
    // without the thread and call step() themselves.
    void setListening(bool on, bool withThread = true);
    bool listening() const { return listening_.load(std::memory_order_relaxed); }

    // Audio thread: the input, every block. Nothing happens while not listening.
    void write(const float* in, uint32_t frames);

    TunerReading reading() const;

    // The analysis on its own, for the tests: takes the newest samples from
    // the ring and moves the reading on by dt seconds.
    void step(double now, double dt);

private:
    void run();
    void update(bool ok, double f, double now, double dt);

    static constexpr int kRing = 65536, kMask = kRing - 1;
    const double sampleRate_;
    std::unique_ptr<std::atomic<float>[]> ring_;
    std::atomic<int64_t> written_{0};
    std::atomic<bool> listening_{false};
    int64_t startedAt_ = 0; // written_ when it started listening

    mutable std::mutex mutex_; // settings_ and reading_
    TunerSettings settings_;
    TunerReading reading_;

    std::thread thread_;
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    bool stop_ = false;

    // The analysis thread's own.
    tuner::PitchDetector detector_;
    std::vector<float> buf_;
    bool have_ = false;
    int note_ = 69;
    double cents_ = 0, lastPitch_ = -10;
    double med_[3] = {0, 0, 0};
    int nmed_ = 0, pendNote_ = -1, pend_ = 0;
};

} // namespace cue
