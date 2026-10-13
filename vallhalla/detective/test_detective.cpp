// Host-side test for the detective module: a fake "game memory" with noise, timers, decoys and
// one real mode value (a GameMaker-style double: 0.0 talking, 1.0 mixing) reached from the main
// module through two pointers. Drives the module through its C ABI like Eden Duo does.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

extern "C" const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t, uint64_t);
extern "C" const EdenDsmodModuleExtensions* eden_dsmod_get_extensions(uint32_t, uint64_t);

namespace {
constexpr uint64_t kHeap = 0x800000000ULL, kHeapSize = 96ull << 20;
constexpr uint64_t kMain = 0x8000000ULL, kMainSize = 16ull << 20;
constexpr uint64_t kHoleBegin = kHeap + (40ull << 20), kHoleEnd = kHoleBegin + (8ull << 20);
std::vector<uint8_t> heap(kHeapSize), mainmem(kMainSize);
std::map<std::string, int64_t> ints;
std::map<std::string, std::string> texts;
uint64_t tick_count = 0;

uint8_t* Host(uint64_t a, size_t n) {
    if (a >= kHeap && a + n <= kHeap + kHeapSize) {
        if (a < kHoleEnd && a + n > kHoleBegin) return nullptr; // unmapped hole
        return &heap[a - kHeap];
    }
    if (a >= kMain && a + n <= kMain + kMainSize) return &mainmem[a - kMain];
    return nullptr;
}
const uint8_t* GetReadPointer(void*, uint64_t a, size_t n) {
    if ((a & 0xFFF) + n > 0x1000) return nullptr;
    return Host(a, n);
}
uint64_t HeapBegin(void*) { return kHeap; }
uint64_t HeapEnd(void*) { return kHeap + kHeapSize; }
uint64_t GetTick(void*) { return tick_count; }
void PubI(void*, const char* n, int64_t v) { ints[n] = v; }
void PubT(void*, const char* n, const char* v) { texts[n] = v; }
void Log(void*, uint32_t, const char*) {}
template <class T> void Put(uint64_t a, T v) { std::memcpy(Host(a, sizeof v), &v, sizeof v); }
} // namespace

