// The guest-state hash: GCN_STATE_HASH=<every>[:<file>]. See state_hash_frame in runtime.h
// and "The guest-state hash" in docs/diagnostics.md.
#include "runtime.h"
#include "input_log.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kBlock = 0x100000;  // the log's per-block hashes cover 1 MB each

// An xxHash64-style mix of 64-bit words, four lanes at a time. Not a published xxHash:
// only a fast, well-mixed digest that two runs compare, so stability across builds of
// this file is all that matters.
constexpr uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull;
inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t round_(uint64_t acc, uint64_t w) { return rotl(acc + w * P2, 31) * P1; }
inline uint64_t load64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

// `len` a multiple of 32. Zeroes in place of the excluded ranges are hashed as such, so an
// excluded range keeps its length and the blocks either side keep their place.
uint64_t hash_span(const uint8_t* p, size_t len, uint64_t seed) {
    uint64_t a = seed + P1 + P2, b = seed + P2, c = seed, d = seed - P1;
    for (size_t i = 0; i < len; i += 32) {
        a = round_(a, load64(p + i));
        b = round_(b, load64(p + i + 8));
        c = round_(c, load64(p + i + 16));
        d = round_(d, load64(p + i + 24));
    }
    uint64_t h = rotl(a, 1) + rotl(b, 7) + rotl(c, 12) + rotl(d, 18) + len;
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

inline uint64_t combine(uint64_t h, uint64_t v) { return round_(h ^ v, v + P3); }

// Guest ranges left out of the hash, as [start, end) offsets into one space.
enum Space { RAM, ARAM, LC };
struct Skip { Space space; uint32_t lo, hi; };
std::mutex g_skip_m;
std::vector<Skip> g_skips;

// Where an address names: main RAM (0x80000000, 0xC0000000 or physical), ARAM given as
// "a:<offset>", or the locked cache (0xE0000000).
bool parse_range(const char* s, const char** endp, Skip& out) {
    bool aram = false;
    if (s[0] == 'a' && s[1] == ':') { aram = true; s += 2; }
    char* e;
    unsigned long lo = strtoul(s, &e, 16);
    if (e == s || (*e != '-' && *e != '+')) return false;
    const bool plus = *e == '+';
    const char* s2 = e + 1;
    unsigned long hi = strtoul(s2, &e, 16);
    if (e == s2) return false;
    if (plus) hi += lo;
    *endp = e;
    if (aram) out = {ARAM, (uint32_t)lo, (uint32_t)std::min<unsigned long>(hi, ARAM_SIZE)};
    else if ((lo & 0xF0000000u) == 0xE0000000u)
        out = {LC, (uint32_t)(lo & 0x0FFFFFFF), (uint32_t)std::min<unsigned long>(hi & 0x0FFFFFFF, LC_SIZE)};
    else out = {RAM, (uint32_t)(lo & 0x01FFFFFF), (uint32_t)std::min<unsigned long>(hi - (lo & ~0x01FFFFFFul), RAM_SIZE)};
    return out.hi > out.lo;
}

// A block of `len` bytes at offset `off` of `space`, with the skipped ranges zeroed. Copies
// only when a skip touches the block.
uint64_t hash_block(Space space, const uint8_t* base, uint32_t off, uint32_t len, std::vector<uint8_t>& scratch) {
    bool touched = false;
    for (const Skip& s : g_skips)
        if (s.space == space && s.lo < off + len && s.hi > off) touched = true;
    if (!touched) return hash_span(base + off, len, off);
    scratch.assign(base + off, base + off + len);
    for (const Skip& s : g_skips)
        if (s.space == space && s.lo < off + len && s.hi > off) {
            const uint32_t a = std::max(s.lo, off) - off, b = std::min(s.hi, off + len) - off;
            memset(scratch.data() + a, 0, b - a);
        }
    return hash_span(scratch.data(), len, off);
}

struct Config {
    uint32_t every = 0;
    FILE* out = nullptr;
};

Config& config() {
    static Config c = [] {
        Config c;
        const char* e = getenv("GCN_STATE_HASH");
        if (!e || !*e) return c;
        char* rest;
        c.every = (uint32_t)strtoul(e, &rest, 10);
        if (!c.every) return c;
        if (*rest == ':' && rest[1]) {
            c.out = fopen(rest + 1, "w");
            if (!c.out) {
                fprintf(stderr, "[hash] cannot write %s; state hash off\n", rest + 1);
                c.every = 0;
                return c;
            }
        } else {
            c.out = stderr;
        }
        if (const char* s = getenv("GCN_STATE_HASH_SKIP")) {
            for (const char* p = s; *p;) {
                Skip k;
                const char* end;
                if (!parse_range(p, &end, k)) {
                    fprintf(stderr, "[hash] GCN_STATE_HASH_SKIP: cannot read '%s'\n", p);
                    break;
                }
                g_skips.push_back(k);
                p = *end == ',' ? end + 1 : end;
            }
        }
        fprintf(c.out, "# gcn state hash v1: every %u frames; ram %u, aram %u, lc %u bytes; blocks of %u\n", c.every,
                RAM_SIZE, ARAM_SIZE, LC_SIZE, kBlock);
        for (const Skip& k : g_skips)
            fprintf(c.out, "# skip %s %08X-%08X\n", k.space == RAM ? "ram" : k.space == ARAM ? "aram" : "lc", k.lo, k.hi);
        fprintf(c.out, "# frame poll tick all ram aram lc | ram blocks | aram blocks\n");
        fflush(c.out);
        return c;
    }();
    return c;
}

}  // namespace

