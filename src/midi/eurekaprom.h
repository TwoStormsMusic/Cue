#pragma once

// Behringer FCB1010 running EurekaPROM 3.0 in I/O mode. The pedals send
// controller messages on press and release, and the host drives the LEDs
// and the 7-segment display with controller messages back.

#include "alsa_midi.h"

#include <string>
#include <vector>

namespace cue::eurekaprom {

constexpr uint8_t kPressCC = 104, kReleaseCC = 105;
constexpr uint8_t kLeftExprCC = 102, kRightExprCC = 103;
constexpr uint8_t kLedOnCC = 106, kLedOffCC = 107;
constexpr uint8_t kDisplayNumberCC = 108;
constexpr uint8_t kDisplayTensLetterCC = 109, kDisplayOnesLetterCC = 110;
constexpr uint8_t kDisplayLeadingCC = 112;
constexpr uint8_t kDisplayTensHexCC = 113, kDisplayOnesHexCC = 114;

// Pedals are numbered 1-10, with Up = 11 and Down = 12.
constexpr int kUp = 11, kDown = 12;

struct Input {
    enum class Kind { None, Press, Release, Expression } kind = Kind::None;
    int pedal = 0;      // Press/Release
    int expression = 0; // 1 = left, 2 = right
    int value = 0;      // Expression 0-127
};

inline int pedalFromValue(uint8_t v) {
    if (v == 0) return 10;
    if (v <= 9) return v;
    if (v == 10) return kUp;
    if (v == 11) return kDown;
    return 0;
}

inline uint8_t ledValueForPedal(int pedal) { return pedal == 10 ? 0 : static_cast<uint8_t>(pedal); }

inline Input decode(const MidiMessage& m) {
    Input in;
    if (m.type() != 0xB0)
        return in;
    if (m.data1 == kPressCC || m.data1 == kReleaseCC) {
        in.pedal = pedalFromValue(m.data2);
        if (in.pedal)
            in.kind = m.data1 == kPressCC ? Input::Kind::Press : Input::Kind::Release;
    } else if (m.data1 == kLeftExprCC || m.data1 == kRightExprCC) {
        in.kind = Input::Kind::Expression;
        in.expression = m.data1 == kLeftExprCC ? 1 : 2;
        in.value = m.data2;
    }
    return in;
}

// LEDs exist for pedals 1-10 only; Up and Down have none.
inline MidiMessage led(uint8_t channel, int pedal, bool on) {
    return MidiMessage::cc(channel, on ? kLedOnCC : kLedOffCC, ledValueForPedal(pedal));
}

inline MidiMessage displayNumber(uint8_t channel, int number) {
    return MidiMessage::cc(channel, kDisplayNumberCC, static_cast<uint8_t>(number < 0 ? 0 : number > 127 ? 127 : number));
}

// Two letters A-Z on the tens and ones digits; anything else blanks the digit.
inline MidiMessage displayLetter(uint8_t channel, bool tens, char letter) {
    const uint8_t v = (letter >= 'A' && letter <= 'Z') ? static_cast<uint8_t>(letter - 'A')
                      : (letter >= 'a' && letter <= 'z') ? static_cast<uint8_t>(letter - 'a')
                                                         : 26;
    return MidiMessage::cc(channel, tens ? kDisplayTensLetterCC : kDisplayOnesLetterCC, v);
}

// The leading digit shows only ' ', '1', '-', '+' (and "+1"); anything else blanks it.
inline MidiMessage displayLeading(uint8_t channel, char c) {
    const uint8_t v = c == '1' ? 1 : c == '-' ? 2 : c == '+' ? 3 : 5;
    return MidiMessage::cc(channel, kDisplayLeadingCC, v);
}

// A hex digit 0-F on the tens or ones digit; anything above blanks it.
inline MidiMessage displayHex(uint8_t channel, bool tens, int digit) {
    return MidiMessage::cc(channel, tens ? kDisplayTensHexCC : kDisplayOnesHexCC, static_cast<uint8_t>(digit < 0 || digit > 15 ? 16 : digit));
}

// Text on the display: letters A-Z and digits 0-9 on the two main digits,
// left-aligned. A third character in front goes on the leading digit if it
// is one it can show (' ', '1', '-', '+').
inline std::vector<MidiMessage> displayText(uint8_t channel, std::string text) {
    char lead = ' ';
    if (text.size() >= 3 && (text[0] == ' ' || text[0] == '1' || text[0] == '-' || text[0] == '+')) {
        lead = text[0];
        text = text.substr(1);
    }
    text.resize(2, ' ');
    std::vector<MidiMessage> out{displayLeading(channel, lead)};
    for (int i = 0; i < 2; ++i) {
        const char c = text[i];
        if (c >= '0' && c <= '9')
            out.push_back(displayHex(channel, i == 0, c - '0'));
        else
            out.push_back(displayLetter(channel, i == 0, c));
    }
    return out;
}

} // namespace cue::eurekaprom
