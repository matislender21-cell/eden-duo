// Host-side test for the VAllhalla module: fake heap with the talking/mixing switch triple,
// fades between states, decoys, and the block moving to a new address (a new night).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "../common/test_gmfile.h"
#include "../common/gm_reader.h"

extern "C" const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t, uint64_t);
extern "C" const EdenDsmodModuleExtensions* eden_dsmod_get_extensions(uint32_t, uint64_t);
extern "C" const EdenDsmodFontExtensions* eden_dsmod_get_font_extensions(uint32_t, uint64_t);

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
std::map<std::string, std::string> texts;
void PubT(void*, const char* n, const char* v) { texts[n] = v; }
std::vector<uint8_t> gmfile;
std::map<std::string, std::vector<uint8_t>> romfs_extra; // other files next to game.unx
size_t ReadRomfs(void*, const char* path, uint64_t off, void* out, size_t n) {
    const std::vector<uint8_t>* f = nullptr;
    if (std::string(path) == "game.unx") f = &gmfile;
    else if (auto it = romfs_extra.find(path); it != romfs_extra.end()) f = &it->second;
    if (!f) return 0;
    if (!out) return f->size();
    if (off >= f->size()) return 0;
    n = std::min<size_t>(n, f->size() - off);
    std::memcpy(out, f->data() + off, n);
    return n;
}
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
    int64_t tilt_arg = 10; // the finger's sideways speed bucket, set by at()
    auto frame = [&](int64_t arg) { // one tick as the package runs it
        ints.clear();
        mod->tick(inst, &host);
        if (arg >= 0) ext->on_action(inst, "shake", arg);
        if (arg >= 0) ext->on_action(inst, "tilt", tilt_arg);
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
        tilt_arg = std::clamp<int64_t>((d >= 0 ? d + 1 : d - 1) / 3, -10, 10) + 10;
        return d < -12 ? 1 : d < -2 ? 2 : d <= 2 ? 3 : d <= 12 ? 4 : 5;
    };
    for (int i = 0; i < 120; ++i) frame(at(700, 650)); // grab and hold still
    ext->on_action(inst, "shake_end", 0);
    last_x = -1;
    for (int i = 0; i < 30; ++i) frame(-1);
    std::printf("grab without shaking: ZR presses %d (want 0)\n", zr);
    failures += zr != 0;
    int seen_frames = 0;
    ext->on_action(inst, "pour", 0); // something in the shaker
    for (int i = 0; i < 40; ++i) { // shake left-right, 40 px each way
        const int x = 700 + ((i / 4) % 2 ? 40 : -40) * (i % 4) / 4;
        frame(at(x, 650));
        seen_frames |= 1 << ints["va_wiggle"];
    }
    std::printf("after shaking: ZR %d (want 1), started %lld, frames used 0x%x\n", zr,
                (long long)ints["va_started"], seen_frames);
    failures += zr != 1 || ints["va_started"] != 1;
    int sk = 40; // keeps the left-right shake going from where it is
    auto shaking = [&]() { const int x = 700 + ((sk / 4) % 2 ? 40 : -40) * (sk % 4) / 4; ++sk; return at(x, 650); };
    for (int i = 0; i < 160; ++i) frame(shaking());
    std::printf("~3 s in: fill %lld blended %lld\n", (long long)ints["va_fill"], (long long)ints["va_blended"]);
    failures += ints["va_fill"] < 50 || ints["va_fill"] > 70 || ints["va_blended"] != 0;
    for (int i = 0; i < 140; ++i) frame(shaking());
    {
        int seen = 0;
        for (int i = 0; i < 60; ++i) { frame(shaking()); seen |= 1 << ints["va_wiggle"]; }
        std::printf("shaker loop while mixing: frames seen 0x%x, angle now %lld sway %lld\n", seen,
                    (long long)ints["va_angle"], (long long)ints["va_sway"]);
        failures += seen == 1;
    }
    std::printf("~6.3 s in: fill %lld blended %lld\n", (long long)ints["va_fill"], (long long)ints["va_blended"]);
    {
        int seen = 0, moved = 0;
        for (int i = 0; i < 30; ++i) {
            frame(shaking());
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
    ext->on_action(inst, "pour", 0); // something in the shaker
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
    {   // shaking a finished drink again does nothing (Reset or serve first)
        last_x = -1;
        for (int i = 0; i < 40; ++i) { const int x = 700 + ((i / 4) % 2 ? 40 : -40) * (i % 4) / 4; frame(at(x, 650)); }
        ext->on_action(inst, "shake_end", 0);
        for (int i = 0; i < 30; ++i) frame(-1);
        std::printf("shaking a finished drink: ZR %d (want 2, no new presses)\n", zr);
        failures += zr != 2;
    }
    {   // stop shaking while still holding: the mix stops there, as mixed
        ext->on_action(inst, "reset", 0);
        ext->on_action(inst, "pour", 0);
        last_x = -1;
        sk = 0;
        for (int i = 0; i < 90; ++i) frame(shaking()); // ~1.5 s of shaking
        const bool going = ints["va_started"] == 1;
        for (int i = 0; i < 20; ++i) frame(at(700, 650)); // finger still, still held
        const bool grace = ints["va_started"] == 1;        // a short pause is not a stop
        for (int i = 0; i < 40; ++i) frame(at(700, 650));
        const bool stopped = ints["va_started"] == 0 && ints["va_done"] == 1 && ints["va_blended"] == 0 &&
                             std::abs(ints["va_angle"]) <= 1 && zr == 4;
        for (int i = 0; i < 400; ++i) frame(at(700, 650)); // holding it still never blends
        const bool still = ints["va_done"] == 1 && ints["va_blended"] == 0 && zr == 4;
        ext->on_action(inst, "shake_end", 0);
        for (int i = 0; i < 10; ++i) frame(-1);
        std::printf("stop shaking: mixing %d, short pause kept %d, stopped as mixed %d, holding never blends %d\n",
                    going, grace, stopped, still);
        failures += !going || !grace || !stopped || !still;
        ext->on_action(inst, "reset", 0);
        zr = 2;
    }
    {   // a pause too long to blend: 3 s, stop, then shaking again cannot reach blended
        ext->on_action(inst, "pour", 0);
        last_x = -1;
        sk = 0;
        for (int i = 0; i < 180; ++i) frame(shaking());
        for (int i = 0; i < 60; ++i) frame(at(700, 650));
        for (int i = 0; i < 200; ++i) frame(shaking());
        ext->on_action(inst, "shake_end", 0);
        for (int i = 0; i < 10; ++i) frame(-1);
        std::printf("3 s + stop + 3 s: done %lld (want 1 mixed), blended %lld\n", (long long)ints["va_done"],
                    (long long)ints["va_blended"]);
        failures += ints["va_done"] != 1 || ints["va_blended"] != 0;
        ext->on_action(inst, "reset", 0);
        zr = 2;
    }
    ext->on_action(inst, "pour", 0); // something in the shaker
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

    // ---- the game's own shaker contents, read next to the switch ----
    {
        const uint64_t x = kHeap + 0x340230; // the seed spot in this fake heap
        auto put_bar = [&](std::initializer_list<double> c, double ice, double aged, double total) {
            const uint64_t off[5] = {0x960, 0x930, 0x900, 0x8D0, 0x8A0};
            int i = 0;
            for (double d : c) Put(x + off[i++], d);
            Put(x + 0x990, total);
            Put(x + 0x9F0, ice);
            Put(x + 0x9C0, aged);
        };
        Triple(x, 0, 1, 1); // mixing
        EdenDsmodHostApi hg = host;
        hg.get_heap_begin = HeapBeginLow;
        void* ig = mod->create(&hg, "{}");
        put_bar({3, 4, 1, 2, 0}, 0, 1, 10);
        for (int i = 0; i < 3; ++i) { ints.clear(); mod->tick(ig, &hg); }
        const bool read = ints["va_game_ok"] == 1 && ints["va_cnt0"] == 3 && ints["va_cnt1"] == 4 && ints["va_cnt2"] == 1 &&
                          ints["va_cnt3"] == 2 && ints["va_cnt4"] == 0 && ints["va_ice_on"] == 0 && ints["va_age_on"] == 1 &&
                          ints["va_grab"] == 1;
        put_bar({3, 4, 1, 2, 0}, 0, 1, 9); // total below the counts: not trusted
        ints.clear(); mod->tick(ig, &hg);
        const bool distrust = ints["va_game_ok"] == 0;
        put_bar({2, 0, 1, 0, 0}, 0, 0, 3); // Sugar Rush poured with the controller
        ints.clear(); mod->tick(ig, &hg);
        ext->on_action(ig, "shake_end", 0);
        // a quick shake to finish it as mixed
        for (int i = 0; i < 40; ++i) { ints.clear(); mod->tick(ig, &hg); ext->on_action(ig, "shake", (i / 3) % 2 ? 1 : 5); }
        ext->on_action(ig, "shake_end", 0);
        for (int i = 0; i < 3; ++i) { ints.clear(); mod->tick(ig, &hg); }
        const bool drink = ints["va_result"] == 1 && texts["va_drink_name"] == "Sugar Rush";
        std::printf("game shaker: read %d, bad total ignored %d, controller-poured Sugar Rush recognised %d\n", read,
                    distrust, drink);
        failures += !read || !distrust || !drink;
        mod->destroy(ig);
    }
    // ---- the Mix button: a mix that shakes by itself, like the game ----
    {
        const uint64_t x = kHeap + 0x340230;
        Triple(x, 0, 1, 1);
        EdenDsmodHostApi hb = host;
        hb.get_heap_begin = HeapBeginLow;
        void* ib = mod->create(&hb, "{}");
        int zrs = 0;
        auto tick = [&]() {
            ints.clear();
            mod->tick(ib, &hb);
            if (ints["va_zr_req"]) { ++zrs; ext->on_action(ib, "zr_done", 0); }
        };
        const uint64_t off[5] = {0x960, 0x930, 0x900, 0x8D0, 0x8A0};
        for (auto o : off) Put(x + o, 0);
        Put(x + 0x9C0, 0);
        const bool empty_refused = !ext->on_action(ib, "mix_btn", 0);
        // Sugar Rush poured: 2 Adelhyde, 1 Delta (game values), blended by mistake: 6 s
        Put(x + 0x960, 2); Put(x + 0x900, 1); Put(x + 0x990, 3); Put(x + 0x9F0, 0);
        for (int i = 0; i < 3; ++i) tick();
        ext->on_action(ib, "mix_btn", 0);
        int amin = 0, amax = 0, smin = 0, smax = 0;
        for (int i = 0; i < 120; ++i) {
            tick();
            amin = std::min<int>(amin, int(ints["va_angle"])); amax = std::max<int>(amax, int(ints["va_angle"]));
            smin = std::min<int>(smin, int(ints["va_sway"])); smax = std::max<int>(smax, int(ints["va_sway"]));
        }
        const bool rocking = ints["va_btn_mixing"] == 1 && amin < -10 && amax > 10 && smin < -8 && smax > 8 && zrs == 1;
        ext->on_action(ib, "shake_end", 0); // a stray shaker drop does not stop it
        for (int i = 0; i < 260; ++i) tick();
        const bool still = ints["va_started"] == 1 && ints["va_blended"] == 1;
        ext->on_action(ib, "mix_btn", 0);
        for (int i = 0; i < 30; ++i) tick();
        const bool stopped = ints["va_started"] == 0 && ints["va_done"] == 2 && zrs == 2 && ints["va_result"] == 2;
        const bool errors_up = ints["va_err_any"] == 1 || ints["va_result"] == 2;
        // Reset with ZL in the game: its shaker empties -> the failed drink and errors go
        Put(x + 0x960, 0); Put(x + 0x900, 0); Put(x + 0x990, 0);
        for (int i = 0; i < 3; ++i) tick();
        const bool zl_reset = errors_up && ints["va_result"] == 0 && ints["va_err_any"] == 0 && ints["va_fail_lock"] == 0;
        std::printf("ZL in the game clears the failed drink and its errors: %d\n", zl_reset);
        failures += !zl_reset;
        Put(x + 0x960, 2); Put(x + 0x900, 1); Put(x + 0x990, 3);
        for (int i = 0; i < 3; ++i) tick();
        ext->on_action(ib, "mix_btn", 0);
        for (int i = 0; i < 90; ++i) tick();
        ext->on_action(ib, "mix_btn", 0);
        for (int i = 0; i < 30; ++i) tick();
        const bool mixed = ints["va_done"] == 1 && ints["va_result"] == 1 && texts["va_drink_name"] == "Sugar Rush" && zrs == 4;
        ext->on_action(ib, "mix_btn", 0); // Serve
        for (int i = 0; i < 30; ++i) tick();
        const bool served = zrs == 5;
        std::printf("Mix button: empty refused %d, shaker rocks %d (angle %d..%d, sway %d..%d), runs on %d, stop blended %d, "
                    "1.5 s = mixed Sugar Rush %d, Serve presses ZR %d\n",
                    empty_refused, rocking, amin, amax, smin, smax, still, stopped, mixed, served);
        failures += !empty_refused || !rocking || !still || !stopped || !mixed || !served;
        mod->destroy(ib);
    }
    // ---- ZR in the game itself: the bottom screen follows ----
    {
        const uint64_t x = kHeap + 0x340230;
        Triple(x, 0, 1, 1);
        EdenDsmodHostApi hz = host;
        hz.get_heap_begin = HeapBeginLow;
        void* iz = mod->create(&hz, "{}");
        int zrs = 0;
        auto tick = [&]() {
            ints.clear();
            mod->tick(iz, &hz);
            if (ints["va_zr_req"]) { ++zrs; ext->on_action(iz, "zr_done", 0); }
        };
        auto game_mix = [&](int on) { Put(x + 0x880, on); Put(x + 0x800, 1 - on); };
        const uint64_t off[5] = {0x960, 0x930, 0x900, 0x8D0, 0x8A0};
        for (auto o : off) Put(x + o, 0);
        Put(x + 0x960, 2); Put(x + 0x900, 1); Put(x + 0x990, 3); Put(x + 0x9F0, 0); Put(x + 0x9C0, 0);
        game_mix(0);
        for (int i = 0; i < 60; ++i) tick();
        game_mix(1); // ZR on the controller
        for (int i = 0; i < 3; ++i) tick();
        const bool follows = ints["va_started"] == 1 && ints["va_btn_mixing"] == 1 && zrs == 0;
        for (int i = 0; i < 320; ++i) tick();
        const bool blends = ints["va_blended"] == 1;
        game_mix(0); // ZR again
        for (int i = 0; i < 3; ++i) tick();
        const bool ends = ints["va_started"] == 0 && ints["va_done"] == 2 && ints["va_result"] == 2 && zrs == 0;
        // our own Mix button: the game takes a moment to start; that is not a stop
        Put(x + 0x960, 0); Put(x + 0x900, 0); Put(x + 0x990, 0); Put(x + 0x800, 0); // ZL in the game
        for (int i = 0; i < 3; ++i) tick();
        Put(x + 0x960, 2); Put(x + 0x900, 1); Put(x + 0x990, 3);
        for (int i = 0; i < 3; ++i) tick();
        ext->on_action(iz, "mix_btn", 0);
        for (int i = 0; i < 20; ++i) tick(); // game still shows not mixing
        const bool lag_ok = ints["va_started"] == 1;
        game_mix(1);
        for (int i = 0; i < 60; ++i) tick();
        ext->on_action(iz, "mix_btn", 0); // Stop
        for (int i = 0; i < 10; ++i) tick(); // game still mixing a moment
        game_mix(0);
        for (int i = 0; i < 60; ++i) tick();
        const bool own = ints["va_started"] == 0 && ints["va_done"] == 1 && ints["va_result"] == 1 && zrs == 2;
        std::printf("ZR in the game: followed %d, blends %d, ends without our ZR %d, own Mix not cut short %d, own Stop once %d\n",
                    follows, blends, ends, lag_ok, own);
        failures += !follows || !blends || !ends || !lag_ok || !own;
        mod->destroy(iz);
    }
    // ---- the bottled A Fedora: counted in the total only; mixed, it is a success ----
    {
        const uint64_t x = kHeap + 0x340230;
        Triple(x, 0, 1, 1);
        EdenDsmodHostApi hf = host;
        hf.get_heap_begin = HeapBeginLow;
        void* ifd = mod->create(&hf, "{}");
        int zrs = 0;
        auto tick = [&]() {
            ints.clear();
            texts.clear();
            mod->tick(ifd, &hf);
            if (ints["va_zr_req"]) { ++zrs; ext->on_action(ifd, "zr_done", 0); }
        };
        auto game_mix = [&](int on) { Put(x + 0x880, on); Put(x + 0x800, 1 - on); };
        const uint64_t off[5] = {0x960, 0x930, 0x900, 0x8D0, 0x8A0};
        for (auto o : off) Put(x + o, 0);
        Put(x + 0x990, 0); Put(x + 0x9F0, 0); Put(x + 0x9C0, 0); Put(x + 0x880, 0); Put(x + 0x800, 0); Put(x + 0x860, 0);
        for (int i = 0; i < 70; ++i) tick();
        const bool empty = ints["va_result"] == 0 && ints["va_grab"] == 0;
        Put(x + 0x990, 1); Put(x + 0x860, 1); // A Fedora goes in
        for (int i = 0; i < 3; ++i) tick();
        const bool grab = ints["va_game_ok"] == 1 && ints["va_grab"] == 1;
        for (int i = 0; i < 60; ++i) tick();
        game_mix(1); // ZR in the game
        for (int i = 0; i < 90; ++i) tick();
        game_mix(0);
        for (int i = 0; i < 3; ++i) tick();
        const bool fedora = ints["va_result"] == 1 && ints["va_ok"] == 1 && texts["va_drink_name"] == "A Fedora" &&
                            texts["va_drink_src"] == "module:gm:~fedora:0:4";
        for (int i = 0; i < 80; ++i) tick();
        Put(x + 0x990, 0); Put(x + 0x800, 0); // Reset in the game
        for (int i = 0; i < 3; ++i) tick();
        const bool cleared = ints["va_result"] == 0;
        std::printf("A Fedora: empty bar nothing %d, shaker grabbable %d, mixed = success %d, Reset clears it %d\n", empty,
                    grab, fedora, cleared);
        failures += !empty || !grab || !fedora || !cleared;
        mod->destroy(ifd);
    }
    // ---- shaker physics: leans the way the finger moves, springs back ----
    {
        void* ip = mod->create(&host, "{}");
        auto step = [&](int64_t t) { ints.clear(); mod->tick(ip, &host); if (t >= 0) ext->on_action(ip, "tilt", t); };
        int64_t maxr = 0, minl = 0, over = 0;
        for (int i = 0; i < 30; ++i) { step(16); maxr = std::max(maxr, ints["va_angle"]); } // right, 18 px/tick
        const int64_t held_r = ints["va_angle"];
        for (int i = 0; i < 30; ++i) { step(4); minl = std::min(minl, ints["va_angle"]); }  // left
        const int64_t held_l = ints["va_angle"];
        for (int i = 0; i < 15; ++i) { step(10); over = std::max(over, ints["va_angle"]); } // stop: wobbles past 0
        for (int i = 0; i < 60; ++i) step(-1); // let go
        const int64_t rest = ints["va_angle"];
        for (int i = 0; i < 20; ++i) step(20); // a very fast flick is capped
        const int64_t cap = ints["va_angle"];
        std::printf("physics: right lean %lld, left lean %lld, wobble past upright %lld, at rest %lld, capped %lld\n",
                    (long long)held_r, (long long)held_l, (long long)over, (long long)rest, (long long)cap);
        failures += !(held_r >= 20 && held_r <= 35) || !(held_l <= -20 && held_l >= -35) || over <= 0 || rest != 0 ||
                    cap > 55 || maxr > 45 || minl < -50;
        mod->destroy(ip);
    }
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
    // ---- game art from the player's own data file ----
    {
        gmfile = vallhalla_test::BuildFakeGm();
        EdenDsmodHostApi h3 = host;
        h3.read_romfs = ReadRomfs;
        void* i3 = mod->create(&h3, "{}");
        struct Got { uint32_t w = 0, h = 0; std::vector<uint8_t> px; } got;
        auto sink = [](void* r, uint32_t ww, uint32_t hh, const uint8_t* p, size_t n) {
            auto* g = static_cast<Got*>(r); g->w = ww; g->h = hh; g->px.assign(p, p + n);
        };
        bool all = true;
        for (const char* key : {"module:gm:spr_shaker:0:3", "module:gm:SPR SHAKER:0:3", "module:gm:sprshaker:0:3"}) {
            got = {};
            const bool ok = ext->load_image(i3, &h3, key, &got, sink);
            // x3: 48x96; texture 0 pixel (0,0) has red 1 -> output pixels (0..2, 0..2)
            const bool good = ok && got.w == 48 && got.h == 96 && got.px[0] == 1 && got.px[(2 * 48 + 2) * 4] == 1 &&
                              got.px[(3 * 48 + 3) * 4] == 200;
            std::printf("game sprite %-28s -> %ux%u %s\n", key, got.w, got.h, good ? "ok" : "FAIL");
            all = all && good;
        }
        got = {};
        const bool f1 = ext->load_image(i3, &h3, "module:gm:spr_shaker:1:1", &got, sink) && got.w == 16 && got.px[0] == 200;
        const bool bg = ext->load_image(i3, &h3, "module:gm:bg_doodle", &got, sink) && got.w == 64 && got.h == 48;
        const bool miss = !ext->load_image(i3, &h3, "module:gm:no_such_sprite", &got, sink);
        const bool sw = ext->load_image(i3, &h3, "module:gm:spr_shaker:1:1:swaprg", &got, sink) && got.px[0] == 10 &&
                        got.px[1] == 200;
        std::printf("red/green swap: %s\n", sw ? "ok" : "FAIL");
        failures += !sw;
        got = {};
        const bool padded = ext->load_image(i3, &h3, "module:gm:spr_shaker:1:1:pad3x2", &got, sink) && got.w == 22 &&
                            got.h == 36 && got.px[3] == 0 && got.px[(2 * 22 + 3) * 4] == 200 &&
                            got.px[(2 * 22 + 2) * 4 + 3] == 0 && got.px[((35) * 22 + 21) * 4 + 3] == 0;
        got = {};
        const bool padsw = ext->load_image(i3, &h3, "module:gm:spr_shaker:1:1:swaprg:pad4x4", &got, sink) &&
                           got.w == 24 && got.h == 40 && got.px[(4 * 24 + 4) * 4] == 10;
        std::printf("bigger touch area (transparent margin): %s, with swap %s\n", padded ? "ok" : "FAIL", padsw ? "ok" : "FAIL");
        failures += !padded || !padsw;
        std::printf("frame 1 %s, background %s, unknown name refused %s\n", f1 ? "ok" : "FAIL", bg ? "ok" : "FAIL", miss ? "ok" : "FAIL");
        failures += !all || !f1 || !bg || !miss;
        mod->destroy(i3);
    }
    // ---- the game's own sounds (decoded on the device, never shipped) ----
    {
        gmfile = vallhalla_test::BuildFakeGm();
        romfs_extra["snd_ext.ogg"] = vallhalla_test::Ogg();
        romfs_extra["audiogroup1.dat"] = vallhalla_test::BuildFakeAudioGroup();
        EdenDsmodHostApi hs = host;
        hs.read_romfs = ReadRomfs;
        vallhalla::GmReader r;
        const bool open = r.Open(hs) && r.Sounds().size() == 4;
        int ch = 0, rate = 0;
        std::vector<int16_t> pcm;
        const auto* clink = r.FindSound("snd_clink");
        const bool wav = clink && r.SoundPcm(*clink, ch, rate, pcm) && ch == 1 && rate == 22050 && pcm.size() == 5 &&
                         pcm[0] == 100 && pcm[3] == -200;
        auto ogg_ok = [&](const char* name) {
            const auto* snd = r.FindSound(name);
            if (!snd || !r.SoundPcm(*snd, ch, rate, pcm) || ch != 1 || rate != 22050) return false;
            if (pcm.size() < 1000 || pcm.size() > 1300) return false; // ~50 ms
            int peak = 0;
            for (auto v : pcm) peak = std::max(peak, std::abs(int(v)));
            return peak > 3000; // a real tone came out
        };
        const bool in_audo = ogg_ok("snd_shake"), ext = ogg_ok("snd_ext"), grouped = ogg_ok("snd_grouped");
        const bool part = r.FindSound("~CLIN") == clink && !r.FindSound("snd_nope");
        std::vector<uint8_t> junk = {'O', 'g', 'g', 'S', 1, 2, 3};
        const bool refuse = !vallhalla::GmReader::DecodeAudio(junk, ch, rate, pcm);
        std::printf("game sounds: listed %d, WAV %d, OGG in data file %d, own file %d, audio group %d, by part %d, junk refused %d\n",
                    open, wav, in_audo, ext, grouped, part, refuse);
        failures += !open || !wav || !in_audo || !ext || !grouped || !part || !refuse;
        romfs_extra.clear();
    }
    // ---- package font ----
    {
        auto* fe = eden_dsmod_get_font_extensions(EDEN_DSMOD_FONT_EXT_VERSION, EDEN_DSMOD_FONT_EXT_HASH);
        std::vector<uint8_t> b = {'V', 'A', 'F', 'N', 10, 0, 65, 0, 2, 0};
        const uint16_t g[14] = {16, 32, 5, 9, 0, 9, 6, 22, 32, 5, 9, 0, 9, 6};
        b.insert(b.end(), (const uint8_t*)g, (const uint8_t*)g + sizeof g);
        struct R { uint32_t line = 0, first = 0, n = 0; uint16_t adv = 0; } r;
        auto sink = [](void* p, uint32_t line, uint32_t first, const EdenDsmodFontGlyph* gl, uint32_t n) {
            auto* rr = static_cast<R*>(p); rr->line = line; rr->first = first; rr->n = n; rr->adv = gl[1].advance;
        };
        const bool ok = fe && fe->decode_font(nullptr, b.data(), b.size(), &r, sink) && r.line == 10 && r.first == 65 &&
                        r.n == 2 && r.adv == 6;
        const bool bad = fe && !fe->decode_font(nullptr, (const uint8_t*)"junk", 4, &r, sink);
        std::printf("font metrics: %s, junk refused: %s\n", ok ? "ok" : "FAIL", bad ? "ok" : "FAIL");
        failures += !ok || !bad;
    }
    // ---- drink results ----
    {
        void* i4 = mod->create(&host, "{}");
        auto tick = [&] { ints.clear(); texts.clear(); mod->tick(i4, &host); };
        auto mix = [&](std::initializer_list<int> pours, bool ice, bool age, int shake_ticks) {
            for (int k : pours) ext->on_action(i4, "pour", k);
            if (ice) ext->on_action(i4, "ice_toggle", 0);
            if (age) ext->on_action(i4, "age_toggle", 0);
            for (int i = 0; i < shake_ticks; ++i) { // shake: alternate fast right / fast left
                tick();
                ext->on_action(i4, "shake", (i / 3) % 2 ? 1 : 5);
                if (ints["va_zr_req"]) ext->on_action(i4, "zr_done", 0);
            }
            ext->on_action(i4, "shake_end", 0);
            for (int i = 0; i < 20; ++i) { tick(); if (ints["va_zr_req"]) ext->on_action(i4, "zr_done", 0); }
            std::printf("  -> result %lld %-22s %s\n", (long long)ints["va_result"], texts["va_drink_name"].c_str(),
                        texts["va_drink_src"].c_str());
        };
        {   // an empty shaker cannot be grabbed, and shaking it does nothing
            tick();
            const bool locked = ints["va_grab"] == 0 && ints["va_shaker_idle"] == 1;
            for (int i = 0; i < 60; ++i) { tick(); ext->on_action(i4, "shake", (i / 3) % 2 ? 1 : 5); }
            ext->on_action(i4, "shake_end", 0);
            tick();
            const bool nothing = ints["va_result"] == 0 && ints["va_started"] == 0;
            ext->on_action(i4, "pour", 0);
            tick();
            const bool open = ints["va_grab"] == 1 && ints["va_shaker_idle"] == 0;
            ext->on_action(i4, "reset", 0);
            std::printf("empty shaker: locked %d, shaking it does nothing %d, grabbable after a pour %d\n", locked, nothing, open);
            failures += !locked || !nothing || !open;
        }
        std::printf("Sugar Rush (2 A, 1 D, mixed):\n");
        mix({0, 0, 2}, false, false, 60);
        bool ok = ints["va_result"] == 1 && texts["va_drink_name"] == "Sugar Rush" &&
                  texts["va_drink_src"] == "module:gm:sugarsushspr:0:4";
        std::printf("Big Sugar Rush with Karmotrine (next pour starts a new drink):\n");
        mix({0, 0, 0, 0, 2, 2, 4, 4, 4}, false, false, 60);
        ok = ok && ints["va_result"] == 1 && texts["va_drink_name"] == "Sugar Rush (Big)";
        std::printf("wrong amounts:\n");
        mix({0, 1, 2}, false, false, 60);
        ok = ok && ints["va_result"] == 2 && ints["va_fail"] == 1 && texts["va_drink_src"] == "module:gm:glitchbottle:0:4";
        std::printf("Moonblast mixed instead of blended (fails), then blended with ice (works):\n");
        mix({0, 0, 0, 0, 0, 0, 2, 3, 4, 4}, true, false, 60);
        ok = ok && ints["va_result"] == 2;
        mix({0, 0, 0, 0, 0, 0, 2, 3, 4, 4}, true, false, 340);
        ok = ok && ints["va_result"] == 1 && texts["va_drink_name"] == "Moonblast";
        {   // a failed drink: windows pop one by one, block, close on tap
            mix({0, 1, 2}, false, false, 60);
            int shown_early = int(ints["va_err0"] + ints["va_err3"]);
            for (int i = 0; i < 60; ++i) tick();
            int all = 0;
            for (int i = 0; i < 4; ++i) all += int(ints["va_err" + std::to_string(i)]);
            const bool blocked = ints["va_err_any"] == 1;
            for (int i = 3; i >= 1; --i) ext->on_action(i4, "close_err", i);
            tick();
            const bool still = ints["va_err_any"] == 1 && ints["va_err0"] == 1;
            ext->on_action(i4, "close_err", 0);
            tick();
            std::printf("errors: early %d, all shown %d, blocked %d, one left blocks %d, cleared %d (broken glass still %lld)\n",
                        shown_early, all, blocked, still, ints["va_err_any"] == 0, (long long)ints["va_fail"]);
            ok = ok && all == 4 && blocked && still && ints["va_err_any"] == 0 && ints["va_fail"] == 1;
            int lo = 9999, hi = 0;
            for (int i = 0; i < 60; ++i) { tick(); lo = std::min<int>(lo, int(ints["va_reset_scale"])); hi = std::max<int>(hi, int(ints["va_reset_scale"])); }
            const bool lock_ok = ints["va_fail_lock"] == 1 && lo == 1000 && hi == 1070;
            std::printf("after the errors: bar locked except Reset %d, Reset pulses %d..%d\n", int(ints["va_fail_lock"]), lo, hi);
            ok = ok && lock_ok;
        }
        ext->on_action(i4, "reset", 0);
        tick();
        ok = ok && ints["va_result"] == 0 && ints["va_fail_lock"] == 0 && ints["va_reset_scale"] == 1000;
        std::printf("drinks: %s\n", ok ? "ok" : "FAIL");
        failures += !ok;
        mod->destroy(i4);
    }
    {   // the "~name" lookup used for drink sprites
        EdenDsmodHostApi h5 = host;
        h5.read_romfs = ReadRomfs;
        void* i5 = mod->create(&h5, "{}");
        struct Got { uint32_t w = 0; } got;
        auto sink = [](void* r, uint32_t ww, uint32_t, const uint8_t*, size_t) { static_cast<Got*>(r)->w = ww; };
        const bool ok = ext->load_image(i5, &h5, "module:gm:~Shaker:0:1", &got, sink) && got.w == 16;
        std::printf("sprite by partial name: %s\n", ok ? "ok" : "FAIL");
        failures += !ok;
        mod->destroy(i5);
    }
    std::printf("%s\n", failures ? "VALLHALLA FAIL" : "VALLHALLA OK");
    return failures ? 1 : 0;
}
