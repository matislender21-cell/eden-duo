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

    auto run = [&](int n) {
        for (int i = 0; i < n; ++i) mod->tick(inst, &host);
        ints.clear();
        mod->sample(inst, &host);
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
    expect("boot, talking, not sure yet", 1, -1);
    ext->on_action(inst, "skip", 0);
    run(1);
    expect("tap the overlay while not sure", 0, -1);
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
    expect("tap the overlay while sure (skip)", 0, 0);
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
    mod->destroy(inst);
    std::printf("%s\n", failures ? "VALLHALLA FAIL" : "VALLHALLA OK");
    return failures ? 1 : 0;
}
