// saka_overlay.cpp - the port's in-game cheat menu (owner: "야구츠쿠3처럼 인게임에서도 작동해야", 2026-11-08).
//
// F2 (or L3+R3 on a pad) opens and closes a Dear ImGui window that the live renderer draws over the game's back
// buffer right before the present (rsx_live_draw_overlay_draw). While it is open the game gets a neutral pad
// (cellPad_overlay_hook) and the window's keyboard and mouse go to the menu (rsx_window_message_hook; Esc closes the
// menu instead of the game). Pad: d-pad moves, ○ chooses, × goes back, L1/R1 switch tabs.
// The panels do what the launcher's 치트 / 선수 / 목록 pages do, through the same files the cheat thread reads
// (cheats\enabled.txt, values.txt, players.txt, lists.txt: saka_hooks.c), and read the game's memory for the
// current values and the game's text archive for the names. SAKA_OVERLAY=0 turns it off; SAKA_OVERLAY_OPEN=1 opens
// it at start (tests).
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "imgui.h"
#include "imgui_internal.h"   // AddSettingsHandler: the menu zoom is saved with the layout
#include "backends/imgui_impl_dx12.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
extern uint8_t* vm_base;
extern int (*rsx_window_message_hook)(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp);
extern int (*rsx_live_draw_overlay_on)(void);
extern void (*rsx_live_draw_overlay_draw)(void* dev, void* queue, void* list, void* backbuffer, size_t rtv,
                                          unsigned width, unsigned height);
extern int (*cellPad_overlay_hook)(unsigned port, unsigned buttons);
extern volatile int g_saka_user_speed;   // main.cpp: the game speed (vblank x N), 1 = normal
int saka_edit_copy_master(uint32_t id, void* out);
uint32_t saka_edit_master(uint32_t id);
}

namespace {

// ---- state shared between the window thread, the game's pad reads and the render thread ---------------------
std::atomic<bool> g_open{false};
std::atomic<unsigned> g_pad{0};       // port 0's buttons at the last read (CELL_PAD_CTRL_* packed, see cellPad.c)
std::atomic<int> g_clientW{0}, g_clientH{0};
struct Msg { unsigned msg; uintptr_t wp; intptr_t lp; };
std::mutex g_msgMutex;
std::vector<Msg> g_msgs;
std::atomic<bool> g_reload{true};      // read the files again (the menu was opened)

void setOpen(bool open)
{
    if (g_open.exchange(open) != open) {
        if (open)
            g_reload = true;
        std::fprintf(stderr, "[overlay] %s\n", open ? "opened" : "closed");
    }
}

// F7: x2 on/off, F8: turbo (x6) on/off, as in the YKTK3 port.
std::atomic<int> g_speedShownAt{0};
void setSpeed(int speed)
{
    g_saka_user_speed = speed;
    g_speedShownAt = (int)GetTickCount();
    std::fprintf(stderr, "[overlay] game speed x%d\n", speed);
}
bool speedBadge();   // below: the speed is up, or was changed in the last 2 s

// F6: super play (2026-11-13, owner: "슈퍼플레이 ... 버튼을 눌러서 마음대로 작동"): saka_hooks.c switch 23 arms the next
// play of the user's team (2026-11-14: until it is used, with every skill that fits the play); the badge says so, or
// that the cheat is off.
extern "C" int saka_superplay_arm(void);
extern "C" int saka_superplay_state(void);   // 0 idle, 1 armed, 2 used in the last 3 s
#include "saka_teamcolor.h"   // the team color fit's state (2026-11-14)
std::atomic<int> g_superShownAt{-100000};
std::atomic<int> g_superArmed{0};

int windowHook(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp)
{
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp == VK_F6) {
        if (!(lp & (1 << 30))) {
            g_superArmed = saka_superplay_arm();
            g_superShownAt = (int)GetTickCount();
        }
        return 1;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && (wp == VK_F7 || wp == VK_F8)) {
        if (!(lp & (1 << 30))) {
            const int want = wp == VK_F7 ? 2 : 6;
            setSpeed(g_saka_user_speed == want ? 1 : want);
        }
        return 1;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp == VK_F2) {
        if (!(lp & (1 << 30)))
            setOpen(!g_open);
        return 1;
    }
    if (!g_open)
        return 0;
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wp == VK_ESCAPE) {
            setOpen(false);
            return 1;
        }
        break;
    case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_IME_CHAR:
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL:
        break;
    default:
        return 0;
    }
    RECT rc;
    if (GetClientRect((HWND)hwnd, &rc)) {
        g_clientW = rc.right - rc.left;
        g_clientH = rc.bottom - rc.top;
    }
    std::lock_guard<std::mutex> lock(g_msgMutex);
    if (g_msgs.size() < 512)
        g_msgs.push_back({msg, wp, lp});
    return 1;
}

int padHook(unsigned port, unsigned buttons)
{
    if (port == 0) {
        g_pad = buttons;
        static bool held = false;   // L3 (0x02) + R3 (0x04) together: open / close
        const bool now = (buttons & 0x6u) == 0x6u;
        if (now && !held)
            setOpen(!g_open);
        held = now;
    }
    return g_open ? 1 : 0;
}

// ---- guest memory and text ----------------------------------------------------------------------------------
uint32_t rd8(uint32_t ea) { return vm_base ? vm_base[ea] : 0; }
uint32_t rd16(uint32_t ea) { return vm_base ? (uint32_t)(vm_base[ea] << 8 | vm_base[ea + 1]) : 0; }
uint32_t rd32(uint32_t ea)
{
    return vm_base ? (uint32_t)vm_base[ea] << 24 | (uint32_t)vm_base[ea + 1] << 16 | (uint32_t)vm_base[ea + 2] << 8 | vm_base[ea + 3] : 0;
}
bool committed(uint32_t ea, uint32_t n)
{
    if (!vm_base)
        return false;
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(vm_base + ea, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
           (uint8_t*)mbi.BaseAddress + mbi.RegionSize >= vm_base + ea + n;
}
bool validPlayer(uint32_t id) { return (id > 0 && id < 0x46A2u) || (id >= 0x7FFBu && id <= 0x7FFFu); }
// A button at the right end of the line a tab bar is about to take (2026-11-09, owner: "리스트 리프레시는 오른쪽에
// 위치해야하고", "팀도 그렇고"; a Trailing tab button sits right after the tabs, not at the right end). Drawn first, then
// the cursor goes back so the tab bar starts on the same line; the tab bar adds no item over its empty part.
bool rightEndButton(const char* label)
{
    const ImVec2 start = ImGui::GetCursorPos();
    const float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(start.x + ImGui::GetContentRegionAvail().x - w);
    const bool pressed = ImGui::SmallButton(label);
    ImGui::SetCursorPos(start);
    return pressed;
}

std::string utf8(const wchar_t* w, int n)
{
    if (n <= 0)
        return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, &s[0], len, nullptr, nullptr);
    return s;
}

using Names = std::map<int, std::string>;

// One table of a text archive (tools/saka_text.py): u16 kind, u16 count, count x (u32 id, u32 offset), strings.
Names textTable(const std::vector<uint8_t>& d, size_t t)
{
    Names names;
    if (t + 4 > d.size())
        return names;
    const int count = d[t + 2] | d[t + 3] << 8;
    const size_t strings = t + 4 + 8 * (size_t)count;
    for (int e = 0; e < count; e++) {
        const size_t at = t + 4 + 8 * (size_t)e;
        if (at + 8 > d.size())
            break;
        uint32_t id, off;
        std::memcpy(&id, &d[at], 4);
        std::memcpy(&off, &d[at + 4], 4);
        const size_t p = strings + off;
        if (p + 2 > d.size())
            continue;
        const size_t len = d[p] | d[p + 1] << 8;
        if (p + 2 + len > d.size())
            continue;
        std::wstring w((const wchar_t*)&d[p + 2], len / 2);
        while (!w.empty() && w.back() == L'\0')
            w.pop_back();
        names[(int)id] = utf8(w.c_str(), (int)w.size());
    }
    return names;
}

std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string dataPath(const char* relative)
{
    const char* hdd0 = std::getenv("PS3_HDD0_ROOT");
    const char* bdvd = std::getenv("PS3_VFS_ROOT");
    std::string patch = std::string(hdd0 ? hdd0 : "game\\dev_hdd0") + "\\game\\BLJM61064\\USRDIR\\data\\" + relative;
    if (GetFileAttributesA(patch.c_str()) != INVALID_FILE_ATTRIBUTES)
        return patch;
    return std::string(bdvd ? bdvd : "game\\dev_bdvd") + "\\PS3_GAME\\USRDIR\\data\\" + relative;
}

std::vector<std::string> readLines(const char* path);   // below (cheats files); loadTexts reads legends.txt
std::vector<std::string> split(const std::string& s, char c);   // below; loadTexts reads playstyles.txt

struct Texts {
    bool loaded = false;
    Names players, nations, staff, scouts;
    std::vector<std::string> abilities;
    Names styles, skillTypes, skills;   // data\text2: playstyle_name, skill_type_name, skill_name (.txb)
    // Training courses (data\text\txt_practice_data.arch: 0 special training, 2 個性強化, 6 能力覚醒) and the staff
    // abilities (data\text2\staff_sp_name.txb), 2026-11-13.
    Names spCourses, persCourses, awakCourses, staffAbilities;
    // cheats\playstyles.txt (tools/make_playstyles.py): Korean names, tier (1 base, 2 upper, 3 top) and family (the
    // base style) of each play style from the style master 0x27; Korean names of skill types (and their level-7 name)
    // and original skills (2026-11-09, owner: "번역도 해줘 ... 계열중에서 하이클래스인지도 구분").
    struct Style { std::string ko, ja; int tier = 0, family = 0; };
    std::map<int, Style> styleInfo;
    std::map<int, std::pair<std::string, std::string>> typeKo;   // korean, level-7 korean
    std::map<int, std::string> origKo;
} g_text;

void loadTexts()
{
    if (g_text.loaded)
        return;
    g_text.loaded = true;
    std::vector<uint8_t> d = readFile(dataPath("text\\txt_static_data.arch"));
    if (d.size() > 0x20) {
        uint32_t n;
        std::memcpy(&n, &d[0], 4);
        const size_t start = 0x10 + 4 * (size_t)n;
        auto table = [&](uint32_t k) {
            uint32_t off;
            std::memcpy(&off, &d[0x10 + 4 * k], 4);
            return textTable(d, start + off);
        };
        if (n > 10) {
            g_text.players = table(0);
            g_text.nations = table(2);
            g_text.staff = table(9);
            g_text.scouts = table(10);
        }
    }
    // Legend tags after the fictional stand-ins for real legends: cheats\legends.txt, "player ID<TAB>label" (as the
    // launcher's GameText.Players).
    for (const std::string& line : readLines("cheats\\legends.txt")) {
        if (line.empty() || line[0] == '#')
            continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos)
            continue;
        const int id = std::atoi(line.substr(0, tab).c_str());
        std::string label = line.substr(tab + 1);
        while (!label.empty() && (label.back() == '\r' || label.back() == ' '))
            label.pop_back();
        auto it = g_text.players.find(id);
        if (it != g_text.players.end() && !label.empty())
            it->second += " (레전드: " + label + ")";
    }
    // Real-person tags for coaches and scouts (2026-11-13, owner: "코치와 스카우트도 실존 인물들의 이름이 있어야 해"):
    // cheats\staff_models.txt, "coach|scout<TAB>ID<TAB>label" (as the launcher's GameText.Staff / Scouts).
    for (const std::string& line : readLines("cheats\\staff_models.txt")) {
        if (line.empty() || line[0] == '#')
            continue;
        const std::vector<std::string> f = split(line, '\t');
        if (f.size() < 3)
            continue;
        Names* names = f[0] == "coach" ? &g_text.staff : f[0] == "scout" ? &g_text.scouts : nullptr;
        std::string label = f[2];
        while (!label.empty() && (label.back() == '\r' || label.back() == ' '))
            label.pop_back();
        if (!names || label.empty())
            continue;
        auto it = names->find(std::atoi(f[1].c_str()));
        if (it != names->end())
            it->second += " (실존: " + label + ")";
    }
    {   // play styles (1-66), skill types (1-16), skills ((type-1)*7+level 1-112, original 113-181)
        std::vector<uint8_t> s = readFile(dataPath("text2\\playstyle_name.txb"));
        g_text.styles = textTable(s, 0);
        std::vector<uint8_t> st = readFile(dataPath("text2\\skill_type_name.txb"));
        g_text.skillTypes = textTable(st, 0);
        std::vector<uint8_t> sk = readFile(dataPath("text2\\skill_name.txb"));
        g_text.skills = textTable(sk, 0);
    }
    for (const std::string& line : readLines("cheats\\playstyles.txt")) {
        std::vector<std::string> f = split(line, '\t');
        for (std::string& x : f)
            while (!x.empty() && (x.back() == '\r' || x.back() == ' '))
                x.pop_back();
        if (f.size() >= 6 && f[0] == "style") {
            Texts::Style& s = g_text.styleInfo[std::atoi(f[1].c_str())];
            s.ko = f[2];
            s.ja = f[3];
            s.tier = std::atoi(f[4].c_str());
            s.family = std::atoi(f[5].c_str());
        } else if (f.size() >= 5 && f[0] == "type") {
            g_text.typeKo[std::atoi(f[1].c_str())] = {f[2], f[4]};
        } else if (f.size() >= 3 && f[0] == "orig") {
            g_text.origKo[std::atoi(f[1].c_str())] = f[2];
        }
    }
    {
        std::vector<uint8_t> p = readFile(dataPath("text\\txt_practice_data.arch"));
        if (p.size() > 0x20) {
            uint32_t n;
            std::memcpy(&n, &p[0], 4);
            const size_t start = 0x10 + 4 * (size_t)n;
            auto table = [&](uint32_t k) {
                uint32_t off;
                std::memcpy(&off, &p[0x10 + 4 * k], 4);
                return textTable(p, start + off);
            };
            if (n > 6) {
                g_text.spCourses = table(0);
                g_text.persCourses = table(2);
                g_text.awakCourses = table(6);
            }
        }
        g_text.staffAbilities = textTable(readFile(dataPath("text2\\staff_sp_name.txb")), 0);
    }
    std::vector<uint8_t> a = readFile(dataPath("text3\\pledit_ability_name.txb"));
    Names ab = textTable(a, 0);
    for (int i = 0; i < 46; i++)
        g_text.abilities.push_back(ab.count(i) ? ab[i] : "능력 " + std::to_string(i));
}

std::string nameOf(const Names& names, int id)
{
    if (&names == &g_text.players && (id == 0x7FFF || id == 0x7FFB || id == 0x7FFC)) {
        uint8_t master[0x1A8];
        if (saka_edit_copy_master((uint32_t)id, master)) {
            wchar_t text[19] = {};
            for (int k = 0; k < 18; ++k) text[k] = (wchar_t)(master[0x82 + k * 2] << 8 | master[0x83 + k * 2]);
            if (text[0]) return utf8(text, (int)wcslen(text));
        }
    }
    auto it = names.find(id);
    return it != names.end() && !it->second.empty() ? it->second : "#" + std::to_string(id);
}

// Nationality from the game's master tables in memory (NOTES.md "Cheats v2"; launcher GameData).
int playerNation(int id)
{
    if (id >= 0x7FFB && id <= 0x7FFF) {
        uint8_t master[0x1A8];
        return saka_edit_copy_master((uint32_t)id, master) ? master[2] : -1;
    }
    const uint32_t ea = id < 10000 ? 0x00C7D6CCu + (uint32_t)id * 0x3Cu : 0x00D9A080u + (uint32_t)(id - 10000) * 0x3Cu;
    return committed(ea + 2, 1) ? (int)rd8(ea + 2) : -1;
}
int staffNation(int id) { return committed(0x00BBA60Cu + (uint32_t)id * 100u + 4, 4) ? (int)rd32(0x00BBA60Cu + (uint32_t)id * 100u + 4) : -1; }
int scoutNation(int id) { return committed(0x00E1F6ACu + (uint32_t)id * 60u + 4, 4) ? (int)rd32(0x00E1F6ACu + (uint32_t)id * 60u + 4) : -1; }

// Grade and age from the master tables, for sorting (2026-11-13, owner: "국적 능력치(등급) 번호 나이 이름순으로 sorting"):
// coaches 0x00BBA60C (100 bytes: +8 base age, +9 rank: 15 SS, 14 S, 13 A by the guides; 11-12 about B, 9-10 about C,
// 7-8 about D by the blog's lists, which overlap), scouts 0x00E1F6AC (60 bytes: +8 rank, no age), players' master
// (0x3C bytes: +0x10 base age, +0x13 grade 0-14, legends 14). -1 when unknown. L: 0 coach, 1 scout, 2 player.
uint32_t playerMaster(int id) { return id >= 0x7FFB && id <= 0x7FFF ? saka_edit_master((uint32_t)id) : id < 10000 ? 0x00C7D6CCu + (uint32_t)id * 0x3Cu : 0x00D9A080u + (uint32_t)(id - 10000) * 0x3Cu; }
int personGrade(int L, int id)
{
    const uint32_t ea = L == 0 ? 0x00BBA60Cu + (uint32_t)id * 100u + 9 : L == 1 ? 0x00E1F6ACu + (uint32_t)id * 60u + 8 : playerMaster(id) + 0x13;
    return committed(ea, 1) ? (int)rd8(ea) : -1;
}
int personAge(int L, int id)
{
    if (L == 1)
        return -1;
    const uint32_t ea = L == 0 ? 0x00BBA60Cu + (uint32_t)id * 100u + 8 : playerMaster(id) + 0x10;
    return committed(ea, 1) ? (int)rd8(ea) : -1;
}
std::string gradeText(int L, int g)
{
    if (g < 0)
        return "?";
    if (L != 0)
        return std::to_string(g);
    const char* r = g >= 15 ? "SS" : g == 14 ? "S" : g == 13 ? "A" : g >= 11 ? "B" : g >= 9 ? "C" : g >= 7 ? "D" : "E";
    return std::string(r) + "(" + std::to_string(g) + ")";
}
// Where a name's added tag starts (" (레전드: ...)" or " (실존: ...)"), npos without one.
size_t tagPos(const std::string& s) { return std::min(s.find(" (레전드"), s.find(" (실존")); }
const char* const kSortNames[5] = {"번호", "이름", "국적", "등급", "나이"};

