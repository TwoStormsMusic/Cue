#include "app/service.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace cue {

namespace {

// A client that does not read its events is dropped past this much.
constexpr size_t kMaxQueued = 8u << 20;

bool socketAddress(const std::string& path, sockaddr_un& addr, std::string* error) {
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) {
        *error = "socket path too long: " + path;
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

std::string frame(const Json& message) {
    const std::string body = message.dump();
    const uint32_t n = static_cast<uint32_t>(body.size());
    std::string out(4, '\0');
    out[0] = static_cast<char>(n >> 24);
    out[1] = static_cast<char>(n >> 16);
    out[2] = static_cast<char>(n >> 8);
    out[3] = static_cast<char>(n);
    return out + body;
}

// Takes one whole message off the front of buf. -1: malformed or too long.
int unframe(std::string& buf, std::string& message) {
    if (buf.size() < 4)
        return 0;
    const auto* b = reinterpret_cast<const unsigned char*>(buf.data());
    const uint32_t n = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
    if (n > kMaxMessage)
        return -1;
    if (buf.size() < 4 + size_t(n))
        return 0;
    message = buf.substr(4, n);
    buf.erase(0, 4 + size_t(n));
    return 1;
}

} // namespace

std::string defaultSocketPath() {
    const char* run = std::getenv("XDG_RUNTIME_DIR");
    if (run && *run)
        return std::string(run) + "/cue/engine.sock";
    return "/tmp/cue-" + std::to_string(getuid()) + "/engine.sock";
}

// ----------------------------------------------------------------- Service

Service::~Service() { close(); }

bool Service::listen(const std::string& path, Handler handler, std::string* error) {
    close();
    sockaddr_un addr;
    if (!socketAddress(path, addr, error))
        return false;
    // Its folder, private to this user.
    const auto slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        const std::string dir = path.substr(0, slash);
        if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
            *error = "cannot create " + dir + ": " + std::strerror(errno);
            return false;
        }
    }
    // Another Cue answering there? Else whatever is there is left over.
    const int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe >= 0) {
        const bool alive = ::connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
        ::close(probe);
        if (alive) {
            *error = "another Cue is running (it answers on " + path + ")";
            return false;
        }
    }
    unlink(path.c_str());
    fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd_ < 0) {
        *error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    const mode_t old = umask(0177);
    const bool bound = bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
    umask(old);
    if (!bound || ::listen(fd_, 8) != 0) {
        *error = "cannot listen on " + path + ": " + std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    path_ = path;
    handler_ = std::move(handler);
    return true;
}

void Service::close() {
    for (auto& c : clients_)
        ::close(c.fd);
    clients_.clear();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        unlink(path_.c_str());
    }
}

