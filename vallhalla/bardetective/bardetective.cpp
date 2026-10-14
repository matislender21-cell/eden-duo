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

    bool OnAction(const std::string& a) {
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

    void Tick() {
        if (job == Job::Build) StepBuild();
        else if (job == Job::Filter) StepFilter();
    }

    void Sample(const EdenDsmodHostApi* h) const {
        auto pt = [&](const char* n, const std::string& v) { h->publish_text(h->userdata, n, v.c_str()); };
        auto pi = [&](const char* n, s64 v) { h->publish_i64(h->userdata, n, v); };
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

EdenDsmodBool OnAction(void* inst, const char* action, int64_t) {
    try {
        return static_cast<BarDetective*>(inst)->OnAction(action ? action : "") ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
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
