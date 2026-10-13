// SPDX-License-Identifier: GPL-3.0-or-later
// VAllhalla "detective" module for VA-11 Hall-A (010086500D3C8000).
//
// Finds the game value that tells dialogue/menus apart from the drink-mixing phase, by
// correlating memory with what the player reports:
//   - The player taps TALKING (label 0) or MIXING (label 1) while the game is in that state.
//   - The first tap for a label builds a candidate pool: every 4-byte int and every 8-byte
//     double (GameMaker numbers are doubles) whose value is a small whole number 1..64.
//   - Every later tap filters every pool: a candidate must keep the value it had for that label,
//     or, the first time it sees the other label, change to a different small value 0..64.
//   - Candidates that have been seen in both states are the answer; the page lists them with
//     their live value so the player can watch one flip between states.
//   - "pointer" looks for main-module pointers (one or two levels) that lead to the top
//     candidate, so a later release can find it again after a reboot (heap addresses move).
// All scans are sliced across ticks so the game never stalls.

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
constexpr u64 kPagesPerTick = 2048;     // 8 MiB of guest memory per tick while building
constexpr size_t kFilterPerTick = 250000;
constexpr size_t kMaxPool = 6000000;    // per label
constexpr int kMaxSmall = 64;
constexpr size_t kTopShown = 8;
constexpr u64 kStructSpan = 0x4000;     // how far into a structure a pointer may point

enum : u8 { kI32 = 0, kF64 = 1 };

struct Pool {
    bool built = false;
    std::vector<u64> addr;   // bit 63: type (0 = i32, 1 = f64)
    std::vector<s16> v[2];   // value seen in each state, -1 = not seen yet
    size_t size() const { return addr.size(); }
};

struct Region {
    u64 begin, end;
    char tag;
};

struct Chain {
    u64 main_off;
    u64 d1;
    bool two;
    u64 d2;
};

enum class Job { Idle, Build, Filter, PtrMain1, PtrHeap, PtrMain2 };

struct Detective {
    EdenDsmodHostApi host{};
    Pool pool[2];
    int marks[2] = {0, 0};
    int last_label = -1;
    std::string status = "Tap TALKING or MIXING while the game is in that state.";

    Job job = Job::Idle;
    int job_label = 0;
    std::vector<Region> regions;
    size_t region_i = 0;
    u64 cursor = 0;
    size_t filter_pool = 0;
    size_t filter_i = 0;
    std::vector<u64> keep_addr;
    std::vector<s16> keep_v[2];
    bool overflow = false;
    u64 work_done = 0, work_total = 1;

    // pointer search
    u64 target = 0;
    std::vector<Chain> chains;
    std::vector<u64> heap_ptrs;   // heap locations that point near the target (level 2)
    std::string ptr_status;

    const u8* Page(u64 page) const {
        return host.get_read_pointer ? host.get_read_pointer(host.userdata, page, kPage) : nullptr;
    }

    bool ReadValue(u64 tagged, int& out) const {
        const bool f64 = (tagged >> 63) != 0;
        const u64 a = tagged & ~(u64(1) << 63);
        const u8* p = host.get_read_pointer ? host.get_read_pointer(host.userdata, a, f64 ? 8 : 4)
                                            : nullptr;
        if (!p) return false;
        if (f64) {
            double d;
            std::memcpy(&d, p, 8);
            if (!std::isfinite(d) || d < 0 || d > kMaxSmall || d != std::floor(d)) {
                out = -2;
                return true;
            }
            out = int(d);
        } else {
            s32 v;
            std::memcpy(&v, p, 4);
            out = (v < 0 || v > kMaxSmall) ? -2 : int(v);
        }
        return true;
    }

