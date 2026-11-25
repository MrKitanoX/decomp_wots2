// The installer (WotS2-Instalador.exe): the player's own disc in, the game
// ready to play out — nothing of the game comes with the program.
//
//   1. the disc image (.iso, or an archive holding it) is unpacked into
//      "extracted" beside the program (native/launcher.cpp does the work);
//   2. a translation patch (mods/ptbr/ptbr.wpatch, made by
//      tools/make_translation_patch.py) is applied to the disc's own files to
//      make the translation mod's files (mods/ptbr/files);
//   3. "Jogar" starts wots2.exe.
//
// Drawn with Dear ImGui in the PC settings menu's colours (lacquer and gold).
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "imgui_internal.h"

#include "native/ini.h"
#include "native/launcher.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <process.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;
using u8 = uint8_t;
using u32 = uint32_t;

namespace {

const char* const kData = "extracted";
const char* const kPatch = "mods/ptbr/ptbr.wpatch";
const char* const kModIni = "mods/ptbr/mod.ini";
const char* const kModFiles = "mods/ptbr/files";

// ── Small helpers ─────────────────────────────────────────────────────────────
u32 crc32(const u8* p, size_t n, u32 crc = 0) {
    static u32 table[256];
    static bool ready = false;
    if (!ready) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
u32 le32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
void put32(std::vector<u8>& v, u32 x) { for (int i = 0; i < 4; i++) v.push_back((u8)(x >> (8 * i))); }

bool read_file(const std::string& path, std::vector<u8>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? (size_t)size : 0);
    const bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}
bool write_file(const std::string& path, const std::vector<u8>& data) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

// ── VOLUME.DAT (FADEBABE): the entries a patch reads ──────────────────────────
u32 name_hash(const std::string& path) {          // the game's (0x141530)
    u32 h = 0;
    for (char raw : path) {
        char c = (char)toupper((unsigned char)raw);
        if (c == '/') c = '\\';
        h = (u32)(int32_t)(signed char)c + h * 19;
    }
    return h;
}
void lzss_unpack(const u8* src, size_t n, std::vector<u8>& out, u32 size) {
    u8 window[4096] = {};
    u32 wpos = 0xFEE, flags = 0;
    size_t ip = 0;
    out.clear();
    out.reserve(size);
    while (out.size() < size) {
        flags >>= 1;
        if (!(flags & 0x100)) {
            if (ip >= n) break;
            flags = src[ip++] | 0xFF00;
        }
        if (flags & 1) {
            if (ip >= n) break;
            const u8 c = src[ip++];
            out.push_back(c);
            window[wpos] = c;
            wpos = (wpos + 1) & 0xFFF;
        } else {
            if (ip + 1 >= n) break;
            const u32 pos = src[ip] | ((src[ip + 1] & 0xF0) << 4), len = (src[ip + 1] & 0x0F) + 3;
            ip += 2;
            for (u32 i = 0; i < len && out.size() < size; i++) {
                const u8 c = window[(pos + i) & 0xFFF];
                out.push_back(c);
                window[wpos] = c;
                wpos = (wpos + 1) & 0xFFF;
            }
        }
    }
    out.resize(size);
}
struct Volume {
    struct Entry { u32 offset, size, flags, storedEnd; };
    FILE* f = nullptr;
    u32 dataStart = 0;
    std::map<u32, Entry> entries;
    ~Volume() { if (f) fclose(f); }
    bool open(const std::string& path) {
        f = fopen(path.c_str(), "rb");
        u8 h[20];
        if (!f || fread(h, 1, 20, f) != 20 || memcmp(h, "\xFA\xDE\xBA\xBE", 4) != 0) return false;
        const u32 count = be32(h + 4);
        dataStart = be32(h + 12);
        std::vector<u8> raw((size_t)count * 24);
        if (fread(raw.data(), 1, raw.size(), f) != raw.size()) return false;
        for (u32 i = 0; i < count; i++) {
            const u8* p = &raw[(size_t)i * 24];
            entries[be32(p)] = {be32(p + 4), be32(p + 8), be32(p + 12), be32(p + 16)};
        }
        return true;
    }
    bool read(const std::string& path, std::vector<u8>& out) {
        auto it = entries.find(name_hash(path));
        if (it == entries.end()) return false;
        const Entry& e = it->second;
        std::vector<u8> stored(e.storedEnd - e.offset);
        fseek(f, (long)(dataStart + e.offset), SEEK_SET);
        if (fread(stored.data(), 1, stored.size(), f) != stored.size()) return false;
        if (e.flags & 2) return false;                          // (LZO: no patched file is)
        if (e.flags & 4) { lzss_unpack(stored.data(), stored.size(), out, e.size); return true; }
        stored.resize(e.size);
        out.swap(stored);
        return true;
    }
};

// ── BPS ───────────────────────────────────────────────────────────────────────
bool bps_apply(const u8* p, size_t n, const std::vector<u8>& source, std::vector<u8>& out, std::string* why) {
    if (n < 16 || memcmp(p, "BPS1", 4) != 0) { *why = "patch danificado"; return false; }
    if (crc32(p, n - 4) != le32(p + n - 4)) { *why = "patch danificado (CRC)"; return false; }
    if (crc32(source.data(), source.size()) != le32(p + n - 12)) { *why = "o arquivo do disco é diferente do esperado"; return false; }
    size_t ip = 4;
    const size_t end = n - 12;
    auto number = [&]() {
        uint64_t data = 0, shift = 1;
        while (ip < end) {
            const u8 x = p[ip++];
            data += (x & 0x7F) * shift;
            if (x & 0x80) break;
            shift <<= 7;
            data += shift;
        }
        return data;
    };
    const uint64_t sourceSize = number(), targetSize = number(), meta = number();
    ip += (size_t)meta;
    if (sourceSize != source.size()) { *why = "o arquivo do disco tem outro tamanho"; return false; }
    out.assign((size_t)targetSize, 0);
    size_t outAt = 0, sourceRel = 0, targetRel = 0;
    while (ip < end) {
        const uint64_t data = number();
        const size_t len = (size_t)(data >> 2) + 1;
        if (outAt + len > out.size()) { *why = "patch inválido"; return false; }
        switch (data & 3) {
        case 0:
            if (outAt + len > source.size()) { *why = "patch inválido"; return false; }
            memcpy(&out[outAt], &source[outAt], len);
            break;
        case 1:
            if (ip + len > end) { *why = "patch inválido"; return false; }
            memcpy(&out[outAt], p + ip, len);
            ip += len;
            break;
        case 2: case 3: {
            const uint64_t d = number();
            const int64_t off = (d & 1 ? -1 : 1) * (int64_t)(d >> 1);
            size_t& rel = (data & 3) == 2 ? sourceRel : targetRel;
            rel = (size_t)((int64_t)rel + off);
            const std::vector<u8>& from = (data & 3) == 2 ? source : out;
            if (rel + len > ((data & 3) == 2 ? source.size() : outAt + len)) { *why = "patch inválido"; return false; }
            for (size_t i = 0; i < len; i++) out[outAt + i] = from[rel + i];
            rel += len;
            break;
        }
        }
        outAt += len;
    }
    if (outAt != out.size() || crc32(out.data(), out.size()) != le32(p + n - 8)) { *why = "o resultado não confere (CRC)"; return false; }
    return true;
}

// ── task/alltask: its tasks unpacked, and packed again ─────────────────────────
// The compressor is tools/repack_task_archive.py's (the translation's), step
// for step, so the file it makes is the translation's to the byte.
std::vector<u8> lzss_pack(const std::vector<u8>& data) {
    const int PRE = 0xFEE, depth = 1024;
    std::vector<u8> buf(PRE, 0);
    buf.insert(buf.end(), data.begin(), data.end());
    const int end = (int)buf.size();
    std::unordered_map<u32, std::vector<int>> chains;
    auto key = [&](int q) { return (u32)buf[q] << 16 | (u32)buf[q + 1] << 8 | buf[q + 2]; };
    auto insert = [&](int q) {
        if (q + 3 > end) return;
        std::vector<int>& l = chains[key(q)];
        l.push_back(q);
        if (l.size() > 8192) l.erase(l.begin(), l.begin() + 4096);
    };
    auto longest = [&](int p, int* src) {
        const int limit = std::min(18, end - p);
        *src = 0;
        if (limit < 3) return 0;
        auto it = chains.find(key(p));
        if (it == chains.end() || it->second.empty()) return 0;
        const std::vector<int>& chain = it->second;
        int bestLen = 0, bestSrc = 0, tried = 0;
        const int lo = p - 4095;
        for (int i = (int)chain.size() - 1; i >= 0; i--) {
            const int s = chain[i];
            if (s < lo) break;
            if (bestLen < limit && buf[s + bestLen] != buf[p + bestLen]) {
                if (++tried >= depth) break;
                continue;
            }
            int len = 3;
            while (len < limit && buf[s + len] == buf[p + len]) len++;
            if (len > bestLen) {
                bestLen = len;
                bestSrc = s;
                if (len == limit) break;
            }
            if (++tried >= depth) break;
        }
        *src = bestSrc;
        return bestLen;
    };
    for (int q = 0; q < PRE; q++) insert(q);
    struct Token { bool literal; u8 value; int len, src; };
    std::vector<Token> tokens;
    int p = PRE;
    while (p < end) {
        int src = 0, step = 0;
        const int len = longest(p, &src);
        if (len >= 3 && len < 18 && p + 1 < end) {
            insert(p);
            int ignored = 0;
            if (longest(p + 1, &ignored) > len) {
                tokens.push_back({true, buf[p], 0, 0});
                p++;
                continue;
            }
            step = len;
            for (int q = p + 1; q < p + step; q++) insert(q);
        } else if (len >= 3) {
            step = len;
            for (int q = p; q < p + step; q++) insert(q);
        } else {
            insert(p);
            tokens.push_back({true, buf[p], 0, 0});
            p++;
            continue;
        }
        tokens.push_back({false, 0, len, src});
        p += step;
    }
    std::vector<u8> enc;
    for (size_t i = 0; i < tokens.size(); i += 8) {
        u8 flags = 0;
        std::vector<u8> body;
        for (size_t b = 0; b < 8 && i + b < tokens.size(); b++) {
            const Token& t = tokens[i + b];
            if (t.literal) {
                flags |= (u8)(1 << b);
                body.push_back(t.value);
            } else {
                const u32 w = (u32)t.src & 0xFFF;
                body.push_back((u8)(w & 0xFF));
                body.push_back((u8)(((w >> 4) & 0xF0) | (t.len - 3)));
            }
        }
        enc.push_back(flags);
        enc.insert(enc.end(), body.begin(), body.end());
    }
    std::vector<u8> out;
    put32(out, (u32)(8 + enc.size()));
    put32(out, (u32)data.size());
    out.insert(out.end(), enc.begin(), enc.end());
    out.resize((out.size() + 15) / 16 * 16, 0);
    return out;
}
u32 align_up(u32 v, u32 a) { return (v + a - 1) / a * a; }
bool unpack_tasks(const std::vector<u8>& blob, std::vector<u8>& out) {
    if (blob.size() < 16) return false;
    const u32 count = le32(&blob[0]), alignment = le32(&blob[4]), start = le32(&blob[8]);
    if (alignment == 0 || 0x10 + (size_t)count * 4 > start || start > blob.size()) return false;
    u32 offset = 0;
    std::vector<u8> task;
    for (u32 i = 0; i < count; i++) {
        const u32 size = le32(&blob[0x10 + i * 4]);
        if (size == 0) {
            put32(out, 0xFFFFFFFFu);
        } else {
            const size_t at = (size_t)start + offset;
            if (at + 8 > blob.size()) return false;
            const u32 packed = le32(&blob[at]), unpacked = le32(&blob[at + 4]);
            const size_t avail = std::min(blob.size() - at, (size_t)size + 16);
            if (packed < 8 || packed > avail) return false;
            lzss_unpack(&blob[at + 8], packed - 8, task, unpacked);
            put32(out, unpacked);
            out.insert(out.end(), task.begin(), task.end());
        }
        offset += align_up(size, alignment);
    }
    return true;
}
bool pack_tasks(const std::vector<u8>& original, const std::vector<u8>& unpacked, std::vector<u8>& out,
                const std::function<void(double)>& progress) {
    const u32 count = le32(&original[0]), alignment = le32(&original[4]), start = le32(&original[8]);
    std::vector<std::vector<u8>> blocks(count);
    size_t at = 0;
    for (u32 i = 0; i < count; i++) {
        if (at + 4 > unpacked.size()) return false;
        const u32 len = le32(&unpacked[at]);
        at += 4;
        if (len == 0xFFFFFFFFu) continue;
        if (at + len > unpacked.size()) return false;
        blocks[i] = lzss_pack(std::vector<u8>(unpacked.begin() + at, unpacked.begin() + at + len));
        at += len;
        progress((double)(i + 1) / count);
    }
    out.assign(original.begin(), original.begin() + 16);
    for (u32 i = 0; i < count; i++) put32(out, (u32)blocks[i].size());
    out.resize(start, 0);
    for (u32 i = 0; i < count; i++) {
        out.insert(out.end(), blocks[i].begin(), blocks[i].end());
        out.resize(out.size() + (align_up((u32)blocks[i].size(), alignment) - blocks[i].size()), 0);
    }
    return true;
}

// ── The work, on its own thread ───────────────────────────────────────────────
struct Job {
    std::thread thread;
    std::atomic<bool> running{false}, cancel{false};
    std::atomic<int> result{0};             // 0 none, 1 done, -1 failed
    std::atomic<float> progress{0.0f};
    std::mutex lock;
    std::string stage, message;
    void say(const std::string& s) { std::lock_guard<std::mutex> g(lock); stage = s; }
    void finish(bool ok, const std::string& m) {
        { std::lock_guard<std::mutex> g(lock); message = m; }
        result = ok ? 1 : -1;
        running = false;
    }
} job;

bool apply_translation(float from, float share, std::string* why) {
    std::vector<u8> patch;
    if (!read_file(kPatch, patch) || patch.size() < 16 || memcmp(patch.data(), "WS2PATCH", 8) != 0 || le32(&patch[8]) != 2) {
        *why = "não encontrei o patch da tradução (mods/ptbr/ptbr.wpatch)";
        return false;
    }
    Volume vol;
    if (!vol.open(std::string(kData) + "/VOLUME.DAT")) {
        *why = "não consegui abrir extracted/VOLUME.DAT";
        return false;
    }
    std::error_code ec;
    fs::remove_all(kModFiles, ec);
    const u32 count = le32(&patch[12]);
    size_t at = 16;
    for (u32 i = 0; i < count; i++) {
        if (job.cancel) { *why = "cancelado"; return false; }
        if (at + 3 > patch.size()) { *why = "patch danificado"; return false; }
        const u8 kind = patch[at];
        const u32 plen = patch[at + 1] | patch[at + 2] << 8;
        at += 3;
        const std::string path(reinterpret_cast<const char*>(&patch[at]), plen);
        at += plen;
        u32 wantSize = 0, wantCrc = 0;
        if (kind == 2) { wantSize = le32(&patch[at]); wantCrc = le32(&patch[at + 4]); at += 8; }
        const u32 blen = le32(&patch[at]);
        at += 4;
        if (at + blen > patch.size()) { *why = "patch danificado"; return false; }
        const float base = from + share * i / count;
        job.progress = base;
        std::vector<u8> source, made;
        if (!vol.read(path, source)) { *why = path + ": não está no disco"; return false; }
        std::string fail;
        if (kind == 2) {
            job.say("Traduzindo as conversas (tarefas do jogo)...");
            std::vector<u8> unpacked, translated;
            if (!unpack_tasks(source, unpacked) || !bps_apply(&patch[at], blen, unpacked, translated, &fail)) {
                *why = path + ": " + (fail.empty() ? std::string("formato inesperado") : fail);
                return false;
            }
            if (!pack_tasks(source, translated, made, [&](double f) { job.progress = base + (float)(share / count * 30.0 * f); })) {
                *why = path + ": formato inesperado";
                return false;
            }
            if (made.size() != wantSize || crc32(made.data(), made.size()) != wantCrc)
                fprintf(stderr, "[setup] %s: made again with another size or CRC (still valid)\n", path.c_str());
        } else {
            job.say("Traduzindo " + path + "...");
            if (!bps_apply(&patch[at], blen, source, made, &fail)) { *why = path + ": " + fail; return false; }
        }
        at += blen;
        if (!write_file(std::string(kModFiles) + "/" + path, made)) { *why = "não consegui gravar " + path; return false; }
    }
    ini::write(kModIni, {{"enabled", "1"}}, "mod");
    return true;
}

void run_install(std::string iso, bool extract, bool translate) {
    job.cancel = false;
    job.result = 0;
    job.progress = 0.0f;
    job.running = true;
    job.thread = std::thread([iso, extract, translate] {
        std::string why;
        const float extractShare = translate ? 0.7f : 1.0f;
        if (extract) {
            job.say("Lendo o disco...");
            const std::string fresh = std::string(kData) + ".novo";
            std::error_code ec;
            fs::remove_all(fresh, ec);
            const bool ok = launcher::extract(iso, fresh, [&](const char* what, double f) {
                job.say(std::string("Extraindo: ") + (what ? what : ""));
                job.progress = (float)f * extractShare;
                return !job.cancel.load();
            }, &why);
            if (!ok) {
                job.finish(false, "Não foi possível extrair a ISO: " + why);
                return;
            }
            fs::remove_all(std::string(kData) + ".antigo", ec);
            if (fs::exists(kData)) fs::rename(kData, std::string(kData) + ".antigo", ec);
            fs::rename(fresh, kData, ec);
            if (ec) {
                job.finish(false, "Não consegui pôr os arquivos na pasta extracted: " + ec.message());
                return;
            }
            fs::remove_all(std::string(kData) + ".antigo", ec);
        }
        if (translate) {
            if (!apply_translation(extract ? extractShare : 0.0f, extract ? 1.0f - extractShare : 1.0f, &why)) {
                job.finish(false, "A tradução não foi aplicada: " + why);
                return;
            }
        } else if (fs::exists(kModIni)) {
            ini::write(kModIni, {{"enabled", "0"}}, "mod");
        }
        job.progress = 1.0f;
        job.finish(true, translate ? "Pronto! Jogo instalado com a tradução PT-BR." : "Pronto! Jogo instalado (em inglês).");
    });
}

// ── The look (as the settings menu) ───────────────────────────────────────────
const ImU32 kNight = IM_COL32(20, 26, 51, 255);
const ImU32 kNightDeep = IM_COL32(11, 14, 30, 255);
const ImU32 kWave = IM_COL32(52, 66, 112, 140);
const ImU32 kLacquer = IM_COL32(142, 27, 27, 255);
const ImU32 kLacquerDark = IM_COL32(84, 14, 16, 255);
const ImU32 kLacquerLight = IM_COL32(184, 46, 40, 255);
const ImU32 kGold = IM_COL32(201, 162, 74, 255);
const ImU32 kGoldLight = IM_COL32(244, 214, 140, 255);
const ImU32 kCream = IM_COL32(239, 230, 207, 255);
const ImU32 kGreen = IM_COL32(120, 190, 110, 255);
const ImU32 kRed = IM_COL32(235, 110, 95, 255);

ImFont *gBody, *gTitle, *gKanji;
ImU32 mix_dark() { return IM_COL32(110, 20, 22, 255); }
float S = 1.0f;

ImU32 alpha(ImU32 c, float a) {
    return (c & ~IM_COL32_A_MASK) | ((ImU32)(((c >> IM_COL32_A_SHIFT) & 0xFF) * a) << IM_COL32_A_SHIFT);
}
void seigaiha(ImDrawList* dl, ImRect r, float radius, ImU32 fill, ImU32 line) {
    dl->PushClipRect(r.Min, r.Max, true);
    int row = 0;
    for (float y = r.Min.y; y < r.Max.y + radius; y += radius * 0.5f, row++)
        for (float x = r.Min.x - radius + ((row & 1) ? radius : 0.0f); x < r.Max.x + radius; x += radius * 2.0f) {
            dl->PathArcTo(ImVec2(x, y), radius, IM_PI, 2.0f * IM_PI, 14);
            dl->PathFillConvex(fill);
            for (float k : {0.92f, 0.68f, 0.44f}) {
                dl->PathArcTo(ImVec2(x, y), radius * k, IM_PI, 2.0f * IM_PI, 14);
                dl->PathStroke(line, 0, std::max(1.0f, radius * 0.045f));
            }
        }
    dl->PopClipRect();
}
void text(ImDrawList* dl, ImFont* f, float px, ImVec2 p, ImU32 c, const char* t, float wrap = 0.0f) {
    dl->AddText(f ? f : ImGui::GetFont(), px, ImVec2(floorf(p.x), floorf(p.y)), c, t, nullptr, wrap);
}
ImVec2 measure(ImFont* f, float px, const char* t, float wrap = 0.0f) {
    return (f ? f : ImGui::GetFont())->CalcTextSizeA(px, FLT_MAX, wrap, t);
}
std::string fit(const std::string& s, float px, float width) {
    if (measure(gBody, px, s.c_str()).x <= width) return s;
    std::string tail = s;
    while (tail.size() > 4 && measure(gBody, px, ("..." + tail).c_str()).x > width) {
        tail.erase(0, 1);
        while (!tail.empty() && (tail[0] & 0xC0) == 0x80) tail.erase(0, 1);
    }
    return "..." + tail;
}
// A lacquer button.  primary = brighter; returns pressed.
bool button(const char* id, const char* label, ImVec2 pos, ImVec2 size, bool enabled, bool primary = false) {
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    const bool lit = enabled && (ImGui::IsItemHovered() || (ImGui::IsItemFocused() && GImGui->NavCursorVisible));
    const bool held = ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImRect r(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    const float a = enabled ? 1.0f : 0.35f;
    dl->AddRectFilled(ImVec2(r.Min.x + 2 * S, r.Min.y + 3 * S), ImVec2(r.Max.x + 2 * S, r.Max.y + 3 * S), alpha(IM_COL32(0, 0, 0, 255), 0.45f * a), 4 * S);
    const ImU32 top = held ? kLacquerDark : lit ? kLacquerLight : primary ? kLacquer : mix_dark();
    dl->AddRectFilledMultiColor(r.Min, r.Max, alpha(top, a), alpha(top, a), alpha(kLacquerDark, a), alpha(kLacquerDark, a));
    dl->AddRect(r.Min, r.Max, alpha(lit ? kGoldLight : kGold, a), 4 * S, 0, (lit || primary ? 2.0f : 1.2f) * S);
    const float px = 24 * S;
    const ImVec2 ts = measure(gBody, px, label);
    text(dl, gBody, px, ImVec2(r.GetCenter().x - ts.x * 0.5f, r.GetCenter().y - px * 0.55f), alpha(lit ? kGoldLight : kCream, a), label);
    return pressed && enabled;
}

bool have_ffmpeg() {
    static int known = -1;
    if (known < 0) {
        known = fs::exists("ffmpeg.exe") ? 1 : 0;
        if (!known)
            if (const char* path = getenv("PATH")) {
                std::string all = path;
                size_t from = 0;
                while (from <= all.size() && !known) {
                    const size_t to = std::min(all.find(';', from), all.size());
                    std::error_code ec;
                    if (to > from && fs::exists(fs::path(all.substr(from, to - from)) / "ffmpeg.exe", ec)) known = 1;
                    from = to + 1;
                }
            }
    }
    return known == 1;
}
std::string ask_for_image() {
#ifdef _WIN32
    char file[MAX_PATH * 4] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = "Imagem do disco (*.iso, ou compactada)\0*.iso;*.img;*.bin;*.zip;*.7z;*.rar;*.gz;*.xz;*.zst;*.tar\0Todos os arquivos\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file;
    ofn.lpstrTitle = "Escolha a ISO do Way of the Samurai 2 (NTSC-U, SLUS-208.93)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return file;
#endif
    return "";
}
void start_game() {
#ifdef _WIN32
    _spawnl(_P_NOWAIT, "wots2.exe", "wots2.exe", nullptr);
#endif
}
void open_folder() {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", ".", nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    {   // Everything is looked for beside the program.
        char self[MAX_PATH * 4];
        if (GetModuleFileNameA(nullptr, self, sizeof self) != 0) {
            std::error_code ec;
            fs::current_path(fs::path(self).parent_path(), ec);
        }
        if (freopen("instalador.log", "w", stderr) != nullptr) setvbuf(stderr, nullptr, _IONBF, 0);
    }
#endif
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    float ddpi = 96.0f;
    SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr);
    S = std::clamp(ddpi / 96.0f, 1.0f, 2.0f) * 0.82f;
    SDL_Window* window = SDL_CreateWindow("Way of the Samurai 2 - Instalador", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          (int)(1040 * S), (int)(820 * S), SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    SDL_GL_SetSwapInterval(1);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    {
        static const ImWchar kText[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0};
        static ImVector<ImWchar> kanji;
        ImFontGlyphRangesBuilder b;
        b.AddText("侍道");
        b.BuildRanges(&kanji);
        auto load = [&](std::initializer_list<const char*> paths, float px, const ImWchar* ranges) -> ImFont* {
            for (const char* p : paths)
                if (fs::exists(p)) return io.Fonts->AddFontFromFileTTF(p, px, nullptr, ranges);
            return nullptr;
        };
        io.Fonts->AddFontDefault();
        gBody = load({"C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeui.ttf"}, 48.0f, kText);
        gTitle = load({"C:/Windows/Fonts/cambriab.ttf", "C:/Windows/Fonts/georgiab.ttf"}, 64.0f, kText);
        gKanji = load({"C:/Windows/Fonts/YuGothB.ttc", "C:/Windows/Fonts/msgothic.ttc"}, 96.0f, kanji.Data);
    }
    ImGui_ImplSDL2_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    std::string iso = argc > 1 ? argv[1] : "";
    bool translate = fs::exists(kPatch);
    std::string note;
    bool quit = false, closeWhenDone = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) quit = true;
            if (ev.type == SDL_DROPFILE) {
                if (!job.running) iso = ev.drop.file;
                SDL_free(ev.drop.file);
            }
        }
        if (job.thread.joinable() && !job.running) job.thread.join();
        const bool busy = job.running;
        const bool installed = launcher::has_game(kData);
        const bool havePatch = fs::exists(kPatch);
        std::map<std::string, std::string> mod;
        const bool ptbrOn = ini::read(kModIni, mod) && mod["mod.enabled"] != "0" && fs::exists(std::string(kModFiles) + "/Us/task/alltask");

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        const ImVec2 D = io.DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(D);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
        ImGui::Begin("##setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float px = 24 * S, small = 19 * S;

        // Background, header, footer.
        dl->AddRectFilled(ImVec2(0, 0), D, kNight);
        seigaiha(dl, ImRect(ImVec2(0, 0), D), 60 * S, kNight, kWave);
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(D.x * 0.75f, D.y), alpha(kNight, 0.95f), alpha(kNight, 0.0f), alpha(kNight, 0.0f), alpha(kNight, 0.95f));
        const float headH = 150 * S;
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(D.x, headH), kLacquerLight, kLacquer, kLacquerDark, kLacquerDark);
        dl->AddLine(ImVec2(0, headH), ImVec2(D.x, headH), kGold, 3 * S);
        dl->AddLine(ImVec2(0, headH + 5 * S), ImVec2(D.x, headH + 5 * S), alpha(kGold, 0.4f), 1 * S);
        {
            const float r = 56 * S;
            const ImVec2 c(48 * S + r, headH * 0.5f);
            dl->AddCircleFilled(c, r, alpha(kLacquerDark, 0.9f));
            dl->AddCircle(c, r, kGold, 0, 3 * S);
            dl->AddCircle(c, r - 6 * S, alpha(kGold, 0.45f), 0, 1 * S);
            const float kp = 46 * S;
            const ImVec2 ks = measure(gKanji, kp, "侍道");
            text(dl, gKanji, kp, ImVec2(c.x - ks.x * 0.5f, c.y - ks.y * 0.5f), kGoldLight, "侍道");
            const float tx = c.x + r + 28 * S;
            text(dl, gTitle, 44 * S, ImVec2(tx + 2 * S, c.y - 44 * S * 0.75f + 2 * S), alpha(IM_COL32(0, 0, 0, 255), 0.5f), "WAY OF THE SAMURAI 2");
            text(dl, gTitle, 44 * S, ImVec2(tx, c.y - 44 * S * 0.75f), kCream, "WAY OF THE SAMURAI 2");
            text(dl, gBody, px * 0.9f, ImVec2(tx + 2 * S, c.y + 16 * S), alpha(kGoldLight, 0.9f), "Instalador da versão nativa para PC");
        }
        const float footH = 74 * S;
        dl->AddRectFilled(ImVec2(0, D.y - footH), D, alpha(kNightDeep, 0.95f));
        dl->AddLine(ImVec2(0, D.y - footH), ImVec2(D.x, D.y - footH), alpha(kGold, 0.6f), 1.5f * S);
        text(dl, gBody, small, ImVec2(36 * S, D.y - footH + 14 * S), alpha(kCream, 0.7f),
             "Você precisa da sua própria cópia do jogo (NTSC-U, SLUS-208.93). Nenhum arquivo do jogo vem com este programa:\n"
             "eles são extraídos da sua ISO para a pasta \"extracted\" ao lado do instalador.", D.x - 72 * S);

        // Body.
        const float x0 = 56 * S, x1 = D.x - 56 * S;
        float y = headH + 36 * S;
        auto section = [&](const char* title) {
            const float tp = 30 * S;
            dl->AddQuadFilled(ImVec2(x0 + 8 * S, y + tp * 0.55f - 5 * S), ImVec2(x0 + 13 * S, y + tp * 0.55f),
                              ImVec2(x0 + 8 * S, y + tp * 0.55f + 5 * S), ImVec2(x0 + 3 * S, y + tp * 0.55f), kLacquerLight);
            text(dl, gTitle, tp, ImVec2(x0 + 24 * S, y), kGold, title);
            const float lx = x0 + 24 * S + measure(gTitle, tp, title).x + 16 * S;
            dl->AddRectFilledMultiColor(ImVec2(lx, y + tp * 0.6f), ImVec2(x1, y + tp * 0.6f + 1.5f * S), alpha(kGold, 0.8f), alpha(kGold, 0.0f), alpha(kGold, 0.0f), alpha(kGold, 0.8f));
            y += tp + 16 * S;
        };

        section("1. Sua cópia do jogo");
        {
            const char* state = installed ? "Jogo instalado (pasta extracted)." : "Jogo ainda não instalado.";
            dl->AddCircleFilled(ImVec2(x0 + 14 * S, y + px * 0.55f), 7 * S, installed ? kGreen : kRed);
            text(dl, gBody, px, ImVec2(x0 + 32 * S, y), kCream, state);
            y += px + 18 * S;
            const float bw = 180 * S, h = 48 * S;
            const ImRect box(ImVec2(x0, y), ImVec2(x1 - bw - 14 * S, y + h));
            dl->AddRectFilled(box.Min, box.Max, alpha(kNightDeep, 0.9f), 4 * S);
            dl->AddRect(box.Min, box.Max, alpha(kGold, 0.5f), 4 * S, 0, 1.2f * S);
            const std::string shown = iso.empty() ? std::string("Escolha a ISO (ou arraste o arquivo para esta janela)") : fit(iso, px * 0.9f, box.GetWidth() - 28 * S);
            text(dl, gBody, px * 0.9f, ImVec2(box.Min.x + 14 * S, box.GetCenter().y - px * 0.5f), alpha(kCream, iso.empty() ? 0.5f : 1.0f), shown.c_str());
            if (button("browse", "Procurar...", ImVec2(x1 - bw, y), ImVec2(bw, h), !busy)) {
                const std::string got = ask_for_image();
                if (!got.empty()) iso = got;
            }
            y += h + 34 * S;
        }

        section("2. Opções");
        {
            const float h = 48 * S;
            ImGui::SetCursorScreenPos(ImVec2(x0, y));
            ImGui::BeginDisabled(busy || !havePatch);
            const bool pressed = ImGui::InvisibleButton("ptbr", ImVec2(x1 - x0, h), ImGuiButtonFlags_EnableNav);
            const bool lit = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && GImGui->NavCursorVisible);
            ImGui::EndDisabled();
            if (pressed) translate = !translate;
            if (lit) dl->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(x1, y + h), alpha(kLacquer, 0.8f), alpha(kLacquerDark, 0.1f), alpha(kLacquerDark, 0.1f), alpha(kLacquer, 0.8f));
            const float a = havePatch ? 1.0f : 0.4f;
            text(dl, gBody, px, ImVec2(x0 + 18 * S, y + h * 0.5f - px * 0.55f), alpha(lit ? kGoldLight : kCream, a),
                 havePatch ? "Tradução PT-BR (textos do jogo em português do Brasil)" : "Tradução PT-BR (patch não encontrado)");
            const float pw = 70 * S, ph = 32 * S, cy = y + h * 0.5f;
            const ImRect pill(ImVec2(x1 - pw - 12 * S, cy - ph * 0.5f), ImVec2(x1 - 12 * S, cy + ph * 0.5f));
            const bool on = translate && havePatch;
            dl->AddRectFilled(pill.Min, pill.Max, alpha(on ? kLacquer : kNightDeep, a), ph * 0.5f);
            dl->AddRect(pill.Min, pill.Max, alpha(kGold, a), ph * 0.5f, 0, 1.5f * S);
            dl->AddCircleFilled(ImVec2(on ? pill.Max.x - ph * 0.5f : pill.Min.x + ph * 0.5f, cy), ph * 0.36f, alpha(on ? kGoldLight : kCream, a));
            y += h + 8 * S;
            {   // The movies (logos, opening, endings) are played through ffmpeg.
                const bool ff = have_ffmpeg();
                dl->AddCircleFilled(ImVec2(x0 + 24 * S, y + small * 0.6f), 5 * S, ff ? kGreen : kGold);
                text(dl, gBody, small, ImVec2(x0 + 38 * S, y), alpha(kCream, 0.75f),
                     ff ? "Vídeos do jogo: ffmpeg encontrado." :
                          "Vídeos do jogo: ffmpeg não encontrado. Ponha ffmpeg.exe nesta pasta para ver a abertura e os finais (opcional).");
                y += small + 8 * S;
            }
            if (installed) {
                const char* t = ptbrOn ? "Tradução aplicada nesta instalação." : "Tradução não aplicada nesta instalação.";
                text(dl, gBody, small, ImVec2(x0 + 18 * S, y), alpha(kCream, 0.65f), t);
                y += small + 8 * S;
            }
            y += 22 * S;
        }

        section("3. Instalar e jogar");
        {
            const float h = 56 * S, gap = 16 * S;
            const bool canExtract = !iso.empty() && fs::exists(iso);
            const bool onlyOptions = installed && !canExtract;
            const char* main = onlyOptions ? "Aplicar opções" : installed ? "Reinstalar com esta ISO" : "Instalar";
            const float w1 = 300 * S, w2 = 190 * S;
            float x = x0;
            if (button("install", main, ImVec2(x, y), ImVec2(w1, h), !busy && (canExtract || installed), true)) {
                run_install(iso, canExtract, translate && havePatch);
                note.clear();
            }
            x += w1 + gap;
            if (button("play", "Jogar", ImVec2(x, y), ImVec2(w2, h), !busy && installed)) {
                start_game();
                quit = true;
            }
            x += w2 + gap;
            if (button("folder", "Abrir pasta", ImVec2(x, y), ImVec2(w2, h), true)) open_folder();
            x += w2 + gap;
            if (button("quit", busy ? "Cancelar" : "Sair", ImVec2(x, y), ImVec2(std::max(w2 * 0.8f, x1 - x), h), true)) {
                if (busy) job.cancel = true;
                else quit = true;
            }
            y += h + 26 * S;

            // Progress, or how it went.
            const int result = job.result;
            if (busy || result != 0) {
                std::string stage, message;
                {
                    std::lock_guard<std::mutex> g(job.lock);
                    stage = job.stage;
                    message = job.message;
                }
                const float f = std::clamp(job.progress.load(), 0.0f, 1.0f);
                const ImRect bar(ImVec2(x0, y), ImVec2(x1, y + 14 * S));
                dl->AddRectFilled(bar.Min, bar.Max, kNightDeep, 7 * S);
                if (f > 0.0f)
                    dl->AddRectFilledMultiColor(bar.Min, ImVec2(bar.Min.x + bar.GetWidth() * f, bar.Max.y), kLacquerDark, kLacquerLight, kLacquerLight, kLacquerDark);
                dl->AddRect(bar.Min, bar.Max, alpha(kGold, 0.6f), 7 * S, 0, 1.2f * S);
                y += 26 * S;
                if (busy) {
                    char pct[16];
                    snprintf(pct, sizeof pct, "%d%%", (int)(f * 100));
                    text(dl, gBody, small, ImVec2(x0, y), alpha(kCream, 0.85f), (std::string(pct) + "  ·  " + fit(stage, small, x1 - x0 - 80 * S)).c_str());
                } else {
                    text(dl, gBody, px, ImVec2(x0, y), result > 0 ? kGoldLight : kRed, message.c_str(), x1 - x0);
                }
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
        if (quit && job.running) {          // (closing while it works: stop it first)
            job.cancel = true;
            closeWhenDone = true;
            quit = false;
        }
        if (closeWhenDone && !job.running) quit = true;
    }
    if (job.thread.joinable()) job.thread.join();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
