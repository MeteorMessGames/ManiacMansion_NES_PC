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

static void save_file(void)
{
    if (!s_path[0]) return;
    FILE *f = fopen(s_path, "w");
    if (!f) return;
    for (int i = 0; i < ACH_COUNT; i++)
        if (s_got[i]) fprintf(f, "%s %lld\n", k_ach[i].id, (long long)s_when[i]);
    fclose(f);
}

void ach_init(const char *dir)
{
    snprintf(s_path, sizeof s_path, "%sManiacMansion.achievements", dir ? dir : "");
    FILE *f = fopen(s_path, "r");
    if (!f) return;
    char id[96];
    long long t;
    while (fscanf(f, "%95s %lld", id, &t) == 2)
        for (int i = 0; i < ACH_COUNT; i++)
            if (!strcmp(id, k_ach[i].id)) { s_got[i] = true; s_when[i] = (time_t)t; }
    fclose(f);
}

int         ach_count(void)         { return ACH_COUNT; }
const char *ach_name(int i)         { return i >= 0 && i < ACH_COUNT ? k_ach[i].name : ""; }
const char *ach_desc(int i)         { return i >= 0 && i < ACH_COUNT ? k_ach[i].desc : ""; }
int         ach_points(int i)       { return i >= 0 && i < ACH_COUNT ? k_ach[i].points : 0; }
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
