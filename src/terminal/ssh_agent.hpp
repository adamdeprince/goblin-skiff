/* Copyright (c) 2026 Adam DePrince. SPDX-License-Identifier: ISC
 * Terminal SSH agent transport. Kept identical in Silkie, Odin Seat and Skiff.
 * Packet storage grows to the SSH protocol's uint32 length; read buffers below
 * are streaming scratch space, not message-size or connection-count limits.
 */
#pragma once
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <map>
#include <set>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace terminal_agent {
using Fields = std::map<std::string, std::string>;
inline uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline uint64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
inline std::string nonce() {
    // 128 bits of instance identity, independent of request/connection capacity.
    unsigned char bytes[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("agent identity entropy unavailable");
    size_t have = 0;
    while (have < sizeof bytes) {
        ssize_t n = read(fd, bytes + have, sizeof bytes - have);
        if (n > 0) have += size_t(n);
        else if (n < 0 && errno == EINTR) continue;
        else { close(fd); throw std::runtime_error("agent identity entropy read failed"); }
    }
    close(fd);
    std::string out;
    for (auto c : bytes) { out += "0123456789abcdef"[c >> 4]; out += "0123456789abcdef"[c & 15]; }
    return out;
}
inline std::string get(const Fields &f, const std::string &key) {
    auto i = f.find(key); return i == f.end() ? "" : i->second;
}
inline bool token(const std::string &s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    return true;
}
inline bool parse(std::string raw, Fields &out) {
    out.clear();
    if (raw.starts_with("\033]")) raw.erase(0, 2);
    if (raw.ends_with("\033\\")) raw.resize(raw.size() - 2);
    else if (raw.ends_with("\007")) raw.pop_back();
    const std::string prefix = "777;ssh-agent;";
    if (!raw.starts_with(prefix)) return false;
    raw.erase(0, prefix.size());
    size_t start = 0;
    for (;;) {
        auto end = raw.find(':', start);
        auto part = raw.substr(start, end == raw.npos ? raw.npos : end - start);
        auto eq = part.find('=');
        if (eq == part.npos || !eq) return false;
        for (unsigned char c : part) if (c < 32 || c > 126) return false;
        if (!out.emplace(part.substr(0, eq), part.substr(eq + 1)).second) return false;
        if (end == raw.npos) break;
        start = end + 1;
    }
    return get(out, "v") == "1" && token(get(out, "id")) && !get(out, "op").empty();
}
inline std::string frame(const Fields &f) {
    std::string out = "\033]777;ssh-agent;";
    for (const auto &[k, v] : f) { if (out.back() != ';') out += ':'; out += k + '=' + v; }
    return out + "\033\\";
}
inline std::string base64(const std::string &bytes) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned bits = 0, nbits = 0;
    for (unsigned char c : bytes) {
        bits = (bits << 8) | c; nbits += 8;
        while (nbits >= 6) { nbits -= 6; out += alphabet[(bits >> nbits) & 63]; }
    }
    if (nbits) out += alphabet[(bits << (6 - nbits)) & 63];
    while (out.size() % 4) out += '=';
    return out;
}
inline bool unbase64(const std::string &text, std::string &out) {
    out.clear();
    if (text.size() % 4) return false;
    unsigned bits = 0, nbits = 0;
    bool padding = false;
    for (unsigned char c : text) {
        if (c == '=') { padding = true; continue; }
        if (padding) return false;
        unsigned v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else return false;
        bits = (bits << 6) | v; nbits += 6;
        if (nbits >= 8) { nbits -= 8; out += char((bits >> nbits) & 255); }
    }
    return base64(out) == text; // canonical padding and zero trailing bits
}
inline uint32_t packet_length(const std::string &s) {
    uint32_t n = 0;
    for (size_t i = 0; i < 4; ++i) n = (n << 8) | (unsigned char)s[i];
    return n;
}
inline bool packet(const std::string &s) {
    return s.size() >= 5 && uint64_t(packet_length(s)) + 4 == s.size();
}
inline std::string failure() { return std::string("\0\0\0\1\5", 5); }
inline bool permitted(const std::string &s) {
    if (!packet(s)) return false;
    unsigned type = (unsigned char)s[4];
    if (type == 11 || type == 13) return true; // identities and signing
    if (type != 27 || s.size() < 9) return false;
    std::string name_length = s.substr(5, 4);
    uint32_t n = packet_length(name_length);
    if (uint64_t(n) + 9 > s.size()) return false;
    auto name = s.substr(9, n);
    return name == "session-bind@openssh.com" || name == "query";
}
inline Fields error(const Fields &request, const std::string &reason) {
    Fields f{{"v", "1"}, {"op", get(request, "op") == "query" ? "cap" : "reply"},
             {"id", get(request, "id")}, {"error", reason}};
    if (f["op"] == "cap") f["caps"] = "0";
    else { f["c"] = get(request, "c"); f["g"] = get(request, "g"); f["data"] = base64(failure()); }
    return f;
}
inline void nonblocking(int fd) {
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) throw std::runtime_error("agent socket flags failed");
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) < 0)
        throw std::runtime_error("agent socket SIGPIPE protection failed");
