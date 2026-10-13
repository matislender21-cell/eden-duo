// SPDX-License-Identifier: GPL-3.0-or-later
// VAllhalla module for VA-11 Hall-A (010086500D3C8000): tells the companion when the game is in
// dialogue/menus and when it is time to mix, so the bottom screen can show the artist's idle
// overlay and lock the bar until mixing starts. Reads guest memory only; writes nothing.
//
// How it knows: the game keeps three numbers (GameMaker doubles) side by side, at X, X+0x150
// and X+0x340, that read 1,0,0 while talking and 0,1,1 while mixing (found with the VAllhalla
// detective on 13 Oct 2026, base game v0). Heap addresses change between boots, so instead of
// a pointer path the module scans the heap for that triple in the background, keeps every
// match, and trusts the match that most recently flipped between the two patterns. In-between
// readings (the game changes the three across a fade) keep the last state.
//
// Outputs (published every sample):
//   va_overlay  1 = show the idle overlay and block the bar, 0 = hide it
//   va_frame    0/1, the overlay's animation frame at 4 fps
//   va_mode     -1 not sure yet, 0 talking, 1 mixing
//   va_proven   how many matches have been seen flipping
//   va_status   short text for the test readout
//   va_zr_req   1 while the module wants ZR pressed (the package presses it, then acks)
//   va_started  1 while a shake-started mix is running in the game
//   va_wiggle   0..4 shaker frame (0 far left, 2 middle, 4 far right) from the finger's motion
//   va_fill     0..100 mixing time towards the blend point (5 s); va_blended 1 past it
// Actions:
//   skip        the player held a finger on the overlay (emergency): hide it until the mode
//               next changes. A plain tap does nothing: the bar stays locked while talking.
//   busy        pinged by the package while the player uses the bar; keeps an emergency
//               opening alive (it closes after ~4 s without use)
//   shake       every tick of a drag: argument = shaker finger x*4096 + y + 1, or 0 when the
//               finger is not on the shaker. Real back-and-forth motion starts the mix (ZR).
//   shake_end   the shaker was let go: stops the mix (ZR again) if a shake started it
//   zr_done     the package pressed ZR for us
//   mixed       the player pressed Mix: while not sure, bring the overlay back (0.12 behaviour)

#include <algorithm>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"

using namespace dsmod_sdk::int_types;

namespace {

constexpr u64 kTitleId = 0x010086500D3C8000ULL;
constexpr u64 kPage = 0x1000;
constexpr u64 kS2 = 0x150, kS3 = 0x340;  // switch 2 and 3, from switch 1
constexpr u64 kPagesPerTick = 1024;      // 4 MiB of heap per tick while scanning
constexpr size_t kMaxCands = 20000;
constexpr size_t kCheckPerTick = 2000;   // unproven matches are re-checked round-robin
constexpr int kStrikes = 90;             // ~1.5 s of garbage before a match is dropped
constexpr u64 kRescanProven = 3600;      // ~60 s between background scans once sure
constexpr u64 kRescanUnsure = 600;       // ~10 s while not sure yet
constexpr u64 kFrameTicks = 15;          // 4 fps at 60 ticks/s
constexpr u64 kEmergencyIdle = 240;      // an emergency opening closes after ~4 s unused
constexpr u64 kBlendTicks = 300;         // the game blends a drink mixed for 5 s
constexpr int kReversalPx = 18;          // a direction change counts after this much travel
constexpr u64 kShakeWindow = 50;         // two reversals within ~0.8 s = shaking
constexpr u64 kZrGap = 14;               // ticks between two of our ZR presses

struct Cand {
    u64 x;
    u8 seen;     // bit 0: talking pattern seen, bit 1: mixing pattern seen
    u8 strikes;
    s8 state;    // last full pattern: 0 talking, 1 mixing
    u64 flipped; // tick of the last change between the two patterns
};

struct Region {
    u64 begin, end;
};

struct Module {
    EdenDsmodHostApi host{};
    std::vector<Cand> cands;
    std::vector<Region> regions;
    size_t region_i = 0, rr = 0;
    u64 cursor = 0, wait = 0, ticks = 0;
    bool scanning = false;

