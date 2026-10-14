// SPDX-License-Identifier: GPL-3.0-or-later
// Reads sprites out of the player's own copy of a GameMaker game (VA-11 Hall-A) through the
// dual-screen host's read_romfs, so a companion can show the game's art without shipping it.
//
//   GmReader r;
//   r.Open(host);                       // finds the data file and indexes it (once)
//   r.Frame("adelhyde", 0, 5, w, h, px);  // that sprite's frame 0, scaled x5 (nearest), RGBA8
//
// Names match ignoring case and anything that is not a letter or digit ("shaker_spr",
// "Shaker Spr" and "SHAKERSPR" are the same). Supports the chunk layouts of GameMaker 1.4 and
// 2.x (sprite records with or without the -1 header; backgrounds), PNG and GameMaker QOI
// texture pages. Not thread-safe: the owner serialises calls (the asset worker).

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "stb_image.h" // implementation compiled once by the module

namespace vallhalla {

class GmReader {
public:
    struct Entry {
        std::string name;
        uint32_t w = 0, h = 0;
        bool background = false;
        std::vector<uint32_t> tpags; // absolute offsets of TPAG entries, one per frame
    };

    static std::string Key(const std::string& s) {
        std::string k;
        for (unsigned char c : s)
            if (std::isalnum(c)) k += char(std::tolower(c));
        return k;
    }

    bool Ready() const { return ready_; }
    const std::string& Path() const { return path_; }
    const std::vector<Entry>& Entries() const { return entries_; }

    // Finds and indexes the data file. Returns false (and can be called again later) while the
    // game's romfs is not readable yet.
    bool Open(const EdenDsmodHostApi& host) {
        if (ready_) return true;
        host_ = host;
        static const char* const kCandidates[] = {
            "game.unx", "data.win", "game.win", "game.droid", "game.ios", "game.dat",
            "assets/game.unx", "assets/data.win", "assets/game.droid", "romfs/game.unx", "Game.unx",
        };
        path_.clear();
        for (const char* c : kCandidates) {
            const size_t n = host_.read_romfs ? host_.read_romfs(host_.userdata, c, 0, nullptr, 0) : 0;
            if (n < 8) continue;
            char magic[4] = {};
            path_ = c;
            Read(0, magic, 4);
            if (std::memcmp(magic, "FORM", 4) == 0) {
                size_ = n;
                break;
            }
            path_.clear();
        }
        if (path_.empty()) return false;
        Index();
        ready_ = true;
        return true;
    }

    // Exact name (ignoring case and punctuation). A name starting with '~' instead searches the
    // names that contain the rest ("~sugar rush" finds "sugar_rush_spr"); an optional
    // "@WxH" suffix ("~sugar rush@35x53") prefers sprites of that size. Ranking: has frames,
    // then the size, then a name ending in "spr", then the shortest name.
    const Entry* Find(const std::string& name) const {
        if (!name.empty() && name[0] == '~') {
            std::string q = name.substr(1);
            uint32_t pw = 0, ph = 0;
            if (const size_t at = q.rfind('@'); at != std::string::npos) {
                std::sscanf(q.c_str() + at + 1, "%ux%u", &pw, &ph);
                q = q.substr(0, at);
            }
            const std::string want = Key(q);
            const Entry* best = nullptr;
            long best_score = -1;
            for (const auto& [k, i] : by_key_) {
                if (want.empty() || k.find(want) == std::string::npos) continue;
                const Entry& e = entries_[i];
                long score = 0;
                if (!e.tpags.empty()) score += 1000000;
                if (pw && e.w == pw && e.h == ph) score += 100000;
                if (k.size() >= 3 && k.compare(k.size() - 3, 3, "spr") == 0) score += 10000;
                score += 1000 - long(std::min<size_t>(k.size(), 999));
                if (score > best_score) {
                    best = &e;
                    best_score = score;
                }
            }
            return best;
        }
        const auto it = by_key_.find(Key(name));
        return it == by_key_.end() ? nullptr : &entries_[it->second];
    }

