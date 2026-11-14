/*
 * Sakatsuku port: lv2 syscall hooks (see NOTES.md).
 *
 * Kept in a C file of its own: main.cpp includes the generated ppu_recomp.h,
 * which defines ppu_context itself, so it cannot also include the runtime's
 * syscall-table header.
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include "ps3recomp/runtime/syscalls/lv2_syscall_table.h"

int64_t sys_timer_usleep(ppu_context* ctx);   /* runtime/syscalls/sys_timer.c */
void*   cellGcm_fifo_kick_event(void);        /* libs/video/cellGcmSys.c */

/* The main thread spends most of a frame in 30 us sys_timer_usleep polls
 * (lr 0x006BF4A8: the command-buffer / label wait under 0x006C6A18) until the
 * RSX drain advances. Measured with SAKA_FPS=60 the title then capped at ~34 fps
 * with the main thread ~70% inside those polls. Kick the drain whenever a guest
 * thread sleeps for less than 1 ms, so the present thread (waiting in
 * cellGcm_fifo_kick_wait) walks the FIFO at once instead of on its next 1 ms
 * pass.
 *
 * OFF by default (SAKA_USLEEP_KICK=1 turns it on): measured no fps change. The
 * ~34 fps cap was later traced (PS3_FTRACE) to the RSX walker parked on a
 * self-JUMP that an interpreted SPU job patches 6-22 ms after the frame is
 * released, not to drain latency -- see NOTES.md. */
static int64_t saka_timer_usleep(ppu_context* ctx)
{
#ifdef _WIN32
    if (ctx->gpr[3] < 1000) {
        HANDLE ev = (HANDLE)cellGcm_fifo_kick_event();
        if (ev) SetEvent(ev);
    }
#endif
    return sys_timer_usleep(ctx);
}

/* ---- SPURS taskset tasks: run their lifted code ---------------------------
 * The EBOOT embeds four SPURS task ELFs (all linked at LS 0x3000, entry
 * 0x3050). The build lifts them (generated_spu/inventory.json), but they
 * only register by fingerprint for a workload START. Here the real (LLE)
 * libsre taskset policy DMAs a task's text from its ELF in the EBOOT into
 * LS 0x3000 and branches to it, and nothing marked the lifted image resident.
 * The whole task, including the one that writes part of each frame's RSX
 * command buffer, therefore ran in the SPU interpreter: 6-22 ms a frame
 * (PS3_FTRACE, NOTES.md).
 *
 * Register each task's text as a verified streamed region keyed by its source
 * EA. The runtime marks it resident on the GET, compares the LS bytes with
 * the lifted text at the first dispatch, and drops ownership when any other
 * load lands in the span. Before registering, the in-memory ELF's FNV-1a-64
 * fingerprint must equal the lifted image's, so a different EBOOT keeps the
 * interpreter.
 *
 * Default: only the RSX command-buffer task (bit 0, ELF 0x00976100). Measured
 * at the title and the first post-title dialog: same picture, 30.0 fps, the
 * RSX waits on it 4.4 ms a frame instead of 25.3 (0.8 vs 10.2 on the dialog);
 * SAKA_FPS=60 reaches 59.8 fps at the title.
 * The other three (bits 1-3) stay interpreted: with all four registered the
 * boot stalls after 5 file opens (normal: 752 by the title BGM) -- a lifted
 * task's syscall or exit into the still-interpreted taskset policy does not
 * hand control back the way the policy expects (the Yakuza port lifts the
 * policy too and registers its stack-reset PCs).
 * SAKA_SPU_TASK_REGIONS=0 turns this off; SAKA_SPU_TASK_MASK=<bits> picks tasks. */
typedef struct { uint32_t lsa, size; const uint8_t* bytes; } saka_spu_code_seg;  /* spu_overlay_code_segment */
int      spu_overlay_register_verified_region(uint32_t content_ea, uint32_t load_lsa, uint32_t span,
                                              int image_id, const saka_spu_code_seg* code, unsigned count);
uint64_t spu_workload_fingerprint(const void* data, size_t n);   /* runtime/spu/spu_workload.c */
extern uint8_t* vm_base;

void saka_register_spu_task_regions(void)
{
    static const struct {
        uint32_t elf_ea, elf_size; uint64_t fp; int image_id; uint32_t text_off, text_size;
    } k_tasks[] = {   /* from generated_spu/inventory.json + each ELF's text PT_LOAD */
        { 0x00976100u, 38116u, 0x5E48E6B0F07092BDull, 344610659, 0x100u, 0x9310u },  /* RSX command-buffer task */
        { 0x0097F700u,  6100u, 0xB0FC83C2FA76C187ull, 372656361, 0x100u, 0x1670u },
        { 0x00980F80u,  5844u, 0x637E61A7D8429918ull, 376995803, 0x100u, 0x1560u },
        { 0x00982680u,  1876u, 0xA2EE7A586AA6ABA6ull, 450957106, 0x100u, 0x05B0u },
    };
    const char* e = getenv("SAKA_SPU_TASK_REGIONS");
    if (e && *e == '0') { fprintf(stderr, "[saka] SPU task regions off (SAKA_SPU_TASK_REGIONS=0)\n"); return; }
    const char* m = getenv("SAKA_SPU_TASK_MASK");
    const unsigned mask = m ? (unsigned)strtoul(m, NULL, 0) : 0x1u;
    for (unsigned i = 0; i < sizeof k_tasks / sizeof k_tasks[0]; i++) {
        if (!(mask & (1u << i))) continue;
        const uint8_t* elf = vm_base + k_tasks[i].elf_ea;
        if (spu_workload_fingerprint(elf, k_tasks[i].elf_size) != k_tasks[i].fp) {
            fprintf(stderr, "[saka] SPU task ELF at 0x%08X: fingerprint differs from the lifted image %d -- left to the interpreter\n",
                    k_tasks[i].elf_ea, k_tasks[i].image_id);
            continue;
        }
        const saka_spu_code_seg seg = { 0x3000u, k_tasks[i].text_size, elf + k_tasks[i].text_off };
        const int ok = spu_overlay_register_verified_region(k_tasks[i].elf_ea + k_tasks[i].text_off, 0x3000u,
                                                           k_tasks[i].text_size, k_tasks[i].image_id, &seg, 1);
        fprintf(stderr, "[saka] SPU task ELF 0x%08X -> lifted image %d for LS 0x3000..0x%05X: %s\n",
                k_tasks[i].elf_ea, k_tasks[i].image_id, 0x3000u + k_tasks[i].text_size, ok ? "registered" : "FAILED");
    }
}

/* ---- Loader decompression on the PPU --------------------------------------
 * func_006DF39C (the async file thread's decoder) sends each compressed file
 * to the sl_decoder SPURS task when a taskset exists, and otherwise decodes it
 * itself on the PPU (loc_006DF49C). Here the SPU task runs interpreted (28-48
 * ms a file, 30 s of the benchmark route's loading); the PPU path is native.
 * recompiled/ppu_recomp_020.cpp asks this function. SAKA_PPU_DECODE=0 keeps
 * the SPU task. */
static volatile LONG s_ppu_decodes = 0;
int saka_ppu_decode_forced(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SAKA_PPU_DECODE"); on = !(e && *e == '0');
                  fprintf(stderr, "[saka] loader decompression: %s\n", on ? "PPU path (native)" : "sl_decoder SPU task"); }
    if (on) InterlockedIncrement(&s_ppu_decodes);
    return on;
}

/* ---- 60-tick mode (SAKA_LOGIC60=1, default off) ----------------------------
 * The game steps its logic once per frame and runs at 30 frames a second:
 * init calls SetInterval(2) (0x001944F8 li r3,2 -> 0x0069BEE0, word
 * 0x00EA40C0) and the frame loop (0x0069E4FC) waits for two vblanks.
 * SAKA_LOGIC60 makes that interval 1 and keeps the frame loop's vblank wait
 * at interval 1 (it skips the wait entirely there, relying on hardware flip
 * pacing), so the game ticks 60 times a second off the 60 Hz vblank -- vblank
 * callbacks keep their rate. Per-frame steps still assume 1/30 s: until each
 * is halved, everything frame-stepped runs at double speed. Recompiled call
 * sites: ppu_recomp_004/024 (interval), ppu_recomp_019 (frame wait);
 * patches/recompiled-0002-logic60.patch. */
int saka_logic60(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SAKA_LOGIC60"); on = (e && *e == '1');
                  fprintf(stderr, "[saka] logic rate: %s\n", on ? "60 ticks/s (SAKA_LOGIC60)" : "30 ticks/s"); }
    return on;
}

/* Under SAKA_LOGIC60 the match simulation still runs at 30 Hz (owner: "30Hz
 * 유지(결과 동일)" -- identical match results). The engine's task manager
 * (func_00634CF8, ppu_recomp_018/029) counts its ticks here; the match tick
 * func_001F65AC (ppu_recomp_005), called by the match scene task
 * func_0065DF60, skips every odd one and reports "still running". Engine-timed
 * systems (dt, animation playback) keep running every tick at dt 1/60.
 * SAKA_LOGIC60_MATCH30=0 steps the match every tick (2x speed, for A/B).
 * patches/recompiled-0004-logic60-match30.patch. */
static volatile unsigned s_logic60_tick;
static unsigned saka_sim_ticks_now(void);
void saka_pos_scan_tick(unsigned frame, unsigned sim_ticks);   /* main.cpp, SAKA_POS_SCAN (diagnostic) */
static void saka_pose_frame(void);
void saka_mem_dump_frame(unsigned sim_ticks, int no_tick_frame);   /* main.cpp, SAKA_MEM_DUMP (diagnostic) */
void saka_watch_frame(unsigned sim_ticks);   /* main.cpp, SAKA_WATCH (diagnostic) */
void saka_logic60_tick(void) { s_logic60_tick++; saka_mem_dump_frame(saka_sim_ticks_now(), (int)(s_logic60_tick & 1u)); saka_watch_frame(saka_sim_ticks_now()); saka_pose_frame(); saka_pos_scan_tick(s_logic60_tick, saka_sim_ticks_now()); }
int saka_logic60_match_skip(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SAKA_LOGIC60_MATCH30");
                  on = saka_logic60() && !(e && *e == '0');
                  if (saka_logic60())
                      fprintf(stderr, "[saka] match simulation: %s\n",
                              on ? "30 Hz, every other tick (SAKA_LOGIC60_MATCH30=0 = every tick)" : "every tick"); }
    return on && (s_logic60_tick & 1u);
}

/* ---- 60-tick animation time (on at 60 ticks; SAKA_ANIM60=0 = off) -----------
 * The player models' per-tick update (Ghidra _opd_FUN_0017e1d8, a virtual
 * method) reads its match player's animation time through FUN_001f2d94
 * (player+0x74), advances its skeleton by the difference to the time it saw
 * last (FUN_00601f44) and evaluates it (FUN_0060206c -> FUN_006035a8). The
 * skeleton is evaluated every tick, but the match tick advances that time at
 * 30 Hz, so every other tick the skeleton advances by 0 and repeats the pose
 * -- bodies, limbs and the root motion that moves them step at 30 Hz.
 * On a tick without the match tick this getter returns the time half a step
 * ahead, c + (c - b) / 2, with a, b, c the last three values it read for that
 * player, when the time went steadily forward (b - a and c - b positive and
 * within half of each other); the model then advances by half a step each
 * tick. Its other caller (FUN_0025f1b0, a match-state skeleton) only sets a
 * display time. A model with an animation controller of its own (this+0x94,
 * the goalkeepers) uses its own time instead (this+0x84, read in the lifted
 * func_0017E1D8); moving that one ahead changed the match (match state at
 * tick 1200 differed from a run without it, while players and ball alone left
 * it identical), so it is only done with SAKA_ANIM60_GK=1.
 * The match tick never runs on those ticks and reads the true time; nothing
 * is written to guest memory. */
/* History sampled once per match tick: the last value a key was read with
 * during a tick that ran the match tick becomes its newest sample when it is
 * next read on another tick. A value that stops changing then shows a zero
 * step instead of its last movement. */
typedef struct { uint32_t key, ptick; float a[3], b[3], c[3], pend[3]; uint8_t n, has_pend; } tick_hist;
static tick_hist s_anim_hist[256];
static unsigned long long s_anim_ahead, s_anim_kept, s_ball_ahead, s_ball_kept;
static unsigned long long s_ball_glide_n; static double s_ball_lag_sum, s_ball_lag_max;   /* drawn behind the game's ball */
static void th_commit(tick_hist* h, int nc)
{
    memcpy(h->a, h->b, (size_t)nc * 4u); memcpy(h->b, h->c, (size_t)nc * 4u); memcpy(h->c, h->pend, (size_t)nc * 4u);
    if (h->n < 3) h->n++;
    h->has_pend = 0;
}
/* 1 on a tick without the match tick once three samples exist and v is still
 * the latest sample (moved since: not the match tick's alone). */
static int th_read(tick_hist* h, const float* v, int nc)
{
    const uint32_t T = s_logic60_tick;
    if (!(T & 1u)) {
        if (h->has_pend && h->ptick != T) th_commit(h, nc);
        memcpy(h->pend, v, (size_t)nc * 4u); h->has_pend = 1; h->ptick = T;
        return 0;
    }
    if (h->has_pend) th_commit(h, nc);
    if (h->n < 3) return 0;
    for (int k = 0; k < nc; k++) if (v[k] != h->c[k]) return 0;
    return 1;
}
static tick_hist* th_find(tick_hist* t, unsigned size, uint32_t key)
{
    for (unsigned k = 0, i = (key >> 4) & (size - 1u); k < size; k++, i = (i + 1u) & (size - 1u)) {
        if (t[i].key == key) return &t[i];
        if (!t[i].key) { memset(&t[i], 0, sizeof t[i]); t[i].key = key; return &t[i]; }
    }
    return NULL;
}
static int saka_anim60_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("SAKA_ANIM60");
        const char* m = getenv("SAKA_LOGIC60_MATCH30");
        on = saka_logic60() && !(m && *m == '0') && !(e && *e == '0');
        if (saka_logic60())
            fprintf(stderr, "[anim60] player animation time between match ticks: %s\n",
                    on ? "half a step ahead (SAKA_ANIM60=0 = as the match tick left it)" : "as the match tick left it (SAKA_ANIM60=0)");
    }
    return on;
}
void saka_anim60_adjust(uint32_t key, double* v)
{
    if (!saka_anim60_on()) return;
    { static int gk = -1;   /* keys with bit 0: a model's own time (this+0x84, goalkeepers): SAKA_ANIM60_GK=1 only, it changes the match */
      if (gk < 0) { const char* e = getenv("SAKA_ANIM60_GK"); gk = e && *e == '1'; }
      if ((key & 1u) && !gk) return; }
    const float t = (float)*v;
    tick_hist* h = th_find(s_anim_hist, 256u, key);
    if (!h || !th_read(h, &t, 1)) { if (h && (s_logic60_tick & 1u)) s_anim_kept++; return; }
    const float d1 = h->b[0] - h->a[0], d2 = h->c[0] - h->b[0];
    if (!(d1 > 0.0f) || !(d2 > 0.0f) || fabsf(d2 - d1) > 0.5f * fmaxf(d1, d2)) { s_anim_kept++; return; }
    *v = (double)(h->c[0] + 0.5f * d2);
    s_anim_ahead++;
}
void saka_anim_time_hook(ppu_context* ctx, uint32_t player) { saka_anim60_adjust(player, &ctx->fpr[1]); }

static void saka_anim60_stats(void)
{
    static time_t t0;
    const time_t now = time(NULL);
    if (!t0) t0 = now;
    if (now - t0 < 5) return;
    t0 = now;
    fprintf(stderr, "[anim60] last 5 s: animation times half a step ahead %llu, left %llu; ball %llu ahead, %llu left (not steady); "
            "ball glide %llu calls, behind the game's ball mean %.2f max %.2f\n",
            s_anim_ahead, s_anim_kept, s_ball_ahead, s_ball_kept, s_ball_glide_n,
            s_ball_glide_n ? s_ball_lag_sum / (double)s_ball_glide_n : 0.0, s_ball_lag_max);
    s_anim_ahead = s_anim_kept = s_ball_ahead = s_ball_kept = s_ball_glide_n = 0;
    s_ball_lag_sum = s_ball_lag_max = 0.0;
}

/* The 30 Hz match tick sees the engine timer as the original game did: the
 * task manager set it up for a 60 Hz tick (interval 1, dt 1/60, speed factors
 * x0.5); for the duration of the match tick it shows interval 2, the factors
 * at x1.0 (this[0x160+4i] = this[0x178+4i], squared at 0x16C) and the
 * measured times doubled, then everything is put back. Timer object at
 * [0x00E93E70], layout from func_00634CF8. Guest words are big-endian. */
static uint32_t ld_be32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void st_be32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void st_be16(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static float ld_bef(uint32_t ea) { const uint32_t w = ld_be32(ea); float f; memcpy(&f, &w, 4); return f; }
static void st_bef(uint32_t ea, float f) { uint32_t w; memcpy(&w, &f, 4); st_be32(ea, w); }

/* ---- 60-tick ball position (with SAKA_ANIM60) --------------------------------
 * The ball model's per-tick update (Ghidra _opd_FUN_0017cb30) builds the position
 * it draws the ball at in vr29, and rolls the ball by the distance moved since
 * the last tick. Two branches meet at 0x0017CD88: with the ball in play
 * (FUN_001f0cdc(ball) == 1) a copy of ball+0x170 (ball = match object+0x1A30,
 * through FUN_001f0ce8); in every other state, most of the match, the position
 * of the object FUN_0020a1ec returns (+4..+0xC). Only that vector is changed, at
 * the merge point (saka_ball_vec_hook; SAKA_BALL60=0 = off): the player models'
 * copy of the ball position turns them toward the ball, and extrapolating it
 * changed the match (state at tick 1200 differed). The match tick moves the ball
 * at 30 Hz, so every other tick the ball stood still and did not roll. On a tick
 * without the match tick the vector gets the position half a step ahead,
 * c + (c - b)/2 per axis, when the ball moved steadily over the last two match
 * ticks (|d2 - d1| within half the larger step plus 5 cm, steps up to 20 units)
 * and has not moved since; kicks, bounces, stops and resets are left as they
 * are. Both hooks sample their history once per match tick (th_read). */
static volatile unsigned s_sim_ticks;   /* match ticks run so far (defined with the match-tick hooks below) */
static tick_hist s_ball_hist[8];
static int ball60_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SAKA_BALL60"); on = !(e && *e == '0'); }
    return on && saka_anim60_on();
}
/* The FUN_001f0ce8 copy hook (ppu_recomp_025.cpp) only saw the in-play branch,
 * which is rare; saka_ball_vec_hook covers both, so this entry does nothing. */
void saka_ball_pos_hook(uint32_t dst, uint32_t obj, uint32_t lr) { (void)dst; (void)obj; (void)lr; }
static uint32_t ball_bs32(uint32_t x) { return (x >> 24) | ((x >> 8) & 0xFF00u) | ((x << 8) & 0xFF0000u) | (x << 24); }
static int ball_ahead(uint32_t obj, float v[3]);
/* Called at 0x0017CD88 in func_0017CB30 (ppu_recomp_004.cpp): vr29 holds the
 * drawn position in guest byte order, gpr31 is the ball object. */
/* SAKA_BALL_DUMP_GATE=<file> (diagnostic, 2026-11-08, owner: "센터링이나 공이 높이 띄워 졌을때 30fps"): when the
 * drawn ball rises above SAKA_BALL_DUMP_Y (default 2.5) the file is created for 120 ticks, so LD_FRAME_DUMP_GATE
 * pointed at it dumps those frames (at most 6 windows a run); every tick of a window is logged as [ball-hi]. */
volatile int g_saka_diag_slowmo;   /* main.cpp: vblank period x this while > 1 (SAKA_BALL_DUMP_SLOW) */
static void ball_high_diag(const float v[3], int moved, const float out[3])
{
    static int on = -1, windows, slow; static const char* gate; static float ymin; static unsigned until;
    if (on < 0) {
        gate = getenv("SAKA_BALL_DUMP_GATE"); on = gate && *gate;
        const char* y = getenv("SAKA_BALL_DUMP_Y"); ymin = y ? (float)atof(y) : 2.5f;
        const char* s = getenv("SAKA_BALL_DUMP_SLOW"); slow = s ? atoi(s) : 0;   /* game speed / n in a window */
    }
    if (!on) return;
    static unsigned calls;   /* the window counts calls: one per tick at 60 ticks, one per frame at 30 */
    calls++;
    if (!until && windows < 6 && v[1] > ymin) {
        FILE* f = fopen(gate, "wb"); if (f) fclose(f);
        until = calls + 120; windows++;
        g_saka_diag_slowmo = slow;
        fprintf(stderr, "[ball-hi] window %d opens at tick %u\n", windows, s_logic60_tick);
    }
    if (!until) return;
    fprintf(stderr, "[ball-hi] tick %u par %u pos %.2f %.2f %.2f%s drawn %.2f %.2f %.2f\n", calls,
            s_logic60_tick & 1u, v[0], v[1], v[2], (moved & 2) ? " glide" : (moved & 1) ? " ahead" : " kept ", out[0], out[1], out[2]);
    if (calls >= until) { remove(gate); until = 0; g_saka_diag_slowmo = 0; }
}
/* Ball hops (2026-11-08, owner: "예, 튀다"; SAKA_BALL_SMOOTH=0 = off). Outside free flight the drawn position is
 * the match object's ball entity ([obj+0x3A10+4] +4..+0xC, see above), which the match only moves now and then:
 * at 30 ticks as at 60 it stands for 2-9 match ticks and then hops 2-8 units (dribble touches, the ball after a
 * landing; 23 of 51 such hops in a 726-frame log stood again right after, the rest began a kick). The same
 * positions come at 30 and 60 ticks, so it is the game's own data. A hop lands exactly on the ground (y 0: 22
 * of those 23), a kick leaves it (y 0.1-2.8 at its first step: all 25). Drawing only: after the ball stood for
 * at least two match ticks, a hop of 1-15 units to y 0 is drawn from where the ball was drawn, the distance
 * left shrinking to a tenth over as many calls as the ball stood (two to six match ticks), so the ball slows
 * down like a pushed ball. When the ball moves on during a glide it shrinks faster (x0.7 a call). Kicks and
 * longer jumps (restarts) are drawn as the game draws them. One call per tick at 60 ticks (m = 2 calls per
 * match tick), one per frame at 30 (m = 1).
 * 2026-11-09 (owner: "공이 선수를 늦게 따라가고 선수들의 움직임 틱에 제대로 붙는 느낌이 아니야"): that glide shrank the
 * distance exponentially over 4-12 calls, so after every dribble touch the drawn ball trailed the game's ball, and the
 * player, for up to a fifth of a second. Now a hop is drawn over one match tick: half of it on the call it happens,
 * the rest on the next call, so from the next frame on the ball is where the game puts it, in step with the players.
 * At 30 ticks (one call per match tick) nothing is glided. SAKA_BALL_SMOOTH=long = the 2026-11-08 glide (for
 * comparison), 0 = off. The [anim60] 5-second line counts the glided calls and how far behind they drew the ball. */
