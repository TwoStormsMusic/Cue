#include "tuner/tuner.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace cue {

namespace {

const char* const kSharps[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* const kFlats[] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
const char* const kRanges[] = {"guitar", "bass", "vocals", "any"};

// Below this the tuner does not look for a pitch (as the TwoStorms Tuner).
constexpr double kSilenceDb = -66.0;

double seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

int tunerRange(const std::string& name) {
    for (int r = 0; r < 4; ++r)
        if (name == kRanges[r])
            return r;
    return -1;
}

const char* tunerRangeName(int range) { return kRanges[std::clamp(range, 0, 3)]; }

Tuner::Tuner(double sampleRate) : sampleRate_(sampleRate), ring_(new std::atomic<float>[kRing]) {
    for (int i = 0; i < kRing; ++i)
        ring_[i].store(0.0f, std::memory_order_relaxed);
}

Tuner::~Tuner() { setListening(false); }

void Tuner::setSettings(const TunerSettings& settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
}

TunerSettings Tuner::settings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

TunerReading Tuner::reading() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reading_;
}

void Tuner::setListening(bool on, bool withThread) {
    if (on == listening())
        return;
    if (on) {
        // A fresh start: nothing heard before now counts.
        startedAt_ = written_.load(std::memory_order_acquire);
        have_ = false;
        nmed_ = pend_ = 0;
        pendNote_ = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reading_ = TunerReading();
        }
        stop_ = false;
        listening_.store(true, std::memory_order_release);
        if (withThread)
            thread_ = std::thread([this] { run(); });
    } else {
        {
            std::lock_guard<std::mutex> lock(wakeMutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable())
            thread_.join();
        listening_.store(false, std::memory_order_release);
    }
}

void Tuner::write(const float* in, uint32_t frames) {
    if (!listening_.load(std::memory_order_relaxed))
        return;
    const int64_t w = written_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < frames; ++i)
        ring_[(w + i) & kMask].store(in[i], std::memory_order_relaxed);
    written_.store(w + frames, std::memory_order_release);
}

void Tuner::run() {
    double last = seconds();
    std::unique_lock<std::mutex> lock(wakeMutex_);
    while (!stop_) {
        wake_.wait_for(lock, std::chrono::milliseconds(30), [this] { return stop_; });
        if (stop_)
            break;
        lock.unlock();
        const double now = seconds();
        step(now, std::min(0.2, now - last));
        last = now;
        lock.lock();
    }
}

void Tuner::step(double now, double dt) {
    const TunerSettings s = settings();
    detector_.setup(sampleRate_, s.range);
    const int need = detector_.need();
    const int64_t w = written_.load(std::memory_order_acquire);
    const int64_t have = w - startedAt_;

    // The level of the newest 2048 samples.
    double sq = 0;
    const int n = static_cast<int>(std::min<int64_t>(have, 2048));
    for (int i = 1; i <= n; ++i) {
        const double v = ring_[(w - i) & kMask].load(std::memory_order_relaxed);
        sq += v * v;
    }
    const double level = n > 0 && sq > 1e-30 ? 10 * std::log10(sq / n) : -200;

    bool ok = false;
    double f = 0;
    if (have >= need && level > kSilenceDb) {
        if (static_cast<int>(buf_.size()) < need)
            buf_.resize(need);
        for (int i = 0; i < need; ++i)
            buf_[i] = ring_[(w - need + i) & kMask].load(std::memory_order_relaxed);
        tuner::PitchDetector::Result r;
        ok = detector_.detect(buf_.data(), r) && r.f > 0;
        f = r.f;
    }
    update(ok, f, now, dt);

    TunerReading r;
    r.signal = level > kSilenceDb;
    r.levelDb = level;
    r.note = have_;
    r.held = !have_ || now - lastPitch_ > 0.25;
    r.inTune = !r.held && std::fabs(cents_) <= s.tolerance + 1e-9;
    r.midi = note_;
    r.cents = cents_;
    r.hz = s.reference * std::pow(2.0, (note_ + cents_ / 100 - 69) / 12);
    r.name = (s.flats ? kFlats : kSharps)[((note_ % 12) + 12) % 12];
    r.octave = static_cast<int>(std::floor(note_ / 12.0)) - 1;
    std::lock_guard<std::mutex> lock(mutex_);
    reading_ = r;
}

// As the TwoStorms Tuner: another note needs two looks in a row past 60
// cents; the cents are the median of the last three, smoothed over ~90 ms.
void Tuner::update(bool ok, double f, double now, double dt) {
    if (!ok)
        return;
    const double midi = 69 + 12 * std::log2(f / settings().reference);
    auto restart = [&](int n) {
        note_ = n;
        cents_ = (midi - n) * 100;
        nmed_ = 0;
        pend_ = 0;
        pendNote_ = -1;
    };
    if (!have_ || now - lastPitch_ > 0.4) {
        restart(static_cast<int>(std::lround(midi)));
        have_ = true;
    } else {
        const double c = (midi - note_) * 100;
        if (std::fabs(c) > 60) {
            const int nn = static_cast<int>(std::lround(midi));
            if (nn == pendNote_) {
                if (++pend_ >= 2)
                    restart(nn);
            } else {
                pendNote_ = nn;
                pend_ = 1;
            }
        } else {
            pend_ = 0;
            med_[nmed_ % 3] = c;
            ++nmed_;
            double m = c;
            if (nmed_ >= 3) {
                const double a = med_[0], b = med_[1], e = med_[2];
                m = std::max(std::min(a, b), std::min(std::max(a, b), e));
            }
            cents_ += (1 - std::exp(-dt / 0.09)) * (m - cents_);
        }
    }
    lastPitch_ = now;
}

} // namespace cue
