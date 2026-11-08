// Player page: the players of the user's club (the roster the port exports in cheats\live\state.bin, names from the
// game's own text) with their values; edits of one player at a time go to cheats\players.txt (by player ID, so they
// follow the player and touch only the club he is in: saka_hooks.c cheats_players_apply), each written once or 고정.
// List page: the staff and scout candidate lists and the overseas (European) player list, filled with the game's
// names; choices go to cheats\lists.txt (cheats_lists_apply).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Controls;

namespace SakatsukuLauncher
{
    public class PlayerEdit
    {
        public uint Id;
        public string Field;   // a<n> ability, l<n> limit, b/h/w<hex offset>
        public long Value;
        public bool Lock;
    }

    // A field of the player record the page edits (offsets from REA, NOTES.md "Cheats").
    public class PlayerField
    {
        public string Key, Label;
        public int Offset, Size;
        public long Min, Max;

        public PlayerField(string key, string label, int offset, int size, long min, long max)
        {
            Key = key; Label = label; Offset = offset; Size = size; Min = min; Max = max;
        }
    }

    public static class PlayerEdits
    {
        static readonly Encoding Utf8 = new UTF8Encoding(false);

        public static readonly PlayerField[] Basic =
        {
            new PlayerField("b5", "나이", 0x05, 1, 15, 50),
            new PlayerField("b7", "계약 연수", 0x07, 1, 1, 9),
            new PlayerField("b4", "각성 레벨", 0x04, 1, 0, 15),
            new PlayerField("hc6", "각성 포인트", 0xC6, 2, 0, 65535),
            new PlayerField("h108", "스타성", 0x108, 2, 0, 65534),
            new PlayerField("w208", "성장 타입", 0x208, 4, 0, 7),
            new PlayerField("hc8", "컨디션", 0xC8, 2, 0, 1000),
            new PlayerField("hca", "피로", 0xCA, 2, 0, 1000),
        };

        static string FilePath(Launcher launcher)
        {
            return Path.Combine(Cheats.Dir(launcher), "players.txt");
        }

        public static List<PlayerEdit> Load(Launcher launcher)
        {
            var edits = new List<PlayerEdit>();
            foreach (string line in Cheats.Lines(FilePath(launcher)))
            {
                string[] f = line.TrimStart('﻿').Split('\t');
                uint id;
                long value;
                if (f.Length >= 3 && uint.TryParse(f[0], out id) && long.TryParse(f[2], out value))
                    edits.Add(new PlayerEdit { Id = id, Field = f[1], Value = value, Lock = f.Length < 4 || f[3] != "0" });
            }
            return edits;
        }

        public static void Save(Launcher launcher, List<PlayerEdit> edits)
        {
            Directory.CreateDirectory(Cheats.Dir(launcher));
            File.WriteAllText(FilePath(launcher), string.Join("", edits.Select(e => e.Id + "\t" + e.Field + "\t" + e.Value + "\t" + (e.Lock ? "1" : "0") + "\r\n")), Utf8);
        }

        // Sets (or with value null removes) one field of one player; a field is kept once per player.
        public static void Set(Launcher launcher, uint id, string field, long? value, bool locked)
        {
            List<PlayerEdit> edits = Load(launcher);
            edits.RemoveAll(e => e.Id == id && e.Field == field);
            if (value.HasValue)
                edits.Add(new PlayerEdit { Id = id, Field = field, Value = value.Value, Lock = locked });
            Save(launcher, edits);
        }

        public static void Clear(Launcher launcher, uint id)
        {
            List<PlayerEdit> edits = Load(launcher);
            edits.RemoveAll(e => e.Id == id);
            Save(launcher, edits);
        }
    }

    // Where the players and lists pages read the game (owner, 2026-11-08: "세이브 별로 읽을수 있도록 세이브 파일을
    // 선택"): the running game's export (cheats\live\state.bin, every 2 s) or a save slot (MAIN0x.DAT, any copy of
    // one too). Edits are kept by player ID and slot as before and are written in the game once that save is loaded.
    // Launcher state "DataSource": "" = the running game, falling back to the newest save slot when there is none.
    public static class GameSource
    {
        public static string SaveDir(Launcher launcher)
        {
            return Path.Combine(launcher.SavesDir, GameFiles.TitleId + "-SAVEDATA");
        }

