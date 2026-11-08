/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"
#include "ps3recomp/include/ps3emu/ps3_ftrace.h"   /* PS3_FTRACE spans (Sakatsuku) */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/mman.h>   /* mmap: the guest arena, see alloc_guest_vm */
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>   /* the main thread's run loop, see run_guest */
#include <pthread.h>
#endif

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
void     saka_install_syscall_hooks(void);    /* saka_hooks.c */
void     saka_register_spu_task_regions(void);/* saka_hooks.c: lifted SPURS taskset tasks */
void     saka_register_cri_job_regions(void); /* saka_hooks.c: lifted CRI ADX2 SPURS jobs */
extern void (*g_ppu_fs_open_hook)(const char* guest_path);   /* ppu_fs.cpp: after each cellFsOpen */
void     rsx_live_draw_set_present_nowait(int on);           /* rsx_live_draw.c: no vsync, show <= 60/s */
void     saka_register_l10n(void);            /* saka_l10n.cpp: cellL10n + OSK option NIDs */
unsigned long long ps3_qpc_us(void);          /* runtime: microseconds (SAKA_FLIPLOG) */
int      saka_load_steps_since(long long since); /* saka_hooks.c: pxd loader steps (load turbo) */
int      saka_load_turbo_factor(void);         /* saka_hooks.c: SAKA_LOAD_TURBO, default 8 */
void     rsx_d3d12_set_present_nowait(int on); /* rsx_d3d12_backend.c: no vsync wait */
unsigned rsx_d3d12_last_frame_draws(void);     /* rsx_d3d12_backend.c: draws in the last frame */
void     ppu_report_guest_lrs(void);           /* runtime: every guest thread's lr/r3/sp ([WATCHDOG]) */
extern "C++" void ps3_sampler_start(void);    /* runtime sampling profiler (PS3_SAMPLE); C++ linkage in ppu_loader.cpp */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */
void     ps3_host_quit(const char* why);      /* cellSysutil.c: REQUEST_EXITGAME, then exit 0 */

/* sys_ppu_thread.c: the main thread's stack, as sys_ppu_thread_get_stack_information
 * reports it. Left at 0 it names VM_STACK_BASE, the first created thread's stack. */
extern uint32_t g_ppu_main_stack_addr;
extern uint32_t g_ppu_main_stack_size;

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern "C" { volatile int g_saka_user_speed = 1; extern volatile int g_saka_diag_slowmo; }   /* the player's game speed: saka_overlay.cpp (F7/F8) */
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
unsigned cellGcm_flip_request_count(void);
void     cellGcm_fifo_kick_wait(unsigned ms);  /* wait for a guest FIFO kick, or ms */
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif

/* Live NV4097 -> D3D12 engine (libs/video/rsx_live_draw.c), the path the
 * Killzone 2/3 Windows ports render with. RSX_LIVE_DRAW=1 selects it, wired as
 * the toolkit's boot harness (runtime/ppu/tests/boot_main.cpp) does: the null
 * backend opens the window, the engine binds its swap chain to that HWND and
 * owns presentation, and rsx_d3d12_backend is not initialised at all. Unset,
 * everything below is what it was. */
#if defined(_WIN32)
extern "C" int      rsx_live_draw_enabled(void);
extern "C" int      rsx_live_draw_init(void* hwnd, uint32_t w, uint32_t h,
                                       const uint8_t* (*guest_ptr)(void*, uint32_t, uint32_t, uint32_t),
                                       void* user);
extern "C" void     rsx_live_draw_present(uint32_t buffer_id);
extern "C" uint32_t rsx_live_draw_get_last_draws(void);
extern "C" int      rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" int      rsx_null_backend_pump_messages(void);
extern "C" void*    rsx_null_backend_get_hwnd(void);
extern "C" void     rsx_null_backend_suppress_present(int on);
extern "C" uint32_t cellGcmResolveLocated(int local, uint32_t offset);
extern "C" uint32_t cellGcmResolveIO(uint32_t offset);
extern "C" unsigned cellGcmGetCurrentDisplayBufferId(void);
#endif
static int s_rsx_live = 0;   /* live engine selected AND up */

#if defined(_WIN32)
/* (location, offset) -> host memory for the engine, exactly as boot_main.cpp:
 * location 0 is RSX local VRAM, 1 main memory through the IO table; the whole
 * min_bytes span must be inside the guest space. */
static const uint8_t* rsx_live_guest_ptr(void* user, uint32_t location,
                                         uint32_t offset, uint32_t min_bytes)
{
    (void)user;
    if (!vm_base) return nullptr;
    uint32_t ea;
    if (location == 0) {
        ea = cellGcmResolveLocated(1, offset);
    } else {
        ea = cellGcmResolveIO(offset);
        if (!ea) ea = cellGcmResolveLocated(0, offset);
    }
    if (!ea || ea == 0xFFFFFFFFu || (uint64_t)ea + min_bytes > 0x100000000ull) return nullptr;
    return (const uint8_t*)vm_base + ea;
}
#endif

/* ---------------------------------------------------------------------------
 * Native-fast loading: the pxd loader pump
 * -----------------------------------------------------------------------*/
/* The game's loader (state machine func_002E7AC8, reached only through a
 * pointer: main loop 0x0069065C -> ... -> 0x005D59E4 -> it) takes ONE step per
 * call and is called once a frame. A step hands the async file thread a job
 * that takes about 1 ms, and the main thread then sleeps out the rest of the
 * 33 ms frame, so every file costs a frame or more -- console pacing.
 *
 * Here the call is re-made inside the same frame while it keeps handing out
 * work: after a call that posted a loader step, wait for the file thread to
 * come back to its semaphore (the job is done), restore the call's arguments
 * and call again, up to SAKA_LOADER_PUMP_MS (default 15 ms) per frame -- the
 * time the main thread otherwise spends idle in its vblank wait. The game's
 * frame rate and speed are untouched; files simply arrive many per frame.
 * SAKA_LOADER_PUMP=0 restores one call per frame. */
extern "C" {
long saka_load_steps_total(void);   /* saka_hooks.c */
long saka_load_waits_total(void);   /* saka_hooks.c */
void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));   /* runtime */
}
static int      s_pump_on = 1;
static unsigned s_pump_budget_us = 15000;
static volatile LONG s_pump_extra = 0;   /* extra calls made (diagnostic) */

/* A lifted body that continues in another fragment finishes through this
 * thread-local trampoline (lifted call sites run DRAIN_TRAMPOLINE after every
 * call), so each call is drained before the next decision. */
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
static inline void saka_drain(ppu_context* ctx)
{
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf((void*)ctx); }
}

/* The budget is per guest frame and shared by all loaders, so a frame never
 * spends more than s_pump_budget_us pumping. Only the main thread runs these. */
static unsigned           s_pump_frame = ~0u;
static unsigned long long s_pump_used_us = 0;

static void saka_loader_pump(ppu_context* ctx, void (*fn)(ppu_context*))
{
    uint64_t gpr_in[13];
    memcpy(gpr_in, ctx->gpr, sizeof gpr_in);   /* r0..r12: TOC, sp and the arguments */
    const uint64_t lr_in = ctx->lr;
    long steps = saka_load_steps_total(), waits = saka_load_waits_total();
    fn(ctx);
    saka_drain(ctx);
    if (!s_pump_on) return;
    const unsigned frame = cellGcm_flip_request_count();
    if (frame != s_pump_frame) { s_pump_frame = frame; s_pump_used_us = 0; }
    const unsigned long long t0 = ps3_qpc_us();
    auto spent = [&]() { return s_pump_used_us + (ps3_qpc_us() - t0); };
    static unsigned st_nostep, st_budget, st_calls; static unsigned long long st_t0;
    static unsigned st_state[32];   /* task state ([task+4]) when pumping stopped */
    st_calls++;
    (void)steps; (void)waits;
    /* All three loaders (0x002E0030/6EE8/7AC8): state at [task+4]; state 9 =
     * waiting for an async operation, whose done flag is byte 1 of the word at
     * [task+0x14] (== 1). The game checks it ONCE a frame (the manager
     * 0x005D59E4 retries only states 3/0xD), so every file cost a whole frame
     * although the file thread needs 0.2-4 ms. Wait for the flag here, within
     * the frame's budget, and step again at once -- what the next frame would
     * have done. */
    const uint32_t task = (uint32_t)gpr_in[3];
    for (int n = 0; n < 256; n++) {
        const uint32_t state = vm_read32(task + 4);
        if (state != 9) {                                   /* not waiting on an async op */
            st_nostep++;
            st_state[state < 31 ? state : 31]++;
            break;
        }
        while (((vm_read32(task + 0x14) >> 8) & 0xFFu) != 1u && spent() < s_pump_budget_us)
            SwitchToThread();
        if (((vm_read32(task + 0x14) >> 8) & 0xFFu) != 1u) { st_budget++; break; }
        memcpy(ctx->gpr, gpr_in, sizeof gpr_in);
        ctx->lr = lr_in;
        fn(ctx);
        saka_drain(ctx);
        InterlockedIncrement(&s_pump_extra);
    }
    s_pump_used_us += ps3_qpc_us() - t0;
    /* Every 2 s while loading: why the pump stopped adding steps. */
    const unsigned long long tnow = ps3_qpc_us();
    if (!st_t0) st_t0 = tnow;
    if (tnow - st_t0 >= 2000000ull) {
        static long st_steps0 = 0;
        const long st_steps = saka_load_steps_total();
        if (st_steps != st_steps0) {
            char sh[256]; int k = 0;
            for (int i = 0; i < 32; i++) if (st_state[i]) k += snprintf(sh + k, sizeof sh - k, " s%d=%u", i, st_state[i]);
            fprintf(stderr, "[pump] last 2 s: %ld steps, %u loader calls; pumping stopped: no new step %u (states%s), frame budget %u\n",
                    st_steps - st_steps0, st_calls, st_nostep, k ? sh : " -", st_budget);
        }
        st_steps0 = st_steps;
        memset(st_state, 0, sizeof st_state);
        st_calls = st_nostep = st_budget = 0; st_t0 = tnow;
    }
}

/* SAKA_LOADER_CB_TRACE=1: who runs the loaders' completion callback 0x002E1460
 * (stores the status word, done flag in byte 1, into [task+0x14]) and when. */
