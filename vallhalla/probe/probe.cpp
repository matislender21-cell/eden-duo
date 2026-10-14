// SPDX-License-Identifier: GPL-3.0-or-later
// VAllhalla probe for VA-11 Hall-A (010086500D3C8000): finds the game's GameMaker data file in
// the player's own romfs, reports its layout, lists sprites and backgrounds, and shows any of
// them on the second screen. A research tool: it ships no game data, it only reads the player's
// copy, so the release can load the art it needs the same way instead of shipping PNGs.
//
// Outputs: pr.file pr.version pr.chunks pr.counts pr.status, browser pr.mode pr.pos pr.name
// pr.info pr.src (a "module:" image key for the image widget) and pr.near0..11.
// Actions: step (argument = +-1/10/100), frame (next frame), mode (matches / all), find (next
// match of keyword group argument).
// Images: module key "spr:<index>:<frame>" -> the sprite frame, centred on a square canvas.
// Mode "sounds" lists the game's sounds instead (likely ice / shaker ones first) and decodes the
// one shown, to prove it can be played from the player's copy (nothing is shipped).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"

#include "../common/gm_reader.h" // sounds (SOND / AUDO) and their decoding

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

using namespace dsmod_sdk::int_types;

namespace {

constexpr u64 kTitleId = 0x010086500D3C8000ULL;
constexpr size_t kNear = 12;

const char* const kCandidates[] = {
    "game.unx", "data.win", "game.win", "game.droid", "game.ios", "game.dat",
    "assets/game.unx", "assets/data.win", "assets/game.droid", "romfs/game.unx", "Game.unx",
};

// Likely names of the pieces we need (lower-case substrings).
const char* const kKeywords[] = {
    "shak", "mix", "blend", "adel", "bron", "delt", "flan", "karm", "ice", "age", "doodl",
    "draw", "sketch", "bottle", "can", "glass", "button", "slot", "reset", "recipe", "book",
    "drink", "bar", "ui", "hud", "panel", "frame", "meter", "jill",
};

struct Chunk {
    std::string tag;
    u64 off;  // data start (after the 8-byte header)
    u64 size;
};

struct Entry { // a sprite or a background
    std::string name;
    u32 w = 0, h = 0;
    bool background = false;
    std::vector<u32> tpags; // absolute file offsets of TPAG entries, one per frame
};

struct Tex {
    u64 off = 0, size = 0;
    std::string fmt; // png / qoi / bz2qoi / ?
};

struct Data { // immutable once ready
    std::string path;
    u64 file_size = 0;
    std::vector<Chunk> chunks;
    std::string version;
    std::vector<Entry> entries;
    std::vector<Tex> textures;
    Chunk tpag{}, txtr{};
};

u32 U32(const std::vector<u8>& b, size_t at) {
    if (at + 4 > b.size()) return 0;
    u32 v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}
u16 U16(const u8* p) {
    u16 v;
    std::memcpy(&v, p, 2);
    return v;
}

std::string Lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// QOI ("fioq" header in GameMaker: little-endian width/height, then the QOI op stream).
bool DecodeGmQoi(const std::vector<u8>& in, int& w, int& h, std::vector<u8>& out) {
    if (in.size() < 12 || std::memcmp(in.data(), "fioq", 4) != 0) return false;
    w = U16(in.data() + 4);
    h = U16(in.data() + 6);
    u32 len;
    std::memcpy(&len, in.data() + 8, 4);
    size_t p = 12;
    const size_t end = std::min<size_t>(in.size(), 12 + size_t(len));
    out.assign(size_t(w) * h * 4, 0);
    u8 px[4] = {0, 0, 0, 255};
    u8 index[64][4] = {};
    int run = 0;
    for (size_t i = 0; i < out.size(); i += 4) {
        if (run > 0) {
            --run;
        } else if (p < end) {
            const u8 b1 = in[p++];
            if ((b1 & 0xC0) == 0x00) {
                std::memcpy(px, index[b1 ^ 0x00], 4);
            } else if ((b1 & 0xE0) == 0x40) {
                run = b1 & 0x1F;
            } else if ((b1 & 0xE0) == 0x60) {
                if (p >= end) break;
                const u8 b2 = in[p++];
                run = (((b1 & 0x1F) << 8) | b2) + 32;
            } else if ((b1 & 0xC0) == 0x80) {
                px[0] += ((b1 >> 4) & 3) - 2;
                px[1] += ((b1 >> 2) & 3) - 2;
                px[2] += (b1 & 3) - 2;
            } else if ((b1 & 0xE0) == 0xC0) {
                if (p >= end) break;
                const u8 b2 = in[p++];
                const int vg = (b1 & 0x1F) - 16;
                px[0] += vg + ((b2 >> 4) & 0xF) - 8;
                px[1] += vg;
                px[2] += vg + (b2 & 0xF) - 8;
            } else if ((b1 & 0xF0) == 0xE0) {
                if (p + 3 > end) break;
                const u8 b2 = in[p++], b3 = in[p++], b4 = in[p++];
                const u32 v = (u32(b1 & 0xF) << 24) | (u32(b2) << 16) | (u32(b3) << 8) | b4;
                px[0] += ((v >> 23) & 0x1F) - 16; // GameMaker's 4-byte diff (rare)
                px[1] += ((v >> 18) & 0x1F) - 16;
                px[2] += ((v >> 13) & 0x1F) - 16;
                px[3] += ((v >> 8) & 0x1F) - 16;
            } else if ((b1 & 0xF0) == 0xF0) {
                if (p + 4 > end) break;
                const u8 mask = b1 & 0x0F;
                if (mask & 8) px[0] = in[p++];
                if (mask & 4) px[1] = in[p++];
                if (mask & 2) px[2] = in[p++];
                if (mask & 1) px[3] = in[p++];
            }
            std::memcpy(index[(px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64], px, 4);
        }
        std::memcpy(&out[i], px, 4);
    }
    return true;
}

struct Probe {
    EdenDsmodHostApi host{};
    int step = 0;              // parse state machine
    std::string status = "Looking for the game's data file...";
    std::string tried;
    Data d;
    std::vector<u8> strg;      // STRG chunk, for names
    std::vector<u8> tpag;      // TPAG chunk (small), so frames never hit the romfs per tick
    u64 strg_off = 0;
    std::atomic<bool> ready{false};

    // browser
    int filter = 0; // 0 likely names, 1 drinks (35x53 sprites), 2 everything, 3 sounds
    vallhalla::GmReader gm;                 // sounds
    std::vector<size_t> sound_order;        // gm.Sounds() indexes, likely ones first
    std::map<size_t, std::string> sound_info; // decoded once: where it lives, length
    std::vector<size_t> matches;
    size_t pos = 0, frame = 0;
    std::vector<size_t> picked; // entry indexes the player marked as "used in my art"
    std::vector<bool> later;    // per pick: marked for a future mechanic
    bool later_mode = false;    // new picks are "later" picks
    size_t pick_page = 0;       // 40 picks per page on the picks screen

    // image worker state (asset worker thread only, plus the mutex)
    std::mutex tex_mutex;
    int cached_tex = -1;
    std::vector<u8> cached_rgba;
    int cached_w = 0, cached_h = 0;

    size_t Read(u64 off, void* out, size_t n) const {
        return host.read_romfs ? host.read_romfs(host.userdata, d.path.c_str(), off, out, n) : 0;
    }
    std::vector<u8> ReadVec(u64 off, u64 n) const {
        std::vector<u8> b;
        b.resize(size_t(n));
        size_t got = 0;
        while (got < n) {
            const size_t want = size_t(std::min<u64>(n - got, 32ull << 20));
            const size_t r = Read(off + got, b.data() + got, want);
            if (r == 0) break;
            got += r;
        }
        b.resize(got);
        return b;
    }
    const Chunk* FindChunk(const char* tag) const {
        for (const auto& c : d.chunks)
            if (c.tag == tag) return &c;
        return nullptr;
    }
    std::string Str(u32 ptr) const {
        if (ptr < strg_off + 4 || ptr >= strg_off + strg.size()) return "?";
        const size_t at = ptr - strg_off;
        const u32 len = U32(strg, at - 4);
        if (len > 200 || at + len > strg.size()) return "?";
        return std::string(reinterpret_cast<const char*>(strg.data() + at), len);
    }
    // A TPAG entry by absolute offset (22 bytes), from memory.
    const u8* TpagAt(u32 v) const {
        if (v < d.tpag.off || v + 22 > d.tpag.off + tpag.size()) return nullptr;
        return tpag.data() + (v - d.tpag.off);
    }
    bool InTpag(u32 v) const {
        return v >= d.tpag.off && v + 22 <= d.tpag.off + d.tpag.size;
    }

    void Step() {
        switch (step) {
        case 0: { // find the file
            for (const char* c : kCandidates) {
                const size_t n = host.read_romfs ? host.read_romfs(host.userdata, c, 0, nullptr, 0) : 0;
                tried += std::string(c) + (n ? "=" + std::to_string(n) : "") + " ";
                if (n >= 8) {
                    char magic[4] = {};
                    d.path = c;
                    d.file_size = n;
                    Read(0, magic, 4);
                    if (std::memcmp(magic, "FORM", 4) == 0) {
                        status = "Found " + d.path + ". Reading the layout...";
                        step = 1;
                        return;
                    }
                }
            }
            d.path.clear();
            status = "No GameMaker data file found. Tried: " + tried;
            step = 99;
            return;
        }
        case 1: { // chunk list
            u64 at = 8;
            while (at + 8 <= d.file_size && d.chunks.size() < 64) {
                u8 h[8];
                if (Read(at, h, 8) != 8) break;
                u32 sz;
                std::memcpy(&sz, h + 4, 4);
                d.chunks.push_back({std::string(reinterpret_cast<char*>(h), 4), at + 8, sz});
                at += 8 + u64(sz);
            }
            const Chunk* s = FindChunk("STRG");
            if (s) {
                strg = ReadVec(s->off, s->size);
                strg_off = s->off;
            }
            if (const Chunk* t = FindChunk("TPAG")) {
                d.tpag = *t;
                tpag = ReadVec(t->off, t->size);
            }
            if (const Chunk* t = FindChunk("TXTR")) d.txtr = *t;
            if (const Chunk* g = FindChunk("GEN8")) {
                const auto b = ReadVec(g->off, std::min<u64>(g->size, 0x80));
                char v[160];
                std::snprintf(v, sizeof v, "bytecode %u, game '%s', IDE %u.%u.%u.%u", b.size() > 1 ? b[1] : 0,
                              Str(U32(b, 0x28)).c_str(), U32(b, 0x2C), U32(b, 0x30), U32(b, 0x34), U32(b, 0x38));
                d.version = v;
            }
            step = 2;
            status = "Reading sprites...";
            return;
        }
        case 2: { // sprites
            if (const Chunk* c = FindChunk("SPRT")) {
                const auto b = ReadVec(c->off, c->size);
                const u32 n = U32(b, 0);
                for (u32 i = 0; i < n && 4 + 4 * i < b.size(); ++i) {
                    const u32 p = U32(b, 4 + 4 * i);
                    if (p < c->off) continue;
                    const size_t r = p - c->off;
                    Entry e;
                    e.name = Str(U32(b, r));
                    e.w = U32(b, r + 4);
                    e.h = U32(b, r + 8);
                    // the frame list: a count followed by that many TPAG pointers, somewhere after
                    // the 14 fixed fields (GameMaker versions add fields in between)
                    for (size_t k = 14; k < 26; ++k) {
                        const u32 cnt = U32(b, r + 4 * k);
                        if (cnt == 0 || cnt > 512) continue;
                        bool ok = true;
                        for (u32 j = 0; j < cnt && ok; ++j) ok = InTpag(U32(b, r + 4 * (k + 1 + j)));
                        if (!ok) continue;
                        for (u32 j = 0; j < cnt; ++j) e.tpags.push_back(U32(b, r + 4 * (k + 1 + j)));
                        break;
                    }
                    d.entries.push_back(std::move(e));
                }
            }
            step = 3;
            return;
        }
        case 3: { // backgrounds (big single images in older GameMaker)
            if (const Chunk* c = FindChunk("BGND")) {
                const auto b = ReadVec(c->off, c->size);
                const u32 n = U32(b, 0);
                for (u32 i = 0; i < n && 4 + 4 * i < b.size(); ++i) {
                    const u32 p = U32(b, 4 + 4 * i);
                    if (p < c->off) continue;
                    const size_t r = p - c->off;
                    Entry e;
                    e.background = true;
                    e.name = Str(U32(b, r));
                    for (size_t k = 1; k < 16; ++k)
                        if (InTpag(U32(b, r + 4 * k))) {
                            e.tpags.push_back(U32(b, r + 4 * k));
                            break;
                        }
                    if (!e.tpags.empty())
                        if (const u8* t = TpagAt(e.tpags[0])) {
                            e.w = U16(t + 16);
                            e.h = U16(t + 18);
                        }
                    d.entries.push_back(std::move(e));
                }
            }
            step = 4;
            return;
        }
        case 4: { // textures: find each blob and its format
            if (d.txtr.size) {
                const auto head = ReadVec(d.txtr.off, std::min<u64>(d.txtr.size, 4 + 4 * 1024));
                const u32 n = U32(head, 0);
                std::vector<u64> blobs;
                for (u32 i = 0; i < n && i < 1024; ++i) {
                    const u32 p = U32(head, 4 + 4 * i);
                    u8 ent[32] = {};
                    Read(p, ent, sizeof ent);
                    Tex t;
                    for (int k = 0; k < 8; ++k) {
                        u32 q;
                        std::memcpy(&q, ent + 4 * k, 4);
                        if (q <= p || q >= d.txtr.off + d.txtr.size) continue;
                        u8 m[8] = {};
                        Read(q, m, 8);
                        if (m[0] == 0x89 && m[1] == 'P' && m[2] == 'N' && m[3] == 'G') t.fmt = "png";
                        else if (std::memcmp(m, "fioq", 4) == 0) t.fmt = "qoi";
                        else if (std::memcmp(m, "2zoq", 4) == 0) t.fmt = "bz2qoi";
                        else continue;
                        t.off = q;
                        break;
                    }
                    d.textures.push_back(t);
                    if (t.off) blobs.push_back(t.off);
                }
                std::sort(blobs.begin(), blobs.end());
                for (auto& t : d.textures) {
                    if (!t.off) continue;
                    auto it = std::upper_bound(blobs.begin(), blobs.end(), t.off);
                    t.size = (it == blobs.end() ? d.txtr.off + d.txtr.size : *it) - t.off;
                }
            }
            if (gm.Open(host)) {
                const auto& snds = gm.Sounds();
                std::vector<std::pair<int, size_t>> order;
                for (size_t i = 0; i < snds.size(); ++i) {
                    const std::string n = Lower(snds[i].name);
                    int score = 0;
                    for (const char* k : {"ice", "shak", "clink", "cube", "rock", "glass", "mix", "blend", "pour"})
                        if (n.find(k) != std::string::npos) ++score;
                    order.push_back({-score, i});
                }
                std::stable_sort(order.begin(), order.end(),
                                 [](const auto& a, const auto& b) { return a.first < b.first; });
                for (const auto& o : order) sound_order.push_back(o.second);
            }
            Rebuild();
            ready = true;
            size_t sprites = 0, bgs = 0;
            std::map<std::string, int> fmts;
            for (const auto& e : d.entries) (e.background ? bgs : sprites)++;
            for (const auto& t : d.textures) fmts[t.fmt.empty() ? "?" : t.fmt]++;
            std::string f;
            for (const auto& [k, v] : fmts) f += " " + k + "=" + std::to_string(v);
            char b[200];
            std::snprintf(b, sizeof b, "%zu sprites, %zu backgrounds, %zu sounds, %zu textures:%s", sprites, bgs,
                          gm.Sounds().size(), d.textures.size(), f.c_str());
            counts = b;
            status = "Ready. Use the buttons to browse.";
            LogCatalog();
            step = 99;
            return;
        }
        default:
            return;
        }
    }

    std::string counts;

    void Rebuild() {
        matches.clear();
        if (filter == 3) {
            matches = sound_order;
            pos = 0;
            frame = 0;
            return;
        }
        for (size_t i = 0; i < d.entries.size(); ++i) {
            if (filter == 2) {
                matches.push_back(i);
                continue;
            }
            if (filter == 1) {
                if (d.entries[i].w == 35 && d.entries[i].h == 53) matches.push_back(i);
                continue;
            }
            const std::string n = Lower(d.entries[i].name);
            for (const char* k : kKeywords)
                if (n.find(k) != std::string::npos) {
                    matches.push_back(i);
                    break;
                }
        }
        pos = 0;
        frame = 0;
    }

    bool OnAction(const std::string& a, s64 arg) {
        if (!ready) return false;
        if (a == "step" && !matches.empty()) {
            const s64 n = s64(matches.size());
            pos = size_t((((s64(pos) + arg) % n) + n) % n);
            frame = 0;
            return true;
        }
        if (a == "frame" && !matches.empty() && filter != 3) {
            const auto& e = d.entries[matches[pos]];
            if (!e.tpags.empty()) frame = (frame + 1) % e.tpags.size();
            return true;
        }
        if (a == "pick" && !matches.empty() && filter == 3) {
            Log("sound pick: " + DescribeSound(matches[pos]));
            return true;
        }
        if (a == "pick" && !matches.empty()) {
            const size_t idx = matches[pos];
            auto it = std::find(picked.begin(), picked.end(), idx);
            if (it == picked.end()) {
                picked.push_back(idx);
                later.push_back(later_mode);
            } else {
                later.erase(later.begin() + (it - picked.begin()));
                picked.erase(it);
            }
            LogPicks();
            return true;
        }
        if (a == "clear_picks") {
            picked.clear();
            later.clear();
            pick_page = 0;
            return true;
        }
        if (a == "later") {
            later_mode = !later_mode;
            return true;
        }
        if (a == "pick_page") {
            const size_t pages = std::max<size_t>(1, (picked.size() + 39) / 40);
            pick_page = (pick_page + 1) % pages;
            return true;
        }
        if (a == "mode") {
            filter = (filter + 1) % 4;
            Rebuild();
            return true;
        }
        return false;
    }

    std::string Describe(size_t idx) const {
        const auto& e = d.entries[idx];
        char b[240];
        std::snprintf(b, sizeof b, "%s  %ux%u  x%zu%s", e.name.c_str(), e.w, e.h, e.tpags.size(),
                      e.background ? "  (bg)" : "");
        return b;
    }

    // "snd_ice  embedded OGG (audiogroup1.dat #12), 0.42 s stereo 44100 Hz": decodes it once.
    std::string DescribeSound(size_t i) {
        const auto& snd = gm.Sounds()[i];
        auto it = sound_info.find(i);
        if (it == sound_info.end()) {
            std::vector<u8> bytes;
            std::string where;
            int ch = 0, rate = 0;
            std::vector<int16_t> pcm;
            std::string info;
            if (!gm.SoundBytes(snd, bytes, &where, 1536u << 10)) { // music is skipped, not decoded
                info = where.empty() ? "NOT FOUND (" + snd.file + ")" : "music/long: " + where;
            } else if (!vallhalla::GmReader::DecodeAudio(bytes, ch, rate, pcm) || ch <= 0 || rate <= 0) {
                info = "found in " + where + " but can't decode (" + std::to_string(bytes.size()) + " bytes)";
            } else {
                char b[200];
                std::snprintf(b, sizeof b, "%s %.2f s %s %d Hz, from %s", bytes[0] == 'O' ? "OGG" : "WAV",
                              double(pcm.size() / size_t(ch)) / rate, ch == 1 ? "mono" : "stereo", rate, where.c_str());
                info = b;
            }
            it = sound_info.emplace(i, info).first;
        }
        return snd.name + "  " + it->second;
    }

    void Log(const std::string& line) const {
        if (host.log) host.log(host.userdata, 1, ("VAllhalla probe: " + line).c_str());
    }

    void LogPicks() const {
        Log("picks (" + std::to_string(picked.size()) + "):");
        for (size_t k = 0; k < picked.size(); ++k)
            Log(std::string(later[k] ? "  later " : "  pick ") + Describe(picked[k]));
    }

    void LogCatalog() const {
        Log("file " + d.path + ", " + d.version + ", " + counts);
        for (size_t i = 0; i < d.entries.size(); ++i) Log(std::to_string(i) + " " + Describe(i));
        for (size_t i = 0; i < gm.Sounds().size(); ++i) {
            const auto& snd = gm.Sounds()[i];
            Log("sound " + std::to_string(i) + " " + snd.name + " file '" + snd.file + "' flags " +
                std::to_string(snd.flags) + " group " + std::to_string(snd.group) + " id " +
                std::to_string(snd.audio_id));
        }
    }

    void Sample(const EdenDsmodHostApi* h) const {
        auto pt = [&](const char* n, const std::string& v) { h->publish_text(h->userdata, n, v.c_str()); };
        auto pi = [&](const char* n, s64 v) { h->publish_i64(h->userdata, n, v); };
        pt("pr.file", d.path.empty() ? "-" : d.path + " (" + std::to_string(d.file_size >> 20) + " MB)");
        pt("pr.version", d.version);
        std::string ch;
        for (const auto& c : d.chunks) ch += c.tag + " ";
        pt("pr.chunks", ch);
        pt("pr.counts", counts);
        pt("pr.status", status);
        pi("pr.ready", ready ? 1 : 0);
        pi("pr.picks", s64(picked.size()));
        const size_t pages = std::max<size_t>(1, (picked.size() + 39) / 40);
        const size_t page = std::min(pick_page, pages - 1);
        for (size_t k = 0; k < 40; ++k) {
            char n[16];
            std::snprintf(n, sizeof n, "pr.pick%zu", k);
            const size_t j = page * 40 + k;
            pt(n, j < picked.size() ? std::to_string(j + 1) + ". " + (later[j] ? "[later] " : "") + Describe(picked[j])
                                    : "");
        }
        pt("pr.pick_page", "page " + std::to_string(page + 1) + " of " + std::to_string(pages));
        pt("pr.later", later_mode ? "LATER: ON (picks are for future ideas)" : "LATER: OFF (picks are for now)");
        pt("pr.mode", filter == 0   ? "showing: likely names"
                      : filter == 1 ? "showing: drinks (35x53)"
                      : filter == 2 ? "showing: everything"
                                    : "showing: SOUNDS (ice / shaker first)");
        if (filter == 3 && ready && !matches.empty()) {
            char b[64];
            std::snprintf(b, sizeof b, "%zu / %zu", pos + 1, matches.size());
            pt("pr.pos", b);
            const auto& snd = gm.Sounds()[matches[pos]];
            pt("pr.name", snd.name);
            const auto it = sound_info.find(matches[pos]);
            pt("pr.info", it == sound_info.end() ? "(decoding...)" : it->second);
            pt("pr.picked", "");
            pt("pr.src", "");
            for (size_t k = 0; k < kNear; ++k) {
                const size_t j = pos + k + 1;
                char n[16];
                std::snprintf(n, sizeof n, "pr.near%zu", k);
                pt(n, j < matches.size() ? std::to_string(j + 1) + "  " + gm.Sounds()[matches[j]].name : "");
            }
            return;
        }
        if (!ready || matches.empty()) {
            pt("pr.src", "");
            pt("pr.name", "");
            return;
        }
        const size_t idx = matches[pos];
        const auto& e = d.entries[idx];
        char b[200];
        std::snprintf(b, sizeof b, "%zu / %zu", pos + 1, matches.size());
        pt("pr.pos", b);
        pt("pr.name", e.name);
        pt("pr.picked", std::find(picked.begin(), picked.end(), idx) != picked.end() ? "PICKED" : "");
        u32 tex = 0xFFFF;
        if (!e.tpags.empty())
            if (const u8* t = TpagAt(e.tpags[frame])) tex = U16(t + 20);
        std::snprintf(b, sizeof b, "%s %ux%u, frame %zu of %zu, texture %u", e.background ? "background" : "sprite",
                      e.w, e.h, frame + 1, e.tpags.size(), tex);
        pt("pr.info", b);
        std::snprintf(b, sizeof b, "module:spr:%zu:%zu", idx, frame);
        pt("pr.src", e.tpags.empty() ? std::string() : std::string(b));
        for (size_t k = 0; k < kNear; ++k) {
            const size_t j = pos + k + 1;
            char n[16];
            std::snprintf(n, sizeof n, "pr.near%zu", k);
            pt(n, j < matches.size() ? std::to_string(j + 1) + "  " + d.entries[matches[j]].name : "");
        }
    }

    // ---- images (asset worker) ----
    bool TexturePixels(int id, int& w, int& h, const std::vector<u8>*& rgba) {
        if (id == cached_tex) {
            w = cached_w;
            h = cached_h;
            rgba = &cached_rgba;
            return true;
        }
        if (id < 0 || size_t(id) >= d.textures.size()) return false;
        const Tex& t = d.textures[size_t(id)];
        if (!t.off || !t.size || t.size > (96ull << 20)) return false;
        const auto blob = ReadVec(t.off, t.size);
        if (t.fmt == "png") {
            int n = 0;
            stbi_uc* px = stbi_load_from_memory(blob.data(), int(blob.size()), &w, &h, &n, 4);
            if (!px) return false;
            cached_rgba.assign(px, px + size_t(w) * h * 4);
            stbi_image_free(px);
        } else if (t.fmt == "qoi") {
            if (!DecodeGmQoi(blob, w, h, cached_rgba)) return false;
        } else {
            return false; // bz2qoi: not handled by the probe yet
        }
        cached_tex = id;
        cached_w = w;
        cached_h = h;
        rgba = &cached_rgba;
        return true;
    }

    bool LoadImage(const char* key, void* receiver, EdenDsmodImageSink sink) {
        if (!ready) return false;
        size_t idx = 0, fr = 0;
        if (std::strncmp(key, "module:", 7) == 0) key += 7;
        if (std::sscanf(key, "spr:%zu:%zu", &idx, &fr) != 2 || idx >= d.entries.size()) return false;
        const auto& e = d.entries[idx];
        if (fr >= e.tpags.size()) return false;
        const u8* t = TpagAt(e.tpags[fr]);
        if (!t) return false;
        const int sx = U16(t), sy = U16(t + 2), sw = U16(t + 4), sh = U16(t + 6);
        const int tx = U16(t + 8), ty = U16(t + 10), bw = std::max<int>(1, U16(t + 16)),
                  bh = std::max<int>(1, U16(t + 18)), tid = U16(t + 20);
        std::scoped_lock lock{tex_mutex};
        int w = 0, h = 0;
        const std::vector<u8>* px = nullptr;
        if (!TexturePixels(tid, w, h, px)) return false;
        // the frame on its full bounding box, centred on a square canvas so it scales evenly
        const int side = std::min(4096, std::max(bw, bh));
        std::vector<u8> out(size_t(side) * side * 4, 0);
        const int ox = (side - bw) / 2 + tx, oy = (side - bh) / 2 + ty;
        for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x) {
                const int gx = sx + x, gy = sy + y, dx = ox + x, dy = oy + y;
                if (gx >= w || gy >= h || dx < 0 || dy < 0 || dx >= side || dy >= side) continue;
                std::memcpy(&out[(size_t(dy) * side + dx) * 4], &(*px)[(size_t(gy) * w + gx) * 4], 4);
            }
        sink(receiver, u32(side), u32(side), out.data(), out.size());
        return true;
    }
};

EdenDsmodBool SupportsBuild(const char*) {
    return EDEN_DSMOD_TRUE;
}

void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != kTitleId) return nullptr;
        auto* p = new Probe();
        p->host = *host;
        return p;
    } catch (...) {
        return nullptr;
    }
}

