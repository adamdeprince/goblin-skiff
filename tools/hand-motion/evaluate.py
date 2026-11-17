#!/usr/bin/env python3
"""Evaluate the client's treatment of hand motion on recorded or synthetic strokes.

This mirrors src/terminal/handmotion.cc: the fixed-lag smoother (a Kalman
filter with a constant-acceleration model per axis and a Rauch-Tung-Striebel
pass over the reports held for the lag) and the error-bounded thinning (a
report is merged away while the stroke stays within the tolerance of every
report dropped, each measurement within its own tolerance of the straight
line between the reports kept, and no more than --keep ms of a moving hand
goes unreported). It reports what each stage does to a stroke:
how many points are kept, how far the sent path strays from the raw one, and
what the points cost on the wire as the text they are today.

A recording is what GOBLIN_SKIFF_RECORD_INPUT writes: one line per frame the
client's terminal sent, as `<ms>\\t<kind>\\t<text>` with ESC written \\e.

    GOBLIN_SKIFF_RECORD_INPUT=~/pen.log goblin-skiff host   # draw something
    tools/hand-motion/evaluate.py ~/pen.log
    tools/hand-motion/evaluate.py --synthetic 150,0.05 --lag 15 --tolerance 64

The C++ is the reference; this is for tuning its settings on a real tablet.
"""
import argparse
import math
import random
import re
import shutil
import subprocess
import sys
import zlib

COORD = 1073741823


# ---- reports -----------------------------------------------------------------

def parse_frame(text):
    """The selector and fields of an OSC 777 frame written out with \\e for ESC."""
    m = re.match(r"\\e\]777;([^;]+);(.*?)(\\e\\\\|\\x07)$", text)
    if not m:
        return None, None
    fields = {}
    for item in m.group(2).split(":"):
        key, sep, value = item.partition("=")
        if not sep:
            return None, None
        fields[key] = value
    return m.group(1), fields


def load(path):
    reports = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t", 2)
            if len(parts) != 3:
                continue
            ms, kind, text = parts
            selector, fields = parse_frame(text)
            if selector in ("tablet-pointer", "goblin-pointer") and fields.get("op") == "event" \
                    and fields.get("kind", "1") in ("1", "2") and fields.get("phase", "").isdigit():
                reports.append((int(ms), fields))
    return reports


def synthetic(speed_px_s, noise_px, seconds=2.0, hz=200, f=4.0, seed=2):
    """Cursive-like loops: steady advance with 4 Hz and 8 Hz oscillation, pressure easing."""
    random.seed(seed)
    reports = []
    n = int(seconds * hz)
    for i in range(n):
        t = i / hz
        x = speed_px_s * t + 6 * math.sin(2 * math.pi * f * t)
        y = 10 * math.sin(2 * math.pi * 2 * f * t + 0.7) + 3 * math.sin(2 * math.pi * f * t)
        p = 0.5 + 0.4 * math.sin(2 * math.pi * f * t / 2)
        phase = 1 if i == 0 else 3 if i == n - 1 else 2
        fields = {"op": "event", "kind": "1", "phase": str(phase), "device": "3", "contact": "0",
                  "t": str(1000 + round(t * 1000)), "buttons": "1" if phase != 3 else "0", "mods": "0", "tool": "1",
                  "valid": "3", "x": str(round((x + random.gauss(0, noise_px)) * 256)),
                  "y": str(round((y + random.gauss(0, noise_px)) * 256)),
                  "p": str(max(0, min(65535, round(65535 * (p + random.gauss(0, 0.003)))))),
                  "tx": str(round(random.gauss(600, 50))), "ty": str(round(random.gauss(-300, 50)))}
        reports.append((10000 + round(t * 1000), fields))
    return reports


def text_of(fields):
    return "777;tablet-pointer;" + ":".join(f"{k}={v}" for k, v in fields.items())