    // One frame on its full bounding box, scaled by an integer factor (nearest neighbour).
    bool Frame(const std::string& name, size_t frame, int scale, int& out_w, int& out_h,
               std::vector<uint8_t>& out) {
        const Entry* e = Find(name);
        if (!e || e->tpags.empty()) return false;
        frame = std::min(frame, e->tpags.size() - 1);
        const uint8_t* t = TpagAt(e->tpags[frame]);
        if (!t) return false;
        const int sx = U16(t), sy = U16(t + 2), sw = U16(t + 4), sh = U16(t + 6);
        const int tx = U16(t + 8), ty = U16(t + 10);
        const int bw = std::max<int>(1, U16(t + 16)), bh = std::max<int>(1, U16(t + 18));
        const int tid = U16(t + 20);
        int w = 0, h = 0;
        const std::vector<uint8_t>* px = Texture(tid, w, h);
        if (!px) return false;
        scale = std::clamp(scale, 1, 16);
        out_w = std::min(4096, bw * scale);
        out_h = std::min(4096, bh * scale);
        out.assign(size_t(out_w) * out_h * 4, 0);
        for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x) {
                const int gx = sx + x, gy = sy + y;
                if (gx >= w || gy >= h) continue;
                const uint8_t* src = &(*px)[(size_t(gy) * w + gx) * 4];
                for (int dy = 0; dy < scale; ++dy)
                    for (int dx = 0; dx < scale; ++dx) {
                        const int ox = (tx + x) * scale + dx, oy = (ty + y) * scale + dy;
                        if (ox >= out_w || oy >= out_h) continue;
                        std::memcpy(&out[(size_t(oy) * out_w + ox) * 4], src, 4);
                    }
            }
        return true;
    }

private:
    struct Chunk {
        char tag[5] = {};
        uint64_t off = 0, size = 0;
    };
    struct Tex {
        uint64_t off = 0, size = 0;
        int fmt = 0; // 1 png, 2 qoi, 3 bz2+qoi (unsupported)
    };
    struct Cached {
        int id = -1;
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        uint64_t used = 0;
    };

    static uint32_t U32(const std::vector<uint8_t>& b, size_t at) {
        if (at + 4 > b.size()) return 0;
        uint32_t v;
        std::memcpy(&v, b.data() + at, 4);
        return v;
    }
    static uint16_t U16(const uint8_t* p) {
        uint16_t v;
        std::memcpy(&v, p, 2);
        return v;
    }

    size_t Read(uint64_t off, void* out, size_t n) const {
        return host_.read_romfs ? host_.read_romfs(host_.userdata, path_.c_str(), off, out, n) : 0;
    }
    std::vector<uint8_t> ReadVec(uint64_t off, uint64_t n) const {
        std::vector<uint8_t> b(static_cast<size_t>(n));
        size_t got = 0;
        while (got < n) {
            const size_t r = Read(off + got, b.data() + got, size_t(std::min<uint64_t>(n - got, 32ull << 20)));
            if (r == 0) break;
            got += r;
        }
        b.resize(got);
        return b;
    }
    const Chunk* FindChunk(const char* tag) const {
        for (const auto& c : chunks_)
            if (std::memcmp(c.tag, tag, 4) == 0) return &c;
        return nullptr;
    }
    std::string Str(uint32_t ptr) const {
        if (ptr < strg_off_ + 4 || ptr >= strg_off_ + strg_.size()) return {};
        const size_t at = ptr - strg_off_;
        const uint32_t len = U32(strg_, at - 4);
        if (len > 256 || at + len > strg_.size()) return {};
        return std::string(reinterpret_cast<const char*>(strg_.data() + at), len);
    }
    const uint8_t* TpagAt(uint32_t v) const {
        if (v < tpag_off_ || v + 22 > tpag_off_ + tpag_.size()) return nullptr;
        return tpag_.data() + (v - tpag_off_);
    }