typedef struct { uint32_t key; float raw[3], drawn[3], off[3], f; unsigned still, left; int active; } ball_glide_st;
static int ball_glide(uint32_t obj, const float raw[3], float v[3])
{
    static int on = -1;   /* 0 off, 1 one match tick (default), 2 the long glide */
    static ball_glide_st g;
    if (on < 0) {
        const char* e = getenv("SAKA_BALL_SMOOTH");
        on = (e && *e == '0') ? 0 : (e && strcmp(e, "long") == 0) ? 2 : 1;
        fprintf(stderr, "[ball] hops of the drawn ball: %s\n",
                on == 1 ? "over one match tick (SAKA_BALL_SMOOTH=long = the long glide, 0 = as the game draws them)"
                        : on == 2 ? "long glide (SAKA_BALL_SMOOTH=long)" : "as the game draws them");
    }
    if (!on) return 0;
    const unsigned m = ball60_on() ? 2u : 1u;
    if (on == 1 && m < 2u) return 0;
    if (g.key != obj) { memset(&g, 0, sizeof g); g.key = obj; memcpy(g.raw, raw, 12); memcpy(g.drawn, v, 12); return 0; }
    const float dx = raw[0] - g.raw[0], dy = raw[1] - g.raw[1], dz = raw[2] - g.raw[2];
    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d == 0.0f) { if (g.still < 100000u) g.still++; }
    else {
        if (d > 15.0f) g.active = 0;                                             /* restart or reset: at once */
        else if (d > 1.0f && raw[1] == 0.0f && g.still >= 2u * m) {             /* a hop after two match ticks */
            if (on == 2) {
                unsigned n = g.still + 1u;
                if (n < 2u * m) n = 2u * m;
                if (n > 6u * m) n = 6u * m;
                g.f = powf(0.1f, 1.0f / (float)n);
            } else {
                g.f = 0.5f; g.left = 1u;                                         /* half now, all of it next call */
            }
            for (int k = 0; k < 3; k++) g.off[k] = g.drawn[k] - v[k];
            g.active = 1;
        } else if (g.active && on == 2 && g.f > 0.7f) g.f = 0.7f;              /* moving on: catch up sooner */
        g.still = 0; memcpy(g.raw, raw, 12);
    }
    if (g.active) {
        if (on == 1) {
            if (g.left) { g.left--; for (int k = 0; k < 3; k++) g.off[k] *= g.f; }
            else g.active = 0;
        } else {
            for (int k = 0; k < 3; k++) g.off[k] *= g.f;
            if (g.off[0] * g.off[0] + g.off[1] * g.off[1] + g.off[2] * g.off[2] < 1e-4f) g.active = 0;
        }
    }
    if (!g.active) { memcpy(g.drawn, v, 12); return 0; }
    const float floor_y = fminf(v[1], 0.0f);
    for (int k = 0; k < 3; k++) v[k] += g.off[k];
    if (v[1] < floor_y) v[1] = floor_y;
    memcpy(g.drawn, v, 12);
    return 2;
}
void saka_ball_vec_hook(ppu_context* ctx)
{
    uint8_t* r = (uint8_t*)&ctx->vr[29];
    float v[3];
    for (int k = 0; k < 3; k++) { uint32_t w; memcpy(&w, r + 4 * k, 4); w = ball_bs32(w); memcpy(&v[k], &w, 4); }
    const float v0[3] = { v[0], v[1], v[2] };
    int moved = ball60_on() ? ball_ahead((uint32_t)ctx->gpr[31], v) : 0;   /* the diagnostic also at 30 ticks */
    moved |= ball_glide((uint32_t)ctx->gpr[31], v0, v);
    if (moved & 2) {   /* how far behind the game's ball the glide drew it */
        const float dx = v[0] - v0[0], dy = v[1] - v0[1], dz = v[2] - v0[2];
        const double lag = sqrt((double)(dx * dx + dy * dy + dz * dz));
        s_ball_glide_n++; s_ball_lag_sum += lag; if (lag > s_ball_lag_max) s_ball_lag_max = lag;
    }
    ball_high_diag(v0, moved, v);
    if (!moved) return;
    for (int k = 0; k < 3; k++) { uint32_t w; memcpy(&w, &v[k], 4); w = ball_bs32(w); memcpy(r + 4 * k, &w, 4); }
}
static int ball_ahead(uint32_t obj, float v[3])
{
    tick_hist* h = th_find(s_ball_hist, 8u, obj);
    if (!h || !th_read(h, v, 3)) { if (h && (s_logic60_tick & 1u)) s_ball_kept++; return 0; }
    float d2[3], dd = 0.0f, l1 = 0.0f, l2 = 0.0f;
    for (int k = 0; k < 3; k++) {
        const float d1 = h->b[k] - h->a[k];
        d2[k] = h->c[k] - h->b[k];
        dd += (d2[k] - d1) * (d2[k] - d1); l1 += d1 * d1; l2 += d2[k] * d2[k];
    }
    dd = sqrtf(dd); l1 = sqrtf(l1); l2 = sqrtf(l2);
    const int ahead = l2 > 1e-4f && l2 <= 20.0f && dd <= 0.5f * fmaxf(l1, l2) + 0.05f;
    { static int dbg = -1; static unsigned last, shown;   /* SAKA_BALL_DEBUG=1: one line per tick for 80 ticks */
      if (dbg < 0) { const char* e = getenv("SAKA_BALL_DEBUG"); dbg = e && *e == '1'; }
      if (dbg && s_sim_ticks >= 1200u && shown < 80u && last != s_logic60_tick) {
          last = s_logic60_tick; shown++;
          fprintf(stderr, "[ball-dbg] tick %u step %.3f prev step %.3f change %.3f%s\n", s_logic60_tick, l2, l1, dd,
                  ahead ? " -> ahead" : !(l2 > 1e-4f) ? " (still)" : " (not steady)");
      } }
    if (!ahead) { s_ball_kept++; return 0; }
    for (int k = 0; k < 3; k++) v[k] = h->c[k] + 0.5f * d2[k];
    /* Never below the pitch: a ball coming down lands (y 0) at the next match tick, and half a step of its fall
     * drew it up to 1.5 below the ground for a frame (2026-11-08, [ball-hi] tick 6353: y -1.48). */
    { const float floor_y = fminf(h->c[1], 0.0f); if (v[1] < floor_y) v[1] = floor_y; }
    s_ball_ahead++;
    return 1;
}
static uint32_t s_sim_timer, s_sim_save[9];
static volatile unsigned s_sim_ticks;   /* match ticks run so far (SAKA_SIMTICK_SHOT) */

/* ---- Pose extrapolation at 60 ticks (SAKA_POSE_INTERP=1 = on) --------------
 * At 60 ticks the match tick runs every other tick. The players' skeletons are
 * evaluated in those ticks only (func_006035A8, Ghidra FUN_006035a8: animation
 * layers sampled at their times into 4x4 world matrices, rows w 0,0,0 and a
 * translation row ending in 1.0); the skinning palettes built from them every
 * tick then repeat the same pose, so bodies and limbs step at 30 Hz while the
 * camera moves every tick (hardware watchpoints: those matrices are written
 * once per match tick, by func_006035A8 alone).
 * At the start of a tick without the match tick every such matrix is set half
 * a step ahead -- cur + (cur - prev) / 2 per float -- so the palettes built in
 * that tick show the in-between pose; at the start of the next tick, before
 * the match tick runs, it is put back, so the simulation never sees it.
 *  - Matrices: 16-byte aligned 4x4 world matrices (row lengths 0.5-2.0,
 *    |cos| <= 0.05, translation within 500) in the pages written during a
 *    match-tick frame -- the write watch shared with the renderer, listed at
 *    the start of the next tick, before anything in it runs; measured again
 *    every 128 match ticks -- outside the main stack.
 *  - A matrix written by anyone else during a tick without the match tick is
 *    not the skeleton's: it is put back no more and left alone until the next
 *    measurement. A matrix that jumped (translation by more than 2, a rotation
 *    element by more than 1: cuts, teleports, snapped blends) is left as is.
 * [pose] prints what it tracked and moved every 5 s. */
int saka_watch_auto(void);                 /* main.cpp, SAKA_WATCH_AUTO (diagnostic) */
void saka_watch_request(uint32_t ea);
uint32_t rsx_ww_harvest_low(void);
uint32_t rsx_ww_written_pages_between(uint32_t lo, uint32_t hi, uint32_t* out, uint32_t max);
typedef struct {
    uint32_t ea, wframe;
    uint8_t have_cur, have_prev, written, bad, moved, jumped;
    uint8_t prev[64], cur[64], ext[64];
} pose_mat;
static pose_mat* s_pm; static uint32_t s_pm_n, s_pm_cap;
static uint32_t* s_pose_pages;
static uint32_t s_rg0, s_pose_last_sim, s_pose_last_rescan; static int s_rs;   /* rescan window state */
static struct { unsigned long long ticks, moved, jumped, put, back, foreign, rescans, pages; double rescan_ms; } s_ps;

static int pose_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("SAKA_POSE_INTERP");
        on = saka_logic60() && (e && *e == '1');
        if (saka_logic60())
            fprintf(stderr, "[pose] 60-tick pose extrapolation: %s\n", on ? "on (SAKA_POSE_INTERP=1)" : "off (SAKA_POSE_INTERP=1 = on)");
    }
    return on;
}
static float pose_bef(const uint8_t* p)
{
    const uint32_t w = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    float f; memcpy(&f, &w, 4); return f;
}
static void pose_put(uint8_t* p, float f)
{
    uint32_t w; memcpy(&w, &f, 4);
    p[0] = (uint8_t)(w >> 24); p[1] = (uint8_t)(w >> 16); p[2] = (uint8_t)(w >> 8); p[3] = (uint8_t)w;
}
/* A 4x4 world matrix at q: three rotation rows (w exactly 0) and a
 * translation row ending in exactly 1.0. */
static int pose_is_world(const uint8_t* q)
{
    float r[16];
    for (int i = 0; i < 16; i++) { r[i] = pose_bef(q + 4 * i); if (!isfinite(r[i])) return 0; }
    if (r[3] != 0.0f || r[7] != 0.0f || r[11] != 0.0f || r[15] != 1.0f) return 0;
    float len[3];
    for (int i = 0; i < 3; i++) {
        len[i] = sqrtf(r[4 * i] * r[4 * i] + r[4 * i + 1] * r[4 * i + 1] + r[4 * i + 2] * r[4 * i + 2]);
        if (len[i] < 0.5f || len[i] > 2.0f) return 0;
    }
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++)
            if (fabsf(r[4 * i] * r[4 * j] + r[4 * i + 1] * r[4 * j + 1] + r[4 * i + 2] * r[4 * j + 2]) > 0.05f * len[i] * len[j]) return 0;
    for (int i = 12; i < 15; i++) if (fabsf(r[i]) > 500.0f) return 0;
    return 1;
}
static void pose_add(pose_mat** list, uint32_t* n, uint32_t* cap, uint32_t ea)
{
    if (*n == *cap) {
        const uint32_t nc = *cap ? *cap * 2u : 2048u;
        pose_mat* p = (pose_mat*)realloc(*list, (size_t)nc * sizeof **list);
        if (!p) return;
        *list = p; *cap = nc;
    }
    pose_mat* m = &(*list)[(*n)++];
    memset(m, 0, sizeof *m);
    m->ea = ea;
}
/* Start of the tick after a measured match-tick frame: the world matrices in
 * the pages written during it (generation in (lo, hi]), keeping what was
 * known about matrices already tracked. */
static void pose_rescan_scan(uint32_t lo, uint32_t hi)
{
    const clock_t c0 = clock();
    enum { MAXP = 1u << 17 };
    if (!s_pose_pages) s_pose_pages = (uint32_t*)malloc(MAXP * sizeof(uint32_t));
    if (!s_pose_pages) return;
    const uint32_t np = rsx_ww_written_pages_between(lo, hi, s_pose_pages, MAXP);
    pose_mat* nl = NULL; uint32_t nn = 0, ncap = 0;
    for (uint32_t i = 0; i < np; i++) {
        const uint32_t pg = s_pose_pages[i];
        if (pg >= 0x0FD00000u && pg < 0x10000000u) continue;          /* main-thread stack */
        const int next = i + 1 < np && s_pose_pages[i + 1] == pg + 4096u;
        for (uint32_t off = 0; off < 4096u; ) {
            if (off + 64u > 4096u && !next) break;
            if (pose_is_world(vm_base + pg + off)) { pose_add(&nl, &nn, &ncap, pg + off); off += 64u; }
            else off += 16u;
        }
    }
    uint32_t j = 0;
    for (uint32_t i = 0; i < nn; i++) {
        while (j < s_pm_n && s_pm[j].ea < nl[i].ea) j++;
        if (j < s_pm_n && s_pm[j].ea == nl[i].ea) { nl[i] = s_pm[j]; nl[i].bad = 0; }
    }
    free(s_pm);
    s_pm = nl; s_pm_n = nn; s_pm_cap = ncap;
    s_ps.rescans++; s_ps.pages += np;
    s_ps.rescan_ms += (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC;
}
/* Start of a tick without the match tick: the pose the last match tick made
 * becomes `cur`, the one before `prev`; each matrix that moved is set half a
 * step ahead for this tick. */
static void pose_capture_apply(void)
{
    for (uint32_t i = 0; i < s_pm_n; i++) {
        pose_mat* m = &s_pm[i];
        m->moved = m->jumped = 0;
        if (m->bad) continue;
        uint8_t* q = vm_base + m->ea;
        if (m->have_cur) { memcpy(m->prev, m->cur, 64); m->have_prev = 1; }
        memcpy(m->cur, q, 64); m->have_cur = 1;
        if (!m->have_prev || !memcmp(m->prev, m->cur, 64) || !pose_is_world(m->cur) || !pose_is_world(m->prev)) continue;
        int jump = 0;
        for (int k = 0; k < 64 && !jump; k += 4) {
            const float a = pose_bef(m->prev + k), b = pose_bef(m->cur + k);
            const float d = b - a;
            if (fabsf(d) > (k >= 48 ? 2.0f : 1.0f)) jump = 1;
            else pose_put(m->ext + k, b + 0.5f * d);
        }
        if (jump) { s_ps.jumped++; m->jumped = 1; continue; }
        m->moved = 1;
        memcpy(q, m->ext, 64);
        m->written = 1; m->wframe = s_logic60_tick;
        s_ps.moved++;
        s_ps.put++;
    }
}
/* Start of a tick with the match tick: the skeleton's own pose goes back
 * before the match tick runs. Overwritten meanwhile: not the skeleton's. */
static void pose_restore(void)
{
    for (uint32_t i = 0; i < s_pm_n; i++) {
        pose_mat* m = &s_pm[i];
        if (!m->written) continue;
        m->written = 0;
        if (s_logic60_tick - m->wframe > 4u) continue;
        uint8_t* q = vm_base + m->ea;
        if (memcmp(q, m->ext, 64)) {
            m->bad = 1; s_ps.foreign++;
            if (saka_watch_auto()) {   /* SAKA_WATCH_AUTO=1: who overwrites it (one per 64 KB block) */
                static uint32_t blocks[4]; static int nb;
                int seen = 0; for (int b = 0; b < nb; b++) seen |= blocks[b] == (m->ea >> 16);
                if (!seen && nb < 4) { blocks[nb++] = m->ea >> 16; saka_watch_request(m->ea + 48u); }
            }
            continue;
        }
        memcpy(q, m->cur, 64);
        s_ps.back++;
    }
}
static void pose_stats(void)
{
    static time_t t0;
    const time_t now = time(NULL);
    if (!t0) t0 = now;
    if (now - t0 < 5) return;
    t0 = now;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < s_pm_n; i++) bad += s_pm[i].bad;
    fprintf(stderr, "[pose] last 5 s: %u matrices tracked (%u not the skeleton's), match ticks %llu, moved %.0f/tick, "
                    "jumped %llu, put back %llu, overwritten %llu | rescans %llu (%llu pages, %.1f ms each)\n",
            s_pm_n, bad, s_ps.ticks, s_ps.ticks ? (double)s_ps.moved / (double)s_ps.ticks : 0.0, s_ps.jumped,
            s_ps.back, s_ps.foreign, s_ps.rescans, s_ps.pages, s_ps.rescans ? s_ps.rescan_ms / (double)s_ps.rescans : 0.0);
    { static int dbg = -1; static int shown = 0;
      if (dbg < 0) { const char* e = getenv("SAKA_POSE_DEBUG"); dbg = e && *e == '1'; }
      if (dbg && shown < 3 && s_pm_n) {
          shown++;
          uint32_t blk = 0xFFFFFFFFu, nt = 0, nb = 0, nm = 0, nj = 0;
          for (uint32_t i = 0; i <= s_pm_n; i++) {
              const uint32_t b = i < s_pm_n ? s_pm[i].ea >> 16 : 0xFFFFFFFEu;
              if (b != blk) {
                  if (blk != 0xFFFFFFFFu && nt) fprintf(stderr, "[pose-dbg] %04X0000: %u matrices, %u overwritten, %u moved, %u jumped (last tick)\n", blk, nt, nb, nm, nj);
                  blk = b; nt = nb = nm = nj = 0;
              }
              if (i < s_pm_n) { nt++; nb += s_pm[i].bad; nm += s_pm[i].moved; nj += s_pm[i].jumped; }
          }
      } }
    memset(&s_ps, 0, sizeof s_ps);
}
/* SAKA_POSE_SHOT=<match tick> with LD_FRAME_DUMP_GATE (+ LD_FRAME_DUMP=<dir>,
 * LD_FRAME_DUMP_EVERY=1): dump the 10 frames from that match tick on
 * (diagnostic, to see the in-between frames). */
static unsigned s_pose_shot_open;
static void saka_pose_shot_tick(void)
{
    static int parsed = 0; static unsigned at = 0; static const char* gate;
    if (!parsed) { parsed = 1; gate = getenv("LD_FRAME_DUMP_GATE"); if (!gate) gate = getenv("LD_SURF_DUMP_GATE");
                   const char* e = getenv("SAKA_POSE_SHOT"); at = e ? (unsigned)strtoul(e, NULL, 10) : 0; }
    if (gate && at && s_sim_ticks == at) {
        FILE* f = fopen(gate, "w"); if (f) { fputs("1\n", f); fclose(f); }
        s_pose_shot_open = s_logic60_tick ? s_logic60_tick : 1u;
        fprintf(stderr, "[pose] dump gate opened at match tick %u\n", s_sim_ticks);
    }
}
/* Every tick's start (saka_logic60_tick). Even ticks run the match tick, odd
 * ones skip it (saka_logic60_match_skip). While a match runs: odd ticks get
 * the half step ahead, even ticks put the skeletons' pose back. A measurement
 * harvests the write watch at the start of a match-tick frame (g0) and at the
 * start of the next tick, then lists the pages written between before
 * anything in that tick runs. */
static void saka_pose_frame(void)
{
    if (s_pose_shot_open && s_logic60_tick - s_pose_shot_open >= 10u) {
        const char* gate = getenv("LD_FRAME_DUMP_GATE");
        if (!gate) gate = getenv("LD_SURF_DUMP_GATE");
        if (gate) remove(gate);
        s_pose_shot_open = 0;
        fprintf(stderr, "[pose] dump gate closed\n");
    }
    if (!pose_on()) return;
    const int in_match = s_pose_last_sim && s_logic60_tick - s_pose_last_sim <= 3u;
    if (!in_match) { s_rs = 0; return; }
    if (!(s_logic60_tick & 1u)) {                        /* this tick runs the match tick */
        pose_restore();
        if (s_rs == 0 && s_sim_ticks - s_pose_last_rescan >= (s_pm_n ? 128u : 16u)) {
            s_rg0 = rsx_ww_harvest_low();
            s_rs = s_rg0 ? 1 : 0;
        }
    } else {                                             /* no match tick this tick */
        if (s_rs == 1) {
            const uint32_t g1 = rsx_ww_harvest_low();
            if (g1) pose_rescan_scan(s_rg0, g1);
            s_rs = 0;
            s_pose_last_rescan = s_sim_ticks;
        }
        pose_capture_apply();
    }
}

void saka_logic60_sim_begin(void)
{
    s_sim_timer = 0;
    if (!saka_logic60()) return;
    const uint32_t t = ld_be32(0x00E93E70u);
    if (!t || ld_be32(t + 0xC0) != 1u) return;
    s_sim_timer = t;
    s_sim_save[0] = ld_be32(t + 0xC0);
    s_sim_save[1] = ld_be32(t + 0x150);
    s_sim_save[2] = ld_be32(t + 0x154);
    for (int i = 0; i < 3; i++) { s_sim_save[3 + i] = ld_be32(t + 0x160 + 4 * i); s_sim_save[6 + i] = ld_be32(t + 0x16C + 4 * i); }
    st_be32(t + 0xC0, 2u);
    st_bef(t + 0x150, ld_bef(t + 0x150) * 2.0f);
    st_bef(t + 0x154, ld_bef(t + 0x154) * 2.0f);
    for (int i = 0; i < 3; i++) {
        const float b = ld_bef(t + 0x178 + 4 * i);   /* x factor[2] = 1.0 */
        st_bef(t + 0x160 + 4 * i, b);
        st_bef(t + 0x16C + 4 * i, b * b);
    }
}
static unsigned saka_sim_ticks_now(void) { return s_sim_ticks; }
void saka_logic60_sim_end(void)
{
    s_sim_ticks++;
    saka_pose_shot_tick();
    if (saka_anim60_on()) saka_anim60_stats();
    if (pose_on()) { s_pose_last_sim = s_logic60_tick; s_ps.ticks++; pose_stats(); }
    /* SAKA_SIMTICK_SHOT=<n>[,<n>...] with LD_SURF_DUMP_GATE=<file>: open the
     * surface-dump gate at those match ticks (3 dumps, one per other tick), so
     * a 30-tick and a 60-tick run can be compared at the SAME simulation tick. */
    { static int parsed = 0, n_at = 0; static unsigned at[16]; static const char* gate;
      static unsigned open_at = 0;
      if (!parsed) { parsed = 1; gate = getenv("LD_SURF_DUMP_GATE");
          const char* e = getenv("SAKA_SIMTICK_SHOT");
          for (const char* q = e; q && *q && n_at < 16; ) { char* endp; unsigned long x = strtoul(q, &endp, 10);
              if (endp == q) break; at[n_at++] = (unsigned)x; q = (*endp == ',') ? endp + 1 : endp; } }
      if (gate && n_at) {
          for (int k = 0; k < n_at; k++) if (s_sim_ticks == at[k]) open_at = s_sim_ticks;
          if (open_at && s_sim_ticks - open_at <= 4u) {
              if (((s_sim_ticks - open_at) & 1u) == 0) { FILE* f = fopen(gate, "w"); if (f) { fprintf(f, "%u\n", s_sim_ticks); fclose(f); }
                  fprintf(stderr, "[simtick] shot at match tick %u\n", s_sim_ticks); }
              else remove(gate);
          } else if (open_at && s_sim_ticks - open_at == 5u) { remove(gate); open_at = 0; }
      } }
    const uint32_t t = s_sim_timer;
    if (!t) return;
    st_be32(t + 0xC0, s_sim_save[0]);
    st_be32(t + 0x150, s_sim_save[1]);
    st_be32(t + 0x154, s_sim_save[2]);
    for (int i = 0; i < 3; i++) { st_be32(t + 0x160 + 4 * i, s_sim_save[3 + i]); st_be32(t + 0x16C + 4 * i, s_sim_save[6 + i]); }
    s_sim_timer = 0;
}

/* ---- CRI ADX2 SPURS jobs: run their lifted code ---------------------------
 * CRI's sound jobs (AtomDecoder, AsrVoice, DspPack1/2, DspPitchShifter,
 * HcaMixer; "JOBCRT Ver14") are streamed by the libspurs_jq policy into the
 * job code buffer at LS 0x7C00 and entered at +0x10. Interpreted, they render
 * the title music in real time but only about half of a match's sound, so the
 * match audio broke up and the match waited on it. tools/gen_cri_spu_jobs.py
 * wraps each one as an SPU ELF at 0x7C00 for the build to lift (spu_jobs/)
 * and writes this table. Each registers its CODE as a verified region keyed by
 * its EBOOT address; the two header words the CRT relocates (+0x3C, +0x40)
 * are left out of the compared bytes. A different EBOOT (fingerprint) keeps
 * the interpreter. SAKA_CRI_JOBS=0 turns this off. */
static uint64_t saka_fnv1a64(const uint8_t* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    while (n--) { h ^= *p++; h *= 0x100000001B3ull; }
    return h;
}

void saka_register_cri_job_regions(void)
{
    static const struct { const char* name; uint32_t ea, code_end; int image_id; uint64_t fp; } k_jobs[] = {
#include "spu_jobs/cri_jobs.inc"
    };
    const char* e = getenv("SAKA_CRI_JOBS");
    if (e && *e == '0') { fprintf(stderr, "[saka] CRI SPU jobs left to the interpreter (SAKA_CRI_JOBS=0)\n"); return; }
    for (unsigned i = 0; i < sizeof k_jobs / sizeof k_jobs[0]; i++) {
        const uint8_t* code = vm_base + k_jobs[i].ea;
        if (saka_fnv1a64(code, k_jobs[i].code_end) != k_jobs[i].fp) {
            fprintf(stderr, "[saka] CRI job %s at 0x%08X: fingerprint differs -- left to the interpreter\n",
                    k_jobs[i].name, k_jobs[i].ea);
            continue;
        }
        const saka_spu_code_seg seg[2] = {
            { 0x7C00u, 0x3Cu, code },                                       /* signature, entry, header */
            { 0x7C60u, k_jobs[i].code_end - 0x60u, code + 0x60u },           /* CRT and the job's code */
        };
        const int ok = spu_overlay_register_verified_region(k_jobs[i].ea, 0x7C00u, k_jobs[i].code_end,
                                                           k_jobs[i].image_id, seg, 2);
        fprintf(stderr, "[saka] CRI job %s 0x%08X -> lifted image %d for LS 0x7C00..0x%05X: %s\n",
                k_jobs[i].name, k_jobs[i].ea, k_jobs[i].image_id, 0x7C00u + k_jobs[i].code_end,
                ok ? "registered" : "FAILED");
    }
}

/* ---- Load turbo: when the pxd file loader is busy ------------------------
 * The pxd loader advances one step per game frame: the main thread posts the
 * async file thread's semaphore at three sites (0x006DC7BC, 0x006DC95C,
 * 0x006DCAFC), the file work itself finishes in about 1 ms, and the main
 * thread then sleeps out the rest of the 33 ms frame in the vblank wait. So
 * every file costs a frame -- the same pace as on the console (RPCS3 run:
 * same timings), only because the game is frame-locked.
 * The post sites are recorded here (QPC time ring); main.cpp's frame clock
 * reads saka_load_steps_since() and runs vblank faster while loading is
 * sustained. SAKA_LOAD_TURBO=1 (or 0) turns it off. */
#define SAKA_LOAD_RING 64
static volatile LONGLONG s_load_ring[SAKA_LOAD_RING];
static volatile LONG     s_load_idx;
/* The semaphore those posts go to (the async file thread's) and how many times
 * a thread has come back to wait on it: the file thread finished a step when
 * this moves (main.cpp's loader pump waits on it). */
static volatile LONG     s_load_sem = -1, s_load_waits = 0;
int64_t sys_semaphore_post(ppu_context* ctx);   /* runtime/syscalls/sys_semaphore.c */
int64_t sys_semaphore_wait(ppu_context* ctx);

long saka_load_steps_total(void) { return s_load_idx; }
long saka_load_waits_total(void) { return s_load_waits; }

/* How long the file thread works on each loader step: from the step's post to
 * the thread's next wait on the loader semaphore. Every 2 s while loading:
 * "[loadstep] n avg max" (the pump can only add steps within a frame when a
 * step takes less than its 15 ms budget). */
static volatile LONGLONG s_step_post_qpc = 0;
static int64_t saka_semaphore_wait(ppu_context* ctx)
{
    if ((LONG)(uint32_t)ctx->gpr[3] == s_load_sem) {
        InterlockedIncrement(&s_load_waits);
#ifdef _WIN32
        const LONGLONG posted = InterlockedExchange64(&s_step_post_qpc, 0);
        if (posted) {
            static LARGE_INTEGER f; static LONGLONG win0; static unsigned n; static double sum, mx;
            LARGE_INTEGER q; QueryPerformanceCounter(&q);
            if (!f.QuadPart) { QueryPerformanceFrequency(&f); win0 = q.QuadPart; }
            const double ms = (double)(q.QuadPart - posted) * 1000.0 / (double)f.QuadPart;
            n++; sum += ms; if (ms > mx) mx = ms;
            if (q.QuadPart - win0 >= 2 * f.QuadPart) {
                fprintf(stderr, "[loadstep] last 2 s: %u steps, file thread %.1f ms avg, %.1f ms max\n",
                        n, n ? sum / n : 0.0, mx);
                n = 0; sum = mx = 0; win0 = q.QuadPart;
            }
        }
#endif
    }
    return sys_semaphore_wait(ctx);
}

