/* Three reusable EDIT signings. BLJM61064 1.03: see NOTES.md.
 * 7FFD/7FFE belong to imported opponent teams, so keep those IDs untouched.
 * The two additional club players use 7FFB/7FFC and private master records.
 * Native roster/save bytes keep their original layout; extra masters travel
 * with the save in .sakatsuku-port/MAINnn.DAT.edit3, bound to that save's hash.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3recomp/runtime/syscalls/sys_memory.h"

#define EDIT_MASTER 0x00EEAFA8u
#define EDIT_PENDING 0x00F6D56Cu
#define EDIT_ROSTER 0x00EE2954u
#define MASTER_SIZE 0x1A8u
#define RECORD_SIZE 0x218u

static uint8_t s_extra[2][MASTER_SIZE];
static uint32_t s_guest;
static PPU_THREAD_LOCAL uint32_t s_release_id;

static uint32_t rd32(uint32_t ea) {
    const uint8_t* p = vm_base + ea;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void wr32(uint32_t ea, uint32_t n) {
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
}
static void wr16(uint32_t ea, uint32_t n) {
    vm_base[ea] = (uint8_t)(n >> 8); vm_base[ea + 1] = (uint8_t)n;
}
static int extra_index(uint32_t id) { return id == 0x7FFBu ? 0 : id == 0x7FFCu ? 1 : -1; }
int saka_edit_own_id(uint32_t id) { return id == 0x7FFFu || extra_index(id) >= 0; }

static unsigned roster_mask(void) {
    unsigned mask = 0;
    for (unsigned n = 0; n < 23; ++n) {
        uint32_t id = rd32(EDIT_ROSTER + n * RECORD_SIZE);
        if (id == 0x7FFFu) mask |= 1;
        else if (extra_index(id) >= 0) mask |= 2u << extra_index(id);
    }
    return mask;
}

/* Only a currently contracted, not-yet-arrived candidate occupies the one
 * native pending slot. A former player or cancelled negotiation is reusable. */
int saka_edit_limit(void) {
    uint32_t id = rd32(EDIT_PENDING);
    unsigned mask = roster_mask();
    unsigned pending = id == 0x7FFFu ? 1 : extra_index(id) >= 0 ? 2u << extra_index(id) : 0;
    return mask == 7 || (pending && !(mask & pending) && (rd32(EDIT_PENDING + 0xE8) & 0x4000u));
}

static int ensure_guest(ppu_context* ctx) {
    if (s_guest) return 1;
    /* Allocate through LV2, rather than guessing a hole in the title's heap.
     * Borrow and restore one word below the current stack for the syscall. */
    uint32_t scratch = (uint32_t)ctx->gpr[1] - 16;
    uint32_t old = rd32(scratch);
    ppu_context call = *ctx;
    call.gpr[3] = 0x10000; call.gpr[4] = SYS_MEMORY_PAGE_SIZE_64K; call.gpr[5] = scratch;
    int64_t rc = sys_memory_allocate(&call);
    if (rc == 0) s_guest = rd32(scratch);
    wr32(scratch, old);
    if (!s_guest) { fprintf(stderr, "[edit3] cannot allocate master records\n"); return 0; }
    memcpy(vm_base + s_guest, s_extra, sizeof s_extra);
    return 1;
}

uint32_t saka_edit_master(uint32_t id) {
    int i = extra_index(id);
    return id == 0x7FFFu ? EDIT_MASTER : i >= 0 && s_guest ? s_guest + i * MASTER_SIZE : 0;
}

/* Export at a stable, synthetic address in state.bin, not a guest allocation.
 * LiveState's existing range format can carry it without a format change. */
void saka_edit_export(void* out) {
    memcpy(out, s_guest ? vm_base + s_guest : (const uint8_t*)s_extra, sizeof s_extra);
}
int saka_edit_copy_master(uint32_t id, void* out) {
    int i = extra_index(id);
    if (id == 0x7FFFu) memcpy(out, vm_base + EDIT_MASTER, MASTER_SIZE);
    else if (i >= 0) memcpy(out, s_guest ? vm_base + s_guest + i * MASTER_SIZE : s_extra[i], MASTER_SIZE);
    else return 0;
    return 1;
}

/* Entry hook of 004C9EF4; the original handles 7FFF and opponent IDs. */
int saka_edit_master_hook(ppu_context* ctx) {
    uint32_t id = (uint32_t)ctx->gpr[3];
    if (extra_index(id) < 0) return 0;
    if (!ensure_guest(ctx)) return 0;
    ctx->gpr[3] = saka_edit_master(id);
    return 1;
}

/* Replaces 0053440C's single-player guard/copy, then uses the original
 * negotiation routine. Never changes the library entry (PLED.DAT). */
void saka_edit_import(ppu_context* ctx, void (*negotiate)(ppu_context*)) {
    const uint32_t src = (uint32_t)ctx->gpr[3], master = (uint32_t)ctx->gpr[4];
    if (!src || !master || rd32(src) != 0x7FFFu || saka_edit_limit() || !ensure_guest(ctx)) return;
    const unsigned mask = roster_mask();
    const uint32_t id = !(mask & 1) ? 0x7FFFu : !(mask & 2) ? 0x7FFBu : 0x7FFCu;
    const uint32_t dst = saka_edit_master(id), desc = s_guest + 0x800;
    memcpy(vm_base + EDIT_PENDING, vm_base + src, RECORD_SIZE);
    memcpy(vm_base + dst, vm_base + master, MASTER_SIZE);
    wr32(EDIT_PENDING, id);
    wr16(dst, id);
    memset(vm_base + desc, 0, 20);
    wr32(desc, id);
    vm_base[desc + 6] = 0x40;
    vm_base[desc + 7] = vm_base[src + 5];
    wr32(desc + 8, 2);
    ppu_context call = *ctx;
    call.gpr[3] = desc; call.gpr[4] = 0; call.gpr[5] = 0;
    negotiate(&call);
    fprintf(stderr, "[edit3] library copied to ID %04X (roster mask %u); library retained\n", id, mask);
}

