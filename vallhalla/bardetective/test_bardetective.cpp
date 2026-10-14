// Host-side test for the bar detective: a fake heap full of small doubles (noise, steady decoys,
// values that change on their own), the talking/mixing switch at its usual spot, and the seven
// real shaker values a little after it. Plays the four steps and checks only the real ones survive.
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
constexpr uint64_t kHeap = 0x800000000ULL, kHeapSize = 0x98000000ULL; // covers the seed spot
constexpr uint64_t kWin = 0x97000000ULL;                               // only this part is backed
constexpr uint64_t kWinSize = 64ull << 20;
std::vector<uint8_t> mem(kWinSize);
std::map<std::string, int64_t> ints;
std::map<std::string, std::string> texts;
std::vector<std::string> logs;

uint8_t* Host(uint64_t a, size_t n) {
    if (a < kHeap + kWin || a + n > kHeap + kWin + kWinSize) return nullptr;
    return &mem[a - kHeap - kWin];
}
const uint8_t* GetReadPointer(void*, uint64_t a, size_t n) {
    if ((a & 0xFFF) + n > 0x1000) return nullptr;
    return Host(a, n);
}
uint64_t HeapBegin(void*) { return kHeap; }
uint64_t HeapEnd(void*) { return kHeap + kHeapSize; }
void PubI(void*, const char* n, int64_t v) { ints[n] = v; }
void PubT(void*, const char* n, const char* v) { texts[n] = v; }
void Log(void*, uint32_t, const char* m) { logs.push_back(m); }
void Put(uint64_t a, double v) { std::memcpy(Host(a, 8), &v, 8); }
} // namespace

int main() {
    std::mt19937_64 rng(99);
    std::vector<uint64_t> noise;
    for (int i = 0; i < 2000000; ++i) {
        const uint64_t a = kHeap + kWin + (rng() % (kWinSize / 8)) * 8;
        Put(a, double(rng() % 7));
        if (i % 5 == 0) noise.push_back(a);
    }
    const uint64_t seed = kHeap + 0x97340230;
    const uint64_t real[7] = {seed + 0x40, seed + 0x60, seed + 0x80, seed + 0xA0, seed + 0xC0, seed + 0x200, seed + 0x210};
    // a decoy that follows the first two steps but not the third
    const uint64_t decoy = seed + 0x5000;
    auto state = [&](int s) {
        static const int e[4][7] = {{1, 2, 3, 4, 5, 1, 1}, {0, 0, 0, 0, 0, 0, 0}, {2, 1, 4, 3, 6, 1, 0}, {3, 4, 1, 2, 0, 0, 1}};
        Put(seed, 0); Put(seed + 0x150, 1); Put(seed + 0x340, 1); // mixing
        for (int i = 0; i < 7; ++i) Put(real[i], e[s][i]);
        Put(decoy, s == 0 ? 1 : s == 1 ? 0 : 7);
        for (size_t k = 0; k < noise.size(); k += 3) Put(noise[k], double(rng() % 7)); // values drift
    };

    auto* mod = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    EdenDsmodHostApi host{};
    host.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    host.struct_size = sizeof host;
    host.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    host.title_id = 0x010086500D3C8000ULL;
    host.get_read_pointer = GetReadPointer;
    host.get_heap_begin = HeapBegin;
    host.get_heap_end = HeapEnd;
    host.publish_i64 = PubI;
    host.publish_text = PubT;
    host.log = Log;
    void* inst = mod->create(&host, "{}");
    int failures = !inst;
    auto run_until_idle = [&]() {
        for (int i = 0; i < 200000; ++i) {
            mod->tick(inst, &host);
            if (ints["bd.busy"] == 0) return;
        }
    };
    for (int s = 0; s < 4; ++s) {
        state(s);
        failures += !ext->on_action(inst, "snap", 0);
        mod->tick(inst, &host);
        run_until_idle();
        std::printf("after snap %d: %s\n", s + 1, texts["bd.progress"].c_str());
    }
    for (int i = 0; i < 7; ++i) {
        char n[16];
        std::snprintf(n, sizeof n, "bd.item%d", i);
        std::printf("  %s\n", texts[n].c_str());
    }
    const char* want[7] = {"S+0x40 ", "S+0x60 ", "S+0x80 ", "S+0xA0 ", "S+0xC0 ", "S+0x200 ", "S+0x210 "};
    for (int i = 0; i < 7; ++i) {
        char n[16];
        std::snprintf(n, sizeof n, "bd.item%d", i);
        const std::string& t = texts[n];
        const size_t first = t.find("   S");
        failures += first == std::string::npos || t.compare(first + 3, std::strlen(want[i]), want[i]) != 0;
    }
    failures += texts["bd.status"].find("Done") == std::string::npos;
    std::printf("watch: %s | %s\n", texts["bd.w0"].c_str(), texts["bd.w12"].c_str());
    failures += texts["bd.w0"] != "S+0x780 = 0" || texts["bd.w12"].find("S+0x9C0 = ") != 0;
    failures += ext->on_action(inst, "snap", 0); // no fifth step
    ext->on_action(inst, "restart", 0);
    mod->tick(inst, &host);
    failures += ints["bd.step"] != 0;
    {   // mixing finder: a flag at S+0x1230 (0 / 1) and a timer at S-0x1240 that counts while mixing
        const uint64_t flag = seed + 0x1230, timer = seed - 0x1240;
        double t = 0;
        auto period = [&](int mixing) {
            Put(flag, mixing);
            failures += !ext->on_action(inst, "label", mixing);
            for (int i = 0; i < 200; ++i) {
                if (mixing) t += 1.0 / 60;
                Put(timer, t);
                Put(seed + 0x2000, double(rng() % 3)); // noise that moves all the time
                mod->tick(inst, &host);
            }
            if (!mixing) t = 0;
        };
        for (int r = 0; r < 3; ++r) { period(0); period(1); }
        std::printf("mixing finder: %s | %s | %s\n", texts["bd.mstatus"].c_str(), texts["bd.m0"].c_str(), texts["bd.m1"].c_str());
        failures += texts["bd.m0"].find("S+0x1230  FLAG  not mixing 0 / mixing 1") != 0 ||
                    texts["bd.m1"].find("S-0x1240  TIMER") != 0;
    }
    std::printf("log lines: %zu\n", logs.size());
    failures += logs.size() < 7;
    mod->destroy(inst);
    std::printf("%s\n", failures ? "BAR DETECTIVE FAIL" : "BAR DETECTIVE OK");
    return failures ? 1 : 0;
}