    s64 mode = -1;        // decided state
    s64 last_mode = -1;
    bool skip = false;    // overlay dismissed by a tap until the mode changes
    bool manual = true;   // overlay while not sure (starts shown, hold hides, Mix shows)
    bool emergency = false; // the bar was opened by the hold; closes again when left unused
    u64 unused = 0;         // ticks since the player last touched the bar during an emergency

    // manual shake
    bool has_pos = false;
    s64 px = 0, py = 0;     // last finger position on the shaker
    s64 run_x = 0, run_y = 0; // travel in the current direction, per axis (signed)
    u64 rev_ticks[2] = {0, 0}; // ticks of the last two direction reversals
    double vxs = 0;         // smoothed horizontal speed, px per tick
    bool started = false;   // we pressed ZR to start a mix that is still running
    u64 mix_ticks = 0;      // how long the current / last mix has run
    int zr_pending = 0;     // ZR presses still to be made
    u64 last_zr = 0;        // tick of our last ZR press
    u64 last_shake = 0;     // tick of the last shake sample on the shaker

    bool ReadF64(u64 a, double& d) const {
        const u8* p = host.get_read_pointer ? host.get_read_pointer(host.userdata, a, 8) : nullptr;
        if (!p) return false;
        std::memcpy(&d, p, 8);
        return true;
    }

    // -2 garbage, -1 in between, 0 talking (1,0,0), 1 mixing (0,1,1)
    int Pattern(u64 x) const {
        double a, b, c;
        if (!ReadF64(x, a) || !ReadF64(x + kS2, b) || !ReadF64(x + kS3, c)) return -2;
        auto bit = [](double d) { return d == 0.0 ? 0 : d == 1.0 ? 1 : -1; };
        const int A = bit(a), B = bit(b), C = bit(c);
        if (A < 0 || B < 0 || C < 0) return -2;
        if (A == 1 && B == 0 && C == 0) return 0;
        if (A == 0 && B == 1 && C == 1) return 1;
        return -1;
    }

    void StartScan() {
        regions.clear();
        const u64 hb = host.get_heap_begin ? host.get_heap_begin(host.userdata) : 0;
        const u64 he = host.get_heap_end ? host.get_heap_end(host.userdata) : 0;
        if (he > hb) regions.push_back({hb & ~(kPage - 1), he});
        region_i = 0;
        cursor = regions.empty() ? 0 : regions[0].begin;
        scanning = !regions.empty();
    }

    void Add(u64 x, int st) {
        auto it = std::lower_bound(cands.begin(), cands.end(), x, [](const Cand& c, u64 v) { return c.x < v; });
        if (it != cands.end() && it->x == x) return;
        if (cands.size() >= kMaxCands) return;
        cands.insert(it, {x, u8(1u << st), 0, s8(st), 0});
    }

    void ScanStep() {
        u64 budget = kPagesPerTick;
        while (budget-- && region_i < regions.size()) {
            const Region& r = regions[region_i];
            if (cursor >= r.end) {
                if (++region_i < regions.size()) cursor = regions[region_i].begin;
                continue;
            }
            const u8* p = host.get_read_pointer ? host.get_read_pointer(host.userdata, cursor, kPage) : nullptr;
            if (p) {
                for (u64 off = 0; off < kPage; off += 8) {
                    u64 w;
                    std::memcpy(&w, p + off, 8);
                    if (w != 0x3FF0000000000000ULL) continue; // 1.0
                    const u64 y = cursor + off;
                    if (Pattern(y) == 0) Add(y, 0);                       // y is switch 1
                    if (y >= kS2 && Pattern(y - kS2) == 1) Add(y - kS2, 1); // y is switch 2
                }
            }
            cursor += kPage;
        }
        if (region_i >= regions.size()) {
            scanning = false;
            wait = Proven() ? kRescanProven : kRescanUnsure;
        }
    }

    bool Check(Cand& c) { // false = drop
        const int st = Pattern(c.x);
        if (st == -2) return ++c.strikes <= kStrikes;
        c.strikes = 0;
        if (st == -1) return true;
        if (c.state != st) c.flipped = ticks;
        c.state = s8(st);
        c.seen |= u8(1u << st);
        return true;
    }