static int64_t saka_semaphore_post(ppu_context* ctx)
{
#ifdef _WIN32
    const uint32_t lr = (uint32_t)ctx->lr;
    if (lr == 0x006DC7BCu || lr == 0x006DC95Cu || lr == 0x006DCAFCu) {
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        s_load_sem = (LONG)(uint32_t)ctx->gpr[3];
        const LONG i = InterlockedIncrement(&s_load_idx);
        s_load_ring[(unsigned)i & (SAKA_LOAD_RING - 1)] = q.QuadPart;
        InterlockedExchange64(&s_step_post_qpc, q.QuadPart);
        /* SAKA_LOADER_CHAIN=1: name the guest call chain of the first few
         * step-A posts (who drives the loader once a frame). */
        { static int on = -1, n = 0;
          if (on < 0) { const char* e = getenv("SAKA_LOADER_CHAIN"); on = e && *e == '1'; }
          if (on && (n < 3 || (i % 40) == 0) && n < 24) { n++;
              extern void ppu_log_host_chain(const char*);
              ppu_log_host_chain(lr == 0x006DCAFCu ? "loader-step-A" :
                                 lr == 0x006DC95Cu ? "loader-step-B" : "loader-step-C"); } }
    }
#endif
    return sys_semaphore_post(ctx);
}

/* Loader steps recorded at or after QPC time `since` (at most SAKA_LOAD_RING). */
int saka_load_steps_since(long long since)
{
    int n = 0;
    for (int i = 0; i < SAKA_LOAD_RING; i++)
        if (s_load_ring[i] && s_load_ring[i] >= since) n++;
    return n;
}

int saka_load_turbo_factor(void)
{
    static int f = -1;
    if (f < 0) {
        /* Off by default since 2026-11-07: the owner wants loads made fast
         * natively, not by running the game's frames faster. SAKA_LOAD_TURBO=8
         * still opts in. */
        const char* e = getenv("SAKA_LOAD_TURBO");
        f = e ? atoi(e) : 1;
        if (f < 1) f = 1;
        if (f > 16) f = 16;
    }
    return f;
}

static void saka_cheats_start(void);   /* cheats\BLJM61064.txt (end of this file) */
void saka_install_syscall_hooks(void)
{
    saka_cheats_start();
    { extern void saka_overlay_install(void); saka_overlay_install(); }   /* saka_overlay.cpp: the F2 menu */
    /* Always: the loader-step record feeds [perf] and the loader work below,
     * not only the opt-in turbo. One lr compare per post. */
    lv2_syscall_register(&g_lv2_syscalls, 94 /* SYS_SEMAPHORE_POST */, saka_semaphore_post);
    lv2_syscall_register(&g_lv2_syscalls, 92 /* SYS_SEMAPHORE_WAIT */, saka_semaphore_wait);
    const char* e = getenv("SAKA_USLEEP_KICK");
    if (!e || *e != '1') return;
    lv2_syscall_register(&g_lv2_syscalls, 141 /* SYS_TIMER_USLEEP */, saka_timer_usleep);
}

/* ---- Cheats -----------------------------------------------------------------
 * cheats/BLJM61064.txt: cheats in VitaCheat's code syntax with PS3 addresses (the
 * PS Vita version's PCSG00198.psv moved over; NOTES.md "Cheats"). cheats/enabled.txt:
 * the names of the cheats that are on, one per line (the launcher writes it). A host
 * thread reads both files when they change (checked every second) and applies the
 * enabled codes ten times a second, as VitaCheat does. Values are stored big-endian,
 * the way the PS3 game reads them.
 *   $0000 addr value      8-bit write ($0100 16-bit, $0200 32-bit)
 *   $4T0x addr value      multi-write (T = 0/1/2: 8/16/32-bit) with the next line,
 *   $CCCC step vstep      CCCC writes, the address +step and the value +vstep each time
 *   $F0nn 0 v             code switch nn: a hook in the recompiled code acts while the
 *                         cheat is on (the Vita ARM code patches, moved; see below);
 *                         v is its value for a switch that takes one (7: the points)
 *   $CT00 addr value      the port's own: write only if memory holds a larger value
 *                         ($DT0x + next line: the same as a multi-write), T = 0/1/2
 *   Lines starting with # are comments; "#@..." lines carry the launcher's metadata.
 * Other code types are skipped, the Vita ARM code patches ($A100/$A200) among them:
 * the game's code is recompiled, so its PPC bytes in memory are never run.
 * Writes go only to committed guest memory. SAKA_CHEATS=0 turns the thread off. */
static volatile uint8_t s_cheat_switch[32];   /* set by the cheat thread from the enabled $F0nn codes */
static volatile long s_cheat_hook_calls[32];  /* calls of each switch's hook while it is on (logged by the thread) */
/* 2026-11-09 (owner: "고정도 가능하게 해주고 갱신도 가능하게 해서 변경도 가능해야해"): a switch can carry a value
 * ($F0nn 0 value, replaced through values.txt like a data code's) and 고정 (values.txt "once" 0/1). gen goes up by
 * one when the switch is turned on, its value or 고정 changes, or 갱신 asks for it (values.txt "refresh"), so a hook
 * can put the value in once more. */
static volatile uint32_t s_cheat_switch_value[32];
static volatile uint8_t s_cheat_switch_lock[32];
static volatile long s_cheat_switch_gen[32];
/* The money (0x00EE2844, 만엔) while a locked cheat writes it: the game's three money changers (func_0053F3A0,
 * func_0053F434, func_00544558) put it back right after their store (saka_money_hook), so a purchase never shows
 * the lowered amount. Before 2026-11-09 only the cheat thread rewrote it ten times a second, and the HUD (func_00084C38,
 * redrawn when money changes) kept the amount after the purchase (owner: "돈 마저도 고정이 안돼네"). */
#define CHEAT_MONEY 0x00EE2844u
static volatile int s_money_lock;
static volatile uint32_t s_money_value;
#ifdef _WIN32
typedef struct { uint8_t size, lower; uint32_t addr, value, count, step, vstep; } cheat_code;
typedef struct { char name[128]; int first, n, on, once, done; unsigned refresh; } cheat_def;
static cheat_code s_cheat_codes[4096];
static cheat_def s_cheat_defs[512];
static int s_ncheat_codes, s_ncheat_defs, s_cheat_skipped;
static FILETIME s_cheat_time[8];

/* ---- Per-save settings (2026-11-09, owner: "이런 치트들이 세이브 파일별로 별도로 작동하는 메커니즘을 야구츠쿠3의 치트적용
 * 관련해서 참고해"; YKTK3 keeps them in cheat_saves/<save>/) ------------------------------------------------------
 * enabled.txt, values.txt, players.txt and lists.txt are kept per save in cheats/saves/<id>/: "MAIN00".. for the save
 * slots (BLJM61064-SAVEDATA/MAINnn.DAT is slot nn+1), "new" for a new game not saved yet. At the title (and in its EDIT
 * mode) the files in cheats/ itself are used. The code list (BLJM61064.txt) is the same for every save.
 * - Which save: the save data files the game reads and writes (cellSaveData's g_savedata_file_hook). After the title
 *   (its menu file, main.cpp's file-open hook calls saka_cheats_title), the game that starts is the MAINnn.DAT read
 *   since, else a new game: one whose roster differs from the one in memory at the title. A new game's settings
 *   start empty. Saving into another slot makes the game that save, and its settings go with it (copied over).
 * - The first save used after this change starts from the settings that were in cheats/ (the owner had one save).
 * - cheats/live/save.txt: "<id>\t<description>" for the launcher and the menu (saka_cheat_profile). */
static volatile LONG s_save_slot = -2;      /* -2 title, -1 new game, 0.. MAINnn */
static volatile LONG s_save_read = -2;      /* the MAINnn.DAT read last since the title (-2 none) */
static volatile LONG s_save_written = -2;   /* a MAINnn.DAT the game wrote, for the cheat thread */
static volatile LONG s_save_title = 1;      /* at the title, or after it until a game starts */
static volatile LONG s_save_title_seen;     /* the cheat thread still has to take the title's roster */
static volatile LONG s_save_gen;            /* +1 on every change of s_save_slot (the F2 menu reloads) */
static int s_profile_switched;              /* once cheats are written again after a change of save */
static uint32_t s_title_roster[23];

static void cheat_profile_dir(LONG slot, char* out, size_t n)
{
    if (slot == -2) snprintf(out, n, "cheats/");
    else if (slot == -1) snprintf(out, n, "cheats/saves/new/");
    else snprintf(out, n, "cheats/saves/MAIN%02ld/", (long)slot);
}
static const char* cheat_path(const char* name)   /* the cheat thread's own buffer */
{
    static char buf[96];
    char dir[64];
    cheat_profile_dir(s_save_slot, dir, sizeof dir);
    snprintf(buf, sizeof buf, "%s%s", dir, name);
    return buf;
}
static void cheat_profile_desc(LONG slot, char* out, size_t n)
{
    if (slot == -2) snprintf(out, n, "타이틀·에디트 (공통 설정)");
    else if (slot == -1) snprintf(out, n, "새 게임 (아직 저장 안 함)");
    else snprintf(out, n, "세이브 %ld (MAIN%02ld.DAT)", (long)slot + 1, (long)slot);
}
static const char* const s_profile_files[8] = { "enabled.txt", "values.txt", "players.txt", "lists.txt", "training.txt", "staff.txt",
                                                "teamcolor.txt", "teamcolor_backup.txt" };
static void cheat_profile_copy(LONG from, LONG to, int only_missing)
{
    char a[64], b[64], fa[96], fb[96];
    cheat_profile_dir(from, a, sizeof a);
    cheat_profile_dir(to, b, sizeof b);
    CreateDirectoryA("cheats/saves", NULL);
    { char d[64]; snprintf(d, sizeof d, "%s", b); d[strlen(d) - 1] = 0; CreateDirectoryA(d, NULL); }
    for (int i = 0; i < 8; i++) {
        snprintf(fa, sizeof fa, "%s%s", a, s_profile_files[i]);
        snprintf(fb, sizeof fb, "%s%s", b, s_profile_files[i]);
        if (GetFileAttributesA(fa) == INVALID_FILE_ATTRIBUTES) { if (!only_missing) DeleteFileA(fb); continue; }
        CopyFileA(fa, fb, only_missing ? TRUE : FALSE);
    }
}

static void cheat_trim(char* s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
}

