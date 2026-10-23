#pragma once

#include <cstdint>
#include <string>
#include <vector>

typedef struct _snd_seq snd_seq_t;

namespace sh {

struct MidiMessage {
    uint8_t status = 0; // includes the channel (0-based) in the low nibble
    uint8_t data1 = 0;
    uint8_t data2 = 0;

    uint8_t type() const { return status & 0xF0; }
    uint8_t channel() const { return status & 0x0F; }
    static MidiMessage cc(uint8_t channel, uint8_t number, uint8_t value) { return {static_cast<uint8_t>(0xB0 | (channel & 0x0F)), number, value}; }
    static MidiMessage program(uint8_t channel, uint8_t number) { return {static_cast<uint8_t>(0xC0 | (channel & 0x0F)), number, 0}; }
};

struct MidiPort {
    int client, port;
    std::string clientName, portName;
    bool canRead, canWrite; // read: it sends to us; write: we can send to it
};

// MIDI through the ALSA sequencer: one client with an input and an output
// port, connectable to any hardware or software port. Not real-time; runs on
// its own thread.
class AlsaMidi {
public:
    ~AlsaMidi();
    bool open(const std::string& clientName, std::string* error);
    void close();

    std::vector<MidiPort> listPorts() const;
    // Connects every port whose "client:port" name contains match (case-insensitive).
    // Returns how many were connected.
    int connectInputFrom(const std::string& match);
    int connectOutputTo(const std::string& match);

    // Waits up to timeoutMs for one message. False on timeout.
    bool read(MidiMessage& message, int timeoutMs);
    void send(const MidiMessage& message);

private:
    snd_seq_t* seq_ = nullptr;
    int inPort_ = -1, outPort_ = -1, client_ = -1;
};

} // namespace sh
