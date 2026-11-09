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
            return Path.Combine(Cheats.ProfileDir(launcher), "players.txt");
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
            Directory.CreateDirectory(Cheats.ProfileDir(launcher));
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

        // Per-player multipliers (owner, 2026-11-09): xt normal training, xs special training / 秘蔵っ子, xa awakening
        // points, 1-100. saka_hooks.c uses them instead of the switch's value for that player (never both).
        public static readonly string[][] Multipliers =
        {
            new[] { "xt", "일반 연습 배수", "연습" },
            new[] { "xs", "특별연습·비장의 아이 배수", "특별" },
            new[] { "xa", "각성 포인트 배수", "각성" },
        };

        public static bool IsMultiplier(string field)
        {
            return Multipliers.Any(m => m[0] == field);
        }

        public static string MultiplierShort(string field)
        {
            string[] m = Multipliers.FirstOrDefault(x => x[0] == field);
            return m != null ? m[2] : field;
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

        static readonly string[] Positions = { "GK", "LSB", "RSB", "CB", "DM", "LSM", "RSM", "OM", "LWG", "RWG", "FW" };

        static string Age(LiveState s, int slot)
        {
            return s.Player(slot, 5, 1) + "세";
        }

        // Play styles and skills (2026-11-09, owner: "플레이스타일 스킬등도 편집이 가능해야해"; record layout from REA Ghidra and
        // the live save: player 617 = ドリブラー [テクニカルドリブル Lv2, テクニカルトラップ Lv2, 高速ドリブル Lv1]):
        // +0x10A u8 the style slot in use (0-4); 5 slots of 0x30 from +0x10C: +0 u32 style (playstyle_name 1-66), +4 u32
        // original skill (skill_name 113-181, 0 none), 5 skills of 8 bytes from +8: u32 type (skill_type_name 1-16) and u8
        // level at +4 (1-7; 0xFF when the type is 0). Written as players.txt fields b/w<offset> (saka_hooks.c).
        static string StyleLabel(int id, Dictionary<int, string> styles)
        {
            GameText.StyleInfo info;
            if (!GameText.StyleInfos.TryGetValue(id, out info))
                return id + "  " + GameText.Name(styles, id);
            return (info.Tier >= 3 ? "★★★" : info.Tier == 2 ? "★★ " : "★   ") + " " + id + "  " + info.Ko + " (" + info.Ja + ")";
        }

        FrameworkElement StyleCard(int slot, uint id, LiveState state, List<PlayerEdit> mine, Dictionary<int, string> names, Action changed)
        {
            GameText.LoadPlayStyles(Launcher);
            Dictionary<int, string> styles = GameText.PlayStyles(Launcher), types = GameText.SkillTypes(Launcher), skills = GameText.Skills(Launcher);
            Func<string, bool> isStyleField = f =>
            {
                int off;
                return f.Length > 1 && (f[0] == 'w' || f[0] == 'b') &&
                       int.TryParse(f.Substring(1), System.Globalization.NumberStyles.HexNumber, null, out off) && off >= 0x10A && off < 0x10C + 5 * 0x30;
            };
            Func<string, uint, uint> now = delegate(string field, uint current)   // an edit's value, else the game's
            {
                PlayerEdit e = mine.FirstOrDefault(x => x.Field == field);
                return e != null ? (uint)e.Value : current;
            };
            var panel = new StackPanel();
            var inUse = new ComboBox { Width = 110 };
            for (int e = 0; e < 5; e++)
                inUse.Items.Add((e + 1) + "번 칸");
            uint cur = now("b10a", state.Player(slot, 0x10A, 1));
            inUse.SelectedIndex = cur < 5 ? (int)cur : 0;
            bool anyLock = mine.Any(x => isStyleField(x.Field) && x.Lock);
            var lockBox = Ui.Switch(anyLock, delegate { }, "고정");   // off until asked for
            var head = Ui.Row(Ui.Text("쓰는 스타일 칸  ", "Body"), inUse);
            head.Margin = new Thickness(0, 0, 0, 6);
            panel.Children.Add(head);
            var styleBoxes = new ComboBox[5];
            var originalBoxes = new ComboBox[5];
            var typeBoxes = new ComboBox[5, 5];
            var levelBoxes = new ComboBox[5, 5];
            for (int e = 0; e < 5; e++)
            {
                int b = 0x10C + e * 0x30;
                // Styles grouped by family (the base style first, then ★★ upper, ★★★ top), Korean (Japanese) names.
                var st = new ComboBox { Width = 330, MaxDropDownHeight = 520 };
                uint style = now("w" + b.ToString("x"), state.Player(slot, b, 4));
                st.Items.Add(new ComboBoxItem { Content = "0  없음", Tag = 0 });
                for (int fam = 1; fam <= 15; fam++)
                {
                    GameText.StyleInfo baseInfo;
                    string famName = GameText.StyleInfos.TryGetValue(fam, out baseInfo) ? baseInfo.Ko : GameText.Name(styles, fam);
                    st.Items.Add(new ComboBoxItem { Content = "── " + famName + " 계열 ──", IsEnabled = false });
                    for (int tier = 1; tier <= 3; tier++)
                        foreach (KeyValuePair<int, GameText.StyleInfo> kv in GameText.StyleInfos.Where(x => x.Value.Family == fam && x.Value.Tier == tier).OrderBy(x => x.Key))
                            st.Items.Add(new ComboBoxItem { Content = StyleLabel(kv.Key, styles), Tag = kv.Key });
                }
                if (GameText.StyleInfos.Count == 0)   // no playstyles.txt: the game's names by number
                    for (int i = 1; i <= 66; i++)
                        st.Items.Add(new ComboBoxItem { Content = i + "  " + GameText.Name(styles, i), Tag = i });
                foreach (object item in st.Items)
                {
                    var ci = item as ComboBoxItem;
                    if (ci != null && ci.Tag is int && (int)ci.Tag == (int)style)
                        st.SelectedItem = ci;
                }
                if (st.SelectedItem == null)
                    st.SelectedIndex = 0;
                var og = new ComboBox { Width = 300, MaxDropDownHeight = 520 };
                og.Items.Add("0  없음");
                for (int i = 113; i <= 181; i++)
                {
                    string ko;
                    og.Items.Add(i + "  " + (GameText.OriginalKo.TryGetValue(i, out ko) ? ko + " (" + GameText.Name(skills, i) + ")" : GameText.Name(skills, i)));
                }
                uint original = now("w" + (b + 4).ToString("x"), state.Player(slot, b + 4, 4));
                og.SelectedIndex = original >= 113 && original <= 181 ? (int)original - 112 : 0;
                styleBoxes[e] = st;
                originalBoxes[e] = og;
                var line = Ui.Row(Ui.Text((e + 1) + "번 칸" + (e == cur ? " (사용 중)  " : "  "), "Body"), st, Ui.Text("  오리지널 ", "Caption"), og);
                line.Margin = new Thickness(0, 8, 0, 2);
                panel.Children.Add(line);
                var skillLine = new WrapPanel { Margin = new Thickness(16, 0, 0, 0) };
                for (int k = 0; k < 5; k++)
                {
                    int t = b + 8 + k * 8;
                    var tb = new ComboBox { Width = 260, Margin = new Thickness(0, 2, 2, 2) };
                    tb.Items.Add("0 없음 (빈칸)");
                    for (int i = 1; i <= 16; i++)
                    {
                        string[] ko;
                        tb.Items.Add(i + " " + (GameText.SkillTypeKo.TryGetValue(i, out ko) ? ko[0] + " (Lv7: " + ko[1] + ")" : GameText.Name(types, i)));
                    }
                    uint type = now("w" + t.ToString("x"), state.Player(slot, t, 4));
                    tb.SelectedIndex = type <= 16 ? (int)type : 0;
                    var lb = new ComboBox { Width = 64, Margin = new Thickness(0, 2, 12, 2) };
                    for (int l = 1; l <= 7; l++)
                        lb.Items.Add("Lv" + l);
                    uint level = now("b" + (t + 4).ToString("x"), state.Player(slot, t + 4, 1));
                    lb.SelectedIndex = level >= 1 && level <= 7 ? (int)level - 1 : 0;
                    typeBoxes[e, k] = tb;
                    levelBoxes[e, k] = lb;
                    skillLine.Children.Add(tb);
                    skillLine.Children.Add(lb);
                }
                panel.Children.Add(skillLine);
            }
            var apply = Ui.Button("스타일·스킬 적용", delegate
            {
                List<PlayerEdit> list = PlayerEdits.Load(Launcher);
                list.RemoveAll(x => x.Id == id && isStyleField(x.Field));
                bool locked = lockBox.IsChecked == true;
                Action<string, long> put = (f, v) => list.Add(new PlayerEdit { Id = id, Field = f, Value = v, Lock = locked });
                put("b10a", inUse.SelectedIndex);
                for (int e = 0; e < 5; e++)
                {
                    int b = 0x10C + e * 0x30;
                    var chosen = styleBoxes[e].SelectedItem as ComboBoxItem;
                    put("w" + b.ToString("x"), chosen != null && chosen.Tag is int ? (int)chosen.Tag : 0);
                    put("w" + (b + 4).ToString("x"), originalBoxes[e].SelectedIndex == 0 ? 0 : originalBoxes[e].SelectedIndex + 112);
                    for (int k = 0; k < 5; k++)
                    {
                        int t = b + 8 + k * 8, type = typeBoxes[e, k].SelectedIndex;
                        put("w" + t.ToString("x"), type);
                        put("b" + (t + 4).ToString("x"), type == 0 ? 0xFF : levelBoxes[e, k].SelectedIndex + 1);
                    }
                }
                PlayerEdits.Save(Launcher, list);
                Window.Toast(GameText.Name(names, id) + ": 플레이스타일·스킬 적용" + (locked ? " (고정)" : " (한 번)") + (Game.IsRunning ? "" : " · 게임을 켜면 적용"));
                changed();
            }, null, "", new Thickness(0, 10, 8, 0));
            var clear = Ui.Button("스타일·스킬 수정 해제", delegate
            {
                List<PlayerEdit> list = PlayerEdits.Load(Launcher);
                list.RemoveAll(x => x.Id == id && isStyleField(x.Field));
                PlayerEdits.Save(Launcher, list);
                Window.Toast(GameText.Name(names, id) + ": 플레이스타일·스킬 수정 해제 (지금 값은 그대로)");
                changed();
            }, null, "", new Thickness(0, 10, 8, 0));
            var buttons = Ui.Row(apply, lockBox, clear);
            panel.Children.Add(buttons);
            return Ui.Card("플레이스타일·스킬", panel,
                Ui.Text("칸마다 스타일, 오리지널 스킬, 스킬 5개(종류·레벨). 스킬 종류 0은 빈칸. 적용하면 이 선수의 다섯 칸과 '쓰는 칸'을 한꺼번에 씁니다.", "Caption"));
        }

        protected override FrameworkElement Build()
        {
            LiveState state = GameSource.Load(Launcher);
            Dictionary<int, string> names = GameText.Players(Launcher);
            string[] abilities = GameText.Abilities(Launcher);
            List<PlayerEdit> edits = PlayerEdits.Load(Launcher);

            if (state == null)
                return Ui.Page("선수", Cheats.ProfileCaption(Launcher) + "\n" + "구단 선수를 한 명씩 고치고 고정합니다.", GameSource.Picker(Window, Launcher),
                    Ui.Card("게임 데이터가 아직 없습니다",
                        Ui.Text("위에서 세이브 파일을 고르거나, 게임을 실행해 세이브를 불러오세요 (포트가 2초마다 cheats\\live\\state.bin에 구단 선수 정보를 씁니다. 안전 설정으로 실행하면 치트가 꺼져 쓰지 않습니다).", "Caption")));

            var roster = new StackPanel();
            var detail = new StackPanel();
            Action<int> show = null;
            Action drawRoster = delegate
            {
                roster.Children.Clear();
                // The club's 23 slots (empty ones shown: the numbers are the game's slots), then the youth team's 15
                // (2026-11-09, owner: "팀의 선수 리스트를 모두 불러오지를 못하는데", "몇명인지 어떤 포지션들로 몇명있는지도
                // 표시"): a count by group and by position (+0x204: 0 GK 1 LSB 2 RSB 3 CB 4 DM 5 LSM 6 RSM 7 OM 8 LWG 9 RWG 10 FW).
                Action<int, int, string, int> summary = delegate(int from, int to, string title, int slots)
                {
                    int count = 0;
                    var group = new int[4];
                    var each = new int[11];
                    for (int s = from; s < to; s++)
                    {
                        if (!LiveState.ValidPlayer(state.PlayerId(s)))
                            continue;
                        count++;
                        uint p = state.Player(s, 0x204, 4);
                        if (p > 10)
                            continue;
                        each[p]++;
                        group[p == 0 ? 0 : p <= 3 ? 1 : p <= 7 ? 2 : 3]++;
                    }
                    var head = Ui.Text(title + " " + count + "명 (" + slots + "칸)  GK " + group[0] + " · DF " + group[1] + " · MF " + group[2] + " · FW " + group[3], "H2");
                    head.Margin = new Thickness(0, from == 0 ? 0 : 10, 0, 2);
                    roster.Children.Add(head);
                    string positionsText = string.Join(" · ", Enumerable.Range(0, 11).Where(q => each[q] > 0).Select(q => Positions[q] + " " + each[q]));
                    if (positionsText.Length > 0)
                    {
                        var d = Ui.Text(positionsText, "Caption");
                        d.Margin = new Thickness(0, 0, 0, 6);
                        roster.Children.Add(d);
                    }
                };
                // 새로고침 (owner: "이 구단 선수 목록도 리프레시가 되어야해"): the page again from the game's current state.
                var reload = Ui.Button("목록 새로고침", delegate
                {
                    Window.Toast("선수 목록을 게임에서 다시 읽음");
                    Shown();
                }, null, "", new Thickness(0, 0, 0, 8));
                reload.HorizontalAlignment = HorizontalAlignment.Right;   // owner: "리스트 리프레시는 오른쪽에"
                roster.Children.Add(reload);
                summary(0, 23, "구단", 23);
                for (int slot = 0; slot < 23 + LiveState.YouthSlots; slot++)
                {
                    if (slot == 23)
                        summary(23, 23 + LiveState.YouthSlots, "유스", LiveState.YouthSlots);
                    uint id = state.PlayerId(slot);
                    if (!LiveState.ValidPlayer(id))
                    {
                        if (slot < 23)
                            roster.Children.Add(Ui.Text((slot + 1).ToString("00") + "  (빈 칸)", "Caption"));
                        continue;
                    }
                    int k = slot;
                    int count = edits.Count(e => e.Id == id && !PlayerEdits.IsMultiplier(e.Field));
                    string mult = string.Join("", edits.Where(e => e.Id == id && PlayerEdits.IsMultiplier(e.Field))
                                                       .Select(e => " " + PlayerEdits.MultiplierShort(e.Field) + "x" + e.Value));
                    string number = slot < 23 ? (slot + 1).ToString("00") : "유스 " + (slot - 22).ToString("00");
                    uint position = state.Player(slot, 0x204, 4);
                    string pos = position <= 10 ? Positions[position] : "?";
                    var button = Ui.Button(number + "  " + pos.PadRight(3) + "  " + GameText.Name(names, id) + "  ·  " + Age(state, slot) +
                                           (count > 0 ? "  ·  수정 " + count : "") + (mult.Length > 0 ? "  ·  배수" + mult : ""),
                                           delegate { show(k); }, slot == selected ? "AccentButton" : null, "");
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
                detail.Children.Add(Ui.Text(GameText.Name(names, id) + "  (선수 ID " + id + ", " + (slot < 23 ? (slot + 1) + "번 칸" : "유스 " + (slot - 22) + "번 칸") + ")", "H2"));
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

                // 선수별 배수 (owner, 2026-11-09: "이것도 선수별로 개별로도 가능하게 ... 중복이나 이런 문제가 발생할수 있는것에 대한
                // 방지 대책도"): one value per kind (PlayerEdits.Set keeps a field once), used instead of the switch's.
                var multPanel = new StackPanel();
                multPanel.Children.Add(Ui.Text("이 선수에게는 '연습' 분류의 배수 치트 대신 이 값을 씁니다 (둘을 곱하지 않음). 치트가 꺼져 있어도 적용, 1~100. 해제하면 전체 설정을 따릅니다. 구단·유스를 떠난 선수에게는 쓰이지 않습니다.", "Caption"));
                foreach (string[] m in PlayerEdits.Multipliers)
                {
                    string field = m[0], label = m[1];
                    PlayerEdit edit = mine.FirstOrDefault(e => e.Field == field);
                    var grid = new Grid { Margin = new Thickness(0, 2, 0, 2) };
                    foreach (double w in new double[] { 190, 110, 64, 58, 58 })
                        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(w) });
                    var box = new TextBox { Text = edit != null ? edit.Value.ToString() : "", Width = 58, Margin = new Thickness(0, 0, 6, 0) };
                    var apply = Ui.Button("적용", delegate
                    {
                        long value;
                        if (!long.TryParse(box.Text.Trim(), out value) || value < 1 || value > 100)
                        {
                            Window.Toast(label + ": 1~100 사이의 숫자를 넣으세요");
                            return;
                        }
                        PlayerEdits.Set(Launcher, id, field, value, true);
                        Window.Toast(GameText.Name(names, id) + " · " + label + " x" + value);
                        edits = PlayerEdits.Load(Launcher);
                        show(slot);
                    }, null, "");
                    apply.Padding = new Thickness(8, 1, 8, 1);
                    var clear = Ui.Button("해제", delegate
                    {
                        PlayerEdits.Set(Launcher, id, field, null, false);
                        Window.Toast(GameText.Name(names, id) + " · " + label + ": 해제 (전체 설정 따름)");
                        edits = PlayerEdits.Load(Launcher);
                        show(slot);
                    }, null, "");
                    clear.Padding = new Thickness(8, 1, 8, 1);
                    clear.IsEnabled = edit != null;
                    UIElement[] cells = { Ui.Text(label, "Body"), Ui.Text(edit != null ? "지금 x" + edit.Value : "전체 설정 따름", "Caption"), box, apply, clear };
                    for (int c = 0; c < cells.Length; c++)
                    {
                        Grid.SetColumn(cells[c], c);
                        var fe = cells[c] as FrameworkElement;
                        if (fe != null)
                            fe.VerticalAlignment = VerticalAlignment.Center;
                        grid.Children.Add(cells[c]);
                    }
                    multPanel.Children.Add(grid);
                }
                detail.Children.Add(Ui.Card("선수별 배수 (이 선수만)", multPanel));
                detail.Children.Add(StyleCard(slot, id, state, mine, names, () => { edits = PlayerEdits.Load(Launcher); drawRoster(); }));

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
                // This player's edits at once (2026-11-09, owner: "모든 치트에 새로고침과 전체적용 고정등의 기능들이 세심하게"):
                // 새로고침 writes the file again (the game puts the once edits in again), 전부 고정 / 전부 한 번만 switch
                // them, 전체 선수에 적용 copies them to every player of the club.
                Action<bool> lockAllEdits = delegate(bool locked)
                {
                    List<PlayerEdit> list = PlayerEdits.Load(Launcher);
                    foreach (PlayerEdit e in list.Where(e => e.Id == id))
                        e.Lock = locked;
                    PlayerEdits.Save(Launcher, list);
                    Window.Toast(GameText.Name(names, id) + (locked ? ": 수정 전부 고정" : ": 수정 전부 한 번만"));
                    edits = PlayerEdits.Load(Launcher);
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
                    Ui.Row(Ui.Button("새로고침", delegate
                           {
                               PlayerEdits.Save(Launcher, PlayerEdits.Load(Launcher));
                               Window.Toast("새로고침: 지금 게임 값을 다시 읽고, 한 번만 쓰는 수정을 게임에 다시 씀" + (Game.IsRunning ? "" : " (게임을 켜면 적용)"));
                               Shown();   // the page again with the game's current values
                           }, null, "", new Thickness(0, 8, 8, 0)),
                           Ui.Button("이 선수 수정 전부 고정", delegate { lockAllEdits(true); }, null, "", new Thickness(0, 8, 8, 0)),
                           Ui.Button("전부 한 번만", delegate { lockAllEdits(false); }, null, "", new Thickness(0, 8, 8, 0)),
                           Ui.Button("전체 선수에 적용", delegate
                           {
                               List<PlayerEdit> list = PlayerEdits.Load(Launcher);
                               List<PlayerEdit> source = list.Where(e => e.Id == id).ToList();
                               int players = 0;
                               for (int s = 0; s < 23; s++)
                               {
                                   uint other = state.PlayerId(s);
                                   if (!LiveState.ValidPlayer(other) || other == id)
                                       continue;
                                   players++;
                                   foreach (PlayerEdit e in source)
                                   {
                                       list.RemoveAll(x => x.Id == other && x.Field == e.Field);
                                       list.Add(new PlayerEdit { Id = other, Field = e.Field, Value = e.Value, Lock = e.Lock });
                                   }
                               }
                               PlayerEdits.Save(Launcher, list);
                               Window.Toast("이 선수의 수정 " + source.Count + "개를 선수 " + players + "명에게 적용");
                               edits = PlayerEdits.Load(Launcher);
                           }, null, "", new Thickness(0, 8, 0, 0))),
                    Ui.Text("능력을 바꾸면 그 능력의 경험치도 게임 표의 값으로 맞춥니다 (게임의 능력 설정 함수와 같은 방식). 게임 최대는 능력·한계 100.", "Caption")));
            };

            if (selected < 0)   // the first player of the club until one is picked
                for (int slot = 0; slot < 23 && selected < 0; slot++)
                    if (LiveState.ValidPlayer(state.PlayerId(slot)))
                        selected = slot;
            drawRoster();
            if (selected >= 0 && selected < 23 + LiveState.YouthSlots && LiveState.ValidPlayer(state.PlayerId(selected)))
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
            return Ui.Page("선수", Cheats.ProfileCaption(Launcher) + "\n" + "'선수 전원' 치트를 선수 한 명씩: 값 적용과 고정. 선수 ID로 기억하므로 선수가 구단을 떠나면 쓰지 않고, GM이 구단을 옮기면 옮긴 구단 선수만 보입니다.",
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
            return Path.Combine(Cheats.ProfileDir(launcher), "lists.txt");
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
            Save(launcher, edits);
        }

        // Writing the file again also makes the game put the once picks in again (새로고침).
        public static void Save(Launcher launcher, List<ListEdit> edits)
        {
            Directory.CreateDirectory(Cheats.ProfileDir(launcher));
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
            // The whole list (2026-11-09, owner: "목록은 새로고침이 가능하게", "전체적용 고정"): 새로고침 puts the once picks
            // in again (after the game made a new list), 전부 고정 / 전부 한 번만 switch every pick, 전부 해제 removes them.
            Action<bool> lockAll = delegate(bool locked)
            {
                List<ListEdit> all = ListEdits.Load(Launcher);
                foreach (ListEdit e in all.Where(e => e.List == list))
                    e.Lock = locked;
                ListEdits.Save(Launcher, all);
                Window.Toast(title + (locked ? ": 전부 고정" : ": 전부 한 번만"));
                Shown();
            };
            var bulk = Ui.Row(
                Ui.Button("새로고침", delegate
                {
                    ListEdits.Save(Launcher, ListEdits.Load(Launcher));
                    Window.Toast(title + ": 새로고침 (고른 이름을 게임에 다시 넣음)" + (Game.IsRunning ? "" : " · 게임을 켜면 적용"));
                }, null, "", new Thickness(0, 0, 8, 0)),
                Ui.Button("전부 고정", delegate { lockAll(true); }, null, "", new Thickness(0, 0, 8, 0)),
                Ui.Button("전부 한 번만", delegate { lockAll(false); }, null, "", new Thickness(0, 0, 8, 0)),
                Ui.Button("전부 해제", delegate
                {
                    List<ListEdit> all = ListEdits.Load(Launcher);
                    all.RemoveAll(e => e.List == list);
                    ListEdits.Save(Launcher, all);
                    Window.Toast(title + ": 전부 해제 (게임이 다음에 목록을 만들 때 원래대로)");
                    Shown();
                }, null, ""));
            bulk.Margin = new Thickness(0, 0, 0, 8);
            panel.Children.Insert(1, bulk);
            return Ui.Card(title, panel, Ui.Text(note, "Caption"));
        }

        protected override FrameworkElement Build()
        {
            LiveState state = GameSource.Load(Launcher);
            return Ui.Page("목록", Cheats.ProfileCaption(Launcher) + "\n" + "게임의 후보 목록을 게임 속 이름에서 골라 바꿉니다. 고정을 켜고 적용하면 게임이 목록을 다시 만들어도 1초에 10번 다시 쓰고, 끄면 한 번만 씁니다.",
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