static const char* cheat_skip_space(const char* s)
{
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s += 3;   /* UTF-8 BOM */
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void cheats_load(void)
{
    s_ncheat_codes = s_ncheat_defs = s_cheat_skipped = 0;
    FILE* f = fopen("cheats/BLJM61064.txt", "rb");
    if (!f) return;
    char line[512];
    int multi = -1;   /* a $4 code waiting for its second line */
    while (fgets(line, sizeof line, f)) {
        cheat_trim(line);
        const char* s = cheat_skip_space(line);
        if (s[0] == '_' && s[1] == 'V' && (s[2] == '0' || s[2] == '1')) {
            if (s_ncheat_defs >= 512) break;
            cheat_def* c = &s_cheat_defs[s_ncheat_defs++];
            snprintf(c->name, sizeof c->name, "%s", cheat_skip_space(s + 3));
            c->first = s_ncheat_codes;
            c->n = c->on = c->once = c->done = 0;
            c->refresh = 0;
            multi = -1;
            continue;
        }
        unsigned type, a, v;
        if (s[0] != '$' || !s_ncheat_defs || sscanf(s + 1, "%x %x %x", &type, &a, &v) != 3) continue;
        if (multi >= 0) {
            cheat_code* m = &s_cheat_codes[multi];
            m->count = type; m->step = a; m->vstep = v;
            multi = -1;
            continue;
        }
        const unsigned kind = type >> 12, bits = (type >> 8) & 0xFu;
        if (kind == 0xF && s_ncheat_codes < 4096) {   /* code switch: size 0, addr = switch number, value = its value */
            cheat_code* k = &s_cheat_codes[s_ncheat_codes++];
            k->size = 0; k->lower = 0; k->addr = type & 0xFFu; k->value = v; k->count = 0; k->step = k->vstep = 0;
            s_cheat_defs[s_ncheat_defs - 1].n++;
            continue;
        }
        const uint8_t size = bits == 0 ? 1 : bits == 1 ? 2 : bits == 2 ? 4 : 0;
        /* the port's own kinds C (one write) and D (multi-write, like 4): write only where the value in memory is
         * larger than the cheat's, so "1 week left" shortens a running construction or training and leaves an
         * idle slot (0) alone */
        if ((kind != 0 && kind != 4 && kind != 0xC && kind != 0xD) || !size || s_ncheat_codes >= 4096) { s_cheat_skipped++; continue; }
        cheat_code* k = &s_cheat_codes[s_ncheat_codes++];
        k->size = size; k->addr = a; k->value = v; k->count = 1; k->step = 0; k->vstep = 0;
        k->lower = kind == 0xC || kind == 0xD;
        s_cheat_defs[s_ncheat_defs - 1].n++;
        if (kind == 4 || kind == 0xD) multi = s_ncheat_codes - 1;
    }
    fclose(f);
}

static void cheats_enable(void)
{
    for (int i = 0; i < s_ncheat_defs; i++) s_cheat_defs[i].on = 0;
    FILE* f = fopen(cheat_path("enabled.txt"), "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        cheat_trim(line);
        const char* s = cheat_skip_space(line);
        for (int i = 0; *s && i < s_ncheat_defs; i++)
            if (strcmp(s_cheat_defs[i].name, s) == 0) s_cheat_defs[i].on = 1;
    }
    fclose(f);
}

static int cheat_committed(uint32_t ea, uint32_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (ea < 0x10000u || (uint64_t)ea + n > 0x40000000ull) return 0;
    if (!VirtualQuery(vm_base + ea, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    return (uint8_t*)mbi.BaseAddress + mbi.RegionSize >= vm_base + ea + n || cheat_committed(ea + n - 1, 1);
}

/* ---- Cheat engine additions (2026-11-08, owner: values, per-player edits and locks as in the YKTK3 launcher) --
 * cheats/values.txt (the launcher writes it): "<cheat name>\t<n>\t<value>" replaces the value of the cheat's n-th
 *   code (0-based, a multi-write counts once; decimal, or hex with 0x); "<cheat name>\tonce\t1" writes the cheat once
 *   (when it is turned on or a value changes, and a game is loaded) instead of ten times a second (고정 off).
 * cheats/players.txt (the launcher writes it): edits of single players by player ID, written into the user's club
 *   roster (0x00EE2954, 23 x 0x218 bytes; the game rebuilds it there when the GM moves to another club, so the edits
 *   follow the club and touch no other club). "<player id>\t<field>\t<value>\t<lock>": field "a<n>" ability n (0-45,
 *   +0x0C+n, 0-100) with its experience (u16 +0x6A+2n) from the game's table 0x4D, as the game's own setter
 *   func_005773A8 does; "l<n>" the limit of ability n (+0x3B+n, 0-100); "b<hex>"/"h<hex>"/"w<hex>" a byte/u16/u32 at
 *   that record offset. lock 1 = ten times a second, 0 = once per file change while the player is in the roster.
 * cheats/live/state.bin (written here every 2 s when it changed): the game state for the launcher's lists (names and
 *   current values): "SAKA", u32 range count, then per range u32 address, u32 size, the bytes (big-endian as in
 *   the game). Written to state.tmp and renamed over state.bin. */
#define CHEAT_ROSTER 0x00EE2954u
#define CHEAT_RECORD 0x218u
typedef struct { uint32_t id; char kind; uint16_t off; uint8_t size; uint32_t value; uint8_t lock, done; } cheat_player_edit;
static cheat_player_edit s_cheat_players[2048];
static int s_ncheat_players;

static uint32_t cheat_number(const char* s)
{
    return (uint32_t)strtoul(s, NULL, (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10);
}

static void cheats_values(void)
{
    FILE* f = fopen(cheat_path("values.txt"), "rb");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        cheat_trim(line);
        char* name = (char*)cheat_skip_space(line);
        char* key = strchr(name, '\t');
        if (!key) continue;
        *key++ = 0;
        char* val = strchr(key, '\t');
        if (!val) continue;
        *val++ = 0;
        for (int i = 0; i < s_ncheat_defs; i++) {
            cheat_def* d = &s_cheat_defs[i];
            if (strcmp(d->name, name) != 0) continue;
            if (strcmp(key, "once") == 0) { d->once = atoi(val) != 0; continue; }
            if (strcmp(key, "refresh") == 0) { d->refresh = (unsigned)strtoul(val, NULL, 10); continue; }   /* 갱신 */
            const int n = atoi(key);
            /* the n-th code of the cheat (codes are stored one per $ line except a multi-write's second line; a code
             * switch's value too) */
            if (n >= 0 && n < d->n) s_cheat_codes[d->first + n].value = cheat_number(val);
        }
    }
    fclose(f);
}

/* Per-player multipliers (2026-11-09, owner: "이것도 선수별로 개별로도 가능하게 해주고. 팀의 선수 리스트에서 선수를
 * 선택하면 ... 중복이나 이런 문제가 발생할수 있는것에 대한 방지 대책도"): players.txt fields xt (normal training), xp
 * (special training, SP), xh (秘蔵っ子: 個性強化 and 能力覚醒), xa (awakening points), 1-100; since 2026-11-13 (owner:
 * "선수별로도 각종 훈련들(기본, 스페셜, 비장의 아이 등)의 배율", "선수들 마다 각각 내가 직접 설정") SP and 秘蔵っ子 have their own
 * values. The older xs (SP and 秘蔵っ子 together) still counts where xp / xh is not set. The hooks of switches 12-14 find the player from the
 * record they get (the club's 23 or the youth team's 15) and use his own value instead of the switch's:
 * - never both multiplied, and his value works with the switch off too;
 * - one value per player and kind (a later line for the same player and kind replaces the earlier one);
 * - a player who left the club and its youth team matches no record, so his line does nothing (it works again if he
 *   comes back);
 * - the awakening points at the end of an SP plan get the larger of the special and the awakening multiplier, not
 *   both multiplied (saka_sp_awake_mult_hook / saka_awake_mult_hook).
 * Two tables: players.txt is read into the one not in use, then they are swapped, so a hook never sees half of one. */
typedef struct { uint32_t id; uint8_t m[5]; } cheat_player_mult;   /* m: t training, s (older SP + 秘蔵っ子), a awakening, p SP, h 秘蔵っ子; 0 = none */
static cheat_player_mult s_pmult[2][128];
static volatile int s_pmult_n[2], s_pmult_cur;

static void cheats_players_load(void)
{
    s_ncheat_players = 0;
    const int nb = !s_pmult_cur;
    int nm = 0;
    FILE* f = fopen(cheat_path("players.txt"), "rb");
    if (!f) { s_pmult_n[nb] = 0; s_pmult_cur = nb; return; }
    char line[256];
    while (fgets(line, sizeof line, f) && s_ncheat_players < 2048) {
        cheat_trim(line);
        const char* s = cheat_skip_space(line);
        char field[32];
        char value[32];
        unsigned id, lock = 1;
        if (sscanf(s, "%u\t%31[^\t]\t%31[^\t]\t%u", &id, field, value, &lock) < 3) continue;
        cheat_player_edit* e = &s_cheat_players[s_ncheat_players];
        e->id = id; e->kind = field[0]; e->value = cheat_number(value); e->lock = lock != 0; e->done = 0;
        if (field[0] == 'x') {   /* a per-player multiplier, not a write */
            static const char kinds[] = "tsaph";
            const char* k = field[1] ? strchr(kinds, field[1]) : NULL;
            if (!k || field[2]) continue;
            const unsigned long v = strtoul(value, NULL, 10);
            int j = 0;
            while (j < nm && s_pmult[nb][j].id != id) j++;
            if (j == nm) {
                if (nm >= 128) continue;
                s_pmult[nb][j].id = id;
                memset(s_pmult[nb][j].m, 0, sizeof s_pmult[nb][j].m);
                nm++;
            }
            s_pmult[nb][j].m[k - kinds] = (uint8_t)(v < 1 ? 0 : v > 100 ? 100 : v);
            continue;
        }
        if (e->kind == 'a' || e->kind == 'l') {
            const unsigned n = (unsigned)atoi(field + 1);
            if (n >= 46) continue;
            e->off = (uint16_t)n; e->size = 1;
            if (e->value > 100) e->value = 100;
        } else if (e->kind == 'b' || e->kind == 'h' || e->kind == 'w') {
            const unsigned off = (unsigned)strtoul(field + 1, NULL, 16);
            e->size = e->kind == 'b' ? 1 : e->kind == 'h' ? 2 : 4;
            if (off + e->size > CHEAT_RECORD) continue;
            e->off = (uint16_t)off;
        } else continue;
        s_ncheat_players++;
    }
    fclose(f);
    s_pmult_n[nb] = nm;
    s_pmult_cur = nb;
}

/* A game is loaded: the user's club roster holds at least one valid player ID (0 < id < 0x46A2, or the game's
 * three special IDs 0x7FFD-0x7FFF). Writes that happen once wait for it, so a save loaded afterwards cannot undo them. */
static int cheat_game_loaded(void)
{
    if (!cheat_committed(CHEAT_ROSTER, 23 * CHEAT_RECORD)) return 0;
    for (uint32_t k = 0; k < 23; k++) {
        const uint32_t id = ld_be32(CHEAT_ROSTER + k * CHEAT_RECORD);
        if ((id > 0 && id < 0x46A2u) || (id >= 0x7FFBu && id <= 0x7FFFu)) return 1;
    }
    return 0;
}

/* The game's experience table (master table 0x4D: u16 per ability value), read the way func_004CA698 does. */
static uint32_t cheat_ability_exp(uint32_t v)
{
    if (!cheat_committed(0x008D4A9Cu + 0x4Du * 4u, 4)) return 0xFFFFFFFFu;
    const uint32_t hdr = ld_be32(0x008D4A9Cu + 0x4Du * 4u);
    if (!hdr || !cheat_committed(hdr, 8)) return 0xFFFFFFFFu;
    const uint32_t data = ld_be32(hdr + 4);
    if (!data || !cheat_committed(data + 2 * v, 2)) return 0xFFFFFFFFu;
    return ((uint32_t)vm_base[data + 2 * v] << 8) | vm_base[data + 2 * v + 1];
}

static void cheats_players_apply(int loaded)
{
    if (!loaded || !s_ncheat_players) return;
    for (int i = 0; i < s_ncheat_players; i++) {
        cheat_player_edit* e = &s_cheat_players[i];
        if (!e->lock && e->done) continue;
        for (uint32_t k = 0; k < 23 + 15; k++) {   /* the club's 23, then its youth team's 15 (0x00EE86E0, 2026-11-09) */
            const uint32_t rec = k < 23 ? CHEAT_ROSTER + k * CHEAT_RECORD : 0x00EE86E0u + (k - 23) * CHEAT_RECORD;
            if (ld_be32(rec) != e->id) continue;
            uint8_t* p = vm_base + rec;
            if (e->kind == 'a') {
                const uint32_t exp = cheat_ability_exp(e->value);
                p[0x0C + e->off] = (uint8_t)e->value;
                if (exp != 0xFFFFFFFFu) { p[0x6A + 2 * e->off] = (uint8_t)(exp >> 8); p[0x6B + 2 * e->off] = (uint8_t)exp; }
            } else if (e->kind == 'l') {
                p[0x3B + e->off] = (uint8_t)e->value;
            } else if (e->size == 1) {
                p[e->off] = (uint8_t)e->value;
            } else if (e->size == 2) {
                p[e->off] = (uint8_t)(e->value >> 8); p[e->off + 1] = (uint8_t)e->value;
            } else {
                st_be32(rec + e->off, e->value);
            }
            e->done = 1;
            break;
        }
    }
}

/* cheats/lists.txt (the launcher writes it): "<list>\t<slot>\t<id>\t<lock>" puts an ID into one of the game's
 * candidate lists, lock 1 = ten times a second (the game redraws the lists), 0 = once per file change:
 *   coach  the staff candidates, 4 x 0xC at 0x00EE72A4 (u32 staff ID 1-825, func_00528FD8 / func_00532EA0)
 *   scout  the scout candidates, 4 x 0xC at 0x00EE72D4 (u32 scout ID 1-427; the game shows 3, or 4 with a bonus)
 *   euro   the overseas (European) player list, 20 x 0x14 at 0x00EE70E8 (u32 player ID, func_0052E190). A player of
 *          that list needs a status record in the 500 x 0xC table at 0x00F0C4B8 (func_004CB098): the record of the
 *          player who was in the slot is handed over (its other fields kept), as the Vita cheat did, or a free one. */
typedef struct { char list; uint8_t slot, lock, done; uint32_t id; } cheat_list_edit;
static cheat_list_edit s_cheat_lists[64];
static int s_ncheat_lists;

static void cheats_lists_load(void)
{
    s_ncheat_lists = 0;
    FILE* f = fopen(cheat_path("lists.txt"), "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) && s_ncheat_lists < 64) {
        cheat_trim(line);
        char list[16];
        unsigned slot, id, lock = 1;
        if (sscanf(cheat_skip_space(line), "%15[^\t]\t%u\t%u\t%u", list, &slot, &id, &lock) < 3) continue;
        const char c = strcmp(list, "coach") == 0 ? 'c' : strcmp(list, "scout") == 0 ? 's' : strcmp(list, "euro") == 0 ? 'e' : 0;
        if (!c || slot >= (c == 'e' ? 20u : 4u)) continue;
        if ((c == 'c' && (id == 0 || id >= 0x33Au)) || (c == 's' && (id == 0 || id >= 0x1ACu)) || (c == 'e' && (id == 0 || id >= 0x46A2u))) continue;
        cheat_list_edit* e = &s_cheat_lists[s_ncheat_lists++];
        e->list = c; e->slot = (uint8_t)slot; e->id = id; e->lock = lock != 0; e->done = 0;
    }
    fclose(f);
}

/* The overseas status records (500 x 12 at 0x00F0C4B8: +0 player ID, +4 age, +5 1-4, +8 flags) are a fixed pool
 * that is normally full (the owner's save: 500 of 500). When the game puts a player in the European list and he has no
 * record, func_0052DEFC takes a free one, else the highest one whose player is neither in the European list (20 x 0x14
 * at 0x00EE70E8) nor in the list at 0x00EE7340 (20 x 0x30), and starts it anew. The same rule here, also skipping the
 * players the cheat itself puts in the list. (2026-11-09, owner: "빈 칸: 게임의 해외 선수 기록 500칸이 다 차서 … 이걸
 * 좀 해결해야하는데") */
static int cheat_euro_spare_record(void)
{
    for (int r = 499; r >= 0; r--) {
        const uint32_t id = ld_be32(0x00F0C4B8u + 0xCu * (uint32_t)r);
        if (id == 0) return r;
        int used = 0;
        for (uint32_t k = 0; k < 20 && !used; k++)
            used = ld_be32(0x00EE70E8u + 0x14u * k) == id || ld_be32(0x00EE7340u + 0x30u * k) == id;
        for (int i = 0; i < s_ncheat_lists && !used; i++)
            used = s_cheat_lists[i].list == 'e' && s_cheat_lists[i].id == id;
        if (!used) return r;
    }
    return -1;
}
/* A player's base age (player master +0x10: table 1 below ID 10000, table 2 above; func_004CA1E4), 16-45. */
static uint8_t cheat_player_base_age(uint32_t id)
{
    const uint32_t h1 = ld_be32(0x008D4A9Cu + 4u), h2 = ld_be32(0x008D4A9Cu + 8u);
    if (!cheat_committed(h1, 8) || !cheat_committed(h2, 8)) return 20;
    const uint32_t n1 = ld_be32(h1);
    const uint32_t m = id < n1 ? ld_be32(h1 + 4) + id * 0x3Cu : ld_be32(h2 + 4) + (id - n1) * 0x3Cu;
    if (!cheat_committed(m, 0x3C)) return 20;
    const uint8_t a = vm_base[m + 0x10];
    return a < 16 ? 16 : a > 45 ? 45 : a;
}

static void cheats_lists_apply(int loaded)
{
    if (!loaded || !s_ncheat_lists || !cheat_committed(0x00F0C4B8u, 500u * 0xCu)) return;
    for (int i = 0; i < s_ncheat_lists; i++) {
        cheat_list_edit* e = &s_cheat_lists[i];
        if (!e->lock && e->done) continue;
        e->done = 1;
        if (e->list != 'e') {
            const uint32_t ea = (e->list == 'c' ? 0x00EE72A4u : 0x00EE72D4u) + 0xCu * e->slot;
            if (ld_be32(ea) != e->id) st_be32(ea, e->id);
            continue;
        }
        const uint32_t ea = 0x00EE70E8u + 0x14u * e->slot;
        const uint32_t old = ld_be32(ea);
        if (old == e->id) continue;
        int twice = 0;   /* already in another slot: the game never lists a player twice (func_0052E190) */
        for (uint32_t k = 0; k < 20 && !twice; k++)
            twice = k != e->slot && ld_be32(0x00EE70E8u + 0x14u * k) == e->id;
        if (twice) continue;
        int have = -1, from = -1, free_ = -1;
        for (int r = 0; r < 500; r++) {
            const uint32_t id = ld_be32(0x00F0C4B8u + 0xCu * (uint32_t)r);
            if (id == e->id) have = r;
            else if (old && id == old && from < 0) from = r;
            else if (id == 0 && free_ < 0) free_ = r;
        }
        int rec_given = -1;
        if (have < 0) {
            int r = from >= 0 ? from : free_;
            if (r < 0) r = cheat_euro_spare_record();   /* 2026-11-09: as the game does when all 500 are used */
            if (r < 0) continue;   /* no record to give the player: leave the list as it is */
            rec_given = r;
        }
        const uint8_t age = cheat_player_base_age(e->id);
        if (rec_given >= 0) {
            const uint32_t rec = 0x00F0C4B8u + 0xCu * (uint32_t)rec_given;
            if (rec_given != from) {   /* another player's record: started the way func_0052DEFC starts one */
                memset(vm_base + rec, 0, 12);
                vm_base[rec + 5] = (uint8_t)(1 + e->id % 4u);
            }
            vm_base[rec + 4] = age;
            st_be32(rec, e->id);
        }
        if (!old) {   /* an empty slot: the entry as func_0052E190 writes one (+4 = 2, +5 = the age, the rest 0) */
            memset(vm_base + ea + 4, 0, 0x10);
            vm_base[ea + 4] = 2;
            vm_base[ea + 5] = have >= 0 ? vm_base[0x00F0C4B8u + 0xCu * (uint32_t)have + 4] : age;
        } else if (rec_given >= 0) {
            vm_base[ea + 5] = age;   /* the new player's age, not the old one's */
        }
        st_be32(ea, e->id);
    }
}

/* cheats/training.txt (the F2 menu writes it; 2026-11-13, owner: "스페셜과 비장의 아이 모두 F2에서 내가 훈련도 지정",
 * "완료해서 성공했다고 슬롯에서 바로 빼지말고 계속 담아 놓을 수 있도록 고정"): "sp|hizo<TAB>slot<TAB>player<TAB>kind
 * <TAB>course<TAB>lock" puts a player and a course into a training slot (REA Ghidra, see "Training" above):
 *   sp    3 x 0xC at 0x00EE83A8: +0 player, +4 course (txt_practice_data table 0), +8 u8 weeks left
 *   hizo  3 x 0x20 at 0x00EE83CC: +0 player, +4 kind (0 個性強化 table 2, 1 能力覚醒 table 6), +8 course, +0xC weeks
 * written with the course's full time when the slot holds something else (the game's assign functions would clear the
 * report records and charge again, so they are not called; no cost is charged). lock 1: whenever the slot differs
 * (the game empties it when the plan ends, or another plan is put in), so the same plan goes on; 0: once per change of
 * the file. The slots stay 3 each (the save keeps 3, with other data right after them). */
typedef struct { char kind; uint8_t slot, sub, lock, done; uint32_t id, course; } cheat_train_edit;
static cheat_train_edit s_cheat_train[6];
static int s_ncheat_train;

static void cheats_training_load(void)
{
    s_ncheat_train = 0;
    FILE* f = fopen(cheat_path("training.txt"), "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) && s_ncheat_train < 6) {
        cheat_trim(line);
        char kind[16];
        unsigned slot, id, sub, course, lock = 1;
        if (sscanf(cheat_skip_space(line), "%15[^\t]\t%u\t%u\t%u\t%u\t%u", kind, &slot, &id, &sub, &course, &lock) < 5) continue;
        const char k = strcmp(kind, "sp") == 0 ? 's' : strcmp(kind, "hizo") == 0 ? 'h' : 0;
        if (!k || slot >= 3u || id == 0 || (id >= 0x46A2u && (id < 0x7FFBu || id > 0x7FFFu)) || course == 0 || course >= 0x100u || sub > 1u) continue;
        cheat_train_edit* e = &s_cheat_train[s_ncheat_train++];
        e->kind = k; e->slot = (uint8_t)slot; e->id = id; e->sub = (uint8_t)(k == 'h' ? sub : 0); e->course = course;
        e->lock = lock != 0; e->done = 0;
    }
    fclose(f);
}
static void cheats_training_apply(int loaded)
{
    if (!loaded || !s_ncheat_train || !cheat_committed(0x00EE83A8u, 0x84u)) return;
    for (int i = 0; i < s_ncheat_train; i++) {
        cheat_train_edit* e = &s_cheat_train[i];
        if (!e->lock && e->done) continue;
        e->done = 1;
        if (e->kind == 's') {
            const uint32_t slot = 0x00EE83A8u + 0xCu * e->slot;
            if (ld_be32(slot) == e->id && ld_be32(slot + 4) == e->course && vm_base[slot + 8]) continue;
            const uint32_t w = vm_base[0x00E18A3Cu + e->course * 0x18u + 0x14u] * 2u;
            memset(vm_base + slot, 0, 0xC);
            st_be32(slot, e->id);
            st_be32(slot + 4, e->course);
            vm_base[slot + 8] = (uint8_t)(w ? (w > 255 ? 255 : w) : 1);
        } else {
            /* 2026-11-14: the kind is the BYTE at +4 (func_0051A820 / func_0051A9EC store it with stb; +5..+7 stay 0).
             * It was written as a u32 before, so a 能力覚醒 plan (1) read as 個性強化 (0) in the game's weekly pass. */
            const uint32_t slot = 0x00EE83CCu + 0x20u * e->slot;
            if (ld_be32(slot) == e->id && ld_be32(slot + 4) == (uint32_t)e->sub << 24 && ld_be32(slot + 8) == e->course && vm_base[slot + 0xC]) continue;
            const uint32_t table = e->sub ? 0x00E154A8u : 0x00E17238u;
            const uint32_t w = (vm_base[table + e->course * 0x3Cu + 4u] & 0x7Fu) * 2u;
            memset(vm_base + slot, 0, 0x20);
            st_be32(slot, e->id);
            vm_base[slot + 4] = e->sub;
            st_be32(slot + 8, e->course);
            vm_base[slot + 0xC] = (uint8_t)(w ? w : 1);
        }
        s_cheat_hook_calls[27]++;
    }
}

/* cheats/staff.txt (the F2 menu writes it; 2026-11-13, owner: "코치와 스카우트들의 능력치도 내가 마음대로 편집하고
 * 특능까지도 마음대로 편집 가능하게 해주고. 계약기간 나이등도 마찬가지"): "coach|scout<TAB>field<TAB>value<TAB>lock" for
 * the hired coach (0x00EE2940) or scout (0x00EE6CF8): +0 id, +4 u8 age, +5 u8 contract years left, +8 u32 salary
 * (万), +0xC flags (bit 0: leaving / not renewed). The game reads the staff's effects from the master record of that id
 * every time (func_004CA29C coach 0x00BBA60C + id x 100, func_004CA3A8 scout 0x00E1F6AC + id x 60), so those are
 * written there: coach +9 rank, +0xA policy point, +0xB policy circle (0-3), +0x18..+0x1F the eight systems, +0x2C
 * eight ability ids (u32, data/text2/staff_sp_name.txb); scout +8 rank, +0x1C eight ability ids. A master edit applies
 * to whoever has that coach id (normally only the user's club). Fields: age years salary keep rank policy circle sys0-7
 * ab0-7. lock 1 = ten times a second, 0 = once per change of the file or of the hired id. */
typedef struct { char who; char field[8]; uint32_t value, last_id; uint8_t lock, done; } cheat_staff_edit;
static cheat_staff_edit s_cheat_staff[48];
static int s_ncheat_staff;

static void cheats_staff_load(void)
{
    s_ncheat_staff = 0;
    FILE* f = fopen(cheat_path("staff.txt"), "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) && s_ncheat_staff < 48) {
        cheat_trim(line);
        char who[16], field[16];
        unsigned value, lock = 1;
        if (sscanf(cheat_skip_space(line), "%15[^\t]\t%15[^\t]\t%u\t%u", who, field, &value, &lock) < 3) continue;
        const char w = strcmp(who, "coach") == 0 ? 'c' : strcmp(who, "scout") == 0 ? 's' : 0;
        if (!w || strlen(field) >= sizeof s_cheat_staff[0].field) continue;
        cheat_staff_edit* e = &s_cheat_staff[s_ncheat_staff++];
        e->who = w; snprintf(e->field, sizeof e->field, "%s", field); e->value = value; e->lock = lock != 0; e->done = 0; e->last_id = 0;
    }
    fclose(f);
}
static void cheats_staff_apply(int loaded)
{
    if (!loaded || !s_ncheat_staff) return;
    for (int i = 0; i < s_ncheat_staff; i++) {
        cheat_staff_edit* e = &s_cheat_staff[i];
        const uint32_t hired = e->who == 'c' ? 0x00EE2940u : 0x00EE6CF8u;
        if (!cheat_committed(hired, 0x14u)) continue;
        const uint32_t id = ld_be32(hired);
        if (id == 0 || (id & 0x80000000u) || id >= (e->who == 'c' ? 0x33Au : 0x1ACu)) continue;   /* none, or a custom coach */
        if (!e->lock && e->done && e->last_id == id) continue;
        e->done = 1; e->last_id = id;
        const uint32_t master = e->who == 'c' ? 0x00BBA60Cu + id * 100u : 0x00E1F6ACu + id * 60u;
        const uint32_t v = e->value;
        const char* f = e->field;
        if (strcmp(f, "age") == 0) vm_base[hired + 4] = (uint8_t)(v < 15 ? 15 : v > 99 ? 99 : v);
        else if (strcmp(f, "years") == 0) vm_base[hired + 5] = (uint8_t)(v < 1 ? 1 : v > 9 ? 9 : v);
        else if (strcmp(f, "salary") == 0) st_be32(hired + 8, v > 100000u ? 100000u : v);
        else if (strcmp(f, "keep") == 0) { if (v) st_be32(hired + 0xC, ld_be32(hired + 0xC) & ~1u); }
        else if (strcmp(f, "rank") == 0) vm_base[master + (e->who == 'c' ? 9 : 8)] = (uint8_t)(v > 15 ? 15 : v);
        else if (e->who == 'c' && strcmp(f, "policy") == 0) vm_base[master + 0xA] = (uint8_t)(v > 24 ? 24 : v);
        else if (e->who == 'c' && strcmp(f, "circle") == 0) vm_base[master + 0xB] = (uint8_t)(v > 3 ? 3 : v);
        else if (e->who == 'c' && strncmp(f, "sys", 3) == 0 && f[3] >= '0' && f[3] <= '7' && !f[4]) vm_base[master + 0x18 + (f[3] - '0')] = v ? 1 : 0;
        else if (strncmp(f, "ab", 2) == 0 && f[2] >= '0' && f[2] <= '7' && !f[3])
            st_be32(master + (e->who == 'c' ? 0x2Cu : 0x1Cu) + 4u * (uint32_t)(f[2] - '0'), v < 0x100u ? v : 0u);
        s_cheat_hook_calls[28]++;
    }
}

static void cheats_export(int loaded)
{
    static const uint32_t ranges[][2] = {
        { 0x00EE2700u, 0x0000B900u },   /* game state: money, the user's club and roster, training, facilities, lists */
        { 0x00F0C4B8u, 500u * 0xCu },   /* the overseas players' status records (func_004CB098) */
        { 0x00F8E030u, 0x00000120u },   /* club affinity, one byte per club */
        { 0x00F8CE28u, 17u * 0xFCu },   /* the GM: current style, then 17 style records (+0xF4 bit 0 = unlocked) */
    };
    static uint8_t* last;
    static size_t last_size;
    if (!loaded) return;
    size_t size = 8 + 8 + 2 * 0x1A8;  /* additional EDIT masters, stable export address */
    for (size_t r = 0; r < sizeof ranges / sizeof ranges[0]; r++) size += 8 + ranges[r][1];
    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) return;
    memcpy(buf, "SAKA", 4);
    const uint32_t n = (uint32_t)(sizeof ranges / sizeof ranges[0]);
    const uint32_t exported_n = n + 1;
    memcpy(buf + 4, &exported_n, 4);
    size_t at = 8;
    for (size_t r = 0; r < n; r++) {
        memcpy(buf + at, &ranges[r][0], 4); memcpy(buf + at + 4, &ranges[r][1], 4);
        if (cheat_committed(ranges[r][0], ranges[r][1])) memcpy(buf + at + 8, vm_base + ranges[r][0], ranges[r][1]);
        else memset(buf + at + 8, 0, ranges[r][1]);
        at += 8 + ranges[r][1];
    }
    {
        extern void saka_edit_export(void*);
        const uint32_t extra_range[2] = { 0x7FFB0000u, 2 * 0x1A8u };
        memcpy(buf + at, extra_range, sizeof extra_range);
        saka_edit_export(buf + at + 8);
    }
    if (last && last_size == size && memcmp(last, buf, size) == 0) { free(buf); return; }
    CreateDirectoryA("cheats/live", NULL);
    FILE* f = fopen("cheats/live/state.tmp", "wb");
    if (f) {
        const int ok = fwrite(buf, 1, size, f) == size;
        fclose(f);
        if (ok) MoveFileExA("cheats/live/state.tmp", "cheats/live/state.bin", MOVEFILE_REPLACE_EXISTING);
    }
    free(last);
    last = buf;
    last_size = size;
}

static void cheats_apply(void)
{
    uint8_t sw[32] = { 0 }, lk[32] = { 0 };
    uint32_t val[32] = { 0 }, ref[32] = { 0 };
    for (int i = 0; i < s_ncheat_defs; i++)
        if (s_cheat_defs[i].on)
            for (int c = s_cheat_defs[i].first; c < s_cheat_defs[i].first + s_cheat_defs[i].n; c++)
                if (s_cheat_codes[c].size == 0 && s_cheat_codes[c].addr < 32) {
                    const uint32_t a = s_cheat_codes[c].addr;
                    sw[a] = 1;
                    val[a] = s_cheat_codes[c].value;
                    lk[a] = !s_cheat_defs[i].once;
                    ref[a] = s_cheat_defs[i].refresh;
                }
    static uint8_t last_sw[32], last_lk[32];
    static uint32_t last_val[32], last_ref[32];
    for (int i = 0; i < 32; i++) {
        s_cheat_switch_value[i] = val[i];
        s_cheat_switch_lock[i] = lk[i];
        if (sw[i] && (!last_sw[i] || val[i] != last_val[i] || lk[i] != last_lk[i] || ref[i] != last_ref[i]))
            s_cheat_switch_gen[i]++;   /* turned on, a new value or 고정, or 갱신 */
        last_sw[i] = sw[i]; last_lk[i] = lk[i]; last_val[i] = val[i]; last_ref[i] = ref[i];
        s_cheat_switch[i] = sw[i];
    }
    int money = 0;
    uint32_t money_value = 0;
    for (int i = 0; i < s_ncheat_defs && !money; i++)
        if (s_cheat_defs[i].on && !s_cheat_defs[i].once)
            for (int c = s_cheat_defs[i].first; c < s_cheat_defs[i].first + s_cheat_defs[i].n; c++) {
                const cheat_code* k = &s_cheat_codes[c];
                if (k->size == 4 && k->addr == CHEAT_MONEY && k->count && !k->lower) { money = 1; money_value = k->value; break; }
            }
    s_money_value = money_value;
    s_money_lock = money;
    int loaded = -1;
    for (int i = 0; i < s_ncheat_defs; i++) {
        if (!s_cheat_defs[i].on) continue;
        if (s_cheat_defs[i].once) {
            if (s_cheat_defs[i].done) continue;
            if (loaded < 0) loaded = cheat_game_loaded();
            if (!loaded) continue;
            s_cheat_defs[i].done = 1;
        }
        for (int c = s_cheat_defs[i].first; c < s_cheat_defs[i].first + s_cheat_defs[i].n; c++) {
            const cheat_code* k = &s_cheat_codes[c];
            const uint32_t span = k->count ? (k->count - 1) * k->step + k->size : 0;
            if (!k->count || !cheat_committed(k->addr, span)) continue;
            uint32_t ea = k->addr, v = k->value;
            for (uint32_t j = 0; j < k->count; j++, ea += k->step, v += k->vstep) {
                uint8_t* p = vm_base + ea;
                if (k->lower) {
                    const uint32_t cur = k->size == 1 ? p[0] : k->size == 2 ? ((uint32_t)p[0] << 8 | p[1]) : ld_be32(ea);
                    if (cur <= v) continue;
                }
                if (k->size == 1) p[0] = (uint8_t)v;
                else if (k->size == 2) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
                else { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
            }
        }
    }
}

/* A once cheat (고정 off) is written again only when it is turned on, its values or 고정 change, or 갱신 asks for it
 * (values.txt "refresh"); the others keep "done" over a reload. Before 2026-11-09 any change of the three files wrote
 * every once cheat again, so turning one cheat on put the others' values back too. */
static uint32_t cheat_sig(const cheat_def* d)
{
    uint32_t h = 2166136261u;
    for (int c = d->first; c < d->first + d->n; c++) {
        const cheat_code* k = &s_cheat_codes[c];
        const uint32_t w[6] = { k->addr, k->value, k->count, k->step, k->vstep, (uint32_t)k->size << 8 | k->lower };
        for (int j = 0; j < 6; j++) h = (h ^ w[j]) * 16777619u;
    }
    const uint32_t s[3] = { (uint32_t)d->on, (uint32_t)d->once, d->refresh };
    for (int j = 0; j < 3; j++) h = (h ^ s[j]) * 16777619u;
    return h;
}

static void cheats_reload(void)
{
    static char names[512][128];
    static uint32_t sigs[512];
    static int done[512];
    const int n = s_ncheat_defs;
    for (int i = 0; i < n; i++) {
        memcpy(names[i], s_cheat_defs[i].name, sizeof names[i]);
        sigs[i] = cheat_sig(&s_cheat_defs[i]);
        done[i] = s_cheat_defs[i].done;
    }
    cheats_load();
    cheats_enable();
    cheats_values();
    const int keep = !s_profile_switched;
    s_profile_switched = 0;
    for (int i = 0; i < s_ncheat_defs && keep; i++) {
        cheat_def* d = &s_cheat_defs[i];
        for (int j = 0; j < n; j++)
            if (strcmp(names[j], d->name) == 0) {
                if (sigs[j] == cheat_sig(d)) d->done = done[j];
                break;
            }
    }
}

/* Which save's settings are in use (see "Per-save settings"); called by the cheat thread once a second. */
static void cheat_profile_tick(void)
{
    const int loaded = cheat_game_loaded();
    LONG slot = s_save_slot;
    if (InterlockedExchange(&s_save_title_seen, 0)) {   /* the title: its roster in memory, to tell a new game */
        for (uint32_t k = 0; k < 23; k++)
            s_title_roster[k] = cheat_committed(CHEAT_ROSTER, 23 * CHEAT_RECORD) ? ld_be32(CHEAT_ROSTER + k * CHEAT_RECORD) : 0;
        slot = -2;
    }
    if (s_save_title) {
        int differs = 0;
        for (uint32_t k = 0; k < 23 && loaded; k++)
            differs |= ld_be32(CHEAT_ROSTER + k * CHEAT_RECORD) != s_title_roster[k];
        const LONG read = s_save_read;
        if (loaded && read >= 0 && differs) { slot = read; InterlockedExchange(&s_save_title, 0); }   /* a save loaded */
        else if (loaded && differs) {                                                                 /* a new game */
            slot = -1;
            InterlockedExchange(&s_save_title, 0);
            for (int i = 0; i < 8; i++) {   /* a new game starts without the last new game's settings (4, 5 stay: as before) */
                char f[96];
                if (i == 4 || i == 5) continue;
                snprintf(f, sizeof f, "cheats/saves/new/%s", s_profile_files[i]);
                DeleteFileA(f);
            }
        }
    } else {
        const LONG read = InterlockedExchange(&s_save_read, -2);
        if (read >= 0 && loaded) slot = read;   /* another save loaded in the game */
    }
    const LONG written = InterlockedExchange(&s_save_written, -2);
    if (written >= 0 && slot != -2 && written != slot) {   /* saved as another slot: the settings go with the game */
        cheat_profile_copy(slot, written, 0);
        slot = written;
    }
    if (slot == s_save_slot) return;
    if (slot >= 0 && GetFileAttributesA("cheats/saves/from_cheats.txt") == INVALID_FILE_ATTRIBUTES) {
        /* the first save used: the settings that were in cheats/ (before they were per save) go with it */
        cheat_profile_copy(-2, slot, 1);
        FILE* m = fopen("cheats/saves/from_cheats.txt", "wb");
        if (m) {
            fprintf(m, "MAIN%02ld\n", (long)slot);
            fclose(m);
        }
    }
    if (slot >= -1) {
        char d[64];
        cheat_profile_dir(slot, d, sizeof d);
        CreateDirectoryA("cheats/saves", NULL);
        d[strlen(d) - 1] = 0;
        CreateDirectoryA(d, NULL);
    }
    InterlockedExchange(&s_save_slot, slot);
    InterlockedIncrement(&s_save_gen);
    s_profile_switched = 1;
    memset(&s_cheat_time[1], 0, sizeof(FILETIME) * 4);   /* the four files are read again from the new place */
    char desc[96];
    cheat_profile_desc(slot, desc, sizeof desc);
    CreateDirectoryA("cheats/live", NULL);
    FILE* f = fopen("cheats/live/save.txt", "wb");
    if (f) {
        if (slot == -2) fprintf(f, "\t%s\n", desc);
        else if (slot == -1) fprintf(f, "new\t%s\n", desc);
        else fprintf(f, "MAIN%02ld\t%s\n", (long)slot, desc);
        fclose(f);
    }
    fprintf(stderr, "[cheats] settings of: %s\n", desc);
}

static int cheat_file_changed(const char* path, FILETIME* last)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    FILETIME t = { 0, 0 };
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &a)) t = a.ftLastWriteTime;
    if (CompareFileTime(&t, last) == 0) return 0;
    *last = t;
    return 1;
}

