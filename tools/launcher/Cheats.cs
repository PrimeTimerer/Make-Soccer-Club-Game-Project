// Cheats page (frame of the YKTK3 launcher's: categories, values, 고정): the cheats of cheats\BLJM61064.txt (VitaCheat
// syntax, PS3 addresses; the port's cheat thread applies them ten times a second) switched through cheats\enabled.txt,
// their values and 고정 through cheats\values.txt. The game reads the files again within a second.
// Also here: the game's own name tables (GameText) and the game state the port exports every 2 s (LiveState), used by
// the player and list pages (Players.cs).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Controls;

namespace SakatsukuLauncher
{
    // The game's text archives (data\text*\*.arch, *.txb: little-endian, UTF-16LE strings), from the 1.03 patch
    // (dev_hdd0) or the disc. txt_static_data.arch tables: 0 player names by player ID, 3/4 club short/full names,
    // 9 staff (coach) names, 10 scout names (tools/saka_text.py).
    public static class GameText
    {
        static readonly Dictionary<string, List<Dictionary<int, string>>> cache = new Dictionary<string, List<Dictionary<int, string>>>();

        static string Find(Launcher launcher, string relative)
        {
            string patch = Path.Combine(launcher.Hdd0, "game", GameFiles.TitleId, "USRDIR", "data", relative);
            if (File.Exists(patch))
                return patch;
            return Path.Combine(launcher.Bdvd, "PS3_GAME", "USRDIR", "data", relative);
        }

        static Dictionary<int, string> Table(byte[] d, int t)
        {
            var entries = new Dictionary<int, string>();
            int count = BitConverter.ToUInt16(d, t + 2);
            int strings = t + 4 + 8 * count;
            for (int e = 0; e < count; e++)
            {
                int id = BitConverter.ToInt32(d, t + 4 + 8 * e);
                int p = strings + BitConverter.ToInt32(d, t + 8 + 8 * e);
                if (p + 2 > d.Length)
                    continue;
                int length = BitConverter.ToUInt16(d, p);
                if (p + 2 + length > d.Length)
                    continue;
                entries[id] = Encoding.Unicode.GetString(d, p + 2, length).TrimEnd('\0');
            }
            return entries;
        }

        // All tables of an .arch file, or the one table of a .txb file.
        public static List<Dictionary<int, string>> Load(Launcher launcher, string relative)
        {
            List<Dictionary<int, string>> tables;
            if (cache.TryGetValue(relative, out tables))
                return tables;
            tables = new List<Dictionary<int, string>>();
            try
            {
                byte[] d = File.ReadAllBytes(Find(launcher, relative));
                if (relative.EndsWith(".txb", StringComparison.OrdinalIgnoreCase))
                    tables.Add(Table(d, 0));
                else
                {
                    int n = BitConverter.ToInt32(d, 0);
                    int start = 0x10 + 4 * n;
                    for (int k = 0; k < n; k++)
                        tables.Add(Table(d, start + BitConverter.ToInt32(d, 0x10 + 4 * k)));
                }
            }
            catch (Exception)
            {
            }
            cache[relative] = tables;
            return tables;
        }

        static Dictionary<int, string> Static(Launcher launcher, int table)
        {
            List<Dictionary<int, string>> tables = Load(launcher, @"text\txt_static_data.arch");
            return table < tables.Count ? tables[table] : new Dictionary<int, string>();
        }

        public static Dictionary<int, string> Nations(Launcher launcher) { return Static(launcher, 2); }
        // Player names, with a tag after the fictional stand-ins for real legends (owner, 2026-11-08: "가상선수들중
        // 레전드 선수들은 괄호 해놓고 구별"): cheats\legends.txt, "player ID<TAB>label" per line, # comments.
        // "名前 (레전드: label)"; the label's English name is searchable too (NameSearch reads Latin letters).
        static Dictionary<int, string> players;

        public static Dictionary<int, string> Players(Launcher launcher)
        {
            if (players == null)
            {
                players = new Dictionary<int, string>(Static(launcher, 0));
                foreach (KeyValuePair<int, string> legend in Legends(launcher))
                {
                    string name;
                    if (players.TryGetValue(legend.Key, out name))
                        players[legend.Key] = name + " (레전드: " + legend.Value + ")";
                }
            }
            return players;
        }

