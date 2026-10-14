// Test helper: a synthetic GameMaker data file (GMS2-style sprites with the -1 header, a
// background, two PNG texture pages). Sprites: spr_shaker (2 frames, 16x32: texture 0 at x 0
// and x 16; pixel (0,0) of texture 0 has red = 1), spr_ui_slot, spr_unrelated; background
// bg_doodle (64x48, texture 1). Sounds: snd_clink (embedded WAV, AUDO 0), snd_shake (OGG in
// AUDO 1), snd_ext (its own file snd_ext.ogg), snd_grouped (audiogroup1.dat, AUDO 0).
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
// (the includer provides stb_image_write.h with its implementation)
#include "test_ogg.h"

namespace vallhalla_test {
struct W { // little writer with back-patching
    std::vector<uint8_t>& b;
    size_t pos() const { return b.size(); }
    void u32(uint32_t v) { b.insert(b.end(), (uint8_t*)&v, (uint8_t*)&v + 4); }
    void u16(uint16_t v) { b.insert(b.end(), (uint8_t*)&v, (uint8_t*)&v + 2); }
    void at32(size_t p, uint32_t v) { std::memcpy(&b[p], &v, 4); }
    void bytes(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
};
inline void PngSink(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
}
inline std::vector<uint8_t> Png(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int i = 0; i < w * h; ++i) { px[i * 4] = r; px[i * 4 + 1] = g; px[i * 4 + 2] = b; px[i * 4 + 3] = 255; }
    px[0] = 1; // a marker pixel at (0,0)
    std::vector<uint8_t> out;
    stbi_write_png_to_func(PngSink, &out, w, h, 4, px.data(), w * 4);
    return out;
}

inline std::vector<uint8_t> Ogg() {
    return std::vector<uint8_t>(kToneOgg, kToneOgg + sizeof kToneOgg);
}
inline std::vector<uint8_t> Wav16(std::vector<int16_t> s) { // mono 22050 Hz
    std::vector<uint8_t> b;
    W w{b};
    const uint32_t data = uint32_t(s.size() * 2);
    w.bytes("RIFF", 4); w.u32(36 + data); w.bytes("WAVE", 4);
    w.bytes("fmt ", 4); w.u32(16); w.u16(1); w.u16(1); w.u32(22050); w.u32(44100); w.u16(2); w.u16(16);
    w.bytes("data", 4); w.u32(data); w.bytes(s.data(), data);
    return b;
}
inline void AppendAudo(W& w, const std::vector<std::vector<uint8_t>>& items) {
    w.u32(uint32_t(items.size()));
    const size_t ptrs = w.pos();
    for (size_t i = 0; i < items.size(); ++i) w.u32(0);
    for (size_t i = 0; i < items.size(); ++i) {
        while (w.pos() % 4) w.b.push_back(0);
        w.at32(ptrs + 4 * i, uint32_t(w.pos()));
        w.u32(uint32_t(items[i].size()));
        w.bytes(items[i].data(), items[i].size());
    }
}
// audiogroup1.dat: a FORM holding only an AUDO chunk
inline std::vector<uint8_t> BuildFakeAudioGroup() {
    std::vector<uint8_t> file;
    W w{file};
    file.insert(file.end(), {'F', 'O', 'R', 'M'});
    w.u32(0);
    w.bytes("AUDO", 4);
    const size_t p = w.pos();
    w.u32(0);
    AppendAudo(w, {Ogg()});
    w.at32(p, uint32_t(w.pos() - p - 4));
    w.at32(4, uint32_t(file.size() - 8));
    return file;
}

inline std::vector<uint8_t> BuildFakeGm() {
    std::vector<uint8_t> file;
    W w{file};
    file.insert(file.end(), {'F', 'O', 'R', 'M'});
    w.u32(0);
    auto chunk = [&](const char* tag) { w.bytes(tag, 4); size_t p = w.pos(); w.u32(0); return p; };
    auto end_chunk = [&](size_t p) { w.at32(p, uint32_t(w.pos() - p - 4)); };
    // STRG first so names have offsets
    size_t strg = chunk("STRG");
    std::map<std::string, uint32_t> str;
    const char* names[] = {"VA11HallA", "spr_shaker", "spr_ui_slot", "spr_unrelated", "bg_doodle",
                           "snd_clink", "snd_shake", "snd_ext", "snd_grouped", ".wav", ".ogg", "snd_ext.ogg"};
    const int nn = int(sizeof names / sizeof names[0]);
    w.u32(uint32_t(nn));
    for (int i = 0; i < nn; ++i) w.u32(0);
    for (int i = 0; i < nn; ++i) {
        w.at32(strg + 8 + 4 * i, uint32_t(w.pos()));
        w.u32(uint32_t(std::strlen(names[i])));
        str[names[i]] = uint32_t(w.pos());
        w.bytes(names[i], std::strlen(names[i]) + 1);
    }
    end_chunk(strg);
    size_t gen = chunk("GEN8");
    { size_t g0 = w.pos(); for (int i = 0; i < 32; ++i) w.u32(0); file[g0 + 1] = 17; w.at32(g0 + 0x28, str["VA11HallA"]); w.at32(g0 + 0x2C, 2); }
    end_chunk(gen);
    // TPAG: 3 entries (shaker frames 0 and 1 on texture 0, background on texture 1)
    size_t tpag = chunk("TPAG");
    w.u32(3);
    size_t tp_ptrs = w.pos();
    for (int i = 0; i < 3; ++i) w.u32(0);
    uint32_t tp[3];
    auto tpe = [&](int i, uint16_t sx, uint16_t sy, uint16_t sw, uint16_t sh, uint16_t bw, uint16_t bh, uint16_t tex) {
        tp[i] = uint32_t(w.pos());
        w.at32(tp_ptrs + 4 * i, tp[i]);
        for (uint16_t v : {sx, sy, sw, sh, uint16_t(0), uint16_t(0), sw, sh, bw, bh, tex}) w.u16(v);
    };
    tpe(0, 0, 0, 16, 32, 16, 32, 0);
    tpe(1, 16, 0, 16, 32, 16, 32, 0);
    tpe(2, 0, 0, 64, 48, 64, 48, 1);
    end_chunk(tpag);
    // SPRT: GMS2 layout: 14 fields, -1, version 1, type 0, count, ptrs
    size_t sprt = chunk("SPRT");
    w.u32(3);
    size_t sp_ptrs = w.pos();
    for (int i = 0; i < 3; ++i) w.u32(0);
    auto sprite = [&](int i, const char* n, int frames) {
        w.at32(sp_ptrs + 4 * i, uint32_t(w.pos()));
        w.u32(str[n]); w.u32(16); w.u32(32);
        for (int k = 0; k < 11; ++k) w.u32(0);
        w.u32(0xFFFFFFFF); w.u32(1); w.u32(0);
        w.u32(uint32_t(frames));
        for (int k = 0; k < frames; ++k) w.u32(tp[k]);
        w.u32(0); // mask count
    };
    sprite(0, "spr_shaker", 2);
    sprite(1, "spr_ui_slot", 1);
    sprite(2, "spr_unrelated", 1);
    end_chunk(sprt);
    size_t bgnd = chunk("BGND");
    w.u32(1);
    size_t bg_ptr = w.pos();
    w.u32(0);
    w.at32(bg_ptr, uint32_t(w.pos()));
    w.u32(str["bg_doodle"]); w.u32(0); w.u32(0); w.u32(0); w.u32(tp[2]);
    end_chunk(bgnd);
    // TXTR: 2 entries (scaled, mips, blob ptr), then the PNG blobs
    size_t txtr = chunk("TXTR");
    w.u32(2);
    size_t tx_ptrs = w.pos();
    w.u32(0); w.u32(0);
    size_t ent[2];
    for (int i = 0; i < 2; ++i) { ent[i] = w.pos(); w.at32(tx_ptrs + 4 * i, uint32_t(ent[i])); w.u32(1); w.u32(0); w.u32(0); }
    const auto p0 = Png(32, 32, 200, 10, 10), p1 = Png(64, 48, 10, 10, 200);
    w.at32(ent[0] + 8, uint32_t(w.pos())); w.bytes(p0.data(), p0.size());
    w.at32(ent[1] + 8, uint32_t(w.pos())); w.bytes(p1.data(), p1.size());
    end_chunk(txtr);
    // SOND: name, flags, type, file, effects, volume, pitch, group, AUDO index
    size_t sond = chunk("SOND");
    w.u32(4);
    size_t so_ptrs = w.pos();
    for (int i = 0; i < 4; ++i) w.u32(0);
    auto sound = [&](int i, const char* n, uint32_t flags, const char* type, const char* f, uint32_t group, int32_t id) {
        w.at32(so_ptrs + 4 * i, uint32_t(w.pos()));
        const float vol = 0.8f, pitch = 1.0f;
        w.u32(str[n]); w.u32(flags); w.u32(str[type]); w.u32(f ? str[f] : str[n]); w.u32(0);
        w.bytes(&vol, 4); w.bytes(&pitch, 4); w.u32(group); w.u32(uint32_t(id));
    };
    sound(0, "snd_clink", 0x65, ".wav", nullptr, 0, 0);
    sound(1, "snd_shake", 0x66, ".ogg", nullptr, 0, 1);
    sound(2, "snd_ext", 0x64, ".ogg", "snd_ext.ogg", 0, -1);
    sound(3, "snd_grouped", 0x66, ".ogg", nullptr, 1, 0);
    end_chunk(sond);
    size_t audo = chunk("AUDO");
    AppendAudo(w, {Wav16({100, -100, 200, -200, 300}), Ogg()});
    end_chunk(audo);
    w.at32(4, uint32_t(file.size() - 8));
    return file;
}
} // namespace vallhalla_test
