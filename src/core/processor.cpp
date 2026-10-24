#include "processor.h"

namespace cue {

namespace {
thread_local bool t_audioThread = false;
}

void markAudioThread() { t_audioThread = true; }
bool isAudioThread() { return t_audioThread; }

} // namespace cue