void Destroy(void* inst) {
    delete static_cast<Probe*>(inst);
}

void Sample(void* inst, const EdenDsmodHostApi* host) {
    try {
        static_cast<Probe*>(inst)->Sample(host);
    } catch (...) {
    }
}

void TickFn(void* inst, const EdenDsmodHostApi* host) {
    auto* p = static_cast<Probe*>(inst);
    try {
        if (!p->ready) {
            p->host = *host;
            p->Step();
        }
        if (p->ready && p->filter == 3 && !p->matches.empty()) p->DescribeSound(p->matches[p->pos]);
        p->Sample(host);
    } catch (...) {
        p->status = "The probe hit an error while reading.";
        p->step = 99;
    }
}

void Configure(void*, const EdenDsmodHostExtensions*) {}

EdenDsmodBool OnAction(void* inst, const char* action, int64_t argument) {
    try {
        return static_cast<Probe*>(inst)->OnAction(action ? action : "", argument) ? EDEN_DSMOD_TRUE
                                                                                   : EDEN_DSMOD_FALSE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

EdenDsmodBool LoadImage(void* inst, const EdenDsmodHostApi*, const char* key, void* receiver,
                        EdenDsmodImageSink sink) {
    try {
        return static_cast<Probe*>(inst)->LoadImage(key ? key : "", receiver, sink) ? EDEN_DSMOD_TRUE
                                                                                   : EDEN_DSMOD_FALSE;
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
    "VAllhalla probe",
    EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ,
    SupportsBuild,
    Create,
    Destroy,
    Sample,
    TickFn,
};

const EdenDsmodModuleExtensions kExtensions = {
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH, Configure, OnAction, LoadImage,
};

} // namespace

DSMOD_SDK_EXPORT_MODULE(kModule)
DSMOD_SDK_EXPORT_EXTENSIONS(kExtensions)
