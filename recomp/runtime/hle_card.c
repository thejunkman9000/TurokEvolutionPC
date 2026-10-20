// CARD: a memory card in slot A, backed by GCI files on the host.
//
// The real CARD library talks to the card over EXI and unlocks it with a DSP
// microcode, so it is replaced at its public entry points instead. Saves are
// stored in Dolphin's GCI-folder format (64-byte CARDDir header + data), so
// they can be exchanged with Dolphin:  saves/GTKE51/Card A/<maker>-<game>-<name>.gci
// Slot B reports no card.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include "ppc.h"
#include "host.h"
#include "hw.h"
#include "../generated/funcs.h"
#include "../generated/symbols.h"

#define CARD_RESULT_READY 0
#define CARD_RESULT_NOCARD -3
#define CARD_RESULT_NOFILE -4
#define CARD_RESULT_EXIST -7
#define CARD_RESULT_INSSPACE -9
#define CARD_RESULT_LIMIT -11
#define CARD_RESULT_NAMETOOLONG -12

#define SECTOR 8192u
#define USER_BLOCKS 59u   // "Memory Card 59"
#define MAX_FILES 127
#define MEM_MBITS 4

typedef struct {
    uint8_t dir[64];      // CARDDir, big-endian, exactly as on the card / in a .gci
    uint8_t *data;        // blocks * SECTOR bytes
    int used;
} CardFile;

static CardFile s_files[MAX_FILES];
static int s_loaded;
static int32_t s_result[2];
static char s_dir_path[MAX_PATH];

// ---- CARDDir field access ----------------------------------------------------------
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
#define DIR_BANNERFMT 7
#define DIR_NAME 8
#define DIR_TIME 0x28
#define DIR_ICONADDR 0x2C
#define DIR_ICONFMT 0x30
#define DIR_ICONSPEED 0x32
#define DIR_PERM 0x34
#define DIR_LENGTH 0x38
#define DIR_COMMENT 0x3C
static uint16_t file_blocks(const CardFile *f) { return be16(f->dir + DIR_LENGTH); }

static uint32_t card_time(void) { return (uint32_t)(time(NULL) - 946684800); }  // seconds since 2000

// ---- host storage ------------------------------------------------------------------------
static void file_path(const CardFile *f, char *out, size_t n) {
    char name[33] = {0};
    memcpy(name, f->dir + DIR_NAME, 32);
    for (char *p = name; *p; p++) if (strchr("\\/:*?\"<>|", *p)) *p = '_';
    snprintf(out, n, "%s\\%.2s-%.4s-%s.gci", s_dir_path, (const char *)f->dir + 4, (const char *)f->dir, name);
}

// Before a save is overwritten, keep the previous version in Backups\ (once per save
// session: only if the file wasn't written in the last minute); the newest 20 are kept.
static void backup_before_write(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    uint64_t t_now = (uint64_t)now.dwHighDateTime << 32 | now.dwLowDateTime;
    uint64_t t_file = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
    if (t_now - t_file < 60ull * 10000000ull) return;  // same save session
    char dir[MAX_PATH], base[MAX_PATH], dst[MAX_PATH];
    snprintf(dir, sizeof dir, "%s\\Backups", s_dir_path);
    CreateDirectoryA(dir, NULL);
    const char *name = strrchr(path, '\\');
    snprintf(base, sizeof base, "%s", name ? name + 1 : path);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(dst, sizeof dst, "%s\\%s %04d-%02d-%02d %02d%02d%02d.gci", dir, base, st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    if (CopyFileA(path, dst, TRUE)) logf_("[card] backed up %s", dst);
    // Keep the newest 20 backups of this save (names sort by date).
    static char names[64][MAX_PATH];
    char pattern[MAX_PATH];
    int n = 0;
    snprintf(pattern, sizeof pattern, "%s\\%s *.gci", dir, base);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { if (n < 64) snprintf(names[n++], MAX_PATH, "%s", fd.cFileName); } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, (size_t)n, MAX_PATH, (int (*)(const void *, const void *))strcmp);
    for (int i = 0; i + 20 < n; i++) {
        char old[MAX_PATH];
        snprintf(old, sizeof old, "%s\\%s", dir, names[i]);
        DeleteFileA(old);
    }
}