    size_t Proven() const {
        size_t n = 0;
        for (const auto& c : cands) n += c.seen == 3;
        return n;
    }

    void Track() {
        std::vector<u8> drop(cands.size(), 0);
        bool any = false;
        for (size_t i = 0; i < cands.size(); ++i)
            if (cands[i].seen == 3 && !Check(cands[i])) drop[i] = any = true;
        for (size_t n = 0; n < std::min(cands.size(), kCheckPerTick); ++n) {
            if (rr >= cands.size()) rr = 0;
            if (cands[rr].seen != 3 && !Check(cands[rr])) drop[rr] = any = true;
            ++rr;
        }
        if (any) {
            size_t w = 0;
            for (size_t i = 0; i < cands.size(); ++i)
                if (!drop[i]) cands[w++] = cands[i];
            cands.resize(w);
        }
        const Cand* best = nullptr;
        for (const auto& c : cands)
            if (c.seen == 3 && c.strikes == 0 && (!best || c.flipped > best->flipped)) best = &c;
        mode = best ? best->state : -1;
    }

    bool Overlay() const {
        if (mode < 0) return manual;
        return mode == 0 && !skip;
    }

    void Reversal() {
        rev_ticks[0] = rev_ticks[1];
        rev_ticks[1] = ticks;
    }

    // One axis: a reversal is a change of direction after at least kReversalPx of travel.
    void Axis(s64 d, s64& run) {
        if (d == 0) return;
        if ((d > 0) != (run > 0) && run != 0) {
            if (run >= kReversalPx || run <= -kReversalPx) Reversal();
            run = 0;
        }
        run += d;
    }

    void Shake(s64 arg) {
        if (arg <= 0) { // not on the shaker
            has_pos = false;
            vxs = 0;
            return;
        }
        last_shake = ticks;
        const s64 x = (arg - 1) / 4096, y = (arg - 1) % 4096;
        if (has_pos) {
            const s64 dx = x - px, dy = y - py;
            Axis(dx, run_x);
            Axis(dy, run_y);
            vxs = vxs * 0.6 + double(dx) * 0.4;
        } else {
            run_x = run_y = 0;
            rev_ticks[0] = rev_ticks[1] = 0;
        }
        px = x;
        py = y;
        has_pos = true;
        const bool shaking = rev_ticks[0] != 0 && ticks - rev_ticks[0] <= kShakeWindow;
        if (shaking && !started) { // start the mix
            started = true;
            mix_ticks = 0;
            ++zr_pending;
        }
    }

    void ShakeEnd() {
        has_pos = false;
        vxs = 0;
        if (started) { // stop the mix
            started = false;
            ++zr_pending;
            if (mode < 0) manual = true; // like Mix: while unsure, the overlay comes back
        }
    }

    int Wiggle() const {
        if (!has_pos) return 2;
        const double k = mix_ticks >= kBlendTicks && started ? 3.0 : 6.0; // blended: wilder
        const int f = int(vxs / k + (vxs >= 0 ? 0.5 : -0.5));
        return 2 + std::clamp(f, -2, 2);
    }

    void Tick() {
        ++ticks;
        if (started) ++mix_ticks;
        // A drag that ends without a drop (second finger, page change) sends no shake_end: once
        // the shaker's samples stop for ~1/3 s, treat it as let go so the mix never runs on.
        if (has_pos && ticks - last_shake > 20) ShakeEnd();
        const bool before = Overlay();
        Track();
        if (mode != last_mode) {
            if (mode < 0) manual = before; // lost track: keep what the player sees
            skip = false;
            emergency = false;
            if (mode == 0 && !started) mix_ticks = 0; // drink served: empty the bar
            last_mode = mode;
        }
        if (emergency && ++unused > kEmergencyIdle) { // opened by mistake (or done): lock again
            emergency = false;
            skip = false;
            if (mode < 0) manual = true;
        }
        if (!scanning && mode < 0 && wait > kRescanUnsure) wait = kRescanUnsure; // lost it: look sooner
        if (scanning) {
            ScanStep();
        } else if (wait) {
            --wait;
        } else {
            StartScan();
        }
    }

