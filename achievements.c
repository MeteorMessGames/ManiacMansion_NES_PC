/* achievements.c - see achievements.h */
#include "achievements.h"
#include "achievements_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QMAX 64

static bool   s_got[ACH_COUNT > 0 ? ACH_COUNT : 1];
static time_t s_when[ACH_COUNT > 0 ? ACH_COUNT : 1];
static uint8_t s_hold[ACH_COUNT > 0 ? ACH_COUNT : 1];
static int    s_queue[QMAX], s_qhead = 0, s_qtail = 0;
static char   s_path[1100] = "";
static bool   s_latch[ACH_COUNT > 0 ? ACH_COUNT : 1][ACH_MAX_CONDS]; /* ACH_LEQ conditions seen true earlier this session */

/* ---- Unlock file ----
 * ManiacMansion.achievements is a small binary file, not text: "MMA1", then 16-byte records
 * { u32 id hash, u32 unix time, u64 SipHash-2-4 tag of the first 8 bytes }. A record whose tag does not
 * verify is ignored, so adding lines to the file (or hand-editing it) unlocks nothing. This stops casual
 * edits only: the key is in the program. Old plain-text files are not read. */
static const uint8_t k_key[16] = { 0x4D, 0x4D, 0xA7, 0x31, 0x9C, 0x52, 0xE0, 0x6B, 0x18, 0xD4, 0x7F, 0x03, 0xB9, 0x2A, 0xC5, 0x61 };

#define ROTL(x, b) (((x) << (b)) | ((x) >> (64 - (b))))
static uint64_t rd64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void wr64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t rd32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* SipHash-2-4 of exactly 8 bytes */
static uint64_t siphash8(const uint8_t *m)
{
    uint64_t k0 = rd64(k_key), k1 = rd64(k_key + 8);
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
#define SIPROUND do { v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32); v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2; \
    v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0; v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32); } while (0)
    uint64_t mi = rd64(m), last = (uint64_t)8 << 56;
    v3 ^= mi; SIPROUND; SIPROUND; v0 ^= mi;
    v3 ^= last; SIPROUND; SIPROUND; v0 ^= last;
    v2 ^= 0xff; SIPROUND; SIPROUND; SIPROUND; SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
#undef SIPROUND
}

static uint32_t id_hash(const char *id)   /* FNV-1a */
{
    uint32_t h = 2166136261u;
    for (; *id; id++) { h ^= (uint8_t)*id; h *= 16777619u; }
    return h;
}

static void save_file(void)
{
    if (!s_path[0]) return;
    FILE *f = fopen(s_path, "wb");
    if (!f) return;
    fwrite("MMA1", 1, 4, f);
    for (int i = 0; i < ACH_COUNT; i++) {
        if (!s_got[i]) continue;
        uint8_t rec[16];
        wr32(rec, id_hash(k_ach[i].id));
        wr32(rec + 4, (uint32_t)s_when[i]);
        wr64(rec + 8, siphash8(rec));
        fwrite(rec, 1, 16, f);
    }
    fclose(f);
}

void ach_init(const char *dir)
{
    snprintf(s_path, sizeof s_path, "%sManiacMansion.achievements", dir ? dir : "");
    FILE *f = fopen(s_path, "rb");
    if (!f) return;
    uint8_t magic[4];
    if (fread(magic, 1, 4, f) == 4 && !memcmp(magic, "MMA1", 4)) {
        uint8_t rec[16];
        while (fread(rec, 1, 16, f) == 16) {
            if (rd64(rec + 8) != siphash8(rec)) continue;            /* tampered or not ours: ignore */
            uint32_t h = rd32(rec);
            for (int i = 0; i < ACH_COUNT; i++)
                if (id_hash(k_ach[i].id) == h) { s_got[i] = true; s_when[i] = (time_t)rd32(rec + 4); }
        }
    }
    fclose(f);
}

int         ach_count(void)         { return ACH_COUNT; }
const char *ach_name(int i)         { return i >= 0 && i < ACH_COUNT ? k_ach[i].name : ""; }
const char *ach_desc(int i)         { return i >= 0 && i < ACH_COUNT ? k_ach[i].desc : ""; }
int         ach_points(int i)       { return i >= 0 && i < ACH_COUNT ? k_ach[i].points : 0; }
int         ach_rarity(int i)       { int p = ach_points(i); return p < 5 ? 0 : p < 10 ? 1 : 2; }
const char *ach_rarity_name(int t)  { static const char *const n[3] = { "Common", "Rare", "Epic" }; return n[t < 0 ? 0 : t > 2 ? 2 : t]; }
bool        ach_hidden(int i)       { return i >= 0 && i < ACH_COUNT && k_ach[i].hidden; }
bool        ach_unlocked(int i)     { return i >= 0 && i < ACH_COUNT && s_got[i]; }

int ach_unlocked_count(void)
{
    int n = 0;
    for (int i = 0; i < ACH_COUNT; i++) n += s_got[i];
    return n;
}

int ach_points_unlocked(void)
{
    int n = 0;
    for (int i = 0; i < ACH_COUNT; i++) if (s_got[i]) n += k_ach[i].points;
    return n;
}

int ach_points_total(void)
{
    int n = 0;
    for (int i = 0; i < ACH_COUNT; i++) n += k_ach[i].points;
    return n;
}

static bool cond_ok(AchRead rd, const AchCond *c, bool *latch)
{
    if (c->op == ACH_SCRIPT) {                       /* "global script <val> is running": slots 1..19 ($633D state, $63A1 id) */
        for (int i = 1; i < 0x14; i++)
            if (rd((uint16_t)(0x633D + i)) && rd((uint16_t)(0x63A1 + i)) == c->val) return true;
        return false;
    }
    uint8_t v = rd(c->addr) & c->mask;
    switch (c->op) {
    case ACH_LEQ: if (v == c->val) *latch = true; return *latch;
    case ACH_EQ: return v == c->val;
    case ACH_NE: return v != c->val;
    case ACH_GE: return v >= c->val;
    case ACH_LE: return v <= c->val;
    case ACH_GT: return v >  c->val;
    default:     return v <  c->val;
    }
}

void ach_poll(AchRead rd)
{
    bool changed = false;
    for (int i = 0; i < ACH_COUNT; i++) {
        if (s_got[i] || !k_ach[i].nconds) continue;
        bool all = true;
        /* conditions are evaluated in order and stop at the first false one, so a first "game is in play"
         * condition keeps later latch conditions from latching on title-screen / boot RAM */
        for (int c = 0; c < k_ach[i].nconds && all; c++)
            if (!cond_ok(rd, &k_ach[i].c[c], &s_latch[i][c])) all = false;
        if (!all) { s_hold[i] = 0; continue; }
        {   /* state-based triggers must hold for 6 polls (frames): ignores one-frame transients while rooms load.
             * Script-running triggers are short-lived by nature (a give handler runs for a frame or two): no hold. */
            bool scripted = false;
            for (int c = 0; c < k_ach[i].nconds; c++) if (k_ach[i].c[c].op == ACH_SCRIPT) scripted = true;
            if (!scripted && ++s_hold[i] < 6) continue;
        }
        s_got[i] = true;
        s_when[i] = time(NULL);
        if ((s_qtail + 1) % QMAX != s_qhead) { s_queue[s_qtail] = i; s_qtail = (s_qtail + 1) % QMAX; }
        changed = true;
    }
    if (changed) save_file();
}

int ach_next_toast(void)
{
    if (s_qhead == s_qtail) return -1;
    int i = s_queue[s_qhead];
    s_qhead = (s_qhead + 1) % QMAX;
    return i;
}