/* ---- TEAM COLOR FIT (2026-11-14, owner: "팀컬러를 설정하는것에 따라서 선발 선수들의 스킬과 플레이스타일이 변하도록 하는
 * 치트"; REA Ghidra re/decomp_all.c + the EBOOT's tables) ---------------------------------------------------------------
 * The game works the club's team colors out each time (func_00580434(out[3], club); nothing is saved):
 *   the 70 colors are tried in the order of the list 0x00E54E70 (0x47 ids, sorted at boot by the master's key, high
 *   first; func_00580BAC), and the first three that hold are the club's. Master 0x008D60A4 + id x 0x4C:
 *     +0 u16 key, +2 u16 bit 0 = not used by the game (18 "upper" colors), +4 five (u32 style group, u32 position
 *     group) pairs (group 0x3D = none): each needs its own starter whose style in use is in the group (0x008D5164 +
 *     group x 0x40: 16 style ids) and whose position (record +8, 0-10) is in the position group's mask (0x00EE1E90 +
 *     group x 4, 27 masks set at boot by func_00580C80: 1-11 one position each, 12 wings, 13 side MF, 14 SB, 15 wings
 *     + FW, 16 DM / side MF / OM, 17 side MF + OM, 18 DM + side MF, 19 DM + OM, 20 SB + CB); +0x2C five (min, max) bytes for the formation's five tactic values (block +0x5C, 0-4
 *     each: defensive/attacking, centre/side, possession/fast, retreat/press, cover/flat); +0x36 (min, max) for the
 *     starters' policy +0xE4 / 0x3334, +0x38 for +0xE6, +0x3A how many starters must be inside both; +0x3C an original
 *     skill the KEY player (the first of role list 5, lineup +0x30A4 + 5 x 0x5C; list 0 is the captain: func_002D617C
 *     and func_002D6204 ask the same lists for the captain's and the key player's skills) must have in his style slot;
 *     +0x40 an extra
 *     condition (func_00580188; type 0 = none in every row).
 *   Levels: each color starts at 1; a row of table 100 (109 x 0x10: three color ids, +0xC a bonus) whose colors are all
 *   among the three adds its bonus to those colors; 4 and more become 3 (func_00593300). The level picks the row of
 *   table 99 (211 x 0x34) with the color's effect.
 * So the colors the owner "sets" with the game plan (48-62 need only tactic values) stay at level 1 until colors of
 * the lines (1-47, by play style and position) stand beside them. The cheat makes that happen by changing only the
 * starters' play styles and skills (and the KEY player's original skill where a color asks for one):
 *   자동 (mode 0): of every set of up to three line colors that can be had with the tactics as they are, the one that
 *     gives the highest levels (then keeps most of the colors the club has without the cheat, then changes fewest);
 *   지정 (mode 1): the colors picked in F2; what the cheat cannot give them (tactic values, policies, a color the
 *     game does not use) is shown there.
 * A changed player gets the style (the group's highest tier; of those the family his own style or his position is
 * nearest to) and that style's skill types (style master 0x00E12B08 + style x 0x3C, +0x10) at the chosen level.
 * Tactic values and policies are never written (the game plan is the owner's; '방침 중앙' holds the policies).
 * Off, a player leaving the eleven, or another plan: his slot goes back to what it was when the cheat first changed it
 * (teamcolor_backup.txt in the save's settings keeps it over a restart); a slot whose style is no longer the cheat's
 * (the game, another edit or another save put one there) is left alone. While a starter is changed his slot is held:
 * what the game or a players.txt style edit writes into it is written over (this runs after them).
 * The 18 "upper" colors (2026-11-14, owner: "팀컬러는 상위팀컬러까지 사용가능하도록"): their rows are all there (needs,
 * three level rows in table 99, bonus rows in table 100, names); only the master's bit keeps func_00580434 from
 * trying them, and nothing else reads it. With 'upper' on (the default) the game's own test passes them for the
 * user's club (saka_teamcolor_upper_hook at that test: r25 = the club object, the canonical one or a match copy;
 * other clubs keep the game's rule), and the plans here count them the same way. Off, or the cheat off: as shipped.
 * teamcolor.txt: "on 0|1", "mode 0|1", "colors a b c", "level 1-7", "upper 0|1" (tab separated; the F2 팀컬러 tab
 * writes it). */
#include "saka_teamcolor.h"
#define TC_CLUB    0x00EE2934u
#define TC_MASTER  0x008D60A4u
#define TC_ORDER   0x00E54E70u
#define TC_GROUPS  0x008D5164u
#define TC_POSMASK 0x00EE1E90u
#define TC_STYLES  0x00E12B08u

typedef struct {
    uint32_t rec[11], id[11], style[11], natural[11];
    uint8_t valid[11], pos[11], e4[11], e6[11], tac[5];
    uint32_t leader_rec, leader_id, leader_orig, leader_natural;
} tc_team;
typedef struct { uint32_t id; uint8_t slot; uint8_t natural[0x30], forced[0x30]; } tc_backup;

static struct { int on, mode; uint32_t pick[3], level; int upper; } s_tc_cfg = { 0, 0, { 0, 0, 0 }, 7, 1 };
static tc_backup s_tc_backup[24];
static int s_tc_nbackup;
static saka_tc_status s_tc_status;
static CRITICAL_SECTION s_tc_lock;
static volatile LONG s_tc_lock_ready;
static uint32_t s_tc_plan_sig;
static uint32_t s_tc_plan_style[11], s_tc_plan_ids[11], s_tc_plan_leader, s_tc_plan_leader_id;
static uint32_t s_tc_plan_target[3];
static uint8_t s_tc_plan_state[3];
static int s_tc_plan_ntarget;

static uint32_t tc_be16(uint32_t ea) { return (uint32_t)vm_base[ea] << 8 | vm_base[ea + 1]; }
static uint32_t tc_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int tc_player_ok(uint32_t id) { return id != 0 && (id < 0x46A2u || (id >= 0x7FFBu && id <= 0x7FFFu)); }
/* 1 = the game does not try this color for the user's club now: its master bit, unless 'upper' lifts it. */
static int tc_unused(uint32_t c) { return (tc_be16(TC_MASTER + c * 0x4Cu + 2u) & 1u) && !(s_tc_cfg.on && s_tc_cfg.upper); }
int saka_teamcolor_upper_hook(uint32_t club)   /* func_00580434's "not used" test; 1 = try the color all the same */
{
    if (!s_tc_cfg.on || !s_tc_cfg.upper || club < 0x10000u) return 0;
    if (club == TC_CLUB) return 1;
    const uint32_t id = ld_be32(club);   /* a match copy of the user's club: +0 is the club, as match_team_is_user tests it */
    return id != 0 && (id == 0x100u || id == ld_be32(0x00EE28ACu));
}
static uint32_t tc_slot_of(uint32_t rec) { const uint32_t s = vm_base[rec + 0x10Au]; return rec + 0x10Cu + 0x30u * (s < 5u ? s : 0u); }
static tc_backup* tc_backup_of(uint32_t id)
{
    for (int i = 0; i < s_tc_nbackup; i++)
        if (s_tc_backup[i].id == id) return &s_tc_backup[i];
    return NULL;
}
static void tc_backup_drop(tc_backup* b) { *b = s_tc_backup[--s_tc_nbackup]; }
/* The slot still holds what the cheat put there (the game may have moved its skills on): style and original skill. */
static int tc_block_mine(const uint8_t* blk, const tc_backup* b) { return memcmp(blk, b->forced, 8) == 0; }
/* The player's own style block: the kept one while the cheat holds his slot. */
static const uint8_t* tc_natural_block(uint32_t rec)
{
    const tc_backup* b = tc_backup_of(ld_be32(rec));
    if (b && b->slot == vm_base[rec + 0x10Au]) return b->natural;
    return vm_base + tc_slot_of(rec);
}
static int tc_tables_ok(void)
{
    const uint32_t h100 = ld_be32(0x008D4A9Cu + 4u * 100u);
    return cheat_committed(TC_MASTER, 71u * 0x4Cu) && cheat_committed(TC_ORDER, 0x47u * 4u) && cheat_committed(TC_GROUPS, 61u * 0x40u) &&
           cheat_committed(TC_POSMASK, 27u * 4u) && cheat_committed(TC_STYLES, 67u * 0x3Cu) && cheat_committed(h100, 8u) &&
           ld_be32(h100) == 109u && cheat_committed(ld_be32(h100 + 4u), 109u * 0x10u) && cheat_committed(TC_CLUB, 0x3B64u) &&
           ld_be32(TC_POSMASK + 4u * 11u) != 0;   /* the position masks are set (boot is past) */
}
/* The eleven, the tactic values and the KEY player, as the game reads them; styles = the players' own. */
static int tc_read(tc_team* t)
{
    memset(t, 0, sizeof *t);
    const uint32_t set = ld_be32(TC_CLUB + 0x3B60u);
    if (set > 1u) return 0;
    const uint32_t lineup = TC_CLUB + set * 0x58Cu;
    if (vm_base[lineup + 0x347Cu] > 2u) return 0;
    const uint32_t form = lineup + vm_base[lineup + 0x347Cu] * 0x90u + 0x32CCu;
    int n = 0;
    for (uint32_t i = 0; i < 11u; i++) {
        const uint32_t ri = ld_be32(lineup + 0x3048u + 4u * i);
        if (ri >= 23u) continue;
        const uint32_t rec = CHEAT_ROSTER + ri * CHEAT_RECORD, id = ld_be32(rec);
        if (!tc_player_ok(id)) continue;
        t->rec[i] = rec; t->id[i] = id; t->valid[i] = 1;
        t->pos[i] = (uint8_t)(ld_be32(rec + 8u) < 11u ? ld_be32(rec + 8u) : 11u);
        t->natural[i] = t->style[i] = tc_be32(tc_natural_block(rec));
        t->e4[i] = (uint8_t)(tc_be16(rec + 0xE4u) / 0x3334u);
        t->e6[i] = (uint8_t)(tc_be16(rec + 0xE6u) / 0x3334u);
        n++;
    }
    for (uint32_t k = 0; k < 5u; k++) t->tac[k] = vm_base[form + 0x5Cu + k];
    const uint32_t li = ld_be32(lineup + 0x30A4u + 5u * 0x5Cu);
    if (li < 23u && tc_player_ok(ld_be32(CHEAT_ROSTER + li * CHEAT_RECORD))) {
        t->leader_rec = CHEAT_ROSTER + li * CHEAT_RECORD;
        t->leader_id = ld_be32(t->leader_rec);
        t->leader_natural = t->leader_orig = tc_be32(tc_natural_block(t->leader_rec) + 4);
    }
    return n;
}
static int tc_in_group(uint32_t g, uint32_t style)
{
    if (!style || g > 60u) return 0;
    for (uint32_t i = 0; i < 16u; i++)
        if (ld_be32(TC_GROUPS + g * 0x40u + 4u * i) == style) return 1;
    return 0;
}
static int tc_pos_ok(uint32_t pg, uint32_t pos) { return pg < 27u && pos < 11u && ((ld_be32(TC_POSMASK + 4u * pg) >> pos) & 1u); }
static int tc_reqs(uint32_t c, uint32_t* g, uint32_t* p)
{
    int n = 0;
    for (uint32_t k = 0; k < 5u; k++) {
        const uint32_t gk = ld_be32(TC_MASTER + c * 0x4Cu + 4u + 8u * k);
        if (gk == 0x3Du) continue;
        g[n] = gk; p[n] = ld_be32(TC_MASTER + c * 0x4Cu + 8u + 8u * k); n++;
    }
    return n;
}
static int tc_match(const tc_team* t, const uint32_t* g, const uint32_t* p, int n, int k, uint32_t used)
{
    if (k == n) return 1;
    for (int i = 0; i < 11; i++) {
        if (((used >> i) & 1u) || !t->valid[i] || !tc_pos_ok(p[k], t->pos[i]) || !tc_in_group(g[k], t->style[i])) continue;
        if (tc_match(t, g, p, n, k + 1, used | 1u << i)) return 1;
    }
    return 0;
}
/* 0 = the color holds; else why not (saka_tc_status target_state 1-5). */
static int tc_why_not(const tc_team* t, uint32_t c)
{
    const uint32_t r = TC_MASTER + c * 0x4Cu;
    uint32_t g[5], p[5];
    if (c < 1u || c > 70u || tc_unused(c) || ld_be32(r + 0x40u) != 0) return 5;
    const int n = tc_reqs(c, g, p);
    if (n && !tc_match(t, g, p, n, 0, 0)) return 1;
    for (uint32_t k = 0; k < 5u; k++)
        if (t->tac[k] < vm_base[r + 0x2Cu + 2u * k] || t->tac[k] > vm_base[r + 0x2Du + 2u * k]) return 2;
    uint32_t cnt = 0;
    for (int i = 0; i < 11; i++)
        cnt += t->valid[i] && t->e4[i] >= vm_base[r + 0x36u] && t->e4[i] <= vm_base[r + 0x37u] && t->e6[i] >= vm_base[r + 0x38u] &&
               t->e6[i] <= vm_base[r + 0x39u];
    if (cnt < vm_base[r + 0x3Au]) return 3;
    const uint32_t cap = ld_be32(r + 0x3Cu);
    if (cap && t->leader_orig != cap) return 4;
    return 0;
}
/* The club's three colors and their levels for t, as func_00580434 works them out; returns the levels' sum. */
static int tc_eval(const tc_team* t, uint32_t active[3], uint8_t level[3])
{
    int n = 0;
    active[0] = active[1] = active[2] = 0;
    for (uint32_t i = 0; i < 0x47u && n < 3; i++) {
        const uint32_t c = ld_be32(TC_ORDER + 4u * i);
        if (c && c <= 70u && !tc_why_not(t, c)) active[n++] = c;
    }
    for (int k = 0; k < 3; k++) level[k] = active[k] ? 1 : 0;
    const uint32_t combos = ld_be32(ld_be32(0x008D4A9Cu + 4u * 100u) + 4u);
    for (uint32_t i = 0; i < 109u; i++) {
        const uint32_t r = combos + i * 0x10u;
        uint8_t add[3] = { 0, 0, 0 };
        int need = 0, hit = 0;
        for (uint32_t j = 0; j < 3u; j++) {
            const uint32_t c = ld_be32(r + 4u * j);
            need += c != 0;
            for (int k = 0; k < 3; k++)
                if (c == active[k] && active[k]) { hit++; add[k] = (uint8_t)(add[k] + vm_base[r + 0xCu]); }
        }
        if (hit == need)
            for (int k = 0; k < 3; k++) level[k] = (uint8_t)(level[k] + add[k]);
    }
    int sum = 0;
    for (int k = 0; k < 3; k++) { if (level[k] >= 4) level[k] = 3; sum += level[k]; }
    return sum;
}
/* The style a starter gets for a group: the highest tier; of those his own style's family, then a family his position
 * usually has, then the first. */
static uint32_t tc_pick_style(uint32_t g, uint32_t pos, uint32_t own)
{
    static const uint8_t fam_of_pos[11][3] = { {14, 15, 0}, {11, 10, 0}, {11, 10, 0}, {12, 13, 0}, {9, 8, 7}, {10, 4, 5},
                                               {10, 4, 5}, {6, 7, 8}, {4, 5, 0}, {4, 5, 0}, {2, 1, 3} };
    const uint32_t own_fam = own && own < 67u ? ld_be32(TC_STYLES + own * 0x3Cu + 4u) : 0;
    uint32_t best = 0;
    int best_score = -1;
    if (g > 60u) return 0;
    for (uint32_t i = 0; i < 16u; i++) {
        const uint32_t s = ld_be32(TC_GROUPS + g * 0x40u + 4u * i);
        if (!s || s >= 67u) continue;
        const uint32_t fam = ld_be32(TC_STYLES + s * 0x3Cu + 4u), tier = ld_be32(TC_STYLES + s * 0x3Cu + 8u);
        int score = (int)(tier > 3u ? 3u : tier) * 10;
        if (fam == own_fam) score += 5;
        for (int j = 0; j < 3 && pos < 11u; j++)
            if (fam_of_pos[pos][j] == fam) { score += 3 - j; break; }
        if (score > best_score) { best_score = score; best = s; }
    }
    return best;
}
/* One color's needs onto the eleven with the fewest new styles; pinned starters keep the style they have in t. */
static int tc_fit_best(const tc_team* t, const uint8_t* pinned, const uint32_t* g, const uint32_t* p, int n, int k, uint32_t used, int cost,
                       int* best_cost, int8_t* pick, int8_t* best_pick)
{
    if (cost >= *best_cost) return 0;
    if (k == n) { *best_cost = cost; memcpy(best_pick, pick, 5); return 1; }
    int found = 0;
    for (int pass = 0; pass < 2; pass++)   /* first the starters who need no change */
        for (int i = 0; i < 11; i++) {
            if (((used >> i) & 1u) || !t->valid[i] || !tc_pos_ok(p[k], t->pos[i])) continue;
            const int fits = tc_in_group(g[k], t->style[i]);
            if (pass == 0 ? !fits : (fits || pinned[i])) continue;
            pick[k] = (int8_t)i;
            found |= tc_fit_best(t, pinned, g, p, n, k + 1, used | 1u << i, cost + pass, best_cost, pick, best_pick);
        }
    return found;
}
/* Fits the colors, in this order, onto t (styles and the KEY player's skill): state[] per color as target_state;
 * returns the number of starters whose style changed. */
static int tc_plan(tc_team* t, const uint32_t* colors, int ncolors, uint8_t* state)
{
    uint8_t pinned[11];
    int forced = 0, leader_set = 0;
    memset(pinned, 0, sizeof pinned);
    for (int ci = 0; ci < ncolors; ci++) {
        const uint32_t c = colors[ci], r = TC_MASTER + c * 0x4Cu;
        uint32_t g[5], p[5];
        state[ci] = 0;
        if (c < 1u || c > 70u || tc_unused(c)) { state[ci] = 5; continue; }
        const int n = tc_reqs(c, g, p);
        const uint32_t cap = ld_be32(r + 0x3Cu);
        if (cap && (!t->leader_rec || (leader_set && t->leader_orig != cap))) { state[ci] = 4; continue; }
        if (n) {
            int best = 99;
            int8_t pick[5] = { -1, -1, -1, -1, -1 }, best_pick[5] = { -1, -1, -1, -1, -1 };
            if (!tc_fit_best(t, pinned, g, p, n, 0, 0, 0, &best, pick, best_pick)) { state[ci] = 1; continue; }
            for (int k = 0; k < n; k++) {
                const int i = best_pick[k];
                if (!tc_in_group(g[k], t->style[i])) { t->style[i] = tc_pick_style(g[k], t->pos[i], t->natural[i]); forced++; }
                pinned[i] = 1;
            }
        }
        if (cap) { t->leader_orig = cap; leader_set = 1; }
    }
    return forced;
}
/* The order of the colors that fits most of them (then changes fewest): out = that order, t = base with the plan. */
static int tc_plan_best(const tc_team* base, const uint32_t* set, int n, uint32_t* out, tc_team* t, uint8_t* state, int* forced)
{
    static const uint8_t perm[6][3] = { {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0} };
    const int nperm = n <= 1 ? 1 : n == 2 ? 2 : 6;
    int best_fit = -1, best_forced = 0;
    for (int pi = 0; pi < nperm; pi++) {
        uint32_t order[3] = { 0, 0, 0 };
        uint8_t st[3] = { 0, 0, 0 };
        int k = 0;
        if (n == 2) { order[0] = set[pi]; order[1] = set[1 - pi]; k = 2; }
        else
            for (; k < n; k++) order[k] = set[perm[pi][k]];
        tc_team cand = *base;
        const int f = tc_plan(&cand, order, n, st);
        int fit = 0;
        for (int j = 0; j < n; j++) fit += st[j] == 0;
        if (fit > best_fit || (fit == best_fit && f < best_forced)) {
            best_fit = fit; best_forced = f;
            *t = cand;
            memcpy(out, order, sizeof order); memcpy(state, st, 3);
            if (fit == n) break;
        }
    }
    *forced = best_forced;
    return best_fit;
}
static void tc_load_config(void)
{
    s_tc_cfg.on = 0; s_tc_cfg.mode = 0; s_tc_cfg.level = 7; s_tc_cfg.upper = 1;
    s_tc_cfg.pick[0] = s_tc_cfg.pick[1] = s_tc_cfg.pick[2] = 0;
    FILE* f = fopen(cheat_path("teamcolor.txt"), "rb");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        unsigned a = 0, b = 0, c = 0;
        cheat_trim(line);
        const char* s = cheat_skip_space(line);
        if (sscanf(s, "on\t%u", &a) == 1) s_tc_cfg.on = a != 0;
        else if (sscanf(s, "mode\t%u", &a) == 1) s_tc_cfg.mode = a == 1;
        else if (sscanf(s, "level\t%u", &a) == 1) s_tc_cfg.level = a < 1u ? 1u : a > 7u ? 7u : a;
        else if (sscanf(s, "upper\t%u", &a) == 1) s_tc_cfg.upper = a != 0;
        else if (sscanf(s, "colors\t%u\t%u\t%u", &a, &b, &c) >= 1) {
            s_tc_cfg.pick[0] = a <= 70u ? a : 0; s_tc_cfg.pick[1] = b <= 70u ? b : 0; s_tc_cfg.pick[2] = c <= 70u ? c : 0;
        }
    }
    fclose(f);
}
static void tc_save_backups(void)
{
    const char* path = cheat_path("teamcolor_backup.txt");
    if (!s_tc_nbackup) { DeleteFileA(path); return; }
    FILE* f = fopen(path, "wb");
    if (!f) return;
    for (int i = 0; i < s_tc_nbackup; i++) {
        const tc_backup* b = &s_tc_backup[i];
        fprintf(f, "%u\t%u\t", b->id, b->slot);
        for (int k = 0; k < 0x30; k++) fprintf(f, "%02X", b->natural[k]);
        fputc('\t', f);
        for (int k = 0; k < 0x30; k++) fprintf(f, "%02X", b->forced[k]);
        fputc('\n', f);
    }
    fclose(f);
}
static void tc_load_backups(void)
{
    s_tc_nbackup = 0;
    s_tc_plan_sig = 0;
    FILE* f = fopen(cheat_path("teamcolor_backup.txt"), "rb");
    if (!f) return;
    char line[320];
    while (fgets(line, sizeof line, f) && s_tc_nbackup < 24) {
        unsigned id, slot;
        char a[0x61], b[0x61];
        if (sscanf(line, "%u\t%u\t%96[0-9A-Fa-f]\t%96[0-9A-Fa-f]", &id, &slot, a, b) != 4 || strlen(a) != 0x60 || strlen(b) != 0x60 || slot > 4u) continue;
        tc_backup* e = &s_tc_backup[s_tc_nbackup++];
        e->id = id; e->slot = (uint8_t)slot;
        for (int k = 0; k < 0x30; k++) {
            unsigned x = 0, y = 0;
            sscanf(a + 2 * k, "%2x", &x); sscanf(b + 2 * k, "%2x", &y);
            e->natural[k] = (uint8_t)x; e->forced[k] = (uint8_t)y;
        }
    }
    fclose(f);
}
/* The block a player's slot holds while his style is the cheat's: his own block with the style, its skills, and the
 * original skill (0 = each as it is). */