void state_hash_exclude(uint32_t addr, uint32_t len, bool aram) {
    std::lock_guard<std::mutex> lk(g_skip_m);
    Skip k;
    if (aram) k = {ARAM, addr, std::min<uint32_t>(addr + len, ARAM_SIZE)};
    else if ((addr & 0xF0000000u) == 0xE0000000u) k = {LC, addr & 0x0FFFFFFF, std::min<uint32_t>((addr & 0x0FFFFFFF) + len, LC_SIZE)};
    else k = {RAM, addr & 0x01FFFFFF, std::min<uint32_t>((addr & 0x01FFFFFF) + len, RAM_SIZE)};
    if (k.hi > k.lo) g_skips.push_back(k);
}

StateHash state_hash_compute(std::vector<uint64_t>* ram_blocks, std::vector<uint64_t>* aram_blocks) {
    std::lock_guard<std::mutex> lk(g_skip_m);
    static std::vector<uint8_t> scratch;
    StateHash h{};
    for (uint32_t off = 0; off < RAM_SIZE; off += kBlock) {
        const uint64_t b = hash_block(RAM, g_mem, off, kBlock, scratch);
        if (ram_blocks) ram_blocks->push_back(b);
        h.ram = combine(h.ram, b);
    }
    for (uint32_t off = 0; off < ARAM_SIZE; off += kBlock) {
        const uint64_t b = hash_block(ARAM, g_aram, off, kBlock, scratch);
        if (aram_blocks) aram_blocks->push_back(b);
        h.aram = combine(h.aram, b);
    }
    h.lc = hash_block(LC, g_mem + LC_BASE, 0, LC_SIZE, scratch);
    h.all = combine(combine(combine(h.ram, h.aram), h.lc), now_ticks());
    return h;
}

void state_hash_frame(uint32_t frame) {
    Config& c = config();
    if (!c.every || frame % c.every) return;
    static double total_ms = 0;
    static uint32_t count = 0;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint64_t> rb, ab;
    rb.reserve(RAM_SIZE / kBlock);
    ab.reserve(ARAM_SIZE / kBlock);
    const StateHash h = state_hash_compute(&rb, &ab);
    std::string line;
    char buf[96];
    snprintf(buf, sizeof buf, "%u %u %llu %016llx %016llx %016llx %016llx |", frame, input_poll_seq(),
             (unsigned long long)now_ticks(), (unsigned long long)h.all, (unsigned long long)h.ram,
             (unsigned long long)h.aram, (unsigned long long)h.lc);
    line += buf;
    for (uint64_t b : rb) { snprintf(buf, sizeof buf, " %08x", (uint32_t)(b >> 32)); line += buf; }
    line += " |";
    for (uint64_t b : ab) { snprintf(buf, sizeof buf, " %08x", (uint32_t)(b >> 32)); line += buf; }
    line += '\n';
    fputs(line.c_str(), c.out);
    fflush(c.out);
    total_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (++count == 100) fprintf(stderr, "[hash] %.2f ms a hash over the first 100\n", total_ms / 100);
}