// ---- the cheat files ----------------------------------------------------------------------------------------
std::vector<std::string> readLines(const char* path)
{
    std::vector<std::string> lines;
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (lines.empty() && line.size() >= 3 && (uint8_t)line[0] == 0xEF && (uint8_t)line[1] == 0xBB && (uint8_t)line[2] == 0xBF)
            line.erase(0, 3);
        lines.push_back(line);
    }
    return lines;
}

void writeLines(const char* path, const std::vector<std::string>& lines)
{
    CreateDirectoryA("cheats", nullptr);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    for (const std::string& l : lines)
        f << l << "\r\n";
}

std::vector<std::string> split(const std::string& s, char c)
{
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == c) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    out.push_back(cur);
    return out;
}

struct CheatValue { int index = 0; std::string label, kind; long long min = 0, max = 0, def = 0; };
struct Cheat {
    std::string name, title, origin, category, note;
    int codes = 0;
    bool onceDefault = false, isSwitch = false, lockable = false;   // lockable: "#@lock" (a switch that honours 고정/갱신)
    std::vector<CheatValue> values;
    std::vector<std::string> codeLines;   // in the game's numbering (multi-write second lines left out)
};

struct CheatFiles {
    std::vector<Cheat> cheats;
    std::set<std::string> enabled;
    std::map<std::string, std::string> values;   // "name\tkey" -> value
} g_files;

// The settings files of the save in use (2026-11-09, owner: per save as in YKTK3): saka_hooks.c says which
// ("cheats/" at the title and in EDIT, "cheats/saves/MAIN00/" for slot 1, "cheats/saves/new/" for a new game).
extern "C" long saka_cheat_profile(char* dir, size_t dir_n, char* desc, size_t desc_n);
std::string profileFile(const char* name)
{
    char dir[64] = "cheats/";
    saka_cheat_profile(dir, sizeof dir, nullptr, 0);
    return std::string(dir) + name;
}

void loadCheats()
{
    g_files = CheatFiles();
    std::string category = "기타";
    bool second = false;
    for (const std::string& raw : readLines("cheats\\BLJM61064.txt")) {
        std::string line = raw;
        while (!line.empty() && (line[0] == ' ' || line[0] == '\t'))
            line.erase(0, 1);
        Cheat* last = g_files.cheats.empty() ? nullptr : &g_files.cheats.back();
        if (line.rfind("#@cat ", 0) == 0) {
            category = line.substr(6);
        } else if (line.size() > 3 && line[0] == '_' && line[1] == 'V' && (line[2] == '0' || line[2] == '1')) {
            Cheat c;
            c.name = line.substr(3);
            while (!c.name.empty() && c.name[0] == ' ')
                c.name.erase(0, 1);
            c.category = category;
            const size_t paren = c.name.rfind(" (");
            if (paren != std::string::npos && c.name.back() == ')') {
                c.title = c.name.substr(0, paren);
                c.origin = c.name.substr(paren + 2, c.name.size() - paren - 3);
            } else {
                c.title = c.name;
            }
            g_files.cheats.push_back(c);
            second = false;
        } else if (last && line.rfind("#@note ", 0) == 0) {
            last->note = line.substr(7);
        } else if (last && line == "#@once") {
            last->onceDefault = true;
        } else if (last && line == "#@lock") {
            last->lockable = true;
        } else if (last && line.rfind("#@value ", 0) == 0) {
            std::vector<std::string> f = split(line.substr(8), '|');
            if (f.size() >= 5) {
                CheatValue v;
                v.index = std::atoi(f[0].c_str());
                v.label = f[1];
                v.min = std::atoll(f[2].c_str());
                v.max = std::atoll(f[3].c_str());
                v.kind = f[4];
                last->values.push_back(v);
            }
        } else if (last && !line.empty() && line[0] == '$') {
            if (second) {
                second = false;
                continue;
            }
            const std::string type = line.size() >= 5 ? line.substr(1, 4) : "";
            if (type.rfind("F0", 0) == 0)
                last->isSwitch = true;
            second = !type.empty() && (type[0] == '4' || type[0] == 'D' || type[0] == 'd');
            last->codes++;
            last->codeLines.push_back(line);
        }
    }
    for (Cheat& c : g_files.cheats)
        for (CheatValue& v : c.values)
            if (v.index < (int)c.codeLines.size()) {
                std::vector<std::string> t;
                std::istringstream is(c.codeLines[v.index]);
                std::string w;
                while (is >> w)
                    t.push_back(w);
                if (t.size() >= 3)
                    v.def = (long long)std::strtoull(t[2].c_str(), nullptr, 16);
            }
    for (const std::string& l : readLines(profileFile("enabled.txt").c_str()))
        if (!l.empty())
            g_files.enabled.insert(l);
    for (const std::string& l : readLines(profileFile("values.txt").c_str())) {
        std::vector<std::string> f = split(l, '\t');
        if (f.size() == 3)
            g_files.values[f[0] + "\t" + f[1]] = f[2];
    }
}

void saveEnabled()
{
    std::vector<std::string> lines;
    for (const Cheat& c : g_files.cheats)
        if (g_files.enabled.count(c.name))
            lines.push_back(c.name);
    writeLines(profileFile("enabled.txt").c_str(), lines);
}

void saveValues()
{
    std::vector<std::string> lines;
    for (const auto& kv : g_files.values)
        lines.push_back(kv.first + "\t" + kv.second);
    writeLines(profileFile("values.txt").c_str(), lines);
}

bool cheatOnce(const Cheat& c)
{
    auto it = g_files.values.find(c.name + "\tonce");
    return it != g_files.values.end() ? it->second == "1" : c.onceDefault;
}

struct PlayerEdit { uint32_t id; std::string field; long long value; bool lock; };
struct ListEdit { std::string list; int slot; uint32_t id; bool lock; };

std::vector<PlayerEdit> loadPlayerEdits()
{
    std::vector<PlayerEdit> out;
    for (const std::string& l : readLines(profileFile("players.txt").c_str())) {
        std::vector<std::string> f = split(l, '\t');
        if (f.size() >= 3)
            out.push_back({(uint32_t)std::strtoul(f[0].c_str(), nullptr, 10), f[1], std::atoll(f[2].c_str()), f.size() < 4 || f[3] != "0"});
    }
    // xs (특별연습·비장의 아이 together, before 2026-11-13) becomes xp (스페셜) and xh (비장의 아이) of the same value; the
    // next save writes them (saka_hooks.c reads both forms).
    std::vector<PlayerEdit> older;
    for (auto it = out.begin(); it != out.end();)
        if (it->field == "xs") {
            older.push_back(*it);
            it = out.erase(it);
        } else
            ++it;
    for (const PlayerEdit& e : older)
        for (const char* f : {"xp", "xh"})
            if (std::none_of(out.begin(), out.end(), [&](const PlayerEdit& x) { return x.id == e.id && x.field == f; }))
                out.push_back({e.id, f, e.value, e.lock});
    return out;
}

void savePlayerEdits(const std::vector<PlayerEdit>& edits)
{
    std::vector<std::string> lines;
    for (const PlayerEdit& e : edits)
        lines.push_back(std::to_string(e.id) + "\t" + e.field + "\t" + std::to_string(e.value) + "\t" + (e.lock ? "1" : "0"));
    writeLines(profileFile("players.txt").c_str(), lines);
}

std::vector<ListEdit> loadListEdits()
{
    std::vector<ListEdit> out;
    for (const std::string& l : readLines(profileFile("lists.txt").c_str())) {
        std::vector<std::string> f = split(l, '\t');
        if (f.size() >= 3)
            out.push_back({f[0], std::atoi(f[1].c_str()), (uint32_t)std::strtoul(f[2].c_str(), nullptr, 10), f.size() < 4 || f[3] != "0"});
    }
    return out;
}

void saveListEdits(const std::vector<ListEdit>& edits)
{
    std::vector<std::string> lines;
    for (const ListEdit& e : edits)
        lines.push_back(e.list + "\t" + std::to_string(e.slot) + "\t" + std::to_string(e.id) + "\t" + (e.lock ? "1" : "0"));
    writeLines(profileFile("lists.txt").c_str(), lines);
}

// ---- the panels -------------------------------------------------------------------------------------------
std::string g_toast;
std::chrono::steady_clock::time_point g_toastAt;
int g_toastLevel = 0;   // 0 done, 1 warning (orange)
std::deque<std::pair<std::chrono::steady_clock::time_point, std::string>> g_toastLog;   // the last ones (status line)
void toast(const std::string& s, int level = 0)
{
    g_toast = s;
    g_toastLevel = level;
    g_toastAt = std::chrono::steady_clock::now();
    g_toastLog.emplace_back(g_toastAt, s);
    if (g_toastLog.size() > 12)
        g_toastLog.pop_front();
}

// ---- a free layout (2026-11-10, owner: "이 F2 내부 인터페이스가 자유롭게 안팎으로 조정가능해야해. 제대로 적용이 안되거나
// 하는걸 보지를 못해") ----
// The window moves and resizes; the roster and the list slots widen by dragging their right border; panes scroll
// sideways when narrower than their rows, and the skill boxes wrap; Ctrl+wheel or 글자 -/+ zooms. cheats\overlay.ini
// keeps all of it. Sizes below are for the 19 px font of 1280x720 and follow the font (resolution and zoom).
float g_uiScale = 1.0f;
float g_footerH = 0.0f;   // the status line under the tabs
bool g_resetRoster = false, g_resetSlots = false;
inline float Px(float px) { return px * ImGui::GetFontSize() / 19.0f; }
float btnW(const char* label) { return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f; }
void zoomBy(float d)
{
    g_uiScale = std::max(0.6f, std::min(2.0f, std::round((g_uiScale + d) * 10.0f) / 10.0f));
    if (ImGui::GetCurrentContext())
        ImGui::MarkIniSettingsDirty();
}
// The right end of the shown part of the current window (screen x): rows wrap against it even while the pane can
// scroll sideways (its work area is then as wide as its widest row).
float visibleRight()
{
    const ImGuiStyle& st = ImGui::GetStyle();
    return ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - st.WindowPadding.x -
           (ImGui::GetScrollMaxY() > 0.0f ? st.ScrollbarSize : 0.0f);
}
// SameLine() when an item this wide still fits on the line; otherwise it starts the next one.
void fitSameLine(float width)
{
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= visibleRight())
        ImGui::SameLine();
}
// A grey note wrapped at the shown width.
void note(const std::string& text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(std::max(ImGui::GetCursorPosX() + Px(200), visibleRight() - ImGui::GetWindowPos().x + ImGui::GetScrollX()));
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}
// Whether an edit is in the game (2026-11-10): shortly after 적용 the row says so in green; red when a 고정 edit does
// not hold for 1.5 s, or a once edit is not in the game 3 s after 적용 (the cheat thread reads players.txt again
// within a second); a once edit the game has changed since is noted in grey a minute later.
// applyCheck returns 0 nothing to show, 1 green, 2 red, 3 grey.
std::map<std::string, std::chrono::steady_clock::time_point> g_appliedAt, g_differSince;
std::set<std::string> g_warned;   // red rows already told in the status line
void appliedNow(const std::string& key)
{
    g_appliedAt[key] = std::chrono::steady_clock::now();
    g_differSince.erase(key);
    g_warned.erase(key);
}
int applyCheck(const std::string& key, bool same, bool lock)
{
    using namespace std::chrono;
    const auto now = steady_clock::now();
    const auto ap = g_appliedAt.find(key);
    const double sinceApply = ap == g_appliedAt.end() ? 1e9 : duration<double>(now - ap->second).count();
    if (same) {
        g_differSince.erase(key);
        return sinceApply < 5.0 ? 1 : 0;
    }
    const double differ = duration<double>(now - g_differSince.emplace(key, now).first->second).count();
    if (lock ? differ > 1.5 : sinceApply > 3.0 && sinceApply < 60.0 && differ > 2.5)
        return 2;
    return !lock && sinceApply >= 60.0 ? 3 : 0;
}
void applyStateText(int state, const std::string& red, const std::string& grey)
{
    if (state == 0)
        return;
    ImGui::SameLine();
    if (state == 1)
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "게임에 들어감");
    else if (state == 2)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", red.c_str());
    else
        ImGui::TextDisabled("%s", grey.c_str());
}
// A red row is told once in the status line too.
void warnOnce(const std::string& key, int state, const std::string& text)
{
    if (state == 2 && g_warned.insert(key).second)
        toast(text, 1);
    else if (state != 2)
        g_warned.erase(key);
}

char g_cheatFilter[128];
std::map<std::string, long long> g_inputs;   // value boxes: "cheat\tn" -> typed value

