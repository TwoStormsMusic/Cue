#include "alsa_midi.h"

#include <alsa/asoundlib.h>
#include <poll.h>

#include <algorithm>
#include <cctype>

namespace sh {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
} // namespace

AlsaMidi::~AlsaMidi() { close(); }

bool AlsaMidi::open(const std::string& clientName, std::string* error) {
    close();
    int err = snd_seq_open(&seq_, "default", SND_SEQ_OPEN_DUPLEX, 0);
    if (err < 0) {
        if (error)
            *error = std::string("ALSA sequencer: ") + snd_strerror(err);
        seq_ = nullptr;
        return false;
    }
    snd_seq_set_client_name(seq_, clientName.c_str());
    client_ = snd_seq_client_id(seq_);
    inPort_ = snd_seq_create_simple_port(seq_, "in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                         SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    outPort_ = snd_seq_create_simple_port(seq_, "out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                          SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (inPort_ < 0 || outPort_ < 0) {
        if (error)
            *error = "cannot create sequencer ports";
        close();
        return false;
    }
    return true;
}

void AlsaMidi::close() {
    if (seq_)
        snd_seq_close(seq_);
    seq_ = nullptr;
    inPort_ = outPort_ = client_ = -1;
}

std::vector<MidiPort> AlsaMidi::listPorts() const {
    std::vector<MidiPort> out;
    if (!seq_)
        return out;
    snd_seq_client_info_t* ci;
    snd_seq_port_info_t* pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq_, ci) >= 0) {
        const int c = snd_seq_client_info_get_client(ci);
        if (c == client_ || c == SND_SEQ_CLIENT_SYSTEM)
            continue;
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq_, pi) >= 0) {
            const unsigned caps = snd_seq_port_info_get_capability(pi);
            if (caps & SND_SEQ_PORT_CAP_NO_EXPORT)
                continue;
            out.push_back({c, snd_seq_port_info_get_port(pi), snd_seq_client_info_get_name(ci), snd_seq_port_info_get_name(pi),
                           (caps & SND_SEQ_PORT_CAP_READ) && (caps & SND_SEQ_PORT_CAP_SUBS_READ),
                           (caps & SND_SEQ_PORT_CAP_WRITE) && (caps & SND_SEQ_PORT_CAP_SUBS_WRITE)});
        }
    }
    return out;
}

int AlsaMidi::connectInputFrom(const std::string& match) {
    int n = 0;
    const std::string m = lower(match);
    for (const auto& p : listPorts()) {
        if (!p.canRead)
            continue;
        if (m == "any" || lower(p.clientName + ":" + p.portName).find(m) != std::string::npos)
            n += snd_seq_connect_from(seq_, inPort_, p.client, p.port) >= 0;
    }
    return n;
}

int AlsaMidi::connectOutputTo(const std::string& match) {
    int n = 0;
    const std::string m = lower(match);
    for (const auto& p : listPorts()) {
        if (!p.canWrite || m == "any")
            continue;
        if (lower(p.clientName + ":" + p.portName).find(m) != std::string::npos)
            n += snd_seq_connect_to(seq_, outPort_, p.client, p.port) >= 0;
    }
    return n;
}

bool AlsaMidi::read(MidiMessage& message, int timeoutMs) {
    if (!seq_)
        return false;
    for (;;) {
        if (snd_seq_event_input_pending(seq_, 1) == 0) {
            const int nfds = snd_seq_poll_descriptors_count(seq_, POLLIN);
            std::vector<pollfd> fds(static_cast<size_t>(nfds));
            snd_seq_poll_descriptors(seq_, fds.data(), static_cast<unsigned>(nfds), POLLIN);
            if (poll(fds.data(), static_cast<nfds_t>(nfds), timeoutMs) <= 0)
                return false;
        }
        snd_seq_event_t* ev = nullptr;
        if (snd_seq_event_input(seq_, &ev) < 0 || !ev)
            return false;
        switch (ev->type) {
        case SND_SEQ_EVENT_CONTROLLER:
            message = MidiMessage::cc(ev->data.control.channel, static_cast<uint8_t>(ev->data.control.param),
                                      static_cast<uint8_t>(ev->data.control.value));
            return true;
        case SND_SEQ_EVENT_PGMCHANGE:
            message = MidiMessage::program(ev->data.control.channel, static_cast<uint8_t>(ev->data.control.value));
            return true;
        case SND_SEQ_EVENT_NOTEON:
            message = {static_cast<uint8_t>(0x90 | ev->data.note.channel), ev->data.note.note, ev->data.note.velocity};
            return true;
        case SND_SEQ_EVENT_NOTEOFF:
            message = {static_cast<uint8_t>(0x80 | ev->data.note.channel), ev->data.note.note, ev->data.note.velocity};
            return true;
        default:
            break; // ignore clock, sysex, port announcements
        }
        timeoutMs = 0;
    }
}

void AlsaMidi::send(const MidiMessage& m) {
    if (!seq_)
        return;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, outPort_);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    switch (m.type()) {
    case 0xB0: snd_seq_ev_set_controller(&ev, m.channel(), m.data1, m.data2); break;
    case 0xC0: snd_seq_ev_set_pgmchange(&ev, m.channel(), m.data1); break;
    case 0x90: snd_seq_ev_set_noteon(&ev, m.channel(), m.data1, m.data2); break;
    case 0x80: snd_seq_ev_set_noteoff(&ev, m.channel(), m.data1, m.data2); break;
    default: return;
    }
    snd_seq_event_output_direct(seq_, &ev);
}

} // namespace sh