static void saka_loader_cb_trace(ppu_context* ctx)
{
    static volatile LONG n = 0;
    const LONG k = InterlockedIncrement(&n);
    static int all = -1; if (all < 0) { const char* e = getenv("SAKA_LOADER_CB_TRACE"); all = e && e[1] == 'a'; }
    if (all || k <= 40 || (k % 200) == 0)
        fprintf(stderr, "[loader-cb] #%ld tid=%u lr=0x%08X task=0x%08X status=0x%08X t=%.3f\n",
                (long)k, (unsigned)ctx->thread_id, (unsigned)ctx->lr, (unsigned)ctx->gpr[5],
                (unsigned)ctx->gpr[4], ps3_qpc_us() / 1e6);
    func_002E1460(ctx);
    saka_drain(ctx);
}

/* The three loader state machines 0x005D59E4 calls through pointers
 * (SAKA_LOADER_CHAIN=1 names them). Direct calls elsewhere are unaffected. */
static void saka_pump_002E0030(ppu_context* c) { saka_loader_pump(c, func_002E0030); }
static void saka_pump_002E6EE8(ppu_context* c) { saka_loader_pump(c, func_002E6EE8); }
static void saka_pump_002E7AC8(ppu_context* c) { saka_loader_pump(c, func_002E7AC8); }

static void saka_install_loader_pump(void)
{
    const char* e = getenv("SAKA_LOADER_PUMP");
    s_pump_on = !(e && *e == '0');
    if (const char* b = getenv("SAKA_LOADER_PUMP_MS")) s_pump_budget_us = (unsigned)atoi(b) * 1000u;
    if (const char* cb = getenv("SAKA_LOADER_CB_TRACE")) if (*cb == '1') ppu_register_function(0x002E1460u, saka_loader_cb_trace);
    if (!s_pump_on) return;
    ppu_register_function(0x002E0030u, saka_pump_002E0030);
    ppu_register_function(0x002E6EE8u, saka_pump_002E6EE8);
    ppu_register_function(0x002E7AC8u, saka_pump_002E7AC8);
    fprintf(stderr, "[saka] loader pump on: the pxd loaders step several times a frame (%u ms budget)\n",
            s_pump_budget_us / 1000u);
}

/* The game's sound output object (the PCM feed's r3). Its ring writer
 * (func_0071EA60) writes block [obj+0x200C] (the next block it will write) while
 * (w - r) mod 8 < lead ([obj+0x2008], 5 as shipped) into port [obj+0x2010].
 *  - SAKA_AUDIO_LEAD (default 7, 0 = as shipped): blocks it may run ahead, so a
 *    late CRI server cycle (its SPU jobs run interpreted) still finds data queued.
 *  - SAKA_AUDIO_HOLD (default on, 0 = off): tells cellAudio when the block it is
 *    about to read is the writer's next one, so the mixer waits for it instead of
 *    playing a stale block and costing a whole 8-block lap of silence. */
extern "C" void cellAudio_set_starve_check(int (*fn)(uint32_t port, uint32_t next_block));
static volatile uint32_t s_snd_obj = 0;
static uint32_t s_snd_lead = 7;
static int saka_audio_starved(uint32_t port, uint32_t next_block)
{
    const uint32_t obj = s_snd_obj;
    if (!obj || vm_read32(obj + 0x2010) != port || vm_read32(obj + 0x2008) <= 1) return 0;
    return vm_read32(obj + 0x200C) == next_block;
}
static void saka_audio_note_obj(uint32_t obj)
{
    if (obj == s_snd_obj || !obj) return;
    s_snd_obj = obj;
    const uint32_t lead = vm_read32(obj + 0x2008);
    if (s_snd_lead && lead > 1 && lead < s_snd_lead && s_snd_lead < 8) {
        vm_write32(obj + 0x2008, s_snd_lead);
        fprintf(stderr, "[saka] sound ring lead %u -> %u blocks (obj 0x%08X)\n", lead, s_snd_lead, obj);
    }
}
static void saka_audio_feed(ppu_context* ctx)
{
    saka_audio_note_obj((uint32_t)ctx->gpr[3]);
    func_0071EB88(ctx);
    saka_drain(ctx);
}

/* SAKA_AUDIO_PROBE=1: once a second, what the game's PCM feed 0x0071EB88
 * (r3 obj, r4 channels, r5 samples, r6 per-channel float pointers) received and
 * how many blocks it wrote into the cellAudio ring. Says whether the mixer
 * upstream hands over sound or silence. Only called through pointers. */
static void saka_audio_feed_probe(ppu_context* ctx)
{
    const uint32_t obj = (uint32_t)ctx->gpr[3], nch = (uint32_t)ctx->gpr[4];
    const uint32_t ns = (uint32_t)ctx->gpr[5], chp = (uint32_t)ctx->gpr[6];
    const unsigned lr = (unsigned)ctx->lr, tid = (unsigned)ctx->thread_id;
    float pk = 0.0f;
    for (uint32_t c = 0; c < nch && c < 8 && chp; c++) {
        const uint32_t p = vm_read32(chp + c * 4);
        for (uint32_t s = 0; p && s < ns && s < 4096; s++) {
            const uint32_t v = vm_read32(p + s * 4); float f; memcpy(&f, &v, 4);
            if (f < 0) f = -f;
            if (f > pk && f < 1e6f) pk = f;
        }
    }
    saka_audio_note_obj(obj);
    const uint32_t w0 = vm_read32(obj + 0x200C);
    func_0071EB88(ctx);
    saka_drain(ctx);
    const uint32_t w1 = vm_read32(obj + 0x200C);
    static unsigned long long t0, last_call, batch_t, gap_max, gap_sum; static unsigned calls, samples, writes, batches; static float peak;
    calls++; samples += ns; if (w1 != w0) writes++; if (pk > peak) peak = pk;
    const unsigned long long now = ps3_qpc_us();
    /* A server cycle feeds in a burst; the gap between bursts is how late the next one came. */
    if (now - last_call > 3000ull) {
        if (batch_t) { const unsigned long long g = now - batch_t; gap_sum += g; if (g > gap_max) gap_max = g; batches++; }
        batch_t = now;
    }
    last_call = now;
    if (!t0) t0 = now;
    if (now - t0 >= 1000000ull) {
        fprintf(stderr, "[audio-probe] calls=%u samples=%u nch=%u peak_in=%.4f ring_writes=%u w=%u lead=%u cycles=%u avg=%.1fms max=%.1fms lr=0x%08X tid=%u\n",
                calls, samples, nch, peak, writes, w1, vm_read32(obj + 0x2008), batches,
                batches ? gap_sum / 1000.0 / batches : 0.0, gap_max / 1000.0, lr, tid);
        calls = samples = writes = batches = 0; peak = 0.0f; gap_max = gap_sum = 0; t0 = now;
    }
}

/* Each distinct job binary pushed (CellSpursJobHeader: eaBinary u64 @0,
 * sizeBinary u16 @8 in 16-byte units), once, with the descriptor's first words. */
static void saka_jq_note_job(uint32_t desc)
{
    if (!desc) return;
    static uint32_t seen[64]; static unsigned n_seen;
    const uint32_t ea = vm_read32(desc + 4), sz = vm_read32(desc + 8) >> 16;
    for (unsigned i = 0; i < n_seen; i++) if (seen[i] == ea) return;
    if (n_seen < 64) seen[n_seen++] = ea;
    fprintf(stderr, "[jq-job] binary ea=0x%08X size=0x%X (desc 0x%08X: %08X %08X %08X %08X %08X %08X %08X %08X)\n",
            ea, sz * 16, desc, vm_read32(desc), vm_read32(desc + 4), vm_read32(desc + 8), vm_read32(desc + 12),
            vm_read32(desc + 16), vm_read32(desc + 20), vm_read32(desc + 24), vm_read32(desc + 28));
}

/* SAKA_JQ_TRACE=1: the job-queue lifecycle (libspurs_jq exports the game and CRI
 * call) and the libsre workload calls behind it, with arguments and results.
 * Only calls made through pointers or import stubs are seen. */
#define SAKA_TRACE_FN(PFX, ADDR, NAME, EVERY)                                              \
    void PFX##_func_##ADDR(ppu_context*);                                                 \
    static void saka_tr_##ADDR(ppu_context* c)                                            \
    {                                                                                     \
        const unsigned long long a3 = c->gpr[3], a4 = c->gpr[4], a5 = c->gpr[5],           \
                                 a6 = c->gpr[6], a7 = c->gpr[7], a8 = c->gpr[8],           \
                                 a9 = c->gpr[9], a10 = c->gpr[10];                         \
        const uint32_t sp = (uint32_t)c->gpr[1];                                          \
        const uint32_t s9 = vm_read32(sp + 0x74), s10 = vm_read32(sp + 0x7C),             \
                       s11 = vm_read32(sp + 0x84), s12 = vm_read32(sp + 0x8C);            \
        const unsigned lr = (unsigned)c->lr, tid = (unsigned)c->thread_id;                \
        if (strstr(NAME, "PushJobBody")) saka_jq_note_job((uint32_t)a4);                  \
        PFX##_func_##ADDR(c);                                                             \
        saka_drain(c);                                                                    \
        static volatile LONG n = 0;                                                       \
        const LONG k = InterlockedIncrement(&n);                                          \
        if (k <= 12 || (EVERY && k % EVERY == 0) || ((unsigned)c->gpr[3] != 0 && k <= 200)) \
            fprintf(stderr, "[jq] %s#%ld(0x%llX, 0x%llX, 0x%llX, 0x%llX, 0x%llX, 0x%llX, 0x%llX, 0x%llX | stk 0x%X 0x%X 0x%X 0x%X) -> 0x%08X lr=0x%08X tid=%u t=%.3f\n", \
                    NAME, (long)k, a3, a4, a5, a6, a7, a8, a9, a10, s9, s10, s11, s12,       \
                    (unsigned)c->gpr[3], lr, tid, ps3_qpc_us() / 1e6);                    \
    }