void cheatsPanel()
{
    ImGui::SetNextItemWidth(Px(320));
    ImGui::InputTextWithHint("##cheatfilter", "찾기 (이름·설명)", g_cheatFilter, sizeof g_cheatFilter);
    ImGui::SameLine();
    ImGui::TextDisabled("켜면 1초 안에 적용. 고정 = 1초에 10번 다시 씀, 끄면 켤 때 한 번만.");
    ImGui::Separator();
    ImGui::BeginChild("cheats", ImVec2(0, -g_footerH), 0, ImGuiWindowFlags_HorizontalScrollbar);
    std::string shown;
    const std::string filter = g_cheatFilter;
    for (Cheat& c : g_files.cheats) {
        if (!filter.empty() && (c.title + c.origin + c.note + c.category).find(filter) == std::string::npos)
            continue;
        if (c.category != shown) {
            shown = c.category;
            int on = 0, all = 0;
            for (const Cheat& o : g_files.cheats)
                if (o.category == shown) {
                    all++;
                    on += g_files.enabled.count(o.name) ? 1 : 0;
                }
            ImGui::Spacing();
            ImGui::SeparatorText((shown + "  ·  " + std::to_string(on) + " / " + std::to_string(all) + " 켜짐").c_str());
        }
        ImGui::PushID(c.name.c_str());
        bool on = g_files.enabled.count(c.name) != 0;
        if (ImGui::Checkbox(c.title.c_str(), &on)) {
            if (on)
                g_files.enabled.insert(c.name);
            else
                g_files.enabled.erase(c.name);
            saveEnabled();
            toast((on ? "켰습니다: " : "껐습니다: ") + c.title);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", c.isSwitch ? "[코드 스위치] " : "", c.origin == "PS3" ? "PS3 포트" : ("Vita: " + c.origin).c_str());
        if (!c.note.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", c.note.c_str());
        // 고정 on the title line: after the value rows, SameLine + Indent drew it over the start of the last row (2026-11-08).
        // Also for a code switch that takes a value; 갱신 writes the value once more while 고정 is off (values.txt
        // "refresh", 2026-11-09, owner: "고정도 가능하게 해주고 갱신도 가능하게 해서 변경도 가능해야해").
        if (!c.isSwitch || c.lockable) {
            ImGui::SameLine();
            bool lock = !cheatOnce(c);
            if (ImGui::Checkbox("고정", &lock)) {
                const std::string key = c.name + "\tonce";
                if (lock == !c.onceDefault)
                    g_files.values.erase(key);
                else
                    g_files.values[key] = lock ? "0" : "1";
                saveValues();
                toast(c.title + (lock ? ": 고정" : ": 한 번만"));
            }
            if (!lock) {
                ImGui::SameLine();
                if (ImGui::SmallButton("갱신")) {
                    const std::string key = c.name + "\trefresh";
                    auto it = g_files.values.find(key);
                    const int n = it != g_files.values.end() ? atoi(it->second.c_str()) : 0;
                    g_files.values[key] = std::to_string(n + 1);
                    saveValues();
                    toast(c.title + ": 갱신 (지금 값을 한 번 더 씀)");
                }
            }
        }
        for (const CheatValue& v : c.values) {
            ImGui::PushID(v.index);
            const std::string key = c.name + "\t" + std::to_string(v.index);
            auto stored = g_files.values.find(key);
            long long current = stored != g_files.values.end() ? std::atoll(stored->second.c_str()) : v.def;
            if (!g_inputs.count(key))
                g_inputs[key] = current;
            long long& typed = g_inputs[key];
            ImGui::Indent(32);
            if (v.kind == "staff" || v.kind == "scout") {
                loadTexts();
                const Names& names = v.kind == "staff" ? g_text.staff : g_text.scouts;
                ImGui::SetNextItemWidth(Px(320));
                if (ImGui::BeginCombo(v.label.c_str(), (std::to_string(typed) + "  " + nameOf(names, (int)typed)).c_str())) {
                    for (const auto& kv : names)
                        if (kv.first >= v.min && kv.first <= v.max)
                            if (ImGui::Selectable((std::to_string(kv.first) + "  " + kv.second).c_str(), kv.first == typed))
                                typed = kv.first;
                    ImGui::EndCombo();
                }
            } else {
                int value32 = (int)std::min<long long>(std::max<long long>(typed, INT32_MIN), INT32_MAX);
                ImGui::SetNextItemWidth(Px(200));
                if (ImGui::InputInt(v.label.c_str(), &value32, 1, 100))
                    typed = value32;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("적용")) {
                typed = std::max(v.min, std::min(v.max, typed));
                if (typed == v.def)
                    g_files.values.erase(key);
                else
                    g_files.values[key] = std::to_string(typed);
                saveValues();
                toast(c.title + " · " + v.label + " = " + std::to_string(typed));
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%lld~%lld)", v.min, v.max);
            ImGui::Unindent(32);
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

struct Field { const char* key; const char* label; int offset, size; long long min, max; };
const Field kBasic[] = {
    {"b5", "나이", 0x05, 1, 15, 50}, {"b7", "계약 연수", 0x07, 1, 1, 9}, {"b4", "각성 레벨", 0x04, 1, 0, 15},
    {"hc6", "각성 포인트", 0xC6, 2, 0, 65535}, {"h108", "스타성", 0x108, 2, 0, 65534},
    {"w208", "성장 타입", 0x208, 4, 0, 7}, {"hc8", "컨디션", 0xC8, 2, 0, 1000}, {"hca", "피로", 0xCA, 2, 0, 1000},
};
int g_slot = -1;
std::map<std::string, int> g_fieldInput;
bool g_styleRefresh = false;   // the play style boxes read the game again (the list's 새로고침)
std::map<std::string, bool> g_fieldLock;

uint32_t fieldValue(uint32_t rec, int offset, int size)
{
    return size == 1 ? rd8(rec + offset) : size == 2 ? rd16(rec + offset) : rd32(rec + offset);
}

void playersPanel()
{
    loadTexts();
    const uint32_t roster = 0x00EE2954u, record = 0x218u;
    if (!committed(roster, 23 * record)) {
        ImGui::TextDisabled("게임 데이터가 아직 없습니다 (세이브를 불러온 뒤).");
        return;
    }
    std::vector<PlayerEdit> edits = loadPlayerEdits();
    // The club's 23 slots (an empty one shown as such: the numbers are the game's slots, 2026-11-09, owner: "팀의 선수
    // 리스트를 모두 불러오지를 못하는데"), then the youth team's 15 (0x00EE86E0, the same record layout); read live.
    const uint32_t youth = 0x00EE86E0u;
    auto slotRec = [&](int s) { return s < 23 ? roster + record * (uint32_t)s : youth + record * (uint32_t)(s - 23); };
    const bool haveYouth = committed(youth, 15 * record);
    if (g_resetRoster) {   // 기본 크기: the first width again
        g_resetRoster = false;
        ImGui::SetNextWindowSize(ImVec2(Px(330), 0.0f), ImGuiCond_Always);
    }
    ImGui::BeginChild("roster", ImVec2(Px(330), -g_footerH), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX,
                      ImGuiWindowFlags_HorizontalScrollbar);
    // Positions (+0x204 u32, 2026-11-09, owner: "몇명인지 어떤 포지션들로 몇명있는지도 표시"): the names of the
    // position aptitudes (pledit_ability_name 19-29), grouped GK / DF / MF / FW.
    static const char* const kPos[11] = {"GK", "LSB", "RSB", "CB", "DM", "LSM", "RSM", "OM", "LWG", "RWG", "FW"};
    auto posOf = [&](int s) { const uint32_t p = rd32(slotRec(s) + 0x204); return p < 11 ? (int)p : -1; };
    auto summary = [&](int from, int to, const char* title, int slots) {
        int n = 0, group[4] = {0, 0, 0, 0}, each[11] = {0};
        for (int s = from; s < to; s++) {
            if (!validPlayer(rd32(slotRec(s))))
                continue;
            n++;
            const int p = posOf(s);
            if (p < 0)
                continue;
            each[p]++;
            group[p == 0 ? 0 : p <= 3 ? 1 : p <= 7 ? 2 : 3]++;
        }
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s %d명 (%d칸)  GK %d · DF %d · MF %d · FW %d", title, n, slots,
                           group[0], group[1], group[2], group[3]);
        std::string detail;
        for (int p = 0; p < 11; p++)
            if (each[p])
                detail += std::string(detail.empty() ? "" : " · ") + kPos[p] + " " + std::to_string(each[p]);
        if (!detail.empty())   // wrapped: the list is narrow
            note(detail);
    };
    // 새로고침 (2026-11-09, owner: "이 구단 선수 목록도 리프레시가 되어야해"): the list reads the game every frame;
    // this also makes every value box and the play style boxes start again from the game's values.
    // Sorting (2026-11-13, owner: "선수나 코치 스카우트들을 국적 능력치(등급) 번호 나이 이름순으로 sorting"): within the
    // club's 23 and within the youth team's 15; 번호 = the game's slot order; empty slots last. Grade = the player
    // master's +0x13 (0-14: Messi 14, Iniesta 13, Kagawa 11; record +4 is the awakening level), age = record +5.
    static int rosterSort = 0;
    static bool rosterRev = false;
    ImGui::SetNextItemWidth(Px(100));
    ImGui::Combo("정렬##roster", &rosterSort, kSortNames, 5);
    ImGui::SameLine();
    ImGui::Checkbox("역순##roster", &rosterRev);
    std::vector<int> order;
    for (int from = 0; from < 38; from += 23) {
        const int to = from == 0 ? 23 : 38;
        std::vector<int> v;
        for (int s = from; s < to; s++)
            v.push_back(s);
        if (rosterSort == 0) {
            if (rosterRev)
                std::reverse(v.begin(), v.end());
        } else
            std::stable_sort(v.begin(), v.end(), [&](int a, int b) {
                const uint32_t ia = rd32(slotRec(a)), ib = rd32(slotRec(b));
                const bool va = validPlayer(ia), vb = validPlayer(ib);
                if (va != vb)
                    return va;   // empty slots last
                if (!va)
                    return a < b;
                int c = 0;
                if (rosterSort == 1) {
                    const std::string na = nameOf(g_text.players, (int)ia), nb = nameOf(g_text.players, (int)ib);
                    const std::string ba = na.substr(0, std::min(tagPos(na), na.size())), bb = nb.substr(0, std::min(tagPos(nb), nb.size()));
                    c = ba < bb ? -1 : ba > bb ? 1 : 0;
                } else if (rosterSort == 2)
                    c = playerNation((int)ia) - playerNation((int)ib);
                else if (rosterSort == 3)   // grade (the master's, as the lists): the highest first
                    c = personGrade(2, (int)ib) - personGrade(2, (int)ia);
                else   // age (record +5): the oldest first
                    c = (int)rd8(slotRec(b) + 5) - (int)rd8(slotRec(a) + 5);
                if (c == 0)
                    c = a - b;
                return rosterRev ? c > 0 : c < 0;
            });
        order.insert(order.end(), v.begin(), v.end());
    }
    summary(0, 23, "구단", 23);
    for (size_t k = 0; k < order.size(); k++) {
        const int s = order[k];
        if (k == 23) {
            if (!haveYouth)
                break;
            ImGui::Separator();
            summary(23, 38, "유스", 15);
        }
        const uint32_t id = rd32(slotRec(s));
        if (!validPlayer(id)) {
            if (s < 23)
                ImGui::TextDisabled("%02d  (빈 칸)", s + 1);
            continue;
        }
        if (g_slot < 0)
            g_slot = s;
        int n = 0;
        std::string mult;   // his own multipliers (xt / xs / xa)
        for (const PlayerEdit& e : edits) {
            if (e.id != id)
                continue;
            if (e.field == "xt" || e.field == "xp" || e.field == "xh" || e.field == "xa")
                mult += std::string(mult.empty() ? "  ·  배수" : "") +
                        (e.field == "xt" ? " 기본x" : e.field == "xp" ? " 스페셜x" : e.field == "xh" ? " 비장x" : " 각성x") +
                        std::to_string(e.value);
            else
                n++;
        }
        char line[320];
        const int pos = posOf(s);
        std::snprintf(line, sizeof line, "%s%02d  %-3s  %s  ·  %u세  ·  %s  ·  등급 %d%s%s##slot%d", s < 23 ? "" : "유스 ",
                      s < 23 ? s + 1 : s - 22, pos >= 0 ? kPos[pos] : "?", nameOf(g_text.players, (int)id).c_str(),
                      rd8(slotRec(s) + 5), nameOf(g_text.nations, playerNation((int)id)).c_str(), personGrade(2, (int)id),
                      n ? ("  ·  수정 " + std::to_string(n)).c_str() : "", mult.c_str(), s);
        if (ImGui::Selectable(line, g_slot == s))
            g_slot = s;
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("player", ImVec2(0, -g_footerH), 0, ImGuiWindowFlags_HorizontalScrollbar);
    if (g_slot >= 0) {
        const uint32_t rec = slotRec(g_slot), id = rd32(rec);
        if (validPlayer(id)) {
            if (g_slot < 23)
                ImGui::Text("%s  (선수 ID %u, %d번 칸)", nameOf(g_text.players, (int)id).c_str(), id, g_slot + 1);
            else
                ImGui::Text("%s  (선수 ID %u, 유스 %d번 칸)", nameOf(g_text.players, (int)id).c_str(), id, g_slot - 22);
            note("값을 넣고 적용. 고정이면 1초에 10번 다시 씀, 아니면 한 번. 이 선수가 지금 구단에 있을 때만. "
                 "적용 뒤 그 줄 끝에 게임에 들어갔는지 나오고, 안 들어가면 빨간 글씨로 나옵니다.");
            // This player's edits at once (2026-11-09, owner: "모든 치트에 새로고침과 전체적용 고정등의 기능들이 세심하게"):
            // 새로고침 writes the file again so the game puts the once edits in again; 전부 고정 / 전부 한 번만 switch them;
            // 전체 선수에 적용 copies this player's edits to every player of the club; 전부 해제 removes them.
            {
                int mine = 0;
                for (const PlayerEdit& e : edits)
                    mine += e.id == id;
                ImGui::TextDisabled("이 선수의 편집 %d개:", mine);
                fitSameLine(btnW("새로고침"));
                if (ImGui::SmallButton("새로고침")) {
                    // The value boxes show the game's current values again (they kept the value of when they were
                    // first drawn; owner screenshot: box 12 while the game had 29), and the once edits are written again.
                    const std::string prefix = std::to_string(id);
                    for (auto it = g_fieldInput.begin(); it != g_fieldInput.end();)
                        it = it->first.compare(0, prefix.size(), prefix) == 0 && it->first.size() > prefix.size() &&
                                     !std::isdigit((unsigned char)it->first[prefix.size()])
                                 ? g_fieldInput.erase(it) : std::next(it);
                    for (auto it = g_fieldLock.begin(); it != g_fieldLock.end();)
                        it = it->first.compare(0, prefix.size(), prefix) == 0 && it->first.size() > prefix.size() &&
                                     !std::isdigit((unsigned char)it->first[prefix.size()])
                                 ? g_fieldLock.erase(it) : std::next(it);
                    savePlayerEdits(loadPlayerEdits());
                    toast("새로고침: 지금 게임 값을 다시 읽고, 한 번만 쓰는 편집을 다시 씀");
                }
                for (int lockAll = 1; lockAll >= 0; lockAll--) {
                    fitSameLine(btnW(lockAll ? "전부 고정" : "전부 한 번만"));
                    if (ImGui::SmallButton(lockAll ? "전부 고정" : "전부 한 번만")) {
                        std::vector<PlayerEdit> all = loadPlayerEdits();
                        for (PlayerEdit& e : all)
                            if (e.id == id) {
                                e.lock = lockAll != 0;
                                g_fieldLock[std::to_string(id) + e.field] = e.lock;
                            }
                        savePlayerEdits(all);
                        toast(nameOf(g_text.players, (int)id) + (lockAll ? ": 편집 전부 고정" : ": 편집 전부 한 번만"));
                    }
                }
                fitSameLine(btnW("전체 선수에 적용"));
                if (ImGui::SmallButton("전체 선수에 적용")) {
                    std::vector<PlayerEdit> all = loadPlayerEdits(), source;
                    for (const PlayerEdit& e : all)
                        if (e.id == id)
                            source.push_back(e);
                    int players = 0;
                    for (int s = 0; s < 23; s++) {
                        const uint32_t other = rd32(roster + record * s);
                        if (!validPlayer(other) || other == id)
                            continue;
                        players++;
                        for (const PlayerEdit& e : source) {
                            all.erase(std::remove_if(all.begin(), all.end(), [&](const PlayerEdit& x) { return x.id == other && x.field == e.field; }), all.end());
                            all.push_back({other, e.field, e.value, e.lock});
                        }
                    }
                    savePlayerEdits(all);
                    toast("편집 " + std::to_string(source.size()) + "개를 선수 " + std::to_string(players) + "명에게 적용");
                }
                fitSameLine(btnW("전부 해제"));
                if (ImGui::SmallButton("전부 해제")) {
                    std::vector<PlayerEdit> all = loadPlayerEdits();
                    all.erase(std::remove_if(all.begin(), all.end(), [&](const PlayerEdit& x) { return x.id == id; }), all.end());
                    savePlayerEdits(all);
                    toast(nameOf(g_text.players, (int)id) + ": 편집 전부 해제");
                }
            }
            auto findEdit = [&](const std::string& field) -> const PlayerEdit* {
                for (const PlayerEdit& e : edits)
                    if (e.id == id && e.field == field)
                        return &e;
                return nullptr;
            };
            auto row = [&](const std::string& field, const std::string& label, const std::string& now, long long lo, long long hi,
                           long long current) {
                ImGui::PushID(field.c_str());
                const PlayerEdit* e = findEdit(field);
                const std::string key = std::to_string(id) + field;
                if (!g_fieldInput.count(key))   // the edit's value, else the current one
                    g_fieldInput[key] = e ? (int)e->value : (int)current;
                if (!g_fieldLock.count(key))
                    g_fieldLock[key] = e ? e->lock : false;   // off until asked for (owner, 2026-11-08)
                ImGui::TextUnformatted(label.c_str());
                ImGui::SameLine(Px(200));
                ImGui::TextDisabled("%s", now.c_str());
                ImGui::SameLine(Px(360));
                ImGui::SetNextItemWidth(Px(120));
                ImGui::InputInt("##v", &g_fieldInput[key], 1, 10);
                ImGui::SameLine();
                bool lk = g_fieldLock[key];
                if (ImGui::Checkbox("고정", &lk))
                    g_fieldLock[key] = lk;
                ImGui::SameLine();
                if (ImGui::SmallButton("적용")) {
                    const long long v = std::max(lo, std::min(hi, (long long)g_fieldInput[key]));
                    std::vector<PlayerEdit> all = loadPlayerEdits();
                    all.erase(std::remove_if(all.begin(), all.end(), [&](const PlayerEdit& x) { return x.id == id && x.field == field; }), all.end());
                    all.push_back({id, field, v, g_fieldLock[key]});
                    savePlayerEdits(all);
                    appliedNow(key);
                    toast(nameOf(g_text.players, (int)id) + " · " + label + " = " + std::to_string(v));
                }
                if (e) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("해제")) {
                        std::vector<PlayerEdit> all = loadPlayerEdits();
                        all.erase(std::remove_if(all.begin(), all.end(), [&](const PlayerEdit& x) { return x.id == id && x.field == field; }), all.end());
                        savePlayerEdits(all);
                        toast(label + ": 해제");
                    }
                    // Is the edit in the game? (2026-11-10)
                    const int state = applyCheck(key, current == e->value, e->lock);
                    applyStateText(state, "안 들어감: 게임 값 " + std::to_string(current) + (e->lock ? " (고정인데 안 바뀜)" : ""),
                                   "게임 값 " + std::to_string(current) + " (한 번만 쓴 값 " + std::to_string(e->value) + ")");
                    warnOnce(key, state, nameOf(g_text.players, (int)id) + " · " + label + ": 게임에 안 들어감 (게임 값 " +
                                             std::to_string(current) + ", 넣은 값 " + std::to_string(e->value) + ")");
                }
                ImGui::PopID();
            };
            // Play styles and skills (2026-11-09, owner: "플레이스타일 스킬등도 편집이 가능해야해"; as the launcher's StyleCard):
            // +0x10A u8 the style slot in use (0-4); 5 slots of 0x30 from +0x10C: +0 u32 style (1-66), +4 u32 original
            // skill (113-181, 0 none), 5 skills of 8 bytes from +8: u32 type (1-16), u8 level at +4 (1-7; 0xFF when the
            // type is 0). Written as players.txt fields b/w<offset> (saka_hooks.c), all together, 고정 or once.
            {
                static const bool openAll = [] { const char* v = std::getenv("SAKA_OVERLAY_OPEN_ALL"); return v && *v == '1'; }();
                if (openAll)   // tests: the play style editor open
                    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            }
            if (ImGui::CollapsingHeader("플레이스타일·스킬")) {
                static uint32_t styleFor = 0;
                if (g_styleRefresh) {
                    g_styleRefresh = false;
                    styleFor = 0;
                }
                static int inUse, style[5], original[5], type[5][5], level[5][5];
                static bool styleLock = false;
                auto hex = [](const char* p, int off) { char b[16]; std::snprintf(b, sizeof b, "%s%x", p, off); return std::string(b); };
                auto now = [&](const std::string& field, uint32_t live) -> int {
                    for (const PlayerEdit& e : edits)
                        if (e.id == id && e.field == field)
                            return (int)e.value;
                    return (int)live;
                };
                if (styleFor != id || ImGui::SmallButton("게임 값 다시 읽기")) {   // the boxes start from the game (or the edits)
                    styleFor = id;
                    inUse = now("b10a", rd8(rec + 0x10A));
                    for (int e = 0; e < 5; e++) {
                        const uint32_t b = rec + 0x10C + 0x30 * e;
                        const int o = 0x10C + 0x30 * e;
                        style[e] = now(hex("w", o), rd32(b));
                        original[e] = now(hex("w", o + 4), rd32(b + 4));
                        for (int k = 0; k < 5; k++) {
                            type[e][k] = now(hex("w", o + 8 + 8 * k), rd32(b + 8 + 8 * k));
                            level[e][k] = now(hex("b", o + 12 + 8 * k), rd8(b + 12 + 8 * k));
                        }
                    }
                }
                auto styleLabel = [&](int v) -> std::string {
                    auto it = g_text.styleInfo.find(v);
                    if (v == 0 || it == g_text.styleInfo.end())
                        return std::to_string(v) + " " + (v == 0 ? std::string("없음") : nameOf(g_text.styles, v));
                    const char* stars = it->second.tier >= 3 ? "★★★" : it->second.tier == 2 ? "★★ " : "★  ";
                    return std::string(stars) + " " + std::to_string(v) + " " + it->second.ko + " (" + it->second.ja + ")";
                };
                auto styleCombo = [&](const char* label, int* v, float width) {
                    ImGui::SetNextItemWidth(width);
                    if (ImGui::BeginCombo(label, styleLabel(*v).c_str(), ImGuiComboFlags_HeightLarge)) {
                        if (ImGui::Selectable("0 없음", *v == 0))
                            *v = 0;
                        for (int fam = 1; fam <= 15; fam++) {   // each family: its base, then upper (★★), then top (★★★)
                            auto base = g_text.styleInfo.find(fam);
                            ImGui::SeparatorText((base != g_text.styleInfo.end() ? base->second.ko : std::to_string(fam)).append(" 계열").c_str());
                            for (int tier = 1; tier <= 3; tier++)
                                for (const auto& kv : g_text.styleInfo)
                                    if (kv.second.family == fam && kv.second.tier == tier &&
                                        ImGui::Selectable(styleLabel(kv.first).c_str(), *v == kv.first))
                                        *v = kv.first;
                        }
                        ImGui::EndCombo();
                    }
                };
                auto typeLabel = [&](int v) -> std::string {
                    auto it = g_text.typeKo.find(v);
                    if (v == 0 || it == g_text.typeKo.end())
                        return std::to_string(v) + " " + (v == 0 ? std::string("없음 (빈칸)") : nameOf(g_text.skillTypes, v));
                    return std::to_string(v) + " " + it->second.first + " (Lv7: " + it->second.second + ")";
                };
                auto origLabel = [&](int v) -> std::string {
                    auto it = g_text.origKo.find(v);
                    if (v == 0 || it == g_text.origKo.end())
                        return std::to_string(v) + " " + (v == 0 ? std::string("없음") : nameOf(g_text.skills, v));
                    return std::to_string(v) + " " + it->second + " (" + nameOf(g_text.skills, v) + ")";
                };
                auto listCombo = [&](const char* label, int* v, int lo, int hi, float width, const std::function<std::string(int)>& name) {
                    ImGui::SetNextItemWidth(width);
                    if (ImGui::BeginCombo(label, name(*v).c_str(), ImGuiComboFlags_HeightLarge)) {
                        if (ImGui::Selectable(name(0).c_str(), *v == 0))
                            *v = 0;
                        for (int i = lo; i <= hi; i++)
                            if (ImGui::Selectable(name(i).c_str(), *v == i))
                                *v = i;
                        ImGui::EndCombo();
                    }
                };
                auto combo = [&](const char* label, int* v, const Names& names, int lo, int hi, float width, bool zero) {
                    ImGui::SetNextItemWidth(width);
                    const std::string cur = std::to_string(*v) + " " + (*v == 0 ? nameOf(names, 0) : nameOf(names, *v));
                    if (ImGui::BeginCombo(label, cur.c_str())) {
                        if (zero && ImGui::Selectable((std::string("0 ") + nameOf(names, 0)).c_str(), *v == 0))
                            *v = 0;
                        for (int i = lo; i <= hi; i++)
                            if (ImGui::Selectable((std::to_string(i) + " " + nameOf(names, i)).c_str(), *v == i))
                                *v = i;
                        ImGui::EndCombo();
                    }
                };
                ImGui::SetNextItemWidth(Px(120));
                ImGui::SliderInt("쓰는 스타일 칸 (0~4)", &inUse, 0, 4);
                for (int e = 0; e < 5; e++) {
                    ImGui::PushID(e);
                    // A slot: label, style, original skill (next line when narrow), then the five skills as many to a
                    // line as fit (2026-11-10: they ran past the pane's right edge, owner screenshot).
                    ImGui::Text("%d번 칸%s", e + 1, e == inUse ? " (사용 중)" : "");
                    ImGui::SameLine(std::max(Px(110), ImGui::CalcTextSize("5번 칸 (사용 중)").x + ImGui::GetStyle().ItemSpacing.x));
                    styleCombo("##style", &style[e], Px(330));
                    fitSameLine(Px(300) + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("오리지널").x);
                    listCombo("오리지널##orig", &original[e], 113, 181, Px(300), origLabel);
                    for (int k = 0; k < 5; k++) {
                        ImGui::PushID(k);
                        if (k)
                            fitSameLine(Px(250) + ImGui::GetStyle().ItemSpacing.x + Px(70));
                        ImGui::BeginGroup();
                        listCombo("##type", &type[e][k], 1, 16, Px(250), typeLabel);
                        ImGui::SameLine();
                        int lv = level[e][k] >= 1 && level[e][k] <= 7 ? level[e][k] : 1;
                        ImGui::SetNextItemWidth(Px(70));
                        if (ImGui::SliderInt("##lv", &lv, 1, 7, "Lv%d"))
                            level[e][k] = lv;
                        else if (level[e][k] < 1 || level[e][k] > 7)
                            level[e][k] = lv;
                        ImGui::EndGroup();
                        ImGui::PopID();
                    }
                    ImGui::PopID();
                }
                ImGui::Checkbox("고정##style", &styleLock);
                ImGui::SameLine();
                auto isStyle = [](const std::string& f) {
                    if (f.size() < 2 || (f[0] != 'w' && f[0] != 'b'))
                        return false;
                    const unsigned long off = std::strtoul(f.c_str() + 1, nullptr, 16);
                    return off >= 0x10A && off < 0x10C + 5 * 0x30;
                };
                if (ImGui::Button("스타일·스킬 적용")) {
                    std::vector<PlayerEdit> all = loadPlayerEdits();
                    all.erase(std::remove_if(all.begin(), all.end(), [&](const PlayerEdit& x) { return x.id == id && isStyle(x.field); }), all.end());
                    all.push_back({id, "b10a", inUse, styleLock});
                    for (int e = 0; e < 5; e++) {
                        const int o = 0x10C + 0x30 * e;
                        all.push_back({id, hex("w", o), style[e], styleLock});
                        all.push_back({id, hex("w", o + 4), original[e], styleLock});
                        for (int k = 0; k < 5; k++) {
                            all.push_back({id, hex("w", o + 8 + 8 * k), type[e][k], styleLock});
                            all.push_back({id, hex("b", o + 12 + 8 * k), type[e][k] == 0 ? 0xFF : level[e][k], styleLock});
                        }
                    }
                    savePlayerEdits(all);
                    appliedNow(std::to_string(id) + "style");
                    toast(nameOf(g_text.players, (int)id) + ": 플레이스타일·스킬 적용" + (styleLock ? " (고정)" : " (한 번)"));
                }
                ImGui::SameLine();
                if (ImGui::Button("스타일·스킬 수정 해제")) {
                    std::vector<PlayerEdit> all = loadPlayerEdits();
                    all.erase(std::remove_if(all.begin(), all.end(), [&](const PlayerEdit& x) { return x.id == id && isStyle(x.field); }), all.end());
                    savePlayerEdits(all);
                    toast(nameOf(g_text.players, (int)id) + ": 플레이스타일·스킬 수정 해제");
                }
                // The style edits against the game (2026-11-10): the first boxes that differ, by name.
                {
                    int mine = 0, differ = 0;
                    bool lock = false;
                    std::string first;
                    for (const PlayerEdit& x : edits) {
                        if (x.id != id || !isStyle(x.field))
                            continue;
                        mine++;
                        lock = lock || x.lock;
                        const uint32_t off = (uint32_t)std::strtoul(x.field.c_str() + 1, nullptr, 16);
                        const long long have = x.field[0] == 'b' ? (long long)rd8(rec + off) : (long long)rd32(rec + off);
                        if (have == x.value || ++differ > 3)
                            continue;
                        std::string where = "쓰는 칸";
                        if (off != 0x10A) {
                            const uint32_t r = (off - 0x10C) % 0x30;
                            where = std::to_string((off - 0x10C) / 0x30 + 1) + "번 칸 " +
                                    (r == 0 ? std::string("스타일") : r == 4 ? std::string("오리지널")
                                                                            : "스킬" + std::to_string((r - 8) / 8 + 1) + (r % 8 == 4 ? " Lv" : ""));
                        }
                        first += (first.empty() ? "" : ", ") + where + " 게임 " + std::to_string(have) + " / 넣은 값 " + std::to_string(x.value);
                    }
                    if (mine) {
                        const std::string key = std::to_string(id) + "style";
                        const int state = applyCheck(key, differ == 0, lock);
                        ImGui::TextDisabled("스타일·스킬 편집 %d칸%s", mine, lock ? " (고정)" : "");
                        applyStateText(state, "안 들어간 칸 " + std::to_string(differ) + "개: " + first,
                                       "게임 값과 다른 칸 " + std::to_string(differ) + "개 (한 번만 쓴 편집): " + first);
                        warnOnce(key, state, nameOf(g_text.players, (int)id) + ": 플레이스타일·스킬 " + std::to_string(differ) + "칸이 게임에 안 들어감");
                    }
                }
            }
            // Per-player multipliers (owner, 2026-11-09: "이것도 선수별로 개별로도 가능하게 해주고 ... 중복이나 이런 문제가
            // 발생할수 있는것에 대한 방지 대책도"): players.txt xt / xs / xa, read by saka_hooks.c (cheat_mult_for). His value
            // replaces the switch's (never both), one per kind, used only while he is in the club or its youth team.
            if (ImGui::CollapsingHeader("선수별 배수 (이 선수만)", ImGuiTreeNodeFlags_DefaultOpen)) {
                note("이 선수만의 훈련 배수입니다. 치트 탭의 '육성·연습' 배수 치트와 상관없이 (꺼져 있어도) 이 선수에게는 이 값을 씁니다. "
                     "둘을 곱하지 않음, 1~100.");
                note("해제하면 전체 설정을 따릅니다. 구단·유스를 떠난 선수에게는 쓰이지 않습니다. 배수는 연습·각성 때 쓰여서 여기서 바로 확인되지는 않습니다.");
                // One value per training kind (2026-11-13, owner: "선수별로도 각종 훈련들(기본, 스페셜, 비장의 아이 등)의 배율",
                // "선수배수는 훈련쪽이 아닌 선수들 마다 각각 내가 직접 설정 가능하도록").
                static const struct { const char* field; const char* label; } kMult[] = {
                    {"xt", "기본 연습 (全体練習) 배수"}, {"xp", "스페셜 (特別練習) 배수"}, {"xh", "비장의 아이 (秘蔵っ子) 배수"}, {"xa", "각성 포인트 배수"}};
                for (const auto& m : kMult) {
                    ImGui::PushID(m.field);
                    const PlayerEdit* e = findEdit(m.field);
                    const std::string key = std::to_string(id) + m.field;
                    if (!g_fieldInput.count(key))
                        g_fieldInput[key] = e ? (int)e->value : 2;
                    ImGui::TextUnformatted(m.label);
                    ImGui::SameLine(Px(200));
                    ImGui::TextDisabled("%s", e ? ("지금 x" + std::to_string(e->value)).c_str() : "전체 설정 따름");
                    ImGui::SameLine(Px(360));
                    ImGui::SetNextItemWidth(Px(120));
                    ImGui::InputInt("##v", &g_fieldInput[key], 1, 10);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("적용")) {
                        const long long v = std::max(1LL, std::min(100LL, (long long)g_fieldInput[key]));
                        g_fieldInput[key] = (int)v;
                        std::vector<PlayerEdit> all = loadPlayerEdits();
                        all.erase(std::remove_if(all.begin(), all.end(), [&](const PlayerEdit& x) { return x.id == id && x.field == m.field; }), all.end());
                        all.push_back({id, m.field, v, true});
                        savePlayerEdits(all);
                        toast(nameOf(g_text.players, (int)id) + " · " + m.label + " x" + std::to_string(v));
                    }
                    if (e) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("해제")) {
                            std::vector<PlayerEdit> all = loadPlayerEdits();
                            all.erase(std::remove_if(all.begin(), all.end(), [&](const PlayerEdit& x) { return x.id == id && x.field == m.field; }), all.end());
                            savePlayerEdits(all);
                            toast(std::string(m.label) + ": 해제 (전체 설정 따름)");
                        }
                    }
                    ImGui::PopID();
                }
                {   // the four at once
                    ImGui::PushID("xall");
                    const std::string key = std::to_string(id) + "xall";
                    if (!g_fieldInput.count(key))
                        g_fieldInput[key] = 2;
                    ImGui::TextUnformatted("전부 (위 4가지 한 번에)");
                    ImGui::SameLine(Px(360));
                    ImGui::SetNextItemWidth(Px(120));
                    ImGui::InputInt("##v", &g_fieldInput[key], 1, 10);
                    auto isMult = [&](const PlayerEdit& x) {
                        return x.id == id && (x.field == "xt" || x.field == "xp" || x.field == "xh" || x.field == "xa");
                    };
                    fitSameLine(btnW("전부 적용"));
                    if (ImGui::SmallButton("전부 적용")) {
                        const long long v = std::max(1LL, std::min(100LL, (long long)g_fieldInput[key]));
                        g_fieldInput[key] = (int)v;
                        std::vector<PlayerEdit> all = loadPlayerEdits();
                        all.erase(std::remove_if(all.begin(), all.end(), isMult), all.end());
                        for (const auto& m : kMult) {
                            all.push_back({id, m.field, v, true});
                            g_fieldInput[std::to_string(id) + m.field] = (int)v;
                        }
                        savePlayerEdits(all);
                        toast(nameOf(g_text.players, (int)id) + " · 훈련 배수 4가지 모두 x" + std::to_string(v));
                    }
                    fitSameLine(btnW("전부 해제"));
                    if (ImGui::SmallButton("전부 해제")) {
                        std::vector<PlayerEdit> all = loadPlayerEdits();
                        all.erase(std::remove_if(all.begin(), all.end(), isMult), all.end());
                        savePlayerEdits(all);
                        toast(nameOf(g_text.players, (int)id) + " · 훈련 배수 4가지 해제 (전체 설정 따름)");
                    }
                    ImGui::PopID();
                }
            }
            if (ImGui::CollapsingHeader("기본", ImGuiTreeNodeFlags_DefaultOpen))
                for (const Field& f : kBasic)
                    row(f.key, f.label, "지금 " + std::to_string(fieldValue(rec, f.offset, f.size)), f.min, f.max,
                        fieldValue(rec, f.offset, f.size));
            const char* groups[] = {"능력", "포지션 적성", "전술 이해", "포메이션 이해"};
            const int starts[] = {0, 19, 30, 38, 46};
            for (int g = 0; g < 4; g++)
                if (ImGui::CollapsingHeader(groups[g], g == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0))
                    for (int a = starts[g]; a < starts[g + 1]; a++) {
                        row("a" + std::to_string(a), g_text.abilities[a],
                            "지금 " + std::to_string(rd8(rec + 0x0C + a)) + " / 한계 " + std::to_string(rd8(rec + 0x3B + a)), 0, 100,
                            rd8(rec + 0x0C + a));
                        row("l" + std::to_string(a), "  └ 한계", "", 0, 100, rd8(rec + 0x3B + a));
                    }
            // As the launcher's page: abilities or limits, all 46 at 100 and locked.
            for (int limits = 0; limits < 2; limits++) {
                if (limits)
                    fitSameLine(btnW("한계 전부 100 고정"));
                if (ImGui::Button(limits ? "한계 전부 100 고정" : "능력 전부 100 고정")) {
                    const char k = limits ? 'l' : 'a';
                    std::vector<PlayerEdit> all = loadPlayerEdits();
                    all.erase(std::remove_if(all.begin(), all.end(), [&](const PlayerEdit& x) { return x.id == id && x.field[0] == k; }), all.end());
                    for (int a = 0; a < 46; a++)
                        all.push_back({id, std::string(1, k) + std::to_string(a), 100, true});
                    savePlayerEdits(all);
                    for (int a = 0; a < 46; a++) {   // the boxes show what was just set
                        g_fieldInput[std::to_string(id) + k + std::to_string(a)] = 100;
                        g_fieldLock[std::to_string(id) + k + std::to_string(a)] = true;
                    }
                    toast(nameOf(g_text.players, (int)id) + (limits ? ": 한계 전부 100 (고정)" : ": 능력 전부 100 (고정)"));
                }
            }
            fitSameLine(btnW("이 선수 수정 전부 해제"));
            if (ImGui::Button("이 선수 수정 전부 해제")) {
                std::vector<PlayerEdit> all = loadPlayerEdits();
                all.erase(std::remove_if(all.begin(), all.end(), [&](const PlayerEdit& x) { return x.id == id; }), all.end());
                savePlayerEdits(all);
                toast(nameOf(g_text.players, (int)id) + ": 수정 전부 해제");
            }
        }
    }
    ImGui::EndChild();
}

