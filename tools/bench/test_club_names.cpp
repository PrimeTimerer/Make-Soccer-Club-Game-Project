/* Display-only localization: disposable memory, no game/save writes. */
#include <cassert>
#include "../../saka_l10n.cpp"
extern "C" { uint8_t* vm_base; }
extern "C" void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}

int main(int argc, char** argv)
{
    const bool overlay = argc == 2 && !strcmp(argv[1], "overlay");
    _putenv_s("PS3_USRDIR_OVERLAY", overlay ? "fixture" : "");
    vm_base = (uint8_t*)VirtualAlloc(nullptr, 0x01000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(vm_base);
    wr32(0xEE28AC, 25);
    const char16_t* full = u"\u6771\u4eac\u30f4\u30a7\u30eb\u30c7\u30a3";
    const char16_t* shortName = u"\u6771\u4eac\uff36";
    for (unsigned i = 0; full[i]; ++i) wr16(0xEE27C0 + i * 2, full[i]);
    for (unsigned i = 0; shortName[i]; ++i) wr16(0xEE27E0 + i * 2, shortName[i]);
    uint8_t saved[64]; memcpy(saved, vm_base + 0xEE27C0, sizeof saved);
    assert(saka_localized_club_id(0xEE27C0) == (overlay ? 25 : -1));
    assert(saka_localized_club_id(0xEE27E0) == (overlay ? 25 : -1));
    assert(!memcmp(saved, vm_base + 0xEE27C0, sizeof saved));
    wr16(0xEE27C0, 'A');
    assert(saka_localized_club_id(0xEE27C0) == -1); // Owner's custom name.
    wr32(0xEE28AC, 24);
    assert(saka_localized_club_id(0xEE27E0) == -1); // Different stock club.
    wr32(0xEE28AC, 0x100);
    assert(saka_localized_club_id(0xEE27E0) == -1);
    assert(saka_localized_club_id(0x1234) == -1);
    VirtualFree(vm_base, 0, MEM_RELEASE);
    puts(overlay ? "PASS: stock full/short localization, custom names and save preservation" : "PASS: overlay disabled");
}