int main() {
    std::mt19937_64 rng(12345);
    for (auto& b : heap) b = uint8_t(rng());
    for (auto& b : mainmem) b = uint8_t(rng() & 0x3F ? 0 : rng());
    // lots of small ints (noise), some steady decoys, some timers that keep changing
    std::vector<uint64_t> noisy, timers;
    for (int i = 0; i < 400000; ++i) {
        uint64_t a = kHeap + (rng() % (kHeapSize / 4)) * 4;
        if (a >= kHoleBegin && a < kHoleEnd) continue;
        Put<int32_t>(a, int32_t(1 + rng() % 64));
        if (i % 7 == 0) noisy.push_back(a);
        if (i % 11 == 0) timers.push_back(a);
    }
    for (int i = 0; i < 50000; ++i) { // GameMaker-style doubles 1..64
        uint64_t a = kHeap + (rng() % (kHeapSize / 8)) * 8;
        if (a >= kHoleBegin && a < kHoleEnd) continue;
        Put<double>(a, double(1 + rng() % 64));
    }
    // the real thing: main+0x123450 -> G; G+0x10 -> S; S+0x48 = mode (double)
    const uint64_t G = kHeap + 0x1234000, S = kHeap + 0x2345600, MODE = S + 0x48;
    // a decoy that flips with the mode, but not reliably (correlates 3 times, then not)
    const uint64_t DECOY = kHeap + 0x3456780;
    Put<uint64_t>(kMain + 0x123450, G);
    Put<uint64_t>(G + 0x10, S);
    // watch path A as found on the device: [main+B07818]+31E0 -> [ ]+170 (+150, +340)
    const uint64_t WG = kHeap + 0x4000000, WI = kHeap + 0x4100000;
    Put<uint64_t>(kMain + 0xB07818, WG);
    Put<uint64_t>(WG + 0x31E0, WI);
    // auto finder targets: the real triple (moves to T2 later, like a new night), a decoy that
    // never flips, and one that turns to garbage
    const uint64_t T = kHeap + 0x5000000, T2 = kHeap + 0x5200000, DS = kHeap + 0x5100000, DG = kHeap + 0x5180000;
    uint64_t real = T;
    Put<double>(DS, 1.0); Put<double>(DS + 0x150, 0.0); Put<double>(DS + 0x340, 0.0);
    Put<double>(DG, 1.0); Put<double>(DG + 0x150, 0.0); Put<double>(DG + 0x340, 0.0);
    bool watch_on = true;
    auto set_state = [&](int mixing, int round) {
        Put<double>(MODE, mixing ? 1.0 : 0.0);
        Put<double>(real, mixing ? 0.0 : 1.0);
        Put<double>(real + 0x150, mixing ? 1.0 : 0.0);
        Put<double>(real + 0x340, mixing ? 1.0 : 0.0);
        if (round == 5) Put<double>(DG + 0x150, 7.0);
        if (watch_on) {
            Put<double>(WI + 0x170, mixing ? 0.0 : 1.0);
            Put<double>(WI + 0x170 + 0x150, mixing ? 1.0 : 0.0);
            Put<double>(WI + 0x170 + 0x340, mixing ? 1.0 : 0.0);
        }
        Put<int32_t>(DECOY, round < 3 ? (mixing ? 2 : 1) : 5);
        for (auto a : timers) Put<int32_t>(a, int32_t(1 + rng() % 64)); // timers change all the time
        for (auto a : noisy) if (rng() % 2) Put<int32_t>(a, int32_t(1 + rng() % 64));
    };

    const EdenDsmodModuleApi* mod = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    const EdenDsmodModuleExtensions* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    assert(mod && ext && ext->on_action && ext->configure);
    EdenDsmodHostApi host{};
    host.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    host.struct_size = sizeof host;
    host.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    host.title_id = 0x010086500D3C8000ULL;
    host.main_base = kMain;
    host.main_size = kMainSize;
    host.get_tick = GetTick;
    host.get_heap_begin = HeapBegin;
    host.get_heap_end = HeapEnd;
    host.get_read_pointer = GetReadPointer;
    host.publish_i64 = PubI;
    host.publish_text = PubT;
    host.log = Log;
    void* inst = mod->create(&host, "{}");
    assert(inst);
    ext->configure(inst, nullptr);
    auto run_until_idle = [&] {
        int guard = 0;
        do {
            ++tick_count;
            mod->tick(inst, &host);
            ints.clear();
            mod->sample(inst, &host);
            if (rng() % 3 == 0) // the game keeps running while the scan is sliced
                for (auto a : timers) Put<int32_t>(a, int32_t(1 + rng() % 64));
        } while (ints["det.busy"] && ++guard < 100000);
        return guard;
    };
    const int seq[] = {0, 1, 0, 1, 0, 1, 1, 0};
    int round = 0;
    for (int label : seq) {
        set_state(label, round++);
        bool ok = ext->on_action(inst, "mark", label);
        int ticks = run_until_idle();
        std::printf("tap %-7s ok=%d ticks=%4d  pool0=%-8lld pool1=%-8lld both=%-6lld | %s\n", label ? "MIXING" : "TALKING", ok,
                    ticks, (long long)ints["det.pool0"], (long long)ints["det.pool1"], (long long)ints["det.both"],
                    texts["det.status"].c_str());
    }
    for (int i = 0; i < 4; ++i) std::printf("  top%d: %s\n", i, texts["det.top" + std::to_string(i)].c_str());
    std::printf("  live=%lld (mode now %s)\n", (long long)ints["det.live"], "talking");
    set_state(1, 99); ints.clear(); mod->sample(inst, &host);
    std::printf("  after switching the game to mixing: live=%lld\n", (long long)ints["det.live"]);
    std::printf("  watch=%lld (%s) | %s | %s | %s\n", (long long)ints["det.watch"], texts["det.votes"].c_str(),
                texts["det.wA"].c_str(), texts["det.wB"].c_str(), texts["det.wC"].c_str());
    set_state(0, 99); ints.clear(); mod->sample(inst, &host);
    std::printf("  talking again: watch=%lld (%s) | %s\n", (long long)ints["det.watch"], texts["det.votes"].c_str(),
                texts["det.wA"].c_str());
    // pointer search on the top candidate
    ext->on_action(inst, "pointer", 0);
    int t = run_until_idle();
    std::printf("pointer search ticks=%d: %s\n", t, texts["det.ptr"].c_str());
    for (int i = 0; i < 6; ++i) if (!texts["det.chain" + std::to_string(i)].empty())
        std::printf("  chain%d: %s\n", i, texts["det.chain" + std::to_string(i)].c_str());
    std::printf("expected: top = H+%llx dbl talk=0 mix=1; chain [main+123450]+10 -> [ ]+48\n",
                (unsigned long long)(MODE - kHeap));
    // auto finder: plain play, no taps
    auto play = [&](int mixing, int ticks) {
        Put<double>(real, mixing ? 0.0 : 1.0);           // in between: 0,0,0 or 1,1,1
        for (int i = 0; i < 60; ++i) { ++tick_count; mod->tick(inst, &host); }
        set_state(mixing, 50);
        for (int i = 0; i < ticks; ++i) { ++tick_count; mod->tick(inst, &host); }
        ints.clear(); mod->sample(inst, &host);
        std::printf("  play %-7s af.state=%lld | %s | %s | %s\n", mixing ? "mixing" : "talking", (long long)ints["af.state"],
                    texts["af.info"].c_str(), texts["af.raw"].c_str(), texts["af.where"].c_str());
        return ints["af.state"];
    };
    play(0, 200);
    const bool a1 = play(1, 200) == 1, a2 = play(0, 200) == 0, a3 = play(1, 200) == 1;
    // the game frees the block and builds a new one somewhere else (new night)
    for (uint64_t o : {0x0ull, 0x150ull, 0x340ull}) Put<double>(real + o, 9.0);
    real = T2;
    watch_on = false; // a stale copy left frozen at "mixing"
    play(0, 2000);    // background rescan finds T2
    play(1, 300);
    const bool a4 = play(0, 300) == 0, a5 = play(1, 300) == 1 && play(0, 300) == 0;
    std::printf("auto finder: %s\n", a1 && a2 && a3 && a4 && a5 ? "AUTO OK" : "AUTO FAIL");
    mod->destroy(inst);
}