static void save_file(const CardFile *f) {
    char path[MAX_PATH];
    file_path(f, path, sizeof path);
    backup_before_write(path);
    FILE *fp = fopen(path, "wb");
    if (!fp) { logf_("[card] cannot write %s", path); return; }
    fwrite(f->dir, 1, 64, fp);
    fwrite(f->data, 1, (size_t)file_blocks(f) * SECTOR, fp);
    fclose(fp);
}

static void load_all(void) {
    if (s_loaded) return;
    s_loaded = 1;
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *slash = strrchr(exe, '\\'); if (slash) *slash = 0;
    snprintf(s_dir_path, sizeof s_dir_path, "%s\\saves", exe);
    // TUROK_SAVE_DIR: use another folder (automated test runs must never touch real saves).
    char alt[MAX_PATH];
    DWORD an = GetEnvironmentVariableA("TUROK_SAVE_DIR", alt, sizeof alt);
    if (an && an < sizeof alt) snprintf(s_dir_path, sizeof s_dir_path, "%s", alt);
    CreateDirectoryA(s_dir_path, NULL);
    strcat(s_dir_path, "\\GTKE51"); CreateDirectoryA(s_dir_path, NULL);
    strcat(s_dir_path, "\\Card A"); CreateDirectoryA(s_dir_path, NULL);

    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*.gci", s_dir_path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%s", s_dir_path, fd.cFileName);
        FILE *fp = fopen(path, "rb");
        if (!fp) continue;
        CardFile f = {0};
        if (fread(f.dir, 1, 64, fp) == 64) {
            size_t bytes = (size_t)file_blocks(&f) * SECTOR;
            f.data = calloc(1, bytes ? bytes : 1);
            if (fread(f.data, 1, bytes, fp) == bytes) {
                for (int i = 0; i < MAX_FILES; i++)
                    if (!s_files[i].used) { f.used = 1; s_files[i] = f; break; }
                logf_("[card] loaded %s (%u blocks)", fd.cFileName, file_blocks(&f));
            } else free(f.data);
        }
        fclose(fp);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static uint32_t used_blocks(void) {
    uint32_t n = 0;
    for (int i = 0; i < MAX_FILES; i++) if (s_files[i].used) n += file_blocks(&s_files[i]);
    return n;
}

static int find_file(uint32_t name_addr) {
    char name[33] = {0};
    for (int i = 0; i < 32; i++) { name[i] = (char)rd8(name_addr + i); if (!name[i]) break; }
    for (int i = 0; i < MAX_FILES; i++) {
        if (!s_files[i].used) continue;
        if (memcmp(s_files[i].dir, g_ram, 4) != 0) continue;  // gameName must match disc ID
        if (strncmp((const char *)s_files[i].dir + DIR_NAME, name, 32) == 0) return i;
    }
    return -1;
}

// ---- helpers -------------------------------------------------------------------------------------
static int32_t finish(PPCContext *c, int chan, int32_t result) {
    if (chan >= 0 && chan < 2) s_result[chan] = result;
    c->r[3] = (uint32_t)result;
    return result;
}

static void async_done(int chan, int32_t result, uint32_t callback) {
    if (callback) irq_queue_callback(callback, (uint32_t)chan, (uint32_t)result);
}

static void fill_fileinfo(uint32_t fi, int chan, int fileNo) {
    wr32(fi + 0x00, (uint32_t)chan);
    wr32(fi + 0x04, (uint32_t)fileNo);
    wr32(fi + 0x08, 0);
    wr32(fi + 0x0C, file_blocks(&s_files[fileNo]) * SECTOR);
    wr16(fi + 0x10, 0);
}

// ---- API ---------------------------------------------------------------------------------------
void SYM_CARDInit(PPCContext *c) { (void)c; load_all(); }

void fn_80122F24(PPCContext *c) {  // CARDGetResultCode(chan)
    int chan = (int)c->r[3];
    c->r[3] = (uint32_t)(chan == 0 ? s_result[0] : CARD_RESULT_NOCARD);
}