# ---- the smoother (fixed-lag RTS, constant acceleration) ------------------------

def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(len(b))) for j in range(len(b[0]))] for i in range(len(a))]


def mat_t(a):
    return [list(r) for r in zip(*a)]


def mat_inv3(m):
    (a, b, c), (d, e, f), (g, h, i) = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if abs(det) < 1e-300:
        return None
    return [[(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
            [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
            [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]]


class Axis:
    """One measured quantity of a contact: position (3 states) with its filter state after the last released point."""

    def __init__(self, noise, drift):
        self.noise, self.drift = noise, drift
        self.have = False

    def start(self, z, t):
        self.x = [z, 0.0, 0.0]
        self.P = [[self.noise ** 2, 0, 0], [0, 1e5, 0], [0, 0, 1e3]]
        self.t = t
        self.have = True

    def expected(self, t):
        dt = t - self.t
        return self.x[0] + self.x[1] * dt + self.x[2] * dt * dt / 2

    def step(self, x, P, dt, z):
        F = [[1, dt, dt * dt / 2], [0, 1, dt], [0, 0, 1]]
        q = self.drift
        Q = [[q * dt ** 5 / 20, q * dt ** 4 / 8, q * dt ** 3 / 6], [q * dt ** 4 / 8, q * dt ** 3 / 3, q * dt ** 2 / 2],
             [q * dt ** 3 / 6, q * dt ** 2 / 2, q * dt]]
        xp = [sum(F[i][k] * x[k] for k in range(3)) for i in range(3)]
        Pp = mat_mul(mat_mul(F, P), mat_t(F))
        Pp = [[Pp[i][j] + Q[i][j] for j in range(3)] for i in range(3)]
        S = Pp[0][0] + self.noise ** 2
        K = [Pp[i][0] / S for i in range(3)]
        y = z - xp[0]
        xf = [xp[i] + K[i] * y for i in range(3)]
        Pf = [[Pp[i][j] - K[i] * Pp[0][j] for j in range(3)] for i in range(3)]
        return xp, Pp, xf, Pf, F

    def smooth(self, zs):
        """The smoothed value of the first of a run of (t, z), the filter advanced through the first alone."""
        x, P, t = self.x, self.P, self.t
        preds, filts = [], []
        after_first = None
        for i, (ti, zi) in enumerate(zs):
            xp, Pp, x, P, F = self.step(x, P, max(0, ti - t), zi)
            preds.append((xp, Pp, F))
            filts.append((x, P))
            t = ti
            if i == 0:
                after_first = (x, P, t)
        xs, Ps = filts[-1]
        for i in range(len(zs) - 2, -1, -1):
            xf, Pf = filts[i]
            xp, Pp, F = preds[i + 1]
            inv = mat_inv3(Pp)
            if inv is None:
                xs, Ps = xf, Pf
                continue
            J = mat_mul(mat_mul(Pf, mat_t(F)), inv)
            dx = [xs[k] - xp[k] for k in range(3)]
            xs = [xf[k] + sum(J[k][m] * dx[m] for m in range(3)) for k in range(3)]
            dP = [[Ps[a][b] - Pp[a][b] for b in range(3)] for a in range(3)]
            Ps = [[Pf[a][b] + mat_mul(mat_mul(J, dP), mat_t(J))[a][b] for b in range(3)] for a in range(3)]
        self.x, self.P, self.t = after_first
        return xs[0]


def smooth_reports(reports, lag_ms, noise, jerk, gap_ms=100, jump_units=16 * 256):
    """Positions smoothed as the client smooths them: each report with the ones that arrive within the lag after it."""
    out = []
    tracks = {}
    for i, (arrived, fields) in enumerate(reports):
        fields = dict(fields)
        phase = int(fields["phase"])
        key = (fields.get("kind", "1"), fields.get("device", "0"), fields.get("contact", "0"))
        if lag_ms and 1 <= phase <= 3 and "x" in fields and "y" in fields and not {"seq", "pred", "est"} & fields.keys():
            run = [(int(fields["t"]), int(fields["x"]), int(fields["y"]))]
            previous = run[0][0]
            for later_arrival, later in reports[i + 1:]:
                if later_arrival - arrived > lag_ms:
                    break
                if (later.get("kind", "1"), later.get("device", "0"), later.get("contact", "0")) != key:
                    continue
                lp = int(later.get("phase", -1))
                if 1 <= lp <= 3 and "x" in later and not {"seq", "pred", "est"} & later.keys():
                    t = max(int(later["t"]), previous + 1)
                    previous = t
                    run.append((t, int(later["x"]), int(later["y"])))
                if lp in (4, 5) or (lp == 3 and key[0] == "2"):
                    break
            tr = tracks.get(key)
            if tr is None:
                tr = tracks[key] = (Axis(noise, jerk), Axis(noise, jerk))
            t0, x0, y0 = run[0]
            fresh = not tr[0].have or t0 < tr[0].t or t0 - tr[0].t > gap_ms or \
                math.hypot(x0 - tr[0].expected(t0), y0 - tr[1].expected(t0)) > jump_units
            if fresh:
                tr[0].start(x0, t0)
                tr[1].start(y0, t0)
            fields["x"] = str(max(-COORD, min(COORD, round(tr[0].smooth([(t, x) for t, x, y in run])))))
            fields["y"] = str(max(-COORD, min(COORD, round(tr[1].smooth([(t, y) for t, x, y in run])))))
        if phase in (4, 5) or (phase == 3 and key[0] == "2"):
            tracks.pop(key, None)
        out.append((arrived, fields))
    return out


# ---- thinning (error bounded) -------------------------------------------------

MEASUREMENTS = {"p": ("pressure", 0), "tx": ("tilt", 0), "ty": ("tilt", 0), "twist": ("angle", 36000),
                "tangent": (131070, 0), "z": (2000000, 0), "dist": (65535, 0), "major": ("position", 0),
                "minor": ("position", 0), "angle": ("angle", 36000), "rx": (65535, 0), "ry": (65535, 0),
                "grip": (65535, 0)}
NEVER_WITH = {"seq", "pred", "est", "ink"}
SAME = ("phase", "buttons", "mods", "tool", "valid", "src", "stage", "conf", "w", "h")


def tolerance_of(settings, name):
    kind, _ = MEASUREMENTS[name]
    if kind == "pressure":
        return settings.pressure_tolerance
    if kind == "tilt":
        return settings.tilt_tolerance
    if kind == "angle":
        return settings.angle_tolerance
    if kind == "position":
        return settings.tolerance
    return settings.fraction_tolerance * kind


def circular(d, period):
    d = math.fmod(d, period)
    return d - period if d > period / 2 else d + period if d < -period / 2 else d


def seg_distance(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    length = dx * dx + dy * dy
    u = 0 if not length else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length))
    return math.hypot(px - (ax + u * dx), py - (ay + u * dy))


def within(settings, last, dropped, held, nxt):
    ax, ay, bx, by = int(last["x"]), int(last["y"]), int(nxt["x"]), int(nxt["y"])
    eps = settings.pad_tolerance if nxt.get("src") == "1" else settings.tolerance
    t0, t1 = int(last.get("t", 0)), int(nxt.get("t", 0))
    for d in dropped + [held]:
        if seg_distance(int(d["x"]), int(d["y"]), ax, ay, bx, by) > eps:
            return False
        u = max(0.0, min(1.0, (int(d["t"]) - t0) / (t1 - t0))) if t1 > t0 and "t" in d else -1
        for name, (_, wrap) in MEASUREMENTS.items():
            if name not in d:
                continue
            if name not in last or name not in nxt:
                return False
            va, vb, vd = float(last[name]), float(nxt[name]), float(d[name])
            tol = tolerance_of(settings, name)
            if u >= 0:
                expected = va + u * (circular(vb - va, wrap) if wrap else vb - va)
                err = circular(vd - expected, wrap) if wrap else vd - expected
                if abs(err) > tol:
                    return False
            elif abs(vd - va) > tol or abs(vd - vb) > tol:
                return False
    return True


def thin(reports, settings, interval_ms=100, keep_ms=25):
    """What the collector sends, as the packets it goes out in: merges while the
    tolerances hold and every report merged away stays within half of keep_ms
    of one kept (0: nothing merges), releases once an interval."""
    packets = [[]]
    held = []        # (contact, compatible, bounded, arrived, since, fields); since: the oldest
                     # report merged in that the one kept before does not cover, or None
    tracks = {}      # contact -> {"kept_at": ms, "last": fields, "dropped": [...]}
    next_release = 0

    def emit(entry):
        contact, _, bounded, arrived, _, fields = entry
        tr = tracks.setdefault(contact, {})
        tr["kept_at"] = arrived
        if bounded:
            tr["last"], tr["dropped"] = fields, []
        packets[-1].append((arrived, fields))

    def flush():
        for e in held:
            emit(e)
        held.clear()

    for arrived, fields in reports:
        phase = int(fields["phase"])
        kind = fields.get("kind", "1")
        mergeable = kind in ("1", "2") and phase in (0, 2) and not NEVER_WITH & fields.keys()
        if not mergeable:
            flush()
            packets[-1].append((arrived, fields))
            continue
        contact = (kind, fields.get("device", "0"), fields.get("contact", "0"))
        compatible = tuple(fields.get(k, "-") for k in SAME)
        bounded = phase == 2

        def near(then):
            return arrived >= then and 2 * (arrived - then) <= keep_ms

        since = None
        for i, (c, comp, b, _, held_since, hf) in enumerate(held):
            if c != contact:
                continue
            tr = tracks.get(contact, {})
            merge = comp == compatible and keep_ms > 0 and (held_since is None or near(held_since))
            if merge and bounded:
                merge = "last" in tr and within(settings, tr["last"], tr["dropped"], hf, fields)
            if not merge:
                flush()
                break
            if bounded:
                tr["dropped"].append(hf)
            since = held_since
            del held[i]
            break
        if since is None:
            tr = tracks.get(contact, {})
            if "kept_at" not in tr or not near(tr["kept_at"]):
                since = arrived
        held.append((contact, compatible, bounded, arrived, since, fields))
        if arrived >= next_release:
            next_release = arrived + interval_ms
            flush()
            packets.append([])
    flush()
    return [packet for packet in packets if packet]


# ---- the record (src/terminal/motioncodec.cc) ------------------------------------

def uvarint(v):
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7f) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def svarint(v):
    return uvarint((v << 1) ^ (v >> 63) if v >= 0 else ((v << 1) ^ -1) & ((1 << 64) - 1))


def canonical(text):
    try:
        v = int(text)
    except ValueError:
        return None
    return v if str(v) == text and -(1 << 63) <= v < (1 << 63) else None


def round_div(n, d):
    q, r = divmod(abs(n), d)
    q = q + 1 if 2 * r >= d else q
    return q if n >= 0 else -q


def predict(series, time, order):
    """series: up to three (t, v), oldest first. Integer, as the C++ does it (which
    also falls back on overflow; nothing a hand does overflows 64 bits)."""
    if not series:
        return 0
    last_t, last_v = series[-1]
    if len(series) == 1 or time <= last_t:
        return last_v
    dt = time - last_t
    if order >= 2 and len(series) == 3:
        (t0, v0), (t1, v1), (t2, v2) = series
        A, B, C = t1 - t0, t2 - t1, t2 - t0
        if A > 0 and B > 0:
            N = (v2 - v1) * dt * A * C + ((v2 - v1) * A - (v1 - v0) * B) * dt * (dt + B)
            return last_v + round_div(N, A * B * C)
    (t1, v1), (t2, v2) = series[-2], series[-1]
    if t2 - t1 > 0:
        return last_v + round_div((v2 - v1) * dt, t2 - t1)
    return last_v


def new_context():
    return {"shapes": [], "tracks": [], "track_index": {}, "have_clock": False, "have_time": False, "clock": 0, "time": 0}


def encode_run(packet, context):
    """The bytes of one record for a packet's reports (selector tablet-pointer,
    each with the client's clock), without ink, in a context that lasts the
    session, as the user stream's does."""
    out = bytearray()
    shapes, tracks, track_index = context["shapes"], context["tracks"], context["track_index"]
    have_clock, have_time, clock, time = context["have_clock"], context["have_time"], context["clock"], context["time"]
    for arrived, fields in packet:
        selector = "tablet-pointer"
        keys = list(fields.keys())
        values = {k: canonical(v) for k, v in fields.items()}
        numeric = [values[k] is not None for k in keys]
        signature = (selector, tuple(keys), tuple(numeric))
        if signature not in shapes:
            shapes.append(signature)
            out += uvarint(1) + uvarint(len(selector)) + selector.encode() + uvarint(len(keys))
            for k, n in zip(keys, numeric):
                out += uvarint((len(k) << 1) | (1 if n else 0)) + k.encode()
        shape = shapes.index(signature)
        time_index = next((i for i, k in enumerate(keys) if k == "t" and numeric[i]), -1)
        key = (selector, fields.get("kind", ""), fields.get("device", ""), fields.get("contact", ""), fields.get("src", ""))
        if key not in track_index:
            track_index[key] = len(tracks)
            tracks.append({"series": {}, "text": {}, "samples": 0})
        track = tracks[track_index[key]]
        has_time = time_index >= 0
        when = values["t"] if has_time else track["samples"]
        residual, changed = {}, []
        for i, k in enumerate(keys):
            if numeric[i]:
                basis = track["samples"] if i == time_index else when
                pred = predict(track["series"].get(k, []), basis, 2 if k in ("x", "y") else 1)
                residual[k] = values[k] - pred
                changed.append(residual[k] != 0)
            else:
                changed.append(track["text"].get(k) != fields[k])
        pred_clock = 0
        if have_clock:
            pred_clock = clock + (when - time if have_time and has_time else 0)
        out += uvarint(3) + uvarint((track_index[key] << 2) | 1) + uvarint(shape)
        mask = 0
        for i, c in enumerate(changed):
            if c:
                mask |= 1 << i
        out += mask.to_bytes((len(keys) + 7) // 8, "little")
        out += svarint(arrived - pred_clock)
        if has_time and changed[time_index]:
            out += svarint(residual["t"])
        for i, k in enumerate(keys):
            if i == time_index or not changed[i]:
                continue
            out += svarint(residual[k]) if numeric[i] else uvarint(len(fields[k])) + fields[k].encode()
        for i, k in enumerate(keys):
            if numeric[i]:
                series = track["series"].setdefault(k, [])
                series.append((track["samples"] if i == time_index else when, values[k]))
                del series[:-3]
            else:
                track["text"][k] = fields[k]
        track["samples"] += 1
        have_clock, clock = True, arrived
        if has_time:
            have_time, time = True, when
    context.update(have_clock=have_clock, have_time=have_time, clock=clock, time=time)
    return bytes(out)


# ---- costs ----------------------------------------------------------------------

def compressed_size(data):
    if shutil.which("zstd"):
        return len(subprocess.run(["zstd", "-22", "--ultra", "-c", "-q"], input=data, capture_output=True).stdout)
    return len(zlib.compress(data, 9))


def rms_from_raw(sent, raw):
    """How far the raw points lie from the polyline the program is sent, in pixels."""
    pts = [(int(f["x"]), int(f["y"])) for f in sent if "x" in f]
    if len(pts) < 2:
        return 0.0
    total = 0.0
    for _, f in raw:
        if "x" not in f:
            continue
        px, py = int(f["x"]), int(f["y"])
        total += min(seg_distance(px, py, *pts[i], *pts[i + 1]) for i in range(len(pts) - 1)) ** 2
    return math.sqrt(total / max(1, len(raw))) / 256


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording", nargs="?", help="a file written by GOBLIN_SKIFF_RECORD_INPUT")
    ap.add_argument("--synthetic", metavar="SPEED,NOISE", help="a made-up stroke: pen speed in px/s and sensor noise in px")
    ap.add_argument("--lag", type=float, default=15, help="smoothing lag in ms (0: off)")
    ap.add_argument("--noise", type=float, default=13, help="position noise, 1/256 px")
    ap.add_argument("--jerk", type=float, default=0.05, help="process noise, how acceleration wanders")
    ap.add_argument("--tolerance", type=float, default=64, help="position tolerance, 1/256 px")
    ap.add_argument("--pad-tolerance", type=float, default=64)
    ap.add_argument("--pressure-tolerance", type=float, default=1311)
    ap.add_argument("--tilt-tolerance", type=float, default=200)
    ap.add_argument("--angle-tolerance", type=float, default=500)
    ap.add_argument("--fraction-tolerance", type=float, default=0.02)
    ap.add_argument("--interval", type=float, default=100, help="the link's send interval in ms")
    ap.add_argument("--keep", type=float, default=25, help="a moving hand is reported at least every so many ms (0: every report)")
    settings = ap.parse_args()
    if settings.synthetic:
        speed, noise = (float(v) for v in settings.synthetic.split(","))
        raw = synthetic(speed, noise)
    elif settings.recording:
        raw = load(settings.recording)
    else:
        ap.error("give a recording or --synthetic")
    if not raw:
        sys.exit("no tablet reports found")
    smoothed = smooth_reports(raw, settings.lag, settings.noise, settings.jerk)
    packets = thin(smoothed, settings, settings.interval, settings.keep)
    thinned = [report for packet in packets for report in packet]
    duration = (raw[-1][0] - raw[0][0]) / 1000 or 1

    def by_packet(reports):
        """Reports in the packets the link would carry them in, once an interval."""
        grouped, start = [], None
        for arrived, fields in reports:
            if start is None or arrived >= start + settings.interval:
                start = arrived
                grouped.append([])
            grouped[-1].append((arrived, fields))
        return grouped

    def frame(payload):
        """A packet's payload as sent: compressed, or as it is when that is shorter."""
        return min(compressed_size(payload), len(payload) + 1)

    print(f"{'stage':>9} {'points':>7} {'Hz':>5} {'rms px':>7} {'text B/pt':>10} {'+zstd':>6} {'record B/pt':>12} {'+zstd':>6}")
    for label, reports, grouped in (("raw", raw, by_packet(raw)), ("smoothed", smoothed, by_packet(smoothed)),
                                    ("thinned", thinned, packets)):
        fields = [f for _, f in reports]
        n = max(1, len(fields))
        text = sum(len(text_of(f)) + 1 for f in fields)
        text_zstd = sum(frame(b"".join(text_of(f).encode() + b"\n" for _, f in packet)) for packet in grouped)
        context = new_context()
        records = [encode_run(packet, context) for packet in grouped]
        print(f"{label:>9} {len(fields):7d} {len(fields) / duration:5.0f} {rms_from_raw(fields, raw):7.3f} "
              f"{text / n:10.1f} {text_zstd / n:6.1f} {sum(len(r) for r in records) / n:12.1f} "
              f"{sum(frame(r) for r in records) / n:6.1f}")
    print(f"per packet of {settings.interval:.0f} ms, framing not counted; ink records not modelled")


if __name__ == "__main__":
    main()
