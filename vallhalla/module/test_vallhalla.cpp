// Host-side test for the VAllhalla module: fake heap with the talking/mixing switch triple,
// fades between states, decoys, and the block moving to a new address (a new night).
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
constexpr uint64_t kHeap = 0x3AABC00000ULL, kHeapSize = 96ull << 20;
std::vector<uint8_t> heap(kHeapSize);
std::map<std::string, int64_t> ints;
int failures = 0;

const uint8_t* GetReadPointer(void*, uint64_t a, size_t n) {
    if ((a & 0xFFF) + n > 0x1000) return nullptr;
    if (a < kHeap || a + n > kHeap + kHeapSize) return nullptr;
    return &heap[a - kHeap];
}
uint64_t HeapBegin(void*) { return kHeap; }
// A heap that "starts" 0x97000000 below the fake memory, so the seed spot (+0x97340230) lands
// inside it at kHeap + 0x340230 (the space below reads as unmapped).
uint64_t HeapBeginLow(void*) { return kHeap - 0x97000000ull; }
uint64_t HeapEnd(void*) { return kHeap + kHeapSize; }
void PubI(void*, const char* n, int64_t v) { ints[n] = v; }
void PubT(void*, const char*, const char*) {}
void Put(uint64_t a, double v) { std::memcpy(&heap[a - kHeap], &v, 8); }
void Triple(uint64_t x, double a, double b, double c) { Put(x, a); Put(x + 0x150, b); Put(x + 0x340, c); }
} // namespace

