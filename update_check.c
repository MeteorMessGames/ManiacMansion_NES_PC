#include "update_check.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#endif

#define RELEASES_URL "https://api.github.com/repos/MeteorMessGames/ManiacMansion_NES_PC/releases/latest"
#define USER_AGENT "ManiacMansionNES-update-check"

static volatile int s_state = 0;       /* 0 idle, 1 running, 2 found newer, 3 done (nothing to report) */
static char s_tag[40];
static char s_current[32];

/* Fetch the URL into buf (NUL-terminated). Returns bytes read, or -1. */
static int http_get(const char *url, char *buf, size_t cap)
{
#ifdef _WIN32
    HINTERNET net = InternetOpenA(USER_AGENT, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net) return -1;
    DWORD to = 6000;
    InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof to);
    InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof to);
    HINTERNET u = InternetOpenUrlA(net, url, "Accept: application/vnd.github+json\r\n", (DWORD)-1,
                                   INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_SECURE, 0);
    int n = -1;
    if (u) {
        size_t got = 0;
        DWORD r = 0;
        while (got + 1 < cap && InternetReadFile(u, buf + got, (DWORD)(cap - 1 - got), &r) && r) got += r;
        buf[got] = 0;
        n = (int)got;
        InternetCloseHandle(u);
    }
    InternetCloseHandle(net);
    return n;
#else
    /* Linux test builds: curl, if present. */
    char cmd[400];
    snprintf(cmd, sizeof cmd, "curl -fsS -m 6 -A " USER_AGENT " -H 'Accept: application/vnd.github+json' '%s' 2>/dev/null", url);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    size_t got = fread(buf, 1, cap - 1, p);
    buf[got] = 0;
    pclose(p);
    return got ? (int)got : -1;
#endif
}

/* "v1.2.3" or "1.2.3" -> 1002003; -1 when it doesn't parse. */
static long ver_num(const char *s)
{
    if (*s == 'v' || *s == 'V') s++;
    int a = 0, b = 0, c = 0;
    if (sscanf(s, "%d.%d.%d", &a, &b, &c) < 2) return -1;
    return (long)a * 1000000L + (long)b * 1000L + c;
}

static int worker(void *arg)
{
    (void)arg;
    static char body[16384];
    if (http_get(RELEASES_URL, body, sizeof body) > 0) {
        const char *k = strstr(body, "\"tag_name\"");
        if (k && (k = strchr(k + 10, ':')) && (k = strchr(k, '"'))) {
            k++;
            const char *e = strchr(k, '"');
            if (e && e - k < (int)sizeof s_tag) {
                memcpy(s_tag, k, (size_t)(e - k));
                s_tag[e - k] = 0;
                long latest = ver_num(s_tag), cur = ver_num(s_current);
                if (latest > 0 && cur > 0 && latest > cur) { s_state = 2; return 0; }
            }
        }
    }
    s_state = 3;
    return 0;
}

void update_check_start(const char *current, int enabled)
{
    if (!enabled || s_state != 0) return;
    snprintf(s_current, sizeof s_current, "%s", current);
    s_state = 1;
    SDL_Thread *t = SDL_CreateThread(worker, "update-check", NULL);
    if (t) SDL_DetachThread(t); else s_state = 3;
}

int update_check_poll(char *out, size_t n)
{
    if (s_state != 2) return 0;
    snprintf(out, n, "%s", s_tag);
    s_state = 3;
    return 1;
}