void SYM_CARDProbeEx(PPCContext *c) {  // (chan, s32 *memSize, s32 *sectorSize)
    int chan = (int)c->r[3];
    if (chan != 0) { finish(c, chan, CARD_RESULT_NOCARD); return; }
    load_all();
    if (c->r[4]) wr32(c->r[4], MEM_MBITS);
    if (c->r[5]) wr32(c->r[5], SECTOR);
    finish(c, chan, CARD_RESULT_READY);
}

void SYM_CARDMountAsync(PPCContext *c) {  // (chan, workArea, detachCb, attachCb)
    int chan = (int)c->r[3];
    uint32_t attach = c->r[6];
    int32_t r = chan == 0 ? CARD_RESULT_READY : CARD_RESULT_NOCARD;
    if (chan == 0) load_all();
    finish(c, chan, r);
    async_done(chan, r, attach);
}

void SYM_CARDUnmount(PPCContext *c) { finish(c, (int)c->r[3], CARD_RESULT_READY); }

void fn_80124708(PPCContext *c) {  // CARDCheckAsync(chan, callback)
    int chan = (int)c->r[3];
    int32_t r = chan == 0 ? CARD_RESULT_READY : CARD_RESULT_NOCARD;
    finish(c, chan, r);
    async_done(chan, r, c->r[4]);
}

void fn_80125914(PPCContext *c) {  // CARDFormat(chan): delete this game's files
    int chan = (int)c->r[3];
    if (chan != 0) { finish(c, chan, CARD_RESULT_NOCARD); return; }
    for (int i = 0; i < MAX_FILES; i++) {
        if (!s_files[i].used) continue;
        char path[MAX_PATH]; file_path(&s_files[i], path, sizeof path);
        DeleteFileA(path);
        free(s_files[i].data);
        s_files[i].used = 0;
    }
    finish(c, chan, CARD_RESULT_READY);
}

void SYM_CARDFreeBlocks(PPCContext *c) {  // (chan, s32 *bytesNotUsed, s32 *filesNotUsed)
    int chan = (int)c->r[3];
    if (chan != 0) { finish(c, chan, CARD_RESULT_NOCARD); return; }
    int files = 0;
    for (int i = 0; i < MAX_FILES; i++) files += s_files[i].used;
    if (c->r[4]) wr32(c->r[4], (USER_BLOCKS - used_blocks()) * SECTOR);
    if (c->r[5]) wr32(c->r[5], (uint32_t)(MAX_FILES - files));
    finish(c, chan, CARD_RESULT_READY);
}

void SYM_CARDOpen(PPCContext *c) {  // (chan, fileName, CARDFileInfo *)
    int chan = (int)c->r[3];
    if (chan != 0) { finish(c, chan, CARD_RESULT_NOCARD); return; }
    int i = find_file(c->r[4]);
    if (i < 0) { finish(c, chan, CARD_RESULT_NOFILE); return; }
    fill_fileinfo(c->r[5], chan, i);
    finish(c, chan, CARD_RESULT_READY);
}

void SYM_CARDClose(PPCContext *c) {  // (CARDFileInfo *)
    wr32(c->r[3], 0xFFFFFFFFu);
    c->r[3] = CARD_RESULT_READY;
}