        public static List<string> Slots(Launcher launcher)
        {
            string dir = SaveDir(launcher);
            if (!Directory.Exists(dir))
                return new List<string>();
            return Directory.GetFiles(dir, "MAIN0?.DAT").OrderBy(p => p, StringComparer.OrdinalIgnoreCase).ToList();
        }

        static string Picked(Launcher launcher)
        {
            string path = launcher.State("DataSource", "");
            return path.Length > 0 && File.Exists(path) ? path : "";
        }

        public static LiveState Load(Launcher launcher)
        {
            string path = Picked(launcher);
            if (path.Length > 0)
                return LiveState.FromSave(path);
            LiveState live = LiveState.Load(launcher);
            if (live != null)
                return live;
            string newest = Slots(launcher).OrderByDescending(File.GetLastWriteTime).FirstOrDefault();
            return newest != null ? LiveState.FromSave(newest) : null;
        }

        public static string SlotName(string path)
        {
            string name = Path.GetFileNameWithoutExtension(path).ToUpperInvariant();
            int n;
            return name.StartsWith("MAIN") && int.TryParse(name.Substring(4), out n) ? "세이브 " + (n + 1) + "번 칸" : Path.GetFileName(path);
        }

        // What the state shown is, for a card title: "세이브 1번 칸 (2026-11-07 17:16 저장)" or "실행 중인 게임 (21:41:00 기준)".
        public static string Describe(LiveState state)
        {
            if (state == null)
                return "데이터 없음";
            return state.Source.Length > 0 ? SlotName(state.Source) + " (" + state.Time.ToString("MM-dd HH:mm") + " 저장)"
                                           : "실행 중인 게임 (" + state.Time.ToString("HH:mm:ss") + " 기준)";
        }

        public static FrameworkElement Picker(MainWindow window, Launcher launcher)
        {
            string picked = Picked(launcher);
            string live = LiveState.FilePath(launcher);
            var pairs = new List<string> { "", File.Exists(live) ? "실행 중인 게임 (" + File.GetLastWriteTime(live).ToString("MM-dd HH:mm:ss") + " 기준)"
                                                                 : "실행 중인 게임 (아직 없음: 가장 최근 세이브를 보여 줍니다)" };
            List<string> slots = Slots(launcher);
            foreach (string slot in slots)
            {
                pairs.Add(slot);
                pairs.Add(SlotName(slot) + " (" + File.GetLastWriteTime(slot).ToString("yyyy-MM-dd HH:mm") + " 저장)");
            }
            if (picked.Length > 0 && !slots.Contains(picked, StringComparer.OrdinalIgnoreCase))
            {
                pairs.Add(picked);
                pairs.Add(SlotName(picked) + " · " + picked);
            }
            pairs.Add("?pick");
            pairs.Add("다른 세이브 파일 고르기…");
            ComboBox box = Ui.Choices(picked, delegate(string value)
            {
                if (value == "?pick")
                {
                    var dialog = new Microsoft.Win32.OpenFileDialog { Title = "사카츠쿠 세이브 파일 (MAIN0x.DAT)", Filter = "사카츠쿠 세이브 (MAIN*.DAT)|MAIN*.DAT|모든 파일 (*.*)|*.*" };
                    if (dialog.ShowDialog(window) != true)
                    {
                        window.Refresh();
                        return;
                    }
                    if (!LiveState.LooksLikeSave(dialog.FileName))
                    {
                        window.Toast("사카츠쿠 세이브 파일(MAIN0x.DAT)이 아닙니다");
                        window.Refresh();
                        return;
                    }
                    value = dialog.FileName;
                }
                launcher.SetState("DataSource", value);
                window.Refresh();
            }, pairs.ToArray());
            box.MinWidth = 420;
            var panel = new StackPanel { Margin = new Thickness(0, 0, 0, 12) };
            panel.Children.Add(Ui.Row(Ui.Text("읽을 데이터   ", "Body"), box));
            var note = Ui.Text("세이브를 골라 보고 고칠 수 있습니다. 고친 값은 선수 ID로 기억해, 게임에서 그 세이브를 불러왔을 때 씁니다.", "Caption");
            note.Margin = new Thickness(0, 4, 0, 0);
            panel.Children.Add(note);
            return panel;
        }
    }