#endif
}
inline ssize_t send_bytes(int fd, const std::string &bytes) {
#ifdef MSG_NOSIGNAL
    return send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
#else
    return send(fd, bytes.data(), bytes.size(), 0);
#endif
}

// A stable, private SSH_AUTH_SOCK. Every packet first queries the current
// terminal, then names its generation in the request. Connections stay pinned
// to that generation so session-bind constraints cannot silently be discarded.
class Proxy {
    struct Connection {
        std::string c = nonce(), generation, input, output, request, id;
        bool querying = false, closing = false;
        uint64_t deadline = 0;
    };
    int listener_ = -1;
    std::string scope_ = nonce();
    std::string directory_, path_;
    std::map<int, Connection> connections_;
    std::vector<std::string> outgoing_;
    uint64_t timeout_;
    void end(int fd) {
        auto i = connections_.find(fd);
        if (i == connections_.end()) return;
        const auto &c = i->second;
        if (!c.generation.empty()) outgoing_.push_back(frame({{"v","1"},{"op","close"},
            {"id",nonce()},{"c",c.c},{"g",c.generation}}));
        close(fd); connections_.erase(i);
    }
    void fail(Connection &c, bool disconnect) {
        c.output += failure(); c.request.clear(); c.id.clear(); c.deadline = 0;
        c.closing = disconnect;
    }
    void next(Connection &c, uint64_t now) {
        if (!c.id.empty() || c.closing || c.input.size() < 4) return;
        uint64_t n = uint64_t(packet_length(c.input)) + 4;
        if (n == 4) { fail(c, true); return; }
        if (n > c.input.size()) return;
        c.request = c.input.substr(0, size_t(n)); c.input.erase(0, size_t(n));
        if (!permitted(c.request)) { fail(c, false); return; }
        c.id = scope_ + '.' + nonce(); c.querying = true; c.deadline = now + std::min<uint64_t>(timeout_, 5000);
        outgoing_.push_back(frame({{"v","1"},{"op","query"},{"id",c.id}}));
    }
public:
    explicit Proxy(uint64_t timeout_ms = 120000) : timeout_(timeout_ms) {
        char name[] = "/tmp/terminal-agent.XXXXXX";
        auto dir = mkdtemp(name);
        if (!dir) throw std::runtime_error("cannot create private agent directory");
        directory_ = dir; path_ = directory_ + "/socket";
        try {
            listener_ = socket(AF_UNIX, SOCK_STREAM, 0);
            if (listener_ < 0) throw std::runtime_error("cannot create agent listener");
            nonblocking(listener_);
            sockaddr_un address{}; address.sun_family = AF_UNIX;
            if (path_.size() >= sizeof address.sun_path) throw std::length_error("agent path exceeds AF_UNIX");
            memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
            if (bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof address) < 0 ||
                chmod(path_.c_str(), 0600) < 0 || listen(listener_, SOMAXCONN) < 0)
                throw std::runtime_error("cannot bind private agent listener");
        } catch (...) { if (listener_ >= 0) close(listener_); unlink(path_.c_str()); rmdir(directory_.c_str()); throw; }
    }
    Proxy(int listener, std::string path, uint64_t timeout_ms = 120000)
        : listener_(listener), directory_(path.substr(0, path.rfind('/'))), path_(std::move(path)), timeout_(timeout_ms) {
        nonblocking(listener_);
    }
    Proxy(const Proxy &) = delete;
    Proxy &operator=(const Proxy &) = delete;
    ~Proxy() {
        for (const auto &[fd, c] : connections_) close(fd);
        if (listener_ >= 0) close(listener_);
        unlink(path_.c_str()); rmdir(directory_.c_str());
    }
    const std::string &path() const { return path_; }
    int listener() const { return listener_; }
    void cancel() {
        outgoing_.clear();
        for (auto &[fd, c] : connections_) {
            if (!c.generation.empty()) outgoing_.push_back(frame({{"v","1"},{"op","close"},
                {"id",nonce()},{"c",c.c},{"g",c.generation}}));
            c.generation.clear();
            c.output.clear();
            fail(c, true);
        }
    }
    std::vector<pollfd> pollfds() const {
        std::vector<pollfd> out{{listener_, POLLIN, 0}};
        for (const auto &[fd, c] : connections_) {
            short events = c.id.empty() && !c.closing ? POLLIN : 0;
            if (!c.output.empty()) events |= POLLOUT;
            out.push_back({fd, events, 0});
        }
        return out;
    }
    int wait_ms(uint64_t now) const {
        int delay = -1;
        for (const auto &[fd, c] : connections_) {
            if (!c.output.empty()) delay = 10;
            if (c.id.empty() && c.input.size() >= 4 &&
                uint64_t(packet_length(c.input)) + 4 <= c.input.size()) return 0;
            if (c.deadline) delay = std::min(delay < 0 ? INT32_MAX : delay,
                int(std::min<uint64_t>(INT32_MAX, c.deadline > now ? c.deadline - now : 0)));
        }
        return delay;
    }
    // May be called after any wakeup; never blocks, including on a slow agent
    // caller that has not read its last reply yet.
    void tick(uint64_t now) {
        auto fds = pollfds();
        if (static_cast<size_t>(static_cast<nfds_t>(fds.size())) != fds.size())
            throw std::length_error("agent poll descriptor count overflow");
        if (poll(fds.data(), static_cast<nfds_t>(fds.size()), 0) < 0) {
            if (errno == EINTR) return;
            throw std::runtime_error("agent socket poll failed");
        }
        if (fds[0].revents & POLLIN) {
            for (;;) {
                int fd = accept(listener_, nullptr, nullptr);
                if (fd < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    throw std::runtime_error("agent accept failed");
                }
                try { nonblocking(fd); connections_.try_emplace(fd); }
                catch (...) { close(fd); throw; }
            }
        }
        for (size_t index = 1; index < fds.size(); ++index) {
            int fd = fds[index].fd;
            auto i = connections_.find(fd);
            if (i == connections_.end()) continue;
            auto &c = i->second;
            if (fds[index].revents & (POLLERR | POLLNVAL | POLLHUP)) { end(fd); continue; }
            if (c.deadline && now >= c.deadline) fail(c, true);
            if ((fds[index].revents & (POLLIN | POLLHUP)) && c.id.empty() && !c.closing) {
                char scratch[8192];
                ssize_t n = read(fd, scratch, sizeof scratch);
                if (n > 0) c.input.append(scratch, size_t(n));
                else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { end(fd); continue; }
            }
            next(c, now);
            if (!c.output.empty()) {
                ssize_t n = send_bytes(fd, c.output);
                if (n > 0) c.output.erase(0, size_t(n));
                else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { end(fd); continue; }
            }
            if (c.closing && c.output.empty()) end(fd);
        }
    }
    bool receive(const std::string &raw, uint64_t now) {
        Fields f;
        if (!parse(raw, f)) return false;
        auto id = get(f, "id");
        for (auto &[fd, c] : connections_) {
            if (c.id != id) continue;
            if (c.querying && get(f, "op") == "cap") {
                auto generation = get(f, "g");
                if (get(f, "caps") != "1" || !token(generation)) { fail(c, false); return true; }
                if (!c.generation.empty() && c.generation != generation) { fail(c, true); return true; }
                c.generation = generation; c.querying = false; c.id = scope_ + '.' + nonce();
                c.deadline = now + timeout_;
                outgoing_.push_back(frame({{"v","1"},{"op","request"},{"id",c.id},
                    {"c",c.c},{"g",c.generation},{"expires",std::to_string(wall_ms() + timeout_)},
                    {"data",base64(c.request)}}));
            } else if (!c.querying && get(f, "op") == "reply") {
                std::string bytes;
                if (get(f, "c") != c.c || get(f, "g") != c.generation ||
                    !unbase64(get(f, "data"), bytes) || !packet(bytes)) { fail(c, true); return true; }
                c.output += bytes; c.request.clear(); c.id.clear(); c.deadline = 0;
                if (!get(f,"error").empty()) c.closing = true;
            }
            return true;
        }
        return id.starts_with(scope_ + '.'); // swallow late replies, never type them into a shell
    }
    std::vector<std::string> take_output() { std::vector<std::string> out; out.swap(outgoing_); return out; }
};