    void SetupRegions() {
        regions.clear();
        const u64 hb = host.get_heap_begin ? host.get_heap_begin(host.userdata) : 0;
        const u64 he = host.get_heap_end ? host.get_heap_end(host.userdata) : 0;
        if (he > hb) regions.push_back({hb & ~(kPage - 1), he, 'H'});
        if (host.main_size) regions.push_back({host.main_base & ~(kPage - 1), host.main_base + host.main_size, 'M'});
        work_total = 1;
        for (auto& r : regions) work_total += (r.end - r.begin) / kPage;
        region_i = 0;
        cursor = regions.empty() ? 0 : regions[0].begin;
        work_done = 0;
    }

    // ---- actions ---------------------------------------------------------------------------
    bool OnAction(const char* action, s64 arg) {
        const std::string a = action ? action : "";
        if (a == "reset") {
            for (auto& p : pool) p = Pool{};
            marks[0] = marks[1] = 0;
            last_label = -1;
            job = Job::Idle;
            chains.clear();
            ptr_status.clear();
            status = "Cleared. Tap TALKING or MIXING while the game is in that state.";
            return true;
        }
        if (job != Job::Idle) return false; // busy: the page shows progress
        if (a == "mark" && (arg == 0 || arg == 1)) {
            job_label = int(arg);
            ++marks[job_label];
            last_label = job_label;
            keep_addr.clear();
            keep_v[0].clear();
            keep_v[1].clear();
            filter_pool = 0;
            filter_i = 0;
            if (pool[0].built || pool[1].built) {
                job = Job::Filter;
                work_total = pool[0].size() + pool[1].size() + 1;
                work_done = 0;
                status = "Filtering...";
            } else {
                StartBuild();
            }
            return true;
        }
        if (a == "pointer") {
            const auto top = Top();
            const size_t i = size_t(std::max<s64>(0, arg));
            if (i >= top.size()) {
                ptr_status = "No candidate to trace yet.";
                return false;
            }
            target = top[i].addr & ~(u64(1) << 63);
            chains.clear();
            heap_ptrs.clear();
            SetupRegions();
            // level 1: main module words pointing just below the target
            regions.erase(std::remove_if(regions.begin(), regions.end(),
                                         [](const Region& r) { return r.tag != 'M'; }),
                          regions.end());
            cursor = regions.empty() ? 0 : regions[0].begin;
            region_i = 0;
            job = Job::PtrMain1;
            ptr_status = "Searching pointers (1/3)...";
            return true;
        }
        return false;
    }

    void StartBuild() {
        SetupRegions();
        overflow = false;
        job = Job::Build;
        status = "Scanning memory (keep the game in this state)...";
    }

    // ---- tick work ------------------------------------------------------------------------
    template <class F>
    bool WalkPages(F&& on_page) {
        u64 budget = kPagesPerTick;
        while (budget-- && region_i < regions.size()) {
            const Region& r = regions[region_i];
            if (cursor >= r.end) {
                if (++region_i < regions.size()) cursor = regions[region_i].begin;
                continue;
            }
            if (const u8* p = Page(cursor)) on_page(cursor, p, r.tag);
            cursor += kPage;
            ++work_done;
        }
        return region_i >= regions.size();
    }

    void StepBuild() {
        Pool& pl = pool[job_label];
        const bool done = WalkPages([&](u64 base, const u8* p, char) {
            if (overflow) return;
            for (u64 off = 0; off < kPage; off += 4) {
                s32 v;
                std::memcpy(&v, p + off, 4);
                if (v >= 1 && v <= kMaxSmall) Add(pl, base + off, kI32, v);
                if ((off & 7) == 0) {
                    double d;
                    std::memcpy(&d, p + off, 8);
                    if (d >= 1.0 && d <= kMaxSmall && d == std::floor(d)) Add(pl, base + off, kF64, int(d));
                }
            }
        });
        if (overflow) {
            pl = Pool{};
            --marks[job_label];
            job = Job::Idle;
            status = "Too many values to track in this state. Try tapping again in the other state first.";
            return;
        }
        if (done) {
            pl.built = true;
            job = Job::Idle;
            Summarize("Pool ready.");
        }
    }