int main() {
    std::mt19937_64 rng(7);
    for (auto& b : heap) b = uint8_t(rng() & 0x7 ? 0 : rng());
    for (int i = 0; i < 20000; ++i) Put(kHeap + (rng() % (kHeapSize / 8)) * 8, double(rng() % 3)); // 0/1/2 noise
    uint64_t real = kHeap + 0x97340230ull % kHeapSize / 16 * 16;
    const uint64_t moved = kHeap + 0x2200000, decoy = kHeap + 0x3300000;
    Triple(decoy, 1, 0, 0); // never flips

    auto* mod = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    EdenDsmodHostApi host{};
    host.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    host.struct_size = sizeof host;
    host.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    host.title_id = 0x010086500D3C8000ULL;
    host.get_heap_begin = HeapBegin;
    host.get_heap_end = HeapEnd;
    host.get_read_pointer = GetReadPointer;
    host.publish_i64 = PubI;
    host.publish_text = PubT;
    void* inst = mod->create(&host, "{}");
    if (!inst || !ext || !ext->on_action) return 1;

    // Like Eden Duo's input path: the snapshot is cleared, then only tick() runs before touches
    // are hit-tested, so every output must come from tick() alone.
    auto run = [&](int n) {
        for (int i = 0; i < n; ++i) {
            ints.clear();
            mod->tick(inst, &host);
        }
    };
    auto state = [&](int mixing) { // fade: switch 1 first, the other two a second later
        Put(real, mixing ? 0 : 1);
        run(60);
        Triple(real, mixing ? 0 : 1, mixing ? 1 : 0, mixing ? 1 : 0);
    };
    auto expect = [&](const char* what, int64_t overlay, int64_t mode) {
        const bool ok = ints["va_overlay"] == overlay && ints["va_mode"] == mode;
        failures += !ok;
        std::printf("%-44s overlay=%lld mode=%lld proven=%lld  %s\n", what, (long long)ints["va_overlay"],
                    (long long)ints["va_mode"], (long long)ints["va_proven"], ok ? "ok" : "FAIL");
    };

    Triple(real, 1, 0, 0);
    run(300);
    failures += !ints.count("va_overlay");
    expect("boot, talking, not sure yet", 1, -1);
    ext->on_action(inst, "skip", 0);
    run(1);
    expect("hold on the overlay while not sure", 0, -1);
    run(250);
    expect("left unused 4 s: locked again", 1, -1);
    ext->on_action(inst, "skip", 0);
    for (int i = 0; i < 10; ++i) {
        run(100);
        ext->on_action(inst, "busy", 0);
    }
    run(1);
    expect("kept open while used (16 s)", 0, -1);
    ext->on_action(inst, "mixed", 0);
    run(1);
    expect("Mix pressed while not sure", 1, -1);
    state(1);
    run(30);
    expect("first mixing: switch flips, now sure", 0, 1);
    state(0);
    run(30);
    expect("back to talking", 1, 0);
    ext->on_action(inst, "skip", 0);
    run(1);
    expect("hold on the overlay while sure", 0, 0);
    run(250);
    expect("left unused 4 s: locked again", 1, 0);
    ext->on_action(inst, "skip", 0);
    run(1);
    state(1);
    run(30);
    expect("mixing again", 0, 1);
    state(0);
    run(30);
    expect("talking again: skip was cleared", 1, 0);
    int frames[2] = {0, 0};
    for (int i = 0; i < 60; ++i) {
        run(1);
        ++frames[ints["va_frame"] & 1];
    }
    std::printf("frames over 1 s: %d / %d\n", frames[0], frames[1]);
    failures += frames[0] != 30;
    // the game frees the block and builds it elsewhere
    Triple(real, 7, 7, 7);
    real = moved;
    Triple(real, 1, 0, 0);
    run(200);
    expect("block moved: old one gone, keeps overlay", 1, -1);
    run(1200); // background rescan finds the new block
    state(1);
    run(30);
    expect("new block flips to mixing", 0, 1);
    state(0);
    run(30);
    expect("and back to talking", 1, 0);
    // ---- manual shake (we are in mixing now) ----
    state(1);
    run(30);
    int zr = 0;
    auto frame = [&](int64_t arg) { // one tick as the package runs it
        ints.clear();
        mod->tick(inst, &host);
        if (arg >= 0) ext->on_action(inst, "shake", arg);
        if (ints["va_zr_req"]) { // zr_auto + zr_ack
            ++zr;
            ext->on_action(inst, "zr_done", 0);
        }
    };
    // The package's derived values: the finger's movement this tick as a step 1..5.
    int last_x = -1;
    auto at = [&](int x, int) -> int64_t {
        const int d = last_x < 0 ? 0 : x - last_x;
        last_x = x;
        return d < -12 ? 1 : d < -2 ? 2 : d <= 2 ? 3 : d <= 12 ? 4 : 5;
    };
    for (int i = 0; i < 120; ++i) frame(at(700, 650)); // grab and hold still
    ext->on_action(inst, "shake_end", 0);
    last_x = -1;
    for (int i = 0; i < 30; ++i) frame(-1);
    std::printf("grab without shaking: ZR presses %d (want 0)\n", zr);
    failures += zr != 0;
    int seen_frames = 0;
    for (int i = 0; i < 40; ++i) { // shake left-right, 40 px each way
        const int x = 700 + ((i / 4) % 2 ? 40 : -40) * (i % 4) / 4;
        frame(at(x, 650));
        seen_frames |= 1 << ints["va_wiggle"];
    }
    std::printf("after shaking: ZR %d (want 1), started %lld, frames used 0x%x\n", zr,
                (long long)ints["va_started"], seen_frames);
    failures += zr != 1 || ints["va_started"] != 1;
    for (int i = 0; i < 160; ++i) frame(at(700, 650));
    std::printf("~3 s in: fill %lld blended %lld\n", (long long)ints["va_fill"], (long long)ints["va_blended"]);
    failures += ints["va_fill"] < 50 || ints["va_fill"] > 70 || ints["va_blended"] != 0;
    for (int i = 0; i < 140; ++i) frame(at(700, 650));
    {
        int seen = 0;
        for (int i = 0; i < 60; ++i) { frame(at(700, 650)); seen |= 1 << ints["va_wiggle"]; }
        std::printf("shaker loop while mixing: frames seen 0x%x\n", seen);
        failures += seen == 1;
    }
    std::printf("~6.3 s in: fill %lld blended %lld\n", (long long)ints["va_fill"], (long long)ints["va_blended"]);
    {
        int seen = 0, moved = 0;
        for (int i = 0; i < 30; ++i) {
            frame(at(700, 650));
            seen |= 1 << ints["va_wiggle"];
            moved += ints["va_jx"] != 0 || ints["va_jy"] != 0;
        }
        std::printf("violent shake: poses 0x%x (want only 0 1 3 7 9), jitter on %d/30 ticks\n", seen, moved);
        failures += (seen & ~0x28B) != 0 || moved < 15;
    }
    failures += ints["va_fill"] != 100 || ints["va_blended"] != 1;
    ext->on_action(inst, "shake_end", 0);
    for (int i = 0; i < 30; ++i) frame(-1);
    std::printf("let go after 5 s: ZR %d (want 2), started %lld, fill %lld, done %lld (want 2)\n", zr,
                (long long)ints["va_started"], (long long)ints["va_fill"], (long long)ints["va_done"]);
    failures += zr != 2 || ints["va_started"] != 0 || ints["va_fill"] != 100 || ints["va_done"] != 2;
    {
        int seen = 0;
        for (int i = 0; i < 60; ++i) { frame(-1); seen |= 1 << ints["va_success_frame"]; }
        std::printf("success popup frames over 1 s: 0x%x (glitch 10-14 included: %s)\n", seen,
                    (seen & (0x1F << 10)) ? "yes" : "no");
        failures += !(seen & (0x1F << 10)) || ints["va_wiggle"] != 0;
    }
    ext->on_action(inst, "reset", 0);
    frame(-1);
    std::printf("Reset: fill %lld done %lld\n", (long long)ints["va_fill"], (long long)ints["va_done"]);
    failures += ints["va_fill"] != 0 || ints["va_done"] != 0;
    last_x = -1;
    for (int i = 0; i < 12; ++i) { // a tiny shake, then let go at once
        const int x = 700 + ((i / 4) % 2 ? 40 : -40) * (i % 4) / 4;
        frame(at(x, 650));
    }
    const int64_t mid_fill = ints["va_fill"];
    ext->on_action(inst, "shake_end", 0);
    for (int i = 0; i < 30; ++i) frame(-1);
    std::printf("tiny shake: fill while shaking %lld, after letting go %lld, done %lld blended %lld\n",
                (long long)mid_fill, (long long)ints["va_fill"], (long long)ints["va_done"],
                (long long)ints["va_blended"]);
    failures += mid_fill > 10 || ints["va_fill"] != 100 || ints["va_done"] != 1 || ints["va_blended"] != 0;
    zr -= 2; // that mix pressed ZR twice more
    for (int i = 0; i < 40; ++i) { // shake again, then the drag is cancelled (no shake_end)
        const int x = 700 + ((i / 4) % 2 ? 40 : -40) * (i % 4) / 4;
        frame(at(x, 650));
    }
    for (int i = 0; i < 40; ++i) frame(-1);
    std::printf("cancelled drag: ZR %d (want 4), started %lld\n", zr, (long long)ints["va_started"]);
    failures += zr != 4 || ints["va_started"] != 0;
    state(0);
    run(30);
    std::printf("served (talking): fill %lld done %lld\n", (long long)ints["va_fill"], (long long)ints["va_done"]);
    failures += ints["va_fill"] != 0 || ints["va_done"] != 0;
    mod->destroy(inst);

    // ---- seed: the usual spot is trusted from the first tick ----
    {
        const uint64_t seed = kHeap + 0x340230;
        Triple(seed, 1, 0, 0);
        EdenDsmodHostApi host2 = host;
        host2.get_heap_begin = HeapBeginLow;
        void* i2 = mod->create(&host2, "{}");
        for (int i = 0; i < 3; ++i) { ints.clear(); mod->tick(i2, &host2); }
        const bool a = ints["va_mode"] == 0 && ints["va_overlay"] == 1;
        Triple(seed, 0, 1, 1);
        for (int i = 0; i < 3; ++i) { ints.clear(); mod->tick(i2, &host2); }
        const bool b = ints["va_mode"] == 1 && ints["va_overlay"] == 0;
        std::printf("seed spot: talking at boot %s, mixing right away %s\n", a ? "ok" : "FAIL", b ? "ok" : "FAIL");
        failures += !a || !b;
        mod->destroy(i2);
    }
    std::printf("%s\n", failures ? "VALLHALLA FAIL" : "VALLHALLA OK");
    return failures ? 1 : 0;
}
