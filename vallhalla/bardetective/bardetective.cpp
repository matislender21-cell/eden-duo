// SPDX-License-Identifier: GPL-3.0-or-later
// VAllhalla "bar detective" for VA-11 Hall-A (010086500D3C8000): finds where the game keeps the
// shaker's contents (the five ingredient amounts, Ice and Aging), so the companion can show what
// the player poured with the controller or the top screen too.
//
// The player mixes four known drinks in the game, one after the other, and taps SNAP at each:
//   1  Adelhyde 1, Bronson 2, Delta 3, Flanergide 4, Karmotrine 5, Ice ON,  Aging ON
//   2  (Reset)  everything empty, Ice OFF, Aging OFF
//   3  Adelhyde 2, Bronson 1, Delta 4, Flanergide 3, Karmotrine 6, Ice ON,  Aging OFF
//   4  (Reset)  Adelhyde 3, Bronson 4, Delta 1, Flanergide 2, Karmotrine 0, Ice OFF, Aging ON
// The first SNAP collects every 8-byte double in the heap holding 1..5 (GameMaker numbers are
// doubles); each candidate starts as a guess for every item whose step-1 value it holds. Later
// SNAPs drop the guesses whose value does not match. What survives is listed per item with its
// live value and its offset from the talking/mixing switch the companion already finds (and from
// the heap start), so the release can read it the same way.
//
// 0.0.2: the first run (14 Oct) found Delta S+0x900, Bronson S+0x930, Adelhyde S+0x960,
// Aging S+0x9C0, Ice S+0x9F0 (S = the switch): 0x30 apart. A watch grid now shows the live value
// of every 0x30 slot from S+0x780 to S+0xC00, so Flanergide and Karmotrine can be spotted by
// pouring them.
//
// 0.0.3: mixing finder. The player starts / stops mixing with ZR in the game and taps
// NOT MIXING or MIXING right after; for ~2.5 s after each tap every double from S-0x4000 to
// S+0x8000 is sampled under that label. Flags = slots steady within each label with a different
// value per label (the same each time); timers = slots that move while mixing and stay put
// while not mixing.
//
// 0.0.4: change finder. BEFORE takes a baseline of every double from S-0x8000 to S+0x10000 (and
// marks the ones that move by themselves over 1 s); do one thing in the game; AFTER lists the
// slots that changed, with their old and new values (bottled items, the drink result...).
// 0.0.5: only real numbers count (0, or between 1e-6 and 1e9 in size): the 8-byte slots
// that hold pointers or flags read as tiny garbage doubles and are skipped. 28 shown.
// 0.0.6: live slots S+0x700..S+0x8F0 every 0x10 (0x730 went 0 -> 500 on a failed drink,
// 0x860 0 -> 1 when A Fedora went in, 0x800 = a drink is ready: after a mix or a bottled item).

#include <algorithm>
#include <cmath>
#include <cstdio>
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
constexpr u64 kPagesPerTick = 2048;  // 8 MiB per tick while collecting
constexpr size_t kFilterPerTick = 400000;
constexpr size_t kMaxPool = 12000000;
constexpr u64 kSeedOff = 0x97340230; // the talking/mixing switch (see vallhalla.cpp)
constexpr int kItems = 7;
constexpr int kSteps = 4;
constexpr const char* kItemName[kItems] = {"Adelhyde", "Bronson", "Delta", "Flanergide", "Karmotrine", "Ice", "Aging"};
constexpr int kExpect[kSteps][kItems] = {
    {1, 2, 3, 4, 5, 1, 1},
    {0, 0, 0, 0, 0, 0, 0},
    {2, 1, 4, 3, 6, 1, 0},
    {3, 4, 1, 2, 0, 0, 1},
};
constexpr const char* kStepText[kSteps] = {
    "STEP 1: in the game pour Adelhyde 1, Bronson 2, Delta 3, Flanergide 4, Karmotrine 5, turn Ice ON and Aging ON. Then tap SNAP and wait.",
    "STEP 2: press Reset in the game (shaker empty, Ice and Aging OFF). Then tap SNAP.",
    "STEP 3: pour Adelhyde 2, Bronson 1, Delta 4, Flanergide 3, Karmotrine 6, Ice ON, Aging OFF. Then tap SNAP.",
    "STEP 4: Reset, then pour Adelhyde 3, Bronson 4, Delta 1, Flanergide 2 (no Karmotrine), Ice OFF, Aging ON. Then tap SNAP.",
};
constexpr size_t kShown = 3;

