#pragma once

// Levels on faders and as text: a wire's, an output's, an input's gain.

#include <QString>

#include <algorithm>
#include <cmath>

namespace cue::gui {

constexpr double kOffDb = -96;

// A wire's or an output's level on its fader: 0 dB three quarters along,
// +12 dB at the end, and below 0 dB a fader's curve down to silent.
inline double levelToFader(double db) {
    if (db <= kOffDb)
        return 0;
    if (db >= 0)
        return 0.75 + std::min(db, 12.0) / 12.0 * 0.25;
    return 0.75 * std::pow(10.0, db / 60.0);
}

inline double faderToLevel(double f) {
    if (f >= 0.75)
        return (f - 0.75) / 0.25 * 12.0;
    if (f <= levelToFader(kOffDb + 0.1))
        return kOffDb;
    return 60.0 * std::log10(f / 0.75);
}

// An input's gain (its calibration) on its fader: -24 to +36 dB evenly,
// with silence at the very start.
constexpr double kGainLowDb = -24, kGainHighDb = 36;
inline double gainToFader(double db) {
    if (db <= kOffDb)
        return 0;
    return std::clamp((db - kGainLowDb) / (kGainHighDb - kGainLowDb), 0.01, 1.0);
}

inline double faderToGain(double f) { return f < 0.01 ? kOffDb : kGainLowDb + f * (kGainHighDb - kGainLowDb); }

inline QString levelText(double db) {
    if (db <= kOffDb)
        return "off";
    const double r = std::round(db * 10) / 10;
    return QString("%1%2 dB").arg(r > 0 ? "+" : r < 0 ? "−" : "").arg(std::fabs(r), 0, 'f', 1);
}

// What was typed for a level: a number of dB, or off.
inline bool typedLevel(QString text, double& db) {
    text = text.trimmed().toLower().replace("−", "-").replace("db", "").trimmed();
    if (text == "off" || text == "-inf" || text == "inf") {
        db = kOffDb;
        return true;
    }
    bool ok = false;
    db = text.toDouble(&ok);
    return ok && std::isfinite(db);
}

} // namespace cue::gui