    void Add(Pool& pl, u64 a, u8 type, int value) {
        if (pl.addr.size() >= kMaxPool) {
            overflow = true;
            return;
        }
        pl.addr.push_back(a | (u64(type) << 63));
        pl.v[job_label].push_back(s16(value));
        pl.v[1 - job_label].push_back(-1);
    }

    void StepFilter() {
        size_t budget = kFilterPerTick;
        while (budget && filter_pool < 2) {
            Pool& pl = pool[filter_pool];
            if (!pl.built || filter_i >= pl.size()) {
                if (pl.built) {
                    pl.addr.swap(keep_addr);
                    pl.v[0].swap(keep_v[0]);
                    pl.v[1].swap(keep_v[1]);
                }
                keep_addr.clear();
                keep_v[0].clear();
                keep_v[1].clear();
                ++filter_pool;
                filter_i = 0;
                continue;
            }
            for (; budget && filter_i < pl.size(); --budget, ++filter_i, ++work_done) {
                int cur;
                if (!ReadValue(pl.addr[filter_i], cur) || cur < 0) continue;
                const int L = job_label, O = 1 - job_label;
                s16 vl = pl.v[L][filter_i];
                const s16 vo = pl.v[O][filter_i];
                if (vl >= 0) {
                    if (cur != vl) continue;           // must be steady within a state
                } else {
                    if (vo >= 0 && cur == vo) continue; // must differ between states
                    vl = s16(cur);
                }
                keep_addr.push_back(pl.addr[filter_i]);
                keep_v[L].push_back(vl);
                keep_v[O].push_back(vo);
            }
        }
        if (filter_pool >= 2) {
            job = Job::Idle;
            if (!pool[job_label].built) {
                StartBuild(); // first tap in this state: also collect its own pool
                return;
            }
            Summarize("Filtered.");
        }
    }

    struct TopItem {
        u64 addr;
        int v0, v1;
    };

    std::vector<TopItem> Top() const {
        std::vector<TopItem> out;
        for (const auto& pl : pool) {
            for (size_t i = 0; i < pl.size() && out.size() < 4096; ++i) {
                if (pl.v[0][i] >= 0 && pl.v[1][i] >= 0) out.push_back({pl.addr[i], pl.v[0][i], pl.v[1][i]});
            }
        }
        // prefer doubles, then lower addresses (globals tend to be allocated early)
        std::sort(out.begin(), out.end(), [](const TopItem& a, const TopItem& b) {
            const bool fa = a.addr >> 63, fb = b.addr >> 63;
            if (fa != fb) return fa > fb;
            return (a.addr & ~(u64(1) << 63)) < (b.addr & ~(u64(1) << 63));
        });
        // drop duplicates (same address as int and double low word)
        out.erase(std::unique(out.begin(), out.end(),
                              [](const TopItem& a, const TopItem& b) { return a.addr == b.addr; }),
                  out.end());
        return out;
    }

    size_t BothCount() const {
        size_t n = 0;
        for (const auto& pl : pool)
            for (size_t i = 0; i < pl.size(); ++i) n += pl.v[0][i] >= 0 && pl.v[1][i] >= 0;
        return n;
    }

    void Summarize(const char* what) {
        char b[256];
        std::snprintf(b, sizeof b, "%s Talking taps: %d, mixing taps: %d. Keep alternating.", what,
                      marks[0], marks[1]);
        status = b;
    }