    void Index() {
        uint64_t at = 8;
        while (at + 8 <= size_ && chunks_.size() < 64) {
            uint8_t h[8];
            if (Read(at, h, 8) != 8) break;
            Chunk c;
            std::memcpy(c.tag, h, 4);
            uint32_t sz;
            std::memcpy(&sz, h + 4, 4);
            c.off = at + 8;
            c.size = sz;
            chunks_.push_back(c);
            at += 8 + uint64_t(sz);
        }
        if (const Chunk* s = FindChunk("STRG")) {
            strg_ = ReadVec(s->off, s->size);
            strg_off_ = s->off;
        }
        if (const Chunk* t = FindChunk("TPAG")) {
            tpag_ = ReadVec(t->off, t->size);
            tpag_off_ = t->off;
        }
        if (const Chunk* c = FindChunk("SPRT")) {
            const auto b = ReadVec(c->off, c->size);
            const uint32_t n = U32(b, 0);
            for (uint32_t i = 0; i < n && 4 + 4 * i < b.size(); ++i) {
                const uint32_t p = U32(b, 4 + 4 * i);
                if (p < c->off) continue;
                const size_t r = p - c->off;
                Entry e;
                e.name = Str(U32(b, r));
                e.w = U32(b, r + 4);
                e.h = U32(b, r + 8);
                for (size_t k = 14; k < 26; ++k) { // frame count + TPAG pointers after the fixed fields
                    const uint32_t cnt = U32(b, r + 4 * k);
                    if (cnt == 0 || cnt > 512) continue;
                    bool ok = true;
                    for (uint32_t j = 0; j < cnt && ok; ++j) ok = TpagAt(U32(b, r + 4 * (k + 1 + j))) != nullptr;
                    if (!ok) continue;
                    for (uint32_t j = 0; j < cnt; ++j) e.tpags.push_back(U32(b, r + 4 * (k + 1 + j)));
                    break;
                }
                Add(std::move(e));
            }
        }
        if (const Chunk* c = FindChunk("BGND")) {
            const auto b = ReadVec(c->off, c->size);
            const uint32_t n = U32(b, 0);
            for (uint32_t i = 0; i < n && 4 + 4 * i < b.size(); ++i) {
                const uint32_t p = U32(b, 4 + 4 * i);
                if (p < c->off) continue;
                const size_t r = p - c->off;
                Entry e;
                e.background = true;
                e.name = Str(U32(b, r));
                for (size_t k = 1; k < 16; ++k)
                    if (const uint8_t* t = TpagAt(U32(b, r + 4 * k))) {
                        e.tpags.push_back(U32(b, r + 4 * k));
                        e.w = U16(t + 16);
                        e.h = U16(t + 18);
                        break;
                    }
                Add(std::move(e));
            }
        }
        if (const Chunk* c = FindChunk("TXTR")) {
            const auto head = ReadVec(c->off, std::min<uint64_t>(c->size, 4 + 4 * 1024));
            const uint32_t n = U32(head, 0);
            std::vector<uint64_t> blobs;
            for (uint32_t i = 0; i < n && i < 1024; ++i) {
                const uint32_t p = U32(head, 4 + 4 * i);
                uint8_t ent[32] = {};
                Read(p, ent, sizeof ent);
                Tex t;
                for (int k = 0; k < 8; ++k) {
                    uint32_t q;
                    std::memcpy(&q, ent + 4 * k, 4);
                    if (q <= p || q >= c->off + c->size) continue;
                    uint8_t m[4] = {};
                    Read(q, m, 4);
                    if (m[0] == 0x89 && m[1] == 'P' && m[2] == 'N' && m[3] == 'G') t.fmt = 1;
                    else if (std::memcmp(m, "fioq", 4) == 0) t.fmt = 2;
                    else if (std::memcmp(m, "2zoq", 4) == 0) t.fmt = 3;
                    else continue;
                    t.off = q;
                    break;
                }
                textures_.push_back(t);
                if (t.off) blobs.push_back(t.off);
            }
            std::sort(blobs.begin(), blobs.end());
            for (auto& t : textures_) {
                if (!t.off) continue;
                const auto it = std::upper_bound(blobs.begin(), blobs.end(), t.off);
                t.size = (it == blobs.end() ? c->off + c->size : *it) - t.off;
            }
        }
    }

    void Add(Entry e) {
        const std::string k = Key(e.name);
        if (!k.empty() && !by_key_.count(k)) by_key_[k] = entries_.size();
        entries_.push_back(std::move(e));
    }