SAKA_TRACE_FN(libspurs_jq, 301003CC, "_cellSpursCreateJobQueueWithJobDescriptorPool", 0)
SAKA_TRACE_FN(libspurs_jq, 30100CA8, "_cellSpursCreateJobQueue", 0)
SAKA_TRACE_FN(libspurs_jq, 301002F0, "cellSpursShutdownJobQueue", 0)
SAKA_TRACE_FN(libspurs_jq, 30100CF0, "cellSpursJoinJobQueue", 0)
SAKA_TRACE_FN(libspurs_jq, 30102E60, "cellSpursJobQueueOpen", 0)
SAKA_TRACE_FN(libspurs_jq, 30102D80, "cellSpursJobQueueClose", 0)
SAKA_TRACE_FN(libspurs_jq, 301033A0, "cellSpursJobQueueSemaphoreInitialize", 0)
SAKA_TRACE_FN(libspurs_jq, 301046F0, "cellSpursJobQueuePort2Create", 0)
SAKA_TRACE_FN(libspurs_jq, 30104814, "cellSpursJobQueuePort2Destroy", 0)
SAKA_TRACE_FN(libspurs_jq, 301045D8, "cellSpursJobQueuePort2Sync", 500)
SAKA_TRACE_FN(libspurs_jq, 30104E0C, "_cellSpursJobQueuePort2CopyPushJobBody", 500)
SAKA_TRACE_FN(libspurs_jq, 30104E48, "_cellSpursJobQueuePort2PushJobBody", 500)
SAKA_TRACE_FN(libsre, 30009F0C, "cellSpursAddWorkloadWithAttribute", 0)
SAKA_TRACE_FN(libsre, 30009FC8, "cellSpursAddWorkload", 0)
SAKA_TRACE_FN(libsre, 3000A158, "cellSpursShutdownWorkload", 0)
SAKA_TRACE_FN(libsre, 3000A304, "cellSpursWaitForWorkloadShutdown", 0)
SAKA_TRACE_FN(libsre, 3000A50C, "cellSpursRemoveWorkload", 0)
SAKA_TRACE_FN(libsre, 3000A960, "cellSpursReadyCountAdd", 500)
SAKA_TRACE_FN(libsre, 3000AC24, "cellSpursReadyCountStore", 500)
SAKA_TRACE_FN(libsre, 3000AAC4, "cellSpursReadyCountCompareAndSwap", 500)
SAKA_TRACE_FN(libsre, 3000A750, "cellSpursSendWorkloadSignal", 500)

/* Boot-logo fast forward state, driven by the files that mark the screens:
 * 0 = before the notice screen, 1 = notice/logos showing (fast), 2 = title
 * reached (never again). The frame clock reads it. */
static volatile LONG s_boot_ff = 0;
static void saka_on_file_open(const char* p)
{
    if (s_boot_ff == 0 && strstr(p, "sb/attention.par")) InterlockedExchange(&s_boot_ff, 1);
    else if (s_boot_ff != 2 && strstr(p, "sb2/bg_top_menu.par")) InterlockedExchange(&s_boot_ff, 2);
}

/* SAKA_DECODE_TRACE=1: the async file thread's decode tasks (sl_decoder):
 * cellSpursCreateTask2WithBinInfo's argument block and binInfo, and how long
 * the cellSpursJoinTask2 that waits for the task takes. */
void libsre_func_300121EC(ppu_context*);   /* cellSpursCreateTask2WithBinInfo */
void libsre_func_30011484(ppu_context*);   /* cellSpursJoinTask2 */
static void saka_tr_create_task2_bin(ppu_context* c)
{
    const uint32_t bin = (uint32_t)c->gpr[5], arg = (uint32_t)c->gpr[6], lr = (uint32_t)c->lr;
    static volatile LONG n = 0; const LONG k = InterlockedIncrement(&n);
    if (k <= 12)
        fprintf(stderr, "[decode] create#%ld lr=0x%08X tid=%u bin: %08X %08X %08X %08X %08X %08X  arg: %08X %08X %08X %08X\n",
                (long)k, lr, (unsigned)c->thread_id, vm_read32(bin), vm_read32(bin + 4), vm_read32(bin + 8),
                vm_read32(bin + 12), vm_read32(bin + 16), vm_read32(bin + 20),
                vm_read32(arg), vm_read32(arg + 4), vm_read32(arg + 8), vm_read32(arg + 12));
    if (k <= 6 && vm_read32(arg + 4) && vm_read32(arg + 4) < 0x40000000u) {
        const uint32_t p = vm_read32(arg + 4);
        fprintf(stderr, "[decode]   *arg[1] 0x%08X: %08X %08X %08X %08X %08X %08X %08X %08X\n", p,
                vm_read32(p), vm_read32(p + 4), vm_read32(p + 8), vm_read32(p + 12),
                vm_read32(p + 16), vm_read32(p + 20), vm_read32(p + 24), vm_read32(p + 28));
    }
    libsre_func_300121EC(c);
    saka_drain(c);
}
static void saka_tr_join_task2(ppu_context* c)
{
    const unsigned long long t0 = ps3_qpc_us();
    const unsigned lr = (unsigned)c->lr;
    libsre_func_30011484(c);
    saka_drain(c);
    static unsigned long long win0, sum, mx; static unsigned n;
    const unsigned long long t1 = ps3_qpc_us(), d = t1 - t0;
    if (!win0) win0 = t1;
    n++; sum += d; if (d > mx) mx = d;
    if (t1 - win0 >= 2000000ull) {
        fprintf(stderr, "[decode] last 2 s: %u joins, %.1f ms avg, %.1f ms max (lr=0x%08X)\n",
                n, n ? sum / 1000.0 / n : 0.0, mx / 1000.0, lr);
        n = 0; sum = mx = 0; win0 = t1;
    }
}

static void saka_install_audio_probe(void)
{
    g_ppu_fs_open_hook = saka_on_file_open;
    if (const char* dt = getenv("SAKA_DECODE_TRACE")) if (*dt == '1') {
        ppu_register_function(0x300121ECu, saka_tr_create_task2_bin);
        ppu_register_function(0x30011484u, saka_tr_join_task2);
    }
    if (const char* t = getenv("SAKA_JQ_TRACE")) if (*t == '1') {
        ppu_register_function(0x301003CCu, saka_tr_301003CC);
        ppu_register_function(0x30100CA8u, saka_tr_30100CA8);
        ppu_register_function(0x301002F0u, saka_tr_301002F0);
        ppu_register_function(0x30100CF0u, saka_tr_30100CF0);
        ppu_register_function(0x30102E60u, saka_tr_30102E60);
        ppu_register_function(0x30102D80u, saka_tr_30102D80);
        ppu_register_function(0x301033A0u, saka_tr_301033A0);
        ppu_register_function(0x301046F0u, saka_tr_301046F0);
        ppu_register_function(0x30104814u, saka_tr_30104814);
        ppu_register_function(0x301045D8u, saka_tr_301045D8);
        ppu_register_function(0x30104E0Cu, saka_tr_30104E0C);
        ppu_register_function(0x30104E48u, saka_tr_30104E48);
        ppu_register_function(0x30009F0Cu, saka_tr_30009F0C);
        ppu_register_function(0x30009FC8u, saka_tr_30009FC8);
        ppu_register_function(0x3000A158u, saka_tr_3000A158);
        ppu_register_function(0x3000A304u, saka_tr_3000A304);
        ppu_register_function(0x3000A50Cu, saka_tr_3000A50C);
        ppu_register_function(0x3000A960u, saka_tr_3000A960);
        ppu_register_function(0x3000AC24u, saka_tr_3000AC24);
        ppu_register_function(0x3000AAC4u, saka_tr_3000AAC4);
        ppu_register_function(0x3000A750u, saka_tr_3000A750);
        fprintf(stderr, "[saka] job-queue trace on\n");
    }
    if (const char* l = getenv("SAKA_AUDIO_LEAD")) s_snd_lead = (uint32_t)atoi(l);
    const char* h = getenv("SAKA_AUDIO_HOLD");
    if (!(h && *h == '0')) cellAudio_set_starve_check(saka_audio_starved);
    const char* e = getenv("SAKA_AUDIO_PROBE");
    if (e && *e == '1') {
        ppu_register_function(0x0071EB88u, saka_audio_feed_probe);
        fprintf(stderr, "[saka] audio probe on (0x0071EB88)\n");
    } else {
        ppu_register_function(0x0071EB88u, saka_audio_feed);
    }
}

/* Draws in the last frame, from whichever renderer is live. */
static unsigned saka_last_draws(void)
{
#if defined(_WIN32)
    if (s_rsx_live) return rsx_live_draw_get_last_draws();
#endif
    return rsx_d3d12_last_frame_draws();
}

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define STACK_SIZE    0x00100000u      /* what the main thread is told it has */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
static volatile LONG g_frames_presented = 0;
/* Sakatsuku diagnostic: exceptions seen by vm_commit_veh, printed every 2 s by
 * the frame clock when SAKA_COUNT_CALLS=1. */
static volatile LONG g_saka_veh_count = 0, g_saka_veh_av = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

/* Live-engine present time per [perf] second (frame clock thread only). */
static unsigned           g_present_n = 0;
static unsigned long long g_present_sum_us = 0, g_present_max_us = 0;
static unsigned long long g_kick_wait_us = 0;          /* waiting for the guest's next commands */
static unsigned g_present_site[3];                      /* presents per call site, per [perf] second */
static long long s_last_pending_present = 0;            /* QPC of the last flip-pending present */
extern "C" unsigned long long rsx_live_draw_fence_wait_us(unsigned* waits);

static void present_guest_frame(void)
{
    /* PS3_FTRACE: the D3D12 backend records no PRESENT span of its own, and
     * PERF's render_frame time stops before the swap-chain Present. */
    const uint64_t ft_t0 = PS3_FT_ON() ? ps3_ft_now() : 0;
#if defined(_WIN32)
    /* The live engine presents the surface of the buffer the guest flipped to
     * (cellGcmSetFlipCommand set it on this thread just before the pending
     * flag), or the 1280x720 colour target when the flip names none. */
    if (s_rsx_live) {
        /* Always on: a present that takes over 80 ms is one line, so a
         * "frame clock STUCK" freeze says whether the time went into presenting
         * (the live engine replays the frame's draws here) or elsewhere. */
        const unsigned long long p0 = ps3_qpc_us();
        rsx_live_draw_present(cellGcmGetCurrentDisplayBufferId());
        const unsigned long long pd = ps3_qpc_us() - p0;
        g_present_n++; g_present_sum_us += pd; if (pd > g_present_max_us) g_present_max_us = pd;
        static unsigned slow_presents = 0;
        if (pd > 80000ull && ++slow_presents <= 200)
            fprintf(stderr, "[slow] present %.0f ms (draws %u, t=%.3f)\n", pd / 1000.0,
                    rsx_live_draw_get_last_draws(), ps3_qpc_us() / 1e6);
    }
    else
#endif
    rsx_backend_present();
    if (ft_t0) ps3_ft_span(PS3_FT_PRESENT, 0, 1, 0, ft_t0, ps3_ft_now());
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
    /* SAKA_FLIPLOG=1: a timestamp per presented frame, to line frame
     * boundaries up with the SEMTID / [fs] open timestamps. */
    { static int on = -1; if (on < 0) on = getenv("SAKA_FLIPLOG") ? 1 : 0;
      if (on) fprintf(stderr, "[flip] t=%.3f\n", ps3_qpc_us() / 1e6); }
}