void SYM_CARDCreate(PPCContext *c) {  // (chan, fileName, size, CARDFileInfo *)
    int chan = (int)c->r[3];
    uint32_t name = c->r[4], size = c->r[5], fi = c->r[6];
    if (chan != 0) { finish(c, chan, CARD_RESULT_NOCARD); return; }
    char nm[33] = {0};
    int len = 0;
    while (len < 33 && (nm[len] = (char)rd8(name + len)) != 0) len++;
    if (len > 32) { finish(c, chan, CARD_RESULT_NAMETOOLONG); return; }
    if (find_file(name) >= 0) { finish(c, chan, CARD_RESULT_EXIST); return; }
    uint32_t blocks = (size + SECTOR - 1) / SECTOR;
    if (used_blocks() + blocks > USER_BLOCKS) { finish(c, chan, CARD_RESULT_INSSPACE); return; }
    int slot = -1;
    for (int i = 0; i < MAX_FILES; i++) if (!s_files[i].used) { slot = i; break; }
    if (slot < 0) { finish(c, chan, CARD_RESULT_LIMIT); return; }
    CardFile *f = &s_files[slot];
    memset(f->dir, 0xFF, 64);
    memcpy(f->dir, g_ram, 4);          // gameName from disc ID
    memcpy(f->dir + 4, g_ram + 4, 2);  // company
    f->dir[DIR_BANNERFMT] = 0;
    memset(f->dir + DIR_NAME, 0, 32);
    memcpy(f->dir + DIR_NAME, nm, (size_t)len);
    put32(f->dir + DIR_TIME, card_time());
    put32(f->dir + DIR_ICONADDR, 0xFFFFFFFFu);
    put16(f->dir + DIR_ICONFMT, 0);
    put16(f->dir + DIR_ICONSPEED, 0);
    f->dir[DIR_PERM] = 0x04;           // public
    f->dir[DIR_PERM + 1] = 0;          // copy times
    put16(f->dir + 0x36, 0);           // start block (not meaningful in a .gci)
    put16(f->dir + DIR_LENGTH, (uint16_t)blocks);
    put32(f->dir + DIR_COMMENT, 0xFFFFFFFFu);
    f->data = calloc(blocks, SECTOR);
    f->used = 1;
    save_file(f);
    if (fi) fill_fileinfo(fi, chan, slot);
    logf_("[card] created '%s' (%u blocks)", nm, blocks);
    finish(c, chan, CARD_RESULT_READY);
}

static int file_from_info(uint32_t fi) {
    int no = (int)rd32(fi + 4);
    return (no >= 0 && no < MAX_FILES && s_files[no].used) ? no : -1;
}

void SYM_CARDReadAsync(PPCContext *c) {  // (CARDFileInfo *, addr, length, offset, cb)
    uint32_t fi = c->r[3], dst = c->r[4], len = c->r[5], off = c->r[6], cb = c->r[7];
    int no = file_from_info(fi);
    if (no < 0) { finish(c, 0, CARD_RESULT_NOFILE); async_done(0, CARD_RESULT_NOFILE, cb); return; }
    CardFile *f = &s_files[no];
    uint32_t size = file_blocks(f) * SECTOR;
    if (off > size) off = size;
    if (len > size - off) len = size - off;
    memcpy(gptr(dst, len, 1), f->data + off, len);
    finish(c, 0, CARD_RESULT_READY);
    async_done(0, CARD_RESULT_READY, cb);
}

void SYM_CARDWriteAsync(PPCContext *c) {  // (CARDFileInfo *, addr, length, offset, cb)
    uint32_t fi = c->r[3], src = c->r[4], len = c->r[5], off = c->r[6], cb = c->r[7];
    int no = file_from_info(fi);
    if (no < 0) { finish(c, 0, CARD_RESULT_NOFILE); async_done(0, CARD_RESULT_NOFILE, cb); return; }
    CardFile *f = &s_files[no];
    uint32_t size = file_blocks(f) * SECTOR;
    if (off > size) off = size;
    if (len > size - off) len = size - off;
    memcpy(f->data + off, gptr(src, len, 0), len);
    put32(f->dir + DIR_TIME, card_time());
    save_file(f);
    finish(c, 0, CARD_RESULT_READY);
    async_done(0, CARD_RESULT_READY, cb);
}

void fn_80126950(PPCContext *c) {  // CARDDeleteAsync(chan, fileName, cb)
    int chan = (int)c->r[3];
    uint32_t cb = c->r[5];
    int i = chan == 0 ? find_file(c->r[4]) : -1;
    int32_t r = chan != 0 ? CARD_RESULT_NOCARD : i < 0 ? CARD_RESULT_NOFILE : CARD_RESULT_READY;
    if (i >= 0) {
        char path[MAX_PATH]; file_path(&s_files[i], path, sizeof path);
        DeleteFileA(path);
        free(s_files[i].data);
        s_files[i].used = 0;
    }
    finish(c, chan, r);
    async_done(chan, r, cb);
}

