/* achievements.c - see achievements.h */
#include "achievements.h"
#include "achievements_table.h"
#include "rom_db.h"
#include "cyc_core.h"   /* cyc_prg_hash */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define QMAX 64

static bool   s_got[ACH_COUNT > 0 ? ACH_COUNT : 1];
static time_t s_when[ACH_COUNT > 0 ? ACH_COUNT : 1];
static uint8_t s_hold[ACH_COUNT > 0 ? ACH_COUNT : 1];
static int    s_queue[QMAX], s_qhead = 0, s_qtail = 0;
static char   s_path[1100] = "";
static bool   s_latch[ACH_COUNT > 0 ? ACH_COUNT : 1][ACH_MAX_CONDS]; /* ACH_LEQ conditions seen true earlier this session */

/* ---- Unlock file ----
 * ManiacMansion.dat (called ManiacMansion.achievements in v1.3 test builds; read and moved on the next save) is a small binary file, not text: "MMA1", then 16-byte records
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

/* Each ROM has its own progress. The prototype's records are the plain id hash (so old unlocks keep working); every other ROM's
 * are the id hash continued over "#" and its tag ("#retail", "#x<HASH of its PRG data>"). Records of the other ROM are kept untouched and written back, so both survive. */
static uint32_t s_salt_hash = 0;   /* the ROM the cached salt is for (0: not taken yet); file scope so a test can switch ROMs */
static const char *rom_salt(void)
{
    static char salt[32];
    if (!s_salt_hash) {   /* cyc_prg_hash() walks the whole PRG on every call and this is asked for every id of every record: once */
        uint32_t h = cyc_prg_hash();
        if (h == 2166136261u) return "";   /* nothing loaded yet: do not keep it */
        char tag[24];
        mm_rom_tag(h, tag, sizeof tag);   /* "" prototype, "retail", else "x<HASH>": one list per ROM */
        if (tag[0]) snprintf(salt, sizeof salt, "#%s", tag); else salt[0] = 0;
        s_salt_hash = h;
    }
    return salt;
}

static uint32_t id_hash(const char *id)   /* FNV-1a */
{
    uint32_t h = 2166136261u;
    for (; *id; id++) { h ^= (uint8_t)*id; h *= 16777619u; }
    for (const char *t = rom_salt(); *t; t++) { h ^= (uint8_t)*t; h *= 16777619u; }
    return h;
}

#define FOREIGN_MAX 1024
static uint8_t s_foreign[FOREIGN_MAX][16];   /* valid signed records that belong to the other ROM */
static int     s_nforeign = 0;

/* One verified record: ours (set the unlock) or the other ROM's (keep it as is). */
static void take_record(const uint8_t *rec)
{
    uint32_t h = rd32(rec);
    for (int i = 0; i < ACH_COUNT; i++)
        if (id_hash(k_ach[i].id) == h) { s_got[i] = true; s_when[i] = (time_t)rd32(rec + 4); return; }
    for (int i = 0; i < s_nforeign; i++) if (!memcmp(s_foreign[i], rec, 16)) return;
    if (s_nforeign < FOREIGN_MAX) memcpy(s_foreign[s_nforeign++], rec, 16);
}

static size_t build_payload(uint8_t *out)   /* "MMA1" + one signed 16-byte record per earned achievement */
{
    size_t n = 0;
    memcpy(out, "MMA1", 4); n = 4;
    for (int i = 0; i < ACH_COUNT; i++) {
        if (!s_got[i]) continue;
        wr32(out + n, id_hash(k_ach[i].id));
        wr32(out + n + 4, (uint32_t)s_when[i]);
        wr64(out + n + 8, siphash8(out + n));
        n += 16;
    }
    for (int i = 0; i < s_nforeign; i++) { memcpy(out + n, s_foreign[i], 16); n += 16; }   /* the other ROM's progress */
    return n;
}

static void load_payload(const uint8_t *p, size_t n)
{
    if (n < 4 || memcmp(p, "MMA1", 4)) return;
    for (size_t o = 4; o + 16 <= n; o += 16) {
        if (rd64(p + o + 8) != siphash8(p + o)) continue;            /* tampered or not ours: ignore */
        take_record(p + o);
    }
}

