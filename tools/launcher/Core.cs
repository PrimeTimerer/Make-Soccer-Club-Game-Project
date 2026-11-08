// Sakatsuku PC launcher (the YKTK3 launcher's frame): WPF on the .NET Framework 4 that is part of Windows, built by
// build_launcher.bat with its C# compiler (C# 5: no string interpolation, no ?.). The port has no settings file of its
// own: the launcher keeps its settings in launcher.ini and hands them to the game as environment variables when it
// starts it, the way run.bat does. It also backs up the save data, keeps logs and makes shortcuts.
//   Sakatsuku Launcher.exe          the launcher window
//   Sakatsuku Launcher.exe --play   starts the game at once (desktop and Steam shortcuts); a window only when it fails
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Markup;
using System.Windows.Threading;
using Microsoft.Win32;

namespace SakatsukuLauncher
{
    static class Program
    {
        [DllImport("user32.dll")]
        static extern bool SetProcessDPIAware();

        [STAThread]
        static int Main(string[] args)
        {
            try
            {
                SetProcessDPIAware();
            }
            catch (Exception)
            {
            }
            var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
            using (Stream theme = typeof(Program).Assembly.GetManifestResourceStream("Theme.xaml"))
                app.Resources = (ResourceDictionary)XamlReader.Load(theme);
            var launcher = new Launcher(AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\', '/'));
            int shot = Array.FindIndex(args, a => a.Equals("--shot", StringComparison.OrdinalIgnoreCase));
            if (shot >= 0 && shot + 1 < args.Length)
                return Shots.Run(app, launcher, args[shot + 1]);
            bool play = args.Any(a => a.Equals("--play", StringComparison.OrdinalIgnoreCase));
            MainWindow window = null;
            Func<MainWindow> showWindow = delegate
            {
                if (window == null)
                {
                    window = new MainWindow(launcher);
                    window.Closed += delegate { app.Shutdown(); };
                }
                window.Show();
                window.Activate();
                return window;
            };
            launcher.Runner.Finished += delegate(RunResult result)
            {
                if (window == null && result.Diagnosis == null)
                    app.Shutdown(); // --play: the game ended normally
                else
                    showWindow().GameFinished(result);
            };
            if (play)
            {
                string problem = launcher.Runner.Start(false);
                if (problem != null)
                    showWindow().ShowProblem(problem);
            }
            else
                showWindow();
            return app.Run();
        }
    }

    // --shot <dir>: every page rendered off screen to <dir>\<page>.png, then exit (to check the layout without a desktop).
    public static class Shots
    {
        public static int Run(Application app, Launcher launcher, string dir)
        {
            Directory.CreateDirectory(dir);
            var window = new MainWindow(launcher) { WindowStartupLocation = WindowStartupLocation.Manual, Left = -20000, Top = 0, ShowActivated = false, ShowInTaskbar = false };
            int shotHeight; // LAUNCHER_SHOT_HEIGHT=<px>: a taller window, to see the whole of a long page
            if (int.TryParse(Environment.GetEnvironmentVariable("LAUNCHER_SHOT_HEIGHT"), out shotHeight) && shotHeight > 0)
                window.Height = shotHeight;
            window.Show();
            string[] keys = { "home", "settings", "controllers", "saves", "cheats", "players", "lists", "logs", "tools" };
            int i = 0;
            var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(800) };
            timer.Tick += delegate
            {
                if (i > 0)
                    Save(window, Path.Combine(dir, keys[i - 1] + ".png"));
                if (i == keys.Length)
                {
                    timer.Stop();
                    app.Shutdown();
                    return;
                }
                window.Navigate(keys[i++]);
            };
            timer.Start();
            return app.Run();
        }

