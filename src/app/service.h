#pragma once

// The control socket of a running Cue. The desktop app and `cue ctl` connect
// to a Unix socket in the user's runtime folder. Each message, either way, is
// a 4-byte big-endian length and then that many bytes of JSON.
//
//   request  {"id": 7, "cmd": "do", "action": "Wah = Auto"}
//   reply    {"id": 7, "ok": true, ...}  or  {"id": 7, "ok": false, "error": "..."}
//   event    {"event": "status", ...}    to the clients that subscribed to it
//
// "subscribe" {"events": ["status", "log"]} and "unsubscribe" are answered
// here; every other request goes to the handler. Everything runs on the
// thread that calls poll() (the main thread, in cue run), so a handler may
// ask plug-ins for their state. Nothing here is on the audio path.

#include "nlohmann/json.hpp"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace cue {

using Json = nlohmann::ordered_json;

constexpr int kProtocol = 1;
constexpr uint32_t kMaxMessage = 16u << 20;

// $XDG_RUNTIME_DIR/cue/engine.sock, else /tmp/cue-<uid>/engine.sock.
std::string defaultSocketPath();

class Service {
public:
    // Answers one request by filling in reply (which already has "id" and
    // "ok": true). Set "ok" false and "error" for a request it refuses.
    using Handler = std::function<void(const Json& request, Json& reply)>;

    ~Service();
    // Fails if another Cue answers on that path; a socket left by one that
    // crashed is replaced.
    bool listen(const std::string& path, Handler handler, std::string* error);
    void close();
    bool listening() const { return fd_ >= 0; }
    const std::string& path() const { return path_; }

    // Accepts clients, reads and answers their requests and sends what is
    // waiting, for up to timeoutMs (returning early once something happened).
    // also holds file descriptors that belong to something else (a plug-in
    // editor's screen): the wait ends when one of them has something too, so
    // the caller can see to it.
    void poll(int timeoutMs, const std::vector<int>& also = {});
    // To every client subscribed to the event.
    void publish(const std::string& event, Json body);
    bool subscribed(const std::string& event) const;
    size_t clients() const { return clients_.size(); }

private:
    struct Client {
        int fd = -1;
        std::string in, out;
        std::set<std::string> events;
    };
    void handle(Client& c, const std::string& text);
    void send(Client& c, const Json& message);
    bool flush(Client& c);

    int fd_ = -1;
    std::string path_;
    Handler handler_;
    std::vector<Client> clients_;
};

// The other end: cue ctl, and the tests.
class ServiceClient {
public:
    ~ServiceClient();
    bool connect(const std::string& path, std::string* error);
    void close();
    // Sends a request and waits for its reply; events that arrive meanwhile
    // are kept for next().
    bool request(Json request, Json& reply, int timeoutMs, std::string* error);
    // The next event (or reply nobody waited for), waiting up to timeoutMs.
    bool next(Json& message, int timeoutMs, std::string* error);

private:
    bool read(Json& message, int timeoutMs, std::string* error);
    int fd_ = -1;
    int nextId_ = 1;
    std::string in_;
    std::vector<Json> pending_;
};

} // namespace cue
