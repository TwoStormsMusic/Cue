// The control socket: requests, replies, events, and what it refuses.

#include "check.h"

#include "app/service.h"

#include <atomic>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace cue;

namespace {

std::string tempSocket() {
    char name[] = "/tmp/cue-service-XXXXXX";
    return std::string(mkdtemp(name) ? name : "/tmp") + "/engine.sock";
}

// A service answering on its own thread, as the main loop of cue run does.
struct Running {
    Service service;
    std::atomic<bool> run{true};
    std::atomic<int> ticks{0};
    std::thread thread;

    bool start(const std::string& path, std::string* error) {
        const bool ok = service.listen(path, [](const Json& req, Json& reply) {
            const std::string cmd = req.at("cmd").get<std::string>();
            if (cmd == "echo")
                reply["text"] = req.at("text");
            else if (cmd == "fail")
                throw std::runtime_error("it failed");
            else {
                reply["ok"] = false;
                reply["error"] = "unknown command '" + cmd + "'";
            }
        }, error);
        if (ok)
            thread = std::thread([this] {
                while (run) {
                    service.poll(5);
                    service.publish("tick", {{"n", ticks.fetch_add(1)}});
                }
            });
        return ok;
    }
    ~Running() {
        run = false;
        if (thread.joinable())
            thread.join();
    }
};

} // namespace

TEST(service_answers_requests_and_sends_events) {
    const std::string path = tempSocket();
    Running r;
    std::string err;
    CHECK_MSG(r.start(path, &err), "%s", err.c_str());

    ServiceClient c;
    CHECK_MSG(c.connect(path, &err), "%s", err.c_str());
    Json reply;
    CHECK_MSG(c.request({{"cmd", "echo"}, {"text", "Wah = Auto"}}, reply, 2000, &err), "%s", err.c_str());
    CHECK(reply.value("ok", false) && reply.value("text", "") == "Wah = Auto");
    CHECK(c.request({{"cmd", "nope"}}, reply, 2000, &err) && !reply.value("ok", true));
    CHECK(reply.value("error", "").find("unknown command 'nope'") != std::string::npos);
    CHECK(c.request({{"cmd", "fail"}}, reply, 2000, &err) && !reply.value("ok", true) && reply.value("error", "") == "it failed");
    CHECK(c.request({{"text", "no cmd"}}, reply, 2000, &err) && !reply.value("ok", true));

    // No events until subscribed; then they keep coming.
    CHECK(c.request({{"cmd", "subscribe"}, {"events", {"tick"}}}, reply, 2000, &err) && reply.value("ok", false));
    Json ev;
    CHECK_MSG(c.next(ev, 2000, &err), "%s", err.c_str());
    CHECK(ev.value("event", "") == "tick" && ev.contains("n"));
    const int first = ev.value("n", -1);
    CHECK(c.next(ev, 2000, &err) && ev.value("n", -1) > first);
    // A reply still finds its way among the events.
    CHECK(c.request({{"cmd", "echo"}, {"text", "x"}}, reply, 2000, &err) && reply.value("text", "") == "x");

    // A second client at the same time.
    ServiceClient d;
    CHECK(d.connect(path, &err) && d.request({{"cmd", "echo"}, {"text", "y"}}, reply, 2000, &err) && reply.value("text", "") == "y");
}

TEST(service_refuses_a_second_cue_and_replaces_a_dead_ones_socket) {
    const std::string path = tempSocket();
    std::string err;
    {
        Running r;
        CHECK_MSG(r.start(path, &err), "%s", err.c_str());
        Service other;
        CHECK(!other.listen(path, nullptr, &err) && err.find("another Cue is running") != std::string::npos);
    }
    // A socket file nobody answers on, as a crashed Cue leaves it.
    {
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
        CHECK(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
        close(fd);
    }
    Running r;
    CHECK_MSG(r.start(path, &err), "%s", err.c_str());
    ServiceClient c;
    CHECK(c.connect(path, &err));

    ServiceClient none;
    CHECK(!none.connect(path + ".missing", &err) && err.find("no Cue is running") != std::string::npos);
}

TEST(service_drops_a_client_that_sends_garbage) {
    const std::string path = tempSocket();
    Running r;
    std::string err;
    CHECK_MSG(r.start(path, &err), "%s", err.c_str());

    // Not JSON: an error reply, and the client stays.
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
    CHECK(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    const char notJson[] = {0, 0, 0, 3, 'a', 'b', 'c'};
    CHECK(write(fd, notJson, sizeof notJson) == static_cast<ssize_t>(sizeof notJson));
    char buf[256];
    const ssize_t n = read(fd, buf, sizeof buf);
    CHECK(n > 4 && std::string(buf + 4, static_cast<size_t>(n - 4)).find("not JSON") != std::string::npos);

    // Longer than any message may be: the connection is closed.
    const unsigned char huge[] = {0x7f, 0xff, 0xff, 0xff};
    CHECK(write(fd, huge, sizeof huge) == 4);
    CHECK(read(fd, buf, sizeof buf) == 0);
    close(fd);

    ServiceClient c;
    Json reply;
    CHECK(c.connect(path, &err) && c.request({{"cmd", "echo"}, {"text", "still here"}}, reply, 2000, &err));
}