struct ListDef { const char* title; const char* list; int slots; uint32_t addr, stride; int maxId; };
const ListDef kLists[] = {
    {"코치 후보", "coach", 4, 0x00EE72A4u, 0xCu, 0x33A},
    {"스카우트 후보", "scout", 4, 0x00EE72D4u, 0xCu, 0x1AC},
    {"유럽 선수 목록", "euro", 20, 0x00EE70E8u, 0x14u, 0x46A2},
};
char g_listSearch[3][96];
int g_listNation[3] = {-1, -1, -1};
int g_listSlot[3] = {0, 0, 0};
bool g_listLock[3] = {false, false, false};   // off until asked for (owner, 2026-11-08)
int g_listSort[3] = {0, 0, 0};                 // kSortNames; 0 = ID (or the closest first for an English search)
bool g_listRev[3] = {false, false, false};     // the other way round (grade and age: the highest first by default)

// The people of each list with their country, built once (the master tables are static data).
std::vector<std::pair<int, int>> g_people[3];
std::map<int, int> g_nationOf[3];

void buildPeople(int L, const Names& names, int maxId)
{
    if (!g_people[L].empty())
        return;
    for (const auto& kv : names)
        if (kv.first > 0 && kv.first < maxId) {
            const int n = L == 0 ? staffNation(kv.first) : L == 1 ? scoutNation(kv.first) : playerNation(kv.first);
            g_people[L].push_back({kv.first, n});
            g_nationOf[L][kv.first] = n;
        }
}