struct Region {
    u64 begin, end;
};

enum class Job { Idle, Build, Filter };

struct BarDetective {
    EdenDsmodHostApi host{};
    int step = 0; // SNAPs done
    std::string status = kStepText[0];
    Job job = Job::Idle;
    std::vector<Region> regions;
    size_t region_i = 0;
    u64 cursor = 0, work_done = 0, work_total = 1;
    bool overflow = false;
    std::vector<u64> addr;
    std::vector<u8> mask; // bit i: still a guess for item i
    size_t filter_i = 0;
    u64 seed = 0;         // the talking/mixing switch, when found at its usual spot

    // mixing finder
    static constexpr s64 kMixFrom = -0x4000, kMixTo = 0x8000;
    static constexpr size_t kMixN = size_t((kMixTo - kMixFrom) / 8);
    struct MixSlot {
        double v[2] = {0, 0};      // the value under each label (flags)
        bool seen[2] = {false, false};
        bool unstable[2] = {false, false};
        double pfirst = 0;         // first value this period
        int pid = -1;              // period it belongs to
        bool moved[2] = {false, false}; // changed within one period
    };
    std::vector<MixSlot> mix;
    int mix_label = -1, mix_period = 0, mix_periods[2] = {0, 0};
    u64 mix_tap = 0, ticks = 0;
    std::vector<std::pair<s64, int>> mix_found; // offset, kind (0 flag, 1 timer)

    void MixSample() {
        if (!seed || mix_label < 0 || ticks < mix_tap + 30 || ticks > mix_tap + 180) return;
        if (mix.size() != kMixN) mix.assign(kMixN, MixSlot{});
        const int L = mix_label;
        for (size_t i = 0; i < kMixN; ++i) {
            double d;
            if (!ReadF64(u64(s64(seed) + kMixFrom + s64(i) * 8), d) || !std::isfinite(d)) continue;
            MixSlot& m = mix[i];
            if (!m.seen[L]) {
                m.v[L] = d;
                m.seen[L] = true;
            } else if (d != m.v[L]) {
                m.unstable[L] = true;
            }
            if (m.pid != mix_period) {
                m.pid = mix_period;
                m.pfirst = d;
            } else if (d != m.pfirst) {
                m.moved[L] = true;
            }
        }
        if (ticks == mix_tap + 180) MixRank();
    }

