// Level loading: the game's level manager loads every level (front-end screens
// included) by .atr path through fn_80019700 (r4 = path). It is wrapped to log
// the path, and TUROK_LEVEL=<path to .atr> replaces the first gameplay level
// that is loaded (New Game), so any level can be tested from the front end.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ppc.h"
#include "host.h"
#include "hw.h"
#include "core_api.h"
#include "../generated/funcs.h"

#define LEVEL_PATH_ADDR 0x80001800u  // low-memory area left free by the OS (Gecko codes live here)

static int s_redirected;

// Intro skip state: fast-forward the front end and press Start on the title screen.
void core_speed_boost(float boost);
uint64_t g_auto_start_at;  // guest ticks; hle_pad.c holds Start briefly from then

static void read_guest_str(uint32_t a, char *out, size_t n) {
    size_t i = 0;
    for (; i + 1 < n; i++) { char ch = (char)rd8(a + (uint32_t)i); if (!ch) break; out[i] = ch; }
    out[i] = 0;
}

// Front-end menu state for the host (menus were drawn for 4:3 and stay pillarboxed):
// set when a front-end screen loads, cleared when a gameplay level loads. In-game
// overlays ("ingame...": pause, options, save) and screens shown from either place
// (confirmation, text input, controller prompt) leave it unchanged.
static volatile int s_in_menu = 1;
CORE_API int core_in_menu(void) { return s_in_menu; }
static void track_menu(const char *path) {
    const char *scr = strstr(path, "screens\\");
    if (!strstr(path, "levels\\")) return;
    if (!scr) { s_in_menu = 0; return; }
    scr += 8;
    if (!strncmp(scr, "ingame", 6) || !strncmp(scr, "confirmation", 12) || !strncmp(scr, "textinput", 9) ||
        !strncmp(scr, "reconnectcontroller", 19)) return;
    s_in_menu = 1;
}

void fn_80019700(PPCContext *c) {
    char path[260];
    read_guest_str(c->r[4], path, sizeof path);
    track_menu(path);
    // Intro skip (TUROK_SKIP_INTRO=<screen>): the boot sequence (logos, legal, intro movie)
    // is replaced by a later front-end screen.
    char skip_buf[64];
    if (strstr(path, "bootupsequence") &&
        GetEnvironmentVariableA("TUROK_SKIP_INTRO", skip_buf, sizeof skip_buf) && skip_buf[0] && skip_buf[0] != '0') {
        char repl[200];
        int n = snprintf(repl, sizeof repl, "data\\levels\\u_jcowlishaw\\screens\\%s\\%s.atr", skip_buf, skip_buf);
        for (int i = 0; i <= n; i++) wr8(LEVEL_PATH_ADDR + 0x100 + i, (uint8_t)repl[i]);
        c->r[4] = LEVEL_PATH_ADDR + 0x100;
        logf_("[level] load %s -> %s (intro skip)", path, repl);
        core_speed_boost(4.f);
        orig_80019700(c);
        return;
    }
    if (GetEnvironmentVariableA("TUROK_SKIP_INTRO", skip_buf, sizeof skip_buf) && skip_buf[0] && skip_buf[0] != '0') {
        if (strstr(path, "firstinteractivescreen")) g_auto_start_at = host_time_ticks() + TB_FREQ;  // after 1 s
        if (strstr(path, "mainmenu")) core_speed_boost(1.f);
    }
    // Win32 lookup, not getenv: the host's --level sets it after the CRT copied the environment.
    char want_buf[260];
    DWORD wl = GetEnvironmentVariableA("TUROK_LEVEL", want_buf, sizeof want_buf);
    const char *want = wl && wl < sizeof want_buf ? want_buf : NULL;
    // Front-end screens live under data\levels\u_jcowlishaw; the first other level is gameplay.
    if (want && *want && !s_redirected && strstr(path, "levels\\") && !strstr(path, "u_jcowlishaw")) {
        // Same form the game uses: data\levels\...\x.atr, lower case, no "$/" prefix.
        char repl[260];
        int n = snprintf(repl, sizeof repl, "%s", want[0] == '$' ? want + 2 : want);
        for (int i = 0; i < n; i++) repl[i] = repl[i] == '/' ? '\\' : (char)tolower((unsigned char)repl[i]);
        for (int i = 0; i <= n; i++) wr8(LEVEL_PATH_ADDR + i, (uint8_t)repl[i]);
        c->r[4] = LEVEL_PATH_ADDR;
        s_redirected = 1;
        logf_("[level] load %s -> %s", path, repl);
    } else {
        logf_("[level] load %s", path);
    }
    orig_80019700(c);
}