    public class PlayersPage : PageBase
    {
        int selected = -1;

        public PlayersPage(MainWindow window) : base(window)
        {
        }

        static string Age(LiveState s, int slot)
        {
            return s.Player(slot, 5, 1) + "세";
        }

        protected override FrameworkElement Build()
        {
            LiveState state = GameSource.Load(Launcher);
            Dictionary<int, string> names = GameText.Players(Launcher);
            string[] abilities = GameText.Abilities(Launcher);
            List<PlayerEdit> edits = PlayerEdits.Load(Launcher);

            if (state == null)
                return Ui.Page("선수", "구단 선수를 한 명씩 고치고 고정합니다.", GameSource.Picker(Window, Launcher),
                    Ui.Card("게임 데이터가 아직 없습니다",
                        Ui.Text("위에서 세이브 파일을 고르거나, 게임을 실행해 세이브를 불러오세요 (포트가 2초마다 cheats\\live\\state.bin에 구단 선수 정보를 씁니다. 안전 설정으로 실행하면 치트가 꺼져 쓰지 않습니다).", "Caption")));

            var roster = new StackPanel();
            var detail = new StackPanel();
            Action<int> show = null;
            Action drawRoster = delegate
            {
                roster.Children.Clear();
                for (int slot = 0; slot < 23; slot++)
                {
                    uint id = state.PlayerId(slot);
                    if (!LiveState.ValidPlayer(id))
                        continue;
                    int k = slot;
                    int count = edits.Count(e => e.Id == id);
                    var button = Ui.Button((slot + 1).ToString("00") + "  " + GameText.Name(names, id) + "  ·  " + Age(state, slot) +
                                           (count > 0 ? "  ·  수정 " + count : ""), delegate { show(k); }, slot == selected ? "AccentButton" : null, "");
                    button.HorizontalContentAlignment = HorizontalAlignment.Left;
                    button.Margin = new Thickness(0, 0, 0, 4);
                    roster.Children.Add(button);
                }
                if (roster.Children.Count == 0)
                    roster.Children.Add(Ui.Text("구단 선수 목록이 비어 있습니다 (세이브를 불러온 뒤 2초쯤 지나 다시 여세요).", "Caption"));
            };

            show = delegate(int slot)
            {
                selected = slot;
                drawRoster();
                detail.Children.Clear();
                uint id = state.PlayerId(slot);
                if (!LiveState.ValidPlayer(id))
                    return;
                List<PlayerEdit> mine = PlayerEdits.Load(Launcher).Where(e => e.Id == id).ToList();
                detail.Children.Add(Ui.Text(GameText.Name(names, id) + "  (선수 ID " + id + ", " + (slot + 1) + "번 칸)", "H2"));
                detail.Children.Add(Ui.Text("값을 넣고 '적용'을 누르면 씁니다. '고정'이 켜져 있으면 1초에 10번 다시 써서 게임이 바꿔도 돌아오고, 꺼져 있으면 한 번만 씁니다. 이 선수가 지금 구단에 있을 때만 씁니다.", "Caption"));

                // One row: label, current value (and limit), box, 고정, 적용, 해제.
                Func<string, string, string, long, long, FrameworkElement> row = delegate(string field, string label, string current, long min, long max)
                {
                    PlayerEdit edit = mine.FirstOrDefault(e => e.Field == field);
                    var grid = new Grid { Margin = new Thickness(0, 2, 0, 2) };
                    foreach (double w in new double[] { 130, 110, 64, 84, 58, 58 })
                        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(w) });
                    var name = Ui.Text(label, "Body");
                    var now = Ui.Text(current, "Caption");
                    var box = new TextBox { Text = edit != null ? edit.Value.ToString() : "", Width = 58, Margin = new Thickness(0, 0, 6, 0) };
                    var lockBox = Ui.Switch(edit != null && edit.Lock, delegate { }, "고정");   // off until asked for
                    var apply = Ui.Button("적용", delegate
                    {
                        long value;
                        if (!long.TryParse(box.Text.Replace(",", "").Trim(), out value) || value < min || value > max)
                        {
                            Window.Toast(label + ": " + min + "~" + max + " 사이의 숫자를 넣으세요");
                            return;
                        }
                        PlayerEdits.Set(Launcher, id, field, value, lockBox.IsChecked == true);
                        Window.Toast(GameText.Name(names, id) + " · " + label + " = " + value + (lockBox.IsChecked == true ? " (고정)" : " (한 번)"));
                        edits = PlayerEdits.Load(Launcher);
                        drawRoster();
                    }, null, "");
                    apply.Padding = new Thickness(8, 1, 8, 1);
                    var clear = Ui.Button("해제", delegate
                    {
                        PlayerEdits.Set(Launcher, id, field, null, false);
                        box.Text = "";
                        Window.Toast(GameText.Name(names, id) + " · " + label + ": 수정 해제 (지금 값은 그대로)");
                        edits = PlayerEdits.Load(Launcher);
                        drawRoster();
                    }, null, "");
                    clear.Padding = new Thickness(8, 1, 8, 1);
                    clear.IsEnabled = edit != null;
                    UIElement[] cells = { name, now, box, lockBox, apply, clear };
                    for (int c = 0; c < cells.Length; c++)
                    {
                        Grid.SetColumn(cells[c], c);
                        var fe = cells[c] as FrameworkElement;
                        if (fe != null)
                            fe.VerticalAlignment = VerticalAlignment.Center;
                        grid.Children.Add(cells[c]);
                    }
                    return grid;
                };

