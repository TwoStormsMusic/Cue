#pragma once

// What a plug-in's own editor changed, as undoable edits: each parameter
// moved from where it was to where it was let go. A plug-in says when a
// knob is grabbed and let go (a gesture); for one that does not, changes to
// one parameter with no more than a moment between them are one edit.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace cue {

struct ParamEdit {
    uint32_t id;
    double from, to; // as setParam takes them
};

class EditorEdits {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kQuiet{600};

    // Any non-real-time thread. A value the host knows the parameter has
    // (set by the host, or read from the plug-in): where an edit starts.
    void known(uint32_t id, double value) {
        std::lock_guard<std::mutex> lock(mutex_);
        known_[id] = value;
    }
    // The plug-in's editor: grabbed, moved, let go.
    void begin(uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        Open& o = open(id);
        o.gesture = true;
    }
    void change(uint32_t id, double value, Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> lock(mutex_);
        Open& o = open(id);
        o.to = value;
        o.moved = true;
        o.last = now;
        known_[id] = value;
    }
    void end(uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = open_.find(id);
        if (it == open_.end())
            return;
        finish(id, it->second);
        open_.erase(it);
    }
    // Main thread: the edits finished since the last call.
    void take(std::vector<ParamEdit>& out, Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = open_.begin(); it != open_.end();) {
            if (!it->second.gesture && it->second.moved && now - it->second.last >= kQuiet) {
                finish(it->first, it->second);
                it = open_.erase(it);
            } else {
                ++it;
            }
        }
        out.insert(out.end(), done_.begin(), done_.end());
        done_.clear();
    }

private:
    struct Open {
        double from = std::numeric_limits<double>::quiet_NaN(), to = 0;
        bool gesture = false, moved = false;
        Clock::time_point last{};
    };
    Open& open(uint32_t id) {
        auto it = open_.find(id);
        if (it != open_.end())
            return it->second;
        Open& o = open_[id];
        if (auto k = known_.find(id); k != known_.end())
            o.from = k->second;
        return o;
    }
    // Kept if it is known where it started and it went somewhere else.
    void finish(uint32_t id, const Open& o) {
        if (o.moved && !std::isnan(o.from) && o.from != o.to)
            done_.push_back({id, o.from, o.to});
    }

    std::mutex mutex_;
    std::unordered_map<uint32_t, double> known_;
    std::unordered_map<uint32_t, Open> open_;
    std::vector<ParamEdit> done_;
};

} // namespace cue
