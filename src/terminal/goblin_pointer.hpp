// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam DePrince
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to
// do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// Goblin pointer protocol v1. Keep the copies in Inkline, Skiff and View identical.
#ifndef GOBLIN_POINTER_HPP
#define GOBLIN_POINTER_HPP
#include <array>
#include <charconv>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace goblin { namespace pointer {
constexpr std::string_view prefix = "777;goblin-pointer;";
constexpr size_t max_frame = 768;
constexpr int unit = 256; // signed, zero-based coordinates in 1/256 pixel
enum Kind { Pen = 1, Touch = 2, Gesture = 3 };
enum Phase { Hover = 0, Down = 1, Move = 2, Up = 3, Cancel = 4 };
enum Valid { Pressure = 1, Tilt = 2, Twist = 4, Tangent = 8, Z = 16 };
enum class Op { Invalid, Mode, Query, Cap, Event };
struct Message {
    Op op = Op::Invalid;
    int mode = 0, id = 0, caps = 7, w = 0, h = 0, cw = 0, ch = 0;
    int kind = Pen, phase = Hover, device = 0, contact = 0, peer = 0;
    int x = 0, y = 0, buttons = 0, mods = 0, tool = 1, valid = 0;
    int p = 0, tx = 0, ty = 0, twist = 0, tangent = 0, z = 0;
    int dx = 0, dy = 0, scale = 10000, angle = 0;
};
inline bool parse(std::string_view body, Message &out) {
    if (body.size() > max_frame) return false;
    if (body.substr(0, 2) == "\033]") {
        body.remove_prefix(2);
        if (body.size() >= 2 && body.substr(body.size()-2) == "\033\\") body.remove_suffix(2);
        else if (!body.empty() && body.back() == '\a') body.remove_suffix(1);
        else return false;
    }
    if (body.substr(0, prefix.size()) != prefix) return false;
    body.remove_prefix(prefix.size());
    Message m;
    int version = 0;
    struct Field { std::string_view name; int *value; int lo; int hi; };
    constexpr int coord = 0x3fffffff;
    const std::array<Field, 30> fields{{
        {"v", &version, 1, 1}, {"mode", &m.mode, 0, 1}, {"id", &m.id, 0, 0x7fffffff},
        {"caps", &m.caps, 0, 7}, {"w", &m.w, 1, 65535}, {"h", &m.h, 1, 65535},
        {"cw", &m.cw, 1, 4096}, {"ch", &m.ch, 1, 4096},
        {"kind", &m.kind, Pen, Gesture}, {"phase", &m.phase, Hover, Cancel},
        {"device", &m.device, 0, 0x7fffffff}, {"contact", &m.contact, 0, 0x7fffffff},
        {"peer", &m.peer, 0, 0x7fffffff}, {"x", &m.x, -coord, coord}, {"y", &m.y, -coord, coord},
        {"buttons", &m.buttons, 0, 31}, {"mods", &m.mods, 0, 15}, {"tool", &m.tool, 0, 2},
        {"valid", &m.valid, 0, 31}, {"p", &m.p, 0, 65535},
        {"tx", &m.tx, -9000, 9000}, {"ty", &m.ty, -9000, 9000},
        {"twist", &m.twist, 0, 35999}, {"tangent", &m.tangent, -65535, 65535},
        {"z", &m.z, -1000000, 1000000}, {"dx", &m.dx, -coord, coord}, {"dy", &m.dy, -coord, coord},
        {"scale", &m.scale, 1, 1000000}, {"angle", &m.angle, -36000, 36000},
        {"reserved", nullptr, 0, 0}
    }};
    uint32_t seen = 0;
    bool have_op = false;
    while (!body.empty()) {
        const auto sep = body.find(':');
        const auto item = body.substr(0, sep);
        const auto eq = item.find('=');
        if (eq == std::string_view::npos || eq == 0 || eq+1 == item.size()) return false;
        const auto name = item.substr(0, eq), value = item.substr(eq+1);
        if (name == "op") {
            if (have_op) return false;
            have_op = true;
            if (value == "mode") m.op = Op::Mode;
            else if (value == "query") m.op = Op::Query;
            else if (value == "cap") m.op = Op::Cap;
            else if (value == "event") m.op = Op::Event;
            else return false;
        } else {
            size_t k = 0;
            for (; k < fields.size(); ++k) if (fields[k].name == name) break;
            if (k == fields.size() || !fields[k].value || (seen & (1u << k))) return false;
            const auto result = std::from_chars(value.data(), value.data()+value.size(), *fields[k].value);
            if (result.ec != std::errc() || result.ptr != value.data()+value.size()
                || *fields[k].value < fields[k].lo || *fields[k].value > fields[k].hi) return false;
            seen |= 1u << k;
        }
        if (sep == std::string_view::npos) break;
        body.remove_prefix(sep+1);
        if (body.empty()) return false;
    }
    if (version != 1 || !have_op) return false;
    const auto has = [&](unsigned bit) { return (seen & (1u << bit)) != 0; };
    if (m.op == Op::Mode && !has(1)) return false;
    if (m.op == Op::Query && !has(2)) return false;
    if (m.op == Op::Cap && (seen & 0xfcu) != 0xfcu) return false;
    if (m.op == Op::Event) {
        // All routing and lifecycle fields are mandatory; measurements are optional.
        constexpr uint32_t required = (1u<<8)|(1u<<9)|(1u<<10)|(1u<<11)|(1u<<13)|(1u<<14);
        if ((seen & required) != required) return false;
        if ((m.valid & Pressure) && !has(19)) return false;
        if ((m.valid & Tilt) && (!has(20) || !has(21))) return false;
        if ((m.valid & Twist) && !has(22)) return false;
        if ((m.valid & Tangent) && !has(23)) return false;
        if ((m.valid & Z) && !has(24)) return false;
        if (m.kind == Gesture && (!has(12) || !has(25) || !has(26) || !has(27) || !has(28))) return false;
    }
    out = m;
    return true;
}
inline std::string encode(const Message &m) {
    std::string s = "\033]" + std::string(prefix) + "v=1:op=";
    const auto add = [&](std::string_view key, int value) { s += ':'; s += key; s += '='; s += std::to_string(value); };
    switch (m.op) {
    case Op::Mode: s += "mode"; add("mode", m.mode); break;
    case Op::Query: s += "query"; add("id", m.id); break;
    case Op::Cap:
        s += "cap"; add("id", m.id); add("caps", m.caps);
        add("w", m.w); add("h", m.h); add("cw", m.cw); add("ch", m.ch); break;
    case Op::Event:
        s += "event"; add("kind", m.kind); add("phase", m.phase);
        add("device", m.device); add("contact", m.contact); add("x", m.x); add("y", m.y);
        add("buttons", m.buttons); add("mods", m.mods); add("tool", m.tool); add("valid", m.valid);
        if (m.valid & Pressure) add("p", m.p);
        if (m.valid & Tilt) { add("tx", m.tx); add("ty", m.ty); }
        if (m.valid & Twist) add("twist", m.twist);
        if (m.valid & Tangent) add("tangent", m.tangent);
        if (m.valid & Z) add("z", m.z);
        if (m.kind == Gesture) { add("peer", m.peer); add("dx", m.dx); add("dy", m.dy); add("scale", m.scale); add("angle", m.angle); }
        break;
    default: return {};
    }
    return s + "\033\\";
}
inline std::string mode(bool enabled) { Message m; m.op=Op::Mode; m.mode=enabled; return encode(m); }