                var basic = new StackPanel();
                foreach (PlayerField f in PlayerEdits.Basic)
                    basic.Children.Add(row(f.Key, f.Label, "지금 " + state.Player(slot, f.Offset, f.Size), f.Min, f.Max));
                detail.Children.Add(Ui.Card("기본", basic));

                string[] groups = { "능력", "포지션 적성", "전술 이해", "포메이션 이해" };
                int[] starts = { 0, 19, 30, 38, 46 };
                for (int g = 0; g < groups.Length; g++)
                {
                    var panel = new StackPanel();
                    for (int a = starts[g]; a < starts[g + 1]; a++)
                    {
                        uint value = state.Player(slot, 0x0C + a, 1), limit = state.Player(slot, 0x3B + a, 1);
                        panel.Children.Add(row("a" + a, abilities[a], "지금 " + value + " / 한계 " + limit, 0, 100));
                        panel.Children.Add(row("l" + a, "  └ 한계", "", 0, 100));
                    }
                    detail.Children.Add(Ui.Card(groups[g], panel));
                }

                Action<bool, string> all = delegate(bool limits, string what)
                {
                    List<PlayerEdit> list = PlayerEdits.Load(Launcher);
                    for (int a = 0; a < 46; a++)
                    {
                        string key = (limits ? "l" : "a") + a;
                        list.RemoveAll(e => e.Id == id && e.Field == key);
                        list.Add(new PlayerEdit { Id = id, Field = key, Value = 100, Lock = true });
                    }
                    PlayerEdits.Save(Launcher, list);
                    Window.Toast(GameText.Name(names, id) + ": " + what + " 100 (고정)");
                    show(slot);
                };
                detail.Children.Add(Ui.Card("한 번에",
                    Ui.Row(Ui.Button("능력 전부 100 고정", delegate { all(false, "능력 전부"); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button("한계 전부 100 고정", delegate { all(true, "한계 전부"); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button("이 선수 수정 전부 해제", delegate
                           {
                               PlayerEdits.Clear(Launcher, id);
                               Window.Toast(GameText.Name(names, id) + ": 수정 전부 해제 (지금 값은 그대로)");
                               edits = PlayerEdits.Load(Launcher);
                               show(slot);
                           }, null, "")),
                    Ui.Text("능력을 바꾸면 그 능력의 경험치도 게임 표의 값으로 맞춥니다 (게임의 능력 설정 함수와 같은 방식). 게임 최대는 능력·한계 100.", "Caption")));
            };

            if (selected < 0)   // the first player of the club until one is picked
                for (int slot = 0; slot < 23 && selected < 0; slot++)
                    if (LiveState.ValidPlayer(state.PlayerId(slot)))
                        selected = slot;
            drawRoster();
            if (selected >= 0 && selected < 23 && LiveState.ValidPlayer(state.PlayerId(selected)))
                show(selected);
            else
                detail.Children.Add(Ui.Text("왼쪽에서 선수를 고르세요.", "Caption"));

            var columns = new Grid();
            columns.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(280) });
            columns.ColumnDefinitions.Add(new ColumnDefinition());
            var left = Ui.Card(GameSource.Describe(state), roster,
                               Ui.Button("새로 읽기", delegate { Shown(); }, null, "", new Thickness(0, 8, 0, 0)));
            left.Margin = new Thickness(0, 0, 12, 0);
            columns.Children.Add(left);
            Grid.SetColumn(detail, 1);
            columns.Children.Add(detail);
            return Ui.Page("선수", "'선수 전원' 치트를 선수 한 명씩: 값 적용과 고정. 선수 ID로 기억하므로 선수가 구단을 떠나면 쓰지 않고, GM이 구단을 옮기면 옮긴 구단 선수만 보입니다.",
                           GameSource.Picker(Window, Launcher), columns);
        }
    }

