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
#include "backends/imgui_impl_dx12.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
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

int windowHook(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp)
{
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
bool validPlayer(uint32_t id) { return (id > 0 && id < 0x46A2u) || (id >= 0x7FFDu && id <= 0x7FFFu); }

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

struct Texts {
    bool loaded = false;
    Names players, nations, staff, scouts;
    std::vector<std::string> abilities;
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
    std::vector<uint8_t> a = readFile(dataPath("text3\\pledit_ability_name.txb"));
    Names ab = textTable(a, 0);
    for (int i = 0; i < 46; i++)
        g_text.abilities.push_back(ab.count(i) ? ab[i] : "능력 " + std::to_string(i));
}

std::string nameOf(const Names& names, int id)
{
    auto it = names.find(id);
    return it != names.end() && !it->second.empty() ? it->second : "#" + std::to_string(id);
}

// Nationality from the game's master tables in memory (NOTES.md "Cheats v2"; launcher GameData).
int playerNation(int id)
{
    const uint32_t ea = id < 10000 ? 0x00C7D6CCu + (uint32_t)id * 0x3Cu : 0x00D9A080u + (uint32_t)(id - 10000) * 0x3Cu;
    return committed(ea + 2, 1) ? (int)rd8(ea + 2) : -1;
}
int staffNation(int id) { return committed(0x00BBA60Cu + (uint32_t)id * 100u + 4, 4) ? (int)rd32(0x00BBA60Cu + (uint32_t)id * 100u + 4) : -1; }
int scoutNation(int id) { return committed(0x00E1F6ACu + (uint32_t)id * 60u + 4, 4) ? (int)rd32(0x00E1F6ACu + (uint32_t)id * 60u + 4) : -1; }

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
    bool onceDefault = false, isSwitch = false;
    std::vector<CheatValue> values;
    std::vector<std::string> codeLines;   // in the game's numbering (multi-write second lines left out)
};

struct CheatFiles {
    std::vector<Cheat> cheats;
    std::set<std::string> enabled;
    std::map<std::string, std::string> values;   // "name\tkey" -> value
} g_files;

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
    for (const std::string& l : readLines("cheats\\enabled.txt"))
        if (!l.empty())
            g_files.enabled.insert(l);
    for (const std::string& l : readLines("cheats\\values.txt")) {
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
    writeLines("cheats\\enabled.txt", lines);
}

void saveValues()
{
    std::vector<std::string> lines;
    for (const auto& kv : g_files.values)
        lines.push_back(kv.first + "\t" + kv.second);
    writeLines("cheats\\values.txt", lines);
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
    for (const std::string& l : readLines("cheats\\players.txt")) {
        std::vector<std::string> f = split(l, '\t');
        if (f.size() >= 3)
            out.push_back({(uint32_t)std::strtoul(f[0].c_str(), nullptr, 10), f[1], std::atoll(f[2].c_str()), f.size() < 4 || f[3] != "0"});
    }
    return out;
}

void savePlayerEdits(const std::vector<PlayerEdit>& edits)
{
    std::vector<std::string> lines;
    for (const PlayerEdit& e : edits)
        lines.push_back(std::to_string(e.id) + "\t" + e.field + "\t" + std::to_string(e.value) + "\t" + (e.lock ? "1" : "0"));
    writeLines("cheats\\players.txt", lines);
}

