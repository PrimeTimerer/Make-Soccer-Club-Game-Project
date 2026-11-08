// Logs page; tools page: desktop and Steam shortcuts, folders.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using Microsoft.Win32;

namespace SakatsukuLauncher
{
    public class LogPage : PageBase
    {
        string selected;

        public LogPage(MainWindow window) : base(window)
        {
        }

        static string Status(LogInfo log)
        {
            if (log.ExitCode == null)
                return "끝을 모름 (실행 중이었거나 런처가 먼저 닫힘)";
            return (log.ExitCode == "0" ? "정상 종료" : "오류로 끝남 (코드 " + log.ExitCode + ")") + (log.PlayTime != null ? " · " + log.PlayTime : "") + (log.Safe ? " · 안전 설정" : "");
        }

        protected override FrameworkElement Build()
        {
            List<LogInfo> logs = Logs.List(Launcher.LogsDir);
            if (selected == null || !logs.Any(l => l.Path == selected))
                selected = logs.Count > 0 ? logs[0].Path : null;
            var list = new ListBox();
            if (logs.Count == 0)
                list.Items.Add(new ListBoxItem { Content = Ui.Text("아직 실행 기록이 없습니다.", "Caption"), IsEnabled = false });
            foreach (LogInfo log in logs)
            {
                var item = new StackPanel();
                var when = Ui.Text(log.Started.ToString("yyyy-MM-dd HH:mm:ss"), "Body");
                when.FontWeight = FontWeights.SemiBold;
                item.Children.Add(when);
                var status = Ui.Text(Status(log), "Caption");
                if (log.ExitCode != null && log.ExitCode != "0")
                    status.Foreground = Ui.Brush("Bad");
                item.Children.Add(status);
                var listItem = new ListBoxItem { Content = item, Tag = log.Path };
                list.Items.Add(listItem);
                if (log.Path == selected)
                    list.SelectedItem = listItem;
            }
            list.SelectionChanged += delegate
            {
                var item = list.SelectedItem as ListBoxItem;
                if (item != null && (string)item.Tag != selected)
                {
                    selected = (string)item.Tag;
                    Window.Refresh();
                }
            };

            var right = new DockPanel();
            LogInfo current = logs.FirstOrDefault(l => l.Path == selected);
            string text = selected != null ? Logs.Tail(selected, 4000) : "아직 로그가 없습니다. 런처로 게임을 실행하면 여기에 기록됩니다.";
            if (current != null && current.ExitCode != null && current.ExitCode != "0")
            {
                Diagnosis d = Diagnosis.Of(int.Parse(current.ExitCode), Logs.Tail(selected, 300));
                if (d != null)
                {
                    var card = Ui.Card(null, Ui.Row(Ui.Icon("", 20, Ui.Brush("Bad")), new TextBlock { Text = "  " + d.Title, FontSize = 15, FontWeight = FontWeights.SemiBold, Foreground = Ui.Brush("Text") }),
                                       Ui.Text(d.Detail, "Caption"), Ui.Text(d.Advice, "Body"));
                    card.BorderBrush = Ui.Brush("Bad");
                    DockPanel.SetDock(card, Dock.Top);
                    right.Children.Add(card);
                }
            }
            var viewer = new TextBox
            {
                Text = text, IsReadOnly = true, FontFamily = Ui.Res<FontFamily>("MonoFont"), FontSize = 12, TextWrapping = TextWrapping.NoWrap,
                AcceptsReturn = true, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
                VerticalContentAlignment = VerticalAlignment.Top,
            };
            viewer.Loaded += delegate { viewer.ScrollToEnd(); };
            var buttons = Ui.Row(
                Ui.Button("복사", delegate
                {
                    Clipboard.SetText(viewer.Text);
                    Window.Toast("로그를 복사했습니다");
                }, null, "", new Thickness(0, 0, 8, 0)),
                Ui.Button("메모장으로 열기", delegate
                {
                    if (selected != null)
                        Process.Start("notepad.exe", "\"" + selected + "\"");
                }, null, "", new Thickness(0, 0, 8, 0)),
                Ui.Button("폴더 열기", delegate { Ui.OpenFolder(Launcher.LogsDir); }, null, ""));
            buttons.Margin = new Thickness(0, 12, 0, 0);
            DockPanel.SetDock(buttons, Dock.Bottom);
            right.Children.Add(buttons);
            right.Children.Add(viewer);

            var body = new Grid { Margin = new Thickness(16, 8, 16, 24) };
            body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            body.RowDefinitions.Add(new RowDefinition());
            body.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(300) });
            body.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(16) });
            body.ColumnDefinitions.Add(new ColumnDefinition());
            var head = new StackPanel { Margin = new Thickness(0, 0, 0, 20) };
            head.Children.Add(Ui.Text("로그", "H1"));
            var sub = Ui.Text("런처로 실행한 게임이 남긴 기록 (최근 20번, logs\\launcher 폴더). 오류로 끝나면 원인을 쉬운 말로 알려 줍니다.", "Caption");
            sub.Margin = new Thickness(0, 4, 0, 0);
            head.Children.Add(sub);
            Grid.SetColumnSpan(head, 3);
            body.Children.Add(head);
            Grid.SetRow(list, 1);
            body.Children.Add(list);
            Grid.SetRow(right, 1);
            Grid.SetColumn(right, 2);
            body.Children.Add(right);
            return body;
        }
    }

    public class ToolsPage : PageBase
    {
        public ToolsPage(MainWindow window) : base(window)
        {
        }

        protected override FrameworkElement Build()
        {
            string steam = SteamShortcut.SteamPath();
            return Ui.Page("도구", "바로가기와 폴더.",
                Ui.Card("바로가기",
                    Ui.Setting("바탕화면 바로가기", "더블클릭하면 런처 창 없이 바로 게임이 시작됩니다 (--play). 오류로 끝날 때만 런처 창이 뜹니다.", Ui.Button("만들기", delegate
                    {
                        try
                        {
                            string path = Shortcuts.Desktop(Launcher);
                            Window.Toast("바탕화면에 만들었습니다: " + Path.GetFileName(path));
                        }
                        catch (Exception error)
                        {
                            Window.Error("바로가기를 만들지 못했습니다", error);
                        }
                    }, null, "")),
                    Ui.Setting("Steam에 추가", steam == null ? "Steam이 설치되어 있지 않습니다." : "비Steam 게임으로 추가합니다 (--play, 홈에서 고른 표지 이미지를 Steam 그림으로). Steam을 끈 상태에서 눌러 주세요.",
                               Ui.Button("추가", delegate
                               {
                                   if (SteamShortcut.SteamRunning)
                                   {
                                       Window.ShowProblem("Steam이 켜져 있습니다. Steam을 완전히 종료한 다음 다시 눌러 주세요 (켜져 있으면 Steam이 바로가기 파일을 덮어씁니다).");
                                       return;
                                   }
                                   try
                                   {
                                       int users = SteamShortcut.Add(Launcher);
                                       Window.Dialog("", Ui.Brush("Good"), "Steam에 추가했습니다", "Steam 사용자 " + users + "명의 라이브러리에 추가했습니다. Steam을 켜면 \"" + SteamShortcut.AppName + "\"이 보입니다.", new DialogAction("확인", null, true));
                                   }
                                   catch (Exception error)
                                   {
                                       Window.Error("Steam에 추가하지 못했습니다", error);
                                   }
                               }, null, ""))),
                Ui.Card("폴더",
                    Ui.Row(Ui.Button("포트 폴더", delegate { Ui.OpenFolder(Launcher.Dir); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button("game", delegate { Ui.OpenFolder(Launcher.GameDir); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button("세이브", delegate { Ui.OpenFolder(Launcher.SavesDir); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button("백업", delegate { Ui.OpenFolder(Launcher.BackupsDir); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button("로그", delegate { Ui.OpenFolder(Launcher.LogsDir); }, null, "", new Thickness(0, 0, 8, 0)),
                           Ui.Button("launcher.ini 열기", delegate
                           {
                               if (!File.Exists(Launcher.StatePath))
                                   Launcher.SetSetting("Fps60", Launcher.Setting("Fps60"));
                               Process.Start("notepad.exe", "\"" + Launcher.StatePath + "\"");
                           }, null, ""))),
                Ui.Card("정보",
                    Ui.Text("サカつく プロサッカークラブをつくろう！ (" + GameFiles.TitleId + ", 1.03) PC 포트의 런처입니다. 비영리 팬 프로젝트이며 SEGA와 관계없습니다.", "Body"),
                    Ui.Text("게임과 그 안의 모든 데이터·코드의 권리는 SEGA에 있습니다. 런처와 포트에는 게임 데이터가 들어 있지 않고, 본인이 가진 게임 파일로만 동작합니다.", "Caption"),
                    Ui.Text("런처의 틀은 YKTK3 PC 런처에서 가져왔습니다.", "Caption")));
        }
    }

    public static class Shortcuts
    {
        // A .lnk on the desktop: the launcher with --play.
        public static string Desktop(Launcher launcher)
        {
            string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "サカつく.lnk");
            Create(path, Path.Combine(launcher.Dir, Launcher.ExeName), "--play", launcher.Dir, "サカつく プロサッカークラブをつくろう！ (PC)");
            return path;
        }

        public static void Create(string path, string target, string arguments, string workingDirectory, string description)
        {
            Type shellType = Type.GetTypeFromProgID("WScript.Shell");
            object shell = Activator.CreateInstance(shellType);
            object link = shellType.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { path });
            Type linkType = link.GetType();
            linkType.InvokeMember("TargetPath", BindingFlags.SetProperty, null, link, new object[] { target });
            linkType.InvokeMember("Arguments", BindingFlags.SetProperty, null, link, new object[] { arguments });
            linkType.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, link, new object[] { workingDirectory });
            linkType.InvokeMember("Description", BindingFlags.SetProperty, null, link, new object[] { description });
            string icon = Path.Combine(workingDirectory, Game.Exe);
            linkType.InvokeMember("IconLocation", BindingFlags.SetProperty, null, link, new object[] { (File.Exists(icon) ? icon : target) + ",0" });
            linkType.InvokeMember("Save", BindingFlags.InvokeMethod, null, link, null);
        }
    }

    // Steam's binary shortcuts.vdf (userdata\<id>\config): a map of maps; types 0 map, 1 string, 2 int32, 8 end.
    public static class SteamShortcut
    {
        public const string AppName = "サカつく プロサッカークラブをつくろう！";

        class Raw
        {
            public byte Type;
            public byte[] Bytes;
        }

        public static string SteamPath()
        {
            try
            {
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam"))
                {
                    object path = key == null ? null : key.GetValue("SteamPath");
                    return path != null && Directory.Exists(path.ToString()) ? Path.GetFullPath(path.ToString()) : null;
                }
            }
            catch (Exception)
            {
                return null;
            }
        }

        public static bool SteamRunning
        {
            get { return Process.GetProcessesByName("steam").Length > 0; }
        }

        static string ReadString(BinaryReader reader)
        {
            var bytes = new List<byte>();
            byte b;
            while ((b = reader.ReadByte()) != 0)
                bytes.Add(b);
            return Encoding.UTF8.GetString(bytes.ToArray());
        }

        static void WriteString(BinaryWriter writer, string text)
        {
            writer.Write(Encoding.UTF8.GetBytes(text));
            writer.Write((byte)0);
        }

        public static List<KeyValuePair<string, object>> ReadMap(BinaryReader reader)
        {
            var map = new List<KeyValuePair<string, object>>();
            while (true)
            {
                byte type = reader.ReadByte();
                if (type == 8)
                    return map;
                string name = ReadString(reader);
                object value;
                switch (type)
                {
                    case 0: value = ReadMap(reader); break;
                    case 1: value = ReadString(reader); break;
                    case 2: value = reader.ReadInt32(); break;
                    case 3: value = new Raw { Type = type, Bytes = reader.ReadBytes(4) }; break;
                    case 7:
                    case 10: value = new Raw { Type = type, Bytes = reader.ReadBytes(8) }; break;
                    default: throw new InvalidDataException("shortcuts.vdf: 모르는 형식 " + type);
                }
                map.Add(new KeyValuePair<string, object>(name, value));
            }
        }

        public static void WriteMap(BinaryWriter writer, List<KeyValuePair<string, object>> map)
        {
            foreach (KeyValuePair<string, object> item in map)
            {
                var raw = item.Value as Raw;
                if (item.Value is List<KeyValuePair<string, object>>)
                {
                    writer.Write((byte)0);
                    WriteString(writer, item.Key);
                    WriteMap(writer, (List<KeyValuePair<string, object>>)item.Value);
                }
                else if (item.Value is string)
                {
                    writer.Write((byte)1);
                    WriteString(writer, item.Key);
                    WriteString(writer, (string)item.Value);
                }
                else if (item.Value is int)
                {
                    writer.Write((byte)2);
                    WriteString(writer, item.Key);
                    writer.Write((int)item.Value);
                }
                else if (raw != null)
                {
                    writer.Write(raw.Type);
                    WriteString(writer, item.Key);
                    writer.Write(raw.Bytes);
                }
            }
            writer.Write((byte)8);
        }

        static object Get(List<KeyValuePair<string, object>> map, string key)
        {
            foreach (KeyValuePair<string, object> item in map)
                if (string.Equals(item.Key, key, StringComparison.OrdinalIgnoreCase))
                    return item.Value;
            return null;
        }

        static uint Crc32(byte[] data)
        {
            uint crc = 0xFFFFFFFFu;
            foreach (byte b in data)
            {
                crc ^= b;
                for (int k = 0; k < 8; k++)
                    crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
            }
            return ~crc;
        }

        // Adds (or updates) the launcher with --play in every Steam user's shortcuts; the cover image as its artwork.
        public static int Add(Launcher launcher)
        {
            string steam = SteamPath();
            if (steam == null)
                throw new InvalidOperationException("Steam을 찾지 못했습니다.");
            string exe = "\"" + Path.Combine(launcher.Dir, Launcher.ExeName) + "\"";
            uint appId = Crc32(Encoding.UTF8.GetBytes(exe + AppName)) | 0x80000000u;
            string cover = launcher.State("CoverImage", "");
            int users = 0;
            string userdata = Path.Combine(steam, "userdata");
            if (!Directory.Exists(userdata))
                throw new InvalidOperationException("Steam에 로그인한 사용자가 없습니다 (userdata 폴더 없음).");
            foreach (string user in Directory.GetDirectories(userdata).Where(d => Path.GetFileName(d).All(char.IsDigit) && Path.GetFileName(d) != "0"))
            {
                string config = Path.Combine(user, "config");
                Directory.CreateDirectory(config);
                string file = Path.Combine(config, "shortcuts.vdf");
                List<KeyValuePair<string, object>> root;
                if (File.Exists(file))
                {
                    File.Copy(file, file + ".sakatsuku-backup", true);
                    using (var reader = new BinaryReader(File.OpenRead(file)))
                        root = ReadMap(reader);
                }
                else
                    root = new List<KeyValuePair<string, object>>();
                var shortcuts = Get(root, "shortcuts") as List<KeyValuePair<string, object>>;
                if (shortcuts == null)
                {
                    shortcuts = new List<KeyValuePair<string, object>>();
                    root.Add(new KeyValuePair<string, object>("shortcuts", shortcuts));
                }
                var entry = new List<KeyValuePair<string, object>>
                {
                    new KeyValuePair<string, object>("appid", unchecked((int)appId)),
                    new KeyValuePair<string, object>("AppName", AppName),
                    new KeyValuePair<string, object>("Exe", exe),
                    new KeyValuePair<string, object>("StartDir", "\"" + launcher.Dir + "\\\""),
                    new KeyValuePair<string, object>("icon", Path.Combine(launcher.Dir, Launcher.ExeName)),
                    new KeyValuePair<string, object>("ShortcutPath", ""),
                    new KeyValuePair<string, object>("LaunchOptions", "--play"),
                    new KeyValuePair<string, object>("IsHidden", 0),
                    new KeyValuePair<string, object>("AllowDesktopConfig", 1),
                    new KeyValuePair<string, object>("AllowOverlay", 1),
                    new KeyValuePair<string, object>("OpenVR", 0),
                    new KeyValuePair<string, object>("Devkit", 0),
                    new KeyValuePair<string, object>("DevkitGameID", ""),
                    new KeyValuePair<string, object>("DevkitOverrideAppID", 0),
                    new KeyValuePair<string, object>("LastPlayTime", 0),
                    new KeyValuePair<string, object>("FlatpakAppID", ""),
                    new KeyValuePair<string, object>("tags", new List<KeyValuePair<string, object>>()),
                };
                int index = shortcuts.FindIndex(s => s.Value is List<KeyValuePair<string, object>> &&
                                                     string.Equals(Get((List<KeyValuePair<string, object>>)s.Value, "Exe") as string, exe, StringComparison.OrdinalIgnoreCase));
                if (index >= 0)
                    shortcuts[index] = new KeyValuePair<string, object>(shortcuts[index].Key, entry);
                else
                    shortcuts.Add(new KeyValuePair<string, object>(shortcuts.Count.ToString(), entry));
                using (var writer = new BinaryWriter(File.Create(file)))
                {
                    WriteMap(writer, root);
                }
                if (cover.Length > 0 && File.Exists(cover))
                {
                    string grid = Path.Combine(config, "grid");
                    Directory.CreateDirectory(grid);
                    string ext = Path.GetExtension(cover);
                    foreach (string suffix in new[] { "p", "", "_hero" })
                        File.Copy(cover, Path.Combine(grid, appId + suffix + ext), true);
                }
                users++;
            }
            return users;
        }
    }
}