    // pointer search: level 1 in main, then heap words pointing near the target, then main
    // words pointing near those
    void StepPtrMain1() {
        const bool done = WalkPages([&](u64 base, const u8* p, char) {
            for (u64 off = 0; off < kPage; off += 8) {
                u64 q;
                std::memcpy(&q, p + off, 8);
                if (q <= target && target - q < kStructSpan && chains.size() < 64)
                    chains.push_back({base + off - host.main_base, target - q, false, 0});
            }
        });
        if (done) {
            SetupRegions();
            regions.erase(std::remove_if(regions.begin(), regions.end(),
                                         [](const Region& r) { return r.tag != 'H'; }),
                          regions.end());
            cursor = regions.empty() ? 0 : regions[0].begin;
            region_i = 0;
            job = Job::PtrHeap;
            ptr_status = "Searching pointers (2/3)...";
        }
    }

    void StepPtrHeap() {
        const bool done = WalkPages([&](u64 base, const u8* p, char) {
            for (u64 off = 0; off < kPage; off += 8) {
                u64 q;
                std::memcpy(&q, p + off, 8);
                if (q <= target && target - q < kStructSpan && heap_ptrs.size() < 20000)
                    heap_ptrs.push_back(base + off);
            }
        });
        if (done) {
            std::sort(heap_ptrs.begin(), heap_ptrs.end());
            SetupRegions();
            regions.erase(std::remove_if(regions.begin(), regions.end(),
                                         [](const Region& r) { return r.tag != 'M'; }),
                          regions.end());
            cursor = regions.empty() ? 0 : regions[0].begin;
            region_i = 0;
            job = Job::PtrMain2;
            ptr_status = "Searching pointers (3/3)...";
        }
    }

    bool ReadU64(u64 a, u64& out) const {
        const u8* p = host.get_read_pointer ? host.get_read_pointer(host.userdata, a, 8) : nullptr;
        if (!p) return false;
        std::memcpy(&out, p, 8);
        return true;
    }

    void StepPtrMain2() {
        const bool done = WalkPages([&](u64 base, const u8* p, char) {
            if (heap_ptrs.empty()) return;
            for (u64 off = 0; off < kPage && chains.size() < 64; off += 8) {
                u64 q;
                std::memcpy(&q, p + off, 8);
                // a heap word inside the structure q points at, which itself points near the target
                auto it = std::lower_bound(heap_ptrs.begin(), heap_ptrs.end(), q);
                if (it == heap_ptrs.end() || *it - q >= kStructSpan) continue;
                u64 inner;
                if (!ReadU64(*it, inner) || inner > target) continue;
                chains.push_back({base + off - host.main_base, *it - q, true, target - inner});
            }
        });
        if (done) {
            job = Job::Idle;
            char b[128];
            std::snprintf(b, sizeof b, "Pointer search done: %zu path(s). Heap words: %zu.", chains.size(),
                          heap_ptrs.size());
            ptr_status = b;
        }
    }

    void Tick() {
        switch (job) {
        case Job::Build: StepBuild(); break;
        case Job::Filter: StepFilter(); break;
        case Job::PtrMain1: StepPtrMain1(); break;
        case Job::PtrHeap: StepPtrHeap(); break;
        case Job::PtrMain2: StepPtrMain2(); break;
        default: break;
        }
    }