// Skiff's explicit -A fallback when the terminal has no provider. Keep each
// local agent connection alive, including its OpenSSH session bindings.
class LocalAgent {
    struct Connection {
        int fd = -1;
        Fields pending;
        std::string input, output;
        std::set<std::string> used;
        bool connecting = false;
        uint64_t deadline = 0;
    };
    std::string path_, identity_, generation_ = nonce();
    std::map<std::string, Connection> connections_;
    std::set<std::string> closed_;
    std::vector<std::string> output_;
    void reply(const Fields &f, const std::string &data, const std::string &reason = {}) {
        Fields response{{"v","1"},{"op","reply"},{"id",get(f,"id")},
            {"c",get(f,"c")},{"g",get(f,"g")},{"data",base64(data)}};
        if (!reason.empty()) response["error"] = reason;
        output_.push_back(frame(response));
    }
    void end(const std::string &channel) {
        auto i = connections_.find(channel);
        if (i != connections_.end()) { if (i->second.fd >= 0) close(i->second.fd); connections_.erase(i); }
        closed_.insert(channel);
    }
    bool refresh() {
        const char *env = getenv("SSH_AUTH_SOCK");
        struct stat info{};
        if (!env || !*env || stat(env,&info) < 0) return false;
        std::string identity = std::string(env) + ':' + std::to_string(info.st_dev) + ':' + std::to_string(info.st_ino);
        if (!identity_.empty() && identity_ != identity) {
            for (auto &[channel,c] : connections_) {
                if (!c.pending.empty()) reply(c.pending,failure(),"changed");
                if (c.fd >= 0) close(c.fd);
            }
            connections_.clear(); closed_.clear(); generation_ = nonce();
        }
        identity_ = identity; path_ = env;
        return true;
    }
public:
    LocalAgent() = default;
    LocalAgent(const LocalAgent &) = delete;
    ~LocalAgent() { for (auto &[channel,c] : connections_) if (c.fd >= 0) close(c.fd); }
    void handle(const Fields &f) {
        auto op = get(f,"op"), channel = get(f,"c");
        if (op == "close") { if (get(f,"g") == generation_ && token(channel)) end(channel); return; }
        bool available = refresh();
        if (op == "query") {
            Fields cap{{"v","1"},{"op","cap"},{"id",get(f,"id")},{"caps",available ? "1" : "0"}};
            if (available) cap["g"] = generation_;
            output_.push_back(frame(cap)); return;
        }
        if (op != "request") return;
        if (!available || get(f,"g") != generation_) { reply(f,failure(),available ? "changed" : "unavailable"); return; }
        std::string bytes, expiry = get(f,"expires"); uint64_t deadline = 0;
        auto [tail, code] = std::from_chars(expiry.data(),expiry.data()+expiry.size(),deadline);
        if (code != std::errc() || tail != expiry.data()+expiry.size() || deadline <= wall_ms()) {
            reply(f,failure(),"expired"); return;
        }
        if (!token(channel) || !unbase64(get(f,"data"),bytes) || !permitted(bytes)) { reply(f,failure(),"denied"); return; }
        if (closed_.count(channel)) { reply(f,failure(),"closed"); return; }
        auto &c = connections_[channel];
        auto id = get(f,"id");
        if (!c.used.insert(id).second || !c.pending.empty()) { reply(f,failure(),"duplicate"); return; }
        if (c.fd < 0) {
            c.fd = socket(AF_UNIX,SOCK_STREAM,0);
            if (c.fd < 0) { reply(f,failure(),"unavailable"); end(channel); return; }
            nonblocking(c.fd);
            sockaddr_un address{}; address.sun_family = AF_UNIX;
            if (path_.size() >= sizeof address.sun_path) { reply(f,failure(),"unavailable"); end(channel); return; }
            memcpy(address.sun_path,path_.c_str(),path_.size()+1);
            int result = connect(c.fd,reinterpret_cast<sockaddr *>(&address),sizeof address);
            if (result < 0 && errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
                reply(f,failure(),"unavailable"); end(channel); return;
            }
            c.connecting = result < 0;
        }
        c.pending=f; c.output=std::move(bytes); c.deadline=deadline;
    }
    std::vector<pollfd> pollfds() const {
        std::vector<pollfd> out;
        for (const auto &[channel,c] : connections_)
            if (!c.pending.empty()) out.push_back({c.fd,short(c.output.empty() ? POLLIN : POLLOUT),0});
        return out;
    }
    int wait_ms() const {
        int result=-1;
        for (const auto &[channel,c] : connections_) if (!c.pending.empty()) {
            if (!c.output.empty()) return 10; // Skiff's outer select watches reads
            uint64_t now=wall_ms();
            result=std::min(result < 0 ? INT32_MAX : result,
                int(std::min<uint64_t>(INT32_MAX,c.deadline > now ? c.deadline-now : 0)));
        }
        return result;
    }
    void tick() {
        auto fds=pollfds();
        if (static_cast<size_t>(static_cast<nfds_t>(fds.size())) != fds.size())
            throw std::length_error("agent descriptor count overflow");
        if (poll(fds.data(),static_cast<nfds_t>(fds.size()),0) < 0) {
            if (errno == EINTR) return;
            throw std::runtime_error("local agent poll failed");
        }
        std::vector<std::string> dead;
        for (auto &[channel,c] : connections_) {
            if (c.pending.empty()) continue;
            if (wall_ms() >= c.deadline) { reply(c.pending,failure(),"timeout"); dead.push_back(channel); continue; }
            short ready=0;
            for (const auto &fd : fds) if (fd.fd==c.fd) ready=fd.revents;
            if (!ready) continue;
            bool failed=ready & (POLLERR|POLLNVAL|POLLHUP);
            if (!failed && c.connecting) {
                int error=0; socklen_t size=sizeof error;
                failed=getsockopt(c.fd,SOL_SOCKET,SO_ERROR,&error,&size)<0 || error;
                c.connecting=false;
            }
            if (!failed && (ready&POLLOUT) && !c.output.empty()) {
                ssize_t n=send_bytes(c.fd,c.output);
                if (n>0) c.output.erase(0,size_t(n));
                else if (n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) failed=true;
            }
            if (!failed && (ready&POLLIN)) {
                char scratch[8192]; ssize_t n=read(c.fd,scratch,sizeof scratch);
                if (n>0) c.input.append(scratch,size_t(n));
                else if (!n || (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)) failed=true;
                if (c.input.size()>=4 && uint64_t(packet_length(c.input))+4<=c.input.size()) {
                    if (packet(c.input)) { reply(c.pending,c.input); c.pending.clear(); c.input.clear(); }
                    else failed=true;
                }
            }
            if (failed) { reply(c.pending,failure(),"unavailable"); dead.push_back(channel); }
        }
        for (const auto &channel : dead) end(channel);
    }
    bool owns(const std::string &generation) const { return generation == generation_; }
    std::vector<std::string> take_output() { std::vector<std::string> out; out.swap(output_); return out; }
};

