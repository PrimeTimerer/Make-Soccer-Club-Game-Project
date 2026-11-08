/*
 * ps3recomp game project -- game-specific hooks and overrides.
 *
 * main.cpp is the runner and is game-agnostic. This file is where a port puts
 * everything that is not: the PRX loading hook it must answer, overrides for
 * firmware functions the HLE library gets wrong for this title, and patches
 * over recompiled code.
 *
 * It starts out as the empty version of all of that, which is what a game with
 * no lifted PRX and no overrides needs.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "ps3recomp/runtime/prx/prx_loader.h"
#include "ps3recomp/runtime/ppu/ppu_context.h"

#include <atomic>
#include <chrono>
#include <thread>

/* ---------------------------------------------------------------------------
 * 1.03 patch boot (see PORT_1.03_RUNTIME.md).
 *
 * The 1.03 EBOOT has to see the disc game plus its HDD update, the way real
 * hardware and RPCS3 (Category GD) present it. ps3recomp's cellGame only knows
 * plain disc/HDD boots and has no cellGamePatchCheck, so these context
 * handlers wrap it. Behaviour is taken from RPCS3
 * rpcs3/Emu/Cell/Modules/cellGame.cpp (cellGameBootCheck "GD" branch,
 * cellGamePatchCheck, cellGameContentPermit's perm.dir):
 *   BootCheck      -> type DISC, attributes PATCH,
 *                     size {hddFree 40 GB - 256 KB, sizeKB NOTCALC, sysSizeKB 4}
 *   ContentPermit after BootCheck  -> /dev_bdvd/PS3_GAME (perm.dir empty)
 *   PatchCheck     -> CELL_OK, size {.., NOTCALC, 0}, perm.dir = title id
 *   ContentPermit after PatchCheck -> /dev_hdd0/game/BLJM61064[/USRDIR]
 * The RPCS3 1.03 run (11 min) called exactly BootCheck, ContentPermit,
 * PatchCheck, ContentPermit at 0:09.5, then DataCheck "/", "BLJM61064" and
 * "NPJB00414".
 * -----------------------------------------------------------------------*/

static const char kSakaTitleId[] = "BLJM61064";
static const uint32_t kNidBootCheck     = 0xF52639EAu;
static const uint32_t kNidPatchCheck    = 0xCE4374F6u;
static const uint32_t kNidContentPermit = 0x70ACEC67u;
static const uint32_t kNidDataCheck     = 0xDB9819F3u;

extern "C" uint8_t* vm_base;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
/* libs/system/cellGame.c -- guest addresses travel as pointer-typed arguments. */
extern "C" void cellGame_init_from_paramsfo(const char* sfo_path);
extern "C" s32  cellGameBootCheck(void* type, void* attributes, void* size, void* dirName);
extern "C" s32  cellGameContentPermit(void* contentInfoPath, void* usrdirPath);
extern "C" s32  cellGameDataCheck(u32 type, const void* dirName, void* size);

/* Set by PatchCheck, consumed by the next ContentPermit (RPCS3's perm.dir). */
static std::atomic<bool> s_patch_permit_pending{false};

