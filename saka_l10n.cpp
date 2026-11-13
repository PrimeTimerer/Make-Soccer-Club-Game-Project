/*
 * Sakatsuku port: the cellL10n string conversions this EBOOT imports, plus
 * cellOskDialogSetKeyLayoutOption (see NOTES.md, "name input").
 *
 * Before opening the PS3 keyboard for the passport NAME field the game converts
 * its prompt with SJISstoUTF8s and UTF8stoUTF16s. Neither had an HLE handler
 * (unresolved NID 0x33435818 / 0xF7681B9A), nor did SetKeyLayoutOption
 * (0xB53C54FA): the conversions failed, cellOskDialogLoadAsync was never
 * reached, and the screen stayed black. ps3recomp's libs/font/cellL10n.c has
 * host-pointer versions of some of these that are not registered by NID.
 *
 * Semantics follow RPCS3's cellL10n.cpp (behavioural reference only): result
 * codes ConversionOK=0, SRCIllegal=1, DSTExhausted=2, ConverterUnknown=3;
 * *src_len / *dst_len are guest s32 lengths in source / destination units, and
 * *dst_len holds the capacity on entry. Shift-JIS is Windows code page 932,
 * which is also what RPCS3 converts through on Windows.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ps3recomp/runtime/ppu/ppu_context.h"

extern "C" uint8_t* vm_base;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

namespace {

enum { kOK = 0, kSrcIllegal = 1, kDstExhausted = 2, kUnknown = 3 };

inline uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline void     wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
inline uint16_t rd16(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint16_t)(p[0] << 8 | p[1]); }
inline void     wr16(uint32_t ea, uint16_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* SJISstoUTF8s(src, src_len*, dst, dst_len*): code page 932 -> UTF-8, as RPCS3's
 * _L10nConvertStr/_ConvertStr does on Windows: both Win32 calls with flags 0
 * (invalid input is not rejected) and length -1 (the source is read to its
 * NUL; src_len does not bound it); the output excludes the NUL and none is
 * written; null dst = size query; output longer than *dst_len = DSTExhausted
 * with *dst_len unchanged; else copy and *dst_len = size. */
void l10n_SJISstoUTF8s(ppu_context* ctx)
{
    const uint32_t src = (uint32_t)ctx->gpr[3];
    const uint32_t dst = (uint32_t)ctx->gpr[5], dst_len = (uint32_t)ctx->gpr[6];
    if (!src || !dst_len) { ctx->gpr[3] = kSrcIllegal; return; }
    const char* s = (const char*)(vm_base + src);
    const int wn = MultiByteToWideChar(932, 0, s, -1, nullptr, 0);   /* counts the NUL */
    std::vector<wchar_t> w((size_t)(wn > 0 ? wn : 1), L'\0');
    if (wn > 0) MultiByteToWideChar(932, 0, s, -1, w.data(), wn);
    const int un = WideCharToMultiByte(CP_UTF8, 0, w.data(), -1, nullptr, 0, nullptr, nullptr);
    std::vector<char> u((size_t)(un > 0 ? un : 1), '\0');
    if (un > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), -1, u.data(), un, nullptr, nullptr);
    const uint32_t size = (uint32_t)strlen(u.data());
    { static int k = 0; if (k++ < 8) printf("[l10n] SJISstoUTF8s -> %u bytes (cap %d)\n", size, (int)rd32(dst_len)); }
    if (!dst) { wr32(dst_len, size); ctx->gpr[3] = kOK; return; }
    if (size > rd32(dst_len)) { ctx->gpr[3] = kDstExhausted; return; }
    memcpy(vm_base + dst, u.data(), size);
    wr32(dst_len, size);
    ctx->gpr[3] = kOK;
}

/* One UTF-8 character at p (avail bytes): code point and bytes used, 0 on an
 * invalid or truncated sequence. */
int utf8_decode(const uint8_t* p, uint32_t avail, uint32_t* cp)
{
    const uint8_t c = p[0];
    if (c < 0x80) { *cp = c; return 1; }
    int n; uint32_t v;
    if (c >= 0xC2 && c <= 0xDF)      { n = 2; v = c & 0x1Fu; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; v = c & 0x0Fu; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; v = c & 0x07u; }
    else return 0;
    if (avail < (uint32_t)n) return 0;
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0u) != 0x80u) return 0;
        v = v << 6 | (p[i] & 0x3Fu);
    }
    if ((n == 3 && v < 0x800) || (n == 4 && (v < 0x10000 || v > 0x10FFFF)) || (v >= 0xD800 && v <= 0xDFFF)) return 0;
    *cp = v;
    return n;
}