        static void Save(Window window, string path)
        {
            var content = (FrameworkElement)window.Content;
            int w = (int)content.ActualWidth, h = (int)content.ActualHeight;
            var visual = new System.Windows.Media.DrawingVisual();
            using (System.Windows.Media.DrawingContext dc = visual.RenderOpen())
            {
                dc.DrawRectangle(window.Background, null, new Rect(0, 0, w, h));
                dc.DrawRectangle(new System.Windows.Media.VisualBrush(content), null, new Rect(0, 0, w, h));
            }
            var bitmap = new System.Windows.Media.Imaging.RenderTargetBitmap(w, h, 96, 96, System.Windows.Media.PixelFormats.Pbgra32);
            bitmap.Render(visual);
            var encoder = new System.Windows.Media.Imaging.PngBitmapEncoder();
            encoder.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bitmap));
            using (FileStream file = File.Create(path))
                encoder.Save(file);
        }
    }

    // The port's folder (the launcher sits in it, next to run.bat) and the files the launcher reads and writes there.
    public class Launcher
    {
        public const string ExeName = "Sakatsuku Launcher.exe";
        public readonly string Dir;
        public readonly GameRunner Runner;

        public Launcher(string dir)
        {
            Dir = dir;
            Runner = new GameRunner(this);
        }

        public string StatePath { get { return Path.Combine(Dir, "launcher.ini"); } }
        public string LogsDir { get { return Path.Combine(Dir, "logs", "launcher"); } }
        public string GameDir { get { return Path.Combine(Dir, "game"); } }
        public string Eboot { get { return Path.Combine(GameDir, "EBOOT.ELF"); } }
        public string Bdvd { get { return Path.Combine(GameDir, "dev_bdvd"); } }
        public string Hdd0 { get { return Path.Combine(GameDir, "dev_hdd0"); } }
        // cellSaveData: ./gamedata/dev_hdd0/home/00000001/savedata under the game's working folder (this one).
        public string SavesDir { get { return Path.Combine(Dir, "gamedata", "dev_hdd0", "home", "00000001", "savedata"); } }
        public string BackupsDir { get { return Path.Combine(Dir, "gamedata", "backups"); } }

        // The optimized build (build_release.bat) when present, else the debug build (build.bat), as run.bat picks.
        public string Exe
        {
            get
            {
                string release = Path.Combine(Dir, "build-release", Game.Exe);
                string debug = Path.Combine(Dir, "build", Game.Exe);
                return File.Exists(release) || !File.Exists(debug) ? release : debug;
            }
        }

        // launcher.ini [Launcher]: the launcher's own state and play record; [Settings]: the game settings.
        public string State(string key, string fallback)
        {
            return new IniFile(StatePath).Get("Launcher", key, fallback);
        }

        public void SetState(string key, string value)
        {
            var ini = new IniFile(StatePath);
            ini.Set("Launcher", key, value);
            ini.Save();
        }

        public string Setting(string key)
        {
            return new IniFile(StatePath).Get("Settings", key, Settings.Default(key));
        }

        public bool On(string key)
        {
            return Setting(key) == "1";
        }

        public void SetSetting(string key, string value)
        {
            var ini = new IniFile(StatePath);
            ini.Set("Settings", key, value);
            ini.Save();
        }

        public void AddPlayTime(TimeSpan time)
        {
            long seconds;
            long.TryParse(State("PlayTimeSeconds", "0"), out seconds);
            SetState("PlayTimeSeconds", (seconds + (long)time.TotalSeconds).ToString());
            SetState("LastPlayed", DateTime.Now.ToString("yyyy-MM-dd HH:mm"));
        }
    }

    // The game settings and what each one hands to the game. Only switches the port already has are used; a value
    // the port's own default covers is not set at all, so a run with every setting at its default is run.bat's.
    public static class Settings
    {
        static readonly Dictionary<string, string> defaults = new Dictionary<string, string>
        {
            { "Fps60", "1" },           // SAKA_LOGIC60: 60 ticks a second at the original game speed
            { "PlayerMotion60", "1" },  // SAKA_ANIM60: player animation time half a step ahead between match ticks
            { "BallMotion60", "1" },    // SAKA_BALL60: the drawn ball position likewise
            { "BallHopSmooth", "1" },   // SAKA_BALL_SMOOTH: the drawn ball's hops (stands, then jumps) glided, 30 and 60 ticks
            { "VertexCache", "1" },     // LD_VTX_CACHE: the renderer's persistent vertex cache
            { "VertexCacheMB", "1024" },
            { "VSync", "1" },           // D3D12_VSYNC=0 turns it off
            { "SyncHighRefresh", "0" }, // LD_SYNC_INTERVAL=auto: present at refresh/60 on 120/180/240 Hz monitors
            { "KoreanPatch", "0" },     // PS3_USRDIR_OVERLAY=game\mods\korean: the Korean patch's files, read before the originals
            { "Mute", "0" },            // PS3_MUTE
            { "Keyboard", "1" },        // PAD_NO_KEYBOARD=1 turns the keyboard pad off
            { "AutoBackup", "1" },      // save data backup before each run
            { "BackupsToKeep", "10" },
        };

        public static string Default(string key)
        {
            string value;
            return defaults.TryGetValue(key, out value) ? value : "";
        }

        public static IEnumerable<string> Keys { get { return defaults.Keys; } }

        // The environment for one run. safe: this run only, the port as it was before the 60-tick work (30 fps,
        // no vertex cache); nothing is written.
        public static Dictionary<string, string> Environment(Launcher launcher, bool safe)
        {
            var env = new Dictionary<string, string>();
            // run.bat: /dev_bdvd -> game\dev_bdvd (original disc, read-only), /dev_hdd0 -> game\dev_hdd0 (1.03 patch).
            env["PS3_VFS_ROOT"] = launcher.Bdvd;
            env["PS3_HDD0_ROOT"] = launcher.Hdd0;
            env["RT_DISPLAY_BY_SIZE"] = "1";
            env["RSX_LIVE_DRAW"] = "1";
            bool fps60 = !safe && launcher.On("Fps60");
            env["SAKA_LOGIC60"] = fps60 ? "1" : null;
            env["SAKA_ANIM60"] = fps60 && !launcher.On("PlayerMotion60") ? "0" : null;
            env["SAKA_BALL60"] = fps60 && !launcher.On("BallMotion60") ? "0" : null;
            env["SAKA_BALL_SMOOTH"] = !safe && launcher.On("BallHopSmooth") ? null : "0";
            bool cache = !safe && launcher.On("VertexCache");
            env["LD_VTX_CACHE"] = cache ? null : "0";
            string mb = launcher.Setting("VertexCacheMB");
            env["LD_VTX_CACHE_MB"] = cache && mb != Default("VertexCacheMB") ? mb : null;
            env["D3D12_VSYNC"] = launcher.On("VSync") ? null : "0";
            env["LD_SYNC_INTERVAL"] = launcher.On("VSync") && launcher.On("SyncHighRefresh") ? "auto" : null;
            env["PS3_USRDIR_OVERLAY"] = !safe && launcher.On("KoreanPatch") && KoreanPatch.Installed(launcher) ? KoreanPatch.Dir(launcher) : null;
            env["PS3_MUTE"] = launcher.On("Mute") ? "1" : null;
            env["PAD_NO_KEYBOARD"] = launcher.On("Keyboard") ? null : "1";
            env["SAKA_CHEATS"] = safe ? "0" : null; // the cheat thread (cheats\enabled.txt)
            return env;
        }
    }

    // A settings file edited in place: comments, order and every other setting stay as they are.
    public class IniFile
    {
        public static readonly Encoding FileEncoding = new UTF8Encoding(false);
        readonly string path;
        public readonly List<string> Lines = new List<string>();
        string newline = "\r\n";

        public IniFile(string path)
        {
            this.path = path;
            if (!File.Exists(path))
                return;
            string text = File.ReadAllText(path, FileEncoding);
            newline = text.Contains("\r\n") || !text.Contains("\n") ? "\r\n" : "\n";
            Lines.AddRange(text.Split('\n').Select(line => line.TrimEnd('\r')));
            if (Lines.Count > 0 && Lines[Lines.Count - 1].Length == 0)
                Lines.RemoveAt(Lines.Count - 1); // the final newline
        }

        public static string SectionName(string line)
        {
            string t = line.Trim();
            return t.StartsWith("[") && t.EndsWith("]") ? t.Substring(1, t.Length - 2).Trim() : null;
        }

        // [start, end) of a section's lines after its header; start = -1 when there is no such section.
        public void SectionRange(string section, out int start, out int end)
        {
            start = end = -1;
            for (int i = 0; i < Lines.Count; i++)
            {
                string name = SectionName(Lines[i]);
                if (name == null)
                    continue;
                if (start >= 0)
                {
                    end = i;
                    return;
                }
                if (string.Equals(name, section, StringComparison.OrdinalIgnoreCase))
                    start = i + 1;
            }
            if (start >= 0)
                end = Lines.Count;
        }

        public static bool KeyValue(string line, out string key, out string value)
        {
            key = value = null;
            string t = line.Trim();
            int equals = t.IndexOf('=');
            if (t.Length == 0 || t.StartsWith("#") || t.StartsWith(";") || equals <= 0)
                return false;
            key = t.Substring(0, equals).Trim();
            value = t.Substring(equals + 1).Trim();
            return true;
        }

        int Find(string section, string key, out int insertAt)
        {
            int start, end;
            SectionRange(section, out start, out end);
            insertAt = start < 0 ? -1 : start;
            for (int i = start; i >= 0 && i < end; i++)
            {
                string k, v;
                if (!KeyValue(Lines[i], out k, out v))
                    continue;
                insertAt = i + 1;
                if (string.Equals(k, key, StringComparison.OrdinalIgnoreCase))
                    return i;
            }
            return -1;
        }

        public string Get(string section, string key, string fallback)
        {
            int insertAt;
            int i = Find(section, key, out insertAt);
            string k, v;
            return i >= 0 && KeyValue(Lines[i], out k, out v) ? v : fallback;
        }

        public void Set(string section, string key, string value)
        {
            int insertAt;
            int i = Find(section, key, out insertAt);
            if (i >= 0)
            {
                int equals = Lines[i].IndexOf('=');
                if (Lines[i].Substring(equals + 1).Trim() != value)
                    Lines[i] = Lines[i].Substring(0, equals + 1) + (Lines[i].Substring(equals - 1, 1) == " " ? " " : "") + value;
                return;
            }
            if (insertAt < 0)
            {
                if (Lines.Count > 0 && Lines[Lines.Count - 1].Trim().Length > 0)
                    Lines.Add("");
                Lines.Add("[" + section + "]");
                insertAt = Lines.Count;
            }
            Lines.Insert(insertAt, key + "=" + value);
        }

        public void Save()
        {
            File.WriteAllText(path, string.Join(newline, Lines) + newline, FileEncoding);
        }
    }

    // PARAM.SFO: the PS3's key/value table (titles, versions) of the disc, the patch and every save.
    public static class Sfo
    {
        public static Dictionary<string, string> Read(string path)
        {
            var map = new Dictionary<string, string>();
            try
            {
                byte[] b = File.ReadAllBytes(path);
                if (b.Length < 20 || b[0] != 0 || b[1] != 'P' || b[2] != 'S' || b[3] != 'F')
                    return map;
                int keys = BitConverter.ToInt32(b, 8), data = BitConverter.ToInt32(b, 12), count = BitConverter.ToInt32(b, 16);
                for (int i = 0; i < count && 20 + 16 * i + 16 <= b.Length; i++)
                {
                    int entry = 20 + 16 * i;
                    int keyAt = keys + BitConverter.ToUInt16(b, entry), format = BitConverter.ToUInt16(b, entry + 2);
                    int length = BitConverter.ToInt32(b, entry + 4), at = data + BitConverter.ToInt32(b, entry + 12);
                    int end = keyAt;
                    while (end < b.Length && b[end] != 0)
                        end++;
                    string key = Encoding.ASCII.GetString(b, keyAt, end - keyAt);
                    if (at < 0 || at + length > b.Length)
                        continue;
                    map[key] = format == 0x0404 ? BitConverter.ToUInt32(b, at).ToString() : Encoding.UTF8.GetString(b, at, length).TrimEnd('\0');
                }
            }
            catch (Exception)
            {
            }
            return map;
        }

        public static string Get(Dictionary<string, string> map, string key)
        {
            string value;
            return map.TryGetValue(key, out value) ? value : "";
        }
    }

    // The game files the port needs (PORT_1.03_RUNTIME.md): the 1.03 ELF, the original disc and the 1.03 patch.
    public class GameFiles
    {
        public const string TitleId = "BLJM61064";
        public bool Eboot, Disc, Patch;
        public string Title = "サカつく プロサッカークラブをつくろう！", DiscVersion = "", PatchVersion = "";

        public static GameFiles Check(Launcher launcher)
        {
            var files = new GameFiles { Eboot = File.Exists(launcher.Eboot) };
            Dictionary<string, string> disc = Sfo.Read(Path.Combine(launcher.Bdvd, "PS3_GAME", "PARAM.SFO"));
            files.Disc = Sfo.Get(disc, "TITLE_ID") == TitleId && Directory.Exists(Path.Combine(launcher.Bdvd, "PS3_GAME", "USRDIR"));
            files.DiscVersion = Sfo.Get(disc, "APP_VER");
            if (Sfo.Get(disc, "TITLE").Length > 0)
                files.Title = Sfo.Get(disc, "TITLE");
            Dictionary<string, string> patch = Sfo.Read(Path.Combine(launcher.Hdd0, "game", TitleId, "PARAM.SFO"));
            files.PatchVersion = Sfo.Get(patch, "APP_VER");
            files.Patch = Sfo.Get(patch, "TITLE_ID") == TitleId && files.PatchVersion == "01.03";
            return files;
        }

        public bool Ready { get { return Eboot && Disc && Patch; } }

        public string Cover(Launcher launcher, string name)
        {
            string path = Path.Combine(launcher.Bdvd, "PS3_GAME", name);
            return File.Exists(path) ? path : null;
        }
    }

    public static class Game
    {
        public const string Exe = "SakatsukuRecomp.exe";

        public static bool IsRunning
        {
            get { return Process.GetProcessesByName(Path.GetFileNameWithoutExtension(Exe)).Length > 0; }
        }

        // Why the game cannot start, or null.
        public static string Problem(Launcher launcher)
        {
            if (!File.Exists(launcher.Exe))
                return "build-release\\" + Exe + "가 없습니다. build_release.bat로 게임을 빌드하세요. 런처는 run.bat와 같은 폴더에 있어야 합니다.";
            if (IsRunning)
                return "게임이 이미 실행 중입니다.";
            GameFiles files = GameFiles.Check(launcher);
            if (!files.Eboot)
                return "game\\EBOOT.ELF(1.03)가 없습니다. PORT_1.03_RUNTIME.md대로 1.03 ELF를 넣어 주세요.";
            if (!files.Disc)
                return "game\\dev_bdvd에 원본 디스크(" + GameFiles.TitleId + ")의 PS3_GAME 폴더가 없습니다.";
            if (!files.Patch)
                return "game\\dev_hdd0\\game\\" + GameFiles.TitleId + "에 1.03 패치가 없습니다" + (files.PatchVersion.Length > 0 ? " (지금 " + files.PatchVersion + ")" : "") + ".";
            return null;
        }
    }

    public class RunResult
    {
        public int ExitCode;
        public TimeSpan Ran;
        public string LogPath;
        public bool SafeMode;
        public Diagnosis Diagnosis;
    }

    // Starts the game with its console hidden, writes what it prints to logs\launcher\saka-<time>.log and explains a
    // failure.
    public class GameRunner
    {
        readonly Launcher launcher;
        Process process;
        StreamWriter log;
        string logPath;
        DateTime started;
        bool safeMode;
        DispatcherTimer flush;
        public event Action<RunResult> Finished;

        public GameRunner(Launcher launcher)
        {
            this.launcher = launcher;
        }

        public bool Running { get { return process != null; } }

        public string Start(bool safe)
        {
            if (process != null)
                return "게임이 이미 실행 중입니다.";
            string problem = Game.Problem(launcher);
            if (problem != null)
                return problem;
            if (launcher.On("AutoBackup"))
            {
                try
                {
                    SaveStore.Backup(launcher, "auto");
                }
                catch (Exception)
                {
                }
            }
            Directory.CreateDirectory(launcher.LogsDir);
            Logs.Trim(launcher.LogsDir, 20);
            logPath = Path.Combine(launcher.LogsDir, "saka-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".log");
            string exe = launcher.Exe;
            var info = new ProcessStartInfo(exe, "\"" + launcher.Eboot + "\"")
            {
                WorkingDirectory = launcher.Dir, UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
            };
            var writer = new StreamWriter(logPath, false, new UTF8Encoding(false));
            writer.WriteLine("# Sakatsuku launcher log");
            writer.WriteLine("# started: " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + (safe ? " (safe settings)" : ""));
            writer.WriteLine("# exe: " + exe);
            foreach (KeyValuePair<string, string> pair in Settings.Environment(launcher, safe))
            {
                if (pair.Value == null)
                    info.EnvironmentVariables.Remove(pair.Key);
                else
                {
                    info.EnvironmentVariables[pair.Key] = pair.Value;
                    writer.WriteLine("# " + pair.Key + "=" + pair.Value);
                }
            }
            var p = new Process { StartInfo = info, EnableRaisingEvents = true };
            DataReceivedEventHandler write = delegate(object sender, DataReceivedEventArgs e)
            {
                if (e.Data != null)
                    lock (writer)
                        writer.WriteLine(e.Data);
            };
            p.OutputDataReceived += write;
            p.ErrorDataReceived += write;
            p.Exited += delegate { OnExited(p, writer); };
            try
            {
                p.Start();
            }
            catch (Exception error)
            {
                writer.Dispose();
                return "게임을 시작하지 못했습니다: " + error.Message;
            }
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            process = p;
            log = writer;
            started = DateTime.Now;
            safeMode = safe;
            flush = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
            flush.Tick += delegate
            {
                lock (writer)
                    writer.Flush();
            };
            flush.Start();
            return null;
        }

        void OnExited(Process p, StreamWriter writer)
        {
            p.WaitForExit(); // the rest of the output
            int code = p.ExitCode;
            TimeSpan ran = DateTime.Now - started;
            lock (writer)
            {
                writer.WriteLine("# exit code: " + code + " (0x" + code.ToString("X8") + ")");
                writer.WriteLine("# play time: " + ran.ToString(@"hh\:mm\:ss"));
                writer.Dispose();
            }
            var result = new RunResult { ExitCode = code, Ran = ran, LogPath = logPath, SafeMode = safeMode };
            result.Diagnosis = Diagnosis.Of(code, Logs.Tail(logPath, 300));
            Application.Current.Dispatcher.BeginInvoke((Action)delegate
            {
                flush.Stop();
                log = null;
                process = null;
                p.Dispose();
                launcher.AddPlayTime(ran);
                if (Finished != null)
                    Finished(result);
            });
        }
    }

    // A failed run in plain words, from the end of its log and the exit code.
    public class Diagnosis
    {
        public string Title, Detail, Advice;
        public bool SafeModeHelps;

        static Diagnosis Make(string title, string detail, string advice, bool safeModeHelps)
        {
            return new Diagnosis { Title = title, Detail = detail, Advice = advice, SafeModeHelps = safeModeHelps };
        }

        public static Diagnosis Of(int code, string log)
        {
            string text = log ?? "";
            uint status = unchecked((uint)code);
            if (code == 0)
                return null;
            if (Regex.IsMatch(text, "cannot open ELF|failed to load ELF|ELF load failed|not an ELF", RegexOptions.IgnoreCase))
                return Make("EBOOT.ELF를 읽지 못했습니다", LastError(text, "game\\EBOOT.ELF가 손상되었거나 1.03 ELF가 아닙니다."),
                            "PORT_1.03_RUNTIME.md대로 1.03 ELF를 다시 넣어 주세요.", false);
            if (Regex.IsMatch(text, "DXGI_ERROR|DEVICE_REMOVED|DEVICE_HUNG|device removed|D3D12CreateDevice", RegexOptions.IgnoreCase))
                return Make("그래픽카드 오류로 끝났습니다", LastError(text, "그래픽 드라이버가 응답하지 않았습니다."),
                            "그래픽 드라이버를 최신으로 업데이트하고, 안전 설정(원래 30fps, 정점 캐시 끔)으로 실행해 보세요.", true);
            if (text.Contains("bad_alloc") || status == 0xC0000017u)
                return Make("메모리가 부족했습니다", "게임이 메모리를 더 받지 못했습니다.",
                            "다른 프로그램을 닫고, 설정에서 정점 캐시 용량을 줄이거나 꺼 보세요.", true);
            if (status == 0xC0000135u || status == 0xC0000139u || status == 0xC000007Bu)
                return Make("필요한 DLL이 없습니다", "게임 폴더의 DLL(SDL2.dll 등)을 찾지 못했습니다 (0x" + status.ToString("X8") + ").",
                            "build_release.bat로 다시 빌드해 build-release 폴더에 DLL이 있는지 확인해 주세요.", false);
            if (status == 0xFFFFFFFFu || code == 1)
                return Make("게임이 강제로 종료되었습니다", "작업 관리자 등 밖에서 게임을 끝냈습니다 (종료 코드 " + code + ").",
                            "직접 끄신 것이 아니라면 로그를 확인해 주세요.", false);
            if (status >= 0xC0000000u)
                return Make("게임이 오류로 멈췄습니다", "종료 코드 0x" + status.ToString("X8") + (status == 0xC0000005u ? " (메모리 접근 오류)" : ""),
                            "안전 설정(원래 30fps, 정점 캐시 끔)으로 다시 실행해 보세요. 계속되면 로그를 함께 알려 주세요.", true);
            return Make("게임이 오류로 끝났습니다", LastError(text, "종료 코드 " + code),
                        "로그에서 마지막 오류를 확인해 주세요. 안전 설정으로 다시 실행해 볼 수 있습니다.", true);
        }

        static string LastError(string text, string fallback)
        {
            string line = text.Split('\n').Select(l => l.Trim()).LastOrDefault(l => Regex.IsMatch(l, "error|fatal|fail", RegexOptions.IgnoreCase) && !l.StartsWith("#"));
            return line ?? fallback;
        }
    }

    public class LogInfo
    {
        public string Path;
        public DateTime Started;
        public string ExitCode; // null while running or when the launcher was closed during the run
        public string PlayTime;
        public bool Safe;
    }

    public static class Logs
    {
        public const string Pattern = "saka-*.log";

        public static List<LogInfo> List(string dir)
        {
            var logs = new List<LogInfo>();
            if (!Directory.Exists(dir))
                return logs;
            foreach (string file in Directory.GetFiles(dir, Pattern).OrderByDescending(f => f, StringComparer.Ordinal))
            {
                var info = new LogInfo { Path = file, Started = File.GetCreationTime(file) };
                try
                {
                    string head = Head(file);
                    Match started = Regex.Match(head, @"# started: ([0-9-]+ [0-9:]+)");
                    if (started.Success)
                        DateTime.TryParse(started.Groups[1].Value, out info.Started);
                    info.Safe = head.Contains("(safe settings)");
                    string tail = Tail(file, 4);
                    Match exit = Regex.Match(tail, @"# exit code: (-?[0-9]+)");
                    if (exit.Success)
                        info.ExitCode = exit.Groups[1].Value;
                    Match time = Regex.Match(tail, @"# play time: ([0-9:]+)");
                    if (time.Success)
                        info.PlayTime = time.Groups[1].Value;
                }
                catch (Exception)
                {
                }
                logs.Add(info);
            }
            return logs;
        }

        static string Head(string path)
        {
            using (var reader = new StreamReader(new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)))
            {
                var buffer = new char[400];
                int n = reader.Read(buffer, 0, buffer.Length);
                return new string(buffer, 0, n);
            }
        }

        // The last lines of a log (it may be large: only its end is read).
        public static string Tail(string path, int lines)
        {
            try
            {
                using (var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    long start = Math.Max(0, file.Length - 256 * 1024);
                    file.Position = start;
                    string text = new StreamReader(file, Encoding.UTF8).ReadToEnd();
                    string[] all = text.Split('\n');
                    return string.Join("\n", all.Skip(Math.Max(start > 0 ? 1 : 0, all.Length - lines)));
                }
            }
            catch (Exception)
            {
                return "";
            }
        }

        public static void Trim(string dir, int keep)
        {
            foreach (string old in Directory.GetFiles(dir, Pattern).OrderByDescending(f => f, StringComparer.Ordinal).Skip(keep))
            {
                try
                {
                    File.Delete(old);
                }
                catch (Exception)
                {
                }
            }
        }
    }

    public class SaveInfo
    {
        public string Dir, Name, Title, SubTitle, Detail, Icon;
        public DateTime Changed;
        public long Size;
    }

    // The save data (gamedata\dev_hdd0\home\00000001\savedata\BLJM61064-*) and its backups: one zip of every save of
    // the game, gamedata\backups\savedata-<time>-<why>.zip.
    public static class SaveStore
    {
        public static List<SaveInfo> List(Launcher launcher)
        {
            var saves = new List<SaveInfo>();
            if (!Directory.Exists(launcher.SavesDir))
                return saves;
            foreach (string dir in Directory.GetDirectories(launcher.SavesDir, GameFiles.TitleId + "*").OrderBy(d => d, StringComparer.OrdinalIgnoreCase))
            {
                Dictionary<string, string> sfo = Sfo.Read(Path.Combine(dir, "PARAM.SFO"));
                var files = new DirectoryInfo(dir).GetFiles();
                string icon = Path.Combine(dir, "ICON0.PNG");
                saves.Add(new SaveInfo
                {
                    Dir = dir, Name = Path.GetFileName(dir), Title = Sfo.Get(sfo, "TITLE"), SubTitle = Sfo.Get(sfo, "SUB_TITLE"),
                    Detail = Sfo.Get(sfo, "DETAIL"), Icon = File.Exists(icon) ? icon : null,
                    Changed = files.Length > 0 ? files.Max(f => f.LastWriteTime) : Directory.GetLastWriteTime(dir),
                    Size = files.Sum(f => f.Length),
                });
            }
            return saves;
        }

        public static List<string> Backups(Launcher launcher)
        {
            if (!Directory.Exists(launcher.BackupsDir))
                return new List<string>();
            return Directory.GetFiles(launcher.BackupsDir, "savedata-*.zip").OrderByDescending(f => f, StringComparer.Ordinal).ToList();
        }

        // A zip of every save of the game; the oldest beyond the kept number are removed. null when there is nothing.
        public static string Backup(Launcher launcher, string why)
        {
            List<SaveInfo> saves = List(launcher);
            if (saves.Count == 0)
                return null;
            Directory.CreateDirectory(launcher.BackupsDir);
            string target = Path.Combine(launcher.BackupsDir, "savedata-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-" + why + ".zip");
            using (ZipArchive zip = ZipFile.Open(target, ZipArchiveMode.Create))
                foreach (SaveInfo save in saves)
                    foreach (string file in Directory.GetFiles(save.Dir, "*", SearchOption.AllDirectories))
                        zip.CreateEntryFromFile(file, save.Name + "/" + file.Substring(save.Dir.Length + 1).Replace('\\', '/'));
            int keep;
            if (!int.TryParse(launcher.Setting("BackupsToKeep"), out keep) || keep < 1)
                keep = 10;
            foreach (string old in Backups(launcher).Skip(keep))
                File.Delete(old);
            return target;
        }

        // Puts a backup back: the saves there now are backed up first ("before-restore"), then replaced.
        public static void Restore(Launcher launcher, string backup)
        {
            if (Game.IsRunning)
                throw new InvalidOperationException("게임이 실행 중입니다. 게임을 끈 다음 복원해 주세요.");
            Backup(launcher, "before-restore");
            string temp = Path.Combine(launcher.BackupsDir, "restore-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
            ZipFile.ExtractToDirectory(backup, temp);
            Directory.CreateDirectory(launcher.SavesDir);
            foreach (SaveInfo save in List(launcher))
                Directory.Delete(save.Dir, true);
            foreach (string dir in Directory.GetDirectories(temp))
                Directory.Move(dir, Path.Combine(launcher.SavesDir, Path.GetFileName(dir)));
            Directory.Delete(temp, true);
        }

        public static string Describe(string backup)
        {
            Match m = Regex.Match(Path.GetFileNameWithoutExtension(backup), @"savedata-(\d{8})-(\d{6})-(.+)");
            if (!m.Success)
                return Path.GetFileName(backup);
            DateTime when;
            string why = m.Groups[3].Value == "auto" ? "실행 전 자동" : m.Groups[3].Value == "before-restore" ? "복원 전" : m.Groups[3].Value == "manual" ? "직접" : m.Groups[3].Value;
            return DateTime.TryParseExact(m.Groups[1].Value + m.Groups[2].Value, "yyyyMMddHHmmss", null, System.Globalization.DateTimeStyles.None, out when)
                ? when.ToString("yyyy-MM-dd HH:mm:ss") + " · " + why : Path.GetFileName(backup);
        }
    }

    // What the PC has (registry and Win32 only: quick, nothing installed).
    public class PcInfo
    {
        public string Cpu = "알 수 없음";
        public int Threads = Environment.ProcessorCount;
        public long Ram;
        public string Gpu = "알 수 없음";
        public long Vram;
        public int ScreenWidth, ScreenHeight;

        [StructLayout(LayoutKind.Sequential)]
        class MemoryStatus
        {
            public uint Length = 64;
            public uint Load;
            public ulong TotalPhys, AvailPhys, TotalPageFile, AvailPageFile, TotalVirtual, AvailVirtual, AvailExtendedVirtual;
        }

        [DllImport("kernel32.dll")]
        static extern bool GlobalMemoryStatusEx([In, Out] MemoryStatus status);

        [DllImport("user32.dll")]
        static extern int GetSystemMetrics(int index);

        public static PcInfo Read()
        {
            var pc = new PcInfo();
            try
            {
                using (RegistryKey cpu = Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\CentralProcessor\0"))
                    if (cpu != null && cpu.GetValue("ProcessorNameString") != null)
                        pc.Cpu = Regex.Replace(cpu.GetValue("ProcessorNameString").ToString(), @"\s+", " ").Trim();
            }
            catch (Exception)
            {
            }
            var memory = new MemoryStatus();
            if (GlobalMemoryStatusEx(memory))
                pc.Ram = (long)memory.TotalPhys;
            pc.ScreenWidth = GetSystemMetrics(0);
            pc.ScreenHeight = GetSystemMetrics(1);
            // Display adapters: the one with the most video memory (the dedicated card on laptops with two).
            try
            {
                using (RegistryKey adapters = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"))
                {
                    foreach (string name in adapters.GetSubKeyNames())
                    {
                        try
                        {
                            using (RegistryKey adapter = adapters.OpenSubKey(name))
                            {
                                object desc = adapter == null ? null : adapter.GetValue("DriverDesc");
                                if (desc == null || desc.ToString().Contains("Basic") || desc.ToString().Contains("Remote"))
                                    continue;
                                long vram = 0;
                                object q = adapter.GetValue("HardwareInformation.qwMemorySize");
                                object d = adapter.GetValue("HardwareInformation.MemorySize");
                                if (q is long)
                                    vram = (long)q;
                                else if (d is int)
                                    vram = (uint)(int)d;
                                else if (d is byte[] && ((byte[])d).Length >= 4)
                                    vram = BitConverter.ToUInt32((byte[])d, 0);
                                if (vram > pc.Vram || pc.Gpu == "알 수 없음")
                                {
                                    pc.Gpu = desc.ToString();
                                    pc.Vram = vram;
                                }
                            }
                        }
                        catch (Exception)
                        {
                        }
                    }
                }
            }
            catch (Exception)
            {
            }
            return pc;
        }

        public static string Size(long bytes)
        {
            if (bytes <= 0)
                return "?";
            if (bytes < 1L << 20)
                return Math.Max(1, bytes >> 10) + " KB";
            return bytes >= 1L << 30 ? (bytes / (double)(1L << 30)).ToString("0.#") + " GB" : (bytes >> 20) + " MB";
        }
    }
}