    bool OnAction(const std::string& a, s64 arg = 0) {
        if (a == "skip") {
            if (mode < 0) manual = false;
            else skip = true;
            emergency = !Overlay();
            unused = 0;
            return true;
        }
        if (a == "shake") {
            unused = 0;
            Shake(arg);
            return true;
        }
        if (a == "shake_end") {
            ShakeEnd();
            return true;
        }
        if (a == "zr_done") {
            if (zr_pending > 0) --zr_pending;
            last_zr = ticks;
            return true;
        }
        if (a == "busy") { // the player is using the bar (pinged while dragging / pressing)
            unused = 0;
            return true;
        }
        if (a == "mixed") {
            if (mode < 0) manual = true;
            return true;
        }
        return false;
    }

    void Sample(const EdenDsmodHostApi* h) const {
        h->publish_i64(h->userdata, "va_overlay", Overlay() ? 1 : 0);
        h->publish_i64(h->userdata, "va_frame", s64((ticks / kFrameTicks) & 1));
        h->publish_i64(h->userdata, "va_mode", mode);
        h->publish_i64(h->userdata, "va_proven", s64(Proven()));
        h->publish_i64(h->userdata, "va_zr_req", zr_pending > 0 && ticks - last_zr >= kZrGap ? 1 : 0);
        h->publish_i64(h->userdata, "va_started", started ? 1 : 0);
        h->publish_i64(h->userdata, "va_wiggle", Wiggle());
        h->publish_i64(h->userdata, "va_fill", s64(std::min<u64>(100, mix_ticks * 100 / kBlendTicks)));
        h->publish_i64(h->userdata, "va_blended", mix_ticks >= kBlendTicks ? 1 : 0);
        const char* st = mode == 1 ? "auto: mixing"
                         : mode == 0 ? (skip ? "auto: talking (opened by hold)" : "auto: talking")
                         : cands.empty() ? "auto: searching"
                                         : "auto: waiting for first mix";
        h->publish_text(h->userdata, "va_status", st);
    }
};

EdenDsmodBool SupportsBuild(const char*) {
    return EDEN_DSMOD_TRUE; // reads only and finds its data by pattern; the package pins the build
}

void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != kTitleId) return nullptr;
        auto* m = new Module();
        m->host = *host;
        return m;
    } catch (...) {
        return nullptr;
    }
}

void Destroy(void* inst) {
    delete static_cast<Module*>(inst);
}

void Sample(void* inst, const EdenDsmodHostApi* host) {
    try {
        static_cast<Module*>(inst)->Sample(host);
    } catch (...) {
    }
}

void TickFn(void* inst, const EdenDsmodHostApi* host) {
    auto* m = static_cast<Module*>(inst);
    try {
        m->host = *host;
        m->Tick();
        // Eden Duo hit-tests touches right after tick() (sample() only feeds the drawing), so the
        // overlay gate must be in the snapshot here too or the input_block lets touches through.
        m->Sample(host);
    } catch (...) {
        m->cands.clear(); // out of memory: start over, the overlay falls back to manual
        m->scanning = false;
    }
}

void Configure(void*, const EdenDsmodHostExtensions*) {}

EdenDsmodBool OnAction(void* inst, const char* action, int64_t argument) {
    try {
        return static_cast<Module*>(inst)->OnAction(action ? action : "", argument) ? EDEN_DSMOD_TRUE
                                                                                       : EDEN_DSMOD_FALSE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

const EdenDsmodModuleApi kModule = {
    EDEN_DSMOD_MODULE_ABI_VERSION,
    sizeof(EdenDsmodModuleApi),
    0,
    EDEN_DSMOD_MODULE_ABI_HASH,
    kTitleId,
    "VAllhalla",
    EDEN_DSMOD_CAP_EXTENSIONS,
    SupportsBuild,
    Create,
    Destroy,
    Sample,
    TickFn,
};

const EdenDsmodModuleExtensions kExtensions = {
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH, Configure, OnAction, nullptr,
};

} // namespace

DSMOD_SDK_EXPORT_MODULE(kModule)
DSMOD_SDK_EXPORT_EXTENSIONS(kExtensions)