        public static Dictionary<int, string> Legends(Launcher launcher)
        {
            var legends = new Dictionary<int, string>();
            try
            {
                string path = Path.Combine(Cheats.Dir(launcher), "legends.txt");
                if (File.Exists(path))
                    foreach (string line in File.ReadAllLines(path, Encoding.UTF8))
                    {
                        if (line.StartsWith("#"))
                            continue;
                        string[] f = line.Split('\t');
                        int id;
                        if (f.Length >= 2 && int.TryParse(f[0].Trim(), out id) && f[1].Trim().Length > 0)
                            legends[id] = f[1].Trim();
                    }
            }
            catch (Exception)
            {
            }
            return legends;
        }
        public static Dictionary<int, string> Clubs(Launcher launcher) { return Static(launcher, 4); }
        public static Dictionary<int, string> Staff(Launcher launcher) { return Static(launcher, 9); }
        public static Dictionary<int, string> Scouts(Launcher launcher) { return Static(launcher, 10); }

        // The 46 abilities of a player record (+0x0C), in record order.
        public static string[] Abilities(Launcher launcher)
        {
            List<Dictionary<int, string>> t = Load(launcher, @"text3\pledit_ability_name.txb");
            var names = new string[46];
            for (int i = 0; i < 46; i++)
            {
                string name;
                names[i] = t.Count > 0 && t[0].TryGetValue(i, out name) ? name : "능력 " + i;
            }
            return names;
        }

        public static string Name(Dictionary<int, string> names, long id)
        {
            string name;
            return names.TryGetValue((int)id, out name) && name.Length > 0 ? name : "#" + id;
        }
    }

    // The game's master tables in game\EBOOT.ELF (static data of the 1.03 EBOOT, big-endian; tables of
    // func_00574270, NOTES.md "Cheats v2"): the nationality (index into the text table of countries) of every player
    // (DB record +2: IDs below 10000 in table 1 at 0x00C7D6CC, the rest in table 2 at 0x00D9A080, 0x3C bytes each,
    // as func_004CA1E4), staff member (u32 +4 of the 100-byte records at 0x00BBA60C) and scout (u32 +4 of the 60-byte
    // records at 0x00E1F6AC).
    public static class GameData
    {
        static byte[] elf;
        static readonly List<ulong[]> segments = new List<ulong[]>(); // vaddr, offset, filesz
        static Dictionary<int, int> players, staff, scouts;

        static bool Load(Launcher launcher)
        {
            if (elf != null)
                return elf.Length > 0;
            elf = new byte[0];
            try
            {
                byte[] d = File.ReadAllBytes(launcher.Eboot);
                ulong phoff = BigU64(d, 0x20);
                int phent = (d[0x36] << 8) | d[0x37], phnum = (d[0x38] << 8) | d[0x39];
                for (int i = 0; i < phnum; i++)
                {
                    int p = (int)phoff + i * phent;
                    if (BigU32(d, p) == 1 && BigU64(d, p + 0x20) > 0)
                        segments.Add(new[] { BigU64(d, p + 0x10), BigU64(d, p + 8), BigU64(d, p + 0x20) });
                }
                elf = d;
            }
            catch (Exception)
            {
            }
            return elf.Length > 0;
        }

        static uint BigU32(byte[] d, int p)
        {
            return (uint)(d[p] << 24 | d[p + 1] << 16 | d[p + 2] << 8 | d[p + 3]);
        }

        static ulong BigU64(byte[] d, int p)
        {
            return (ulong)BigU32(d, p) << 32 | BigU32(d, p + 4);
        }

        static int Offset(uint va)
        {
            foreach (ulong[] s in segments)
                if (va >= s[0] && va < s[0] + s[2])
                    return (int)(s[1] + (va - s[0]));
            return -1;
        }

        static Dictionary<int, int> Table(uint va, int count, int size, int field, bool word, int firstId)
        {
            var map = new Dictionary<int, int>();
            for (int i = 0; i < count; i++)
            {
                int p = Offset(va + (uint)(i * size));
                if (p < 0 || p + size > elf.Length)
                    break;
                map[firstId + i] = word ? (int)BigU32(elf, p + field) : elf[p + field];
            }
            return map;
        }