/* [freeze] watchdog, on a thread of its own: the frame clock below is the
 * thread that blocks when the GPU or a Present hangs, so it cannot report
 * that. No guest frame for 1.2 s (after the first one) prints where every
 * guest thread is (lr = its last direct call site) and whether the frame
 * clock itself is still turning, again every 3 s while it lasts. */
static volatile LONG g_frame_clock_beats = 0;
static DWORD WINAPI freeze_watchdog(LPVOID)
{
    unsigned flips = 0; LONG beats = 0;
    ULONGLONG since = GetTickCount64(), next = 0, beat_t = since;
    int reports = 0;
    for (;;) {
        Sleep(200);
        const ULONGLONG now = GetTickCount64();
        const LONG b = g_frame_clock_beats;
        if (b != beats) { beats = b; beat_t = now; }
        const unsigned fc = cellGcm_flip_request_count();
        if (fc != flips) {
            if (next) fprintf(stderr, "[freeze] over after %.1f s\n", (now - since) / 1000.0);
            flips = fc; since = now; next = 0;
            continue;
        }
        if (fc && now - since > 1200 && (!next || now >= next) && reports < 40) {
            fprintf(stderr, "[freeze] no frame for %.1f s (t=%.3f, last draws %u, frame clock %s); guest threads:\n",
                    (now - since) / 1000.0, ps3_qpc_us() / 1e6, saka_last_draws(),
                    now - beat_t > 1000 ? "STUCK (GPU wait / Present?)" : "running");
            ppu_report_guest_lrs();
            fflush(stderr);
            next = now + 3000; reports++;
        }
    }
    return 0;
}

/* SAKA_SPURS_DUMP=<seconds>: the SPURS workload table every n seconds (CellSpurs at
 * SAKA_SPURS_EA, default 0x17D00080: ready count, contention, state, signal, and each
 * workload's policy-module address, argument, and first 0x40 argument bytes).
 * Says whether a stalled job queue is not requested, not scheduled, or running. */
static uint8_t saka_rd8(uint32_t ea) { const uint32_t w = vm_read32(ea & ~3u); return (uint8_t)(w >> (24 - 8 * (ea & 3))); }
static DWORD WINAPI spurs_dump_thread(LPVOID p)
{
    const unsigned secs = (unsigned)(uintptr_t)p;
    const char* e = getenv("SAKA_SPURS_EA");
    const uint32_t sp = e ? (uint32_t)strtoul(e, 0, 0) : 0x17D00080u;
    for (;;) {
        Sleep(secs * 1000u);
        uint8_t b[0x100];
        for (unsigned i = 0; i < 0x100; i++) b[i] = saka_rd8(sp + i);
        fprintf(stderr, "[spurs] t=%.1f enabled=%08X sig1=%02X%02X sig2=%02X%02X idling=%02X flagRecv=%02X sysMsg=%02X nSpus=%u\n",
                ps3_qpc_us() / 1e6, vm_read32(sp + 0xB0), b[0x70], b[0x71], b[0x78], b[0x79], b[0x73], b[0x77], b[0x72], b[0x76]);
        for (unsigned w = 0; w < 32; w++) {
            const unsigned lo = w & 15, hi = w >> 4;
            const uint8_t st = hi ? b[0xD0 + lo] : b[0x80 + lo];
            if (!st) continue;
            const uint32_t info = sp + (hi ? 0x1000u + lo * 0x20u : 0xB00u + lo * 0x20u);
            const uint32_t addr = vm_read32(info + 4), arg = vm_read32(info + 12);
            fprintf(stderr, "[spurs]  wid=%2u st=%u status=%02X ev=%02X ready=%02X cur=%02X pend=%02X max=%02X pm=0x%08X arg=0x%08X uid=%u prio=%08X%08X\n",
                    w, st, hi ? b[0xE0 + lo] : b[0x90 + lo], hi ? b[0xF0 + lo] : b[0xA0 + lo],
                    hi ? b[0x10 + lo] : b[0x00 + lo], b[0x20 + lo], b[0x30 + lo], b[0x50 + lo],
                    addr, arg, vm_read32(info + 0x10), vm_read32(info + 0x14), vm_read32(info + 0x18));
            if (arg && arg < 0x40000000u) {
                fprintf(stderr, "[spurs]    arg:");
                for (unsigned i = 0; i < 0x40; i += 4) fprintf(stderr, " %08X", vm_read32(arg + i));
                fprintf(stderr, "\n");
            }
        }
        fflush(stderr);
    }
    return 0;
}

/* SAKA_HOST_SAMPLE=<start s>:<seconds>:<file>: a sampling profile of the HOST
 * code the frame-clock thread runs (FIFO drain + live-engine present), which
 * PS3_SAMPLE (guest code only) cannot see. Every ~0.5 ms the thread is
 * suspended and its RIP binned by module RVA in a preallocated table (no
 * allocation while it is suspended); at the end "rva count" lines go to
 * <file>, resolved by tools/host_sample_report.py against the linker map. */
static HANDLE s_frame_clock_thread = NULL;
static HANDLE s_main_guest_thread = NULL;   /* the boot thread: runs the guest main loop */
extern "C" ppu_context* volatile g_ppu_main_ctx;   /* ppu_loader.cpp ppu_run */
static DWORD WINAPI host_sample_thread(LPVOID)
{
    const char* e = getenv("SAKA_HOST_SAMPLE");
    double start = 0, secs = 0; char path[512] = {0};
    if (!e || sscanf(e, "%lf:%lf:%511[^\n]", &start, &secs, path) != 3) {
        fprintf(stderr, "[hostsamp] SAKA_HOST_SAMPLE=<start s>:<seconds>:<file>\n");
        return 0;
    }
    const char* which = getenv("SAKA_HOST_SAMPLE_THREAD");
    const int main_thread = which && !strcmp(which, "main");
    Sleep((DWORD)(start * 1000.0));
    HANDLE const target = main_thread ? s_main_guest_thread : s_frame_clock_thread;
    enum { N = 1u << 16 };
    static uint64_t key[N]; static uint32_t cnt[N];
    static uint32_t lrk[256], lrn[256];   /* guest lr while the main thread waits in a DLL */
    const uint64_t base = (uint64_t)GetModuleHandleA(NULL);
    unsigned long long total = 0, lost = 0;
    const ULONGLONG end = GetTickCount64() + (ULONGLONG)(secs * 1000.0);
    timeBeginPeriod(1);
    while (GetTickCount64() < end && target) {
        if (SuspendThread(target) != (DWORD)-1) {
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
            const BOOL ok = GetThreadContext(target, &c);
            ResumeThread(target);
            if (ok) {
                const uint64_t k = c.Rip - base;
                uint32_t i = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 48) & (N - 1);
                unsigned probes = 0;
                while (key[i] && key[i] != k && probes < N) { i = (i + 1) & (N - 1); probes++; }
                if (probes < N) { key[i] = k; cnt[i]++; total++; } else lost++;
                /* Main thread outside the exe (a wait in ntdll): which guest
                 * call led there -- the context's lr is the last call site. */
                if (main_thread && g_ppu_main_ctx && c.Rip - base >= 0x10000000ull) {
                    const uint32_t lr = (uint32_t)g_ppu_main_ctx->lr;
                    uint32_t j = (lr * 2654435761u) >> 24;
                    for (unsigned p = 0; p < 256; p++, j = (j + 1) & 255)
                        if (!lrk[j] || lrk[j] == lr) { lrk[j] = lr; lrn[j]++; break; }
                }
            }
        }
        Sleep(0); YieldProcessor();
        LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        do { QueryPerformanceCounter(&t1); } while ((t1.QuadPart - t0.QuadPart) * 2000 < f.QuadPart);   /* ~0.5 ms */
    }
    timeEndPeriod(1);
    FILE* fo = fopen(path, "w");
    if (fo) {
        fprintf(fo, "# host samples of the %s thread: %llu (lost %llu), base 0x%llX\n",
                main_thread ? "guest main" : "frame-clock", total, lost, (unsigned long long)base);
        /* Samples outside the exe image: total them by DLL (driver, D3D12, ntdll...). */
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
        const uint64_t image_size = nt->OptionalHeader.SizeOfImage;
        struct { char name[64]; unsigned long long n; } mods[64]; int nm = 0;
        for (uint32_t i = 0; i < N; i++) {
            if (!cnt[i]) continue;
            if (key[i] < image_size) { fprintf(fo, "%llX %u\n", (unsigned long long)key[i], cnt[i]); continue; }
            HMODULE hm = NULL; char full[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(uintptr_t)(base + key[i]), &hm) && hm)
                GetModuleFileNameA(hm, full, sizeof full);
            const char* sl = strrchr(full, '\\'); const char* nmx = sl ? sl + 1 : full;
            int k = 0; for (; k < nm; k++) if (!strcmp(mods[k].name, nmx)) break;
            if (k == nm && nm < 64) { snprintf(mods[nm].name, sizeof mods[nm].name, "%s", nmx); mods[nm].n = 0; nm++; }
            if (k < 64) mods[k].n += cnt[i];
        }
        for (int k = 0; k < nm; k++) fprintf(fo, "M %s %llu\n", mods[k].name, mods[k].n);
        /* "# lr <guest lr> <samples>": the guest call site of main-thread waits. */
        for (int k = 0; k < 256; k++) if (lrn[k]) fprintf(fo, "# lr %08X %u\n", lrk[k], lrn[k]);
        /* ...and inside ntdll/kernelbase, by the nearest export below each address. */
        for (uint32_t i = 0; i < N; i++) {
            if (!cnt[i] || key[i] < image_size) continue;
            const uint64_t a = base + key[i];
            HMODULE hm = NULL; char full[MAX_PATH] = "?";
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCSTR)(uintptr_t)a, &hm) || !hm) continue;
            GetModuleFileNameA(hm, full, sizeof full);
            if (!strstr(full, "ntdll") && !strstr(full, "KERNELBASE") && !strstr(full, "kernel32")) continue;
            const uint8_t* mb = (const uint8_t*)hm;
            const IMAGE_NT_HEADERS* mn = (const IMAGE_NT_HEADERS*)(mb + ((const IMAGE_DOS_HEADER*)mb)->e_lfanew);
            const IMAGE_DATA_DIRECTORY dd = mn->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (!dd.VirtualAddress) continue;
            const IMAGE_EXPORT_DIRECTORY* ed = (const IMAGE_EXPORT_DIRECTORY*)(mb + dd.VirtualAddress);
            const DWORD* fns = (const DWORD*)(mb + ed->AddressOfFunctions);
            const DWORD* nms = (const DWORD*)(mb + ed->AddressOfNames);
            const WORD* ords = (const WORD*)(mb + ed->AddressOfNameOrdinals);
            const uint64_t rva = a - (uint64_t)mb; DWORD best = 0; const char* bn = "?";
            for (DWORD j = 0; j < ed->NumberOfNames; j++) {
                const DWORD f = fns[ords[j]];
                if (f <= rva && f > best) { best = f; bn = (const char*)(mb + nms[j]); }
            }
            const char* sl = strrchr(full, '\\');
            fprintf(fo, "E %s!%s %u\n", sl ? sl + 1 : full, bn, cnt[i]);
        }
        fclose(fo);
    }
    fprintf(stderr, "[hostsamp] %llu samples of the frame-clock thread -> %s\n", total, path);
    return 0;
}