// CARDStat (0x6C): name 0x00, length 0x20, time 0x24, gameName 0x28, company 0x2C,
// bannerFormat 0x2E, iconAddr 0x30, iconFormat 0x34, iconSpeed 0x36, commentAddr 0x38,
// offsetBanner 0x3C, offsetBannerTlut 0x40, offsetIcon[8] 0x44, offsetIconTlut 0x64, offsetData 0x68
static void update_icon_offsets(const CardFile *f, uint32_t st) {
    uint32_t offset = be32(f->dir + DIR_ICONADDR);
    uint8_t banner = f->dir[DIR_BANNERFMT];
    uint16_t iconfmt = be16(f->dir + DIR_ICONFMT);
    if (offset == 0xFFFFFFFFu) { banner = 0; iconfmt = 0; offset = 0; wr8(st + 0x2E, 0); wr16(st + 0x34, 0); wr16(st + 0x36, 0); }
    switch (banner & 3) {
    case 1: wr32(st + 0x3C, offset); offset += 96 * 32; wr32(st + 0x40, offset); offset += 512; break;
    case 2: wr32(st + 0x3C, offset); offset += 2 * 96 * 32; wr32(st + 0x40, 0xFFFFFFFFu); break;
    default: wr32(st + 0x3C, 0xFFFFFFFFu); wr32(st + 0x40, 0xFFFFFFFFu); break;
    }
    int tlut = 0;
    for (int i = 0; i < 8; i++) {
        switch ((iconfmt >> (2 * i)) & 3) {
        case 1: wr32(st + 0x44 + 4 * i, offset); offset += 32 * 32; tlut = 1; break;
        case 2: wr32(st + 0x44 + 4 * i, offset); offset += 2 * 32 * 32; break;
        default: wr32(st + 0x44 + 4 * i, 0xFFFFFFFFu); break;
        }
    }
    if (tlut) { wr32(st + 0x64, offset); offset += 512; } else wr32(st + 0x64, 0xFFFFFFFFu);
    wr32(st + 0x68, offset);
}

void SYM_CARDGetStatus(PPCContext *c) {  // (chan, fileNo, CARDStat *)
    int chan = (int)c->r[3], no = (int)c->r[4];
    uint32_t st = c->r[5];
    if (chan != 0) { finish(c, chan, CARD_RESULT_NOCARD); return; }
    if (no < 0 || no >= MAX_FILES || !s_files[no].used) { finish(c, chan, CARD_RESULT_NOFILE); return; }
    const CardFile *f = &s_files[no];
    for (int i = 0; i < 32; i++) wr8(st + i, f->dir[DIR_NAME + i]);
    wr32(st + 0x20, file_blocks(f) * SECTOR);
    wr32(st + 0x24, be32(f->dir + DIR_TIME));
    for (int i = 0; i < 4; i++) wr8(st + 0x28 + i, f->dir[i]);
    wr8(st + 0x2C, f->dir[4]); wr8(st + 0x2D, f->dir[5]);
    wr8(st + 0x2E, f->dir[DIR_BANNERFMT]);
    wr32(st + 0x30, be32(f->dir + DIR_ICONADDR));
    wr16(st + 0x34, be16(f->dir + DIR_ICONFMT));
    wr16(st + 0x36, be16(f->dir + DIR_ICONSPEED));
    wr32(st + 0x38, be32(f->dir + DIR_COMMENT));
    update_icon_offsets(f, st);
    finish(c, chan, CARD_RESULT_READY);
}

void SYM_CARDSetStatus(PPCContext *c) {  // (chan, fileNo, CARDStat *)
    int chan = (int)c->r[3], no = (int)c->r[4];
    uint32_t st = c->r[5];
    if (chan != 0) { finish(c, chan, CARD_RESULT_NOCARD); return; }
    if (no < 0 || no >= MAX_FILES || !s_files[no].used) { finish(c, chan, CARD_RESULT_NOFILE); return; }
    CardFile *f = &s_files[no];
    f->dir[DIR_BANNERFMT] = rd8(st + 0x2E);
    put32(f->dir + DIR_ICONADDR, rd32(st + 0x30));
    put16(f->dir + DIR_ICONFMT, rd16(st + 0x34));
    put16(f->dir + DIR_ICONSPEED, rd16(st + 0x36));
    put32(f->dir + DIR_COMMENT, rd32(st + 0x38));
    put32(f->dir + DIR_TIME, card_time());
    update_icon_offsets(f, st);
    save_file(f);
    finish(c, chan, CARD_RESULT_READY);
}