void Service::poll(int timeoutMs) {
    if (fd_ < 0) {
        if (timeoutMs > 0)
            usleep(static_cast<useconds_t>(timeoutMs) * 1000);
        return;
    }
    std::vector<pollfd> fds;
    fds.push_back({fd_, POLLIN, 0});
    for (const auto& c : clients_)
        fds.push_back({c.fd, static_cast<short>(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
    if (::poll(fds.data(), fds.size(), timeoutMs) <= 0)
        return;

    std::vector<bool> drop(clients_.size(), false);
    for (size_t i = 0; i < clients_.size(); ++i) {
        Client& c = clients_[i];
        const short ev = fds[i + 1].revents;
        if (ev & POLLIN) {
            char buf[65536];
            for (;;) {
                const ssize_t n = recv(c.fd, buf, sizeof buf, 0);
                if (n > 0) {
                    c.in.append(buf, static_cast<size_t>(n));
                    continue;
                }
                if (n == 0 || (errno != EAGAIN && errno != EINTR))
                    drop[i] = true;
                break;
            }
            std::string message;
            int r;
            while ((r = unframe(c.in, message)) > 0)
                handle(c, message);
            if (r < 0)
                drop[i] = true;
        } else if (ev & (POLLHUP | POLLERR)) {
            drop[i] = true;
        }
        if (!drop[i] && !flush(c))
            drop[i] = true;
    }
    for (size_t i = clients_.size(); i-- > 0;)
        if (drop[i]) {
            ::close(clients_[i].fd);
            clients_.erase(clients_.begin() + static_cast<long>(i));
        }

    if (fds[0].revents & POLLIN)
        for (;;) {
            const int fd = accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd < 0)
                break;
            Client c;
            c.fd = fd;
            clients_.push_back(std::move(c));
        }
}

void Service::handle(Client& c, const std::string& text) {
    Json reply;
    Json request;
    try {
        request = Json::parse(text);
    } catch (const std::exception& e) {
        send(c, {{"ok", false}, {"error", std::string("not JSON: ") + e.what()}});
        return;
    }
    if (!request.is_object()) {
        send(c, {{"ok", false}, {"error", "a request is a JSON object"}});
        return;
    }
    if (request.contains("id"))
        reply["id"] = request["id"];
    reply["ok"] = true;
    try {
        const std::string cmd = request.value("cmd", "");
        if (cmd == "subscribe" || cmd == "unsubscribe") {
            for (const auto& e : request.value("events", Json::array()))
                cmd == "subscribe" ? (void)c.events.insert(e.get<std::string>()) : (void)c.events.erase(e.get<std::string>());
            reply["events"] = Json(std::vector<std::string>(c.events.begin(), c.events.end()));
        } else if (cmd.empty()) {
            reply["ok"] = false;
            reply["error"] = "a request needs a \"cmd\"";
        } else if (handler_) {
            handler_(request, reply);
        }
    } catch (const std::exception& e) {
        reply["ok"] = false;
        reply["error"] = e.what();
    }
    send(c, reply);
}

void Service::send(Client& c, const Json& message) {
    if (c.out.size() > kMaxQueued)
        return; // dropped at the next flush
    c.out += frame(message);
}

bool Service::flush(Client& c) {
    while (!c.out.empty()) {
        const ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
        if (n > 0) {
            c.out.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            break;
        return false;
    }
    return c.out.size() <= kMaxQueued;
}

void Service::publish(const std::string& event, Json body) {
    if (!subscribed(event))
        return;
    Json message;
    message["event"] = event;
    for (auto& [k, v] : body.items())
        message[k] = v;
    const std::string bytes = frame(message);
    for (auto& c : clients_)
        if (c.events.count(event) && c.out.size() <= kMaxQueued) {
            c.out += bytes;
            flush(c);
        }
}

bool Service::subscribed(const std::string& event) const {
    for (const auto& c : clients_)
        if (c.events.count(event))
            return true;
    return false;
}

// ----------------------------------------------------------------- client

ServiceClient::~ServiceClient() { close(); }

bool ServiceClient::connect(const std::string& path, std::string* error) {
    close();
    sockaddr_un addr;
    if (!socketAddress(path, addr, error))
        return false;
    fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0 || ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        *error = "no Cue is running (nothing answers on " + path + ")";
        close();
        return false;
    }
    return true;
}

void ServiceClient::close() {
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
    in_.clear();
    pending_.clear();
}

bool ServiceClient::read(Json& message, int timeoutMs, std::string* error) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        std::string text;
        const int r = unframe(in_, text);
        if (r < 0) {
            *error = "Cue sent a message that is too long";
            return false;
        }
        if (r > 0) {
            try {
                message = Json::parse(text);
            } catch (const std::exception& e) {
                *error = std::string("Cue sent something that is not JSON: ") + e.what();
                return false;
            }
            return true;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            *error = "Cue did not answer in time";
            return false;
        }
        pollfd p{fd_, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(left)) <= 0)
            continue;
        char buf[65536];
        const ssize_t n = recv(fd_, buf, sizeof buf, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            *error = "Cue closed the connection";
            return false;
        }
        in_.append(buf, static_cast<size_t>(n));
    }
}

bool ServiceClient::request(Json request, Json& reply, int timeoutMs, std::string* error) {
    if (fd_ < 0) {
        *error = "not connected";
        return false;
    }
    const int id = nextId_++;
    request["id"] = id;
    const std::string bytes = frame(request);
    size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n = ::send(fd_, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            *error = "Cue closed the connection";
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    for (;;) {
        Json m;
        if (!read(m, timeoutMs, error))
            return false;
        if (m.contains("id") && m["id"] == id) {
            reply = std::move(m);
            return true;
        }
        pending_.push_back(std::move(m));
    }
}

bool ServiceClient::next(Json& message, int timeoutMs, std::string* error) {
    if (!pending_.empty()) {
        message = std::move(pending_.front());
        pending_.erase(pending_.begin());
        return true;
    }
    return fd_ >= 0 ? read(message, timeoutMs, error) : (*error = "not connected", false);
}

} // namespace cue