/* Sakatsuku: vblank on its own timer thread. The drain loop below used to
 * tick vblank between drains, so a drain of one match frame (~20 ms of
 * command translation) held the 60 Hz beat back and then fired the missed
 * ticks in a burst. The game paces itself on those ticks -- it waits for two
 * per frame (0x0069E4FC) -- so every late beat cost it time: 25-28 fps on a
 * host much faster than the console. Real hardware's vblank never waits for
 * the RSX. The drain thread keeps the drain and the presents; this thread
 * only advances the count and completes executed flips, both of which are
 * locked/atomic (cellGcmTickVBlank, cellGcmTickFlip).
 * SAKA_VBLANK_THREAD=0 restores ticking inside the drain loop. */
extern "C" volatile uint32_t g_step_census_frame;   /* ppu_loader.cpp PPU_STEP_CENSUS: guest frame index */
extern "C" void ppu_lwcond_wake_all(void);           /* ppu_sysprx.cpp: wake lwcond waiters */
static volatile long long s_vbt_period = 0;   /* QPC ticks per beat (turbo / boot ff change it) */
static volatile LONG s_vbt_resync = 0;        /* restart the beat from now */
static volatile LONG s_vbt_ticks = 0;         /* beats, for [perf] */
static volatile LONG s_vbt_maxgap_us = 0;     /* longest beat-to-beat gap since the last [perf] */
static int s_vbt_on = 0;

static void vbt_note_beat(long long now, long long qpf)
{
    static long long last = 0;
    if (last) {
        const LONG gap = (LONG)(((now - last) * 1000000) / qpf);
        if (gap > s_vbt_maxgap_us) s_vbt_maxgap_us = gap;
    }
    last = now;
    InterlockedIncrement(&s_vbt_ticks);
}

static DWORD WINAPI vblank_thread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+): ~0.5 ms
     * wakeups. Older systems get a plain timer at timeBeginPeriod(1). */
    HANDLE tmr = CreateWaitableTimerExW(NULL, NULL, 0x00000002, TIMER_ALL_ACCESS);
    if (!tmr) tmr = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    LARGE_INTEGER qpf, q;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&q);
    long long next = q.QuadPart;
    for (;;) {
        QueryPerformanceCounter(&q);
        if (InterlockedExchange(&s_vbt_resync, 0)) next = q.QuadPart;
        const long long left = next - q.QuadPart;
        if (left > 0) {
            if (tmr) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)((left * 10000000LL) / qpf.QuadPart);
                if (due.QuadPart > -1) due.QuadPart = -1;
                SetWaitableTimerEx(tmr, &due, 0, NULL, NULL, NULL, 0);
                WaitForSingleObject(tmr, 100);
            } else {
                Sleep(1);
            }
            continue;
        }
        cellGcmTickVBlank();
        cellGcmTickFlip();
        /* Wake lwcond waiters so one runs the vblank handler now instead of a
         * 1 ms poll later (ppu_sysprx.cpp sys_lwcond_wait). */
        ppu_lwcond_wake_all();
        vbt_note_beat(q.QuadPart, qpf.QuadPart);
        const long long p = s_vbt_period;
        next += p > 0 ? p : qpf.QuadPart / 60;
        if (q.QuadPart - next > qpf.QuadPart / 4) next = q.QuadPart;   /* far behind: resync, no burst */
    }
    return 0;
}