// Incremental input demultiplexer. Ordinary input retains its exact order.
// A matching frame is bounded, validated, and delivered atomically, never as keys.
class InputFilter {
    std::string pending_;
    std::string paste_marker_;
    bool paste_ = false;
    bool matched_ = false, discard_ = false, escape_ = false;
public:
    bool pending() const { return !pending_.empty() || matched_; }
    template<class Plain, class Frame>
    void feed(std::string_view bytes, Plain plain, Frame frame) {
        const std::string start = "\033]" + std::string(prefix);
        std::string ordinary;
        const auto flush = [&] { if (!ordinary.empty()) { plain(ordinary); ordinary.clear(); } };
        for (const char c : bytes) {
            // Bracketed paste is literal data, even if it contains an OSC
            // that resembles a report. Track its delimiters across reads.
            if (!matched_) {
                paste_marker_ += c;
                if (paste_marker_.size() > 6) paste_marker_.erase(0, 1);
                const bool was_paste = paste_;
                if (paste_marker_ == "\033[200~") paste_ = true;
                else if (paste_marker_ == "\033[201~") paste_ = false;
                if (was_paste || paste_) { ordinary += pending_; pending_.clear(); ordinary += c; continue; }
            }
            if (!matched_) {
                if (pending_.empty() && c != '\033') { ordinary += c; continue; }
                pending_ += c;
                if (start.compare(0, pending_.size(), pending_) != 0) {
                    // Preserve a second ESC as the start of a possible next frame.
                    if (c == '\033' && pending_.size() > 1) {
                        ordinary.append(pending_, 0, pending_.size()-1); pending_ = "\033";
                    } else { ordinary += pending_; pending_.clear(); }
                } else if (pending_.size() == start.size()) { flush(); matched_ = true; }
                continue;
            }
            if (!discard_) {
                pending_ += c;
                if (pending_.size() > max_frame) { pending_.clear(); discard_ = true; }
            }
            const bool end = c == '\a' || (escape_ && c == '\\');
            const bool cancel = c == '\030' || c == '\032' || (escape_ && c != '\\');
            if (end || cancel) {
                Message message;
                if (end && !discard_ && parse(pending_, message)) { flush(); frame(message, pending_); }
                pending_.clear(); matched_ = discard_ = escape_ = false;
                if (cancel && c != '\030' && c != '\032') { ordinary += '\033'; ordinary += c; }
            } else escape_ = c == '\033';
        }
        flush();
    }
    // Only a prefix (e.g. a lone Escape key) expires. Never leak a partial
    // report into shell input. The caller uses its existing escape-key timer.
    template<class Plain> void expire(Plain plain) {
        if (!matched_ && !pending_.empty()) { plain(pending_); pending_.clear(); }
    }
};
} }
#endif