/* The old release routine always clears the sole native master. Preserve it
 * while releasing an additional player, and clear only that player's records. */
void saka_edit_release(ppu_context* ctx, void (*release)(ppu_context*)) {
    uint8_t saved[MASTER_SIZE];
    const uint32_t id = (uint32_t)ctx->gpr[3], previous = s_release_id;
    const int extra = extra_index(id);
    if (extra >= 0) memcpy(saved, vm_base + EDIT_MASTER, sizeof saved);
    s_release_id = id;
    release(ctx);
    s_release_id = previous;
    if (extra >= 0) memcpy(vm_base + EDIT_MASTER, saved, sizeof saved);
}

void saka_edit_clear_records(void) {
    for (unsigned table = 0; table < 3; ++table) {
        uint32_t base = 0x00EE27A0u + (0x22A52u + table * 0x42u) * 4;
        for (unsigned n = 0; n < 11; ++n) {
            uint32_t ea = base + n * 0x18, id = rd32(ea + 8);
            if (s_release_id ? id == s_release_id : saka_edit_own_id(id)) memset(vm_base + ea, 0, 0x18);
        }
    }
}

void saka_edit_title(void) {
    memset(s_extra, 0, sizeof s_extra);
    if (s_guest) memset(vm_base + s_guest, 0, sizeof s_extra);
}

static uint64_t hash_bytes(uint64_t h, const uint8_t* p, size_t n) {
    while (n--) { h ^= *p++; h *= UINT64_C(1099511628211); }
    return h;
}
static uint64_t file_hash(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t b[8192]; size_t n; uint64_t h = UINT64_C(14695981039346656037);
    while ((n = fread(b, 1, sizeof b, f)) != 0) h = hash_bytes(h, b, n);
    if (ferror(f)) h = 0;
    fclose(f); return h;
}
static void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (i * 8)); }
static uint64_t get64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = v * 256 + p[i]; return v; }

/* Called after the host has successfully read/written a MAIN file. These
 * small sidecars live in a subfolder, outside the PS3 save-file enumeration. */
void saka_edit_savedata(const char* dir, const char* file, unsigned op, int result) {
    unsigned slot; char tail[16];
    if (result < 0 || !dir || !file || sscanf(file, "MAIN%2u%15s", &slot, tail) != 2 ||
        slot > 99 || strcmp(tail, ".DAT") || (op != 0 && op != 1 && op != 3)) return;
    char native[1200], folder[1200], path[1280], temp[1300];
    snprintf(native, sizeof native, "%s/%s", dir, file);
    snprintf(folder, sizeof folder, "%s/.sakatsuku-port", dir);
    snprintf(path, sizeof path, "%s/%s.edit3", folder, file);
    uint64_t h = file_hash(native);
    uint8_t blob[24 + sizeof s_extra];
    if (op == 0) {
        memset(s_extra, 0, sizeof s_extra);
        FILE* f = fopen(path, "rb");
        if (f) {
            size_t n = fread(blob, 1, sizeof blob, f); int end = fgetc(f); fclose(f);
            if (n == sizeof blob && end == EOF && !memcmp(blob, "SKEDIT3\0", 8) && h &&
                get64(blob + 8) == h && get64(blob + 16) == hash_bytes(UINT64_C(14695981039346656037), blob + 24, sizeof s_extra)) {
                memcpy(s_extra, blob + 24, sizeof s_extra);
                fprintf(stderr, "[edit3] restored extra masters for %s\n", file);
            } else fprintf(stderr, "[edit3] rejected mismatched/corrupt sidecar for %s\n", file);
        }
        if (s_guest) memcpy(vm_base + s_guest, s_extra, sizeof s_extra);
        return;
    }
    if (!h) return;
    if (s_guest) memcpy(s_extra, vm_base + s_guest, sizeof s_extra);
    memcpy(blob, "SKEDIT3\0", 8); put64(blob + 8, h);
    put64(blob + 16, hash_bytes(UINT64_C(14695981039346656037), (const uint8_t*)s_extra, sizeof s_extra));
    memcpy(blob + 24, s_extra, sizeof s_extra);
#ifdef _WIN32
    CreateDirectoryA(folder, NULL);
#endif
    snprintf(temp, sizeof temp, "%s.new", path);
    FILE* f = fopen(temp, "wb");
    if (!f) { fprintf(stderr, "[edit3] cannot write %s\n", temp); return; }
    int ok = fwrite(blob, 1, sizeof blob, f) == sizeof blob;
    if (fclose(f) != 0) ok = 0;
#ifdef _WIN32
    if (ok) ok = MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (ok) ok = rename(temp, path) == 0;
#endif
    fprintf(stderr, "[edit3] %s extra masters for %s\n", ok ? "saved" : "FAILED saving", file);
}

extern void (*g_savedata_host_file_hook)(const char*, const char*, unsigned, int);
void saka_edit_install(void) { g_savedata_host_file_hook = saka_edit_savedata; }