        public static Dictionary<int, int> PlayerNations(Launcher launcher)
        {
            if (players == null)
            {
                players = new Dictionary<int, int>();
                if (Load(launcher))
                {
                    players = Table(0x00C7D6CC, 10000, 0x3C, 2, false, 0);
                    foreach (var kv in Table(0x00D9A080, 8082, 0x3C, 2, false, 10000))
                        players[kv.Key] = kv.Value;
                }
            }
            return players;
        }

        public static Dictionary<int, int> StaffNations(Launcher launcher)
        {
            if (staff == null)
                staff = Load(launcher) ? Table(0x00BBA60C, 826, 100, 4, true, 0) : new Dictionary<int, int>();
            return staff;
        }

        public static Dictionary<int, int> ScoutNations(Launcher launcher)
        {
            if (scouts == null)
                scouts = Load(launcher) ? Table(0x00E1F6AC, 428, 60, 4, true, 0) : new Dictionary<int, int>();
            return scouts;
        }
    }

    // cheats\live\state.bin: "SAKA", u32 count, then per range u32 address, u32 size and the bytes (big-endian, as
    // the game keeps them). The port writes it every 2 s while a game is loaded.
    public class LiveState
    {
        readonly List<KeyValuePair<uint, byte[]>> ranges = new List<KeyValuePair<uint, byte[]>>();
        public DateTime Time;

        public static string FilePath(Launcher launcher)
        {
            return Path.Combine(Cheats.Dir(launcher), "live", "state.bin");
        }

        public static LiveState Load(Launcher launcher)
        {
            string path = FilePath(launcher);
            if (!File.Exists(path))
                return null;
            try
            {
                byte[] d = File.ReadAllBytes(path);
                if (d.Length < 8 || Encoding.ASCII.GetString(d, 0, 4) != "SAKA")
                    return null;
                var state = new LiveState { Time = File.GetLastWriteTime(path) };
                int n = BitConverter.ToInt32(d, 4), at = 8;
                for (int r = 0; r < n && at + 8 <= d.Length; r++)
                {
                    uint addr = BitConverter.ToUInt32(d, at);
                    int size = BitConverter.ToInt32(d, at + 4);
                    if (at + 8 + size > d.Length)
                        break;
                    var bytes = new byte[size];
                    Buffer.BlockCopy(d, at + 8, bytes, 0, size);
                    state.ranges.Add(new KeyValuePair<uint, byte[]>(addr, bytes));
                    at += 8 + size;
                }
                return state;
            }
            catch (Exception)
            {
                return null;
            }
        }

        public bool Has(uint addr, int size)
        {
            return ranges.Any(r => addr >= r.Key && addr + size <= r.Key + r.Value.Length);
        }

        public uint Read(uint addr, int size)
        {
            foreach (var r in ranges)
                if (addr >= r.Key && addr + size <= r.Key + r.Value.Length)
                {
                    uint v = 0;
                    for (int k = 0; k < size; k++)
                        v = (v << 8) | r.Value[addr - r.Key + k];
                    return v;
                }
            return 0;
        }

        // A save slot (MAIN0x.DAT, slot x + 1) is the game state from 0x00EE27A0 on, byte for byte (2026-11-08: the
        // four ranges the port exports matched the save they were loaded from), so the same reads work on it.
        public const uint SaveBase = 0x00EE27A0;
        public string Source = "";   // the save file read, or "" for the running game's export

        public static LiveState FromSave(string path)
        {
            try
            {
                byte[] d = File.ReadAllBytes(path);
                var state = new LiveState { Time = File.GetLastWriteTime(path), Source = path };
                state.ranges.Add(new KeyValuePair<uint, byte[]>(SaveBase, d));
                return state;
            }
            catch (Exception)
            {
                return null;
            }
        }

        // A file big enough to hold the roster with at least one player in it.
        public static bool LooksLikeSave(string path)
        {
            try
            {
                if (new FileInfo(path).Length < Roster - SaveBase + 23 * Record)
                    return false;
                LiveState s = FromSave(path);
                return s != null && Enumerable.Range(0, 23).Any(k => ValidPlayer(s.PlayerId(k)));
            }
            catch (Exception)
            {
                return false;
            }
        }

        public const uint Roster = 0x00EE2954, Record = 0x218;

        public uint PlayerId(int slot)
        {
            return Read(Roster + Record * (uint)slot, 4);
        }