    static bool DecodeQoi(const std::vector<uint8_t>& in, int& w, int& h, std::vector<uint8_t>& out) {
        if (in.size() < 12 || std::memcmp(in.data(), "fioq", 4) != 0) return false;
        w = U16(in.data() + 4);
        h = U16(in.data() + 6);
        uint32_t len;
        std::memcpy(&len, in.data() + 8, 4);
        size_t p = 12;
        const size_t end = std::min<size_t>(in.size(), 12 + size_t(len));
        out.assign(size_t(w) * h * 4, 0);
        uint8_t px[4] = {0, 0, 0, 255};
        uint8_t index[64][4] = {};
        int run = 0;
        for (size_t i = 0; i < out.size(); i += 4) {
            if (run > 0) {
                --run;
            } else if (p < end) {
                const uint8_t b1 = in[p++];
                if ((b1 & 0xC0) == 0x00) {
                    std::memcpy(px, index[b1], 4);
                } else if ((b1 & 0xE0) == 0x40) {
                    run = b1 & 0x1F;
                } else if ((b1 & 0xE0) == 0x60) {
                    if (p >= end) break;
                    run = (((b1 & 0x1F) << 8) | in[p++]) + 32;
                } else if ((b1 & 0xC0) == 0x80) {
                    px[0] += ((b1 >> 4) & 3) - 2;
                    px[1] += ((b1 >> 2) & 3) - 2;
                    px[2] += (b1 & 3) - 2;
                } else if ((b1 & 0xE0) == 0xC0) {
                    if (p >= end) break;
                    const uint8_t b2 = in[p++];
                    const int vg = (b1 & 0x1F) - 16;
                    px[0] += vg + ((b2 >> 4) & 0xF) - 8;
                    px[1] += vg;
                    px[2] += vg + (b2 & 0xF) - 8;
                } else if ((b1 & 0xF0) == 0xE0) {
                    if (p + 3 > end) break;
                    const uint32_t v = (uint32_t(b1 & 0xF) << 24) | (uint32_t(in[p]) << 16) |
                                       (uint32_t(in[p + 1]) << 8) | in[p + 2];
                    p += 3;
                    px[0] += ((v >> 23) & 0x1F) - 16;
                    px[1] += ((v >> 18) & 0x1F) - 16;
                    px[2] += ((v >> 13) & 0x1F) - 16;
                    px[3] += ((v >> 8) & 0x1F) - 16;
                } else {
                    const uint8_t mask = b1 & 0x0F;
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

    // A decoded texture page; a few are kept (least recently used is dropped).
    const std::vector<uint8_t>* Texture(int id, int& w, int& h) {
        ++clock_;
        for (auto& c : cache_)
            if (c.id == id) {
                c.used = clock_;
                w = c.w;
                h = c.h;
                return &c.rgba;
            }
        if (id < 0 || size_t(id) >= textures_.size()) return nullptr;
        const Tex& t = textures_[size_t(id)];
        if (!t.off || !t.size || t.size > (96ull << 20)) return nullptr;
        const auto blob = ReadVec(t.off, t.size);
        Cached c;
        c.id = id;
        c.used = clock_;
        if (t.fmt == 1) {
            int n = 0;
            stbi_uc* px = stbi_load_from_memory(blob.data(), int(blob.size()), &c.w, &c.h, &n, 4);
            if (!px) return nullptr;
            c.rgba.assign(px, px + size_t(c.w) * c.h * 4);
            stbi_image_free(px);
        } else if (t.fmt == 2) {
            if (!DecodeQoi(blob, c.w, c.h, c.rgba)) return nullptr;
        } else {
            return nullptr;
        }
        if (cache_.size() >= 3) {
            auto oldest = std::min_element(cache_.begin(), cache_.end(),
                                           [](const Cached& a, const Cached& b) { return a.used < b.used; });
            cache_.erase(oldest);
        }
        cache_.push_back(std::move(c));
        w = cache_.back().w;
        h = cache_.back().h;
        return &cache_.back().rgba;
    }

    EdenDsmodHostApi host_{};
    bool ready_ = false;
    std::string path_;
    uint64_t size_ = 0;
    std::vector<Chunk> chunks_;
    std::vector<uint8_t> strg_, tpag_;
    uint64_t strg_off_ = 0, tpag_off_ = 0;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, size_t> by_key_;
    std::vector<Tex> textures_;
    std::vector<Cached> cache_;
    uint64_t clock_ = 0;
};

} // namespace vallhalla
