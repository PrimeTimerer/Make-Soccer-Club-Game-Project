/* Disposable host test for the port's EDIT lifetime and save-extension rules.
 * argv[1]: NEW test directory. argv[2]: read-only PLED.DAT fixture.
 * Compile with clang-cl, linking saka_edit_players.c directly below.
 */
#include <assert.h>
#include "../../saka_edit_players.c"
uint8_t* vm_base;
uint32_t ppu_hle_inject_base;
void (*g_savedata_host_file_hook)(const char*, const char*, unsigned, int);

int64_t sys_memory_allocate(ppu_context* ctx) {
    wr32((uint32_t)ctx->gpr[5], 0x10000000u); return 0;
}
static unsigned imports;
static void negotiate(ppu_context* ctx) {
    uint32_t desc = (uint32_t)ctx->gpr[3], id = rd32(desc);
    assert(id == rd32(EDIT_PENDING));
    assert(vm_base[desc + 6] == 0x40 && rd32(desc + 8) == 2);
    imports++;
}
static void join(unsigned slot) {
    memcpy(vm_base + EDIT_ROSTER + slot * RECORD_SIZE, vm_base + EDIT_PENDING, RECORD_SIZE);
    memset(vm_base + EDIT_PENDING, 0, RECORD_SIZE);
}
static void release(ppu_context* ctx) {
    uint32_t id = (uint32_t)ctx->gpr[3];
    saka_edit_clear_records();
    memset(vm_base + EDIT_MASTER, 0, MASTER_SIZE);
    for (unsigned i = 0; i < 23; ++i)
        if (rd32(EDIT_ROSTER + i * RECORD_SIZE) == id) memset(vm_base + EDIT_ROSTER + i * RECORD_SIZE, 0, RECORD_SIZE);
}
static void make_native(const char* dir, const char* name, const char* bytes) {
    char path[1200]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "wb"); assert(f); assert(fwrite(bytes, 1, strlen(bytes), f) == strlen(bytes)); fclose(f);
}
int main(int argc, char** argv) {
    assert(argc == 3);
    assert(CreateDirectoryA(argv[1], NULL)); /* refuses a non-disposable existing directory */
    vm_base = VirtualAlloc(NULL, 0x20000000u, MEM_RESERVE, PAGE_READWRITE); assert(vm_base);
    assert(VirtualAlloc(vm_base + 0x00EE0000, 0xC0000, MEM_COMMIT, PAGE_READWRITE));
    assert(VirtualAlloc(vm_base + 0x02000000, 0x10000, MEM_COMMIT, PAGE_READWRITE));
    assert(VirtualAlloc(vm_base + 0x03000000, 0x10000, MEM_COMMIT, PAGE_READWRITE));
    assert(VirtualAlloc(vm_base + 0x10000000, 0x10000, MEM_COMMIT, PAGE_READWRITE));
    FILE* f = fopen(argv[2], "rb"); assert(f);
    assert(fread(vm_base + 0x03000000, 1, 0x600, f) == 0x600); fclose(f);
    assert(rd32(0x03000000) == 0x7FFF);
    ppu_context ctx; memset(&ctx, 0, sizeof ctx); ctx.gpr[1] = 0x02008000;
    for (unsigned i = 0; i < 3; ++i) {
        ctx.gpr[3] = 0x03000000; ctx.gpr[4] = 0x03000218;
        wr16(0x0300029A, 'A' + i);
        uint8_t before[0x600]; memcpy(before, vm_base + 0x03000000, sizeof before);
        saka_edit_import(&ctx, negotiate);
        assert(!memcmp(before, vm_base + 0x03000000, sizeof before));
        join(i);
    }
    assert(imports == 3 && roster_mask() == 7 && saka_edit_limit());
    assert(rd32(EDIT_ROSTER) == 0x7FFF && rd32(EDIT_ROSTER + RECORD_SIZE) == 0x7FFB && rd32(EDIT_ROSTER + 2 * RECORD_SIZE) == 0x7FFC);
    assert(vm_base[saka_edit_master(0x7FFF) + 0x83] == 'A');
    assert(vm_base[saka_edit_master(0x7FFB) + 0x83] == 'B');
    assert(vm_base[saka_edit_master(0x7FFC) + 0x83] == 'C');
    saka_edit_import(&ctx, negotiate); assert(imports == 3);
    ctx.gpr[3] = 0x7FFB; saka_edit_release(&ctx, release);
    assert(!saka_edit_limit() && vm_base[EDIT_MASTER + 0x83] == 'A');
    ctx.gpr[3] = 0x03000000; ctx.gpr[4] = 0x03000218; wr16(0x0300029A, 'Z');
    saka_edit_import(&ctx, negotiate); join(1);
    assert(imports == 4 && vm_base[saka_edit_master(0x7FFB) + 0x83] == 'Z');
    assert(vm_base[saka_edit_master(0x7FFF) + 0x83] == 'A' && vm_base[saka_edit_master(0x7FFC) + 0x83] == 'C');
    make_native(argv[1], "MAIN00.DAT", "disposable slot zero");
    saka_edit_savedata(argv[1], "MAIN00.DAT", 1, 20);
    saka_edit_title();
    saka_edit_savedata(argv[1], "MAIN00.DAT", 0, 20);
    assert(vm_base[saka_edit_master(0x7FFB) + 0x83] == 'Z');
    make_native(argv[1], "MAIN01.DAT", "disposable slot one");
    saka_edit_savedata(argv[1], "MAIN01.DAT", 0, 19);
    assert(vm_base[saka_edit_master(0x7FFB) + 0x83] == 0);
    saka_edit_savedata(argv[1], "MAIN00.DAT", 0, 20);
    assert(vm_base[saka_edit_master(0x7FFC) + 0x83] == 'C');
    make_native(argv[1], "MAIN00.DAT", "different save in same slot");
    saka_edit_savedata(argv[1], "MAIN00.DAT", 0, 26);
    assert(vm_base[saka_edit_master(0x7FFB) + 0x83] == 0);
    puts("PASS: 3 distinct masters, fourth blocked, source retained, rehire, release isolation, save reload, slot isolation, stale sidecar rejected");
    VirtualFree(vm_base, 0, MEM_RELEASE);
    return 0;
}