// Client-side relay shared by Odin Seat and Skiff. Native terminal access is
// preferred; an explicit -A authorizes the local SSH_AUTH_SOCK fallback.
class Client {
    bool allow_local_ = false;
    LocalAgent local_;
    std::string native_generation_;
    std::map<std::string,std::pair<Fields,uint64_t>> queries_;
    std::vector<std::string> terminal_, remote_;
public:
    explicit Client(bool allow_local = false) : allow_local_(allow_local) {}
    void allow_local(bool value) { allow_local_ = value; }
    bool from_remote(const std::string &raw) {
        Fields f;
        if (!parse(raw,f)) return false;
        auto op=get(f,"op"), generation=get(f,"g");
        if (op=="query") {
            queries_[get(f,"id")]={f,now_ms()+1000}; terminal_.push_back(raw);
        } else if (allow_local_ && local_.owns(generation)) local_.handle(f);
        else if (op=="close" || (op=="request" && !native_generation_.empty() && generation==native_generation_))
            terminal_.push_back(raw);
        else if (op=="request") remote_.push_back(frame(error(f,"changed")));
        return true;
    }
    bool from_terminal(const std::string &raw) {
        Fields f;
        if (!parse(raw,f)) return false;
        auto op=get(f,"op");
        if (op=="cap") {
            auto i=queries_.find(get(f,"id"));
            if (i==queries_.end()) return true; // expired or duplicate capability reply
            if (get(f,"caps")=="1" && token(get(f,"g"))) native_generation_=get(f,"g");
            else if (allow_local_) { local_.handle(i->second.first); queries_.erase(i); return true; }
            queries_.erase(i);
        }
        if (op=="cap" || op=="reply") remote_.push_back(raw);
        return true;
    }
    void tick() {
        auto now=now_ms();
        for (auto i=queries_.begin(); i!=queries_.end();) {
            if (now<i->second.second) { ++i; continue; }
            if (allow_local_) local_.handle(i->second.first);
            else remote_.push_back(frame(error(i->second.first,"unavailable")));
            i=queries_.erase(i);
        }
        local_.tick();
        auto replies=local_.take_output();
        remote_.insert(remote_.end(),std::make_move_iterator(replies.begin()),std::make_move_iterator(replies.end()));
    }
    std::vector<pollfd> pollfds() const { return local_.pollfds(); }
    int wait_ms() const {
        int delay=local_.wait_ms(); auto now=now_ms();
        for (const auto &[id,q] : queries_) {
            auto remaining=q.second>now ? q.second-now : 0;
            delay=std::min(delay<0 ? INT32_MAX : delay,int(std::min<uint64_t>(INT32_MAX,remaining)));
        }
        if (!terminal_.empty() || !remote_.empty()) return 0;
        return delay;
    }
    std::vector<std::string> take_terminal() { std::vector<std::string> out; out.swap(terminal_); return out; }
    std::vector<std::string> take_remote() { std::vector<std::string> out; out.swap(remote_); return out; }
};
} // namespace terminal_agent