        public uint Player(int slot, int offset, int size)
        {
            return Read(Roster + Record * (uint)slot + (uint)offset, size);
        }

        public static bool ValidPlayer(uint id)
        {
            return (id > 0 && id < 0x46A2) || (id >= 0x7FFD && id <= 0x7FFF);
        }
    }

    public class CheatValue
    {
        public int Index;   // the cheat's n-th code (a multi-write counts once)
        public string Label, Kind; // number, staff, scout
        public long Min, Max, Default;
    }

    public class CheatInfo
    {
        public string Name, Title, Origin, Category, Note = "";
        public int Codes;
        public bool OnceDefault, Switch;
        public readonly List<CheatValue> Values = new List<CheatValue>();
    }

    public static class Cheats
    {
        static readonly Encoding Utf8 = new UTF8Encoding(false);

        public static string Dir(Launcher launcher)
        {
            return Path.Combine(launcher.Dir, "cheats");
        }

        public static string FilePath(Launcher launcher)
        {
            return Path.Combine(Dir(launcher), "BLJM61064.txt");
        }

        static string EnabledPath(Launcher launcher)
        {
            return Path.Combine(Dir(launcher), "enabled.txt");
        }

        static string ValuesPath(Launcher launcher)
        {
            return Path.Combine(Dir(launcher), "values.txt");
        }

        public static string[] Lines(string path)
        {
            try
            {
                return File.Exists(path) ? File.ReadAllLines(path, Encoding.UTF8) : new string[0];
            }
            catch (Exception)
            {
                return new string[0];
            }
        }

        // "_V0 name" / "_V1 name" starts a cheat; "#@cat", "#@note", "#@value n|label|min|max|kind", "#@once" are the
        // generator's metadata (tools/psv_to_ps3.py); "$" lines are its codes, counted as the game counts them (the
        // second line of a multi-write, $4 or $D, belongs to the code before it).
        public static List<CheatInfo> List(Launcher launcher)
        {
            var cheats = new List<CheatInfo>();
            string category = "기타";
            bool second = false;
            foreach (string raw in Lines(FilePath(launcher)))
            {
                string line = raw.Trim().TrimStart('﻿');
                CheatInfo last = cheats.Count > 0 ? cheats[cheats.Count - 1] : null;
                if (line.StartsWith("#@cat "))
                    category = line.Substring(6).Trim();
                else if (line.Length > 3 && line[0] == '_' && line[1] == 'V' && (line[2] == '0' || line[2] == '1'))
                {
                    string name = line.Substring(3).Trim();
                    int paren = name.LastIndexOf(" (", StringComparison.Ordinal);
                    bool origin = paren > 0 && name.EndsWith(")");
                    cheats.Add(new CheatInfo { Name = name, Category = category,
                                               Title = origin ? name.Substring(0, paren) : name,
                                               Origin = origin ? name.Substring(paren + 2, name.Length - paren - 3) : "" });
                    second = false;
                }
                else if (last != null && line.StartsWith("#@note "))
                    last.Note = line.Substring(7).Trim();
                else if (last != null && line == "#@once")
                    last.OnceDefault = true;
                else if (last != null && line.StartsWith("#@value "))
                {
                    string[] f = line.Substring(8).Split('|');
                    long min, max;
                    int index;
                    if (f.Length >= 5 && int.TryParse(f[0], out index) && long.TryParse(f[2], out min) && long.TryParse(f[3], out max))
                        last.Values.Add(new CheatValue { Index = index, Label = f[1], Min = min, Max = max, Kind = f[4] });
                }
                else if (last != null && line.StartsWith("$"))
                {
                    if (second)
                    {
                        second = false;
                        continue;
                    }
                    string type = line.Length >= 5 ? line.Substring(1, 4) : "";
                    if (type.StartsWith("F0", StringComparison.OrdinalIgnoreCase))
                        last.Switch = true;
                    second = type.StartsWith("4") || type.StartsWith("D", StringComparison.OrdinalIgnoreCase);
                    last.Codes++;
                }
            }
            // The default of a value is the value in its code line.
            foreach (CheatInfo cheat in cheats.Where(c => c.Values.Count > 0))
            {
                List<string> codes = Codes(launcher, cheat.Name);
                foreach (CheatValue v in cheat.Values)
                {
                    long value;
                    if (v.Index < codes.Count && long.TryParse(codes[v.Index].Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries)[2],
                                                              System.Globalization.NumberStyles.HexNumber, null, out value))
                        v.Default = value;
                }
            }
            return cheats;
        }