static DWORD WINAPI frame_clock(LPVOID)
{
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "ps3recomp";

    int rsx_ok;
#if defined(_WIN32)
    if (rsx_live_draw_enabled()) {
        rsx_ok = (rsx_null_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
        if (rsx_ok && rsx_live_draw_init(rsx_null_backend_get_hwnd(), WINDOW_WIDTH, WINDOW_HEIGHT,
                                         rsx_live_guest_ptr, nullptr) == 0) {
            s_rsx_live = 1;
            rsx_null_backend_suppress_present(1);
            fprintf(stderr, "[rsx] live-draw engine up (D3D12); legacy D3D12 backend not used\n");
        } else {
            fprintf(stderr, "[rsx] live-draw init FAILED -- falling back to the legacy D3D12 backend\n");
            rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
        }
    } else
#endif
    rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", s_rsx_live ? "live-draw" : RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");

    unsigned  last_flip = 0;
    /* Sakatsuku: vblank at exactly 60 Hz from QueryPerformanceCounter. The
     * template ticked every 16 ms of GetTickCount64 (~62.5 Hz, and that clock
     * only advances in ~15.6 ms steps, so ticks arrived in bursts).
     * SAKA_VBLANK_HZ overrides the rate. */
    LARGE_INTEGER qpf, qnow;
    QueryPerformanceFrequency(&qpf);
    /* Sakatsuku 60 fps (SAKA_FPS=60). The game sets its vsync interval, the
     * big-endian word at 0x00EA40C0, to 2 during init (0x001944F8 li r3,2 ->
     * 0x0069BEE0); its frame wait (0x0069E4FC, check at 0x0069E704) then holds
     * each frame until two vblanks have passed: 30 fps. Interval 1 instead
     * skips that wait entirely (real hardware's VSYNC flip did the capping),
     * which ran uncapped here (116 fps in the intro). So keep the game's own
     * pacing and tick vblank at 120 Hz: two ticks per frame = 60 fps. */
    int fps60 = 0;
    { const char* e = getenv("SAKA_FPS"); fps60 = (e && atoi(e) >= 60); }
    double vblank_hz = fps60 ? 120.0 : 60.0;
    { const char* e = getenv("SAKA_VBLANK_HZ"); if (e && atof(e) > 0.0) vblank_hz = atof(e); }
    const long long tick_period = (long long)((double)qpf.QuadPart / vblank_hz + 0.5);
    fprintf(stderr, "[saka] vblank %.1f Hz (SAKA_FPS=%d)\n", vblank_hz, fps60 ? 60 : 30);
    QueryPerformanceCounter(&qnow);
    long long next_tick = qnow.QuadPart;
    { const char* e = getenv("SAKA_VBLANK_THREAD");
      if (rsx_ok && !(e && *e == '0')) {
          s_vbt_period = tick_period;
          HANDLE h = CreateThread(NULL, 256u * 1024, vblank_thread, NULL, 0, NULL);
          if (h) { s_vbt_on = 1; CloseHandle(h); }
      }
      fprintf(stderr, "[saka] vblank beat: %s\n", s_vbt_on ? "own timer thread" : "drain loop"); }

    /* Load turbo. The game is frame-locked and its pxd loader moves one step
     * per frame, so a file costs a whole 33 ms frame although its work takes
     * ~1 ms (console pace, not a port slowdown). While loading is sustained
     * -- at least 9 loader steps (3 files) in the last 0.5 s, then until
     * 0.15 s pass without one -- vblank ticks `turbo_factor` times faster and
     * Present stops waiting for the display's vblank, so the game runs as fast
     * as the host allows and loads finish that much sooner. Anything animating
     * on a loading screen runs faster with it; the game's speed outside loads
     * is unchanged. SAKA_LOAD_TURBO=N sets the factor (default 8, 1 = off). */
    const int turbo_factor = saka_load_turbo_factor();
    const long long turbo_window = qpf.QuadPart / 2, turbo_hold = (qpf.QuadPart * 15) / 100;
    /* ...and only on a loading/transition screen. The loader also streams
     * during interactive scenes (the face editor loads each sample's parts
     * from an open archive with 150-250 draws on screen), which must keep the
     * game's speed. A fade or loading screen draws a handful of quads.
     * SAKA_LOAD_TURBO_DRAWS sets the limit (default 48). */
    unsigned turbo_draws = 48;
    { const char* e = getenv("SAKA_LOAD_TURBO_DRAWS"); if (e) turbo_draws = (unsigned)atoi(e); }
    int turbo = 0, turbo_logs = 0;
    LARGE_INTEGER turbo_t0 = { 0 };
    /* Boot-logo fast forward factor: SAKA_BOOT_FF (default 16, 0 or 1 = off). */
    int boot_ff_factor = 16;
    { const char* e = getenv("SAKA_BOOT_FF"); if (e) boot_ff_factor = atoi(e); }
    long long ff_t0 = 0;

    int count_diag = 0;
    { const char* e = getenv("SAKA_COUNT_CALLS"); count_diag = (e && *e == '1'); }
    ULONGLONG diag_next = GetTickCount64() + 2000;
    LONG diag_veh = 0, diag_av = 0;

    for (;;) {
        if (count_diag && GetTickCount64() >= diag_next) {
            LONG v = g_saka_veh_count, a = g_saka_veh_av;
            fprintf(stderr, "[saka-count] VEH exceptions/s %ld (access violations %ld)\n",
                    (long)((v - diag_veh) / 2), (long)((a - diag_av) / 2));
            diag_veh = v; diag_av = a; diag_next += 2000;
        }
        /* Wait on the FIFO kick, not a flat sleep: a guest polling an RSX fence
         * that has run dry (cellGcm_ref_on_poll) signals it, and the drain then
         * runs at once instead of on the next 1 ms pass. Timeout = old sleep. */
        { const unsigned long long kw0 = ps3_qpc_us();
          cellGcm_fifo_kick_wait(1);
          g_kick_wait_us += ps3_qpc_us() - kw0; }
        QueryPerformanceCounter(&qnow);
        long long now = qnow.QuadPart;

        if (turbo_factor > 1) {
            const int recent = saka_load_steps_since(now - turbo_hold) > 0 &&
                               saka_last_draws() <= turbo_draws;
            const int on = turbo ? recent
                                 : recent && saka_load_steps_since(now - turbo_window) >= 9;
            if (on != turbo) {
                turbo = on;
                rsx_d3d12_set_present_nowait(on);
                static LONG turbo_f0 = 0;
                if (on) { turbo_t0 = qnow; next_tick = now; turbo_f0 = g_frames_presented; InterlockedExchange(&s_vbt_resync, 1); }
                if (turbo_logs < 40) {
                    turbo_logs++;
                    const double s = (double)(now - turbo_t0.QuadPart) / (double)qpf.QuadPart;
                    if (on) fprintf(stderr, "[saka] load turbo on (x%d)\n", turbo_factor);
                    else    fprintf(stderr, "[saka] load turbo off after %.2f s (%ld frames, %.0f fps)\n",
                                    s, (long)(g_frames_presented - turbo_f0),
                                    s > 0.0 ? (double)(g_frames_presented - turbo_f0) / s : 0.0);
                }
            }
        }
        /* Boot-logo fast forward (s_boot_ff, set by the file-open hook): the
         * notice/logo screens are timed in frames and nothing loads behind
         * them (~12 s), so tick vblank boot_ff_factor times faster until the
         * title's first file opens. Frames are still all rendered. */
        { static int ff_prev = 0;
          const int ff = boot_ff_factor > 1 && s_boot_ff == 1;
          if (ff != ff_prev) {
              ff_prev = ff;
              if (s_rsx_live) rsx_live_draw_set_present_nowait(ff); else rsx_d3d12_set_present_nowait(ff);
              if (ff) { next_tick = now; ff_t0 = now; InterlockedExchange(&s_vbt_resync, 1); }
              if (ff) fprintf(stderr, "[saka] boot logos: fast forward x%d\n", boot_ff_factor);
              else    fprintf(stderr, "[saka] boot logos: fast forward off after %.2f s\n",
                              (double)(now - ff_t0) / (double)qpf.QuadPart);
          } }
        /* The player's game speed (g_saka_user_speed: F7 = x2, F8 = turbo, the F2 menu; saka_overlay.cpp): vblank
         * ticks that much faster, and Present stops waiting for the display (every frame is still rendered). */
        const int user_speed = g_saka_user_speed > 1 ? (g_saka_user_speed > 16 ? 16 : g_saka_user_speed) : 1;
        { static int user_prev = 1;
          if (user_speed != user_prev) {
              user_prev = user_speed;
              const int ff_now = boot_ff_factor > 1 && s_boot_ff == 1;
              if (s_rsx_live) rsx_live_draw_set_present_nowait(user_speed > 1 || ff_now);
              else rsx_d3d12_set_present_nowait(user_speed > 1 || turbo);
              next_tick = now; InterlockedExchange(&s_vbt_resync, 1);
              fprintf(stderr, "[saka] game speed x%d\n", user_speed);
          } }
        const long long period = (boot_ff_factor > 1 && s_boot_ff == 1) ? tick_period / boot_ff_factor
                               : turbo ? tick_period / (turbo_factor > user_speed ? turbo_factor : user_speed)
                               : tick_period / user_speed;
        /* Diagnostic slow motion (saka_hooks.c g_saka_diag_slowmo, SAKA_BALL_DUMP_SLOW): the frame dump keeps up. */
        { if (g_saka_diag_slowmo > 1) s_vbt_period = period * g_saka_diag_slowmo; else s_vbt_period = period; }

        InterlockedIncrement(&g_frame_clock_beats);   /* for the freeze watchdog */
        g_step_census_frame = cellGcm_flip_request_count();   /* PPU_STEP_CENSUS frame index */

        /* F12 under the live engine: open SAKA_CAPTURE_GATE (a file) for
         * 0.6 s. The engine's LD_SURF_DUMP_GATE / RSX_DRAW_CSV_GATE watch that
         * file, so one press dumps every render surface once and writes the
         * draw CSV for those frames (run_logged.bat sets all three). */
        if (s_rsx_live) {
            static const char* gate = (const char*)-1;
            static ULONGLONG gate_until = 0;
            if (gate == (const char*)-1) gate = getenv("SAKA_CAPTURE_GATE");
            if (gate && *gate) {
                const ULONGLONG ms = GetTickCount64();
                HWND fg = GetForegroundWindow(); DWORD fpid = 0;
                if (fg) GetWindowThreadProcessId(fg, &fpid);
                if (!gate_until && fpid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F12) & 0x8000)) {
                    FILE* gf = fopen(gate, "w");
                    if (gf) { fputs("1\n", gf); fclose(gf); }
                    gate_until = ms + 600;
                    fprintf(stderr, "[saka] F12: capture gate open (t=%.3f)\n", ps3_qpc_us() / 1e6);
                } else if (gate_until && ms >= gate_until) {
                    DeleteFileA(gate);
                    gate_until = 0;
                    fprintf(stderr, "[saka] F12: capture gate closed\n");
                }
            }
        }

        /* [perf] once a second: presented fps, whether the load turbo was on,
         * the last frame's draws and the loader steps in that second -- so a
         * player's log shows where a delay was and what the game was doing. */
        { static long long perf_next = 0; static unsigned perf_f0 = 0; static int perf_turbo_s = 0;
          static int perf_on = -1;
          if (perf_on < 0) { const char* e = getenv("SAKA_PERF_LOG"); perf_on = !(e && *e == '0'); }
          if (turbo) perf_turbo_s = 1;
          if (perf_on) {
              if (!perf_next) { perf_next = now + qpf.QuadPart; perf_f0 = cellGcm_flip_request_count(); }
              else if (now >= perf_next) {
                  const unsigned f = cellGcm_flip_request_count();   /* guest frames (flips) */
                  static LONG pump_x0 = 0;
                  const LONG px = s_pump_extra;
                  static unsigned long long fw0 = 0; static unsigned fn0 = 0;
                  unsigned fn = 0; const unsigned long long fw = s_rsx_live ? rsx_live_draw_fence_wait_us(&fn) : 0;
                  static LONG vbt_t0 = 0;
                  const LONG vbt_n = s_vbt_ticks, vbt_gap = InterlockedExchange(&s_vbt_maxgap_us, 0);
                  fprintf(stderr, "[perf] t=%.1f fps=%u turbo=%d draws=%u loader=%d pump+=%ld present=%.1f/%.1fms wait: gpu=%.0fms/%u kick=%.0fms sites=%u/%u/%u vbl=%ld/%.1fms\n",
                          ps3_qpc_us() / 1e6, f - perf_f0, perf_turbo_s,
                          saka_last_draws(), saka_load_steps_since(now - qpf.QuadPart),
                          (long)(px - pump_x0),
                          g_present_n ? g_present_sum_us / 1000.0 / g_present_n : 0.0, g_present_max_us / 1000.0,
                          (fw - fw0) / 1000.0, fn - fn0, g_kick_wait_us / 1000.0,
                          g_present_site[0], g_present_site[1], g_present_site[2],
                          (long)(vbt_n - vbt_t0), vbt_gap / 1000.0);
                  vbt_t0 = vbt_n;
                  g_present_site[0] = g_present_site[1] = g_present_site[2] = 0;
                  g_present_n = 0; g_present_sum_us = 0; g_present_max_us = 0;
                  fw0 = fw; fn0 = fn; g_kick_wait_us = 0;
                  pump_x0 = px;
                  perf_f0 = f; perf_turbo_s = turbo; perf_next += qpf.QuadPart;
                  if (now >= perf_next) perf_next = now + qpf.QuadPart;
              }
          } }

        int fired = 0;
        while (!s_vbt_on && (long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            vbt_note_beat(now, qpf.QuadPart);
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && cellGcm_take_flip_pending()) {
                g_present_site[0]++;
                s_last_pending_present = now;
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += period;         /* 60 Hz (SAKA_VBLANK_HZ); faster in load turbo */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                g_present_site[1]++;
                s_last_pending_present = now;
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            cellGcm_rsx_process_fifo();

            const uint64_t ft_p0 = PS3_FT_ON() ? ps3_ft_now() : 0;
            const int pump_rc = s_rsx_live ? rsx_null_backend_pump_messages() : rsx_backend_pump();
            if (ft_p0) ps3_ft_span(PS3_FT_PUMP, 0, 0, 0, ft_p0, ps3_ft_now());
            if (pump_rc != 0) {
                rsx_ok = 0;              /* window closed */
                /* ...so quit, as the console's Quit Game does, instead of
                 * leaving the game running with no window. */
                ps3_host_quit("window closed");
                continue;
            }
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            /* Only as a fallback now: the flip REQUEST count moves when the
             * guest queues a flip, before the drain reaches it, so presenting
             * here showed a half-drawn frame (the match intro's black flashes)
             * and doubled the presents, each one a GPU wait. When flips arrive
             * through the FIFO (the pending flag above), this stays quiet; it
             * still covers boot and any flip that never reaches the FIFO.
             * SAKA_PRESENT_ON_REQUEST=1 restores the old behaviour. */
            unsigned fc = cellGcm_flip_request_count();
            static int on_request = -1;
            if (on_request < 0) { const char* e = getenv("SAKA_PRESENT_ON_REQUEST"); on_request = (e && *e == '1') ? 1 : 0; }
            const int fifo_flips_live = s_last_pending_present &&
                                        (now - s_last_pending_present) < qpf.QuadPart / 10;   /* 100 ms */
            if (fc != last_flip && (on_request || !fifo_flips_live)) {
                g_present_site[2]++;
                present_guest_frame();
                last_flip = fc;
            } else if (fc == 0) {
                if (!s_rsx_live) rsx_backend_present();
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    InterlockedIncrement(&g_saka_veh_count);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        InterlockedIncrement(&g_saka_veh_av);
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    /* MEM_WRITE_WATCH: the OS keeps a per-page written bit (the CPU's own
     * dirty bit, no faults), so the live renderer re-hashes a cached texture
     * only when its pages were written (rsx_live_draw.c ld_ww_*). Hashing every
     * bound texture each frame was ~13% of the frame-clock thread in a match.
     * PS3_VM_WRITE_WATCH=0 reserves without it (the renderer then hashes). */
    { const char* e = getenv("PS3_VM_WRITE_WATCH");
      if (!(e && *e == '0'))
          vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE); }
    if (!vm_base)
        vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is mapped up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It spans the whole 32-bit space plus the guard, as on Windows.
     * It used to stop below 0xE0000000, but the raw SPU windows (local store
     * and problem-state registers, runtime/spu/spu_raw.c) live from there up.
     * Every load and store a title made into one was reported as an
     * out-of-bounds access and dropped, so a raw SPU was never loaded or
     * started, and sys_raw_spu_create, which clears the registers through
     * vm_base directly, wrote past the end of the mapping. With the whole
     * space backed, ppu_vm_size 0 also gives the loader's bounds check the
     * one-compare fast path Windows already has.
     *
     * mmap, not calloc: vm_commit() hands vm_base + addr to mprotect, which
     * takes page-aligned addresses only. glibc serves an allocation this big
     * from its own mmap chunk and returns a pointer 16 bytes past the page
     * start, so every commit failed with EINVAL and sys_memory_allocate
     * answered ENOMEM to every call. mmap's base is page-aligned and zeroed. */
    void* p = mmap(NULL, VM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    vm_base = (p == MAP_FAILED) ? nullptr : (uint8_t*)p;
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#endif
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

/* ---------------------------------------------------------------------------
 * Running the guest
 * -----------------------------------------------------------------------*/
#if defined(__APPLE__)
/* AppKit only runs on the process's main thread. The Metal backend creates
 * its window and pumps its events with dispatch_sync onto the main queue
 * when it is called from any other thread, as it is from the frame clock.
 * With the guest on the main thread nothing ever serviced that queue: the
 * frame clock blocked in its first rsx_backend_init, no vblank or flip was
 * ever delivered and the FIFO never drained. A title that waits for a flip
 * hangs there. So the guest runs on a thread of its own, with the same
 * 256 MB host stack every guest thread gets (sys_ppu_thread.c), and the
 * main thread turns the run loop until ppu_run returns. Headless runs never
 * touch the main queue, which is why CI does not see the difference. */
struct guest_run { uint32_t entry, stack_top; int rc; volatile int done; };

static void* guest_thread(void* arg)
{
    guest_run* g = (guest_run*)arg;
    g->rc = ppu_run(g->entry, g->stack_top);
    __atomic_store_n(&g->done, 1, __ATOMIC_RELEASE);
    CFRunLoopStop(CFRunLoopGetMain());
    return nullptr;
}

static int run_guest(uint32_t entry, uint32_t stack_top)
{
    static guest_run g;
    g.entry = entry; g.stack_top = stack_top; g.rc = 0; g.done = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u * 1024u * 1024u);
    pthread_t th;
    int err = pthread_create(&th, &attr, guest_thread, &g);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        fprintf(stderr, "[boot] could not start the guest thread (%d)\n", err);
        return -1;
    }
    /* Short slices: CFRunLoopStop only ends a run that is already going. */
    while (!__atomic_load_n(&g.done, __ATOMIC_ACQUIRE))
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    pthread_join(th, nullptr);
    return g.rc;
}
#else
static int run_guest(uint32_t entry, uint32_t stack_top) { return ppu_run(entry, stack_top); }
#endif

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: %s <PPU ELF>\n", argv[0]);
        return 2;
    }

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
    /* Sakatsuku: opt out of Windows 11 power throttling. With no visible window
     * (hidden test runs, a minimised or occluded game) Windows applies EcoQoS and
     * ignores the 1 ms timer resolution above, so 1 ms waits become ~15.6 ms and
     * the RSX drain wakes late: the title stalled at ~34 fps with the drain
     * thread at 12% CPU. Same fix as the YKTK3 port. SAKA_POWER_THROTTLING=1
     * keeps Windows' default. Resolved at run time: older SDK headers lack it. */
    {
        const char* pt = getenv("SAKA_POWER_THROTTLING");
        if (!(pt && *pt == '1')) {
            typedef struct { ULONG Version, ControlMask, StateMask; } saka_power_state;
            typedef BOOL (WINAPI *saka_spi_fn)(HANDLE, int, LPVOID, DWORD);
            saka_spi_fn spi = (saka_spi_fn)(void (*)(void))GetProcAddress(
                GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
            /* Version 1; EXECUTION_SPEED (0x1) | IGNORE_TIMER_RESOLUTION (0x4)
             * in ControlMask with StateMask 0 = both throttles off;
             * 4 = ProcessPowerThrottling. */
            saka_power_state s = { 1, 0x1 | 0x4, 0 };
            BOOL ok = spi ? spi(GetCurrentProcess(), 4, &s, (DWORD)sizeof s) : FALSE;
            fprintf(stderr, "[saka] power throttling off (EcoQoS, timer resolution): %s\n",
                    ok ? "ok" : "failed");
        }
    }
#endif

    printf("=== ps3recomp game runner ===\n");

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */
    saka_install_syscall_hooks();   /* Sakatsuku: usleep polls kick the RSX drain (saka_hooks.c) */
    saka_register_spu_task_regions();  /* Sakatsuku: EBOOT SPURS tasks run their lifted code */
    saka_register_cri_job_regions();   /* Sakatsuku: CRI sound jobs run their lifted code */
    saka_register_l10n();              /* Sakatsuku: SJIS/UTF conversions the name entry needs */
    saka_install_loader_pump();        /* Sakatsuku: several loader steps per frame (native-fast loads) */
    saka_install_audio_probe();        /* Sakatsuku: SAKA_AUDIO_PROBE=1 reports the PCM feed */
    ps3_sampler_start();     /* no-op unless PS3_SAMPLE=<ms> (+ PS3_SAMPLE_MAIN / PS3_SAMPLE_DRAIN) */

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
    s_frame_clock_thread = CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);
    /* SAKA_HOST_SAMPLE_THREAD=main samples this thread -- the guest main
     * thread that runs the game's frame loop -- instead of the frame clock. */
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_main_guest_thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    if (getenv("SAKA_HOST_SAMPLE")) CreateThread(NULL, 0, host_sample_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, freeze_watchdog, NULL, 0, NULL);
    if (const char* sd = getenv("SAKA_SPURS_DUMP"))
        if (atoi(sd) > 0) CreateThread(NULL, 0, spurs_dump_thread, (LPVOID)(uintptr_t)atoi(sd), 0, NULL);

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    /* A title that reads its own stack bounds (to find data its parent left
     * below the top, say) gets the stack it really runs on. */
    g_ppu_main_stack_addr = STACK_TOP - STACK_SIZE;
    g_ppu_main_stack_size = STACK_SIZE;

    int rc = run_guest(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}