// English search over the game's Japanese names (owner, 2026-11-08: "영어로도 검색이 가능하도록"). The launcher's
// NameSearch.cs has the same rules and says why: katakana → Hepburn romaji, both sides folded to one loose spelling,
// a match within a few edits where a vowel edit costs half.
namespace namesearch {
const char* const kKata[] = {
    "a", "a", "i", "i", "u", "u", "e", "e", "o", "o",
    "ka", "ga", "ki", "gi", "ku", "gu", "ke", "ge", "ko", "go",
    "sa", "za", "shi", "ji", "su", "zu", "se", "ze", "so", "zo",
    "ta", "da", "chi", "ji", "", "tsu", "zu", "te", "de", "to", "do",
    "na", "ni", "nu", "ne", "no",
    "ha", "ba", "pa", "hi", "bi", "pi", "fu", "bu", "pu", "he", "be", "pe", "ho", "bo", "po",
    "ma", "mi", "mu", "me", "mo",
    "ya", "ya", "yu", "yu", "yo", "yo",
    "ra", "ri", "ru", "re", "ro",
    "wa", "wa", "wi", "we", "wo", "n", "vu", "ka", "ke",
};
bool vowel(char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }
bool endsWith(const std::string& s, const char* e)
{
    const size_t n = std::strlen(e);
    return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}
std::string romaji(const std::string& utf8)
{
    std::string o;
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c0 = (unsigned char)utf8[i];
        uint32_t cp = 0;
        size_t len = 1;
        if (c0 < 0x80) cp = c0;
        else if ((c0 >> 5) == 6 && i + 1 < utf8.size()) { cp = ((c0 & 0x1Fu) << 6) | (utf8[i + 1] & 0x3F); len = 2; }
        else if ((c0 >> 4) == 14 && i + 2 < utf8.size()) { cp = ((c0 & 0x0Fu) << 12) | ((utf8[i + 1] & 0x3Fu) << 6) | (utf8[i + 2] & 0x3F); len = 3; }
        else if ((c0 >> 3) == 30) len = 4;
        i += len;
        if (cp >= 0x3041 && cp <= 0x3096)
            cp += 0x60;   // hiragana → katakana
        if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) {
            o += (char)std::tolower((int)cp);   // Latin letters (a legend tag's real name) as they are
            continue;
        }
        if (cp == 0x30FC || cp == 0x30C3)
            continue;     // ー and ッ: doubled letters fold away anyway
        if (cp < 0x30A1 || cp > 0x30F6) {
            if (!o.empty() && o.back() != ' ')
                o += ' ';
            continue;
        }
        const std::string r = kKata[cp - 0x30A1];
        const bool smallVowel = cp == 0x30A1 || cp == 0x30A3 || cp == 0x30A5 || cp == 0x30A7 || cp == 0x30A9;
        const bool smallY = cp == 0x30E3 || cp == 0x30E5 || cp == 0x30E7;
        const size_t n = o.size();
        if (smallVowel && n > 0) {
            if (o[n - 1] == 'u' && (n == 1 || o[n - 2] == ' '))
                o[n - 1] = 'w';
            else if (vowel(o[n - 1]))
                o.pop_back();
            o += r;
        } else if (smallY && n > 0) {
            if (endsWith(o, "shi") || endsWith(o, "chi") || endsWith(o, "ji")) {
                o.pop_back();
                o += r.substr(1);
            } else if (vowel(o[n - 1])) {
                o.pop_back();
                o += r;
            } else
                o += r;
        } else
            o += r;
    }
    const size_t a = o.find_first_not_of(' '), b = o.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : o.substr(a, b - a + 1);
}
void replaceAll(std::string& s, const char* from, const char* to)
{
    const size_t fl = std::strlen(from), tl = std::strlen(to);
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + tl))
        s.replace(p, fl, to);
}
std::string key(const std::string& text)
{
    std::string t;
    for (char ch : text) {
        const char c = (char)std::tolower((unsigned char)ch);
        if (c >= 'a' && c <= 'z')
            t += c;
    }
    const char* pairs[][2] = {{"sch", "s"}, {"tch", "C"}, {"ch", "C"}, {"sh", "s"}, {"ph", "f"}, {"th", "s"}, {"ck", "k"},
                              {"dj", "z"}, {"dz", "z"}, {"tz", "s"}, {"ts", "s"}, {"nh", "n"}};
    for (const auto& p : pairs)
        replaceAll(t, p[0], p[1]);
    std::string k;
    for (size_t i = 0; i < t.size(); i++) {
        const char c = t[i], next = i + 1 < t.size() ? t[i + 1] : ' ', after = i + 2 < t.size() ? t[i + 2] : ' ';
        if ((c == 'q' || c == 'g') && next == 'u' && (after == 'e' || after == 'i')) {
            k += c == 'q' ? 'k' : 'g';
            i++;
            continue;
        }
        switch (c) {
        case 'c': k += (next == 'e' || next == 'i' || next == 'y') ? 's' : 'k'; break;
        case 'C': k += 'c'; break;
        case 'q': k += 'k'; break;
        case 'x': k += "ks"; break;
        case 'v': k += 'b'; break;
        case 'l': k += 'r'; break;
        case 'j': k += 'z'; break;
        case 'y': k += 'i'; break;
        case 'h': break;
        default: k += c; break;
        }
    }
    std::string o;
    for (char c : k)
        if (o.empty() || o.back() != c)
            o += c;
    return o;
}
int distance(const std::string& q, const std::string& t)
{
    const size_t m = q.size(), n = t.size();
    std::vector<int> prev(n + 1, 0), cur(n + 1, 0);
    for (size_t i = 1; i <= m; i++) {
        const int qc = vowel(q[i - 1]) ? 1 : 2;
        cur[0] = prev[0] + qc;
        for (size_t j = 1; j <= n; j++) {
            const int tc = vowel(t[j - 1]) ? 1 : 2;
            const int sub = q[i - 1] == t[j - 1] ? 0 : std::max(qc, tc);
            cur[j] = std::min(std::min(prev[j] + qc, cur[j - 1] + tc), prev[j - 1] + sub);
        }
        std::swap(prev, cur);
    }
    return *std::min_element(prev.begin(), prev.end());
}
bool isEnglish(const std::string& s)
{
    bool letter = false;
    for (char ch : s) {
        if ((unsigned char)ch > 127)
            return false;
        letter |= std::isalpha((unsigned char)ch) != 0;
    }
    return letter;
}
// [0] the whole name's spelling, then each word's.
const std::vector<std::string>& nameKeys(const std::string& name)
{
    static std::map<std::string, std::vector<std::string>> cache;
    auto it = cache.find(name);
    if (it == cache.end()) {
        const std::string r = romaji(name);
        std::vector<std::string> k{key(r)};
        std::istringstream words(r);
        for (std::string w; words >> w;)
            k.push_back(key(w));
        it = cache.emplace(name, std::move(k)).first;
    }
    return it->second;
}
// -1 = no match, else an order (lower first): a whole word 0, a word's start 1, inside a word 2 + 4 per edit cost,
// across words 4 + 4 per edit cost (as NameSearch.cs).
int score(const std::string& name, const std::string& query)
{
    const std::string q = key(query);
    if (q.size() < 2)
        return -1;
    const std::vector<std::string>& k = nameKeys(name);
    if (k[0].empty())
        return -1;
    const int tolerance = q.size() <= 3 ? 0 : q.size() <= 4 ? 1 : q.size() <= 7 ? 2 : q.size() <= 10 ? 3 : 4;
    int best = -1;
    for (size_t w = 1; w < k.size(); w++) {
        int order;
        if (k[w] == q)
            order = 0;
        else if (k[w].compare(0, q.size(), q) == 0)
            order = 1;
        else {
            const int d = distance(q, k[w]);
            if (d > tolerance)
                continue;
            order = 2 + 4 * d;
        }
        if (best < 0 || order < best)
            best = order;
    }
    if (best < 0) {
        const int d = distance(q, k[0]);
        if (d <= tolerance)
            best = 4 + 4 * d;
    }
    return best;
}
}   // namespace namesearch

void listsPanel()
{
    loadTexts();
    // SAKA_OVERLAY_SEARCH=<text> / SAKA_OVERLAY_LIST=0-2 (tests): the search boxes' first text and the list shown first.
    static int testList = -2;
    if (testList == -2) {
        if (const char* s = std::getenv("SAKA_OVERLAY_SEARCH"))
            for (auto& box : g_listSearch)
                std::snprintf(box, sizeof box, "%s", s);
        const char* l = std::getenv("SAKA_OVERLAY_LIST");
        testList = l ? std::atoi(l) : -1;
    }
    std::vector<ListEdit> edits = loadListEdits();
    if (rightEndButton("새로고침##lists")) {
        saveListEdits(edits);
        toast("목록 새로고침 (고른 이름을 게임에 다시 넣음)");
    }
    if (ImGui::BeginTabBar("lists")) {
        for (int L = 0; L < 3; L++) {
            const ListDef& d = kLists[L];
            if (!ImGui::BeginTabItem(d.title, nullptr, testList == L ? ImGuiTabItemFlags_SetSelected : 0))
                continue;
            const Names& names = L == 0 ? g_text.staff : L == 1 ? g_text.scouts : g_text.players;
            buildPeople(L, names, d.maxId);
            auto nation = [&](int id) { auto it = g_nationOf[L].find(id); return it != g_nationOf[L].end() ? it->second : -1; };
            // The slots: current name and country; the chosen slot gets the pick.
            if (g_resetSlots) {   // 기본 크기
                g_resetSlots = false;
                ImGui::SetNextWindowSize(ImVec2(Px(460), 0.0f), ImGuiCond_Always);
            }
            ImGui::BeginChild("slots", ImVec2(Px(460), -g_footerH), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX,
                              ImGuiWindowFlags_HorizontalScrollbar);
            for (int s = 0; s < d.slots; s++) {
                const uint32_t cur = committed(d.addr + d.stride * s, 4) ? rd32(d.addr + d.stride * s) : 0;
                std::string text = std::to_string(s + 1) + "번  " + (cur ? nameOf(names, (int)cur) + "  · " + nameOf(g_text.nations, nation((int)cur)) : "(비어 있음)");
                for (const ListEdit& e : edits)
                    if (e.list == d.list && e.slot == s)
                        text += "  [" + std::string(e.lock ? "고정" : "한 번") + "]";
                if (ImGui::Selectable((text + "##s" + std::to_string(s)).c_str(), g_listSlot[L] == s))
                    g_listSlot[L] = s;
            }
            ImGui::Separator();
            // An empty euro slot needs a free record in the game's 500 overseas status records (0x00F0C4B8, 12 bytes):
            // when all are used the game state has no room for one more player there (2026-11-09, owner: "교체도 잘
            // 작동하지 않아"; the live save had 500 of 500 used). A slot with a player takes over that player's record.
            if (L == 2 && g_listSlot[L] >= 0 && g_listSlot[L] < d.slots) {
                const uint32_t slotAddr = d.addr + d.stride * g_listSlot[L];
                if (committed(slotAddr, 4) && rd32(slotAddr) == 0 && committed(0x00F0C4B8u, 500u * 0xCu)) {
                    int freeRecords = 0;
                    for (uint32_t r = 0; r < 500; r++)
                        freeRecords += rd32(0x00F0C4B8u + 0xCu * r) == 0;
                    if (freeRecords == 0)
                        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "빈 칸: 게임의 해외 선수 기록 500칸이 다 차서\n넣을 수 없음. 이름이 있는 칸을 바꾸세요.");
                }
            }
            ImGui::Checkbox("고정 (게임이 목록을 다시 만들어도 유지)", &g_listLock[L]);
            if (ImGui::Button("이 칸 해제")) {
                edits.erase(std::remove_if(edits.begin(), edits.end(), [&](const ListEdit& e) { return e.list == d.list && e.slot == g_listSlot[L]; }), edits.end());
                saveListEdits(edits);
                toast(std::string(d.title) + " " + std::to_string(g_listSlot[L] + 1) + "번: 해제");
            }
            // The whole list (2026-11-09, owner: "목록은 새로고침이 가능하게", "전체적용 고정"): 새로고침 writes the file
            // again so the game puts the once picks in again (after it made a new list); 전부 고정 / 전부 한 번만; 전부 해제.
            for (int lockAll = 1; lockAll >= 0; lockAll--) {
                ImGui::SameLine();
                if (ImGui::Button(lockAll ? "전부 고정" : "전부 한 번만")) {
                    for (ListEdit& e : edits)
                        if (e.list == d.list)
                            e.lock = lockAll != 0;
                    saveListEdits(edits);
                    g_listLock[L] = lockAll != 0;
                    toast(std::string(d.title) + (lockAll ? ": 전부 고정" : ": 전부 한 번만"));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("전부 해제")) {
                edits.erase(std::remove_if(edits.begin(), edits.end(), [&](const ListEdit& e) { return e.list == d.list; }), edits.end());
                saveListEdits(edits);
                toast(std::string(d.title) + ": 전부 해제");
            }
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("pick", ImVec2(0, -g_footerH), 0, ImGuiWindowFlags_HorizontalScrollbar);
            // Country filter: the countries of this list's people, the most common first.
            std::map<int, int> count;
            for (const auto& pn : g_people[L])
                count[pn.second]++;
            std::vector<std::pair<int, int>> countries(count.begin(), count.end());
            std::sort(countries.begin(), countries.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) { return a.second > b.second; });
            const std::string current = g_listNation[L] < 0 ? "전체 국가" : nameOf(g_text.nations, g_listNation[L]);
            ImGui::SetNextItemWidth(Px(260));
            if (ImGui::BeginCombo("국가", current.c_str())) {
                if (ImGui::Selectable("전체 국가", g_listNation[L] < 0))
                    g_listNation[L] = -1;
                for (const auto& c : countries)
                    if (ImGui::Selectable((nameOf(g_text.nations, c.first) + " (" + std::to_string(c.second) + "명)").c_str(), g_listNation[L] == c.first))
                        g_listNation[L] = c.first;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Px(260));
            ImGui::InputTextWithHint("##search", "이름, 영어 이름 또는 ID", g_listSearch[L], sizeof g_listSearch[L]);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Px(110));
            ImGui::Combo("정렬", &g_listSort[L], kSortNames, 5);
            ImGui::SameLine();
            ImGui::Checkbox("역순", &g_listRev[L]);
            const std::string search = g_listSearch[L];
            ImGui::TextDisabled("고른 이름이 왼쪽에서 고른 %d번 칸에 들어갑니다.", g_listSlot[L] + 1);
            if (ImGui::BeginListBox("##names", ImVec2(-1, -1))) {
                // The people shown, worked out again only when the search or the country changes: an English search
                // (romaji) scores every name, the closest first; Japanese text or an ID matches as before.
                const bool english = namesearch::isEnglish(search);
                static std::string hitsFor[3];
                static std::vector<std::pair<int, int>> hits[3];   // (score, index into g_people[L])
                const std::string hitsKey = search + "\x1f" + std::to_string(g_listNation[L]) + "\x1f" +
                                            std::to_string(g_listSort[L]) + (g_listRev[L] ? "r" : "");
                if (hitsFor[L] != hitsKey) {
                    hitsFor[L] = hitsKey;
                    hits[L].clear();
                    for (size_t p = 0; p < g_people[L].size(); p++) {
                        const auto& pn = g_people[L][p];
                        const auto kvIt = names.find(pn.first);
                        if (kvIt == names.end() || (g_listNation[L] >= 0 && pn.second != g_listNation[L]))
                            continue;
                        int sc = 0;
                        if (!search.empty()) {
                            sc = english ? namesearch::score(kvIt->second, search)
                                         : (kvIt->second.find(search) != std::string::npos || std::to_string(kvIt->first) == search ? 0 : -1);
                            if (sc < 0)
                                continue;
                        } else if (g_listNation[L] < 0 && d.maxId > 900)
                            break;   // 18,082 players: pick a country or search first
                        hits[L].push_back({sc, (int)p});
                    }
                    const int sortBy = g_listSort[L];
                    if (sortBy != 0) {   // 이름 / 국적 / 등급 / 나이, then the ID; grade and age the highest first
                        auto bare = [&](int person) {   // the name without its tag
                            const std::string& s = names.at(person);
                            return s.substr(0, std::min(tagPos(s), s.size()));
                        };
                        const bool high = sortBy >= 3;
                        std::stable_sort(hits[L].begin(), hits[L].end(), [&](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                            const auto& pa = g_people[L][a.second];
                            const auto& pb = g_people[L][b.second];
                            int c = 0;
                            if (sortBy == 1) {
                                const std::string na = bare(pa.first), nb = bare(pb.first);
                                c = na < nb ? -1 : na > nb ? 1 : 0;
                            } else if (sortBy == 2)
                                c = pa.second - pb.second;
                            else {
                                const int va = sortBy == 3 ? personGrade(L, pa.first) : personAge(L, pa.first);
                                const int vb = sortBy == 3 ? personGrade(L, pb.first) : personAge(L, pb.first);
                                c = high ? vb - va : va - vb;
                            }
                            if (c == 0)
                                c = pa.first - pb.first;
                            return g_listRev[L] ? c > 0 : c < 0;
                        });
                    } else if (english)   // the closest first, then the shorter name
                        std::stable_sort(hits[L].begin(), hits[L].end(), [&](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                            if (a.first != b.first)
                                return a.first < b.first;
                            auto base = [&](int person) {   // without the legend tag
                                const std::string& s = names.at(person);
                                const size_t tag = tagPos(s);
                                return tag == std::string::npos ? s.size() : tag;
                            };
                            return base(g_people[L][a.second].first) < base(g_people[L][b.second].first);
                        });
                    else if (g_listRev[L])
                        std::reverse(hits[L].begin(), hits[L].end());
                }
                int shown = 0;
                for (const auto& hit : hits[L]) {
                    const auto& pn = g_people[L][hit.second];
                    const auto kvIt = names.find(pn.first);
                    if (kvIt == names.end())
                        continue;
                    const auto& kv = *kvIt;
                    const int n = pn.second;
                    if (++shown > 400)
                        break;
                    const size_t tag = tagPos(kv.second);   // the brackets: the Japanese name's romaji only
                    const int grade = personGrade(L, kv.first), age = personAge(L, kv.first);
                    const std::string item = kv.second + (english ? "  [" + namesearch::romaji(kv.second.substr(0, tag)) + "]" : std::string()) +
                                             "  (" + std::to_string(kv.first) + ")  · " + nameOf(g_text.nations, n) +
                                             "  · 등급 " + gradeText(L, grade) + (age >= 0 ? "  · " + std::to_string(age) + "세" : std::string());
                    if (ImGui::Selectable(item.c_str())) {
                        edits.erase(std::remove_if(edits.begin(), edits.end(), [&](const ListEdit& e) { return e.list == d.list && e.slot == g_listSlot[L]; }), edits.end());
                        edits.push_back({d.list, g_listSlot[L], (uint32_t)kv.first, g_listLock[L]});
                        saveListEdits(edits);
                        toast(std::string(d.title) + " " + std::to_string(g_listSlot[L] + 1) + "번 = " + kv.second);
                    }
                }
                if (shown == 0)
                    ImGui::TextDisabled(d.maxId > 900 && search.empty() && g_listNation[L] < 0 ? "국가를 고르거나 이름을 찾으세요." : "해당하는 이름이 없습니다.");
                ImGui::EndListBox();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        testList = -1;
    }
}