    // ---- publishing (every sample; the snapshot is cleared each tick) -----------------------
    void Sample(const EdenDsmodHostApi* h) {
        auto pi = [&](const char* n, s64 v) { h->publish_i64(h->userdata, n, v); };
        auto pt = [&](const char* n, const std::string& v) { h->publish_text(h->userdata, n, v.c_str()); };
        pi("det.busy", job != Job::Idle);
        pi("det.progress", s64(std::min<u64>(100, work_done * 100 / std::max<u64>(1, work_total))));
        pi("det.pool0", s64(pool[0].size()));
        pi("det.pool1", s64(pool[1].size()));
        pi("det.taps0", marks[0]);
        pi("det.taps1", marks[1]);
        pt("det.status", status);
        pt("det.ptr", ptr_status);
        const u64 hb = host.get_heap_begin ? host.get_heap_begin(host.userdata) : 0;
        char b[160];
        std::snprintf(b, sizeof b, "heap %llx-%llx  main %llx +%llx", (unsigned long long)hb,
                      (unsigned long long)(host.get_heap_end ? host.get_heap_end(host.userdata) : 0),
                      (unsigned long long)host.main_base, (unsigned long long)host.main_size);
        pt("det.regions", b);
        // the top list is cheap only while small; skip it during big jobs
        size_t both = 0;
        std::vector<TopItem> top;
        if (job == Job::Idle || job == Job::PtrMain1 || job == Job::PtrHeap || job == Job::PtrMain2) {
            if (pool[0].size() + pool[1].size() < 100000) {
                top = Top();
                both = top.size();
            } else {
                both = BothCount();
            }
        }
        pi("det.both", s64(both));
        for (size_t i = 0; i < kTopShown; ++i) {
            std::string line;
            if (i < top.size()) {
                const u64 a = top[i].addr & ~(u64(1) << 63);
                int cur = -1;
                ReadValue(top[i].addr, cur);
                const bool in_heap = a >= hb && a < (host.get_heap_end ? host.get_heap_end(host.userdata) : 0);
                std::snprintf(b, sizeof b, "%c+%llx %s  talk=%d mix=%d  now=%d", in_heap ? 'H' : 'M',
                              (unsigned long long)(a - (in_heap ? hb : host.main_base)),
                              (top[i].addr >> 63) ? "dbl" : "int", top[i].v0, top[i].v1, cur);
                line = b;
            }
            char n[32];
            std::snprintf(n, sizeof n, "det.top%zu", i);
            pt(n, line);
        }
        // live state guess from the top candidate (the overlay test uses this)
        s64 live = -1;
        if (!top.empty()) {
            int cur;
            if (ReadValue(top[0].addr, cur) && cur >= 0) live = cur == top[0].v1 ? 1 : (cur == top[0].v0 ? 0 : -1);
        }
        pi("det.live", live);
        for (size_t i = 0; i < 6; ++i) {
            std::string line;
            if (i < chains.size()) {
                const auto& c = chains[i];
                if (c.two)
                    std::snprintf(b, sizeof b, "[main+%llx]+%llx -> [ ]+%llx", (unsigned long long)c.main_off,
                                  (unsigned long long)c.d1, (unsigned long long)c.d2);
                else
                    std::snprintf(b, sizeof b, "[main+%llx]+%llx", (unsigned long long)c.main_off,
                                  (unsigned long long)c.d1);
                line = b;
            }
            char n[32];
            std::snprintf(n, sizeof n, "det.chain%zu", i);
            pt(n, line);
        }
    }
};

// ---- C ABI glue ------------------------------------------------------------------------------
EdenDsmodBool SupportsBuild(const char*) {
    return EDEN_DSMOD_TRUE; // a research tool: reads only, any build
}

void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != kTitleId) return nullptr;
        auto* d = new Detective();
        d->host = *host;
        return d;
    } catch (...) {
        return nullptr;
    }
}

void Destroy(void* inst) {
    delete static_cast<Detective*>(inst);
}

void Sample(void* inst, const EdenDsmodHostApi* host) {
    try {
        static_cast<Detective*>(inst)->Sample(host);
    } catch (...) {
    }
}

void TickFn(void* inst, const EdenDsmodHostApi* host) {
    try {
        auto* d = static_cast<Detective*>(inst);
        d->host = *host;
        d->Tick();
    } catch (const std::bad_alloc&) {
        auto* d = static_cast<Detective*>(inst);
        for (auto& p : d->pool) p = Pool{};
        d->job = Job::Idle;
        d->status = "Out of memory while scanning. Tap CLEAR and try again.";
    } catch (...) {
    }
}

void Configure(void*, const EdenDsmodHostExtensions*) {}

EdenDsmodBool OnAction(void* inst, const char* action, int64_t argument) {
    try {
        return static_cast<Detective*>(inst)->OnAction(action, argument) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
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
    "VAllhalla detective",
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
