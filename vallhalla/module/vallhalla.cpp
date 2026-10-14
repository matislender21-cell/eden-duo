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
//   va_wiggle   0..11 frame of the artist's shaker loop while mixing (violent once blended)
//   va_jx/va_jy jitter offsets in px for the shaker while the shake is violent (blended)
//   va_angle    shaker tilt in degrees (+ = clockwise) and va_sway its sideways sway in the
//               artist's GIF pixels: her swing (0 6 20 44 20 6 0 -13 -27 -45 -27 -13, 140 ms
//               a step) while mixing, a hard +-45 rattle once blended, 0 otherwise
// Images: "module:gm:<sprite name>[:<frame>[:<scale>]]" -> that sprite from the player's own
// game data (never shipped), scaled by an integer factor with nearest neighbour.
//   va_success_frame  0..26 frame of the artist's Success popup while va_done
//   va_fill     0..100: while shaking, time towards the blend point (5 s); full once let go
//   va_blended  1 when the drink is (or will be) blended; va_done 1 mixed / 2 blended after
//               a shake-mix ended (shows "Success!!") until the drink is served or Reset
// Actions:
//   skip        the player held a finger on the overlay (emergency): hide it until the mode
//               next changes. A plain tap does nothing: the bar stays locked while talking.
//   busy        pinged by the package while the player uses the bar; keeps an emergency
//               opening alive (it closes after ~4 s without use)
//   shake       every tick the shaker is carried: argument = movement step 1..5 (fast left,
//               left, still, right, fast right). Real back-and-forth motion starts the mix (ZR).
//   shake_end   the shaker was let go: stops the mix (ZR again) if a shake started it
//   zr_done     the package pressed ZR for us
//   reset       the player pressed Reset: empty the bar and the shaker
//   pour        one unit poured (argument 0..4: Adelhyde, Bronson, Delta, Flanergide, Karmotrine)
//   ice_toggle / age_toggle   Ice / Age toggled (pressing X / Y)
// After a shake-mix: va_result 1 (a recipe: va_ok) or 2 (failed: va_fail), va_drink_src (the
// drink's game sprite, or the broken glass) and va_drink_name, until served or Reset.
//   mixed       the player pressed Mix: while not sure, bring the overlay back (0.12 behaviour)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>


#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "../common/gm_reader.h"

// stb_image's implementation, once (gm_reader.h only needs its declarations)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

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
// Where the switches were in every session so far (heap + offset, base game v0). Checked from
// the first tick and trusted straight away when it reads the pattern, so the very first drink
// does not wait for a scan and a flip. If it is ever wrong, the real match wins on its first
// flip (the most recent flip decides, and a seed that never flips has flipped = 0).
constexpr u64 kSeedOff = 0x97340230;
constexpr u64 kEmergencyIdle = 240;      // an emergency opening closes after ~4 s unused
constexpr u64 kBlendTicks = 300;         // the game blends a drink mixed for 5 s
constexpr int kReversalTicks = 2;        // a direction change counts after this many moving ticks
constexpr u64 kShakeWindow = 50;         // two reversals within ~0.8 s = shaking
constexpr u64 kZrGap = 14;               // ticks between two of our ZR presses

