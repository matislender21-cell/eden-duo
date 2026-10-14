// Host test for the probe: a synthetic GameMaker data file (GMS2-style sprite records with the
// -1 special header, a background, two PNG texture pages) served as the romfs file "game.unx".
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "../common/test_gmfile.h"

extern "C" const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t, uint64_t);
extern "C" const EdenDsmodModuleExtensions* eden_dsmod_get_extensions(uint32_t, uint64_t);

namespace {
std::vector<uint8_t> file;
std::map<std::string, int64_t> ints;
std::map<std::string, std::string> texts;
int failures = 0;

std::map<std::string, std::vector<uint8_t>> extra; // files next to game.unx
size_t ReadRomfs(void*, const char* path, uint64_t off, void* out, size_t n) {
    const std::vector<uint8_t>* f = std::string(path) == "game.unx" ? &file : nullptr;
    if (auto it = extra.find(path); it != extra.end()) f = &it->second;
    if (!f) return 0;
    if (!out) return f->size();
    if (off >= f->size()) return 0;
    n = std::min<size_t>(n, f->size() - off);
    std::memcpy(out, f->data() + off, n);
    return n;
}
void PubI(void*, const char* n, int64_t v) { ints[n] = v; }
void PubT(void*, const char* n, const char* v) { texts[n] = v; }
int log_lines = 0;
void Log(void*, uint32_t, const char*) { ++log_lines; }

} // namespace

int main() {
    file = vallhalla_test::BuildFakeGm();
    extra["snd_ext.ogg"] = vallhalla_test::Ogg();
    extra["audiogroup1.dat"] = vallhalla_test::BuildFakeAudioGroup();

    auto* mod = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    EdenDsmodHostApi host{};
    host.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    host.struct_size = sizeof host;
    host.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    host.title_id = 0x010086500D3C8000ULL;
    host.read_romfs = ReadRomfs;
    host.publish_i64 = PubI;
    host.publish_text = PubT;
    host.log = Log;
    void* inst = mod->create(&host, "{}");
    if (!inst || !ext->load_image) return 1;
    for (int i = 0; i < 10; ++i) mod->tick(inst, &host);
    for (const char* k : {"pr.status", "pr.file", "pr.version", "pr.chunks", "pr.counts", "pr.mode", "pr.pos", "pr.name", "pr.info", "pr.src", "pr.near0", "pr.near1"})
        std::printf("%-10s %s\n", k, texts[k].c_str());
    failures += texts["pr.name"] != "spr_shaker" || texts["pr.counts"].find("3 sprites, 1 backgrounds, 4 sounds, 2 textures: png=2") == std::string::npos;
    struct Got { uint32_t w = 0, h = 0; std::vector<uint8_t> px; } got;
    auto sink = [](void* r, uint32_t ww, uint32_t hh, const uint8_t* p, size_t n) {
        auto* g = static_cast<Got*>(r); g->w = ww; g->h = hh; g->px.assign(p, p + n);
    };
    bool ok = ext->load_image(inst, &host, texts["pr.src"].c_str(), &got, sink);
    // frame 0 = texture 0 at (0,0) 16x32, centred on a 32x32 canvas: x offset 8, marker pixel red=1
    std::printf("image frame0: ok=%d %ux%u, pixel(8,0) = %u,%u,%u,%u\n", ok, got.w, got.h, got.px[8 * 4], got.px[8 * 4 + 1], got.px[8 * 4 + 2], got.px[8 * 4 + 3]);
    failures += !ok || got.w != 32 || got.px[8 * 4] != 1 || got.px[8 * 4 + 3] != 255 || got.px[3] != 0;
    ext->on_action(inst, "frame", 0);
    texts.clear(); mod->tick(inst, &host);
    ok = ext->load_image(inst, &host, texts["pr.src"].c_str(), &got, sink);
    std::printf("image frame1: ok=%d %s pixel(8,0) red=%u\n", ok, texts["pr.info"].c_str(), got.px[8 * 4]);
    failures += !ok || got.px[8 * 4] != 200;
    ext->on_action(inst, "step", 2); // matches: shaker, ui_slot, bg_doodle
    texts.clear(); mod->tick(inst, &host);
    ok = ext->load_image(inst, &host, texts["pr.src"].c_str(), &got, sink);
    std::printf("background: %s / %s ok=%d %ux%u blue=%u\n", texts["pr.name"].c_str(), texts["pr.info"].c_str(), ok, got.w, got.h, got.px[(8 * 64 + 8) * 4 + 2]);
    failures += !ok || texts["pr.name"] != "bg_doodle" || got.w != 64;
    ext->on_action(inst, "pick", 0);           // bg_doodle
    ext->on_action(inst, "step", -2);
    ext->on_action(inst, "later", 0);
    ext->on_action(inst, "pick", 0);           // spr_shaker, as a later pick
    texts.clear(); mod->tick(inst, &host);
    std::printf("picks: %lld | %s | %s | %s\n", (long long)ints["pr.picks"], texts["pr.pick0"].c_str(),
                texts["pr.pick1"].c_str(), texts["pr.picked"].c_str());
    failures += ints["pr.picks"] != 2 || texts["pr.pick1"].find("[later] spr_shaker  16x32  x2") == std::string::npos ||
                texts["pr.picked"] != "PICKED";
    ext->on_action(inst, "mode", 0);
    texts.clear(); mod->tick(inst, &host);
    std::printf("drinks mode: %s\n", texts["pr.mode"].c_str());
    failures += texts["pr.mode"] != "showing: drinks (35x53)";
    ext->on_action(inst, "mode", 0);
    texts.clear(); mod->tick(inst, &host);
    std::printf("all mode: %s %s\n", texts["pr.mode"].c_str(), texts["pr.pos"].c_str());
    failures += texts["pr.pos"] != "1 / 4";
    ext->on_action(inst, "mode", 0);
    texts.clear(); mod->tick(inst, &host);
    std::printf("sounds mode: %s %s | %s | %s\n", texts["pr.mode"].c_str(), texts["pr.pos"].c_str(),
                texts["pr.name"].c_str(), texts["pr.info"].c_str());
    failures += texts["pr.mode"].find("SOUNDS") == std::string::npos || texts["pr.pos"] != "1 / 4" ||
                texts["pr.info"].find("WAV 0.00 s mono 22050 Hz") == std::string::npos;
    {
        bool ext_ok = false, grp_ok = false;
        for (int i = 0; i < 4; ++i) {
            ext->on_action(inst, "step", 1);
            texts.clear(); mod->tick(inst, &host);
            if (texts["pr.name"] == "snd_ext") ext_ok = texts["pr.info"].find("OGG 0.05 s mono 22050 Hz, from snd_ext.ogg") == 0;
            if (texts["pr.name"] == "snd_grouped") grp_ok = texts["pr.info"].find("from audiogroup1.dat #0") != std::string::npos;
        }
        std::printf("sounds decoded on the device: own file %d, audio group %d\n", ext_ok, grp_ok);
        failures += !ext_ok || !grp_ok;
    }
    mod->destroy(inst);
    std::printf("log lines: %d\n", log_lines);
    failures += log_lines < 5;
    std::printf("%s\n", failures ? "PROBE FAIL" : "PROBE OK");
    return failures ? 1 : 0;
}
