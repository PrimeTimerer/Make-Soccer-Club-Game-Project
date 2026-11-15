/* Host-only checks of the unfinished Nov 13 batch. No game/save/file writes.
 * Unused runtime services abort if accidentally reached. */
#include <assert.h>
#include "../../saka_hooks.c"
uint8_t* vm_base;
/* Other runtime services are deliberately unavailable in this host fixture. */
lv2_syscall_table g_lv2_syscalls;
void (*g_savedata_file_hook)(const char*, const char*, unsigned, int);
uint64_t spu_workload_fingerprint(const void* p, size_t n) { abort(); }
int spu_overlay_register_verified_region(uint32_t a,uint32_t b,uint32_t c,int d,const saka_spu_code_seg* e,unsigned f) { abort(); }
void saka_mem_dump_frame(unsigned a, int b) { abort(); }
void saka_watch_frame(unsigned a) { abort(); }
int saka_watch_auto(void) { abort(); }
void saka_watch_request(uint32_t a) { abort(); }
uint32_t rsx_ww_harvest_low(void) { abort(); }
uint32_t rsx_ww_written_pages_between(uint32_t a,uint32_t b,uint32_t* c,uint32_t d) { abort(); }
void saka_pos_scan_tick(unsigned a, unsigned b) { abort(); }
void saka_edit_install(void) { abort(); }
void saka_overlay_install(void) { abort(); }
void ppu_log_host_chain(const char* a) { abort(); }
int64_t sys_semaphore_post(ppu_context* a) { abort(); }
int64_t sys_semaphore_wait(ppu_context* a) { abort(); }
void* cellGcm_fifo_kick_event(void) { abort(); }
int64_t sys_timer_usleep(ppu_context* a) { abort(); }
void saka_edit_title(void) { abort(); }
void saka_edit_export(void* a) { abort(); }

int main(void)
{
    vm_base = VirtualAlloc(NULL, 0x1000000u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(vm_base);
    ppu_context ctx = {0};

    /* Maximum attendance still has a positive 64-bit-scaled ticket income. */
    s_cheat_switch[21] = 1; s_cheat_switch_value[21] = 999999;
    st_be32(0xF99260, 9000); st_be32(0xF99264, 1000); st_be32(0xF9927C, 3000);
    saka_attendance_hook(&ctx);
    assert(ld_be32(0xF99260) + ld_be32(0xF99264) == 999999);
    assert(ld_be32(0xF9927C) == 299999);
    s_cheat_switch[21] = 0; saka_attendance_hook(&ctx);
    assert(ld_be32(0xF9927C) == 299999);

    /* A youth's own training multiplier overrides the club-wide multiplier. */
    st_be32(0xEE86E0, 123);
    s_cheat_switch[12] = 1; s_cheat_switch_value[12] = 9;
    s_pmult_n[0] = 1; s_pmult_cur = 0;
    s_pmult[0][0].id = 123; s_pmult[0][0].m[0] = 4;
    ctx.gpr[3] = 0xEE86E0; ctx.gpr[5] = 7;
    saka_youth_mult_hook(&ctx); assert(ctx.gpr[5] == 28);
    ctx.gpr[5] = 20000; saka_youth_mult_hook(&ctx); assert(ctx.gpr[5] == 65535);

    /* Locked courses are restored after completion; a one-shot course is not. */
    s_ncheat_train = 2;
    s_cheat_train[0] = (cheat_train_edit){'s', 0, 0, 1, 0, 123, 1};
    s_cheat_train[1] = (cheat_train_edit){'h', 1, 0, 0, 0, 123, 1};
    vm_base[0xE18A3C + 0x18 + 0x14] = 3;
    vm_base[0xE17238 + 0x3C + 4] = 4;
    cheats_training_apply(1);
    assert(ld_be32(0xEE83A8) == 123 && vm_base[0xEE83B0] == 6);
    assert(ld_be32(0xEE83EC) == 123 && vm_base[0xEE83F8] == 8);
    memset(vm_base + 0xEE83A8, 0, 0xC); memset(vm_base + 0xEE83EC, 0, 0x20);
    cheats_training_apply(1);
    assert(ld_be32(0xEE83A8) == 123 && vm_base[0xEE83B0] == 6);
    assert(ld_be32(0xEE83EC) == 0);

    /* Hired-staff edits reach both the contract and the selected master only. */
    st_be32(0xEE2940, 1); st_be32(0xEE6CF8, 2);
    s_ncheat_staff = 4;
    s_cheat_staff[0] = (cheat_staff_edit){'c', "age", 42, 0, 1, 0};
    s_cheat_staff[1] = (cheat_staff_edit){'c', "ab0", 17, 0, 1, 0};
    s_cheat_staff[2] = (cheat_staff_edit){'s', "years", 5, 0, 1, 0};
    s_cheat_staff[3] = (cheat_staff_edit){'s', "rank", 15, 0, 1, 0};
    cheats_staff_apply(1);
    assert(vm_base[0xEE2944] == 42 && vm_base[0xEE6CFD] == 5);
    assert(ld_be32(0xBBA60C + 100 + 0x2C) == 17);
    assert(vm_base[0xE1F6AC + 120 + 8] == 15 && vm_base[0xE1F6AC + 60 + 8] == 0);

    /* F6 remains user-only when the user is engine team 1 (away). 2026-11-14: armed until used (no 20 s window);
     * the offered skills and the kick-off are in test_nov14_cheats.c. */
    s_cheat_switch[23] = 1; s_engine_user[0] = 0; s_engine_user[1] = 1;
    assert(saka_superplay_arm());
    ctx.gpr[22] = 0x1000; ctx.gpr[27] = 0x1334; ctx.fpr[31] = 50;
    saka_superplay_hook(&ctx); assert(ctx.fpr[31] == 1.0e9);
    ctx.gpr[27] += 0x126C; ctx.fpr[31] = 50;
    saka_superplay_hook(&ctx); assert(ctx.fpr[31] == -1.0 && !s_superplay_armed);
    s_superplay_done_until = 0; ctx.fpr[31] = 50;
    saka_superplay_hook(&ctx); assert(ctx.fpr[31] == 50);
    puts("PASS: attendance/income, youth multiplier/cap, course-end lock, hired staff, away-team F6 scope/expiry");
    VirtualFree(vm_base, 0, MEM_RELEASE);
    return 0;
}