/* ---- SAKA_POS_SCAN=<match tick>: find what the 30 Hz match tick moves ------
 * Diagnostic only; run at 60 ticks. From the first frame at or after that
 * match tick it copies the guest's committed memory below 0x40000000 at each
 * of SAKA_POS_SCAN_N frames (default 9), noting which frames ran the match
 * tick. Then it writes to logs/posscan.txt every big-endian float that stayed
 * within +-200, changed across at least two frames that ran the tick and never
 * across a frame that did not -- state only the match tick writes -- with its
 * values and the frames' tick counts. */
#include <vector>
#include <math.h>
extern "C" void saka_pos_scan_tick(unsigned frame, unsigned sim_ticks)
{
#if defined(_WIN32)
    static unsigned at = 0, K = 9, taken = 0; static int parsed = 0, done = 0;
    if (!parsed) {
        parsed = 1;
        const char* e = getenv("SAKA_POS_SCAN"); at = e ? (unsigned)strtoul(e, nullptr, 10) : 0;
        const char* k = getenv("SAKA_POS_SCAN_N"); if (k) K = (unsigned)strtoul(k, nullptr, 10);
        if (K < 3) K = 3; if (K > 16) K = 16;
    }
    (void)frame;
    if (!at || done || sim_ticks < at) return;
    struct Reg { uint32_t ea, n; uint8_t* s[16]; };
    static std::vector<Reg> regs;
    static unsigned sims[16];
    auto bef = [](const uint8_t* q) {
        const uint32_t w = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
        float v; memcpy(&v, &w, 4); return v; };
    if (taken == 0) {
        uint8_t* p = vm_base + 0x10000u;
        uint8_t* const end = vm_base + 0x40000000ull;
        while (p < end) {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(p, &mi, sizeof mi)) break;
            uint8_t* rb = (uint8_t*)mi.BaseAddress;
            uint8_t* re = rb + mi.RegionSize;
            if (re > end) re = end;
            if (rb < p) rb = p;
            if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                (mi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) {
                Reg r; memset(&r, 0, sizeof r); r.ea = (uint32_t)(rb - vm_base); r.n = (uint32_t)(re - rb);
                regs.push_back(r);
            }
            p = re;
        }
    }
    for (auto& r : regs) { r.s[taken] = (uint8_t*)malloc(r.n); if (r.s[taken]) memcpy(r.s[taken], vm_base + r.ea, r.n); }
    sims[taken] = sim_ticks;
    fprintf(stderr, "[posscan] frame snapshot %u at match tick %u\n", taken, sim_ticks);
    if (++taken < K) return;
    done = 1;
    FILE* f = fopen("logs/posscan.txt", "w");
    if (f) { fputs("# ticks", f); for (unsigned i = 0; i < K; i++) fprintf(f, " %u", sims[i]); fputc('\n', f); }
    unsigned long long nw_out = 0;
    for (auto& r : regs) {
        int ok = 1;
        for (unsigned i = 0; i < K; i++) if (!r.s[i]) ok = 0;
        if (ok) {
            for (uint32_t o = 0; o + 4 <= r.n; o += 4) {
                if (!memcmp(r.s[0] + o, r.s[K - 1] + o, 4)) continue;
                unsigned simchg = 0; int bad = 0;
                for (unsigned i = 0; i + 1 < K && !bad; i++) {
                    const int same = !memcmp(r.s[i] + o, r.s[i + 1] + o, 4);
                    if (sims[i] == sims[i + 1]) { if (!same) bad = 1; }
                    else if (!same) simchg++;
                }
                if (bad || simchg < 2) continue;
                for (unsigned i = 0; i < K && !bad; i++) { const float v = bef(r.s[i] + o); if (!isfinite(v) || fabsf(v) > 200.f) bad = 1; }
                if (bad) continue;
                nw_out++;
                if (!f) continue;
                fprintf(f, "%08X", r.ea + o);
                for (unsigned i = 0; i < K; i++) fprintf(f, " %.4f", bef(r.s[i] + o));
                fputc('\n', f);
            }
        }
        for (unsigned i = 0; i < 16; i++) free(r.s[i]);
    }
    regs.clear();
    if (f) fclose(f);
    fprintf(stderr, "[posscan] %llu floats change only with the match tick -> logs/posscan.txt\n", nw_out);