static void guest_be32(uint32_t ea, uint32_t v)
{
    if (!ea) return;
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void guest_content_size(uint32_t ea, uint32_t sys_size_kb)
{
    if (!ea) return;                              /* CellGameContentSize: 3 x be s32 */
    guest_be32(ea + 0, 40u * 1024u * 1024u - 256u);   /* hddFreeSizeKB */
    guest_be32(ea + 4, 0xFFFFFFFFu);                  /* sizeKB = CELL_GAME_SIZEKB_NOTCALC */
    guest_be32(ea + 8, sys_size_kb);                  /* sysSizeKB */
}

static void guest_str(uint32_t ea, const char* s, size_t cap)
{
    if (!ea || !cap) return;
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(vm_base + ea, s, n);
    vm_base[ea + n] = 0;
}

static void saka_cellGameBootCheck(ppu_context* ctx)
{
    const uint32_t type = (uint32_t)ctx->gpr[3], attr = (uint32_t)ctx->gpr[4];
    const uint32_t size = (uint32_t)ctx->gpr[5], dir  = (uint32_t)ctx->gpr[6];
    /* Run ps3recomp's own BootCheck first: it arms its disc check state, so the
     * ContentPermit that follows still answers /dev_bdvd/PS3_GAME. */
    s32 rc = cellGameBootCheck((void*)(uintptr_t)type, (void*)(uintptr_t)attr,
                               (void*)(uintptr_t)size, (void*)(uintptr_t)dir);
    if (rc == 0) {
        guest_be32(type, 1);        /* CELL_GAME_GAMETYPE_DISC */
        guest_be32(attr, 0x1);      /* CELL_GAME_ATTRIBUTE_PATCH */
        guest_content_size(size, 4);
        s_patch_permit_pending = false;
        printf("[saka] cellGameBootCheck -> DISC + PATCH (1.03 patch boot)\n");
    }
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

static void saka_cellGamePatchCheck(ppu_context* ctx)
{
    guest_content_size((uint32_t)ctx->gpr[3], 0);
    s_patch_permit_pending = true;
    printf("[saka] cellGamePatchCheck -> CELL_OK (/dev_hdd0/game/%s)\n", kSakaTitleId);
    ctx->gpr[3] = 0;   /* CELL_OK */
}

static void saka_cellGameContentPermit(ppu_context* ctx)
{
    const uint32_t cip = (uint32_t)ctx->gpr[3], usr = (uint32_t)ctx->gpr[4];
    /* Always run ps3recomp's permit so its check state is closed as usual. */
    s32 rc = cellGameContentPermit((void*)(uintptr_t)cip, (void*)(uintptr_t)usr);
    if (rc == 0 && s_patch_permit_pending.exchange(false) && cip && usr) {
        char a[128], b[128];   /* CELL_GAME_PATH_MAX */
        snprintf(a, sizeof a, "/dev_hdd0/game/%s", kSakaTitleId);
        snprintf(b, sizeof b, "/dev_hdd0/game/%s/USRDIR", kSakaTitleId);
        guest_str(cip, a, sizeof a);
        guest_str(usr, b, sizeof b);
        printf("[saka] cellGameContentPermit -> %s (patch)\n", a);
    }
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

static void saka_cellGameDataCheck(ppu_context* ctx)
{
    s_patch_permit_pending = false;   /* a new check owns the next permit */
    s32 rc = cellGameDataCheck((u32)ctx->gpr[3], (const void*)(uintptr_t)(uint32_t)ctx->gpr[4],
                               (void*)(uintptr_t)(uint32_t)ctx->gpr[5]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

static void saka_register_patch_boot(void)
{
    /* Title id, title and version come from the update's PARAM.SFO, as RPCS3
     * loads Emu.GetDir()+PARAM.SFO for a GD boot (CATEGORY GD keeps ps3recomp's
     * DISC game type). */
    char sfo[1024];
    const char* hdd0 = getenv("PS3_HDD0_ROOT");
    snprintf(sfo, sizeof sfo, "%s/game/%s/PARAM.SFO",
             (hdd0 && hdd0[0]) ? hdd0 : "game/dev_hdd0", kSakaTitleId);
    cellGame_init_from_paramsfo(sfo);

    ps3_hle_register_ctx(kNidBootCheck,     "cellGameBootCheck",     saka_cellGameBootCheck);
    ps3_hle_register_ctx(kNidPatchCheck,    "cellGamePatchCheck",    saka_cellGamePatchCheck);
    ps3_hle_register_ctx(kNidContentPermit, "cellGameContentPermit", saka_cellGameContentPermit);
    ps3_hle_register_ctx(kNidDataCheck,     "cellGameDataCheck",     saka_cellGameDataCheck);
}

/* ---------------------------------------------------------------------------
 * PRX loading
 *
 * main.cpp calls this once, after the lifted function table is registered and
 * vm_base is live, before the game runs. A title that lifts a real system PRX
 * -- libsre, which is cellSpurs and cellSync -- loads its image into guest RAM
 * here and registers its exports. Most ports never need to: the HLE library
 * answers those modules instead.
 * -----------------------------------------------------------------------*/

/* Sakatsuku runs libsre (cellSpurs/cellSync) and libspurs_jq as LLE, as RPCS3 does
 * for this title: the HLE library lacks the SPURS v2 APIs the game uses
 * (InitializeWithAttribute2, CreateTaskset2, CreateTask2WithBinInfo, JobQueue Port2).
 * The images come from the owner's own firmware (FW 4.93), relocated and lifted
 * offline -- see NOTES.md. Tables are generated into prx_src/<module>/. */
extern "C" const prx_func_entry libsre_function_table[];
extern "C" const uint64_t        libsre_function_table_count;
extern "C" const prx_export      libsre_exports[];
extern "C" const uint32_t        libsre_export_count;
extern "C" const prx_func_entry libspurs_jq_function_table[];
extern "C" const uint64_t        libspurs_jq_function_table_count;
extern "C" const prx_export      libspurs_jq_exports[];
extern "C" const uint32_t        libspurs_jq_export_count;

/* runtime/ppu/ppu_loader.cpp: guest code address -> lifted host function. */
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(void*));

static void prx_register(uint32_t guest_addr, void (*host)(void*))
{
    ppu_register_function(guest_addr, host);
}

static void load_prx(const char* dir, const char* name, uint32_t base,
                     const prx_func_entry* funcs, uint64_t nfuncs,
                     const prx_export* exports, uint32_t nexports)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s_image.bin", dir, name);
    uint32_t size = 0;
    uint8_t* img = prx_image_load_file(path, &size);
    if (!img) {
        fprintf(stderr, "[prx] %s: image not found at %s -- its imports stay on HLE\n", name, path);
        return;
    }
    prx_module m;
    memset(&m, 0, sizeof(m));
    m.name         = name;
    m.base         = base;      /* must match the lift_prx.py / ppu_lifter --base */
    m.image        = img;
    m.image_size   = size;
    m.funcs        = funcs;
    m.func_count   = nfuncs;
    m.exports      = exports;
    m.export_count = nexports;
    prx_load_result r = prx_load_module(&m, prx_register);
    fprintf(stderr, "[prx] %s: ok=%d base=0x%08X size=0x%X funcs=%u exports=%u\n",
            name, r.ok, base, size, r.funcs_registered, nexports);
    free(img);   /* the loader copied the bytes into guest RAM */
}

/* Diagnostic (SAKA_COUNT_CALLS=1): count dispatched calls of hot guest
 * functions found by PS3_SAMPLE_MAIN and print the rate every 2 s. Only calls
 * that go through the dispatch table (indirect calls) are counted. */
void func_0062C720(ppu_context* ctx);   /* lifted; C++ linkage in ppu_recomp.h */
void func_006D4410(ppu_context* ctx);
static std::atomic<unsigned long long> s_cnt_62C720{0}, s_cnt_6D4410{0};
static void cnt_62C720(ppu_context* c) { s_cnt_62C720.fetch_add(1, std::memory_order_relaxed); func_0062C720(c); }
static void cnt_6D4410(ppu_context* c) { s_cnt_6D4410.fetch_add(1, std::memory_order_relaxed); func_006D4410(c); }
static void saka_install_call_counters(void)
{
    const char* e = getenv("SAKA_COUNT_CALLS");
    if (!(e && *e == '1')) return;
    ppu_register_function(0x0062C720u, (void (*)(void*))cnt_62C720);
    ppu_register_function(0x006D4410u, (void (*)(void*))cnt_6D4410);
    std::thread([] {
        for (;;) {
            unsigned long long a0 = s_cnt_62C720, b0 = s_cnt_6D4410;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            fprintf(stderr, "[saka-count] per second: func_0062C720 %llu, func_006D4410 %llu\n",
                    (s_cnt_62C720 - a0) / 2, (s_cnt_6D4410 - b0) / 2);
        }
    }).detach();
}

extern "C" void ps3_load_prx_modules(void)
{
    saka_register_patch_boot();   /* before ppu_hle_init: ctx handlers win over the table */
    saka_install_call_counters(); /* diagnostic, SAKA_COUNT_CALLS=1 */

    const char* dir = getenv("SAKA_PRX_DIR");
    if (!dir || !dir[0]) dir = "game/firmware";
    load_prx(dir, "libsre", 0x30000000u, libsre_function_table, libsre_function_table_count,
             libsre_exports, libsre_export_count);
    load_prx(dir, "libspurs_jq", 0x30100000u, libspurs_jq_function_table,
             libspurs_jq_function_table_count, libspurs_jq_exports, libspurs_jq_export_count);
}

/* ---------------------------------------------------------------------------
 * Example: Override a specific NID
 *
 * The module system resolves function imports by NID.  To replace a default
 * HLE stub with custom logic, register your function at init time.
 * -----------------------------------------------------------------------*/

/*
 * Example: override cellGcmSetFlipMode (NID 0xA53D12AE) to force vsync.
 *
 *   static void my_cellGcmSetFlipMode(u32 mode)
 *   {
 *       printf("[hook] cellGcmSetFlipMode: forcing VSYNC (was %u)\n", mode);
 *       // Force vsync regardless of what the game requested
 *       cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
 *   }
 *
 * Then in your init code:
 *
 *   ps3::modules::override_nid(0xA53D12AE,
 *       reinterpret_cast<void*>(my_cellGcmSetFlipMode));
 */

/* ---------------------------------------------------------------------------
 * Example: Hook into module loading
 *
 * You can intercept cellSysmoduleLoadModule to perform custom actions
 * when a particular module is loaded.
 * -----------------------------------------------------------------------*/

/*
 *   static s32 my_loadmodule_hook(u16 id)
 *   {
 *       printf("[hook] Module 0x%04X loaded\n", id);
 *
 *       if (id == CELL_SYSMODULE_AUDIO) {
 *           // Perform custom audio backend initialization
 *           init_my_audio_backend();
 *       }
 *
 *       // Call the original implementation
 *       return cellSysmoduleLoadModule(id);
 *   }
 */

/* ---------------------------------------------------------------------------
 * Game-specific patches
 *
 * Sometimes you need to patch recompiled code at specific addresses.
 * Use the patch system to modify behavior without editing generated code.
 * -----------------------------------------------------------------------*/

/*
 *   // Skip an integrity check at guest address 0x00812FA0
 *   ps3::patches::nop_range(0x00812FA0, 0x00812FB0);
 *
 *   // Replace a function call with a direct return
 *   ps3::patches::force_return(0x009A1000, 0);  // return 0
 */