// ---- 연습: training slots (2026-11-13, owner: "스페셜과 비장의 아이 모두 F2에서 내가 훈련도 지정", "완료해서 성공했다고
// 슬롯에서 바로 빼지말고 계속 담아 놓을 수 있도록 고정하는 기능을 F2에서", "3개가 아닌 몇개까지 ... 최대한의 슬롯") -------------
// training.txt "sp|hizo\tslot\tplayer\tkind\tcourse\tlock", applied by saka_hooks.c cheats_training_apply. The slots stay
// 3 of each: the save keeps 3, with other data right after them (REA: the loops are < 3 and the arrays touch).
struct TrainEdit { std::string kind; int slot; uint32_t id; int sub; int course; bool lock; };
std::vector<TrainEdit> loadTrainEdits()
{
    std::vector<TrainEdit> out;
    for (const std::string& l : readLines(profileFile("training.txt").c_str())) {
        std::vector<std::string> f = split(l, '\t');
        if (f.size() >= 5)
            out.push_back({f[0], std::atoi(f[1].c_str()), (uint32_t)std::strtoul(f[2].c_str(), nullptr, 10), std::atoi(f[3].c_str()),
                           std::atoi(f[4].c_str()), f.size() < 6 || f[5] != "0"});
    }
    return out;
}
void saveTrainEdits(const std::vector<TrainEdit>& edits)
{
    std::vector<std::string> lines;
    for (const TrainEdit& e : edits)
        lines.push_back(e.kind + "\t" + std::to_string(e.slot) + "\t" + std::to_string(e.id) + "\t" + std::to_string(e.sub) + "\t" +
                        std::to_string(e.course) + "\t" + (e.lock ? "1" : "0"));
    writeLines(profileFile("training.txt").c_str(), lines);
}