/* UTF8stoUTF16s(src, src_len*, dst u16*, dst_len*), as RPCS3: walk exactly
 * *src_len bytes one character at a time -- a 0x00 byte is a character like any
 * other, the loop does not stop there -- writing big-endian units while they fit.
 * Too small: *dst_len = units written, DSTExhausted. Invalid: *src_len -= bytes
 * done, *dst_len = units, SRCIllegal. No terminator is written. Sakatsuku asks
 * len=1024 cap=16 and takes the DSTExhausted answer, as it does on RPCS3. */
void l10n_UTF8stoUTF16s(ppu_context* ctx)
{
    const uint32_t src = (uint32_t)ctx->gpr[3], src_len = (uint32_t)ctx->gpr[4];
    const uint32_t dst = (uint32_t)ctx->gpr[5], dst_len = (uint32_t)ctx->gpr[6];
    if (!src || !src_len || !dst_len) { ctx->gpr[3] = kSrcIllegal; return; }
    const uint32_t n = rd32(src_len);
    if (n == 0) { wr32(dst_len, 0); ctx->gpr[3] = kOK; return; }
    const uint32_t cap = dst ? rd32(dst_len) : 0;
    uint32_t pos = 0, len = 0;
    int rc = kOK;
    while (pos < n) {
        uint32_t cp = 0;
        const int k = utf8_decode(vm_base + src + pos, n - pos, &cp);
        if (!k) { wr32(src_len, n - pos); wr32(dst_len, len); rc = kSrcIllegal; break; }
        const uint32_t units = cp >= 0x10000 ? 2u : 1u;
        if (dst) {
            if (len + units > cap) { wr32(dst_len, len); rc = kDstExhausted; break; }
            if (units == 1) wr16(dst + len * 2, (uint16_t)cp);
            else {
                const uint32_t v = cp - 0x10000;
                wr16(dst + len * 2, (uint16_t)(0xD800 | (v >> 10)));
                wr16(dst + len * 2 + 2, (uint16_t)(0xDC00 | (v & 0x3FF)));
            }
        }
        len += units; pos += (uint32_t)k;
    }
    if (rc == kOK) wr32(dst_len, len);
    { static int c = 0; if (c++ < 8) printf("[l10n] UTF8stoUTF16s(len=%u cap=%u) -> rc=%d units=%u\n", n, cap, rc, len); }
    ctx->gpr[3] = (uint64_t)rc;
}

/* UTF16toUTF8(src u16*, dst u8*, dst_len*): one character (or surrogate pair).
 * Returns the UTF-16 units consumed (1 or 2), 0 for a broken surrogate pair;
 * *dst_len gets the UTF-8 bytes written. */