static void tc_make_block(uint8_t* out, const uint8_t* natural, uint32_t style, uint32_t orig, uint32_t level)
{
    memcpy(out, natural, 0x30);
    if (style && style < 67u && style != tc_be32(natural)) {
        out[0] = out[1] = out[2] = 0; out[3] = (uint8_t)style;
        for (uint32_t k = 0; k < 5u; k++) {
            const uint32_t type = ld_be32(TC_STYLES + style * 0x3Cu + 0x10u + 4u * k);
            uint8_t* e = out + 8 + 8 * k;
            memset(e, 0, 8);
            if (type >= 1u && type <= 16u) { e[3] = (uint8_t)type; e[4] = (uint8_t)level; }
            else e[4] = 0xFF;
        }
    }
    if (orig) { out[4] = out[5] = 0; out[6] = (uint8_t)(orig >> 8); out[7] = (uint8_t)orig; }
}
/* Puts a player's slot to style / orig (0, 0 = his own again). 1 = the kept blocks changed (the file is written). */
static int tc_set_player(uint32_t rec, uint32_t style, uint32_t orig)
{
    const uint32_t id = ld_be32(rec);
    const uint8_t slot = vm_base[rec + 0x10Au] < 5u ? vm_base[rec + 0x10Au] : 0;
    uint8_t* cur = vm_base + rec + 0x10Cu + 0x30u * slot;
    tc_backup* b = tc_backup_of(id);
    int changed = 0;
    if (b && b->slot != slot) {   /* he uses another slot now: the old one gets its own block back */
        uint8_t* old = vm_base + rec + 0x10Cu + 0x30u * b->slot;
        if (tc_block_mine(old, b)) memcpy(old, b->natural, 0x30);
        tc_backup_drop(b); b = NULL; changed = 1;
    }
    if (!style && !orig) {
        if (!b) return changed;
        if (tc_block_mine(cur, b)) memcpy(cur, b->natural, 0x30);
        tc_backup_drop(b);
        return 1;
    }
    uint8_t want[0x30];
    tc_make_block(want, b ? b->natural : cur, style, orig, s_tc_cfg.level);
    if (!b) {
        if (memcmp(want, cur, 0x30) == 0 || s_tc_nbackup >= 24) return changed;   /* nothing to change */
        b = &s_tc_backup[s_tc_nbackup++];
        b->id = id; b->slot = slot;
        memcpy(b->natural, cur, 0x30); memcpy(b->forced, want, 0x30);
        memcpy(cur, want, 0x30);
        return 1;
    }
    if (memcmp(want, b->natural, 0x30) == 0) {   /* the plan asks nothing of him any more */
        if (tc_block_mine(cur, b)) memcpy(cur, b->natural, 0x30);
        tc_backup_drop(b);
        return 1;
    }
    if (memcmp(b->forced, want, 0x30) != 0) { memcpy(b->forced, want, 0x30); changed = 1; }
    if (memcmp(cur, want, 0x30) != 0) memcpy(cur, want, 0x30);
    return changed;
}
static uint32_t tc_sig(const tc_team* t)
{
    uint32_t h = 2166136261u;
#define TC_MIX(v) h = (h ^ (uint32_t)(v)) * 16777619u
    for (int i = 0; i < 11; i++) { TC_MIX(t->id[i]); TC_MIX(t->natural[i]); TC_MIX(t->pos[i] | t->e4[i] << 8 | t->e6[i] << 16 | t->valid[i] << 24); }
    for (int k = 0; k < 5; k++) TC_MIX(t->tac[k]);
    TC_MIX(t->leader_id); TC_MIX(t->leader_natural);
    TC_MIX(s_tc_cfg.on | s_tc_cfg.mode << 1 | s_tc_cfg.upper << 2); TC_MIX(s_tc_cfg.pick[0]); TC_MIX(s_tc_cfg.pick[1]); TC_MIX(s_tc_cfg.pick[2]);
#undef TC_MIX
    return h ? h : 1u;
}
static long tc_score(const tc_team* t, const uint32_t plain[3], int forced)
{
    uint32_t act[3];
    uint8_t lev[3];
    long score = tc_eval(t, act, lev) * 10000L - forced;
    for (int k = 0; k < 3; k++)
        for (int j = 0; j < 3; j++) score += act[k] && act[k] == plain[j] ? 100 : 0;
    return score;
}
/* 자동: the best set of up to three line colors for the eleven and the tactics as they are (out in fitting order). */
static int tc_auto(const tc_team* base, const uint32_t plain[3], uint32_t out[3])
{
    uint32_t cand[47];
    int ncand = 0, nout = 0;
    for (uint32_t c = 1; c <= 47u; c++) {
        const uint32_t r = TC_MASTER + c * 0x4Cu;
        uint32_t g[5], p[5];
        int ok = !tc_unused(c) && ld_be32(r + 0x40u) == 0 && tc_reqs(c, g, p) > 0 && vm_base[r + 0x3Au] == 0;
        for (uint32_t k = 0; k < 5u && ok; k++)
            ok = base->tac[k] >= vm_base[r + 0x2Cu + 2u * k] && base->tac[k] <= vm_base[r + 0x2Du + 2u * k];
        if (ok) cand[ncand++] = c;
    }
    long best = tc_score(base, plain, 0);   /* nothing changed */
    out[0] = out[1] = out[2] = 0;
    for (int a = 0; a < ncand; a++)
        for (int b = a; b < ncand; b++)
            for (int c = b; c < ncand; c++) {
                uint32_t set[3], order[3];
                uint8_t state[3];
                int n = 0, forced = 0;
                if (b == a && c != b) continue;   /* {a, a, c} is {a, c}: taken at b = c */
                set[n++] = cand[a];
                if (b != a) set[n++] = cand[b];
                if (c != b) set[n++] = cand[c];
                tc_team t;
                if (tc_plan_best(base, set, n, order, &t, state, &forced) != n) continue;
                const long score = tc_score(&t, plain, forced);
                if (score > best) { best = score; nout = n; memcpy(out, order, sizeof order); }
            }
    return nout;
}
static void tc_publish(saka_tc_status* st)
{
    for (uint32_t c = 1; c <= 70u && st->loaded; c++) {
        memcpy(st->color_tac[c], vm_base + TC_MASTER + c * 0x4Cu + 0x2Cu, 10);
        st->color_unused[c] = (uint8_t)tc_unused(c);
    }
    EnterCriticalSection(&s_tc_lock);
    s_tc_status = *st;
    LeaveCriticalSection(&s_tc_lock);
}
static void cheats_teamcolor_apply(int loaded)
{
    static int had_on;
    static saka_tc_status st;   /* the cheat thread's own (it is large) */
    if (!s_tc_lock_ready) { InitializeCriticalSection(&s_tc_lock); InterlockedExchange(&s_tc_lock_ready, 1); }
    memset(&st, 0, sizeof st);
    st.on = s_tc_cfg.on; st.mode = s_tc_cfg.mode; st.level = s_tc_cfg.level; st.upper = s_tc_cfg.upper;
    memcpy(st.pick, s_tc_cfg.pick, sizeof st.pick);
    tc_team team;
    if (!loaded || !tc_tables_ok() || tc_read(&team) < 7) { tc_publish(&st); return; }
    st.loaded = 1;
    memcpy(st.tac, team.tac, 5);
    tc_eval(&team, st.plain, st.plain_level);
    int dirty = 0;
    if (!s_tc_cfg.on) {
        if (s_tc_nbackup || had_on) {   /* everyone gets his own slot back */
            for (uint32_t k = 0; k < 23u && s_tc_nbackup; k++) {
                const uint32_t rec = CHEAT_ROSTER + k * CHEAT_RECORD;
                if (tc_backup_of(ld_be32(rec))) dirty |= tc_set_player(rec, 0, 0);
            }
            if (s_tc_nbackup) { s_tc_nbackup = 0; dirty = 1; }   /* players no longer in the club */
            if (dirty) { tc_save_backups(); fprintf(stderr, "[cheats] team color fit off: the players' own styles are back\n"); }
            had_on = 0;
            s_tc_plan_sig = 0;
        }
        memcpy(st.active, st.plain, sizeof st.active); memcpy(st.active_level, st.plain_level, sizeof st.active_level);
        tc_publish(&st);
        return;
    }
    had_on = 1;
    const uint32_t sig = tc_sig(&team);
    if (sig != s_tc_plan_sig) {   /* the eleven, the tactics or the settings changed: plan again */
        uint32_t set[3] = { 0, 0, 0 }, targets[3] = { 0, 0, 0 };
        int n = 0, forced = 0;
        tc_team t = team;
        memset(s_tc_plan_state, 0, sizeof s_tc_plan_state);
        if (s_tc_cfg.mode == 0) {
            n = tc_auto(&team, st.plain, set);
            if (n) tc_plan_best(&team, set, n, targets, &t, s_tc_plan_state, &forced);
        } else {
            for (int k = 0; k < 3; k++) {
                int dup = 0;
                for (int j = 0; j < n; j++) dup |= set[j] == s_tc_cfg.pick[k];
                if (s_tc_cfg.pick[k] && !dup) set[n++] = s_tc_cfg.pick[k];
            }
            if (n) tc_plan_best(&team, set, n, targets, &t, s_tc_plan_state, &forced);
        }
        uint32_t act[3];
        uint8_t lev[3];
        tc_eval(&t, act, lev);
        for (int k = 0; k < n; k++) {   /* what the styles cannot give: tactics, policies; or stronger colors first */
            if (s_tc_plan_state[k]) continue;
            const int why = tc_why_not(&t, targets[k]);
            if (why) s_tc_plan_state[k] = (uint8_t)why;
            else if (act[0] != targets[k] && act[1] != targets[k] && act[2] != targets[k]) s_tc_plan_state[k] = 6;
        }
        memcpy(s_tc_plan_style, t.style, sizeof s_tc_plan_style);
        memcpy(s_tc_plan_ids, t.id, sizeof s_tc_plan_ids);
        s_tc_plan_leader = t.leader_orig != team.leader_natural ? t.leader_orig : 0;
        s_tc_plan_leader_id = t.leader_id;
        memcpy(s_tc_plan_target, targets, sizeof targets);
        s_tc_plan_ntarget = n;
        s_tc_plan_sig = sig;
        fprintf(stderr, "[cheats] team color fit (%s): colors %u %u %u (states %u %u %u), %d styles changed -> %u Lv%u, %u Lv%u, %u Lv%u (the players' own: %u Lv%u, %u Lv%u, %u Lv%u)\n",
                s_tc_cfg.mode ? "picked" : "auto", targets[0], targets[1], targets[2], s_tc_plan_state[0], s_tc_plan_state[1],
                s_tc_plan_state[2], forced, act[0], lev[0], act[1], lev[1], act[2], lev[2], st.plain[0], st.plain_level[0], st.plain[1],
                st.plain_level[1], st.plain[2], st.plain_level[2]);
    }
    /* write the plan: the starters it changes and the KEY player; everyone else with a kept slot gets it back */
    for (uint32_t k = 0; k < 23u; k++) {
        const uint32_t rec = CHEAT_ROSTER + k * CHEAT_RECORD, id = ld_be32(rec);
        if (!tc_player_ok(id)) continue;
        uint32_t style = 0, orig = 0, natural = 0;
        for (int i = 0; i < 11; i++)
            if (team.valid[i] && team.rec[i] == rec && s_tc_plan_ids[i] == id && s_tc_plan_style[i] != team.natural[i]) {
                style = s_tc_plan_style[i]; natural = team.natural[i];
            }
        if (rec == team.leader_rec && id == s_tc_plan_leader_id) orig = s_tc_plan_leader;
        if (style || orig || tc_backup_of(id)) dirty |= tc_set_player(rec, style, orig);
        if (style && st.nforced < 12) {
            st.forced[st.nforced].id = id; st.forced[st.nforced].style = style; st.forced[st.nforced].natural = natural;
            st.nforced++;
        }
    }
    for (int i = 0; i < s_tc_nbackup;) {   /* kept slots of players who left the club */
        int here = 0;
        for (uint32_t k = 0; k < 23u && !here; k++) here = ld_be32(CHEAT_ROSTER + k * CHEAT_RECORD) == s_tc_backup[i].id;
        if (here) i++;
        else { tc_backup_drop(&s_tc_backup[i]); dirty = 1; }
    }
    if (dirty) { tc_save_backups(); s_cheat_hook_calls[30]++; }
    {   /* what the game sees now: the slots as they are */
        tc_team now = team;
        for (int i = 0; i < 11; i++)
            if (now.valid[i]) now.style[i] = ld_be32(tc_slot_of(now.rec[i]));
        if (now.leader_rec) now.leader_orig = ld_be32(tc_slot_of(now.leader_rec) + 4u);
        tc_eval(&now, st.active, st.active_level);
    }
    memcpy(st.target, s_tc_plan_target, sizeof st.target);
    memcpy(st.target_state, s_tc_plan_state, sizeof st.target_state);
    st.ntarget = s_tc_plan_ntarget;
    st.leader_id = s_tc_plan_leader ? s_tc_plan_leader_id : 0;
    st.leader_orig = s_tc_plan_leader;
    tc_publish(&st);
}
void saka_teamcolor_status(saka_tc_status* out)   /* for the F2 팀컬러 tab */
{
    memset(out, 0, sizeof *out);
    if (!s_tc_lock_ready) return;
    EnterCriticalSection(&s_tc_lock);
    *out = s_tc_status;
    LeaveCriticalSection(&s_tc_lock);
}

/* SAKA_CARD_LOG=1 (diagnostic, 2026-11-09, red cards): the match result 0x00F97B24 (func_004A206C's param 2), two
 * teams of 0x37C from +0x604 (club ID), 23 players of 0x24 from +0x60C (ID), +0x620 goals, +0x625 yellows, +0x626 red:
 * every change is logged with the match tick, to find when and where the game writes them. */
static void card_log_tick(void)
{
    static int on = -1;
    static uint8_t last[2][23][3];
    if (on < 0) { const char* e = getenv("SAKA_CARD_LOG"); on = e && *e == '1'; }
    if (!on || !cheat_committed(0x00F97B24u, 0x604u + 2u * 0x37Cu)) return;
    for (uint32_t tm = 0; tm < 2; tm++)
        for (uint32_t s = 0; s < 23; s++) {
            const uint32_t e = 0x00F97B24u + tm * 0x37Cu + s * 0x24u;
            const uint8_t now[3] = { vm_base[e + 0x620u], vm_base[e + 0x625u], vm_base[e + 0x626u] };
            if (memcmp(now, last[tm][s], 3) == 0) continue;
            memcpy(last[tm][s], now, 3);
            fprintf(stderr, "[cards] match tick %u team %u club %u slot %u id %u goals %u yellows %u red %u (entry 0x%08X)\n",
                    s_sim_ticks, tm, ld_be32(0x00F97B24u + 0x604u + tm * 0x37Cu), s, ld_be32(e + 0x60Cu), now[0], now[1], now[2], e);
        }
}

static DWORD WINAPI cheat_thread(LPVOID arg)
{
    (void)arg;
    for (unsigned tick = 0;; tick++) {
        Sleep(100);
        if (!vm_base) continue;
        card_log_tick();
        {   /* SAKA_SUPERPLAY_AUTO=<seconds> (tests): F6 by itself every so many seconds */
            extern int saka_superplay_arm(void);
            static int every = -1;
            if (every < 0) { const char* e = getenv("SAKA_SUPERPLAY_AUTO"); every = e ? atoi(e) : 0; }
            if (every > 0 && tick % ((unsigned)every * 10u) == 0 && cheat_game_loaded()) saka_superplay_arm();
        }
        if (tick % 10 == 0) {
            cheat_profile_tick();
            const int a = cheat_file_changed("cheats/BLJM61064.txt", &s_cheat_time[0]);
            const int b = cheat_file_changed(cheat_path("enabled.txt"), &s_cheat_time[1]);
            const int c = cheat_file_changed(cheat_path("values.txt"), &s_cheat_time[2]);
            if (a || b || c) {
                cheats_reload();
                int on = 0, once = 0;
                for (int i = 0; i < s_ncheat_defs; i++) { on += s_cheat_defs[i].on; once += s_cheat_defs[i].on && s_cheat_defs[i].once; }
                if (s_ncheat_defs)
                    fprintf(stderr, "[cheats] %d cheats, %d on (%d once); %d codes, %d skipped (not data writes)\n",
                            s_ncheat_defs, on, once, s_ncheat_codes, s_cheat_skipped);
            }
            if (cheat_file_changed(cheat_path("players.txt"), &s_cheat_time[3])) {
                cheats_players_load();
                fprintf(stderr, "[cheats] %d player edits\n", s_ncheat_players);
            }
            if (cheat_file_changed(cheat_path("lists.txt"), &s_cheat_time[4])) {
                cheats_lists_load();
                fprintf(stderr, "[cheats] %d list edits\n", s_ncheat_lists);
            }
            if (cheat_file_changed(cheat_path("training.txt"), &s_cheat_time[5])) {
                cheats_training_load();
                fprintf(stderr, "[cheats] %d training slot edits\n", s_ncheat_train);
            }
            if (cheat_file_changed(cheat_path("staff.txt"), &s_cheat_time[6])) {
                cheats_staff_load();
                fprintf(stderr, "[cheats] %d staff edits\n", s_ncheat_staff);
            }
            {   /* the team color fit: its kept slots belong to the save, its settings to the file */
                static LONG tc_slot = -100;
                if (tc_slot != s_save_slot) { tc_slot = s_save_slot; tc_load_backups(); }
                if (cheat_file_changed(cheat_path("teamcolor.txt"), &s_cheat_time[7])) {
                    tc_load_config();
                    fprintf(stderr, "[cheats] team color fit %s (%s; colors %u %u %u; skill level %u; upper colors %s)\n", s_tc_cfg.on ? "on" : "off",
                            s_tc_cfg.mode ? "picked" : "auto", s_tc_cfg.pick[0], s_tc_cfg.pick[1], s_tc_cfg.pick[2], s_tc_cfg.level,
                            s_tc_cfg.upper ? "on" : "off");
                }
            }
        }
        cheats_apply();
        if (s_ncheat_players || s_ncheat_lists || s_ncheat_train || s_ncheat_staff || tick % 20 == 0) {
            const int loaded = cheat_game_loaded();
            cheats_players_apply(loaded);
            cheats_lists_apply(loaded);
            cheats_training_apply(loaded);
            cheats_staff_apply(loaded);
            if (tick % 20 == 0) cheats_export(loaded);
        }
        if (s_tc_cfg.on || s_tc_nbackup || tick % 10 == 0) cheats_teamcolor_apply(cheat_game_loaded());   /* after the player edits */
        static long shown[32];
        if (tick % 100 == 0 && memcmp(shown, (const void*)s_cheat_hook_calls, sizeof shown) != 0) {
            for (int i = 0; i < 32; i++) shown[i] = s_cheat_hook_calls[i];
            fprintf(stderr, "[cheats] code switches used: youth visit %ld, trade chance %ld, foreign slots %ld, "
                            "hizo roll %ld, facility check %ld, facility 1 week %ld, edit points %ld, substitution %ld, "
                            "interview %ld, course any %ld, slot keep %ld, training x %ld, special x %ld, awakening x %ld, "
                            "negotiation %ld, full house %ld, satisfaction %ld, suspensions lifted %ld, team skill uses given back %ld, cards removed %ld, per-player x %ld, "
                            "training reports started %ld, super plays %ld\n",
                    shown[1], shown[2], shown[3], shown[4], shown[5], shown[6], shown[7], shown[8], shown[9],
                    shown[10], shown[11], shown[12], shown[13], shown[14], shown[15], shown[16], shown[17], shown[18], shown[19], shown[20], shown[31],
                    shown[29], shown[23]);
        }
    }
    return 0;
}

/* cellSaveData's view of the save files (ps3recomp libs/system/cellSaveData.c): MAINnn.DAT read = a load, written = a
 * save (CELL_SAVEDATA_FILEOP_WRITE 1, WRITE_NOTRUNC 3). MAINIDX.DAT and SYSTEM.DAT are not saves. */
extern void (*g_savedata_file_hook)(const char* dir, const char* file, unsigned op, int result);
static void saka_savedata_file(const char* dir, const char* file, unsigned op, int result)
{
    unsigned n = 100;
    char tail[8] = "";
    if (result < 0 || !dir || !file || strncmp(dir, "BLJM61064", 9) != 0) return;
    if (sscanf(file, "MAIN%2u%7s", &n, tail) != 2 || n > 99 || strcmp(tail, ".DAT") != 0) return;
    if (op == 0) InterlockedExchange(&s_save_read, (LONG)n);
    else if (op == 1 || op == 3) InterlockedExchange(&s_save_written, (LONG)n);
}
/* main.cpp's file-open hook: the title's menu file was opened (the title or its menu is showing). */
void saka_cheats_title(void)
{
    extern void saka_edit_title(void);
    saka_edit_title();
    InterlockedExchange(&s_save_title, 1);
    InterlockedExchange(&s_save_read, -2);
    InterlockedExchange(&s_save_title_seen, 1);
}
/* For the F2 menu (saka_overlay.cpp): the folder of the settings in use ("cheats/" or "cheats/saves/MAIN00/"), its
 * description, and a generation that changes with it. */
long saka_cheat_profile(char* dir, size_t dir_n, char* desc, size_t desc_n)
{
    const LONG slot = s_save_slot;
    if (dir) cheat_profile_dir(slot, dir, dir_n);
    if (desc) cheat_profile_desc(slot, desc, desc_n);
    return (long)s_save_gen;
}

