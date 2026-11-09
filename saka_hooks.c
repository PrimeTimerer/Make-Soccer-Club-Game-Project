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
static FILETIME s_cheat_time[5];

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
static const char* const s_profile_files[4] = { "enabled.txt", "values.txt", "players.txt", "lists.txt" };
static void cheat_profile_copy(LONG from, LONG to, int only_missing)
{
    char a[64], b[64], fa[96], fb[96];
    cheat_profile_dir(from, a, sizeof a);
    cheat_profile_dir(to, b, sizeof b);
    CreateDirectoryA("cheats/saves", NULL);
    { char d[64]; snprintf(d, sizeof d, "%s", b); d[strlen(d) - 1] = 0; CreateDirectoryA(d, NULL); }
    for (int i = 0; i < 4; i++) {
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
 * 선택하면 ... 중복이나 이런 문제가 발생할수 있는것에 대한 방지 대책도"): players.txt fields xt (normal training), xs
 * (special training and 秘蔵っ子), xa (awakening points), 1-100. The hooks of switches 12-14 find the player from the
 * record they get (the club's 23 or the youth team's 15) and use his own value instead of the switch's:
 * - never both multiplied, and his value works with the switch off too;
 * - one value per player and kind (a later line for the same player and kind replaces the earlier one);
 * - a player who left the club and its youth team matches no record, so his line does nothing (it works again if he
 *   comes back);
 * - the awakening points at the end of an SP plan get the larger of the special and the awakening multiplier, not
 *   both multiplied (saka_sp_awake_mult_hook / saka_awake_mult_hook).
 * Two tables: players.txt is read into the one not in use, then they are swapped, so a hook never sees half of one. */
typedef struct { uint32_t id; uint8_t m[3]; } cheat_player_mult;   /* m: training, special, awakening; 0 = none */
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
            static const char kinds[] = "tsa";
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
        if ((id > 0 && id < 0x46A2u) || (id >= 0x7FFDu && id <= 0x7FFFu)) return 1;
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
    size_t size = 8;
    for (size_t r = 0; r < sizeof ranges / sizeof ranges[0]; r++) size += 8 + ranges[r][1];
    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) return;
    memcpy(buf, "SAKA", 4);
    const uint32_t n = (uint32_t)(sizeof ranges / sizeof ranges[0]);
    memcpy(buf + 4, &n, 4);
    size_t at = 8;
    for (size_t r = 0; r < n; r++) {
        memcpy(buf + at, &ranges[r][0], 4); memcpy(buf + at + 4, &ranges[r][1], 4);
        if (cheat_committed(ranges[r][0], ranges[r][1])) memcpy(buf + at + 8, vm_base + ranges[r][0], ranges[r][1]);
        else memset(buf + at + 8, 0, ranges[r][1]);
        at += 8 + ranges[r][1];
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
            for (int i = 0; i < 4; i++) {
                char f[96];
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
        }
        cheats_apply();
        if (s_ncheat_players || s_ncheat_lists || tick % 20 == 0) {
            const int loaded = cheat_game_loaded();
            cheats_players_apply(loaded);
            cheats_lists_apply(loaded);
            if (tick % 20 == 0) cheats_export(loaded);
        }
        static long shown[32];
        if (tick % 100 == 0 && memcmp(shown, (const void*)s_cheat_hook_calls, sizeof shown) != 0) {
            for (int i = 0; i < 32; i++) shown[i] = s_cheat_hook_calls[i];
            fprintf(stderr, "[cheats] code switches used: youth visit %ld, trade chance %ld, foreign slots %ld, "
                            "hizo roll %ld, facility check %ld, facility 1 week %ld, edit points %ld, substitution %ld, "
                            "interview %ld, course any %ld, slot keep %ld, training x %ld, special x %ld, awakening x %ld, "
                            "negotiation %ld, full house %ld, satisfaction %ld, suspensions lifted %ld, team skill uses given back %ld, cards removed %ld, per-player x %ld\n",
                    shown[1], shown[2], shown[3], shown[4], shown[5], shown[6], shown[7], shown[8], shown[9],
                    shown[10], shown[11], shown[12], shown[13], shown[14], shown[15], shown[16], shown[17], shown[18], shown[19], shown[20], shown[31]);
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
static int match_team_is_user(uint32_t t)
{
    if (t > 1u) return 0;
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
/* The multiplier for the player of the record addr points into, kind k (0 training, 1 special, 2 awakening): his own
 * (players.txt x<k>), else the switch's while it is on, else 1 (nothing). */
static uint32_t cheat_mult_for(uint32_t addr, int k, int sw)
{
    const uint32_t rec = cheat_record_of(addr);
    if (rec) {
        const uint32_t id = ld_be32(rec);
        const int c = s_pmult_cur, n = s_pmult_n[c];
        for (int j = 0; j < n; j++)
            if (s_pmult[c][j].id == id && s_pmult[c][j].m[k]) { s_cheat_hook_calls[31]++; return s_pmult[c][j].m[k]; }
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
void saka_train_snapshot_hook(ppu_context* ctx)   /* func_005177A0 before its SP and 秘蔵っ子 passes */
{
    (void)ctx;
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
void saka_sp_week_mult_hook(ppu_context* ctx)   /* SP weekly, before func_00577318: r3 = record, r5 = r24 = experience */
{
    const uint32_t m = cheat_mult_for((uint32_t)ctx->gpr[3], 1, 13);
    if (m <= 1) return;
    ctx->gpr[5] = cheat_scale_by(ctx->gpr[5], m, 0xFFFF);
    ctx->gpr[24] = ctx->gpr[5];
    s_cheat_hook_calls[13]++;
}
void saka_personal_mult_hook(ppu_context* ctx)   /* 個性強化 end, before func_00577318: r3 = record, r5 = experience */
{
    const uint32_t m = cheat_mult_for((uint32_t)ctx->gpr[3], 1, 13);
    if (m <= 1) return;
    ctx->gpr[5] = cheat_scale_by(ctx->gpr[5], m, 0xFFFF);
    s_cheat_hook_calls[13]++;
}
void saka_awaken_limit_mult_hook(ppu_context* ctx)   /* 能力覚醒 limit rise (func_00512910): r4 = record + ability, r6 = the rise */
{
    const uint32_t m = cheat_mult_for((uint32_t)ctx->gpr[4], 1, 13);
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
    const uint32_t ms = cheat_mult_for(rec, 1, 13), ma = cheat_mult_for(rec, 2, 14), m = ms > ma ? ms : ma;
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
void saka_money_hook(ppu_context* ctx)   /* after the store of func_0053F3A0 / func_0053F434 / func_00544558 */
{
    (void)ctx;
    if (s_money_lock) st_be32(CHEAT_MONEY, s_money_value);
}
void saka_hizo_roll_hook(ppu_context* ctx)
{
    if (s_cheat_switch[4]) { ctx->gpr[3] = 1; s_cheat_hook_calls[4]++; }
}
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