    void MixRank() {
        mix_found.clear();
        for (size_t i = 0; i < mix.size(); ++i) {
            const MixSlot& m = mix[i];
            const s64 off = kMixFrom + s64(i) * 8;
            if (m.seen[0] && m.seen[1] && !m.unstable[0] && !m.unstable[1] && m.v[0] != m.v[1])
                mix_found.push_back({off, 0});
        }
        for (size_t i = 0; i < mix.size(); ++i) {
            const MixSlot& m = mix[i];
            if (m.moved[1] && !m.moved[0] && m.seen[0]) mix_found.push_back({kMixFrom + s64(i) * 8, 1});
        }
        std::stable_sort(mix_found.begin(), mix_found.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second < b.second;
            return (a.first < 0 ? -a.first : a.first) < (b.first < 0 ? -b.first : b.first);
        });
    }
    int count[kItems] = {};
    std::vector<u64> best[kItems];

    const u8* Ptr(u64 a, size_t n) const {
        return host.get_read_pointer ? host.get_read_pointer(host.userdata, a, n) : nullptr;
    }
    bool ReadF64(u64 a, double& d) const {
        const u8* p = Ptr(a, 8);
        if (!p) return false;
        std::memcpy(&d, p, 8);
        return true;
    }
    static int Small(double d) { // 0..63 when a whole number in range, else -1
        if (!std::isfinite(d) || d < 0 || d > 63 || d != std::floor(d)) return -1;
        return int(d);
    }

    void FindSeed() {
        const u64 hb = host.get_heap_begin ? host.get_heap_begin(host.userdata) : 0;
        double a = -1, b = -1, c = -1;
        const u64 x = hb + kSeedOff;
        if (ReadF64(x, a) && ReadF64(x + 0x150, b) && ReadF64(x + 0x340, c) &&
            ((a == 1 && b == 0 && c == 0) || (a == 0 && b == 1 && c == 1)))
            seed = x;
    }

    void Start() {
        regions.clear();
        const u64 hb = host.get_heap_begin ? host.get_heap_begin(host.userdata) : 0;
        const u64 he = host.get_heap_end ? host.get_heap_end(host.userdata) : 0;
        if (he > hb) regions.push_back({hb & ~(kPage - 1), he});
        work_total = 1;
        for (auto& r : regions) work_total += (r.end - r.begin) / kPage;
        region_i = 0;
        cursor = regions.empty() ? 0 : regions[0].begin;
        work_done = 0;
        overflow = false;
        addr.clear();
        mask.clear();
        job = Job::Build;
        status = "Collecting... keep the game exactly like this.";
    }

    void StepBuild() {
        u64 budget = kPagesPerTick;
        while (budget-- && region_i < regions.size()) {
            const Region& r = regions[region_i];
            if (cursor >= r.end) {
                if (++region_i < regions.size()) cursor = regions[region_i].begin;
                continue;
            }
            if (const u8* p = Ptr(cursor, kPage)) {
                for (size_t o = 0; o < kPage; o += 8) {
                    double d;
                    std::memcpy(&d, p + o, 8);
                    const int v = Small(d);
                    if (v < 1 || v > 5) continue;
                    u8 m = 0;
                    for (int i = 0; i < kItems; ++i)
                        if (kExpect[0][i] == v) m |= u8(1 << i);
                    if (addr.size() >= kMaxPool) {
                        overflow = true;
                        continue;
                    }
                    addr.push_back(cursor + o);
                    mask.push_back(m);
                }
            }
            cursor += kPage;
            ++work_done;
        }
        if (region_i >= regions.size()) {
            job = Job::Idle;
            step = 1;
            Rank();
            status = std::string(overflow ? "(too many, kept the first 12 million) " : "") + kStepText[1];
        }
    }

    void StepFilter() {
        size_t budget = kFilterPerTick;
        while (budget-- && filter_i < addr.size()) {
            double d;
            u8 m = mask[filter_i];
            const int v = ReadF64(addr[filter_i], d) ? Small(d) : -1;
            for (int i = 0; i < kItems; ++i)
                if ((m >> i & 1) && v != kExpect[step][i]) m &= u8(~(1 << i));
            mask[filter_i] = m;
            ++filter_i;
        }
        if (filter_i >= addr.size()) { // compact
            size_t w = 0;
            for (size_t r = 0; r < addr.size(); ++r)
                if (mask[r]) {
                    addr[w] = addr[r];
                    mask[w] = mask[r];
                    ++w;
                }
            addr.resize(w);
            mask.resize(w);
            job = Job::Idle;
            ++step;
            Rank();
            status = step < kSteps ? std::string(kStepText[step])
                                   : "Done! Pour in the game and watch the live values follow. Send a photo.";
            Log();
        }
    }

    // Per item: how many guesses are left, and the ones nearest the switch (the same game object
    // or globals block is the most likely home), else the first ones found.
    void Rank() {
        for (int i = 0; i < kItems; ++i) {
            count[i] = 0;
            best[i].clear();
        }
        const auto dist = [this](u64 a) { return seed ? (a > seed ? a - seed : seed - a) : a; };
        for (int i = 0; i < kItems; ++i) {
            std::vector<u64> all;
            for (size_t k = 0; k < addr.size(); ++k)
                if (mask[k] >> i & 1) {
                    ++count[i];
                    if (all.size() < 500000) all.push_back(addr[k]);
                }
            const size_t n = std::min(all.size(), kShown);
            std::partial_sort(all.begin(), all.begin() + n, all.end(),
                              [&](u64 a, u64 c) { return dist(a) < dist(c); });
            best[i].assign(all.begin(), all.begin() + n);
        }
    }

    std::string Where(u64 a) const {
        const u64 hb = host.get_heap_begin ? host.get_heap_begin(host.userdata) : 0;
        char b[96];
        if (seed) {
            const s64 d = s64(a) - s64(seed);
            std::snprintf(b, sizeof b, "S%c0x%llX H+0x%llX", d < 0 ? '-' : '+', (unsigned long long)(d < 0 ? -d : d),
                          (unsigned long long)(a - hb));
        } else {
            std::snprintf(b, sizeof b, "H+0x%llX", (unsigned long long)(a - hb));
        }
        return b;
    }

    void Log() const {
        if (!host.log) return;
        for (int i = 0; i < kItems; ++i) {
            std::string line = std::string("VAllhalla bar detective: ") + kItemName[i] + " " + std::to_string(count[i]) + " left:";
            size_t n = 0;
            for (size_t k = 0; k < addr.size() && n < 12; ++k)
                if (mask[k] >> i & 1) {
                    line += " " + Where(addr[k]);
                    ++n;
                }
            host.log(host.userdata, 1, line.c_str());
        }
    }

    bool OnAction(const std::string& a, s64 arg = 0) {
        if (a == "label" && (arg == 0 || arg == 1)) {
            if (!seed) FindSeed();
            if (!seed) return false;
            mix_label = int(arg);
            mix_tap = ticks;
            ++mix_period;
            ++mix_periods[arg];
            return true;
        }
        if (a == "before") {
            if (!seed) FindSeed();
            if (!seed) return false;
            ch_state = 1;
            ch_tap = ticks;
            ch_found.clear();
            return true;
        }
        if (a == "after") {
            if (ch_state < 2) return false;
            ch_state = 3;
            ch_tap = ticks;
            return true;
        }
        if (a == "mix_clear") {
            mix.clear();
            mix_found.clear();
            mix_label = -1;
            mix_periods[0] = mix_periods[1] = 0;
            return true;
        }
        if (a == "snap") {
            if (job != Job::Idle || step >= kSteps) return false;
            if (!seed) FindSeed();
            if (step == 0) {
                Start();
            } else {
                filter_i = 0;
                job = Job::Filter;
                status = "Checking...";
            }
            return true;
        }
        if (a == "restart") {
            job = Job::Idle;
            step = 0;
            addr.clear();
            addr.shrink_to_fit();
            mask.clear();
            mask.shrink_to_fit();
            for (int i = 0; i < kItems; ++i) {
                count[i] = 0;
                best[i].clear();
            }
            status = kStepText[0];
            return true;
        }
        return false;
    }

    static constexpr u64 kWatchFrom = 0x780, kWatchStep = 0x30;
    static constexpr int kWatchN = 26;
    static const char* Known(u64 off) {
        switch (off) {
        case 0x900: return " Delta";
        case 0x930: return " Bronson";
        case 0x960: return " Adelhyde";
        case 0x9C0: return " Aging";
        case 0x9F0: return " Ice";
        case 0x8D0: return " Flanergide";
        case 0x8A0: return " Karmotrine";
        case 0x990: return " total";
        case 0x880: return " mixing";
        case 0x800: return " drink ready";
        case 0x860: return " Fedora?";
        case 0x730: return " result?";
        default: return "";
        }
    }

    // change finder
    static constexpr s64 kChFrom = -0x8000, kChTo = 0x10000;
    static constexpr size_t kChN = size_t((kChTo - kChFrom) / 8);
    std::vector<double> ch_before;
    std::vector<u8> ch_noisy;
    u64 ch_tap = 0;
    int ch_state = 0; // 0 idle, 1 taking the baseline, 2 baseline ready, 3 after
    std::vector<std::pair<s64, double>> ch_found; // offset, value before

    static bool Nice(double d) {
        const double a = std::fabs(d);
        return d == 0 || (a >= 1e-6 && a <= 1e9);
    }
    double SlotAt(s64 off, bool& ok) const {
        double d = 0;
        ok = seed && ReadF64(u64(s64(seed) + off), d) && std::isfinite(d);
        return d;
    }
    void ChangeTick() {
        if (ch_state == 1) {
            if (ch_before.size() != kChN) {
                ch_before.assign(kChN, 0);
                ch_noisy.assign(kChN, 0);
            }
            const bool first = ticks == ch_tap + 1;
            for (size_t i = 0; i < kChN; ++i) {
                bool ok;
                const double d = SlotAt(kChFrom + s64(i) * 8, ok);
                if (!ok) {
                    ch_noisy[i] = 1;
                    continue;
                }
                if (first) {
                    ch_before[i] = d;
                    ch_noisy[i] = 0;
                } else if (d != ch_before[i]) {
                    ch_noisy[i] = 1;
                }
            }
            if (ticks >= ch_tap + 60) ch_state = 2;
        } else if (ch_state == 3 && ticks == ch_tap + 30) {
            ch_found.clear();
            for (size_t i = 0; i < kChN; ++i) {
                if (ch_noisy[i]) continue;
                bool ok;
                const double d = SlotAt(kChFrom + s64(i) * 8, ok);
                if (ok && d != ch_before[i] && Nice(d) && Nice(ch_before[i]))
                    ch_found.push_back({kChFrom + s64(i) * 8, ch_before[i]});
            }
            std::stable_sort(ch_found.begin(), ch_found.end(), [](const auto& a, const auto& b) {
                return (a.first < 0 ? -a.first : a.first) < (b.first < 0 ? -b.first : b.first);
            });
            if (host.log)
                for (const auto& [off, was] : ch_found) {
                    char b[120];
                    bool ok;
                    std::snprintf(b, sizeof b, "VAllhalla bar detective: changed S%c0x%llX %g -> %g", off < 0 ? '-' : '+',
                                  (unsigned long long)(off < 0 ? -off : off), was, SlotAt(off, ok));
                    host.log(host.userdata, 1, b);
                }
        }
    }

    void Tick() {
        ++ticks;
        ChangeTick();
        if (!seed) FindSeed();
        MixSample();
        if (job == Job::Build) StepBuild();
        else if (job == Job::Filter) StepFilter();
    }

    void Sample(const EdenDsmodHostApi* h) const {
        auto pt = [&](const char* n, const std::string& v) { h->publish_text(h->userdata, n, v.c_str()); };
        auto pi = [&](const char* n, s64 v) { h->publish_i64(h->userdata, n, v); };
        {
            const char* st = !seed ? "switch not found (be at the bar)"
                             : ch_state == 0 ? "tap BEFORE"
                             : ch_state == 1 ? "taking the baseline... keep still"
                             : ch_state == 2 ? "baseline ready: do ONE thing in the game, then tap AFTER"
                             : ticks < ch_tap + 30 ? "checking..." : "done: these changed";
            char b[200];
            std::snprintf(b, sizeof b, "%s   (%zu changed)", st, ch_found.size());
            pt("bd.cstatus", b);
            for (int k = 0; k < 32; ++k) { // live slots
                const u64 off = 0x700 + u64(k) * 0x10;
                bool ok;
                const double d = SlotAt(s64(off), ok);
                char v[96];
                if (ok && Nice(d)) std::snprintf(v, sizeof v, "S+0x%llX = %g%s", (unsigned long long)off, d, Known(off));
                else std::snprintf(v, sizeof v, "S+0x%llX   -", (unsigned long long)off);
                char n[16];
                std::snprintf(n, sizeof n, "bd.l%d", k);
                pt(n, v);
            }
            for (size_t k = 0; k < 28; ++k) {
                char n[16];
                std::snprintf(n, sizeof n, "bd.c%zu", k);
                if (k >= ch_found.size()) {
                    pt(n, "");
                    continue;
                }
                const auto [off, was] = ch_found[k];
                bool ok;
                const double now = SlotAt(off, ok);
                char v[160];
                std::snprintf(v, sizeof v, "S%c0x%llX   %g  ->  %g%s", off < 0 ? '-' : '+',
                              (unsigned long long)(off < 0 ? -off : off), was, ok ? now : -1.0,
                              off >= 0 ? Known(u64(off)) : "");
                pt(n, v);
            }
        }
        {
            char b[200];
            const bool rec = mix_label >= 0 && ticks <= mix_tap + 180;
            std::snprintf(b, sizeof b, "%s   taps: not mixing %d, mixing %d   found: %zu flags/timers",
                          !seed ? "switch not found (be at the bar)"
                          : rec ? (ticks < mix_tap + 30 ? "wait..." : (mix_label ? "RECORDING: MIXING" : "RECORDING: NOT MIXING"))
                                : "ready",
                          mix_periods[0], mix_periods[1], mix_found.size());
            pt("bd.mstatus", b);
            for (size_t k = 0; k < 12; ++k) {
                char n[16];
                std::snprintf(n, sizeof n, "bd.m%zu", k);
                if (k >= mix_found.size()) {
                    pt(n, "");
                    continue;
                }
                const auto [off, kind] = mix_found[k];
                const MixSlot& m = mix[size_t((off - kMixFrom) / 8)];
                double now = 0;
                const bool ok = ReadF64(u64(s64(seed) + off), now);
                char v[160];
                if (kind == 0)
                    std::snprintf(v, sizeof v, "S%c0x%llX  FLAG  not mixing %g / mixing %g   now %g", off < 0 ? '-' : '+',
                                  (unsigned long long)(off < 0 ? -off : off), m.v[0], m.v[1], ok ? now : -1.0);
                else
                    std::snprintf(v, sizeof v, "S%c0x%llX  TIMER   now %g", off < 0 ? '-' : '+',
                                  (unsigned long long)(off < 0 ? -off : off), ok ? now : -1.0);
                pt(n, v);
            }
        }
        pt("bd.status", status);
        pi("bd.step", step);
        pi("bd.busy", job != Job::Idle ? 1 : 0);
        char b[200];
        if (job == Job::Build)
            std::snprintf(b, sizeof b, "collecting %llu%%, %zu found", (unsigned long long)(work_done * 100 / work_total), addr.size());
        else if (job == Job::Filter)
            std::snprintf(b, sizeof b, "checking %zu%%", addr.empty() ? 100 : filter_i * 100 / addr.size());
        else
            std::snprintf(b, sizeof b, "snaps done: %d of %d, %zu guesses, switch %s", step, kSteps, addr.size(),
                          seed ? "found" : "not found yet");
        pt("bd.progress", b);
        for (int k = 0; k < kWatchN; ++k) {
            const u64 off = kWatchFrom + u64(k) * kWatchStep;
            double d = 0;
            char v[96];
            if (seed && ReadF64(seed + off, d) && std::isfinite(d))
                std::snprintf(v, sizeof v, "S+0x%llX = %g%s", (unsigned long long)off, d, Known(off));
            else
                std::snprintf(v, sizeof v, "S+0x%llX = ?", (unsigned long long)off);
            char n[16];
            std::snprintf(n, sizeof n, "bd.w%d", k);
            pt(n, v);
        }
        for (int i = 0; i < kItems; ++i) {
            std::string line = std::string(kItemName[i]) + ": ";
            if (step == 0) {
                line += "-";
            } else {
                line += std::to_string(count[i]) + (count[i] == 1 ? " place" : " places");
                for (u64 a : best[i]) {
                    double d = -1;
                    const bool ok = ReadF64(a, d);
                    char v[32];
                    if (ok && std::isfinite(d)) std::snprintf(v, sizeof v, "%g", d);
                    else std::snprintf(v, sizeof v, "?");
                    line += "   " + Where(a) + " = " + v;
                }
            }
            char n[16];
            std::snprintf(n, sizeof n, "bd.item%d", i);
            pt(n, line);
        }
    }
};

EdenDsmodBool SupportsBuild(const char*) {
    return EDEN_DSMOD_TRUE; // a research tool: reads only, any build
}

void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != kTitleId) return nullptr;
        auto* d = new BarDetective();
        d->host = *host;
        return d;
    } catch (...) {
        return nullptr;
    }
}

void Destroy(void* inst) {
    delete static_cast<BarDetective*>(inst);
}

void Sample(void* inst, const EdenDsmodHostApi* host) {
    try {
        static_cast<BarDetective*>(inst)->Sample(host);
    } catch (...) {
    }
}

void TickFn(void* inst, const EdenDsmodHostApi* host) {
    auto* d = static_cast<BarDetective*>(inst);
    try {
        d->host = *host;
        d->Tick();
        d->Sample(host);
    } catch (const std::bad_alloc&) {
        d->OnAction("restart");
        d->status = "Out of memory while collecting. Tap RESTART and try again.";
    } catch (...) {
    }
}

void Configure(void*, const EdenDsmodHostExtensions*) {}

EdenDsmodBool OnAction(void* inst, const char* action, int64_t argument) {
    try {
        return static_cast<BarDetective*>(inst)->OnAction(action ? action : "", argument) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
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
    "VAllhalla bar detective",
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