static void saka_cheats_start(void)
{
    extern void saka_edit_install(void);
    saka_edit_install();
    const char* e = getenv("SAKA_CHEATS");
    if (e && *e == '0') return;
    g_savedata_file_hook = saka_savedata_file;
    HANDLE t = CreateThread(NULL, 0, cheat_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
#else
static void saka_cheats_start(void) {}
#endif

/* ---- Code switches: the Vita ARM code patches of PCSG00198.psv, moved ---------
 * Found in the PS3 EBOOT with REA's Ghidra (NOTES.md "Cheats", code switches); each
 * hook is called from the recompiled function named and acts only while its $F0nn
 * code is on.
 * 1 WAKATE MUST FIND: func_0053557C sets up the scout's visit to a young player:
 *   the chance (byte r29+0x29F = 0x00F8EEE3, from the area table) is rolled up to
 *   twice against rand(1,100). Hook at 0x0053570C, after the chance is stored and
 *   before the roll (which reads it from memory): chance 100.
 * 2 TRADE MUST: func_0051F148 returns the reasons a club refuses to deal for a
 *   player (bits 0-3 club and country, 4-10 flags from the player's record);
 *   func_0051F2B4 the acceptance chance, rolled as rand % 100 < chance
 *   (func_00520338). Hooks at their epilogues: no player-record reasons; chance 100.
 * 3 FOREIGN PLAYER FRAME OVER: func_00584388 writes a club's free foreign-player
 *   slots and free special (Asian) slots (limit - count over the 23 roster slots).
 *   For the user's club (func_0049578C != 0, noted after that call) both become at
 *   least 23. Other clubs keep their limits. */
void saka_youth_find_hook(ppu_context* ctx)
{
    if (s_cheat_switch[1]) { vm_base[(uint32_t)ctx->gpr[29] + 0x29Fu] = 100; s_cheat_hook_calls[1]++; }
}
void saka_trade_reasons_hook(ppu_context* ctx)
{
    if (s_cheat_switch[2]) ctx->gpr[3] &= 0xFull;
}
void saka_trade_chance_hook(ppu_context* ctx)
{
    if (s_cheat_switch[2]) { ctx->gpr[3] = 100; s_cheat_hook_calls[2]++; }
}
static int s_foreign_mine;
void saka_foreign_note(ppu_context* ctx)
{
    s_foreign_mine = (int32_t)ctx->gpr[3] != 0;
}
void saka_foreign_hook(ppu_context* ctx)
{
    if (!s_cheat_switch[3] || !s_foreign_mine) return;
    s_cheat_hook_calls[3]++;
    for (int k = 0; k < 2; k++) {
        const uint32_t ea = (uint32_t)ctx->gpr[29 + k];
        if ((int32_t)ld_be32(ea) < 23) st_be32(ea, 23);
    }
}
/* 3 also the limits themselves (2026-11-13, owner: "베켄바우어를 영입하고 다음달 되니까 여기서 방출을 해도 진행이 안돼.
 *   외국인 문제가 여전히 제대로 작동하지 않는것 같아"): at the month start the personnel menu (func_0037D2F4) asks to
 *   release max(players - 23, foreigners - limit) players (menu_jinji_information_text 1300 / 1301: あと○人自由契約に
 *   してください。その内、外国籍選手を○人…), with the limits from func_00583EAC(&foreign, &special, club) -- not from
 *   func_00584388, so the free-slot hook above did not reach it. Hook at func_00583EAC's entry (the club, r5) and exit
 *   (r30 / r31 point at the two limits): for the user's club both become at least 23, so only a squad over 23 must be
 *   cut. The same limits serve signings (func_00584388) and youth promotion (11101 外国籍選手枠に空きがないため). */
static uint32_t s_limit_club;
void saka_foreign_limit_entry(ppu_context* ctx)
{
    s_limit_club = (uint32_t)ctx->gpr[5];
}
void saka_foreign_limit_hook(ppu_context* ctx)
{
    const uint32_t club = s_limit_club;
    s_limit_club = 0;
    if (!s_cheat_switch[3] || !club || !(club == 0x100u || club == ld_be32(0x00EE28ACu) || club == ld_be32(0x00EE2934u))) return;
    for (int k = 0; k < 2; k++) {
        const uint32_t ea = (uint32_t)ctx->gpr[30 + k];
        if (cheat_committed(ea, 4u) && (int32_t)ld_be32(ea) < 23) { st_be32(ea, 23); s_cheat_hook_calls[3]++; }
    }
}
/* 3 also the lineup (2026-11-09, owner: "외국인 선수 정원초과도 제발 해결해주고"; the formation screen said 외국인 선수
 *   정원 초과): func_005864DC counts the foreign players among the starting 11 and the 7 substitutes against the league's
 *   rules (func_0057F700: +4 foreign, +8 Asian, +0xC special) and returns 0 when there are too many; its only caller
 *   func_00633E50 then returns 3, and the team menus (before a match FUN_0038A7E0 / FUN_00432FCC, the team's
 *   FUN_0042973C) show 外国籍選手枠をオーバーしています and do not take the lineup. Hook at its return: 1. Those menus
 *   are the user's club's only. */
void saka_foreign_lineup_hook(ppu_context* ctx)
{
    if (s_cheat_switch[3] && (int32_t)ctx->gpr[3] == 0) { ctx->gpr[3] = 1; s_cheat_hook_calls[3]++; }
}
/* 17 SATISFACTION S (2026-11-09, owner: "경기 후 만족도도 S로 잘 작동하다가 잘 먹히지 않에"; there was no cheat for it,
 *   the S grades came from the game): func_00541640 (from func_00576DA0 after the user's match; the result at
 *   0x00F99290) grades the supporters' survey 1-6 (E D C B A S): match content (func_00541270, r31), players (the best
 *   player's rating, r29), facilities (the built facilities' satisfaction, r30), promotion (the promotions run, r27).
 *   The overall grade is their average, and the supporters gained are computed from them. Hook after func_0053D558,
 *   before the grades are used: all four 6, so the overall is S and the supporters gained are the S ones. */
void saka_satisfaction_hook(ppu_context* ctx)
{
    if (!s_cheat_switch[17]) return;
    ctx->gpr[27] = ctx->gpr[29] = ctx->gpr[30] = ctx->gpr[31] = 6;
    s_cheat_hook_calls[17]++;
}

/* 18 NO SUSPENSION (2026-11-09, owner: "레드카드를 받거나 옐로우가 누적되어서 출장이 불가능해지지 않도록 해줘"):
 *   func_004A206C records each match per competition and player, 5 bytes (games, goals, assists, rating, cards) in the
 *   competition records (league 0x00F0FB78 + c x 0x2EA4, the others from 0x00F359CC; +0x22E8 + team x 0x73 +
 *   slot x 5). The cards byte: 0x80 = suspended for the next match (a red card, or two yellows in one match), low 7 bits
 *   = the yellows collected; at the competition's limit (2 or 4) the player misses the next match.
 *   func_0049D020 (who can play: +0xFC of the team's player copies, 2 starter / 1 substitute / 0 not) asks
 *   func_00572524(competition, club, slot) for that record. Hook at its return: for the user's club (func_0049578C:
 *   0x100 or the club ID at 0x00EE28AC, noted at the entry) the cards byte is cleared, so nobody of the club is
 *   suspended and no yellows collect; suspensions already given are lifted the same way. 0x00EE1700 is the game's
 *   empty record for a player it does not find: left alone. */
static int s_cards_mine;
void saka_cards_entry_hook(ppu_context* ctx)   /* func_00572524 entry: r4 = club */
{
    const uint32_t club = (uint32_t)ctx->gpr[4];
    s_cards_mine = club != 0 && (club == 0x100u || club == ld_be32(0x00EE28ACu));
}
void saka_cards_hook(ppu_context* ctx)   /* func_00572524 return: r3 = the 5-byte record */
{
    const uint32_t rec = (uint32_t)ctx->gpr[3];
    if (!s_cheat_switch[18] || !s_cards_mine || !rec || rec == 0x00EE1700u) return;
    if (vm_base[rec + 4]) { vm_base[rec + 4] = 0; s_cheat_hook_calls[18]++; }
}

/* 19 TEAM SKILL UNLIMITED (2026-11-09, owner: "게이지 무한 유지와 시도개수 무한"; REA Ghidra and bench dumps):
 *   the match's 試合指示 manager keeps per team t (0 home, 1 away; 0x28 apart) the uses left at +0x2C914 (set at kick-off
 *   by func_002361EC from the team's maximum: the user's is u32 0x00EE6CF0 capped at 6), and per instruction slot a state
 *   at +0x2C918 (4 just used, 1 running, 3 over) and its time at +0x2C91C (func_0023A158 copies the match engine's).
 *   Both ways of using one take one use: func_00239E78 and func_0023A6E8 (state 4). Hook after each store: for the
 *   user's team the use is given back. Team t is the user's when the match team object 0x00F8F158 + t x 0x43C4 starts
 *   with the user's club (0x100 or u32 0x00EE28AC, as func_0049578C); the other team keeps its count. The effect time
 *   is the team object's +0x43C0 (minutes added, u32 0x00EE6CF4: MATCH ORDERS sets 90). */
/* Which engine team is the user's (2026-11-13, owner: "횟수가 6에서 2로 줄었어 무한이 안돼"): the 試合指示 manager's and the
 * play sequence's team t is the match engine's team entry t (func_00208AE8: match + 0x334 + t x 0x126C), whose order is
 * not that of the match team objects 0x00F8F158 / 0x00F9351C (they agreed in a home match, not in an away one: no use
 * was given back). The entry's +4 is the team's data (func_002DDD14 reads its count at +0x58), whose +8 points at its
 * match team object, whose +0 is the club (bench: t0 data +8 = 0x00F8F158, club 0x100; t1 0x00F9351C, club 0x21).
 * -1 = not known (the old check). */
static volatile int s_engine_user[2] = {-1, -1};
static void superplay_kickoff(void);
void saka_team_skill_kickoff_hook(uint32_t entry, uint32_t t)
{
    if (t > 1u || !cheat_committed(entry, 8u)) return;
    const uint32_t data = ld_be32(entry + 4u);
    const uint32_t obj = cheat_committed(data, 0xCu) ? ld_be32(data + 8u) : 0;
    const uint32_t club = obj && cheat_committed(obj, 4u) ? ld_be32(obj) : 0;
    s_engine_user[t] = club ? (club == 0x100u || club == ld_be32(0x00EE28ACu)) : -1;
    if (t == 0) superplay_kickoff();   /* a new kick-off: an F6 from before it is forgotten */
    fprintf(stderr, "[cheats] kick-off: engine team %u data 0x%08X team object 0x%08X club 0x%X -> %s\n", t, data, obj, club,
            s_engine_user[t] > 0 ? "user" : s_engine_user[t] == 0 ? "other" : "not known");
}
static int match_team_is_user(uint32_t t)
{
    if (t > 1u) return 0;
    if (s_engine_user[t] >= 0) return s_engine_user[t];
    const uint32_t club = ld_be32(0x00F8F158u + t * 0x43C4u);
    return club != 0 && (club == 0x100u || club == ld_be32(0x00EE28ACu));
}
void saka_team_skill_hook(uint32_t count_ea, uint32_t team)
{
    if (!s_cheat_switch[19] || !match_team_is_user(team)) return;
    st_be32(count_ea, ld_be32(count_ea) + 1u);
    s_cheat_hook_calls[19]++;
}

/* 20 NO CARDS (2026-11-09, owner: "플레이어의 팀에 옐로우나 레드카드가 경기중에 나오지 않게"; REA Ghidra, NOTES.md
 *   "Cards found statically"): the match builds each play record (0x38D8 bytes) in func_00205220 (or func_00205568)
 *   from the previous one; func_0028EC08 → func_0028CA48 writes the play's event at record+0x28. A foul is type 9/10
 *   (11 a variant): +0x2C period, +0x58 the fouler's side, +0x5C the fouler, +0x48/+0x4C the victim, +0x60 the
 *   referee's code (taken from the previous play's pending decision): code % 3 = 0 no card, 1 yellow (a second one
 *   is red), 2 red; code / 3 = the kind of foul (3-11 also hurt the victim). Every reader comes after this point:
 *   func_00204900 (restart), func_002DCC48 (cards on the player, status 5 = sent off), func_002D730C (match stats →
 *   the result's +0x19 / +0x1A), the card scene. Hook right after the record is built, before func_00204900: on the
 *   user's side the code loses its card (code - code % 3), so the foul and its free kick / injury stay.
 *   Side → team as func_002CDA60: core table 3 = u32 [seq+8] + 0xC, X = u32 [[core3 + 0x10] + 0x50] = team 0's side in
 *   periods 0/2/4, the other side in 1/3/5. */
void saka_no_cards_hook(uint32_t rec, uint32_t seq)
{
    static long plays, fouls;
    if (!rec || !seq) return;
    plays++;
    const uint32_t ev = rec + 0x28u;
    const int32_t type = (int32_t)ld_be32(ev);
    if (type < 9 || type > 11) return;
    const int32_t code = (int32_t)ld_be32(ev + 0x38u);
    if (++fouls <= 64 && (code <= 0 || code % 3 == 0))
        fprintf(stderr, "[nocards] foul type %d code %d (no card) side %d (plays %ld, fouls %ld)\n", type, code,
                (int32_t)ld_be32(ev + 0x30u), plays, fouls);
    if (code <= 0 || code > 11 || code % 3 == 0) return;
    const int32_t side = (int32_t)ld_be32(ev + 0x30u), period = (int32_t)ld_be32(ev + 4u);
    const uint32_t db = ld_be32(seq + 8u);
    const uint32_t core3 = db ? ld_be32(db + 0xCu) : 0;
    const uint32_t sides = core3 ? ld_be32(core3 + 0x10u) : 0;
    if (!sides || (side != 0 && side != 1) || period < 0 || period > 5) return;
    const uint32_t x = ld_be32(sides + 0x50u);
    const uint32_t side0 = (period & 1) ? (uint32_t)(x == 0) : x;
    const uint32_t team = (uint32_t)side == side0 ? 0u : 1u;
    const int mine = match_team_is_user(team);
    fprintf(stderr, "[nocards] foul type %d code %d (%s) side %d period %d -> team %u%s%s\n", type, code,
            code % 3 == 1 ? "yellow" : "red", side, period, team, mine ? " (ours)" : "",
            mine && s_cheat_switch[20] ? ": card removed" : "");
    if (!s_cheat_switch[20] || !mine) return;
    st_be32(ev + 0x38u, (uint32_t)(code - code % 3));
    s_cheat_hook_calls[20]++;
}

/* 4 HIZO TRAINING SUCCESS (the port's own): func_00567A2C (new trait) and func_00567B74 (new skill) roll
 *   rand(0,99) against the cumulative chances of the training's three outcomes; a roll above them all gives nothing
 *   (func_005177A0, the weekly update, calls them when a 秘蔵っ子 training ends). Hook after the roll: 1, so the
 *   first outcome with a chance is taken. It can still come out empty when the player already has that outcome.
 *   SP training (特別練習) has no such roll: its weekly ability experience and the awakening points at the end
 *   follow from the player (func_00515B80), so there is nothing to force there.
 * 5 FACILITY ANY (the port's own): func_0054F92C tells the menu and func_0054220C whether a clubhouse facility
 *   (27 slots at 0x00EE7C06, 6 bytes: id, -, u16 weeks left) can be built: 0 built already, 1 over the clubhouse's
 *   capacity (the facilities' sizes, master table 0x2B +0xA, against the clubhouse level's capacity: the "cost"),
 *   2 not enough money, 3 clubhouse level too low, 4/5 can. Hook at its return: 1 and 3 become 4.
 * 6 FACILITY 1 WEEK (the port's own): func_0054220C stores the build time (master +9, x2 weeks) at slot +2. Hook
 *   after the store: 1 week. The stadium's func_00542100 does the same (slot in r27 too; 2026-11-09). */
/* 9 INTERVIEW (면담 무제한, 2026-11-09, owner: "면담도 무제한이 아니었어"): interviews are limited per month (1 + GM
 *   skills 0x73/0x74). func_00590B58(mask) tests the month's flags u32 0x00EECE24 (bits 1/0x20/0x40/0x80 = interviews,
 *   set by func_003F9DF0 when a player is chosen, cleared monthly by func_0058B004); the schedule asks it when it opens
 *   (func_003F6338) and on a choice (func_003F73AC). The cheat's data code clears the bits ten times a second; this hook
 *   makes the test itself read them as clear, so nothing depends on that timing. r4 = the flags just loaded. */
void saka_interview_hook(ppu_context* ctx)
{
    if (s_cheat_switch[9]) { ctx->gpr[4] &= ~0xE1ull; s_cheat_hook_calls[9]++; }
}

/* Training (2026-11-09, owner: "비장의 아이 훈련도 아무에게나 걸어놓고 ... 내가 원할때 까지 슬롯에 고정", "특별훈련
 * 비장의아이훈련의 효과에 대한 배수설정", "일반적인 훈련의 배수효과"; REA Ghidra, re/decomp_all.c):
 * - Slots: special training (SP) 3 x 0xC at 0x00EE83A8 (+0 player ID, +4 course, +8 u8 weeks left); 秘蔵っ子 3 x 0x20
 *   at 0x00EE83CC (+0 ID, +4 kind 1 能力覚醒 / 0 個性強化, +8 course, +0xC u8 weeks left). The weekly update
 *   func_005177A0 counts the weeks down; an SP slot gives experience every week and ends in func_00515B80; a 秘蔵っ子
 *   slot gives its effect only when it ends, then is cleared (func_0050CD18 counts the finished plans).
 * - Choosing: no age limit; only a badly injured player (+0x100 == 2) or one abroad (+0xE8 & 0x20000) is left out
 *   (func_0006D858). The courses grey out when func_005122DC says the player does not meet them (play style +0x28,
 *   skill type/level +0x2C/+0x30, trait +0x34, awakening level +0x38 of the course record).
 * 10 COURSE ANY: func_005122DC returns 1 (met); a 能力覚醒 course also needs the awakening-ready bit (+0xE8 0x100000)
 *    when it ends, read as set.
 * 11 SLOT KEEP: func_005177A0 is snapshotted before its slot passes; when a plan ends and the game clears its slot,
 *    the slot is put back with the course's full time (SP: master u8 0x00E18A3C + course x 0x18 + 0x14, x2 weeks;
 *    能力覚醒 0x00E154A8 + course x 0x3C + 4, 個性強化 0x00E17238 + course x 0x3C + 4, & 0x7F, x2). A plan the user
 *    takes off in the menu, or one an injury clears (func_00514FB0), is not put back. With "1턴 완료" the effect comes
 *    every week. The slots stay 3: the save has room for 3 of each, nothing more.
 * 12 TRAINING x: normal training's experience (func_00515548, before func_00577318: r5 and r20, the week's total).
 * 13 SPECIAL x: SP weekly experience (r5, r24), 個性強化 end experience (r5), 能力覚醒 limit rise (func_00512910 r6;
 *    the game caps limits at 100), SP end awakening points (r4).
 * 14 AWAKENING x: every awakening-point gain of a player of the user's club (func_00568B08 entry, r3 player, r4). */
static uint8_t s_keep_sp[3][0xC], s_keep_hizo[3][0x20];
static int s_keep_valid;

static uint32_t cheat_multiplier(int sw)
{
    const uint32_t m = s_cheat_switch_value[sw];
    return m < 1 ? 1 : m > 100 ? 100 : m;
}
static uint64_t cheat_scaled(uint64_t reg, int sw, int32_t cap)
{
    const int32_t v = (int32_t)reg;
    if (v <= 0) return reg;
    const int64_t m = (int64_t)v * cheat_multiplier(sw);
    return (uint64_t)(int64_t)(int32_t)(m > cap ? cap : m);
}
/* The record (the club's 23 or the youth team's 15) that addr points into, 0 for any other address. */
static uint32_t cheat_record_of(uint32_t addr)
{
    if (addr >= CHEAT_ROSTER && addr < CHEAT_ROSTER + 23u * CHEAT_RECORD)
        return CHEAT_ROSTER + (addr - CHEAT_ROSTER) / CHEAT_RECORD * CHEAT_RECORD;
    if (addr >= 0x00EE86E0u && addr < 0x00EE86E0u + 15u * CHEAT_RECORD)
        return 0x00EE86E0u + (addr - 0x00EE86E0u) / CHEAT_RECORD * CHEAT_RECORD;
    return 0;
}
/* The multiplier for the player of the record addr points into, kind k (0 training, 2 awakening, 3 SP, 4 秘蔵っ子): his
 * own (players.txt x<k>; for SP and 秘蔵っ子 the older shared xs when x<k> is not set), else the switch's while it is on,
 * else 1 (nothing). */
static uint32_t cheat_mult_for(uint32_t addr, int k, int sw)
{
    const uint32_t rec = cheat_record_of(addr);
    if (rec) {
        const uint32_t id = ld_be32(rec);
        const int c = s_pmult_cur, n = s_pmult_n[c];
        for (int j = 0; j < n; j++)
            if (s_pmult[c][j].id == id) {
                const uint8_t* m = s_pmult[c][j].m;
                const uint8_t v = m[k] ? m[k] : (k == 3 || k == 4) ? m[1] : 0;
                if (v) { s_cheat_hook_calls[31]++; return v; }
            }
    }
    return s_cheat_switch[sw] ? cheat_multiplier(sw) : 1;
}
static uint64_t cheat_scale_by(uint64_t reg, uint32_t m, int32_t cap)
{
    const int32_t v = (int32_t)reg;
    if (v <= 0 || m <= 1) return reg;
    const int64_t r = (int64_t)v * m;
    return (uint64_t)(int64_t)(int32_t)(r > cap ? cap : r);
}
void saka_course_cond_hook(ppu_context* ctx)   /* func_005122DC's return: r3 = 1 when the player meets the course */
{
    if (s_cheat_switch[10]) { ctx->gpr[3] = 1; s_cheat_hook_calls[10]++; }
}
void saka_awaken_ready_hook(ppu_context* ctx)   /* func_005177A0, a 能力覚醒 plan ends: r3 = the player's +0xE8 */
{
    if (s_cheat_switch[10]) { ctx->gpr[3] |= 0x100000ull; s_cheat_hook_calls[10]++; }
}
/* 26 SP IN ONE WEEK, FULL EFFECT (2026-11-13, owner: "스페셜 훈련도 1턴에 가능하도록. 너무 느려"): before the weekly SP
 *   pass every running SP slot (weeks left > 1) is set to 1 week and its weeks are kept here; the pass then gives this
 *   week's experience times those weeks (saka_sp_week_mult_hook: r30 = the slot index, r29 = the slot) and ends the
 *   plan (func_00515B80: its end effect, the awakening points, once as normal). So the plan's whole experience comes in
 *   one week. The data cheat 'SP 연습 1턴 완료' sets 1 without that (one week's experience). */
static uint8_t s_sp_weeks[3] = {1, 1, 1};
/* The plans' report records (2026-11-14, owner: "치트로 설정해 놓은 연습들이 이벤트가 단 한번도 일어나지않았어 ... 효과가
 * 없어"; REA Ghidra, re/decomp_all.c). The game's assign functions start one per slot, and the weekly pass gives a
 * plan's result and its event only when that record carries the plan's player:
 *   SP     0x00EEB724 + slot x 0x18 (func_00511F50): +0 player, +4 his rating then (func_00564BAC), +0x10 the experience
 *          so far; func_00515B80 at the end: +0xA awakening points, +0xC course, +0x14, event 0x24 (0x27 on a rating rise).
 *   能力覚醒 0x00EEB76C + slot x 0x18 (func_0051A820): +0 player; at the end +0xC course, +0x14 the skill roll, event 0x25.
 *   個性強化 0x00EEB7B4 + slot x 0x10 (func_0051A9EC): +0 player; at the end +4 course, +8 trait roll, +0xC skill roll,
 *          event 0x6A; the event scene gives the trait or the skill from this record.
 * The F2 slots and SLOT KEEP fill a slot without those functions (they charge the fee and clear the advice list), so
 * the record stayed empty or kept the last plan's result: no roll, no event, nothing given (the owner's log: "hizo
 * roll 0" over weeks of finished plans). Before each weekly pass every running slot now gets the record its assign
 * function would have left: started again when it carries another player, or still shows a finished plan (+0xC / +4
 * course set; the game's own records never do while the slot runs). The last result stays until then, so the event
 * shown after the pass still reads it. The SP record's rating is taken by the game's own function, called from the
 * recompiled code right after this hook (saka_train_sp_report_rec). */
static uint32_t s_sp_report_new[3];
static uint32_t train_roster_record(uint32_t id)
{
    if (!id) return 0;
    for (uint32_t k = 0; k < 23u; k++)
        if (ld_be32(CHEAT_ROSTER + k * CHEAT_RECORD) == id) return CHEAT_ROSTER + k * CHEAT_RECORD;
    return 0;
}
static void train_report_sync(void)
{
    if (!cheat_committed(0x00EE83A8u, 0x84u) || !cheat_committed(0x00EEB724u, 0xC0u)) return;
    for (uint32_t i = 0; i < 3u; i++) {
        const uint32_t slot = 0x00EE83A8u + 0xCu * i, rep = 0x00EEB724u + 0x18u * i, id = ld_be32(slot);
        const uint32_t rec = vm_base[slot + 8] ? train_roster_record(id) : 0;
        s_sp_report_new[i] = 0;
        if (!rec || (ld_be32(rep) == id && ld_be32(rep + 0xCu) == 0)) continue;
        memset(vm_base + rep, 0, 0x18);
        st_be32(rep, id);
        s_sp_report_new[i] = rec;
        s_cheat_hook_calls[29]++;
    }
    for (uint32_t i = 0; i < 3u; i++) {
        const uint32_t slot = 0x00EE83CCu + 0x20u * i, id = ld_be32(slot);
        if (!vm_base[slot + 0xC] || !train_roster_record(id)) continue;
        const int awaken = vm_base[slot + 4] == 1;
        if (!awaken && vm_base[slot + 4] != 0) continue;
        const uint32_t rep = awaken ? 0x00EEB76Cu + 0x18u * i : 0x00EEB7B4u + 0x10u * i;
        if (ld_be32(rep) == id && ld_be32(rep + (awaken ? 0xCu : 4u)) == 0) continue;
        memset(vm_base + rep, 0, awaken ? 0x18 : 0x10);
        st_be32(rep, id);
        s_cheat_hook_calls[29]++;
    }
}
uint32_t saka_train_sp_report_rec(uint32_t i)   /* the player record whose SP report (slot i) started in this pass, else 0 */
{
    if (i >= 3u) return 0;
    const uint32_t rec = s_sp_report_new[i];
    s_sp_report_new[i] = 0;
    return rec;
}
void saka_train_snapshot_hook(ppu_context* ctx)   /* func_005177A0 before its SP and 秘蔵っ子 passes */
{
    (void)ctx;
    train_report_sync();
    for (uint32_t i = 0; i < 3u; i++) {
        const uint32_t slot = 0x00EE83A8u + 0xCu * i;
        const uint8_t w = vm_base[slot + 8];
        s_sp_weeks[i] = 1;
        if (s_cheat_switch[26] && ld_be32(slot) != 0 && w > 1) {
            s_sp_weeks[i] = w;
            vm_base[slot + 8] = 1;
            s_cheat_hook_calls[26]++;
        }
    }
    s_keep_valid = s_cheat_switch[11];
    if (!s_keep_valid) return;
    memcpy(s_keep_sp, vm_base + 0x00EE83A8u, sizeof s_keep_sp);
    memcpy(s_keep_hizo, vm_base + 0x00EE83CCu, sizeof s_keep_hizo);
}
void saka_train_sp_keep_hook(ppu_context* ctx)   /* after func_00515B80 ended an SP plan: r29 = its slot, r30 = index */
{
    const uint32_t slot = (uint32_t)ctx->gpr[29], i = (uint32_t)ctx->gpr[30];
    if (!s_cheat_switch[11] || !s_keep_valid || i >= 3 || slot != 0x00EE83A8u + 0xCu * i) return;
    const uint8_t* s = s_keep_sp[i];
    const uint32_t id = (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | s[3];
    const uint32_t course = (uint32_t)s[4] << 24 | (uint32_t)s[5] << 16 | (uint32_t)s[6] << 8 | s[7];
    if (!id || ld_be32(slot) != 0 || course >= 0x100u) return;   /* nothing kept, or the slot is in use again */
    memcpy(vm_base + slot, s, 0xC);
    const uint32_t weeks = vm_base[0x00E18A3Cu + course * 0x18u + 0x14u] * 2u;
    vm_base[slot + 8] = (uint8_t)(weeks ? (weeks > 255 ? 255 : weeks) : 1);
    s_cheat_hook_calls[11]++;
}
static void hizo_keep(ppu_context* ctx, uint32_t table)   /* after func_0050CD18: r30 = the 秘蔵っ子 slot, r31 = index */
{
    const uint32_t slot = (uint32_t)ctx->gpr[30], i = (uint32_t)ctx->gpr[31];
    if (!s_cheat_switch[11] || !s_keep_valid || i >= 3 || slot != 0x00EE83CCu + 0x20u * i) return;
    const uint8_t* s = s_keep_hizo[i];
    const uint32_t id = (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | s[3];
    const uint32_t course = (uint32_t)s[8] << 24 | (uint32_t)s[9] << 16 | (uint32_t)s[10] << 8 | s[11];
    if (!id || ld_be32(slot) != 0 || course >= 0x100u) return;
    memcpy(vm_base + slot, s, 0x20);
    const uint32_t weeks = (vm_base[table + course * 0x3Cu + 4u] & 0x7Fu) * 2u;
    vm_base[slot + 0xC] = (uint8_t)(weeks ? weeks : 1);
    s_cheat_hook_calls[11]++;
}
void saka_train_awaken_keep_hook(ppu_context* ctx) { hizo_keep(ctx, 0x00E154A8u); }
void saka_train_personal_keep_hook(ppu_context* ctx) { hizo_keep(ctx, 0x00E17238u); }
/* Switches 12-14 and the per-player multipliers (cheat_mult_for): the player is the record in r3 (r4 for the limit
 * rise, the record + the ability). */
void saka_train_mult_hook(ppu_context* ctx)   /* normal training, before func_00577318: r3 = record, r5 = r20 = experience */
{
    const uint32_t m = cheat_mult_for((uint32_t)ctx->gpr[3], 0, 12);
    if (m <= 1) return;
    ctx->gpr[5] = cheat_scale_by(ctx->gpr[5], m, 0xFFFF);
    ctx->gpr[20] = ctx->gpr[5];
    s_cheat_hook_calls[12]++;
}
/* Youth weekly growth (2026-11-13, owner: "유스도 훈련배수 가능하도록"): func_005161F0 adds each youth player's ability
 * experience through func_00577318 at six calls (two rounds), outside the club's training (func_00515548), so the
 * training hook never saw them. r3 = the youth record (0x00EE86E0 + n * 0x218), r5 = the gain: the youth player's own
 * 기본 (xt) multiplier, else switch 12 while it is on (as the club's training). Only r5: the weekly sum at [r28+4] is
 * added before the call and stays as the game made it. */
void saka_youth_mult_hook(ppu_context* ctx)
{
    const uint32_t m = cheat_mult_for((uint32_t)ctx->gpr[3], 0, 12);
    if (m <= 1) return;
    ctx->gpr[5] = cheat_scale_by(ctx->gpr[5], m, 0xFFFF);
    s_cheat_hook_calls[12]++;
}
void saka_sp_week_mult_hook(ppu_context* ctx)   /* SP weekly, before func_00577318: r3 = record, r5 = r24 = experience */
{
    uint32_t m = cheat_mult_for((uint32_t)ctx->gpr[3], 3, 13);
    const uint32_t i = (uint32_t)ctx->gpr[30];   /* SP IN ONE WEEK: the weeks this slot had left */
    if (i < 3u && s_sp_weeks[i] > 1 && (uint32_t)ctx->gpr[29] == 0x00EE83A8u + 0xCu * i &&
        ld_be32(0x00EE83A8u + 0xCu * i) == ld_be32((uint32_t)ctx->gpr[3]))
        m = (m < 1 ? 1 : m) * s_sp_weeks[i];
    if (m <= 1) return;
    ctx->gpr[5] = cheat_scale_by(ctx->gpr[5], m, 0xFFFF);
    ctx->gpr[24] = ctx->gpr[5];
    s_cheat_hook_calls[13]++;
}
void saka_personal_mult_hook(ppu_context* ctx)   /* 個性強化 end, before func_00577318: r3 = record, r5 = experience */
{
    const uint32_t m = cheat_mult_for((uint32_t)ctx->gpr[3], 4, 13);
    if (m <= 1) return;
    ctx->gpr[5] = cheat_scale_by(ctx->gpr[5], m, 0xFFFF);
    s_cheat_hook_calls[13]++;
}
void saka_awaken_limit_mult_hook(ppu_context* ctx)   /* 能力覚醒 limit rise (func_00512910): r4 = record + ability, r6 = the rise */
{
    const uint32_t m = cheat_mult_for((uint32_t)ctx->gpr[4], 4, 13);   /* 能力覚醒 is a 秘蔵っ子 plan */
    if (m <= 1) return;
    ctx->gpr[6] = cheat_scale_by(ctx->gpr[6], m, 100);
    s_cheat_hook_calls[13]++;
}
/* SP end: func_00568B08 (every awakening-point gain, switch 14's hook) is called right after with the same record. The
 * points get the larger of the two multipliers here, and that call does not multiply them again (no x special x
 * awakening). */
static volatile uint32_t s_awake_done_rec;
void saka_sp_awake_mult_hook(ppu_context* ctx)   /* SP end, before func_00568B08: r3 = record, r4 = awakening points */
{
    const uint32_t rec = (uint32_t)ctx->gpr[3];
    const uint32_t ms = cheat_mult_for(rec, 3, 13), ma = cheat_mult_for(rec, 2, 14), m = ms > ma ? ms : ma;
    s_awake_done_rec = rec;
    if (m <= 1) return;
    ctx->gpr[4] = cheat_scale_by(ctx->gpr[4], m, 0xFFFF);
    s_cheat_hook_calls[ms >= ma ? 13 : 14]++;
}
void saka_awake_mult_hook(ppu_context* ctx)   /* func_00568B08 entry: r3 = player record, r4 = points */
{
    const uint32_t rec = (uint32_t)ctx->gpr[3];
    if (s_awake_done_rec) {
        const int sp_end = s_awake_done_rec == rec;
        s_awake_done_rec = 0;
        if (sp_end) return;
    }
    /* the user's club, or its youth team (15 records of the same layout at 0x00EE86E0; owner, 2026-11-09) */
    if (!cheat_record_of(rec)) return;
    const uint32_t m = cheat_mult_for(rec, 2, 14);
    if (m <= 1) return;
    ctx->gpr[4] = cheat_scale_by(ctx->gpr[4], m, 0xFFFF);
    s_cheat_hook_calls[14]++;
}

/* 15 NEGOTIATION (2026-11-09, owner: "협상도 100%가 통하지 않네"): every negotiation runs in the EV_TRANSFER scene
 *   (FUN_00347610); func_0053115C(gauge, attempts) decides it: 2 = signs (offer % + bonus >= 100), 1 = one more
 *   chance, 0 = refused (and the club's affinity drops). Hook at its return: 2. The club's transfer-fee talks use the
 *   same gauge, so they are accepted too; the player signs for the salary on offer.
 * 16 FULL HOUSE (owner: "구장 관중치트도 필요해"): func_0053D7A8 computes a match's attendance (seats = stadium master
 *   0x43 +0x11 x 1000); after it asks the game's own full-house flag (func_004D1190(0xF)), r3 = 1 takes that branch:
 *   attendance = seats, unsold = 0 (it goes to 0x00F99260 for the match, and into the gate income). */
void saka_negotiate_hook(ppu_context* ctx)
{
    if (s_cheat_switch[15]) { ctx->gpr[3] = 2; s_cheat_hook_calls[15]++; }
}
/* Only the user's own matches (2026-11-09, owner: "자동으로 팀감지하는 기능은 모든 치트에게서 넣어야"): func_005763B0
 * asks for the user's match into 0x00F99254 (attendance at +0xC); the other clubs' simulated matches (func_00496998)
 * into a stack buffer, and they were made full too. r16 = func_0053D7A8's first argument, kept from its entry. */
void saka_full_house_hook(ppu_context* ctx)
{
    if (s_cheat_switch[16] && (uint32_t)ctx->gpr[16] == 0x00F99254u) { ctx->gpr[3] = 1; s_cheat_hook_calls[16]++; }
}
/* 21 ATTENDANCE (2026-11-13, owner: "관중수도 999999까지 가능한 치트"; REA Ghidra, re/decomp_all.c): the user's match
 *   result 0x00F99254 has the attendance at +0xC, four more counts at +0x10..+0x1C (func_007A5B5C's total is the sum of
 *   the five) and the unsold seats at +0x20; func_0053D7A8 caps it at the stadium's seats. Set after the gate income
 *   func_00540AA0 (in func_005763B0, 0x00576F7C): its int32 products (count x price 3000-4000) overflow above about
 *   536,870, so the income is taken at the game's count and scaled here with 64-bit math (output 0x00F99278, [1] =
 *   ticket income in 万円, added to the money by the func_0053F3A0(3) right after). The total becomes the value
 *   (1-999,999). func_00541640 (supporters, before this point) still sees the game's count. */
void saka_attendance_hook(ppu_context* ctx)
{
    (void)ctx;
    if (!s_cheat_switch[21]) return;
    const uint32_t rec = 0x00F99254u, income = 0x00F99278u + 4u;
    if (!cheat_committed(rec, 0x24u) || !cheat_committed(income, 4u)) return;
    int64_t others = 0;
    for (uint32_t o = 0x10u; o <= 0x1Cu; o += 4u) others += (int32_t)ld_be32(rec + o);
    const int64_t base = (int64_t)(int32_t)ld_be32(rec + 0xCu) + others;
    uint32_t want = s_cheat_switch_value[21];
    if (want < 1u) want = 1u;
    if (want > 999999u) want = 999999u;
    if (base <= 0) return;
    const int64_t att = (int64_t)want - others;
    st_be32(rec + 0xCu, (uint32_t)(att > 0 ? att : 0));
    st_be32(rec + 0x20u, 0u);
    const int64_t old = (int32_t)ld_be32(income);
    if (old > 0) {
        int64_t v = old * (int64_t)want / base;
        st_be32(income, (uint32_t)(v > 0x7FFFFFFF ? 0x7FFFFFFF : v));
    }
    s_cheat_hook_calls[21]++;
    fprintf(stderr, "[cheats] attendance %lld -> %u, ticket income %lld -> %d (万円)\n", (long long)base, want,
            (long long)old, (int32_t)ld_be32(income));
}
/* 24 NO DISSATISFACTION (2026-11-13, owner: "불만도도 All Zero 무조건 유지하도록"; REA Ghidra): each roster slot has a
 *   0xD0-byte block at 0x00EEB800; its u32 at +8 holds the dissatisfaction causes (0x1 another player, 0x2 the manager,
 *   0x4 salary, 0x8/0x10/0x20 playing time, 0x80 clubhouse; 0x100 reported, 0x40000000 angry, 0x80000000 decided)
 *   and the byte at +0xC the angry months. The monthly judge func_00589B48 sets them; the coach's reports and the
 *   events test (word & 0xBF). Live 2026-11-13: the salary bit was on for the four players the coach meant. Hook at the
 *   judge's end: clear +8..+0x17 and +0x50..+0x73 of the 23 blocks, as the game's own clearing (func_0058877C(id, 1),
 *   the 'speech' ability) does; the cheat's data codes also clear +8..+0x17 ten times a second. */
void saka_dissat_hook(ppu_context* ctx)
{
    (void)ctx;
    if (!s_cheat_switch[24] || !cheat_committed(0x00EEB800u, 23u * 0xD0u)) return;
    for (uint32_t i = 0; i < 23u; i++) {
        const uint32_t b = 0x00EEB800u + i * 0xD0u;
        for (uint32_t o = 8u; o < 0x18u; o += 4u) st_be32(b + o, 0u);
        for (uint32_t o = 0x50u; o < 0x74u; o += 4u) st_be32(b + o, 0u);
    }
    s_cheat_hook_calls[24]++;
}
/* 25 LOYALTY MAX (2026-11-13, owner: "충성도도 있으면 무조건 맥스고정"): loyalty (忠誠度) is the u16 at player record
 *   +0x200, 0-100; func_005680B8 (event deltas from the table 0x008D4730) and func_00568380 clamp and store it. Hook
 *   after each store: 100 for a record of the user's club or youth team (r31); the data codes hold it too. */
void saka_loyalty_hook(ppu_context* ctx)
{
    if (!s_cheat_switch[25]) return;
    const uint32_t rec = cheat_record_of((uint32_t)ctx->gpr[31]);
    if (!rec || rec != (uint32_t)ctx->gpr[31]) return;
    st_be16(rec + 0x200u, 100u);
    s_cheat_hook_calls[25]++;
}
/* 22 TRAINING SLOT DUPLICATES (2026-11-13, owner: "비장의 아이훈련을 한 선수가 모든 슬롯을 쓰는 중복이 가능하도록"): the
 *   training menu's player list (func_00071EDC) takes a player only when his row's slot marker (func_00073AD0 秘蔵っ子
 *   +0x444, func_00073BD0 SP +0x43C: the slot he is in, or -1) is -1 or the slot being edited. Hook after each load:
 *   -1, so one player can fill every slot (the assign functions only ask that the target slot is empty). Each slot
 *   runs on its own: SP experience adds up; a second 能力覚醒 needs the awakening-ready bit again (COURSE ANY reads it
 *   as set). The slots stay 3 each: the save keeps 3 of each, with other data right after them. */
void saka_slot_dup_hook(ppu_context* ctx)
{
    if (s_cheat_switch[22]) { ctx->gpr[4] = (uint64_t)(int64_t)-1; s_cheat_hook_calls[22]++; }
}
/* 23 SUPER PLAY ON F6 (2026-11-13, owner: "3d경기때 작동하는 슈퍼플레이, ... 버튼을 눌러서 마음대로 작동", "스크립트니까
 *   관련한 스크립트를 작동하게"): a skill's play (光プレー) comes from func_0023B100: for each candidate the play
 *   offers (func_0024D348: skills of the players in it that match the play), rand(10000)/100 against the skill's
 *   chance; the successes then compete with rand(100)+1, and the winner's skill and player go into the play record
 *   (+0x2564 / +0x2568), from which the game plays its own scene. At the compare: r22 = the match, r27 = the
 *   candidate's engine team entry (match + 0x334 + t x 0x126C), fpr31 = the roll.
 * 2026-11-14 (owner: "히카리플레이가 작동하지 않아. 누르면 무조건 작동하게 해줘. 무슨 조건 상관없이"; the owner's log:
 *   about 40 presses in a match, one forced play): the old 20 s arming only forced the roll, and a play has a
 *   candidate only when one of the user's players in it OWNS a skill of a kind that fits the play, which is rare.
 *   Now F6 stays armed until it is used, and while it is armed the user's players in a play are offered every skill
 *   that fits it, owned or not (REA Ghidra + the EBOOT's tables):
 *     the match skill table PTR 0x00AC0EC4 (183 x 0x34, the ids of skill_name.txb): +0xC u8 grade (chance 1-100%),
 *       +0x10 side (1 the side with the ball, 2 the other, 3 either), +0x14 kind, +0x18 0 = a play skill (1 = a
 *       passive one), +0x1C 5/6 = one more condition at the roll (func_002D617C / func_002D6204);
 *     the kind table PTR 0x00AA5494 (kind x 0x18): +4.. the five play kinds (the play's +4) the kind fits: 0-4
 *       dribbles (dribbles; tackle and charge on the other side), 5 pass, 6 feed / cross, 8 / 9 shots, 11-15 traps,
 *       17 / 18 saves, 20 pass cut.
 *   func_0024D348 takes a participant's six skills into its frame (sp + 0xAC), then tries each: the kind must fit
 *   the play, func_0024C450 must build the skill's version of the play, and that version must be of the play's own
 *   class (func_002419B4 +8 / +0x10). saka_superplay_skills_hook (after the six are fetched: r30 = the player's
 *   match object, [+0xBC] -> his record; r21 = his team in the play, r18 = the play's context: +0x2B0 + team x 4 the
 *   engine team, +0x2C4 the side with the ball; r20 = the play, +4 its kind) lists one skill of each fitting kind
 *   the player does not have (the level-7 one; keeper skills only for a keeper, +0x204 = 0; none with +0x1C over
 *   4). saka_superplay_more_hook (after the six were tried; r23 = the candidate table, +team x 4 its count) puts
 *   the next ones into the frame and has the loop run again, until the team has a candidate or the list ends; a
 *   team's table holds five (0x17C = 5 x 0x4C), so never more than five at a time. The game's own checks all stay,
 *   so only plays the engine can show are made; the phases without skill plays (the play context's +0x298 4 / 5,
 *   situations 0x25 / 0x28-0x2A / 0x2E) stay without. The roll hook then makes the user's first candidate succeed
 *   and the other team's fail in that play. Armed until used; a kick-off clears it, and so do two minutes without a
 *   play (F6 pressed outside a match: the badge would stay otherwise; in a match the wait was 0-35 s). */
static volatile LONG s_superplay_armed;
static volatile uint64_t s_superplay_done_until, s_superplay_fired_at, s_superplay_armed_at;
static volatile long s_superplay_offers;   /* plays in which skills were offered while armed (diagnosis) */
int saka_superplay_arm(void)
{
    if (!s_cheat_switch[23]) return 0;
    s_superplay_armed_at = GetTickCount64();
    InterlockedExchange(&s_superplay_armed, 1);
    fprintf(stderr, "[cheats] super play armed (F6): the user's team's next play\n");
    return 1;
}
int saka_superplay_state(void)   /* for the F2 badge: 0 idle, 1 armed, 2 used in the last 3 s */
{
    if (s_superplay_armed && GetTickCount64() - s_superplay_armed_at > 120000u) InterlockedExchange(&s_superplay_armed, 0);
    if (s_cheat_switch[23] && s_superplay_armed) return 1;
    return s_superplay_fired_at && GetTickCount64() - s_superplay_fired_at < 3000u ? 2 : 0;
}
static struct { uint32_t frame, team; int n, pos; uint8_t list[72]; } s_sp_it;
static int superplay_kind_fits(uint32_t kinds, uint32_t kind, int32_t pk)
{
    if (kind >= 0x44u) return 0;
    for (uint32_t j = 1; j <= 5u; j++)
        if ((int32_t)ld_be32(kinds + kind * 0x18u + 4u * j) == pk) return 1;
    return 0;
}
void saka_superplay_skills_hook(ppu_context* ctx)   /* func_0024D348, a participant's six skills are in the frame */
{
    s_sp_it.n = s_sp_it.pos = 0;
    if (!s_cheat_switch[23] || !s_superplay_armed) return;
    const uint32_t sp = (uint32_t)ctx->gpr[1], pctx = (uint32_t)ctx->gpr[18], play = (uint32_t)ctx->gpr[20];
    const uint32_t team = (uint32_t)ctx->gpr[21], obj = (uint32_t)ctx->gpr[30];
    if (team > 1u || !cheat_committed(pctx + 0x2B0u, 0x18u) || !cheat_committed(play, 0x10u) || !cheat_committed(obj + 0xBCu, 4u)) return;
    if (!match_team_is_user(ld_be32(pctx + 0x2B0u + 4u * team))) return;
    const uint32_t skills = ld_be32(0x00AC0EC4u), kinds = ld_be32(0x00AA5494u);
    if (!cheat_committed(skills, 183u * 0x34u) || !cheat_committed(kinds, 0x44u * 0x18u)) return;
    const uint32_t pp = ld_be32(obj + 0xBCu);
    const uint32_t rec = cheat_committed(pp, 4u) ? ld_be32(pp) : 0;
    if (!rec || !cheat_committed(rec, CHEAT_RECORD)) return;
    const int keeper = ld_be32(rec + 0x204u) == 0;
    const int32_t pk = (int32_t)ld_be32(play + 4u);
    const int same = ld_be32(pctx + 0x2C4u) == team;
    uint8_t best[0x44];
    memset(best, 0, sizeof best);
    for (uint32_t id = 1; id <= 146u; id++) {
        const uint32_t r = skills + id * 0x34u;
        const uint32_t side = ld_be32(r + 0x10u), kind = ld_be32(r + 0x14u);
        if (ld_be32(r + 0x18u) != 0 || ld_be32(r + 0x1Cu) > 4u || kind >= 0x44u) continue;
        if ((side == 1u && !same) || (side == 2u && same) || side == 0 || side > 3u) continue;
        if ((kind == 0x20u || kind == 0x21u || kind == 0x40u) != keeper) continue;
        if (!superplay_kind_fits(kinds, kind, pk)) continue;
        const int top = id <= 112u && id % 7u == 0;   /* the level-7 skill of a type */
        const int had = best[kind] && best[kind] <= 112u && best[kind] % 7u == 0;
        if (!best[kind] || (top && !had)) best[kind] = (uint8_t)id;
    }
    for (uint32_t kind = 0; kind < 0x44u; kind++) {
        if (!best[kind]) continue;
        int own = 0;   /* a skill of this kind is among his six: the game tries it itself */
        for (uint32_t i = 0; i < 6u && !own; i++) {
            const uint32_t id = ld_be32(sp + 0xACu + 4u * i);
            own = id && id < 183u && ld_be32(skills + id * 0x34u + 0x14u) == kind && ld_be32(skills + id * 0x34u + 0x18u) == 0;
        }
        if (!own && s_sp_it.n < (int)sizeof s_sp_it.list) s_sp_it.list[s_sp_it.n++] = best[kind];
    }
    s_sp_it.frame = sp;
    s_sp_it.team = team;
    if (s_sp_it.n) s_superplay_offers++;
}
int saka_superplay_more_hook(ppu_context* ctx)   /* func_0024D348, the six in the frame were tried; 1 = six more are in it */
{
    if (s_sp_it.pos >= s_sp_it.n) return 0;
    const uint32_t sp = (uint32_t)ctx->gpr[1], table = (uint32_t)ctx->gpr[23], team = (uint32_t)ctx->gpr[21];
    if (sp != s_sp_it.frame || team != s_sp_it.team || !s_superplay_armed || !cheat_committed(table, 8u)) { s_sp_it.n = 0; return 0; }
    if (ld_be32(table + 4u * team) >= 1u) { s_sp_it.n = 0; return 0; }   /* the team has its candidate */
    for (uint32_t i = 0; i < 6u; i++)   /* five at most: a team's table holds five candidates */
        st_be32(sp + 0xACu + 4u * i, i < 5u && s_sp_it.pos < s_sp_it.n ? s_sp_it.list[s_sp_it.pos++] : 0u);
    return 1;
}
static void superplay_kickoff(void) { InterlockedExchange(&s_superplay_armed, 0); }
void saka_superplay_hook(ppu_context* ctx)
{
    if (!s_cheat_switch[23]) return;
    const uint64_t now = GetTickCount64();
    const int armed = s_superplay_armed != 0, done = now < s_superplay_done_until;
    if (!armed && !done) return;
    const uint32_t entry = (uint32_t)ctx->gpr[27], match = (uint32_t)ctx->gpr[22];
    if (entry < match + 0x334u) return;
    const uint32_t t = (entry - match - 0x334u) / 0x126Cu;
    if (t > 1u || entry != match + 0x334u + t * 0x126Cu) return;
    if (match_team_is_user(t)) {
        if (!armed) return;
        ctx->fpr[31] = -1.0;   /* below any chance: this candidate succeeds */
        InterlockedExchange(&s_superplay_armed, 0);
        s_superplay_done_until = now + 500u;   /* the rest of this play: the other team's candidates still fail */
        s_superplay_fired_at = now;
        s_cheat_hook_calls[23]++;
        fprintf(stderr, "[cheats] super play: the user's team's skill forced (plays offered skills while armed: %ld)\n", s_superplay_offers);
    } else
        ctx->fpr[31] = 1.0e9;  /* above any chance */
}
void saka_money_hook(ppu_context* ctx)   /* after the store of func_0053F3A0 / func_0053F434 / func_00544558 */
{
    (void)ctx;
    if (s_money_lock) st_be32(CHEAT_MONEY, s_money_value);
}
void saka_hizo_roll_hook(ppu_context* ctx)
{
    if (s_cheat_switch[4]) { ctx->gpr[3] = 1; s_cheat_hook_calls[4]++; }
}
/* 4, 100% (2026-11-13, owner: "스페셜이나 비장의아이나 모두 100% 훈련성공하도록"; REA Ghidra): both rolls take the
 * first outcome k with roll <= cum[k] (the three cumulative chances at sp+0x70, started from the shared total, so the
 * skill roll's continue from the trait roll's) and then drop it when the player already has it: a trait already in
 * his five trait bytes (record +0x20C; an edit player never gets traits 4/5), a skill already at that level in his
 * current style slot (func_00566314: skill table 0x94, 0xC bytes: +4 type, +8 level, +9 original). With the old roll
 * of 1 a course repeated by the slot lock gave nothing from its second run on. Now the roll goes to the first
 * reachable outcome the player does not have; when he has them all, the first (nothing can be given then). Special
 * training has no roll. func_00567A2C: r31 player, r30 outcomes; func_00567B74: r30 player, r31 outcomes. */
static int hizo_is_edit(uint32_t id) { return id >= 0x7FFBu && id <= 0x7FFFu; }
static int hizo_has_trait(uint32_t player, uint32_t trait)
{
    if (!cheat_committed(player, 0x218u)) return 0;
    for (uint32_t i = 0; i < 5u; i++)
        if (vm_base[player + 0x20Cu + i] == trait) return 1;
    return hizo_is_edit(ld_be32(player)) && (trait == 4u || trait == 5u);
}
static int hizo_has_skill(uint32_t player, uint32_t skill)
{
    if (!cheat_committed(player, 0x218u)) return 0;
    const uint32_t slot = player + 0x10Cu + 0x30u * vm_base[player + 0x10Au];
    const uint32_t desc = ld_be32(0x008D4A9Cu + 4u * 0x94u);
    if (!cheat_committed(desc, 8u)) return 0;
    const uint32_t rec = ld_be32(desc + 4u) + skill * 0xCu;
    if (!cheat_committed(rec, 0xCu) || !cheat_committed(slot, 0x30u)) return 0;
    if (vm_base[rec + 9u]) return ld_be32(slot + 4u) == skill;   /* an original skill */
    const uint32_t type = ld_be32(rec + 4u);
    if (!type) return 0;
    int at = -1;
    for (int i = 0; i < 5; i++)
        if (ld_be32(slot + 8u + 8u * (uint32_t)i) == type) at = i;
    return at >= 0 && vm_base[slot + 0xCu + 8u * (uint32_t)at] == vm_base[rec + 8u];
}
static void hizo_roll(ppu_context* ctx, int skill)
{
    if (!s_cheat_switch[4]) return;
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    const uint32_t player = (uint32_t)ctx->gpr[skill ? 30 : 31], outcomes = (uint32_t)ctx->gpr[skill ? 31 : 30];
    if (!cheat_committed(sp + 0x70u, 12u) || !cheat_committed(outcomes, 12u)) { ctx->gpr[3] = 1; return; }
    int first = -1, pick = -1;
    for (int k = 0; k < 3 && pick < 0; k++) {
        const uint32_t cum = ld_be32(sp + 0x70u + 4u * (uint32_t)k);
        const uint32_t before = k ? ld_be32(sp + 0x70u + 4u * (uint32_t)(k - 1)) : 0u;
        const uint32_t out = ld_be32(outcomes + 4u * (uint32_t)k);
        if (cum < 1u || cum <= before || !out) continue;   /* not reachable, or nothing */
        if (first < 0) first = k;
        if (!(skill ? hizo_has_skill(player, out) : hizo_has_trait(player, out))) pick = k;
    }
    if (pick < 0) pick = first;
    ctx->gpr[3] = pick <= 0 ? 1u : ld_be32(sp + 0x70u + 4u * (uint32_t)(pick - 1)) + 1u;
    s_cheat_hook_calls[4]++;
}
void saka_hizo_trait_roll_hook(ppu_context* ctx) { hizo_roll(ctx, 0); }   /* func_00567A2C after func_00593490 */
void saka_hizo_skill_roll_hook(ppu_context* ctx) { hizo_roll(ctx, 1); }   /* func_00567B74 after func_00593490 */
/* 2026-11-09 (owner: the clubhouse went to SPACE 48/20 and its list broke; "구장시설은 아예 제한이 풀리지도 않았어";
 *   "모든 설치가능한 시설은 0스페이스로"): 1 (over capacity) is no longer turned into 4, because building past the
 *   capacity left the menus inconsistent. The cheat's own data codes set every facility's size to 0 instead (master
 *   table 0x2B clubhouse, 41 x 0x14 at 0x00E25B2C, +0xA; table 0x2A stadium, 50 x 0x10 at 0x00E25E68, +0xA; static
 *   data in the EBOOT), so the used space (func_005402C4 / func_00540238, summing the sizes of the 27 built slots)
 *   stays 0 and 1 cannot happen. The same hook now also sits at the stadium's check func_0054F130 (same codes: 0
 *   built, 1 capacity, 2 money, 3 stadium type, 4/5 can). */
void saka_facility_check_hook(ppu_context* ctx)
{
    if (!s_cheat_switch[5]) return;
    if ((int32_t)ctx->gpr[3] == 3) { ctx->gpr[3] = 4; s_cheat_hook_calls[5]++; }
}
/* 7 EDIT 99 (Vita "EDIT 99", a heap byte there, so VitaCheat kept it at 99): the edit-player menu object (heap)
 *   keeps its points at +0x8A8; func_003CB8E8 starts them at 50 (0x32) and func_003CAD38 adds the slot bonus
 *   (+0x8AC) and caps the sum at 99, the game's own maximum. The value is the switch's (0-99; 99 in the list).
 *   2026-11-09 (owner: the points fell to 9 after spending, "고정이 되게 해줘"): func_003CC0D4, the menu's update,
 *   spends them: an item level (+0x8B0[i], 0-3) costs 10 and an ability point 1, going back refunds them, and
 *   cancelling an ability edit restores +0x90C. 고정: each of those writes is followed by the value again
 *   (saka_edit_points_lock_hook; _merge_hook after the slot bonus), and the update's entry puts it back if anything
 *   else changed it. 고정 off: the value goes in when a new player starts, and once when the cheat is turned on, the
 *   value changes or 갱신 is pressed, also in the middle of an edit (the entry hook; the update then redraws the
 *   points tag as func_003CB89C does). */
static volatile uint32_t s_edit_obj;   /* the menu whose points func_003CB8E8 started (its points tag exists) */
static long s_edit_gen_done = -1;      /* the switch generation put into it last */

static uint32_t edit_points_value(void)
{
    const uint32_t v = s_cheat_switch_value[7];
    return v > 99 ? 99 : v;
}
void saka_edit_points_hook(ppu_context* ctx)   /* func_003CB8E8 after the start value is stored; r29 = the menu */
{
    s_edit_obj = (uint32_t)ctx->gpr[29];
    if (!s_cheat_switch[7]) return;
    st_be32(s_edit_obj + 0x8A8u, edit_points_value());
    s_edit_gen_done = s_cheat_switch_gen[7];
    s_cheat_hook_calls[7]++;
}
void saka_edit_points_lock_hook(ppu_context* ctx)   /* after func_003CC0D4 changes the points; r30 = the menu */
{
    if (!s_cheat_switch[7] || !s_cheat_switch_lock[7]) return;
    st_be32((uint32_t)ctx->gpr[30] + 0x8A8u, edit_points_value());
    s_cheat_hook_calls[7]++;
}
void saka_edit_points_merge_hook(ppu_context* ctx)   /* func_003CAD38 after the slot bonus; r30 = the menu, r29 = the points shown next */
{
    if (!s_cheat_switch[7] || !s_cheat_switch_lock[7]) return;
    const uint32_t v = edit_points_value();
    st_be32((uint32_t)ctx->gpr[30] + 0x8A8u, v);
    ctx->gpr[29] = v;
    s_cheat_hook_calls[7]++;
}
int saka_edit_points_frame(ppu_context* ctx)   /* func_003CC0D4's entry; r3 = the menu. 1: the caller redraws the points */
{
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    if (!s_cheat_switch[7] || obj != s_edit_obj) return 0;
    const long gen = s_cheat_switch_gen[7];
    const uint32_t v = edit_points_value();
    const uint32_t cur = ld_be32(obj + 0x8A8u);
    if (gen == s_edit_gen_done && (!s_cheat_switch_lock[7] || cur == v)) return 0;
    s_edit_gen_done = gen;
    if (cur == v) return 0;
    st_be32(obj + 0x8A8u, v);
    s_cheat_hook_calls[7]++;
    return 1;
}
void saka_facility_build_hook(ppu_context* ctx)
{
    if (!s_cheat_switch[6]) return;
    const uint32_t ea = (uint32_t)ctx->gpr[27] + 2u;
    if (((vm_base[ea] << 8) | vm_base[ea + 1]) > 1) { vm_base[ea] = 0; vm_base[ea + 1] = 1; s_cheat_hook_calls[6]++; }
}