// VA-11 Hall-A drink recipes (names and amounts only). Order: Adelhyde, Bronson Extract,
// Powdered Delta, Flanergide, Karmotrine (-1 = optional: any amount). Checked against the
// community wiki on 14 Oct 2026; Sunshine Cloud (on the rocks, blended, not aged) confirmed in game.
struct Recipe {
    const char* name;
    int amt[5];
    bool ice, aged, blended;
};
constexpr Recipe kRecipes[] = {
    {"Bad Touch", {0, 2, 2, 2, 4}, true, false, false},
    {"Beer", {1, 2, 1, 2, 4}, false, false, false},
    {"Bleeding Jane", {0, 1, 3, 3, 0}, false, false, true},
    {"Bloom Light", {4, 0, 1, 2, 3}, true, true, false},
    {"Blue Fairy", {4, 0, 0, 1, -1}, false, true, false},
    {"Brandtini", {6, 0, 3, 0, 1}, false, true, false},
    {"Cobalt Velvet", {2, 0, 0, 3, 5}, true, false, false},
    {"Crevice Spike", {0, 0, 2, 4, -1}, false, false, true},
    {"Flaming Moai", {1, 1, 2, 3, 5}, false, false, false},
    {"Fluffy Dream", {3, 0, 3, 0, -1}, false, true, false},
    {"Fringe Weaver", {1, 0, 0, 0, 9}, false, true, false},
    {"Frothy Water", {1, 1, 1, 1, 0}, false, true, false},
    {"Grizzly Temple", {3, 3, 3, 0, 1}, false, false, true},
    {"Gut Punch", {0, 5, 0, 1, -1}, false, true, false},
    {"Marsblast", {0, 6, 1, 4, 2}, false, false, true},
    {"Mercuryblast", {1, 1, 3, 3, 2}, true, false, true},
    {"Moonblast", {6, 0, 1, 1, 2}, true, false, true},
    {"Piano Man", {2, 3, 5, 5, 3}, true, false, false},
    {"Piano Woman", {5, 5, 2, 3, 3}, false, true, false},
    {"Pile Driver", {0, 3, 0, 3, 4}, false, false, false},
    {"Sparkle Star", {2, 0, 1, 0, -1}, false, true, false},
    {"Sugar Rush", {2, 0, 1, 0, -1}, false, false, false},
    {"Sunshine Cloud", {2, 2, 0, 0, -1}, true, false, true},
    {"Suplex", {0, 4, 0, 3, 3}, true, false, false},
    {"Zen Star", {4, 4, 4, 4, 4}, true, false, false},
};
constexpr int kShakerMax = 20; // units the shaker holds (a Big drink is every amount doubled)