void trainingPanel()
{
    loadTexts();
    const uint32_t roster = 0x00EE2954u, record = 0x218u;
    if (!committed(roster, 23 * record) || !committed(0x00EE83A8u, 0x84u)) {
        ImGui::TextDisabled("게임 데이터가 아직 없습니다 (세이브를 불러온 뒤).");
        return;
    }
    std::vector<TrainEdit> edits = loadTrainEdits();
    note("칸마다 선수와 코스를 골라 적용. 고정이면 코스가 끝나 게임이 칸을 비우거나 다른 코스로 바꿔도 같은 선수·코스로 다시 "
         "채움 (코스 비용은 들지 않음). 칸은 특별연습·비장의 아이 각 3칸이 게임 데이터의 최대 (바로 뒤에 다른 세이브 데이터가 있어 "
         "늘릴 수 없음). 한 선수를 여러 칸에: 치트 탭 '같은 선수 여러 칸'. 1주 완료: 'SP 1턴 완료, 경험치 그대로'·'비장의 아이 1턴 "
         "완료'. 성공: '비장의 아이 반드시 성공' (특별연습은 실패 판정이 없음).");
    std::vector<std::pair<uint32_t, std::string>> players;   // the club's players
    for (int s = 0; s < 23; s++) {
        const uint32_t id = rd32(roster + record * (uint32_t)s);
        if (validPlayer(id))
            players.push_back({id, nameOf(g_text.players, (int)id)});
    }
    static int pickPlayer[6] = {-1, -1, -1, -1, -1, -1}, pickCourse[6], pickSub[6];
    static bool pickLock[6] = {true, true, true, true, true, true}, init[6];
    for (int row = 0; row < 6; row++) {
        const bool sp = row < 3;
        const int slot = row % 3;
        const char* kind = sp ? "sp" : "hizo";
        if (row == 0)
            ImGui::SeparatorText("특별연습 (3칸)");
        if (row == 3)
            ImGui::SeparatorText("비장의 아이 (3칸)");
        ImGui::PushID(row);
        const uint32_t ea = sp ? 0x00EE83A8u + 0xCu * slot : 0x00EE83CCu + 0x20u * slot;
        const uint32_t curId = rd32(ea);
        const int curSub = sp ? 0 : (int)rd8(ea + 4);   // the kind is the byte at +4 (2026-11-14)
        const int curCourse = (int)rd32(ea + (sp ? 4 : 8));
        const int weeks = rd8(ea + (sp ? 8 : 0xC));
        const Names& curNames = sp ? g_text.spCourses : curSub ? g_text.awakCourses : g_text.persCourses;
        std::string now = std::to_string(slot + 1) + "번  지금: " +
                          (curId ? nameOf(g_text.players, (int)curId) + "  · " + (sp ? "" : curSub ? "능력각성 " : "개성강화 ") +
                                       nameOf(curNames, curCourse) + "  · 남은 " + std::to_string(weeks) + "주"
                                 : std::string("(비어 있음)"));
        const TrainEdit* mine = nullptr;
        for (const TrainEdit& e : edits)
            if (e.kind == kind && e.slot == slot)
                mine = &e;
        if (mine)
            now += std::string("  [") + (mine->lock ? "고정" : "한 번") + ": " + nameOf(g_text.players, (int)mine->id) + "]";
        ImGui::TextUnformatted(now.c_str());
        if (!init[row]) {   // the first picks: the designation, else the slot's current plan
            init[row] = true;
            const uint32_t want = mine ? mine->id : curId;
            for (size_t k = 0; k < players.size(); k++)
                if (players[k].first == want)
                    pickPlayer[row] = (int)k;
            pickCourse[row] = mine ? mine->course : curCourse;
            pickSub[row] = mine ? mine->sub : curSub;
            if (mine)
                pickLock[row] = mine->lock;
        }
        ImGui::SetNextItemWidth(Px(220));
        const bool havePick = pickPlayer[row] >= 0 && pickPlayer[row] < (int)players.size();
        if (ImGui::BeginCombo("##player", havePick ? players[pickPlayer[row]].second.c_str() : "선수 고르기")) {
            for (size_t k = 0; k < players.size(); k++)
                if (ImGui::Selectable((players[k].second + "##p" + std::to_string(k)).c_str(), pickPlayer[row] == (int)k))
                    pickPlayer[row] = (int)k;
            ImGui::EndCombo();
        }
        if (!sp) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Px(110));
            const char* subs[2] = {"개성강화", "능력각성"};
            if (ImGui::Combo("##sub", &pickSub[row], subs, 2))
                pickCourse[row] = 0;
        }
        ImGui::SameLine();
        const Names& names = sp ? g_text.spCourses : pickSub[row] ? g_text.awakCourses : g_text.persCourses;
        ImGui::SetNextItemWidth(Px(250));
        const std::string cname = pickCourse[row] > 0 ? nameOf(names, pickCourse[row]) + " (" + std::to_string(pickCourse[row]) + ")" : "코스 고르기";
        if (ImGui::BeginCombo("##course", cname.c_str())) {
            for (const auto& kv : names) {
                if (kv.first <= 0 || kv.second.empty() || kv.second == "なし" || kv.second.find("総数") != std::string::npos)
                    continue;
                if (ImGui::Selectable((kv.second + " (" + std::to_string(kv.first) + ")").c_str(), pickCourse[row] == kv.first))
                    pickCourse[row] = kv.first;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Checkbox("고정", &pickLock[row]);
        ImGui::SameLine();
        const std::string what = std::string(sp ? "특별연습 " : "비장의 아이 ") + std::to_string(slot + 1) + "번";
        if (ImGui::Button("적용")) {
            if (!havePick || pickCourse[row] <= 0)
                toast(what + ": 선수와 코스를 고르세요", 1);
            else {
                edits.erase(std::remove_if(edits.begin(), edits.end(), [&](const TrainEdit& e) { return e.kind == kind && e.slot == slot; }), edits.end());
                edits.push_back({kind, slot, players[pickPlayer[row]].first, sp ? 0 : pickSub[row], pickCourse[row], pickLock[row]});
                saveTrainEdits(edits);
                toast(what + ": " + players[pickPlayer[row]].second + " · " + nameOf(names, pickCourse[row]) + (pickLock[row] ? " (고정)" : " (한 번)"));
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("해제")) {
            edits.erase(std::remove_if(edits.begin(), edits.end(), [&](const TrainEdit& e) { return e.kind == kind && e.slot == slot; }), edits.end());
            saveTrainEdits(edits);
            toast(what + ": 해제 (지금 칸의 코스는 끝까지 그대로)");
        }
        ImGui::PopID();
    }
}

// ---- 스태프: the hired coach and scout (2026-11-13, owner: "코치와 스카우트들의 능력치도 내가 마음대로 편집하고 특능까지도
// 마음대로 편집 가능하게 해주고. 계약기간 나이등도 마찬가지") ------------------------------------------------------------------
// staff.txt "coach|scout\tfield\tvalue\tlock", applied by saka_hooks.c cheats_staff_apply (the fields are listed there).
struct StaffEdit { std::string who, field; long long value; bool lock; };
std::vector<StaffEdit> loadStaffEdits()
{
    std::vector<StaffEdit> out;
    for (const std::string& l : readLines(profileFile("staff.txt").c_str())) {
        std::vector<std::string> f = split(l, '\t');
        if (f.size() >= 3)
            out.push_back({f[0], f[1], std::atoll(f[2].c_str()), f.size() < 4 || f[3] != "0"});
    }
    return out;
}
void saveStaffEdits(const std::vector<StaffEdit>& edits)
{
    std::vector<std::string> lines;
    for (const StaffEdit& e : edits)
        lines.push_back(e.who + "\t" + e.field + "\t" + std::to_string(e.value) + "\t" + (e.lock ? "1" : "0"));
    writeLines(profileFile("staff.txt").c_str(), lines);
}

void staffPanel()
{
    loadTexts();
    std::vector<StaffEdit> edits = loadStaffEdits();
    note("고용한 코치·스카우트를 바꿉니다. 나이·계약·연봉은 고용 기록, 등급·정책·시스템·특능은 그 사람의 마스터 기록에 씀 (게임이 효과를 "
         "늘 거기서 읽음; 같은 사람을 쓰는 다른 구단이 있으면 그쪽도 같이 바뀜). 고정이면 1초에 10번 다시 씀, 아니면 한 번 (고용한 "
         "사람이 바뀌면 다시 한 번).");
    static std::map<std::string, int> input;
    static std::map<std::string, bool> lockOf;
    auto setEdit = [&](const std::string& who, const std::string& field, long long v, bool lock) {
        edits.erase(std::remove_if(edits.begin(), edits.end(), [&](const StaffEdit& e) { return e.who == who && e.field == field; }), edits.end());
        edits.push_back({who, field, v, lock});
    };
    auto dropEdit = [&](const std::string& who, const std::string& field) {
        edits.erase(std::remove_if(edits.begin(), edits.end(), [&](const StaffEdit& e) { return e.who == who && e.field == field; }), edits.end());
    };
    auto findEdit = [&](const std::string& who, const std::string& field) -> const StaffEdit* {
        for (const StaffEdit& e : edits)
            if (e.who == who && e.field == field)
                return &e;
        return nullptr;
    };
    for (int w = 0; w < 2; w++) {
        const bool coach = w == 0;
        const std::string who = coach ? "coach" : "scout";
        const uint32_t hired = coach ? 0x00EE2940u : 0x00EE6CF8u;
        ImGui::SeparatorText(coach ? "코치" : "스카우트");
        if (!committed(hired, 0x14)) {
            ImGui::TextDisabled("게임 데이터가 아직 없습니다 (세이브를 불러온 뒤).");
            continue;
        }
        const uint32_t id = rd32(hired);
        if (id == 0 || (id & 0x80000000u) || id >= (coach ? 0x33Au : 0x1ACu)) {
            ImGui::TextDisabled(id ? "직접 만든 코치는 아직 편집하지 않습니다." : "고용한 사람이 없습니다.");
            continue;
        }
        const uint32_t master = coach ? 0x00BBA60Cu + id * 100u : 0x00E1F6ACu + id * 60u;
        const Names& names = coach ? g_text.staff : g_text.scouts;
        ImGui::Text("%s  (ID %u)  · %s", nameOf(names, (int)id).c_str(), id,
                    nameOf(g_text.nations, coach ? staffNation((int)id) : scoutNation((int)id)).c_str());
        ImGui::PushID(w);
        auto row = [&](const char* field, const char* label, long long cur, int lo, int hi, const std::string& shown) {
            ImGui::PushID(field);
            const std::string k = who + "." + field;
            if (!input.count(k))
                input[k] = (int)cur;
            if (!lockOf.count(k))
                lockOf[k] = true;
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s: %s", label, shown.c_str());
            ImGui::SameLine(Px(300));
            ImGui::SetNextItemWidth(Px(130));
            if (ImGui::InputInt("##v", &input[k]))
                input[k] = std::max(lo, std::min(hi, input[k]));
            ImGui::SameLine();
            ImGui::Checkbox("고정", &lockOf[k]);
            ImGui::SameLine();
            if (ImGui::Button("적용")) {
                setEdit(who, field, input[k], lockOf[k]);
                saveStaffEdits(edits);
                toast(std::string(coach ? "코치 " : "스카우트 ") + label + " = " + std::to_string(input[k]) + (lockOf[k] ? " (고정)" : " (한 번)"));
            }
            ImGui::SameLine();
            if (ImGui::Button("해제")) {
                dropEdit(who, field);
                saveStaffEdits(edits);
                toast(std::string(coach ? "코치 " : "스카우트 ") + label + ": 해제");
            }
            if (const StaffEdit* e = findEdit(who, field)) {
                ImGui::SameLine();
                ImGui::TextDisabled("[%s %lld]", e->lock ? "고정" : "한 번", e->value);
            }
            ImGui::PopID();
        };
        const int age = rd8(hired + 4), years = rd8(hired + 5), rank = rd8(master + (coach ? 9 : 8));
        const uint32_t salary = rd32(hired + 8);
        row("age", "나이", age, 15, 99, std::to_string(age) + "세");
        row("years", "계약 남은 해", years, 1, 9, std::to_string(years) + "년" + ((rd32(hired + 0xC) & 1u) ? " (재계약 안 함 표시)" : ""));
        row("salary", "연봉 (만엔)", salary, 0, 100000, std::to_string(salary) + "만");
        row("rank", "등급", rank, 0, 15, coach ? gradeText(0, rank) : std::to_string(rank));
        if (rd32(hired + 0xC) & 1u) {
            if (ImGui::Button("떠나지 않게 (재계약 가능으로)")) {
                setEdit(who, "keep", 1, true);
                saveStaffEdits(edits);
                toast(std::string(coach ? "코치" : "스카우트") + ": 재계약 안 함 표시를 지움 (고정)");
            }
        }
        if (coach) {
            const int policy = rd8(master + 0xA), circle = rd8(master + 0xB);
            row("policy", "정책 위치 (0~24)", policy, 0, 24, std::to_string(policy));
            row("circle", "정책 원 크기 (0~3)", circle, 0, 3, std::to_string(circle));
            // systems 3-4-3 .. 5-4-1 (+0x18..+0x1F)
            static const char* const kSys[8] = {"3-4-3", "3-5-2", "3-6-1", "4-3-3", "4-4-2", "4-5-1", "5-3-2", "5-4-1"};
            static bool sys[8], sysInit;
            static bool sysLock = true;
            if (!sysInit) {
                sysInit = true;
                for (int s = 0; s < 8; s++)
                    sys[s] = rd8(master + 0x18 + s) != 0;
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("잘하는 시스템:");
            for (int s = 0; s < 8; s++) {
                ImGui::SameLine();
                ImGui::Checkbox(kSys[s], &sys[s]);
            }
            ImGui::SameLine();
            ImGui::Checkbox("고정##sys", &sysLock);
            ImGui::SameLine();
            if (ImGui::Button("적용##sys")) {
                for (int s = 0; s < 8; s++)
                    setEdit(who, "sys" + std::to_string(s), sys[s] ? 1 : 0, sysLock);
                saveStaffEdits(edits);
                toast("코치 시스템 적용");
            }
            ImGui::SameLine();
            if (ImGui::Button("해제##sys")) {
                for (int s = 0; s < 8; s++)
                    dropEdit(who, "sys" + std::to_string(s));
                saveStaffEdits(edits);
                toast("코치 시스템: 해제");
            }
        }
        // abilities (8 x u32: coach +0x2C, scout +0x1C; names data\text2\staff_sp_name.txb)
        const uint32_t abBase = master + (coach ? 0x2Cu : 0x1Cu);
        static int ab[2][8];
        static bool abInit[2], abLock[2] = {true, true};
        if (!abInit[w]) {
            abInit[w] = true;
            for (int s = 0; s < 8; s++)
                ab[w][s] = (int)rd32(abBase + 4u * s);
        }
        ImGui::TextUnformatted("특능 (8칸):");
        for (int s = 0; s < 8; s++) {
            ImGui::PushID(100 + s);
            const int cur = (int)rd32(abBase + 4u * s);
            if (s % 2)
                ImGui::SameLine(Px(470));
            ImGui::SetNextItemWidth(Px(330));
            const std::string label = ab[w][s] ? nameOf(g_text.staffAbilities, ab[w][s]) : std::string("(없음)");
            if (ImGui::BeginCombo("##ab", (label + (cur != ab[w][s] ? "  *" : "")).c_str())) {
                if (ImGui::Selectable("(없음)", ab[w][s] == 0))
                    ab[w][s] = 0;
                for (const auto& kv : g_text.staffAbilities)
                    if (kv.first > 0 && kv.first < 0x100 && !kv.second.empty() && kv.second.find("総数") == std::string::npos &&
                        ImGui::Selectable((kv.second + " (" + std::to_string(kv.first) + ")").c_str(), ab[w][s] == kv.first))
                        ab[w][s] = kv.first;
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::Checkbox("고정##ab", &abLock[w]);
        ImGui::SameLine();
        if (ImGui::Button("특능 적용")) {
            for (int s = 0; s < 8; s++)
                setEdit(who, "ab" + std::to_string(s), ab[w][s], abLock[w]);
            saveStaffEdits(edits);
            toast(std::string(coach ? "코치" : "스카우트") + " 특능 적용");
        }
        ImGui::SameLine();
        if (ImGui::Button("특능 해제")) {
            for (int s = 0; s < 8; s++)
                dropEdit(who, "ab" + std::to_string(s));
            saveStaffEdits(edits);
            abInit[w] = false;
            toast(std::string(coach ? "코치" : "스카우트") + " 특능: 해제 (원래 값은 게임을 다시 켜면 돌아옴)");
        }
        ImGui::PopID();
    }
}

// ---- 팀컬러: the team color fit (2026-11-14, owner: "팀컬러를 설정하는것에 따라서 선발 선수들의 스킬과 플레이스타일이
// 변하도록 하는 치트") -------------------------------------------------------------------------------------------------
// teamcolor.txt "on / mode / colors / level" (tab separated), read by saka_hooks.c cheats_teamcolor_apply, which also
// gives the state shown here (saka_teamcolor.h). Names: cheats\teamcolors.txt (tools/make_teamcolors.py).
struct TeamColorName { std::string ko, ja, kind; bool unused = false; int orig = 0; };
std::map<int, TeamColorName> g_teamColors;
void loadTeamColors()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    for (const std::string& line : readLines("cheats\\teamcolors.txt")) {
        std::vector<std::string> f = split(line, '\t');
        for (std::string& x : f)
            while (!x.empty() && (x.back() == '\r' || x.back() == ' '))
                x.pop_back();
        if (f.size() >= 7 && f[0] == "color") {
            TeamColorName& c = g_teamColors[std::atoi(f[1].c_str())];
            c.ko = f[2];
            c.ja = f[3];
            c.kind = f[4];
            c.unused = f[5] == "1";
            c.orig = std::atoi(f[6].c_str());
        }
    }
}
std::string teamColorLabel(int id, bool japanese = true)
{
    if (id <= 0)
        return "(없음)";
    auto it = g_teamColors.find(id);
    if (it == g_teamColors.end())
        return "팀컬러 " + std::to_string(id);
    return japanese ? it->second.ko + " (" + it->second.ja + ")" : it->second.ko;
}
struct TeamColorCfg { bool on = false; int mode = 0; int pick[3] = {0, 0, 0}; int level = 7; bool upper = true; };
TeamColorCfg loadTeamColorCfg()
{
    TeamColorCfg c;
    for (const std::string& l : readLines(profileFile("teamcolor.txt").c_str())) {
        std::vector<std::string> f = split(l, '\t');
        if (f.size() >= 2 && f[0] == "on")
            c.on = std::atoi(f[1].c_str()) != 0;
        else if (f.size() >= 2 && f[0] == "mode")
            c.mode = std::atoi(f[1].c_str()) == 1 ? 1 : 0;
        else if (f.size() >= 2 && f[0] == "level")
            c.level = std::max(1, std::min(7, std::atoi(f[1].c_str())));
        else if (f.size() >= 2 && f[0] == "upper")
            c.upper = std::atoi(f[1].c_str()) != 0;
        else if (f.size() >= 2 && f[0] == "colors")
            for (size_t k = 0; k < 3 && k + 1 < f.size(); k++)
                c.pick[k] = std::max(0, std::min(70, std::atoi(f[k + 1].c_str())));
    }
    return c;
}
void saveTeamColorCfg(const TeamColorCfg& c)
{
    writeLines(profileFile("teamcolor.txt").c_str(),
               {std::string("on\t") + (c.on ? "1" : "0"), "mode\t" + std::to_string(c.mode),
                "colors\t" + std::to_string(c.pick[0]) + "\t" + std::to_string(c.pick[1]) + "\t" + std::to_string(c.pick[2]),
                "level\t" + std::to_string(c.level), std::string("upper\t") + (c.upper ? "1" : "0")});
}
std::string styleKo(int id)
{
    auto it = g_text.styleInfo.find(id);
    if (it == g_text.styleInfo.end())
        return id ? "스타일 " + std::to_string(id) : "(없음)";
    return it->second.ko + std::string(it->second.tier == 3 ? " ★★★" : it->second.tier == 2 ? " ★★" : " ★");
}
void teamColorPanel()
{
    loadTexts();
    loadTeamColors();
    static const char* const axis[5] = {"수비적⇔공격적", "중앙⇔사이드", "점유율⇔속공", "후퇴 수비⇔프레스", "커버⇔플랫"};
    saka_tc_status st;
    saka_teamcolor_status(&st);
    TeamColorCfg cfg = loadTeamColorCfg();
    note("게임은 팀컬러를 저장하지 않고 그때그때 계산합니다: 선발 11명의 플레이스타일·포지션으로 정해지는 라인 팀컬러 (FW·MF·DF), 작전의 "
         "전술 값 5개로 정해지는 팀컬러 (짧은 카운터·측면강화·ALL 등), 선수 방침으로 정해지는 팀컬러 중 조건이 맞는 것에서 강한 순서로 "
         "3개. 레벨은 함께 뽑힌 팀컬러끼리의 조합 보너스로 오릅니다 (최대 Lv.3). 이 치트는 선발 선수의 플레이스타일과 그 스타일의 스킬 "
         "(필요하면 키 플레이어(KEY)의 오리지널 스킬)만 바꿔서 팀컬러를 맞춥니다. 전술 값과 선수 방침은 바꾸지 않습니다. 게임 데이터에 "
         "들어 있지만 게임이 막아 둔 상위 팀컬러 18개 (철벽의 측면강화·MF, 티키타카 등)도 '상위 팀컬러 사용'을 켜면 우리 구단에 한해 "
         "쓸 수 있습니다. 끄거나, 선수가 "
         "선발에서 빠지면 원래 스타일·스킬로 돌아갑니다. 켜져 있는 동안 바뀐 선발의 그 스타일 칸은 고정됩니다 (선수 탭의 스타일 편집보다 "
         "우선).");
    bool changed = false;
    changed |= ImGui::Checkbox("팀컬러 맞춤 켜기", &cfg.on);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("자동 (지금 전술·선발로 레벨이 가장 높은 조합)", &cfg.mode, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("지정", &cfg.mode, 1);
    ImGui::SetNextItemWidth(Px(160));
    changed |= ImGui::SliderInt("바뀐 스타일의 스킬 레벨 (7 = 최고)", &cfg.level, 1, 7);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("상위 팀컬러 사용 (게임이 막아 둔 18개)", &cfg.upper);
    if (cfg.mode == 1) {
        for (int k = 0; k < 3; k++) {
            ImGui::PushID(k);
            ImGui::SetNextItemWidth(Px(420));
            if (ImGui::BeginCombo(("팀컬러 " + std::to_string(k + 1)).c_str(), teamColorLabel(cfg.pick[k]).c_str())) {
                if (ImGui::Selectable("(없음)", cfg.pick[k] == 0)) {
                    cfg.pick[k] = 0;
                    changed = true;
                }
                for (const auto& kv : g_teamColors) {
                    const TeamColorName& c = kv.second;
                    const std::string label = teamColorLabel(kv.first) + (c.kind == "line" ? "  · 라인" : c.kind == "tactics" ? "  · 전술 값" : "  · 선수 방침") +
                                              (c.unused ? (cfg.upper ? "  [상위]" : "  [상위: '상위 팀컬러 사용'이 꺼져 있어 뽑히지 않음]") : "") + "##c" +
                                              std::to_string(kv.first);
                    if (ImGui::Selectable(label.c_str(), cfg.pick[k] == kv.first)) {
                        cfg.pick[k] = kv.first;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
    }
    if (changed) {
        saveTeamColorCfg(cfg);
        toast(std::string("팀컬러 맞춤: ") + (cfg.on ? (cfg.mode ? "켬 (지정)" : "켬 (자동)") : "끔 (선수들의 원래 스타일·스킬로)"));
    }
    ImGui::Separator();
    if (!st.loaded) {
        ImGui::TextDisabled("게임 데이터가 아직 없습니다 (세이브를 불러온 뒤).");
        return;
    }
    auto three = [&](const uint32_t* ids, const uint8_t* lv) {
        std::string s;
        for (int k = 0; k < 3; k++)
            if (ids[k])
                s += (s.empty() ? "" : "  ·  ") + teamColorLabel((int)ids[k], false) + " Lv." + std::to_string(lv[k]);
        return s.empty() ? std::string("(없음)") : s;
    };
    ImGui::Text("지금 팀컬러: %s", three(st.active, st.active_level).c_str());
    if (st.on)
        ImGui::TextDisabled("선수들의 원래 스타일이면: %s", three(st.plain, st.plain_level).c_str());
    {
        std::string t = "전술 값 (작전 화면, 5단계):";
        for (int k = 0; k < 5; k++)
            t += std::string("  ") + axis[k] + " " + std::to_string(st.tac[k] + 1);
        ImGui::TextDisabled("%s", t.c_str());
    }
    if (!st.on)
        return;
    ImGui::SeparatorText("맞추는 팀컬러");
    if (st.ntarget == 0)
        note(st.mode == 0 ? "지금 전술 값과 선발로는 스타일을 바꿔서 레벨이 더 오르는 조합이 없습니다. 작전의 전술 값이나 포메이션 (포지션 "
                            "구성)을 바꾸면 달라집니다."
                          : "팀컬러를 고르세요.");
    for (int k = 0; k < st.ntarget && k < 3; k++) {
        const int c = (int)st.target[k];
        std::string why;
        switch (st.target_state[k]) {
        case 0: why = "맞춤"; break;
        case 1: why = "선발에 이 팀컬러가 요구하는 포지션의 선수가 모자람 (포메이션에 그 포지션이 없거나, 고른 다른 팀컬러가 그 선수를 씀)"; break;
        case 2: {
            why = "전술 값이 맞지 않음 (작전에서 직접 바꿔야 함):";
            for (int a = 0; a < 5 && c >= 1 && c <= 70; a++) {
                const int lo = st.color_tac[c][2 * a], hi = st.color_tac[c][2 * a + 1];
                if (st.tac[a] < lo || st.tac[a] > hi)
                    why += std::string("  ") + axis[a] + " " + std::to_string(lo + 1) + "~" + std::to_string(hi + 1) + " 필요 (지금 " + std::to_string(st.tac[a] + 1) + ")";
            }
            break;
        }
        case 3: why = "선수 방침 조건 (선발 6명 이상)이 맞지 않음. 방침 값은 이 치트가 바꾸지 않음 ('방침 중앙' 치트·선수 편집)"; break;
        case 4: why = "키 플레이어(KEY)의 오리지널 스킬을 다른 팀컬러가 이미 씀 (한 선수에 하나)"; break;
        case 5: why = "게임이 막아 둔 상위 팀컬러 ('상위 팀컬러 사용'을 켜면 쓸 수 있음)"; break;
        default: why = "조건은 맞지만 더 강한 팀컬러 3개가 먼저 뽑힘"; break;
        }
        ImGui::TextColored(st.target_state[k] == 0 ? ImVec4(0.6f, 0.9f, 0.6f, 1.0f) : ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s: %s",
                           teamColorLabel(c).c_str(), why.c_str());
    }
    ImGui::SeparatorText("바뀐 선발 선수");
    if (st.nforced == 0 && !st.leader_orig)
        ImGui::TextDisabled("바뀐 선수 없음.");
    for (int i = 0; i < st.nforced && i < 12; i++)
        ImGui::Text("%s:  %s  →  %s", nameOf(g_text.players, (int)st.forced[i].id).c_str(), styleKo((int)st.forced[i].natural).c_str(),
                    styleKo((int)st.forced[i].style).c_str());
    if (st.leader_orig) {
        auto it = g_text.origKo.find((int)st.leader_orig);
        ImGui::Text("%s (키 플레이어 KEY): 오리지널 스킬 → %s", nameOf(g_text.players, (int)st.leader_id).c_str(),
                    it != g_text.origKo.end() ? it->second.c_str() : std::to_string(st.leader_orig).c_str());
    }
}

void speedBadgeWindow(unsigned w)
{
    const int sp = g_saka_user_speed;
    ImGui::SetNextWindowPos(ImVec2(w - 16.0f, 16.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##speed", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    if (sp > 1)
        ImGui::Text(">> 게임 속도 x%d   (F7 / F8: 끄기)", sp);
    else if ((int)GetTickCount() - g_speedShownAt < 2000)
        ImGui::TextUnformatted("게임 속도 보통");
    const int super = saka_superplay_state();   // 2026-11-14: armed until used, so the badge stays while it waits
    if (super == 1)
        ImGui::TextUnformatted("슈퍼플레이 대기 (F6): 우리 팀의 다음 플레이에서 스킬 발동");
    else if (super == 2)
        ImGui::TextUnformatted("슈퍼플레이 발동!");
    else if ((int)GetTickCount() - g_superShownAt < 2500 && !g_superArmed)
        ImGui::TextUnformatted("슈퍼플레이 (F6): 치트 목록의 '슈퍼플레이 F6' 스위치를 켜야 작동");
    ImGui::End();
}

void drawMenu(unsigned w, unsigned h)
{
    ImGui::GetStyle().FontScaleMain = g_uiScale;   // the menu zoom (Ctrl+wheel, 글자 -/+)
    if (speedBadge())
        speedBadgeWindow(w);
    if (!g_open)
        return;
    static long profileGen = -1;
    static std::string profileDesc;
    {
        char desc[128] = "";
        const long gen = saka_cheat_profile(nullptr, 0, desc, sizeof desc);
        if (gen != profileGen) {   // another save's settings: read them
            profileGen = gen;
            profileDesc = desc;
            g_reload = true;
        }
    }
    if (g_reload.exchange(false))
        loadCheats();
    // The window (2026-11-10): first at the old size in the middle, then where the player moves and sizes it
    // (cheats\overlay.ini); 기본 크기 / 화면 가득 set it again, and a window left outside the screen comes back.
    static int layout = 0;   // this frame: 1 기본 크기, 2 화면 가득
    if (layout == 2) {
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2((float)w, (float)h), ImGuiCond_Always);
    } else {
        const ImGuiCond cond = layout == 1 ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        ImGui::SetNextWindowSize(ImVec2(std::min(1400.0f, w * 0.92f), std::min(900.0f, h * 0.9f)), cond);
        ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), cond, ImVec2(0.5f, 0.5f));
    }
    layout = 0;
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min((float)w, 420.0f), std::min((float)h, 260.0f)), ImVec2((float)w, (float)h));
    bool open = true;
    if (ImGui::Begin("サカつく 치트 메뉴   (F2 · Esc · 패드 L3+R3: 닫기   F7: 2배속   F8: 터보)", &open, ImGuiWindowFlags_NoCollapse)) {
        {
            const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
            const ImVec2 q(std::max(std::min(p.x, (float)w - 120.0f), 120.0f - s.x), std::max(0.0f, std::min(p.y, (float)h - 40.0f)));
            if (q.x != p.x || q.y != p.y)
                ImGui::SetWindowPos(q);
        }
        g_footerH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 1.0f;
        ImGui::TextUnformatted("게임 속도");
        for (int sp : {1, 2, 3, 4, 6}) {
            ImGui::SameLine();
            const std::string label = sp == 1 ? "보통" : sp == 6 ? "터보 (x6)" : "x" + std::to_string(sp);
            if (ImGui::RadioButton(label.c_str(), g_saka_user_speed == sp))
                setSpeed(sp);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(빠르면 소리가 빨라지거나 끊길 수 있음)");
        // Zoom and window size at the right end of this line (on the next one when it is full).
        {
            const ImGuiStyle& st = ImGui::GetStyle();
            char pct[16];
            std::snprintf(pct, sizeof pct, "%d%%", (int)std::lround(g_uiScale * 100.0f));
            const float total = btnW("글자 -") + ImGui::CalcTextSize(pct).x + btnW("글자 +") + btnW("기본 크기") + btnW("화면 가득") + st.ItemSpacing.x * 4.0f;
            ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x >= total)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - total);
            else
                ImGui::NewLine();
            const char* zoomTip = "글자와 칸 크기 (Ctrl+마우스 휠로도 조절, 60~200%)";
            if (ImGui::SmallButton("글자 -"))
                zoomBy(-0.1f);
            ImGui::SetItemTooltip("%s", zoomTip);
            ImGui::SameLine();
            ImGui::TextUnformatted(pct);
            ImGui::SameLine();
            if (ImGui::SmallButton("글자 +"))
                zoomBy(0.1f);
            ImGui::SetItemTooltip("%s", zoomTip);
            const char* sizeTip = "창은 제목줄을 끌어 옮기고 가장자리·모서리를 끌어 크기 조절. 왼쪽 목록은 오른쪽 테두리를 끌어 넓힘. 다음 실행에도 남음.";
            ImGui::SameLine();
            if (ImGui::SmallButton("기본 크기")) {
                layout = 1;
                g_resetRoster = g_resetSlots = true;
            }
            ImGui::SetItemTooltip("%s", sizeTip);
            ImGui::SameLine();
            if (ImGui::SmallButton("화면 가득"))
                layout = 2;
            ImGui::SetItemTooltip("%s", sizeTip);
        }
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "이 설정: %s  (치트·선수·목록 설정은 세이브마다 따로)", profileDesc.c_str());
        // 설치한 시설 수 (owner, 2026-11-09: "어디까지가 최대인지 체크하고 ... 내가 얼마나 설치 했는지를 알아 볼 수 있도록"):
        // the stadium's and the clubhouse's 27 slots (6 bytes, facility ID at +0; 0x00EE7B64 / 0x00EE7C06) are all the
        // game and its save hold; 27 built is the game's 施設の数が上限に達しています.
        if (committed(0x00EE7B64u, 27u * 6u * 2u)) {
            int stadium = 0, club = 0;
            for (uint32_t i = 0; i < 27; i++) {
                stadium += rd8(0x00EE7B64u + 6u * i) != 0;
                club += rd8(0x00EE7C06u + 6u * i) != 0;
            }
            ImGui::TextDisabled("설치한 시설: 구장 %d/27칸 · 클럽하우스 %d/27칸  (각 27칸이 게임의 최대. 다 차면 시설 철거로 칸을 비워야 새로 지음)",
                                stadium, club);
        }
        // SAKA_OVERLAY_TAB=0/1/2 (tests): the tab shown first.
        static int firstTab = -2;
        if (firstTab == -2) {
            const char* t = std::getenv("SAKA_OVERLAY_TAB");
            firstTab = t ? std::atoi(t) : -1;
        }
        auto tabFlags = [&](int i) { return firstTab == i ? ImGuiTabItemFlags_SetSelected : 0; };
        static int shownTab = 0;
        // 새로고침 at the right end of the tab line (2026-11-09, owner: "리스트 리프레시는 오른쪽에 위치해야하고", "팀도
        // 그렇고"): the tab shown reads the game and its files again.
        if (rightEndButton("새로고침##tabs")) {
            if (shownTab == 0) {
                g_reload = true;
                toast("치트 목록과 설정을 다시 읽음");
            } else if (shownTab == 1) {
                g_fieldInput.clear();
                g_fieldLock.clear();
                g_styleRefresh = true;
                savePlayerEdits(loadPlayerEdits());
                toast("선수 목록과 값 칸을 게임에서 다시 읽고, 한 번만 쓰는 편집을 다시 씀");
            } else {
                saveListEdits(loadListEdits());
                toast("목록을 다시 읽고, 고른 이름을 게임에 다시 넣음");
            }
        }
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("치트", nullptr, tabFlags(0))) {
                shownTab = 0;
                cheatsPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("선수", nullptr, tabFlags(1))) {
                shownTab = 1;
                playersPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("목록", nullptr, tabFlags(2))) {
                shownTab = 2;
                listsPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("연습", nullptr, tabFlags(3))) {
                shownTab = 3;
                trainingPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("스태프", nullptr, tabFlags(4))) {
                shownTab = 4;
                staffPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("팀컬러", nullptr, tabFlags(5))) {
                shownTab = 5;
                teamColorPanel();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        firstTab = -1;
        // The status line (2026-11-10; it was a 3 s toast at the screen's bottom): the last result stays, a warning in
        // orange; hover it for the earlier ones.
        ImGui::Separator();
        if (g_toast.empty())
            ImGui::TextDisabled("적용·해제 결과가 여기에 남습니다. 게임에 안 들어간 값은 그 줄 끝(빨강)과 여기(주황)에 나옵니다.");
        else {
            using namespace std::chrono;
            const auto now = steady_clock::now();
            ImGui::TextColored(g_toastLevel ? ImVec4(1.0f, 0.6f, 0.3f, 1.0f) : ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", g_toast.c_str());
            const bool hover = ImGui::IsItemHovered();
            ImGui::SameLine();
            ImGui::TextDisabled("(%lld초 전, 마우스를 올리면 지난 기록)", (long long)duration_cast<seconds>(now - g_toastAt).count());
            if ((hover || ImGui::IsItemHovered()) && ImGui::BeginTooltip()) {
                for (auto it = g_toastLog.rbegin(); it != g_toastLog.rend(); ++it)
                    ImGui::Text("%lld초 전  %s", (long long)duration_cast<seconds>(now - it->first).count(), it->second.c_str());
                ImGui::EndTooltip();
            }
        }
    }
    ImGui::End();
    if (!open)
        setOpen(false);
}

// ---- ImGui on the live renderer ----------------------------------------------------------------------------
bool g_inited = false, g_failed = false;
ID3D12DescriptorHeap* g_srvHeap = nullptr;
UINT g_srvStep = 0;
std::vector<int> g_srvFree;

void srvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    const int i = g_srvFree.back();
    g_srvFree.pop_back();
    *cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    *gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu->ptr += (SIZE_T)i * g_srvStep;
    gpu->ptr += (UINT64)i * g_srvStep;
}

void srvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    g_srvFree.push_back((int)((cpu.ptr - g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / g_srvStep));
}

bool init(ID3D12Device* dev, ID3D12CommandQueue* queue, unsigned h)
{
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 64;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap))))
        return false;
    g_srvStep = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int i = 63; i >= 0; i--)
        g_srvFree.push_back(i);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // The menu's place, size, pane widths and zoom (2026-11-10, owner: "자유롭게 안팎으로 조정"), next to the cheat
    // files; the zoom in its own [SakaOverlay][Main] section.
    io.IniFilename = "cheats\\overlay.ini";
    io.LogFilename = nullptr;
    {
        ImGuiSettingsHandler hd;
        hd.TypeName = "SakaOverlay";
        hd.TypeHash = ImHashStr("SakaOverlay");
        hd.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char*) -> void* { return (void*)1; };
        hd.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line) {
            float f = 0.0f;
            if (std::sscanf(line, "Zoom=%f", &f) == 1 && f >= 0.6f && f <= 2.0f)
                g_uiScale = f;
        };
        hd.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* out) {
            out->appendf("[%s][Main]\nZoom=%.2f\n\n", handler->TypeName, g_uiScale);
        };
        ImGui::AddSettingsHandler(&hd);
    }
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.94f;

    // Korean UI text and Japanese names: Malgun Gothic with a Japanese font merged in (glyphs are rasterized on
    // demand by ImGui 1.92, so no glyph ranges).
    const float px = std::max(18.0f, std::min(30.0f, h / 38.0f));
    char win[MAX_PATH];
    GetWindowsDirectoryA(win, MAX_PATH);
    const std::string fonts = std::string(win) + "\\Fonts\\";
    ImFont* font = nullptr;
    for (const char* f : {"malgun.ttf", "gulim.ttc"})
        if (!font && GetFileAttributesA((fonts + f).c_str()) != INVALID_FILE_ATTRIBUTES)
            font = io.Fonts->AddFontFromFileTTF((fonts + f).c_str(), px);
    if (!font)
        io.Fonts->AddFontDefault();
    for (const char* f : {"YuGothM.ttc", "meiryo.ttc", "msgothic.ttc"})
        if (GetFileAttributesA((fonts + f).c_str()) != INVALID_FILE_ATTRIBUTES) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            io.Fonts->AddFontFromFileTTF((fonts + f).c_str(), px, &cfg);
            break;
        }

    ImGui_ImplDX12_InitInfo info;
    info.Device = dev;
    info.CommandQueue = queue;
    info.NumFramesInFlight = 4;
    info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = g_srvHeap;
    info.SrvDescriptorAllocFn = srvAlloc;
    info.SrvDescriptorFreeFn = srvFree;
    if (!ImGui_ImplDX12_Init(&info))
        return false;
    std::fprintf(stderr, "[overlay] ImGui %s on the live renderer, font %.0f px\n", IMGUI_VERSION, px);
    return true;
}