void l10n_UTF16toUTF8(ppu_context* ctx)
{
    const uint32_t src = (uint32_t)ctx->gpr[3], dst = (uint32_t)ctx->gpr[4], dst_len = (uint32_t)ctx->gpr[5];
    if (!src || !dst || !dst_len) { ctx->gpr[3] = 0; return; }
    uint32_t cp = rd16(src), used = 1;
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        const uint32_t lo = rd16(src + 2);
        if (lo < 0xDC00 || lo > 0xDFFF) { ctx->gpr[3] = 0; return; }
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        used = 2;
    }
    uint8_t b[4]; uint32_t n;
    if (cp < 0x80)         { b[0] = (uint8_t)cp; n = 1; }
    else if (cp < 0x800)   { b[0] = (uint8_t)(0xC0 | cp >> 6);  b[1] = (uint8_t)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { b[0] = (uint8_t)(0xE0 | cp >> 12); b[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (uint8_t)(0x80 | (cp & 0x3F)); n = 3; }
    else                   { b[0] = (uint8_t)(0xF0 | cp >> 18); b[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (uint8_t)(0x80 | (cp & 0x3F)); n = 4; }
    memcpy(vm_base + dst, b, n);
    wr32(dst_len, n);
    ctx->gpr[3] = used;
}

/* UTF8toSJIS(ch u8, dst u8*, dst_len*): one BYTE, as RPCS3 does (it "doesn't
 * work backwards"). An ASCII byte converts; a byte of a multi-byte sequence is
 * incomplete on its own and reports dst_len -1. */
void l10n_UTF8toSJIS(ppu_context* ctx)
{
    const uint8_t ch = (uint8_t)ctx->gpr[3];
    const uint32_t dst = (uint32_t)ctx->gpr[4], dst_len = (uint32_t)ctx->gpr[5];
    if (!dst_len) { ctx->gpr[3] = kSrcIllegal; return; }
    if (ch < 0x80) {
        if (dst) vm_base[dst] = ch;
        wr32(dst_len, 1);
    } else {
        wr32(dst_len, 0xFFFFFFFFu);   /* -1: incomplete input */
    }
    ctx->gpr[3] = kOK;
}

/* cellOskDialogSetKeyLayoutOption(option): only shapes the PS3 keyboard's
 * layout; the host input box has none. */
void osk_SetKeyLayoutOption(ppu_context* ctx)
{
    printf("[cellOskDialog] SetKeyLayoutOption(0x%X)\n", (unsigned)ctx->gpr[3]);
    ctx->gpr[3] = 0;   /* CELL_OK */
}

} // namespace

extern "C" void saka_register_l10n(void)
{
    ps3_hle_register_ctx(0x33435818u, "SJISstoUTF8s",                    l10n_SJISstoUTF8s);
    ps3_hle_register_ctx(0xF7681B9Au, "UTF8stoUTF16s",                   l10n_UTF8stoUTF16s);
    ps3_hle_register_ctx(0x317AB7C2u, "UTF16toUTF8",                     l10n_UTF16toUTF8);
    ps3_hle_register_ctx(0xEB3DC670u, "UTF8toSJIS",                      l10n_UTF8toSJIS);
    ps3_hle_register_ctx(0xB53C54FAu, "cellOskDialogSetKeyLayoutOption", osk_SetKeyLayoutOption);
}

/* An existing save stores its stock club name independently of text tables.
 * With a text overlay, return the stock table ID only when the saved name is
 * still the original J-club name. Custom names and save bytes stay untouched.
 * Original strings: BLJM61064 txt_static_data tables 3/4, IDs 1..40. */
extern "C" int saka_localized_club_id(uint32_t name_ea)
{
    if (name_ea != 0x00EE27C0u && name_ea != 0x00EE27E0u) return -1;
    static const bool overlay = [] {
        const char* root = std::getenv("PS3_USRDIR_OVERLAY");
        return root && *root;
    }();
    if (!overlay) return -1;
    const uint32_t id = rd32(0x00EE28ACu);
    if (id < 1 || id > 40) return -1;
    static const char16_t* const names[40][2] = {
        {u"\u4ed9\u53f0", u"\u30d9\u30ac\u30eb\u30bf\u4ed9\u53f0"}, // 1
        {u"\u9e7f\u5cf6", u"\u9e7f\u5cf6\u30a2\u30f3\u30c8\u30e9\u30fc\u30ba"}, // 2
        {u"\u6d66\u548c", u"\u6d66\u548c\u30ec\u30c3\u30ba"}, // 3
        {u"\u5927\u5bae", u"\u5927\u5bae\u30a2\u30eb\u30c7\u30a3\u30fc\u30b8\u30e3"}, // 4
        {u"\u67cf", u"\u67cf\u30ec\u30a4\u30bd\u30eb"}, // 5
        {u"\uff26\u6771\u4eac", u"\uff26\uff23\u6771\u4eac"}, // 6
        {u"\u5ddd\u5d0e\uff26", u"\u5ddd\u5d0e\u30d5\u30ed\u30f3\u30bf\u30fc\u30ec"}, // 7
        {u"\u6a2a\u6d5c\uff26\uff2d", u"\u6a2a\u6d5c\uff26\u30fb\u30de\u30ea\u30ce\u30b9"}, // 8
        {u"\u6e58\u5357", u"\u6e58\u5357\u30d9\u30eb\u30de\u30fc\u30ec"}, // 9
        {u"\u7532\u5e9c", u"\u30f4\u30a1\u30f3\u30d5\u30a9\u30fc\u30ec\u7532\u5e9c"}, // 10
        {u"\u65b0\u6f5f", u"\u30a2\u30eb\u30d3\u30ec\u30c3\u30af\u30b9\u65b0\u6f5f"}, // 11
        {u"\u6e05\u6c34", u"\u6e05\u6c34\u30a8\u30b9\u30d1\u30eb\u30b9"}, // 12
        {u"\u78d0\u7530", u"\u30b8\u30e5\u30d3\u30ed\u78d0\u7530"}, // 13
        {u"\u540d\u53e4\u5c4b", u"\u540d\u53e4\u5c4b\u30b0\u30e9\u30f3\u30d1\u30b9"}, // 14
        {u"\uff23\u5927\u962a", u"\u30bb\u30ec\u30c3\u30bd\u5927\u962a"}, // 15
        {u"\u5e83\u5cf6", u"\u30b5\u30f3\u30d5\u30ec\u30c3\u30c1\u30a7\u5e83\u5cf6"}, // 16
        {u"\u9ce5\u6816", u"\u30b5\u30ac\u30f3\u9ce5\u6816"}, // 17
        {u"\u5927\u5206", u"\u5927\u5206\u30c8\u30ea\u30cb\u30fc\u30bf"}, // 18
        {u"\u672d\u5e4c", u"\u30b3\u30f3\u30b5\u30c9\u30fc\u30ec\u672d\u5e4c"}, // 19
        {u"\u5c71\u5f62", u"\u30e2\u30f3\u30c6\u30c7\u30a3\u30aa\u5c71\u5f62"}, // 20
        {u"\u6c34\u6238", u"\u6c34\u6238\u30db\u30fc\u30ea\u30fc\u30db\u30c3\u30af"}, // 21
        {u"\u6803\u6728", u"\u6803\u6728\uff33\uff23"}, // 22
        {u"\u7fa4\u99ac", u"\u30b6\u30b9\u30d1\u30af\u30b5\u30c4\u7fa4\u99ac"}, // 23
        {u"\u5343\u8449", u"\u30b8\u30a7\u30d5\u30e6\u30ca\u30a4\u30c6\u30c3\u30c9\u5343\u8449"}, // 24
        {u"\u6771\u4eac\uff36", u"\u6771\u4eac\u30f4\u30a7\u30eb\u30c7\u30a3"}, // 25
        {u"\u6a2a\u6d5c\uff26\uff23", u"\u6a2a\u6d5c\uff26\uff23"}, // 26
        {u"\u677e\u672c", u"\u677e\u672c\u5c71\u96c5\uff26\uff23"}, // 27
        {u"\u5bcc\u5c71", u"\u30ab\u30bf\u30fc\u30ec\u5bcc\u5c71"}, // 28
        {u"\uff26\uff23\u5c90\u961c", u"\uff26\uff23\u5c90\u961c"}, // 29
        {u"\u4eac\u90fd", u"\u4eac\u90fd\u30b5\u30f3\u30ac\uff26\uff0e\uff23\uff0e"}, // 30
        {u"\uff27\u5927\u962a", u"\u30ac\u30f3\u30d0\u5927\u962a"}, // 31
        {u"\u795e\u6238", u"\u30f4\u30a3\u30c3\u30bb\u30eb\u795e\u6238"}, // 32
        {u"\u9ce5\u53d6", u"\u30ac\u30a4\u30ca\u30fc\u30ec\u9ce5\u53d6"}, // 33
        {u"\u5ca1\u5c71", u"\u30d5\u30a1\u30b8\u30a2\u30fc\u30ce\u5ca1\u5c71"}, // 34
        {u"\u5fb3\u5cf6", u"\u5fb3\u5cf6\u30f4\u30a9\u30eb\u30c6\u30a3\u30b9"}, // 35
        {u"\u611b\u5a9b", u"\u611b\u5a9b\uff26\uff23"}, // 36
        {u"\u798f\u5ca1", u"\u30a2\u30d3\u30b9\u30d1\u798f\u5ca1"}, // 37
        {u"\u5317\u4e5d\u5dde", u"\u30ae\u30e9\u30f4\u30a1\u30f3\u30c4\u5317\u4e5d\u5dde"}, // 38
        {u"\u9577\u5d0e", u"\uff36\u30fb\u30d5\u30a1\u30fc\u30ec\u30f3\u9577\u5d0e"}, // 39
        {u"\u718a\u672c", u"\u30ed\u30a2\u30c3\u30bd\u718a\u672c"}, // 40
    };
    const char16_t* original = names[id - 1][name_ea == 0x00EE27C0u];
    for (unsigned i = 0; i < 16; ++i) {
        if (rd16(name_ea + i * 2) != original[i]) return -1;
        if (!original[i]) return (int)id;
    }
    return -1;
}