#else
    (void)frame; (void)sim_ticks;
#endif
}

/* ---- SAKA_MEM_DUMP=<match tick>: guest memory at two frame starts ----------
 * Diagnostic only; run at 60 ticks. At the start of the first frame without
 * the match tick once that many match ticks ran, and again two frames later
 * (one match tick on), writes the guest's committed memory below 0x40000000
 * to logs/memdump_a.bin / memdump_b.bin as [ea u32][size u32][bytes]
 * records (little-endian headers), for offline searches. */
#if defined(_WIN32)
static void saka_mem_dump_to(const char* path, unsigned sim_ticks);
#endif
extern "C" void saka_mem_dump_frame(unsigned sim_ticks, int no_tick_frame)
{
#if defined(_WIN32)
    /* SAKA_MEM_DUMP_GATE=<file> (diagnostic, menus too): when the file appears, the
     * guest memory goes to logs/memdump_<file's text>.bin and the file is removed,
     * so one run can take several dumps (e.g. before and after a value changes). */
    { static const char* gate = (const char*)-1; static unsigned n = 0;
      if (gate == (const char*)-1) gate = getenv("SAKA_MEM_DUMP_GATE");
      if (gate && ++n % 10 == 0) {
          if (FILE* g = fopen(gate, "rb")) {
              char tag[32] = { 0 }; size_t k = fread(tag, 1, sizeof tag - 1, g); fclose(g); remove(gate);
              while (k && (tag[k - 1] == '\n' || tag[k - 1] == '\r' || tag[k - 1] == ' ')) tag[--k] = 0;
              char path[96]; snprintf(path, sizeof path, "logs/memdump_%s.bin", k ? tag : "gate");
              saka_mem_dump_to(path, sim_ticks);
          }
      } }
    static unsigned at = 0, done = 0; static int parsed = 0;
    if (!parsed) { parsed = 1; const char* e = getenv("SAKA_MEM_DUMP"); at = e ? (unsigned)strtoul(e, nullptr, 10) : 0; }
    if (!at || done >= 2 || !no_tick_frame || sim_ticks < at) return;
    static unsigned first_sim = 0;
    if (done == 1 && sim_ticks == first_sim) return;   /* the next tick-less frame after one more match tick */
    saka_mem_dump_to(done == 0 ? "logs/memdump_a.bin" : "logs/memdump_b.bin", sim_ticks);
    if (done == 0) first_sim = sim_ticks;
    done++;
#else
    (void)sim_ticks; (void)no_tick_frame;
#endif
}
#if defined(_WIN32)
static void saka_mem_dump_to(const char* path, unsigned sim_ticks)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint8_t* p = vm_base + 0x10000u;
    uint8_t* const end = vm_base + 0x40000000ull;
    size_t tot = 0;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(p, &mi, sizeof mi)) break;
        uint8_t* rb = (uint8_t*)mi.BaseAddress;
        uint8_t* re = rb + mi.RegionSize;
        if (re > end) re = end;
        if (rb < p) rb = p;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            const uint32_t hdr[2] = { (uint32_t)(rb - vm_base), (uint32_t)(re - rb) };
            fwrite(hdr, 4, 2, f); fwrite(rb, 1, (size_t)(re - rb), f); tot += (size_t)(re - rb);
        }
        p = re;
    }
    fclose(f);
    fprintf(stderr, "[memdump] %s at match tick %u: %.1f MB\n", path, sim_ticks, tot / 1048576.0);
}
#endif


/* ---- SAKA_WATCH=<ea>[,<ea>...] (up to 4) + SAKA_WATCH_TICK=<match tick> -----
 * Diagnostic only: which host code writes a guest address. From that match
 * tick on, a hardware data breakpoint (write, 4 bytes) on each address in
 * every thread of the process for SAKA_WATCH_FRAMES ticks (default 6); each
 * hit's host code address is counted and written to logs/watch_<i>.txt as
 * "rva count" lines for tools/host_sample_report.py, which names the lifted
 * function. Arming and disarming run on a helper thread (a thread cannot set
 * its own debug registers while running). */
#if defined(_WIN32)
#include <tlhelp32.h>
static uint8_t* s_watch_addr[4]; static uint32_t s_watch_ea[4]; static int s_watch_n;
static struct { volatile uintptr_t rip; volatile LONG n; } s_watch_hits[4][512];
static LONG CALLBACK watch_veh(PEXCEPTION_POINTERS e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD64 dr6 = e->ContextRecord->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t rip = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    for (int i = 0; i < 4; i++) {
        if (!(dr6 & (1ull << i))) continue;
        for (int k = 0; k < 512; k++) {
            if (s_watch_hits[i][k].rip == rip) { InterlockedIncrement(&s_watch_hits[i][k].n); break; }
            if (!s_watch_hits[i][k].rip &&
                InterlockedCompareExchangePointer((PVOID volatile*)&s_watch_hits[i][k].rip, (PVOID)rip, NULL) == NULL) {
                InterlockedIncrement(&s_watch_hits[i][k].n); break; }
        }
    }
    e->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static DWORD WINAPI watch_set_all(LPVOID arm)
{
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te; te.dwSize = sizeof te;
    int set = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &c)) {
                DWORD64 dr7 = 0;
                DWORD64* dr = &c.Dr0;
                for (int i = 0; i < 4; i++) dr[i] = 0;
                if (arm)
                    for (int i = 0; i < s_watch_n; i++) {
                        dr[i] = (DWORD64)s_watch_addr[i];
                        dr7 |= 1ull << (2 * i);          /* local enable */
                        dr7 |= 1ull << (16 + 4 * i);     /* RW = 01: data writes */
                        dr7 |= 3ull << (18 + 4 * i);     /* LEN = 11: 4 bytes */
                    }
                c.Dr7 = dr7; c.Dr6 = 0;
                if (SetThreadContext(th, &c)) set++;
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    fprintf(stderr, "[watch] %s on %d threads\n", arm ? "armed" : "disarmed", set);
    return 0;
}
/* SAKA_WATCH_AUTO=1: other code asks for the addresses (saka_watch_request, up to
 * four); the watch arms on the next tick once four arrived or 16 ticks after the
 * first. */
static uint32_t s_watch_req[4]; static int s_watch_req_n; static unsigned s_watch_req_tick;
extern "C" int saka_watch_auto(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SAKA_WATCH_AUTO"); on = e && *e == '1'; }
    return on;
}
extern "C" void saka_watch_request(uint32_t ea)
{
    if (!saka_watch_auto() || s_watch_req_n >= 4) return;
    for (int i = 0; i < s_watch_req_n; i++) if (s_watch_req[i] == ea) return;
    s_watch_req[s_watch_req_n++] = ea;
    fprintf(stderr, "[watch] requested %08X\n", ea);
}
extern "C" void saka_watch_frame(unsigned sim_ticks)
{
    static int state = -1; static unsigned at = 0, frames = 6, armed_at = 0, tick = 0;
    if (state < 0) {
        state = 0;
        const char* e = getenv("SAKA_WATCH"); const char* t = getenv("SAKA_WATCH_TICK"); const char* fr = getenv("SAKA_WATCH_FRAMES");
        if (fr) frames = (unsigned)strtoul(fr, nullptr, 10);
        if (saka_watch_auto()) { state = 4; at = 0; }
        else if (!e || !t) { state = 3; return; }
        if (state == 0) at = (unsigned)strtoul(t, nullptr, 10);
        for (const char* q = e; state == 0 && *q && s_watch_n < 4; ) {
            char* endp; const unsigned long ea = strtoul(q, &endp, 16);
            if (endp == q) break;
            s_watch_ea[s_watch_n] = (uint32_t)ea; s_watch_addr[s_watch_n] = vm_base + ea; s_watch_n++;
            q = *endp == ',' ? endp + 1 : endp;
        }
    }
    tick++;
    if (state == 4) {
        if (s_watch_req_n && !s_watch_req_tick) s_watch_req_tick = tick;
        if (s_watch_req_n == 4 || (s_watch_req_n && tick - s_watch_req_tick >= 16u)) {
            for (int i = 0; i < s_watch_req_n; i++) { s_watch_ea[i] = s_watch_req[i]; s_watch_addr[i] = vm_base + s_watch_req[i]; }
            s_watch_n = s_watch_req_n; at = sim_ticks; state = 0;
        } else return;
    }
    if (state == 0 && sim_ticks >= at && s_watch_n) {
        AddVectoredExceptionHandler(1, watch_veh);
        HANDLE h = CreateThread(NULL, 0, watch_set_all, (LPVOID)1, 0, NULL);
        if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
        state = 1; armed_at = tick;
    } else if (state == 1 && tick - armed_at >= frames) {
        HANDLE h = CreateThread(NULL, 0, watch_set_all, (LPVOID)0, 0, NULL);
        if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
        state = 2;
        const uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
        for (int i = 0; i < s_watch_n; i++) {
            char path[64]; snprintf(path, sizeof path, "logs/watch_%d.txt", i);
            FILE* f = fopen(path, "w");
            if (!f) continue;
            fprintf(f, "# watch %08X: host writers over %u ticks from match tick %u\n", s_watch_ea[i], frames, at);
            for (int k = 0; k < 512 && s_watch_hits[i][k].rip; k++)
                fprintf(f, "%llX %ld\n", (unsigned long long)(s_watch_hits[i][k].rip - base), (long)s_watch_hits[i][k].n);
            fclose(f);
        }
        fprintf(stderr, "[watch] results in logs/watch_*.txt\n");
    }
}
#else
extern "C" void saka_watch_frame(unsigned sim_ticks) { (void)sim_ticks; }
#endif