// -1 = failed, else index into kRecipes; *big set for a doubled drink.
int MatchRecipe(const int c[5], bool ice, bool aged, bool blended, bool* big) {
    int total = 0;
    for (int i = 0; i < 5; ++i) total += c[i];
    if (total == 0 || total > kShakerMax) return -1;
    for (int r = 0; r < int(std::size(kRecipes)); ++r) {
        const Recipe& R = kRecipes[r];
        if (R.ice != ice || R.aged != aged || R.blended != blended) continue;
        for (int size = 1; size <= 2; ++size) {
            bool ok = true;
            for (int i = 0; i < 5 && ok; ++i)
                ok = R.amt[i] < 0 || c[i] == R.amt[i] * size;
            if (ok) {
                *big = size == 2;
                return r;
            }
        }
    }
    return -1;
}

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
    int scans = 0;          // finished heap scans (test readout)

    s64 mode = -1;        // decided state
    s64 last_mode = -1;
    bool skip = false;    // overlay dismissed by a tap until the mode changes
    bool manual = true;   // overlay while not sure (starts shown, hold hides, Mix shows)
    bool emergency = false; // the bar was opened by the hold; closes again when left unused
    u64 unused = 0;         // ticks since the player last touched the bar during an emergency

    // manual shake
    bool has_pos = false;   // the shaker is being carried (samples arriving)
    int dir = 0;            // last moving direction: -1, +1 (0 = none yet)
    int run = 0;            // moving ticks in that direction
    int last_step = 3;      // last movement step (1..5)
    u64 rev_ticks[2] = {0, 0}; // ticks of the last two direction reversals
    bool started = false;   // we pressed ZR to start a mix that is still running
    u64 mix_ticks = 0;      // how long the current / last mix has run
    int done = 0;           // last shake-mix finished: 0 no, 1 mixed, 2 blended (Success!!)
    int counts[5] = {0, 0, 0, 0, 0}; // units poured from the bottom screen this drink
    bool ice = false, aged = false;  // Ice / Age toggled on for this drink
    int result = 0;         // 0 nothing yet, 1 a recipe (success), 2 failed
    int drink = -1;         // kRecipes index when result == 1
    bool big = false;
    u64 done_tick = 0;      // when it finished (Success animation)
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
            ++scans;
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

    void Seed() {
        const u64 hb = host.get_heap_begin ? host.get_heap_begin(host.userdata) : 0;
        const u64 he = host.get_heap_end ? host.get_heap_end(host.userdata) : 0;
        const u64 x = hb + kSeedOff;
        if (hb == 0 || x + kS3 + 8 > he) return;
        auto it = std::lower_bound(cands.begin(), cands.end(), x, [](const Cand& c, u64 v) { return c.x < v; });
        if (it != cands.end() && it->x == x) {
            it->seen = 3; // re-found by a scan before the seed check: trust it too
            return;
        }
        const int st = Pattern(x);
        if (st < 0) return;
        Add(x, st);
        it = std::lower_bound(cands.begin(), cands.end(), x, [](const Cand& c, u64 v) { return c.x < v; });
        if (it != cands.end() && it->x == x) it->seen = 3;
    }

    void Track() {
        Seed();
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

    // One finger sample per tick while the shaker is carried. The package cannot hand the module
    // a live position (module arguments are constants), so it sends the finger's movement this
    // tick as a step: 1 fast left/up, 2 left/up, 3 still, 4 right/down, 5 fast right/down
    // (the larger of the two axes; see the "shk_*" derived values in the manifest).
    void Shake(s64 step) {
        if (step < 1 || step > 5) return;
        if (!has_pos || ticks - last_shake > 20) { // a new grab
            dir = 0;
            run = 0;
            rev_ticks[0] = rev_ticks[1] = 0;
        }
        last_shake = ticks;
        has_pos = true;
        last_step = int(step);
        const int sgn = step < 3 ? -1 : step > 3 ? 1 : 0;
        if (sgn != 0) {
            if (sgn != dir) {
                if (dir != 0 && run >= kReversalTicks) Reversal();
                dir = sgn;
                run = 0;
            }
            ++run;
        }
        const bool shaking = rev_ticks[0] != 0 && ticks - rev_ticks[0] <= kShakeWindow;
        if (shaking && !started) { // start the mix
            started = true;
            mix_ticks = 0;
            done = 0;
            ++zr_pending;
        }
    }

    void NewDrink() {
        done = 0;
        result = 0;
        drink = -1;
        big = false;
        ice = aged = false;
        for (int& c : counts) c = 0;
    }

    void ShakeEnd() {
        has_pos = false;
        if (started) { // stop the mix: any mix is a finished drink, 5 s or more is blended
            started = false;
            done = mix_ticks >= kBlendTicks ? 2 : 1;
            drink = MatchRecipe(counts, ice, aged, done == 2, &big);
            result = drink >= 0 ? 1 : 2;
            done_tick = ticks;
            ++zr_pending;
            if (mode < 0) manual = true; // like Mix: while unsure, the overlay comes back
        }
    }

    // The artist's shaker loop (12 frames, 140 ms each) plays while the drink mixes. Once it is
    // blended the shake turns violent: only the hard-tilt poses (1 3 1 0 7 9 7 0), 35 ms each, plus
    // a jitter of a few pixels (va_jx / va_jy). Frame 0 (upright) when not mixing.
    // The artist's swing, measured from her GIF: tilt (degrees) and sideways sway (GIF px).
    static constexpr int kSwingAngle[12] = {0, 6, 20, 44, 20, 6, 0, -13, -27, -45, -27, -13};
    static constexpr int kSwingSway[12] = {0, 26, 44, 52, 44, 26, 0, -10, -31, -46, -31, -10};

    int SwingStep() const {
        if (!started) return -1;
        const u64 ms = mix_ticks * 1000 / 60;
        if (Violent()) {
            static constexpr int kSeq[8] = {1, 3, 1, 0, 7, 9, 7, 0};
            return kSeq[(ms / 35) % 8];
        }
        return int((ms / 140) % 12);
    }
    int Angle() const {
        const int k = SwingStep();
        return k < 0 ? 0 : kSwingAngle[k];
    }
    int Sway() const {
        const int k = SwingStep();
        return k < 0 ? 0 : kSwingSway[k];
    }

    bool Violent() const {
        return started && mix_ticks >= kBlendTicks;
    }

    int Wiggle() const {
        if (!started) return 0;
        const u64 ms = mix_ticks * 1000 / 60;
        if (Violent()) {
            static constexpr int kSeq[8] = {1, 3, 1, 0, 7, 9, 7, 0};
            return kSeq[(ms / 35) % 8];
        }
        return int((ms / 140) % 12);
    }

    // Small pseudo-random offsets that change every 2 ticks while the shake is violent.
    int Jitter(int axis, int amp) const {
        if (!Violent()) return 0;
        u64 h = (mix_ticks / 2) * 0x9E3779B97F4A7C15ULL + u64(axis) * 0xC2B2AE3D27D4EB4FULL;
        h ^= h >> 29;
        h *= 0xBF58476D1CE4E5B9ULL;
        h ^= h >> 32;
        return int(h % u64(2 * amp + 1)) - amp;
    }

    // The artist's Success popup (27 frames, 30 ms each; the glitch is frames 10-14), looping
    // from the glitch while a finished drink waits to be served.
    int SuccessFrame() const {
        if (!done) return 0;
        const u64 ms = (ticks - done_tick) * 1000 / 60;
        return int((ms / 30 + 10) % 27);
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
            if (mode == 0 && !started) { // drink served: empty the bar, start a new drink
                mix_ticks = 0;
                NewDrink();
            }
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

    // ---- game art (asset worker) ----
    std::mutex gm_mutex;
    vallhalla::GmReader gm;
    mutable std::mutex picked_mutex;
    std::map<std::string, std::string> picked; // "~query" -> the sprite it found (test readout)

    bool LoadImage(const EdenDsmodHostApi* h, const char* key, void* receiver, EdenDsmodImageSink sink) {
        std::string k = key;
        if (k.rfind("module:", 0) == 0) k = k.substr(7);
        if (k.rfind("gm:", 0) != 0) return false;
        k = k.substr(3);
        // gm:<name>[:<frame>[:<scale>]]
        std::vector<std::string> parts;
        size_t at = 0;
        for (;;) {
            const size_t c = k.find(':', at);
            parts.push_back(k.substr(at, c == std::string::npos ? std::string::npos : c - at));
            if (c == std::string::npos) break;
            at = c + 1;
        }
        const size_t frame = parts.size() > 1 ? size_t(std::strtoul(parts[1].c_str(), nullptr, 10)) : 0;
        const int scale = parts.size() > 2 ? int(std::strtol(parts[2].c_str(), nullptr, 10)) : 1;
        std::scoped_lock lock{gm_mutex};
        if (!gm.Open(h ? *h : host)) return false;
        int w = 0, ht = 0;
        std::vector<u8> px;
        if (!parts[0].empty() && parts[0][0] == '~') {
            const auto* e = gm.Find(parts[0]);
            std::scoped_lock l2{picked_mutex};
            picked[parts[0]] = e ? e->name : std::string("(no match)");
        }
        if (!gm.Frame(parts[0], frame, scale, w, ht, px)) return false;
        sink(receiver, u32(w), u32(ht), px.data(), px.size());
        return true;
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
        if (a == "reset") { // the player pressed Reset: empty the bar and the shaker
            if (!started) {
                mix_ticks = 0;
                NewDrink();
            }
            return true;
        }
        if (a == "pour" && arg >= 0 && arg < 5) { // one unit poured from the bottom screen
            if (done) { // the last drink was finished: this pour starts a new one
                mix_ticks = 0;
                NewDrink();
            }
            ++counts[arg];
            return true;
        }
        if (a == "ice_toggle") {
            if (done) NewDrink();
            ice = !ice;
            return true;
        }
        if (a == "age_toggle") {
            if (done) NewDrink();
            aged = !aged;
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
        h->publish_i64(h->userdata, "va_jx", Jitter(0, 7));
        h->publish_i64(h->userdata, "va_jy", Jitter(1, 4));
        h->publish_i64(h->userdata, "va_angle", Angle());
        h->publish_i64(h->userdata, "va_sway", Sway());
        // While shaking the bar counts towards blending (full at 5 s); once let go the drink is
        // done, so the bar is full: blue for mixed, light for blended.
        h->publish_i64(h->userdata, "va_fill",
                       done ? 100 : s64(std::min<u64>(100, mix_ticks * 100 / kBlendTicks)));
        h->publish_i64(h->userdata, "va_blended", done == 2 || (started && mix_ticks >= kBlendTicks) ? 1 : 0);
        h->publish_i64(h->userdata, "va_done", done);
        h->publish_i64(h->userdata, "va_result", result);
        h->publish_i64(h->userdata, "va_ok", result == 1 ? 1 : 0);
        h->publish_i64(h->userdata, "va_fail", result == 2 ? 1 : 0);
        if (result == 1) {
            std::string key = std::string("module:gm:~") + kRecipes[drink].name + "@35x53:0:4";
            h->publish_text(h->userdata, "va_drink_src", key.c_str());
            h->publish_text(h->userdata, "va_drink_name",
                            (std::string(kRecipes[drink].name) + (big ? " (Big)" : "")).c_str());
            std::string q = std::string("~") + kRecipes[drink].name + "@35x53";
            std::scoped_lock l2{picked_mutex};
            const auto it = picked.find(q);
            h->publish_text(h->userdata, "va_drink_sprite",
                            ("sprite: " + (it == picked.end() ? std::string("loading...") : it->second)).c_str());
        } else if (result == 2) {
            h->publish_text(h->userdata, "va_drink_src", "module:gm:glitchbottle:0:4");
            h->publish_text(h->userdata, "va_drink_name", "Failed drink");
        }
        h->publish_i64(h->userdata, "va_success_frame", SuccessFrame());
        const char* st = mode == 1 ? "auto: mixing"
                         : mode == 0 ? (skip ? "auto: talking (opened by hold)" : "auto: talking")
                         : cands.empty() ? "auto: searching"
                                         : "auto: waiting for first mix";
        // test readout: state, matches (proven), finished scans, heap size scanned
        const u64 hb = host.get_heap_begin ? host.get_heap_begin(host.userdata) : 0;
        const u64 he = host.get_heap_end ? host.get_heap_end(host.userdata) : 0;
        char line[160];
        std::snprintf(line, sizeof line, "%s  %zu/%zu s%d %lluM%s", st, Proven(), cands.size(), scans,
                      (unsigned long long)((he > hb ? he - hb : 0) >> 20), scanning ? " *" : "");
        h->publish_text(h->userdata, "va_status", line);
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

EdenDsmodBool LoadImageFn(void* inst, const EdenDsmodHostApi* host, const char* key, void* receiver,
                          EdenDsmodImageSink sink) {
    try {
        return static_cast<Module*>(inst)->LoadImage(host, key ? key : "", receiver, sink) ? EDEN_DSMOD_TRUE
                                                                                         : EDEN_DSMOD_FALSE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

EdenDsmodBool OnAction(void* inst, const char* action, int64_t argument) {
    try {
        return static_cast<Module*>(inst)->OnAction(action ? action : "", argument) ? EDEN_DSMOD_TRUE
                                                                                       : EDEN_DSMOD_FALSE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

// The package font (PixelMplus12, M+ FONT LICENSE; see LICENSE-PixelMplus.txt in the package):
// "VAFN" u16 line_height, u16 first_codepoint, u16 count, then count glyphs of
// u16 x, y, w, h, s16 bearing_x, bearing_y, u16 advance (little-endian), made by the package
// build from the TTF. The atlas is the package's pixelmplus12.png.
EdenDsmodBool DecodeFont(void*, const uint8_t* bytes, size_t size, void* receiver, EdenDsmodFontSink sink) {
    if (!bytes || size < 10 || std::memcmp(bytes, "VAFN", 4) != 0) return EDEN_DSMOD_FALSE;
    u16 line, first, count;
    std::memcpy(&line, bytes + 4, 2);
    std::memcpy(&first, bytes + 6, 2);
    std::memcpy(&count, bytes + 8, 2);
    if (size < 10 + size_t(count) * 14 || line == 0) return EDEN_DSMOD_FALSE;
    std::vector<EdenDsmodFontGlyph> glyphs(count);
    for (u16 i = 0; i < count; ++i) {
        const u8* g = bytes + 10 + size_t(i) * 14;
        std::memcpy(&glyphs[i].x, g, 2);
        std::memcpy(&glyphs[i].y, g + 2, 2);
        std::memcpy(&glyphs[i].w, g + 4, 2);
        std::memcpy(&glyphs[i].h, g + 6, 2);
        std::memcpy(&glyphs[i].bearing_x, g + 8, 2);
        std::memcpy(&glyphs[i].bearing_y, g + 10, 2);
        std::memcpy(&glyphs[i].advance, g + 12, 2);
    }
    sink(receiver, line, first, glyphs.data(), count);
    return EDEN_DSMOD_TRUE;
}

const EdenDsmodFontExtensions kFontExtensions = {
    EDEN_DSMOD_FONT_EXT_VERSION, sizeof(EdenDsmodFontExtensions), EDEN_DSMOD_FONT_EXT_HASH, DecodeFont,
};

const EdenDsmodModuleApi kModule = {
    EDEN_DSMOD_MODULE_ABI_VERSION,
    sizeof(EdenDsmodModuleApi),
    0,
    EDEN_DSMOD_MODULE_ABI_HASH,
    kTitleId,
    "VAllhalla",
    EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ,
    SupportsBuild,
    Create,
    Destroy,
    Sample,
    TickFn,
};

const EdenDsmodModuleExtensions kExtensions = {
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH, Configure, OnAction, LoadImageFn,
};

} // namespace

DSMOD_SDK_EXPORT_MODULE(kModule)
DSMOD_SDK_EXPORT_EXTENSIONS(kExtensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(kFontExtensions)