    public class ListEdit
    {
        public string List;
        public int Slot;
        public uint Id;
        public bool Lock;
    }

    public static class ListEdits
    {
        static readonly Encoding Utf8 = new UTF8Encoding(false);

        static string FilePath(Launcher launcher)
        {
            return Path.Combine(Cheats.Dir(launcher), "lists.txt");
        }

        public static List<ListEdit> Load(Launcher launcher)
        {
            var edits = new List<ListEdit>();
            foreach (string line in Cheats.Lines(FilePath(launcher)))
            {
                string[] f = line.TrimStart('﻿').Split('\t');
                int slot;
                uint id;
                if (f.Length >= 3 && int.TryParse(f[1], out slot) && uint.TryParse(f[2], out id))
                    edits.Add(new ListEdit { List = f[0], Slot = slot, Id = id, Lock = f.Length < 4 || f[3] != "0" });
            }
            return edits;
        }

        public static void Set(Launcher launcher, string list, int slot, uint? id, bool locked)
        {
            List<ListEdit> edits = Load(launcher);
            edits.RemoveAll(e => e.List == list && e.Slot == slot);
            if (id.HasValue)
                edits.Add(new ListEdit { List = list, Slot = slot, Id = id.Value, Lock = locked });
            Directory.CreateDirectory(Cheats.Dir(launcher));
            File.WriteAllText(FilePath(launcher), string.Join("", edits.Select(e => e.List + "\t" + e.Slot + "\t" + e.Id + "\t" + (e.Lock ? "1" : "0") + "\r\n")), Utf8);
        }
    }

    public class ListsPage : PageBase
    {
        // A name's length without its legend tag (GameText.Players), for the search order.
        static int BaseLength(string name)
        {
            int tag = name.IndexOf(" (레전드", StringComparison.Ordinal);
            return tag >= 0 ? tag : name.Length;
        }

        public ListsPage(MainWindow window) : base(window)
        {
        }