/* ---- The unlock file ----
 * ManiacMansion.achievements, next to the exe, holds every ROM's records (the signed payload above). Release builds write it encrypted:
 * "MMA2", an 8-byte nonce, then the payload XORed with a keystream (SipHash-2-4 of the nonce and a block counter, keyed with the program's
 * key). Test builds (MM_SIM) write the payload as it is ("MMA1") so the test tools can read it, unless built with MM_ACH_ENCRYPT; every build reads both. Each record keeps its
 * own tag, so a changed or invented record is ignored. The key is in the program: this keeps casual looking and editing out, it does not stop
 * a determined cheater. If the file cannot be written the unlocks stay in memory for the session. A new exe keeps the file (and so the unlocks). */
#define ACH_FILE_NAME "ManiacMansion.dat"
#define ACH_OLD_FILE_NAME "ManiacMansion.achievements"   /* the first name of the unlock file: still read, then replaced by the new name on the next save */

#define PAYLOAD_MAX (4 + (size_t)(ACH_COUNT + FOREIGN_MAX) * 16)

static void keystream_xor(uint8_t *data, size_t n, const uint8_t nonce[8])
{
    uint64_t nn = rd64(nonce);
    for (size_t o = 0, blk = 0; o < n; o += 8, blk++) {
        uint8_t m[8], ks[8];
        wr64(m, nn ^ (0x9E3779B97F4A7C15ULL * (uint64_t)(blk + 1)));
        wr64(ks, siphash8(m));
        for (size_t k = 0; k < 8 && o + k < n; k++) data[o + k] ^= ks[k];
    }
}

static void make_nonce(uint8_t nonce[8])
{
    static uint32_t counter = 0;
    uint64_t v = (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL ^ ((uint64_t)clock() << 20) ^ ((uint64_t)++counter * 0xD6E8FEB86659FD93ULL);
    v ^= (uint64_t)(uintptr_t)&v;   /* address-space layout adds a little more */
    wr64(nonce, v);
}

static void save_file(void)
{
    if (!s_path[0]) return;
    static uint8_t pay[PAYLOAD_MAX], out[12 + PAYLOAD_MAX];
    size_t pn = build_payload(pay), on;
#if defined(MM_SIM) && !defined(MM_ACH_ENCRYPT)   /* MM_ACH_ENCRYPT: a test build that writes the real (encrypted) format */
    memcpy(out, pay, pn);
    on = pn;
#else
    uint8_t nonce[8];
    make_nonce(nonce);
    memcpy(out, "MMA2", 4);
    memcpy(out + 4, nonce, 8);
    memcpy(out + 12, pay, pn);
    keystream_xor(out + 12, pn, nonce);
    on = 12 + pn;
#endif
    char tmp[1110];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(out, 1, on, f) == on;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp); return; }
    bool moved;
#ifdef _WIN32
    moved = MoveFileExA(tmp, s_path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    moved = rename(tmp, s_path) == 0;
#endif
    if (!moved) { remove(tmp); return; }
    {   /* the file under its first name has been carried over: drop it */
        char old[1110];
        size_t k = strlen(s_path), nl = strlen(ACH_FILE_NAME);
        if (k >= nl) { snprintf(old, sizeof old, "%.*s" ACH_OLD_FILE_NAME, (int)(k - nl), s_path); remove(old); }
    }
}

void ach_init(const char *dir)
{
    snprintf(s_path, sizeof s_path, "%s" ACH_FILE_NAME, dir ? dir : "");
    FILE *f = fopen(s_path, "rb");
    if (!f) {
        char old[1110];
        snprintf(old, sizeof old, "%s" ACH_OLD_FILE_NAME, dir ? dir : "");
        f = fopen(old, "rb");
    }
    if (!f) return;
    static uint8_t buf[12 + PAYLOAD_MAX];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n >= 12 && !memcmp(buf, "MMA2", 4)) {
        keystream_xor(buf + 12, n - 12, buf + 4);
        load_payload(buf + 12, n - 12);
    } else if (n >= 4 && !memcmp(buf, "MMA1", 4)) {
        load_payload(buf, n);   /* test builds, and files written before the encrypted format */
    }
}

static bool     s_resync = false;     /* set when a state was loaded; consumed by the test-build logic in ach_poll */
static unsigned s_reset_gen = 0;      /* bumped by every reset, so test builds can re-arm their once-per-session pops */
void ach_state_loaded(void) { s_resync = true; }