ImGuiKey vkToKey(uintptr_t vk)
{
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_CONTROL: return ImGuiKey_LeftCtrl;
    case VK_SHIFT: return ImGuiKey_LeftShift;
    case 'A': return ImGuiKey_A;
    case 'C': return ImGuiKey_C;
    case 'V': return ImGuiKey_V;
    case 'X': return ImGuiKey_X;
    case 'Z': return ImGuiKey_Z;
    default: return ImGuiKey_None;
    }
}

void feedInput(unsigned w, unsigned h)
{
    ImGuiIO& io = ImGui::GetIO();
    std::vector<Msg> msgs;
    {
        std::lock_guard<std::mutex> lock(g_msgMutex);
        msgs.swap(g_msgs);
    }
    const float sx = g_clientW > 0 ? (float)w / g_clientW : 1.0f, sy = g_clientH > 0 ? (float)h / g_clientH : 1.0f;
    for (const Msg& m : msgs) {
        switch (m.msg) {
        case WM_MOUSEMOVE:
            io.AddMousePosEvent((float)(short)LOWORD(m.lp) * sx, (float)(short)HIWORD(m.lp) * sy);
            break;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: io.AddMouseButtonEvent(0, true); break;
        case WM_LBUTTONUP: io.AddMouseButtonEvent(0, false); break;
        case WM_RBUTTONDOWN: io.AddMouseButtonEvent(1, true); break;
        case WM_RBUTTONUP: io.AddMouseButtonEvent(1, false); break;
        case WM_MBUTTONDOWN: io.AddMouseButtonEvent(2, true); break;
        case WM_MBUTTONUP: io.AddMouseButtonEvent(2, false); break;
        case WM_MOUSEWHEEL:
            if (g_open && (LOWORD(m.wp) & MK_CONTROL)) {   // Ctrl+wheel: the menu zoom (2026-11-10)
                zoomBy((short)HIWORD(m.wp) > 0 ? 0.1f : -0.1f);
                break;
            }
            io.AddMouseWheelEvent(0.0f, (float)(short)HIWORD(m.wp) / WHEEL_DELTA);
            break;
        case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
            const bool down = m.msg == WM_KEYDOWN || m.msg == WM_SYSKEYDOWN;
            const ImGuiKey key = vkToKey(m.wp);
            if (key != ImGuiKey_None)
                io.AddKeyEvent(key, down);
            if (m.wp == VK_CONTROL)
                io.AddKeyEvent(ImGuiMod_Ctrl, down);
            if (m.wp == VK_SHIFT)
                io.AddKeyEvent(ImGuiMod_Shift, down);
            break;
        }
        case WM_CHAR:
            if (m.wp >= 0x20 && m.wp < 0x80)
                io.AddInputCharacter((unsigned)m.wp);
            break;
        case WM_IME_CHAR: {   // the window is ANSI: a double-byte character of the system code page
            char mb[2];
            int n = 0;
            if (m.wp > 0xFF)
                mb[n++] = (char)(m.wp >> 8);
            mb[n++] = (char)(m.wp & 0xFF);
            wchar_t wc[2];
            const int k = MultiByteToWideChar(CP_ACP, 0, mb, n, wc, 2);
            for (int i = 0; i < k; i++)
                io.AddInputCharacterUTF16(wc[i]);
            break;
        }
        }
    }
    // The pad (CELL_PAD_CTRL_*: UP 0x10, RIGHT 0x20, DOWN 0x40, LEFT 0x80, L1 0x400, R1 0x800, TRIANGLE 0x1000,
    // CIRCLE 0x2000, CROSS 0x4000, SQUARE 0x8000): ○ chooses, × goes back, as in the game.
    const unsigned p = g_pad;
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (p & 0x10u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (p & 0x20u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (p & 0x40u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (p & 0x80u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (p & 0x2000u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (p & 0x4000u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, (p & 0x8000u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, (p & 0x1000u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadL1, (p & 0x400u) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadR1, (p & 0x800u) != 0);
}

bool speedBadge()
{
    return g_saka_user_speed > 1 || (int)GetTickCount() - g_speedShownAt < 2000 || (int)GetTickCount() - g_superShownAt < 2500 ||
           saka_superplay_state() != 0;
}
int overlayOn() { return g_open || speedBadge() ? 1 : 0; }

// SAKA_OVERLAY_SHOT=<file.bmp> (tests): after the menu has been drawn for a while, one back buffer (game + menu)
// is copied to a readback buffer and written as a BMP once the GPU is past it (an own fence, signalled on the queue
// at the next frame, completes after the frame that holds the copy).
struct Shot {
    const char* path = nullptr;
    int frames = 0, stage = 0;   // stage 0 wait, 1 copy recorded, 2 fence signalled, 3 done
    ID3D12Resource* readback = nullptr;
    ID3D12Fence* fence = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    unsigned w = 0, h = 0;
} g_shot;

void shotStep(ID3D12Device* dev, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cl, ID3D12Resource* bb, unsigned w, unsigned h)
{
    if (!g_shot.path) {
        static bool read = false;
        if (read)
            return;
        read = true;
        g_shot.path = std::getenv("SAKA_OVERLAY_SHOT");
        if (!g_shot.path)
            return;
    }
    if (g_shot.stage == 0 && ++g_shot.frames >= 90) {
        D3D12_RESOURCE_DESC desc = bb->GetDesc();
        UINT64 total = 0;
        dev->GetCopyableFootprints(&desc, 0, 1, 0, &g_shot.fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = total;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_shot.readback))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_shot.fence)))) {
            g_shot.stage = 3;
            return;
        }
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = g_shot.readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = g_shot.fp;
        src.pResource = bb;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cl->ResourceBarrier(1, &b);
        g_shot.w = w;
        g_shot.h = h;
        g_shot.stage = 1;
    } else if (g_shot.stage == 1) {
        queue->Signal(g_shot.fence, 1);   // after the previous frame's list, which holds the copy
        g_shot.stage = 2;
    } else if (g_shot.stage == 2 && g_shot.fence->GetCompletedValue() >= 1) {
        uint8_t* p = nullptr;
        D3D12_RANGE r = {0, (SIZE_T)g_shot.fp.Footprint.RowPitch * g_shot.h};
        if (SUCCEEDED(g_shot.readback->Map(0, &r, (void**)&p))) {
            std::ofstream f(g_shot.path, std::ios::binary);
            const uint32_t row = g_shot.w * 4, size = 54 + row * g_shot.h;
            uint8_t hdr[54] = {'B', 'M'};
            std::memcpy(hdr + 2, &size, 4);
            const uint32_t off = 54, dib = 40, planes_bpp = 1 | (32 << 16);
            std::memcpy(hdr + 10, &off, 4);
            std::memcpy(hdr + 14, &dib, 4);
            const int32_t bw = (int32_t)g_shot.w, bh = -(int32_t)g_shot.h;   // top-down
            std::memcpy(hdr + 18, &bw, 4);
            std::memcpy(hdr + 22, &bh, 4);
            std::memcpy(hdr + 26, &planes_bpp, 4);
            f.write((const char*)hdr, 54);
            std::vector<uint8_t> line(row);
            for (unsigned y = 0; y < g_shot.h; y++) {
                const uint8_t* s = p + (size_t)y * g_shot.fp.Footprint.RowPitch;
                for (unsigned x = 0; x < g_shot.w; x++) {   // RGBA -> BGRA
                    line[x * 4 + 0] = s[x * 4 + 2];
                    line[x * 4 + 1] = s[x * 4 + 1];
                    line[x * 4 + 2] = s[x * 4 + 0];
                    line[x * 4 + 3] = 255;
                }
                f.write((const char*)line.data(), row);
            }
            g_shot.readback->Unmap(0, nullptr);
            std::fprintf(stderr, "[overlay] shot %s (%ux%u)\n", g_shot.path, g_shot.w, g_shot.h);
        }
        g_shot.stage = 3;
    }
}

void overlayDraw(void* dev, void* queue, void* list, void* backbuffer, size_t rtv, unsigned w, unsigned h)
{
    if (g_failed || !w || !h)
        return;
    if (!g_inited) {
        g_inited = true;
        if (!init((ID3D12Device*)dev, (ID3D12CommandQueue*)queue, h)) {
            g_failed = true;
            std::fprintf(stderr, "[overlay] ImGui init failed: menu off\n");
            return;
        }
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    io.DeltaTime = std::max(1.0f / 1000.0f, std::chrono::duration<float>(now - last).count());
    last = now;
    feedInput(w, h);
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    drawMenu(w, h);
    ImGui::Render();
    auto* cl = (ID3D12GraphicsCommandList*)list;
    D3D12_CPU_DESCRIPTOR_HANDLE target;
    target.ptr = rtv;
    cl->OMSetRenderTargets(1, &target, FALSE, nullptr);
    cl->SetDescriptorHeaps(1, &g_srvHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
    shotStep((ID3D12Device*)dev, (ID3D12CommandQueue*)queue, cl, (ID3D12Resource*)backbuffer, w, h);
}

} // namespace

extern "C" void saka_overlay_install(void)
{
    const char* off = std::getenv("SAKA_OVERLAY");
    if (off && *off == '0')
        return;
    rsx_window_message_hook = windowHook;
    rsx_live_draw_overlay_on = overlayOn;
    rsx_live_draw_overlay_draw = overlayDraw;
    cellPad_overlay_hook = padHook;
    const char* open = std::getenv("SAKA_OVERLAY_OPEN");
    if (open && *open == '1')
        setOpen(true);
}

#else
extern "C" void saka_overlay_install(void) {}
#endif