std::vector<ListEdit> loadListEdits()
{
    std::vector<ListEdit> out;
    for (const std::string& l : readLines("cheats\\lists.txt")) {
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
    writeLines("cheats\\lists.txt", lines);
}

// ---- the panels -------------------------------------------------------------------------------------------
std::string g_toast;
std::chrono::steady_clock::time_point g_toastAt;
void toast(const std::string& s)
{
    g_toast = s;
    g_toastAt = std::chrono::steady_clock::now();
}

char g_cheatFilter[128];
std::map<std::string, long long> g_inputs;   // value boxes: "cheat\tn" -> typed value

void cheatsPanel()
{
    ImGui::SetNextItemWidth(320);
    ImGui::InputTextWithHint("##cheatfilter", "찾기 (이름·설명)", g_cheatFilter, sizeof g_cheatFilter);
    ImGui::SameLine();
    ImGui::TextDisabled("켜면 1초 안에 적용. 고정 = 1초에 10번 다시 씀, 끄면 켤 때 한 번만.");
    ImGui::Separator();
    ImGui::BeginChild("cheats");
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
        if (!c.isSwitch) {
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
                ImGui::SetNextItemWidth(320);
                if (ImGui::BeginCombo(v.label.c_str(), (std::to_string(typed) + "  " + nameOf(names, (int)typed)).c_str())) {
                    for (const auto& kv : names)
                        if (kv.first >= v.min && kv.first <= v.max)
                            if (ImGui::Selectable((std::to_string(kv.first) + "  " + kv.second).c_str(), kv.first == typed))
                                typed = kv.first;
                    ImGui::EndCombo();
                }
            } else {
                int value32 = (int)std::min<long long>(std::max<long long>(typed, INT32_MIN), INT32_MAX);
                ImGui::SetNextItemWidth(200);
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
    ImGui::BeginChild("roster", ImVec2(330, 0), ImGuiChildFlags_Borders);
    for (int s = 0; s < 23; s++) {
        const uint32_t id = rd32(roster + record * s);
        if (!validPlayer(id))
            continue;
        if (g_slot < 0)
            g_slot = s;
        int n = 0;
        for (const PlayerEdit& e : edits)
            n += e.id == id ? 1 : 0;
        char line[160];
        std::snprintf(line, sizeof line, "%02d  %s  ·  %u세%s##slot%d", s + 1, nameOf(g_text.players, (int)id).c_str(),
                      rd8(roster + record * s + 5), n ? ("  ·  수정 " + std::to_string(n)).c_str() : "", s);
        if (ImGui::Selectable(line, g_slot == s))
            g_slot = s;
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("player");
    if (g_slot >= 0) {
        const uint32_t rec = roster + record * g_slot, id = rd32(rec);
        if (validPlayer(id)) {
            ImGui::Text("%s  (선수 ID %u, %d번 칸)", nameOf(g_text.players, (int)id).c_str(), id, g_slot + 1);
            ImGui::TextDisabled("값을 넣고 적용. 고정이면 1초에 10번 다시 씀, 아니면 한 번. 이 선수가 지금 구단에 있을 때만.");
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
                ImGui::SameLine(200);
                ImGui::TextDisabled("%s", now.c_str());
                ImGui::SameLine(360);
                ImGui::SetNextItemWidth(120);
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
                }
                ImGui::PopID();
            };
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
                    ImGui::SameLine();
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
            ImGui::SameLine();
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
    if (ImGui::BeginTabBar("lists")) {
        for (int L = 0; L < 3; L++) {
            const ListDef& d = kLists[L];
            if (!ImGui::BeginTabItem(d.title, nullptr, testList == L ? ImGuiTabItemFlags_SetSelected : 0))
                continue;
            const Names& names = L == 0 ? g_text.staff : L == 1 ? g_text.scouts : g_text.players;
            buildPeople(L, names, d.maxId);
            auto nation = [&](int id) { auto it = g_nationOf[L].find(id); return it != g_nationOf[L].end() ? it->second : -1; };
            // The slots: current name and country; the chosen slot gets the pick.
            ImGui::BeginChild("slots", ImVec2(460, 0), ImGuiChildFlags_Borders);
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
            ImGui::Checkbox("고정 (게임이 목록을 다시 만들어도 유지)", &g_listLock[L]);
            if (ImGui::Button("이 칸 해제")) {
                edits.erase(std::remove_if(edits.begin(), edits.end(), [&](const ListEdit& e) { return e.list == d.list && e.slot == g_listSlot[L]; }), edits.end());
                saveListEdits(edits);
                toast(std::string(d.title) + " " + std::to_string(g_listSlot[L] + 1) + "번: 해제");
            }
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("pick");
            // Country filter: the countries of this list's people, the most common first.
            std::map<int, int> count;
            for (const auto& pn : g_people[L])
                count[pn.second]++;
            std::vector<std::pair<int, int>> countries(count.begin(), count.end());
            std::sort(countries.begin(), countries.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) { return a.second > b.second; });
            const std::string current = g_listNation[L] < 0 ? "전체 국가" : nameOf(g_text.nations, g_listNation[L]);
            ImGui::SetNextItemWidth(260);
            if (ImGui::BeginCombo("국가", current.c_str())) {
                if (ImGui::Selectable("전체 국가", g_listNation[L] < 0))
                    g_listNation[L] = -1;
                for (const auto& c : countries)
                    if (ImGui::Selectable((nameOf(g_text.nations, c.first) + " (" + std::to_string(c.second) + "명)").c_str(), g_listNation[L] == c.first))
                        g_listNation[L] = c.first;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(260);
            ImGui::InputTextWithHint("##search", "이름, 영어 이름 또는 ID", g_listSearch[L], sizeof g_listSearch[L]);
            const std::string search = g_listSearch[L];
            ImGui::TextDisabled("고른 이름이 왼쪽에서 고른 %d번 칸에 들어갑니다.", g_listSlot[L] + 1);
            if (ImGui::BeginListBox("##names", ImVec2(-1, -1))) {
                // The people shown, worked out again only when the search or the country changes: an English search
                // (romaji) scores every name, the closest first; Japanese text or an ID matches as before.
                const bool english = namesearch::isEnglish(search);
                static std::string hitsFor[3];
                static std::vector<std::pair<int, int>> hits[3];   // (score, index into g_people[L])
                const std::string hitsKey = search + "\x1f" + std::to_string(g_listNation[L]);
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
                    if (english)   // the closest first, then the shorter name
                        std::stable_sort(hits[L].begin(), hits[L].end(), [&](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                            if (a.first != b.first)
                                return a.first < b.first;
                            auto base = [&](int person) {   // without the legend tag
                                const std::string& s = names.at(person);
                                const size_t tag = s.find(" (레전드");
                                return tag == std::string::npos ? s.size() : tag;
                            };
                            return base(g_people[L][a.second].first) < base(g_people[L][b.second].first);
                        });
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
                    const size_t tag = kv.second.find(" (레전드");   // the brackets: the Japanese name's romaji only
                    const std::string item = kv.second + (english ? "  [" + namesearch::romaji(kv.second.substr(0, tag)) + "]" : std::string()) +
                                             "  (" + std::to_string(kv.first) + ")  · " + nameOf(g_text.nations, n);
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

void speedBadgeWindow(unsigned w)
{
    const int sp = g_saka_user_speed;
    ImGui::SetNextWindowPos(ImVec2(w - 16.0f, 16.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##speed", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    if (sp > 1)
        ImGui::Text(">> 게임 속도 x%d   (F7 / F8: 끄기)", sp);
    else
        ImGui::TextUnformatted("게임 속도 보통");
    ImGui::End();
}

void drawMenu(unsigned w, unsigned h)
{
    if (speedBadge())
        speedBadgeWindow(w);
    if (!g_open)
        return;
    if (g_reload.exchange(false))
        loadCheats();
    const ImVec2 size(std::min(1400.0f, w * 0.92f), std::min(900.0f, h * 0.9f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::Begin("サカつく 치트 메뉴   (F2 · Esc · 패드 L3+R3: 닫기   F7: 2배속   F8: 터보)", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("게임 속도");
        for (int sp : {1, 2, 3, 4, 6}) {
            ImGui::SameLine();
            const std::string label = sp == 1 ? "보통" : sp == 6 ? "터보 (x6)" : "x" + std::to_string(sp);
            if (ImGui::RadioButton(label.c_str(), g_saka_user_speed == sp))
                setSpeed(sp);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(빠르면 소리가 빨라지거나 끊길 수 있음)");
        // SAKA_OVERLAY_TAB=0/1/2 (tests): the tab shown first.
        static int firstTab = -2;
        if (firstTab == -2) {
            const char* t = std::getenv("SAKA_OVERLAY_TAB");
            firstTab = t ? std::atoi(t) : -1;
        }
        auto tabFlags = [&](int i) { return firstTab == i ? ImGuiTabItemFlags_SetSelected : 0; };
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("치트", nullptr, tabFlags(0))) {
                cheatsPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("선수", nullptr, tabFlags(1))) {
                playersPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("목록", nullptr, tabFlags(2))) {
                listsPanel();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        firstTab = -1;
    }
    ImGui::End();
    if (!open)
        setOpen(false);
    if (!g_toast.empty() && std::chrono::steady_clock::now() - g_toastAt < std::chrono::seconds(3)) {
        ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h - 40.0f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::Begin("##toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextUnformatted(g_toast.c_str());
        ImGui::End();
    }
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
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
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
        case WM_MOUSEWHEEL: io.AddMouseWheelEvent(0.0f, (float)(short)HIWORD(m.wp) / WHEEL_DELTA); break;
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

bool speedBadge() { return g_saka_user_speed > 1 || (int)GetTickCount() - g_speedShownAt < 2000; }
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