/* Menu switch (Options > Achievements). While off nothing is checked, banners and chimes do not play and nothing is saved.
 * Turning it back on does not hand out what became true in the meantime: every achievement whose conditions are already
 * all true at that moment is held back (s_supp) until it has been false once. */
static bool s_enabled = true;
static bool s_rearm = false;
static bool s_supp[ACH_COUNT > 0 ? ACH_COUNT : 1];
bool ach_enabled(void) { return s_enabled; }
void ach_set_enabled(bool on)
{
    if (on == s_enabled) return;
    s_enabled = on;
    memset(s_hold, 0, sizeof s_hold);
    if (on) s_rearm = true;
    else s_qhead = s_qtail = 0;   /* drop any banner still queued */
}

/* Forget every unlock (the menu's "Reset all achievements"). */
void ach_reset_all(void)
{
    s_reset_gen++;
    for (int i = 0; i < ACH_COUNT; i++) { s_got[i] = false; s_when[i] = 0; s_hold[i] = 0; }
    s_qhead = s_qtail = 0;
    save_file();
}

int         ach_count(void)         { return ACH_COUNT; }
const char *ach_id(int i)           { return i >= 0 && i < ACH_COUNT ? k_ach[i].id : ""; }
const char *ach_name(int i)         { return i >= 0 && i < ACH_COUNT ? k_ach[i].name : ""; }
const char *ach_desc(int i)         { return i >= 0 && i < ACH_COUNT ? k_ach[i].desc : ""; }
int         ach_points(int i)       { return i >= 0 && i < ACH_COUNT ? k_ach[i].points : 0; }
int         ach_rarity(int i)       { return i >= 0 && i < ACH_COUNT ? k_ach[i].rarity : 0; }
const char *ach_rarity_name(int t)  { static const char *const n[4] = { "Common", "Rare", "Epic", "Legendary" }; return n[t < 0 ? 0 : t > 3 ? 3 : t]; }
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
    if (c->op == ACH_SCRIPT_SEEN) {                  /* a script that ends right as it starts the one we want (script 12 -> 62) is never seen
                                                      * running together with it, so remember that it ran */
        for (int i = 1; i < 0x14; i++)
            if (rd((uint16_t)(0x633D + i)) && rd((uint16_t)(0x63A1 + i)) == c->val) *latch = true;
        return *latch;
    }
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

/* Boot, logo and title-screen RAM holds leftovers that satisfy single-variable triggers (Var[168] == 3 was true for the
 * first seconds after launch and popped "An Interesting Development" on the first screen), so nothing is evaluated until
 * a game is actually in play: a kid is ego, both kids are chosen and we are past the kid-select screen. */
static bool in_play(AchRead rd)
{
    uint8_t ego = rd(0x6100), room = rd(0x63C5);
    return ego >= 1 && ego <= 7 && rd(0x6130) != 0 && rd(0x6131) != 0 && room != 0 && room != 33 && room != 45 && room != 49;
}

void ach_poll(AchRead rd)
{
    bool changed = false;
    if (!s_enabled) { memset(s_hold, 0, sizeof s_hold); return; }
    if (!in_play(rd)) { memset(s_hold, 0, sizeof s_hold); return; }
    if (s_rearm) {   /* just switched back on: whatever is already true is not new */
        s_rearm = false;
        for (int i = 0; i < ACH_COUNT; i++) {
            if (s_got[i] || !k_ach[i].nconds) continue;
            bool all = true;
            for (int c = 0; c < k_ach[i].nconds && all; c++)
                if (!cond_ok(rd, &k_ach[i].c[c], &s_latch[i][c])) all = false;
            s_supp[i] = all;
        }
    }
    for (int i = 0; i < ACH_COUNT; i++) {
        if (s_got[i] || !k_ach[i].nconds) continue;
        if (s_supp[i]) {   /* held back until its conditions have been false once */
            bool all = true;
            for (int c = 0; c < k_ach[i].nconds && all; c++)
                if (!cond_ok(rd, &k_ach[i].c[c], &s_latch[i][c])) all = false;
            if (!all) { s_supp[i] = false; s_hold[i] = 0; }
            continue;
        }
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