        // The code lines of one cheat, in the game's numbering (multi-write second lines left out).
        static List<string> Codes(Launcher launcher, string name)
        {
            var codes = new List<string>();
            bool inside = false, second = false;
            foreach (string raw in Lines(FilePath(launcher)))
            {
                string line = raw.Trim().TrimStart('﻿');
                if (line.StartsWith("_V"))
                {
                    inside = line.Substring(3).Trim() == name;
                    second = false;
                }
                else if (inside && line.StartsWith("$"))
                {
                    if (second)
                    {
                        second = false;
                        continue;
                    }
                    string type = line.Length >= 5 ? line.Substring(1, 4) : "";
                    second = type.StartsWith("4") || type.StartsWith("D", StringComparison.OrdinalIgnoreCase);
                    codes.Add(line);
                }
            }
            return codes;
        }

        // The "# Not moved yet" list at the end of the file: Vita cheats without a PS3 address yet.
        public static List<string> NotMoved(Launcher launcher)
        {
            var names = new List<string>();
            bool inList = false;
            foreach (string line in Lines(FilePath(launcher)))
            {
                if (line.StartsWith("# Not moved yet"))
                    inList = true;
                else if (inList && line.StartsWith("#   "))
                    names.Add(line.Substring(4).Trim());
            }
            return names;
        }

        public static HashSet<string> Enabled(Launcher launcher)
        {
            return new HashSet<string>(Lines(EnabledPath(launcher)).Select(l => l.Trim().TrimStart('﻿')).Where(l => l.Length > 0));
        }

        public static int EnabledCount(Launcher launcher)
        {
            HashSet<string> on = Enabled(launcher);
            return List(launcher).Count(c => on.Contains(c.Name));
        }

        public static void Set(Launcher launcher, string name, bool on)
        {
            HashSet<string> enabled = Enabled(launcher);
            if (on)
                enabled.Add(name);
            else
                enabled.Remove(name);
            Directory.CreateDirectory(Dir(launcher));
            // Only names the file still has, in its order.
            var names = List(launcher).Select(c => c.Name).Where(enabled.Contains);
            File.WriteAllText(EnabledPath(launcher), string.Join("\r\n", names) + "\r\n", Utf8);
        }

        // values.txt: "<cheat name>\t<n>\t<value>" and "<cheat name>\tonce\t0|1" (saka_hooks.c cheats_values).
        public static Dictionary<string, string> Values(Launcher launcher)
        {
            var values = new Dictionary<string, string>();
            foreach (string line in Lines(ValuesPath(launcher)))
            {
                string[] f = line.TrimStart('﻿').Split('\t');
                if (f.Length == 3)
                    values[f[0] + "\t" + f[1]] = f[2];
            }
            return values;
        }

        public static void SetValue(Launcher launcher, string name, string key, string value)
        {
            Dictionary<string, string> values = Values(launcher);
            if (value == null)
                values.Remove(name + "\t" + key);
            else
                values[name + "\t" + key] = value;
            Directory.CreateDirectory(Dir(launcher));
            File.WriteAllText(ValuesPath(launcher), string.Join("", values.Select(kv => kv.Key + "\t" + kv.Value + "\r\n")), Utf8);
        }

        public static bool Once(Launcher launcher, CheatInfo cheat)
        {
            string once;
            return Values(launcher).TryGetValue(cheat.Name + "\tonce", out once) ? once == "1" : cheat.OnceDefault;
        }

