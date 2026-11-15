/* Host-only checks of the 2026-11-14 changes: the training report records, the F6 super play hooks and the team color
 * fit. No game is started. Guest memory = the EBOOT's own data (argv[1], read only) plus a save's RAM image (argv[2],
 * read only) at 0x00EE27A0, so the team color part runs on the real tables and a real eleven.
 * RUN IT IN AN EMPTY WORKING FOLDER: the team color fit writes cheats/teamcolor_backup.txt relative to the cwd.
 * Unused runtime services abort if accidentally reached. */
#include <assert.h>
#include "../../saka_hooks.c"
uint8_t* vm_base;
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

#define MEM 0x1400000u
static uint64_t be64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }
static uint8_t* read_file(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* d = malloc(*n);
    fread(d, 1, *n, f); fclose(f);
    return d;
}
static void load_elf(const char* path)
{
    size_t n;
    uint8_t* d = read_file(path, &n);
    const uint64_t phoff = be64(d + 0x20);
    const unsigned phentsize = d[0x36] << 8 | d[0x37], phnum = d[0x38] << 8 | d[0x39];
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t* ph = d + phoff + i * phentsize;
        if ((ph[0] << 24 | ph[1] << 16 | ph[2] << 8 | ph[3]) != 1) continue;
        const uint64_t off = be64(ph + 8), vaddr = be64(ph + 16), filesz = be64(ph + 32);
        if (vaddr >= MEM) continue;
        memcpy(vm_base + vaddr, d + off, (size_t)(vaddr + filesz > MEM ? MEM - vaddr : filesz));
    }
    free(d);
}
static void boot_tables(void)   /* what the game sets at boot: the position masks (func_00580C80) and the sorted color list */
{
    static const uint32_t mask[27] = { 0, 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400, 0x300, 0x60, 0x6, 0x700, 0xF0,
                                       0xE0, 0x70, 0x90, 0xE, 0xF, 0x7E0, 0x1F, 0x366, 0x498, 0x7FF };
    for (uint32_t i = 0; i < 27; i++) st_be32(TC_POSMASK + 4 * i, mask[i]);
    uint32_t ids[0x47];
    for (uint32_t i = 0; i < 0x47; i++) ids[i] = i;
    for (int i = 0; i < 0x47; i++)   /* key high first, then the lower id (func_00580BAC) */
        for (int j = i + 1; j < 0x47; j++) {
            const long a = (long)tc_be16(TC_MASTER + ids[i] * 0x4C) * 0x10000 - (long)ids[i];
            const long b = (long)tc_be16(TC_MASTER + ids[j] * 0x4C) * 0x10000 - (long)ids[j];
            if (b > a) { const uint32_t t = ids[i]; ids[i] = ids[j]; ids[j] = t; }
        }
    for (uint32_t i = 0; i < 0x47; i++) st_be32(TC_ORDER + 4 * i, ids[i]);
}
static void show(const char* what)
{
    saka_tc_status st;
    saka_teamcolor_status(&st);
    printf("%s: now %u Lv%u, %u Lv%u, %u Lv%u | own %u Lv%u, %u Lv%u, %u Lv%u | targets %d: %u(%u) %u(%u) %u(%u) | %d styles changed, leader %u -> skill %u | tactics %u %u %u %u %u\n",
           what, st.active[0], st.active_level[0], st.active[1], st.active_level[1], st.active[2], st.active_level[2], st.plain[0],
           st.plain_level[0], st.plain[1], st.plain_level[1], st.plain[2], st.plain_level[2], st.ntarget, st.target[0], st.target_state[0],
           st.target[1], st.target_state[1], st.target[2], st.target_state[2], st.nforced, st.leader_id, st.leader_orig, st.tac[0],
           st.tac[1], st.tac[2], st.tac[3], st.tac[4]);
    for (int i = 0; i < st.nforced; i++) printf("    player %u: style %u -> %u\n", st.forced[i].id, st.forced[i].natural, st.forced[i].style);
}

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: test_nov14_cheats <EBOOT.ELF> <MAINnn.DAT>   (run in an empty folder)\n"); return 2; }
    if (GetFileAttributesA("cheats/BLJM61064.txt") != INVALID_FILE_ATTRIBUTES) { fprintf(stderr, "refusing to run in a folder with real cheats\n"); return 2; }
    CreateDirectoryA("cheats", NULL);
    vm_base = VirtualAlloc(NULL, MEM, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(vm_base);
    ppu_context ctx = {0};
    load_elf(argv[1]);
    size_t nsave;
    uint8_t* save = read_file(argv[2], &nsave);
    assert(0x00EE27A0u + nsave <= MEM);
    memcpy(vm_base + 0x00EE27A0u, save, nsave);
    boot_tables();
    const uint32_t r0 = CHEAT_ROSTER, id0 = ld_be32(r0);
    assert(tc_player_ok(id0));

    /* ---- training: the report records before the weekly pass ---- */
    uint8_t keep[0x200];
    memcpy(keep, vm_base + 0x00EE83A8u, 0x84); memcpy(keep + 0x84, vm_base + 0x00EEB724u, 0xC0);
    memset(vm_base + 0x00EE83A8u, 0, 0x84); memset(vm_base + 0x00EEB724u, 0, 0xC0);
    st_be32(0x00EE83A8u, id0); st_be32(0x00EE83ACu, 1); vm_base[0x00EE83B0u] = 3;            /* SP slot 0: a plan nobody assigned */
    st_be32(0x00EE83B4u, id0); st_be32(0x00EE83B8u, 2); vm_base[0x00EE83BCu] = 2;            /* SP slot 1: a running plan of the game's own */
    st_be32(0x00EEB73Cu, id0); st_be32(0x00EEB74Cu, 55);                                      /*   its report, 55 experience so far */
    st_be32(0x00EE83C0u, id0); st_be32(0x00EE83C4u, 3); vm_base[0x00EE83C8u] = 1;            /* SP slot 2: refilled after a finished plan */
    st_be32(0x00EEB754u, id0); st_be32(0x00EEB760u, 3); st_be32(0x00EEB764u, 777);            /*   whose result is still in the report */
    st_be32(0x00EE83CCu + 0x20, id0); st_be32(0x00EE83CCu + 0x28, 29); vm_base[0x00EE83CCu + 0x2C] = 1;                      /* 個性強化, slot 1 */
    st_be32(0x00EE83CCu + 0x40, id0); vm_base[0x00EE83CCu + 0x44] = 1; st_be32(0x00EE83CCu + 0x48, 5); vm_base[0x00EE83CCu + 0x4C] = 4;   /* 能力覚醒, slot 2 */
    saka_train_snapshot_hook(&ctx);
    assert(ld_be32(0x00EEB724u) == id0 && saka_train_sp_report_rec(0) == r0 && saka_train_sp_report_rec(0) == 0);
    assert(ld_be32(0x00EEB74Cu) == 55 && saka_train_sp_report_rec(1) == 0);                                  /* the game's own: untouched */
    assert(ld_be32(0x00EEB754u) == id0 && ld_be32(0x00EEB760u) == 0 && ld_be32(0x00EEB764u) == 0 && saka_train_sp_report_rec(2) == r0);
    assert(ld_be32(0x00EEB7B4u + 0x10) == id0 && ld_be32(0x00EEB7B4u + 0x20) == 0);                          /* 個性強化 report of slot 1 only */
    assert(ld_be32(0x00EEB76Cu + 0x30) == id0 && ld_be32(0x00EEB76Cu) == 0 && ld_be32(0x00EEB76Cu + 0x18) == 0);   /* 能力覚醒 report of slot 2 only */
    const long started = s_cheat_hook_calls[29];
    saka_train_snapshot_hook(&ctx);
    assert(s_cheat_hook_calls[29] == started);                                                                /* a second pass starts nothing again */
    s_ncheat_train = 1;
    s_cheat_train[0] = (cheat_train_edit){'h', 0, 1, 1, 0, id0, 7};                                         /* F2: 能力覚醒 course 7 into slot 0 */
    vm_base[0x00E154A8u + 7 * 0x3C + 4] = 3;
    cheats_training_apply(1);
    assert(ld_be32(0x00EE83CCu) == id0 && vm_base[0x00EE83D0u] == 1 && ld_be32(0x00EE83D0u) == 0x01000000u && ld_be32(0x00EE83D4u) == 7 && vm_base[0x00EE83D8u] == 6);
    s_ncheat_train = 0;
    memcpy(vm_base + 0x00EE83A8u, keep, 0x84); memcpy(vm_base + 0x00EEB724u, keep + 0x84, 0xC0);
    puts("PASS: training report records (new, own, refilled; 個性強化 / 能力覚醒 by the kind byte), 能力覚醒 written as a byte");

    /* ---- F6 super play: offered skills, forced roll, kick-off ---- */
    {
        const uint32_t sp = 0x00200000u, pctx = 0x00201000u, play = 0x00202000u, obj = 0x00203000u, ptr = 0x00204000u, table = 0x00205000u;
        const uint32_t skills = ld_be32(0x00AC0EC4u);
        s_cheat_switch[23] = 1; s_engine_user[0] = 0; s_engine_user[1] = 1;
        st_be32(obj + 0xBC, ptr); st_be32(ptr, r0);
        const uint32_t pos_keep = ld_be32(r0 + 0x204);
        st_be32(r0 + 0x204, 10);                                       /* a forward */
        st_be32(pctx + 0x2B0, 0); st_be32(pctx + 0x2B4, 1);            /* play team 0 = engine team 0 (other), 1 = engine team 1 (user) */
        st_be32(pctx + 0x2C4, 1);                                      /* the user's side has the ball */
        st_be32(play + 4, 8);                                          /* a shot */
        ctx.gpr[1] = sp; ctx.gpr[18] = pctx; ctx.gpr[20] = play; ctx.gpr[30] = obj; ctx.gpr[23] = table;
        for (int i = 0; i < 6; i++) st_be32(sp + 0xAC + 4 * i, 0);
        st_be32(sp + 0xAC, 14);                                        /* he owns バズーカシュート (kind 0x39) */
        ctx.gpr[21] = 1;
        saka_superplay_skills_hook(&ctx);
        assert(s_sp_it.n == 0);                                        /* not armed: nothing offered */
        assert(saka_superplay_arm() && saka_superplay_state() == 1);
        ctx.gpr[21] = 0;
        saka_superplay_skills_hook(&ctx);
        assert(s_sp_it.n == 0);                                        /* the other team's player: nothing */
        ctx.gpr[21] = 1;
        saka_superplay_skills_hook(&ctx);
        assert(s_sp_it.n >= 8);
        int kinds_seen = 0;
        for (int i = 0; i < s_sp_it.n; i++) {
            const uint32_t r = skills + s_sp_it.list[i] * 0x34;
            assert(ld_be32(r + 0x10) == 1 && ld_be32(r + 0x18) == 0 && ld_be32(r + 0x1C) <= 4);   /* a shot skill of the side with the ball */
            assert(ld_be32(r + 0x14) != 0x39 && ld_be32(r + 0x14) != 0x21);                       /* not the kind he owns, no keeper skill */
            kinds_seen++;
        }
        st_be32(table + 4, 0);
        assert(saka_superplay_more_hook(&ctx) == 1 && ld_be32(sp + 0xAC) == s_sp_it.list[0] && ld_be32(sp + 0xAC + 20) == 0);   /* five, then an empty one */
        st_be32(table + 4, 1);                                         /* the team has a candidate now */
        assert(saka_superplay_more_hook(&ctx) == 0);
        st_be32(play + 4, 5); st_be32(pctx + 0x2C4, 0);                /* the other side passes: no skill of a field player fits */
        saka_superplay_skills_hook(&ctx);
        assert(s_sp_it.n == 0);
        st_be32(play + 4, 2);                                          /* the other side dribbles: tackle and charge */
        saka_superplay_skills_hook(&ctx);
        assert(s_sp_it.n == 2);
        ctx.gpr[22] = 0x1000; ctx.gpr[27] = 0x1334; ctx.fpr[31] = 50;  /* the roll: the other team fails, the user's succeeds once */
        saka_superplay_hook(&ctx); assert(ctx.fpr[31] == 1.0e9);
        ctx.gpr[27] += 0x126C; ctx.fpr[31] = 50;
        saka_superplay_hook(&ctx); assert(ctx.fpr[31] == -1.0 && saka_superplay_state() == 2);
        s_superplay_done_until = 0; ctx.fpr[31] = 50;
        saka_superplay_hook(&ctx); assert(ctx.fpr[31] == 50);
        assert(saka_superplay_arm());
        saka_team_skill_kickoff_hook(0x00206000u, 0);                  /* a kick-off forgets an F6 from before it */
        assert(!s_superplay_armed);
        st_be32(r0 + 0x204, pos_keep);
        printf("PASS: F6 super play (%d fitting shot kinds offered to a forward who owns one; other team, keeper skills, pass plays excluded; roll forced once; kick-off clears)\n", kinds_seen);
    }

    /* ---- team color fit on the save's own eleven ---- */
    static uint8_t before[23 * CHEAT_RECORD];
    memcpy(before, vm_base + CHEAT_ROSTER, sizeof before);
    assert(tc_tables_ok());
    cheats_teamcolor_apply(1); show("off ");
    s_tc_cfg.on = 1; s_tc_cfg.mode = 0; s_tc_cfg.level = 7; s_tc_cfg.upper = 0;
    cheats_teamcolor_apply(1); show("auto, upper off");
    s_tc_cfg.upper = 1;
    cheats_teamcolor_apply(1); show("auto");
    {
        saka_tc_status st;
        saka_teamcolor_status(&st);
        const int sum_now = st.active_level[0] + st.active_level[1] + st.active_level[2], sum_own = st.plain_level[0] + st.plain_level[1] + st.plain_level[2];
        assert(sum_now >= sum_own);
        for (int k = 0; k < st.ntarget; k++) assert(st.target_state[k] == 0);   /* auto only takes colors it can fit */
        assert(st.nforced == 0 || memcmp(before, vm_base + CHEAT_ROSTER, sizeof before) != 0);
        assert((st.nforced > 0 || st.leader_orig) == (s_tc_nbackup > 0));
        static uint8_t held[23 * CHEAT_RECORD];
        memcpy(held, vm_base + CHEAT_ROSTER, sizeof held);
        cheats_teamcolor_apply(1);                                              /* a second tick changes nothing */
        assert(memcmp(held, vm_base + CHEAT_ROSTER, sizeof held) == 0);
        if (st.nforced) {   /* the game moves a skill of a changed slot on: the slot is held, and still goes back when off */
            uint32_t rec = 0;
            for (uint32_t k = 0; k < 23 && !rec; k++)
                if (ld_be32(CHEAT_ROSTER + k * CHEAT_RECORD) == st.forced[0].id) rec = CHEAT_ROSTER + k * CHEAT_RECORD;
            vm_base[tc_slot_of(rec) + 12] = 3;
            cheats_teamcolor_apply(1);
            assert(memcmp(held, vm_base + CHEAT_ROSTER, sizeof held) == 0);
            vm_base[tc_slot_of(rec) + 12] = 3;
        }
        /* a restart: the kept slots come back from the file */
        const int n = s_tc_nbackup;
        s_tc_nbackup = 0; tc_load_backups();
        assert(s_tc_nbackup == n);
    }
    s_tc_cfg.on = 0;
    cheats_teamcolor_apply(1); show("off ");
    assert(memcmp(before, vm_base + CHEAT_ROSTER, sizeof before) == 0 && s_tc_nbackup == 0);
    assert(GetFileAttributesA("cheats/teamcolor_backup.txt") == INVALID_FILE_ATTRIBUTES);
    puts("PASS: team color fit auto (levels not lower, only fitted colors, held slot, restart, off = the save's bytes again)");

    /* picked colors, including ones the styles alone cannot give */
    static const uint32_t picks[][3] = { {22, 41, 58}, {2, 14, 35}, {6, 24, 59}, {50, 56, 58}, {23, 32, 69}, {1, 0, 0} };
    for (unsigned t = 0; t < sizeof picks / sizeof picks[0]; t++) {
        char name[48];
        s_tc_cfg.on = 1; s_tc_cfg.mode = 1; memcpy(s_tc_cfg.pick, picks[t], sizeof picks[t]);
        cheats_teamcolor_apply(1);
        snprintf(name, sizeof name, "pick %u %u %u", picks[t][0], picks[t][1], picks[t][2]);
        show(name);
        saka_tc_status st;
        saka_teamcolor_status(&st);
        for (int k = 0; k < st.ntarget; k++)
            if (st.target_state[k] == 0)
                assert(st.active[0] == st.target[k] || st.active[1] == st.target[k] || st.active[2] == st.target[k]);   /* "fitted" means the game has it */
    }
    {   /* the 18 "upper" colors: open to the user's club while the cheat and 'upper' are on */
        static const uint32_t up[3] = { 23, 22, 58 };
        saka_tc_status st;
        assert(tc_be16(TC_MASTER + 23 * 0x4C + 2) & 1);                       /* the game ships 23 as unused */
        s_tc_cfg.on = 1; s_tc_cfg.mode = 1; s_tc_cfg.upper = 0; memcpy(s_tc_cfg.pick, up, sizeof up);
        cheats_teamcolor_apply(1); show("upper off, pick 23 22 58");
        saka_teamcolor_status(&st);
        assert(st.target_state[0] == 5 || st.target_state[1] == 5 || st.target_state[2] == 5);
        assert(st.active[0] != 23 && st.active[1] != 23 && st.active[2] != 23 && st.color_unused[23] == 1);
        assert(!saka_teamcolor_upper_hook(TC_CLUB));
        s_tc_cfg.upper = 1;
        cheats_teamcolor_apply(1); show("upper on,  pick 23 22 58");
        saka_teamcolor_status(&st);
        assert(st.active[0] == 23 && st.color_unused[23] == 0 && st.leader_orig == 172);   /* the KEY player gets its skill */
        assert(saka_teamcolor_upper_hook(TC_CLUB));
        st_be32(0x00300000u, 0x100u); assert(saka_teamcolor_upper_hook(0x00300000u));      /* a match copy of the user's club */
        st_be32(0x00300000u, 0x21u); assert(!saka_teamcolor_upper_hook(0x00300000u));      /* another club keeps the game's rule */
        assert(tc_be16(TC_MASTER + 23 * 0x4C + 2) & 1);                       /* the game's table itself is not written */
        s_tc_cfg.mode = 0;
        cheats_teamcolor_apply(1); show("upper on,  auto");
        saka_teamcolor_status(&st);
        for (int k = 0; k < st.ntarget; k++) assert(st.target_state[k] == 0);
    }
    s_tc_cfg.on = 0;
    cheats_teamcolor_apply(1);
    assert(!saka_teamcolor_upper_hook(TC_CLUB));
    assert(memcmp(before, vm_base + CHEAT_ROSTER, sizeof before) == 0 && s_tc_nbackup == 0);
    puts("PASS: team color fit picked colors and the upper colors (fitted = active in the game's rule; user's club only; off restores)");
    VirtualFree(vm_base, 0, MEM_RELEASE);
    return 0;
}