        // list: coach / scout / euro; slots, address and stride as saka_hooks.c cheats_lists_apply.
        FrameworkElement ListCard(string title, string list, int slots, uint addr, uint stride, Dictionary<int, string> names, int maxId,
                                  Dictionary<int, int> nationOf, LiveState state, string note)
        {
            List<ListEdit> edits = ListEdits.Load(Launcher);
            Dictionary<int, string> nations = GameText.Nations(Launcher);
            var panel = new StackPanel();
            var ids = names.Keys.Where(i => i > 0 && i < maxId).OrderBy(i => i).ToList();
            var pickers = new List<ComboBox>();
            int nation = -1; // the country filter: -1 = all
            Func<int, string> label = delegate(int id)
            {
                int n;
                return GameText.Name(names, id) + "  (" + id + ")" + (nationOf.TryGetValue(id, out n) ? "  · " + GameText.Name(nations, n) : "");
            };
            Func<int, string> labelRomaji = delegate(int id)   // the romaji the English search matched, after the name
            {
                int n;
                string shown = GameText.Name(names, id);
                int tag = shown.IndexOf(" (레전드", StringComparison.Ordinal);   // the Japanese name only
                string romaji = NameSearch.Romaji(tag >= 0 ? shown.Substring(0, tag) : shown);
                return GameText.Name(names, id) + (romaji.Length > 0 ? "  [" + romaji + "]" : "") + "  (" + id + ")" +
                       (nationOf.TryGetValue(id, out n) ? "  · " + GameText.Name(nations, n) : "");
            };
            // A picker lists up to 400 names of the chosen country containing the search text (with neither: all names
            // of the short lists; for the 18,082 players only the chosen one).
            Action<ComboBox, string, uint> fillPicker = delegate(ComboBox pick, string search, uint keep)
            {
                pick.Items.Clear();
                // English (romaji) search: every name scored, the closest first (NameSearch.cs); Japanese text or an ID
                // matches as before.
                bool english = NameSearch.IsEnglish(search);
                IEnumerable<int> inNation = ids.Where(i => nation < 0 || (nationOf.ContainsKey(i) && nationOf[i] == nation));
                IEnumerable<int> shown = search.Length == 0 ? inNation
                    : english ? inNation.Select(i => new KeyValuePair<int, int>(i, NameSearch.Score(names[i], search))).Where(p => p.Value >= 0)
                                        .OrderBy(p => p.Value).ThenBy(p => BaseLength(names[p.Key])).ThenBy(p => p.Key).Select(p => p.Key)
                    : inNation.Where(i => names[i].IndexOf(search, StringComparison.CurrentCultureIgnoreCase) >= 0 || i.ToString() == search);
                if (search.Length == 0 && nation < 0 && ids.Count > 900)
                    shown = new int[0];
                foreach (int id in shown.Take(400))
                    pick.Items.Add(english ? labelRomaji(id) : label(id));
                if (keep != 0)
                {
                    string item = label((int)keep);
                    if (!pick.Items.Contains(item))
                        pick.Items.Insert(0, item);
                    pick.SelectedItem = item;
                }
            };
            Func<ComboBox, uint> chosen = delegate(ComboBox pick)
            {
                string text = pick.SelectedItem as string ?? "";
                int dot = text.LastIndexOf("  · ", StringComparison.Ordinal);
                if (dot > 0)
                    text = text.Substring(0, dot);
                int open = text.LastIndexOf('('), close = text.LastIndexOf(')');
                uint id;
                return open >= 0 && close > open && uint.TryParse(text.Substring(open + 1, close - open - 1), out id) ? id : 0;
            };
            var searchBox = new TextBox { Width = 220 };
            // Countries of this list's people, the most common first.
            var countries = ids.Where(nationOf.ContainsKey).GroupBy(i => nationOf[i]).OrderByDescending(g => g.Count())
                               .Select(g => new KeyValuePair<int, int>(g.Key, g.Count())).ToList();
            var countryBox = new ComboBox { Width = 220, Margin = new Thickness(16, 0, 0, 0) };
            countryBox.Items.Add("전체 국가 (" + ids.Count + "명)");
            foreach (var c in countries)
                countryBox.Items.Add(GameText.Name(nations, c.Key) + " (" + c.Value + "명)");
            countryBox.SelectedIndex = 0;
            for (int s = 0; s < slots; s++)
            {
                int slot = s;
                uint current = state != null ? state.Read(addr + stride * (uint)slot, 4) : 0;
                ListEdit edit = edits.FirstOrDefault(e => e.List == list && e.Slot == slot);
                var grid = new Grid { Margin = new Thickness(0, 2, 0, 2) };
                foreach (double w in new double[] { 50, 200, 300, 90, 60, 60 })
                    grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(w) });
                var slotLabel = Ui.Text((slot + 1) + "번", "Body");
                int currentNation;
                var now = Ui.Text(state == null ? "-" : current == 0 ? "(비어 있음)" : GameText.Name(names, current) +
                                  (nationOf.TryGetValue((int)current, out currentNation) ? " · " + GameText.Name(nations, currentNation) : ""), "Caption");
                var pick = new ComboBox { Width = 290 };
                fillPicker(pick, "", edit != null ? edit.Id : 0);
                pickers.Add(pick);
                var lockBox = Ui.Switch(edit != null && edit.Lock, delegate { }, "고정");   // off until asked for
                var apply = Ui.Button("적용", delegate
                {
                    uint id = chosen(pick);
                    if (id == 0 || id >= maxId)
                    {
                        Window.Toast("목록에서 이름을 고르세요 (위 검색 칸에 이름 일부를 넣으면 추려집니다)");
                        return;
                    }
                    ListEdits.Set(Launcher, list, slot, id, lockBox.IsChecked == true);
                    Window.Toast(title + " " + (slot + 1) + "번 = " + GameText.Name(names, id) + (Game.IsRunning ? " (게임에 바로 적용)" : ""));
                }, null, "");
                apply.Padding = new Thickness(8, 1, 8, 1);
                var clear = Ui.Button("해제", delegate
                {
                    ListEdits.Set(Launcher, list, slot, null, false);
                    pick.SelectedIndex = -1;
                    Window.Toast(title + " " + (slot + 1) + "번: 해제 (게임이 다음에 목록을 만들 때 원래대로)");
                }, null, "");
                clear.Padding = new Thickness(8, 1, 8, 1);
                UIElement[] cells = { slotLabel, now, pick, lockBox, apply, clear };
                for (int c = 0; c < cells.Length; c++)
                {
                    Grid.SetColumn(cells[c], c);
                    var fe = cells[c] as FrameworkElement;
                    if (fe != null)
                        fe.VerticalAlignment = VerticalAlignment.Center;
                    grid.Children.Add(cells[c]);
                }
                panel.Children.Add(grid);
            }
            Action refill = delegate
            {
                foreach (ComboBox pick in pickers)
                    fillPicker(pick, searchBox.Text.Trim(), chosen(pick));
            };
            searchBox.TextChanged += delegate { refill(); };
            countryBox.SelectionChanged += delegate
            {
                nation = countryBox.SelectedIndex <= 0 ? -1 : countries[countryBox.SelectedIndex - 1].Key;
                refill();
            };
            var searchRow = Ui.Row(Ui.Text("이름 검색 (일본어·영어)  ", "Caption"), searchBox, countryBox);
            searchRow.Margin = new Thickness(0, 0, 0, 8);
            panel.Children.Insert(0, searchRow);
            return Ui.Card(title, panel, Ui.Text(note, "Caption"));
        }

        protected override FrameworkElement Build()
        {
            LiveState state = GameSource.Load(Launcher);
            return Ui.Page("목록", "게임의 후보 목록을 게임 속 이름에서 골라 바꿉니다. 고정을 켜고 적용하면 게임이 목록을 다시 만들어도 1초에 10번 다시 쓰고, 끄면 한 번만 씁니다.",
                GameSource.Picker(Window, Launcher),
                state == null ? Ui.Text("위에서 세이브 파일을 고르거나 게임을 실행해 세이브를 불러오면 지금 목록이 보입니다. 고르는 것은 지금도 됩니다.", "Caption")
                              : Ui.Text("보이는 목록: " + GameSource.Describe(state), "Caption"),
                ListCard("코치 후보", "coach", 4, 0x00EE72A4, 0xC, GameText.Staff(Launcher), 0x33A, GameData.StaffNations(Launcher), state,
                         "인사의 코치 후보 4칸 (스태프 ID 1~825)."),
                ListCard("스카우트 후보", "scout", 4, 0x00EE72D4, 0xC, GameText.Scouts(Launcher), 0x1AC, GameData.ScoutNations(Launcher), state,
                         "스카우트 후보 4칸 (게임은 보통 3칸을 보여 줌, ID 1~427)."),
                ListCard("유럽 선수 목록", "euro", 20, 0x00EE70E8, 0x14, GameText.Players(Launcher), 0x46A2, GameData.PlayerNations(Launcher), state,
                         "해외(유럽) 선수 목록 20칸 (선수 ID). 고른 선수에게는 원래 칸 선수의 상태 기록 (0x00F0C4B8 표)을 넘겨 줍니다 (Vita 치트와 같은 방식)."));
        }
    }
}