        // A name picker of the game's names (staff/scout/player IDs), or null for a plain number.
        public static Dictionary<int, string> NamesFor(Launcher launcher, string kind)
        {
            if (kind == "staff")
                return GameText.Staff(launcher);
            if (kind == "scout")
                return GameText.Scouts(launcher);
            if (kind == "player")
                return GameText.Players(launcher);
            return null;
        }
    }

    public class CheatPage : PageBase
    {
        string filter = "";

        public CheatPage(MainWindow window) : base(window)
        {
        }

        FrameworkElement ValueLine(CheatInfo cheat, CheatValue v, Dictionary<string, string> values)
        {
            string stored;
            long current = v.Default;
            if (values.TryGetValue(cheat.Name + "\t" + v.Index, out stored))
                long.TryParse(stored, out current);
            var label = Ui.Text(v.Label + "  (" + v.Min.ToString("N0") + "~" + v.Max.ToString("N0") + ", 기본 " + v.Default.ToString("N0") + ")", "Caption");
            label.VerticalAlignment = VerticalAlignment.Center;
            label.Margin = new Thickness(0, 0, 10, 0);
            FrameworkElement input;
            Func<string> read;
            Dictionary<int, string> names = Cheats.NamesFor(Launcher, v.Kind);
            if (names != null && names.Count > 0)
            {
                var box = new ComboBox { Width = 260 };
                var ids = names.Keys.Where(id => id >= v.Min && id <= v.Max).OrderBy(id => id).ToList();
                foreach (int id in ids)
                    box.Items.Add(id + "  " + names[id]);
                box.SelectedIndex = ids.IndexOf((int)current);
                input = box;
                read = delegate
                {
                    string text = (box.SelectedItem as string ?? box.Text ?? "").Trim();
                    int space = text.IndexOf(' ');
                    return space > 0 ? text.Substring(0, space) : text;
                };
            }
            else
            {
                var box = new TextBox { Width = 120, Text = current.ToString() };
                input = box;
                read = () => box.Text.Replace(",", "").Trim();
            }
            var apply = Ui.Button("적용", delegate
            {
                long value;
                if (!long.TryParse(read(), out value) || value < v.Min || value > v.Max)
                {
                    Window.Toast(v.Label + ": " + v.Min.ToString("N0") + "~" + v.Max.ToString("N0") + " 사이의 값을 넣으세요");
                    return;
                }
                try
                {
                    Cheats.SetValue(Launcher, cheat.Name, v.Index.ToString(), value == v.Default ? null : value.ToString());
                    Window.Toast(cheat.Title + " · " + v.Label + " = " + value.ToString("N0") + (Game.IsRunning ? " (게임에 바로 적용)" : ""));
                }
                catch (Exception error)
                {
                    Window.Error("값을 저장하지 못했습니다", error);
                }
            }, null, "");
            apply.Padding = new Thickness(10, 3, 10, 3);
            apply.Margin = new Thickness(8, 0, 0, 0);
            var line = Ui.Row(label, input, apply);
            line.Margin = new Thickness(0, 6, 0, 0);
            return line;
        }

        protected override FrameworkElement Build()
        {
            List<CheatInfo> cheats = Cheats.List(Launcher);
            HashSet<string> on = Cheats.Enabled(Launcher);
            Dictionary<string, string> values = Cheats.Values(Launcher);
            var list = new StackPanel();
            Action fill = null;
            fill = delegate
            {
                list.Children.Clear();
                if (cheats.Count == 0)
                    list.Children.Add(Ui.Text("cheats\\BLJM61064.txt가 없거나 치트가 없습니다.", "Caption"));
                foreach (var group in cheats.Where(c => filter.Length == 0 ||
                                                        (c.Title + " " + c.Origin + " " + c.Note + " " + c.Category).IndexOf(filter, StringComparison.CurrentCultureIgnoreCase) >= 0)
                                            .GroupBy(c => c.Category))
                {
                    string category = group.Key;
                    int count = cheats.Count(c => c.Category == category), lit = cheats.Count(c => c.Category == category && on.Contains(c.Name));
                    var header = Ui.Text(category + "  ·  " + lit + " / " + count + " 켜짐", "H2");
                    header.Margin = new Thickness(0, list.Children.Count == 0 ? 0 : 14, 0, 6);
                    list.Children.Add(header);
                    foreach (CheatInfo item in group)
                    {
                        CheatInfo cheat = item;
                        var extra = new StackPanel();
                        Border kind = cheat.Switch ? Ui.Chip("", "코드 스위치", Ui.Brush("Good"))
                                    : cheat.Title.Contains("[미검증]") ? Ui.Chip("", "미검증", Ui.Brush("Warn"))
                                    : Ui.Chip("", "데이터 " + cheat.Codes + "줄", Ui.Brush("SubText"));
                        kind.Padding = new Thickness(8, 1, 8, 1);
                        kind.HorizontalAlignment = HorizontalAlignment.Left;
                        kind.Margin = new Thickness(0, 4, 0, 0);
                        extra.Children.Add(kind);
                        foreach (CheatValue v in cheat.Values)
                            extra.Children.Add(ValueLine(cheat, v, values));
                        if (!cheat.Switch)
                        {
                            var lockBox = Ui.Switch(!Cheats.Once(Launcher, cheat), delegate(bool locked)
                            {
                                try
                                {
                                    Cheats.SetValue(Launcher, cheat.Name, "once", locked == !cheat.OnceDefault ? null : (locked ? "0" : "1"));
                                    Window.Toast(cheat.Title + (locked ? ": 고정 (1초에 10번 다시 씀)" : ": 한 번만 씀 (켤 때·값을 바꿀 때)"));
                                }
                                catch (Exception error)
                                {
                                    Window.Error("설정을 저장하지 못했습니다", error);
                                }
                            }, "고정 (끄면 켤 때 한 번만 씀)");
                            lockBox.Margin = new Thickness(0, 6, 0, 0);
                            extra.Children.Add(lockBox);
                        }
                        string hint = (cheat.Origin.Length > 0 && cheat.Origin != "PS3" ? "Vita: " + cheat.Origin : "PS3 포트") +
                                      (cheat.Note.Length > 0 ? " · " + cheat.Note : "");
                        list.Children.Add(Ui.ToggleRow(cheat.Title, hint, on.Contains(cheat.Name), delegate(bool value)
                        {
                            try
                            {
                                Cheats.Set(Launcher, cheat.Name, value);
                                if (value)
                                    on.Add(cheat.Name);
                                else
                                    on.Remove(cheat.Name);
                                Window.Toast((value ? "켰습니다: " : "껐습니다: ") + cheat.Title + (Game.IsRunning ? " (게임에 바로 적용)" : ""));
                            }
                            catch (Exception error)
                            {
                                Window.Error("치트 설정을 저장하지 못했습니다", error);
                            }
                        }, extra));
                    }
                }
            };
            fill();

            var search = new TextBox { Width = 260, Margin = new Thickness(0, 0, 0, 10) };
            search.TextChanged += delegate { filter = search.Text.Trim(); fill(); };

            var notMoved = new StackPanel();
            foreach (string name in Cheats.NotMoved(Launcher))
                notMoved.Children.Add(Ui.Text("· " + name, "Caption"));

            return Ui.Page("치트", "게임 안에서도 됩니다: F2 (패드 L3+R3) = 치트 메뉴, F7 = 2배속, F8 = 터보 (x6). 켜고 끄면 게임 중에도 1초 안에 적용됩니다. 고정이 켜진 치트는 값을 1초에 10번 다시 쓰고, 고정을 끄면 켤 때(값을 바꿀 때) 한 번만 씁니다. 끄면 더 쓰지 않을 뿐, 이미 바뀐 값은 그대로 남습니다.",
                Ui.Card("찾기", Ui.Row(Ui.Text("이름·설명으로 찾기  ", "Caption"), search)),
                Ui.Card("치트 (분류별)", list,
                    Ui.Text("모두 지금 구단의 데이터에 씁니다. GM이 다른 구단·리그로 옮기면 게임이 그 구단을 같은 자리에 다시 만들므로 옮긴 구단에만 적용됩니다. 선수 한 명씩은 '선수' 페이지, 후보 목록은 '목록' 페이지에서. 세이브하면 바뀐 값이 저장되니 먼저 세이브 페이지에서 백업하세요. 안전 설정으로 실행하면 치트는 꺼집니다.", "Caption")),
                Ui.Card("옮기지 않았거나 뺀 Vita 치트", notMoved),
                Ui.Card("파일",
                    Ui.Row(Ui.Button("치트 파일 열기", delegate { Process.Start("notepad.exe", "\"" + Cheats.FilePath(Launcher) + "\""); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button("cheats 폴더 열기", delegate { Ui.OpenFolder(Cheats.Dir(Launcher)); }, null, "")),
                    Ui.Text("VitaCheat 문법에 PS3 주소입니다 ($0000 8비트, $0100 16비트, $0200 32비트, $4T0x + 둘째 줄 = 여러 칸 쓰기, $CT00/$DT0x = 메모리 값이 더 클 때만 쓰기, $F0nn = 코드 스위치). 직접 추가한 치트도 여기 나옵니다.", "Caption")));
        }
    }
}
