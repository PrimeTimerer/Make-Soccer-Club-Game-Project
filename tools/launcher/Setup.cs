// 게임 파일 page: makes the files the port needs from the player's own disc, 1.03 update and PS3 firmware.
// The encrypted files (the update's EBOOT.BIN, the firmware's libsre.sprx / libspurs_jq.sprx) are decrypted by RPCS3
// (rpcs3.exe --decrypt), which the player installs and sets up as usual; the launcher never holds a Sony key.
// Every result is checked against the hashes of the files the port was built from (2026-11-16: RPCS3's --decrypt
// output is byte for byte the port's own game\EBOOT.ELF and firmware modules).
//   game\EBOOT.ELF                            the 1.03 EBOOT, decrypted
//   game\firmware\<module>.prx / _image.bin   the firmware modules, decrypted, and relocated as the exe loads them
//   game\dev_hdd0\game\BLJM61064               a copy of the installed 1.03 update (the game writes here)
//   the disc: used where it is (launcher.ini [Setup] DiscDir), or copied to game\dev_bdvd (from a folder or an ISO)
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Controls;

namespace SakatsukuLauncher
{
    // Where RPCS3 keeps the game, the update and the firmware.
    public static class Rpcs3
    {
        public const string Download = "https://rpcs3.net/download";

        static bool IsRpcs3(string dir)
        {
            return !string.IsNullOrEmpty(dir) && File.Exists(Path.Combine(dir, "rpcs3.exe"));
        }

        // The folder saved in launcher.ini, a running RPCS3, or a search of the usual places (a few levels deep).
        public static string Find(Launcher launcher)
        {
            string saved = launcher.SetupValue("Rpcs3");
            if (IsRpcs3(saved))
                return saved;
            try
            {
                foreach (Process p in Process.GetProcessesByName("rpcs3"))
                {
                    string dir = Path.GetDirectoryName(p.MainModule.FileName);
                    if (IsRpcs3(dir))
                        return dir;
                }
            }
            catch (Exception)
            {
            }
            string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            var roots = new List<string>
            {
                Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), Path.Combine(home, "Downloads"),
                Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), home,
                Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
                Path.GetDirectoryName(launcher.Dir) ?? launcher.Dir,
            };
            foreach (DriveInfo drive in DriveInfo.GetDrives())
                if (drive.DriveType == DriveType.Fixed && drive.IsReady)
                    roots.Add(drive.RootDirectory.FullName);
            // Every RPCS3 found; the one with the most of this game wins (a PC may have several, e.g. one per game).
            var found = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (string root in roots.Distinct(StringComparer.OrdinalIgnoreCase))
                Search(root, 4, found);
            string best = null;
            int bestScore = -1;
            foreach (string dir in found)
            {
                SetupState s = Setup.Detect(launcher, dir);
                int score = (s.UpdateOk ? 4 : 0) + (s.DiscOk ? 2 : 0) + (s.FirmwareOk ? 1 : 0);
                if (score > bestScore)
                {
                    best = dir;
                    bestScore = score;
                }
            }
            return best;
        }

        static void Search(string dir, int depth, HashSet<string> found)
        {
            if (IsRpcs3(dir))
            {
                found.Add(dir);
                return;
            }
            if (depth == 0 || found.Count >= 16)
                return;
            string[] subs;
            try
            {
                subs = Directory.GetDirectories(dir);
            }
            catch (Exception)
            {
                return;
            }
            foreach (string sub in subs)
            {
                string name = Path.GetFileName(sub);
                if (name.StartsWith(".") || name.StartsWith("$") || name.Equals("Windows", StringComparison.OrdinalIgnoreCase) ||
                    name.Equals("AppData", StringComparison.OrdinalIgnoreCase) || name.Equals("node_modules", StringComparison.OrdinalIgnoreCase) ||
                    name.StartsWith("dev_", StringComparison.OrdinalIgnoreCase))
                    continue;
                Search(sub, depth - 1, found);
            }
        }

        // config\vfs.yml "/dev_hdd0/: $(EmulatorDir)dev_hdd0/" (RPCS3's default when the file or line is missing).
        public static string Device(string rpcs3, string device)
        {
            string path = null;
            foreach (string file in new[] { Path.Combine(rpcs3, "config", "vfs.yml"), Path.Combine(rpcs3, "vfs.yml") })
            {
                string value = YamlValue(file, "/" + device + "/");
                if (value != null)
                {
                    path = value;
                    break;
                }
            }
            if (string.IsNullOrEmpty(path))
                path = "$(EmulatorDir)" + device + "/";
            path = path.Replace("$(EmulatorDir)", rpcs3.TrimEnd('\\', '/') + "/");
            return Path.GetFullPath(path.Replace('/', '\\'));
        }

        // config\games.yml "BLJM61064: <the folder that holds PS3_GAME>/".
        public static string Disc(string rpcs3)
        {
            foreach (string file in new[] { Path.Combine(rpcs3, "config", "games.yml"), Path.Combine(rpcs3, "games.yml") })
            {
                string value = YamlValue(file, GameFiles.TitleId);
                if (!string.IsNullOrEmpty(value))
                    return Path.GetFullPath(value.Replace('/', '\\'));
            }
            return null;
        }

        static string YamlValue(string file, string key)
        {
            try
            {
                if (!File.Exists(file))
                    return null;
                foreach (string raw in File.ReadAllLines(file, Encoding.UTF8))
                {
                    string line = raw.Trim();
                    if (!line.StartsWith(key + ":"))
                        continue;
                    string value = line.Substring(key.Length + 1).Trim();
                    if (value.Length >= 2 && (value[0] == '"' || value[0] == '\'') && value[value.Length - 1] == value[0])
                        value = value.Substring(1, value.Length - 2);
                    return value;
                }
            }
            catch (Exception)
            {
            }
            return null;
        }

        // rpcs3.exe with arguments; hidden for --decrypt (no window), shown for installs (RPCS3's own progress window).
        public static int Run(string rpcs3, string arguments, bool hidden, int timeoutMs)
        {
            var info = new ProcessStartInfo(Path.Combine(rpcs3, "rpcs3.exe"), arguments) { WorkingDirectory = rpcs3, UseShellExecute = false, CreateNoWindow = hidden };
            using (Process p = Process.Start(info))
            {
                if (!p.WaitForExit(timeoutMs))
                    return -1;
                return p.ExitCode;
            }
        }
    }

    // What this PC has: RPCS3 and what is installed in it, or a disc chosen by hand.
    public class SetupState
    {
        public string Rpcs3, Disc, Update, UpdateVersion = "", Firmware;
        public bool DiscOk, UpdateOk, FirmwareOk;
    }

    public static class Setup
    {
        public const string EbootSha256 = "A51447964301B558CDC2B279ECA1F4E92C70DD471FD6BA939695490B0F1C100F";
        // module -> (decrypted .prx SHA-256, relocated image SHA-256, load base: see stubs.cpp ps3_load_prx_modules)
        static readonly Dictionary<string, string[]> Modules = new Dictionary<string, string[]>
        {
            { "libsre", new[] { "9E7EE6EDC99B1D372686CF927B883B18F19ED4D32D1FAF35E214CCA4AE0737B6", "088BD13689C4980D063C7188631A5904BE4BFA510620EF539FFB987985E660BA", "30000000" } },
            { "libspurs_jq", new[] { "DFE0A5B8FB7DEC1CF15573DB03A30B155948B39D61465FEB384D4E64B613F641", "F1E1EFC0AC29E2F275FDCA4B4AA486C5B4E37DA93965F9B30C4EA81A6F2F635E", "30100000" } },
        };

        public static IEnumerable<string> ModuleNames { get { return Modules.Keys; } }

        public static string Sha256(string path)
        {
            using (var sha = SHA256.Create())
            using (FileStream f = File.OpenRead(path))
                return BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "");
        }

        static string Sha256(byte[] data)
        {
            using (var sha = SHA256.Create())
                return BitConverter.ToString(sha.ComputeHash(data)).Replace("-", "");
        }

        public static bool EbootOk(Launcher launcher)
        {
            return File.Exists(launcher.Eboot) && new FileInfo(launcher.Eboot).Length == 15355712 && Sha256(launcher.Eboot) == EbootSha256;
        }

        public static bool FirmwareReady(Launcher launcher)
        {
            return Modules.Keys.All(m => File.Exists(Path.Combine(launcher.FirmwareDir, m + "_image.bin")));
        }

        public static bool IsDisc(string dir)
        {
            if (string.IsNullOrEmpty(dir))
                return false;
            Dictionary<string, string> sfo = Sfo.Read(Path.Combine(dir, "PS3_GAME", "PARAM.SFO"));
            return Sfo.Get(sfo, "TITLE_ID") == GameFiles.TitleId && Directory.Exists(Path.Combine(dir, "PS3_GAME", "USRDIR"));
        }

        public static SetupState Detect(Launcher launcher, string rpcs3)
        {
            var s = new SetupState { Rpcs3 = rpcs3 };
            string chosen = launcher.SetupValue("DiscDir");
            if (IsDisc(chosen))
                s.Disc = chosen;
            if (rpcs3 == null)
                return s;
            if (s.Disc == null)
            {
                string disc = Rpcs3.Disc(rpcs3);
                if (IsDisc(disc))
                    s.Disc = disc;
            }
            s.DiscOk = s.Disc != null;
            string update = Path.Combine(Rpcs3.Device(rpcs3, "dev_hdd0"), "game", GameFiles.TitleId);
            Dictionary<string, string> sfo = Sfo.Read(Path.Combine(update, "PARAM.SFO"));
            s.UpdateVersion = Sfo.Get(sfo, "APP_VER");
            s.UpdateOk = Sfo.Get(sfo, "TITLE_ID") == GameFiles.TitleId && s.UpdateVersion == "01.03" && File.Exists(Path.Combine(update, "USRDIR", "EBOOT.BIN"));
            s.Update = update;
            s.Firmware = Path.Combine(Rpcs3.Device(rpcs3, "dev_flash"), "sys", "external");
            s.FirmwareOk = Modules.Keys.All(m => File.Exists(Path.Combine(s.Firmware, m + ".sprx")));
            return s;
        }

        // A decrypted PRX placed at its load base with its relocations applied (as ps3recomp's tools/lift_prx.py).
        static byte[] Relocate(byte[] d, uint baseAddress)
        {
            Func<int, ulong> be64 = delegate(int o) { ulong v = 0; for (int i = 0; i < 8; i++) v = v << 8 | d[o + i]; return v; };
            Func<int, uint> be32 = delegate(int o) { return (uint)(d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]); };
            if (d.Length < 0x40 || d[0] != 0x7F || d[1] != 'E' || d[4] != 2 || d[5] != 2)
                throw new InvalidDataException("복호화된 PRX(ELF64 빅 엔디언)가 아닙니다");
            int phoff = (int)be64(0x20), phnum = d[0x38] << 8 | d[0x39];
            var loads = new List<ulong[]>();   // offset, vaddr, filesz, memsz
            ulong relaOff = 0, relaSize = 0;
            for (int i = 0; i < phnum; i++)
            {
                int o = phoff + i * 0x38;
                uint type = be32(o);
                if (type == 1)
                    loads.Add(new[] { be64(o + 8), be64(o + 16), be64(o + 32), be64(o + 40) });
                else if (type == 0x700000A4)
                {
                    relaOff = be64(o + 8);
                    relaSize = be64(o + 32);
                }
            }
            ulong top = loads.Max(l => l[1] + l[3]);
            var img = new byte[top];
            var segBase = new List<ulong>();
            foreach (ulong[] l in loads)
            {
                Array.Copy(d, (long)l[0], img, (long)l[1], (long)l[2]);
                segBase.Add(baseAddress + l[1]);
            }
            Action<ulong, ulong, int> put = delegate(ulong at, ulong value, int bytes)
            {
                for (int i = 0; i < bytes; i++)
                    img[at + (ulong)i] = (byte)(value >> (8 * (bytes - 1 - i)));
            };
            for (ulong i = 0; i < relaSize / 24; i++)
            {
                int o = (int)(relaOff + i * 24);
                ulong offset = be64(o), ptr = be64(o + 16);
                int indexValue = d[o + 10], indexAddr = d[o + 11];
                uint type = be32(o + 12);
                ulong target = segBase[indexAddr] - baseAddress + offset;
                ulong value = (indexValue == 0xFF ? 0 : segBase[indexValue]) + ptr;
                switch (type)
                {
                    case 1: put(target, value & 0xFFFFFFFF, 4); break;            // ADDR32
                    case 4: put(target, value & 0xFFFF, 2); break;                // ADDR16_LO
                    case 5: put(target, (value >> 16) & 0xFFFF, 2); break;        // ADDR16_HI
                    case 6: put(target, ((value + 0x8000) >> 16) & 0xFFFF, 2); break;   // ADDR16_HA
                    case 44: put(target, value, 8); break;                        // REL64
                    default: throw new InvalidDataException("처리하지 못하는 재배치 형식 " + type);
                }
            }
            return img;
        }

        // A decrypted module (from RPCS3 or chosen by hand) checked and installed: <module>.prx and <module>_image.bin.
        static void InstallModule(Launcher launcher, string module, string prx)
        {
            string[] want = Modules[module];
            if (Sha256(prx) != want[0])
                throw new InvalidDataException(module + ".prx가 포트가 쓰는 펌웨어(4.9x)의 것과 다릅니다. RPCS3에 최신 PS3 펌웨어를 설치한 뒤 다시 하세요.");
            byte[] image = Relocate(File.ReadAllBytes(prx), Convert.ToUInt32(want[2], 16));
            if (Sha256(image) != want[1])
                throw new InvalidDataException(module + " 재배치 결과가 예상과 다릅니다.");
            Directory.CreateDirectory(launcher.FirmwareDir);
            File.Copy(prx, Path.Combine(launcher.FirmwareDir, module + ".prx"), true);
            File.WriteAllBytes(Path.Combine(launcher.FirmwareDir, module + "_image.bin"), image);
        }

        static void InstallEboot(Launcher launcher, string elf)
        {
            if (Sha256(elf) != EbootSha256)
                throw new InvalidDataException("EBOOT가 1.03 업데이트의 것과 다릅니다 (" + GameFiles.TitleId + " 1.03이 필요합니다).");
            Directory.CreateDirectory(launcher.GameDir);
            File.Copy(elf, launcher.Eboot, true);
        }

        // Files chosen by hand that are already decrypted (EBOOT.ELF / EBOOT.elf, libsre.prx, libspurs_jq.prx).
        public static string InstallDecrypted(Launcher launcher, IEnumerable<string> paths)
        {
            var done = new List<string>();
            foreach (string path in paths)
            {
                string name = Path.GetFileNameWithoutExtension(path).ToLowerInvariant();
                if (Modules.ContainsKey(name))
                {
                    InstallModule(launcher, name, path);
                    done.Add(name);
                }
                else if (name == "eboot")
                {
                    InstallEboot(launcher, path);
                    done.Add("EBOOT");
                }
            }
            return done.Count == 0 ? "고른 파일 중에 EBOOT.ELF, libsre.prx, libspurs_jq.prx가 없습니다." : null;
        }

        // Everything RPCS3 has, made into the port's files. progress gets one line per step.
        public static void Prepare(Launcher launcher, SetupState s, Action<string> progress)
        {
            string work = Path.Combine(launcher.GameDir, "_setup");
            if (Directory.Exists(work))
                Directory.Delete(work, true);
            Directory.CreateDirectory(work);
            try
            {
                // 1. The disc: used where it is.
                if (s.Disc != null)
                {
                    launcher.SetSetupValue("DiscDir", s.Disc);
                    progress("디스크: " + s.Disc + " 를 그대로 씁니다");
                }
                // 2. The 1.03 update: copied (the game writes its install data next to it).
                string hddGame = Path.Combine(launcher.Hdd0, "game", GameFiles.TitleId);
                Dictionary<string, string> have = Sfo.Read(Path.Combine(hddGame, "PARAM.SFO"));
                if (Sfo.Get(have, "APP_VER") != "01.03")
                {
                    progress("1.03 업데이트를 복사하는 중...");
                    CopyTree(s.Update, hddGame);
                    progress("1.03 업데이트 복사 완료");
                }
                else
                    progress("1.03 업데이트: 이미 있음");
                // 3. EBOOT: RPCS3 decrypts a copy of the update's EBOOT.BIN (nothing is written into RPCS3's folders).
                if (!EbootOk(launcher))
                {
                    progress("EBOOT를 RPCS3로 복호화하는 중...");
                    string bin = Path.Combine(work, "EBOOT.BIN");
                    File.Copy(Path.Combine(s.Update, "USRDIR", "EBOOT.BIN"), bin, true);
                    Decrypt(s.Rpcs3, bin);
                    string elf = Path.Combine(work, "EBOOT.elf");
                    if (!File.Exists(elf))
                        throw new InvalidOperationException("RPCS3가 EBOOT를 복호화하지 못했습니다. RPCS3의 유틸리티 > PS3 바이너리 복호화로 EBOOT.BIN을 직접 복호화한 뒤 '복호화한 파일 고르기'를 쓰세요.");
                    InstallEboot(launcher, elf);
                    progress("EBOOT 1.03: 확인됨");
                }
                else
                    progress("EBOOT 1.03: 이미 있음");
                // 4. Firmware modules: one decrypt each (RPCS3 decrypts only the first of several paths).
                foreach (string module in Modules.Keys)
                {
                    string image = Path.Combine(launcher.FirmwareDir, module + "_image.bin");
                    if (File.Exists(image) && Sha256(image) == Modules[module][1])
                    {
                        progress(module + ": 이미 있음");
                        continue;
                    }
                    progress(module + "를 RPCS3로 복호화하는 중...");
                    string sprx = Path.Combine(work, module + ".sprx");
                    File.Copy(Path.Combine(s.Firmware, module + ".sprx"), sprx, true);
                    Decrypt(s.Rpcs3, sprx);
                    string prx = Path.Combine(work, module + ".prx");
                    if (!File.Exists(prx))
                        throw new InvalidOperationException("RPCS3가 " + module + ".sprx를 복호화하지 못했습니다.");
                    InstallModule(launcher, module, prx);
                    progress(module + ": 확인됨");
                }
            }
            finally
            {
                try
                {
                    Directory.Delete(work, true);
                }
                catch (Exception)
                {
                }
            }
        }

        static void Decrypt(string rpcs3, string path)
        {
            int code = Rpcs3.Run(rpcs3, "--decrypt \"" + path + "\"", true, 120000);
            if (code == -1)
                throw new TimeoutException("RPCS3가 2분 안에 끝나지 않았습니다.");
            for (int i = 0; i < 20 && !File.Exists(Path.ChangeExtension(path, path.EndsWith(".sprx") ? ".prx" : ".elf")); i++)
                Thread.Sleep(250);
        }

        static void CopyTree(string from, string to)
        {
            Directory.CreateDirectory(to);
            foreach (string dir in Directory.GetDirectories(from, "*", SearchOption.AllDirectories))
                Directory.CreateDirectory(to + dir.Substring(from.Length));
            foreach (string file in Directory.GetFiles(from, "*", SearchOption.AllDirectories))
                File.Copy(file, to + file.Substring(from.Length), true);
        }

        // A disc image (decrypted PS3 ISO): mounted by Windows, PS3_GAME and PS3_DISC.SFB copied to game\dev_bdvd.
        public static void CopyIso(Launcher launcher, string iso, Action<string> progress)
        {
            progress("ISO를 연결하는 중...");
            string letter = PowerShell("(Mount-DiskImage -ImagePath '" + iso.Replace("'", "''") + "' -PassThru | Get-Volume).DriveLetter").Trim();
            if (letter.Length != 1)
                throw new InvalidOperationException("ISO를 열지 못했습니다 (복호화된 PS3 ISO가 필요합니다). " + letter);
            try
            {
                string root = letter + ":\\";
                if (!IsDisc(root))
                    throw new InvalidDataException("ISO 안에 " + GameFiles.TitleId + "의 PS3_GAME 폴더가 없습니다.");
                progress("디스크 파일을 복사하는 중 (약 3.4 GB)...");
                string target = Path.Combine(launcher.GameDir, "dev_bdvd");
                CopyTree(Path.Combine(root, "PS3_GAME"), Path.Combine(target, "PS3_GAME"));
                string sfb = Path.Combine(root, "PS3_DISC.SFB");
                if (File.Exists(sfb))
                    File.Copy(sfb, Path.Combine(target, "PS3_DISC.SFB"), true);
                launcher.SetSetupValue("DiscDir", "");
                progress("디스크 복사 완료: game\\dev_bdvd");
            }
            finally
            {
                PowerShell("Dismount-DiskImage -ImagePath '" + iso.Replace("'", "''") + "' | Out-Null");
            }
        }

        static string PowerShell(string command)
        {
            var info = new ProcessStartInfo("powershell.exe", "-NoProfile -NonInteractive -Command \"" + command.Replace("\"", "\\\"") + "\"")
            {
                UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
            };
            using (Process p = Process.Start(info))
            {
                string output = p.StandardOutput.ReadToEnd() + p.StandardError.ReadToEnd();
                p.WaitForExit();
                return output;
            }
        }
    }

    public class SetupPage : PageBase
    {
        string rpcs3;
        bool searched, busy, finding;
        readonly List<string> log = new List<string>();

        public SetupPage(MainWindow window) : base(window)
        {
        }

        public override void Shown()
        {
            if (!searched && !busy)
            {
                searched = true;
                busy = finding = true;
                base.Shown();
                var launcher = Launcher;
                ThreadPool.QueueUserWorkItem(delegate
                {
                    string found = Rpcs3.Find(launcher);
                    Window.Dispatcher.BeginInvoke((Action)delegate
                    {
                        rpcs3 = found;
                        busy = finding = false;
                        if (found != null)
                            Launcher.SetSetupValue("Rpcs3", found);
                        Window.Refresh();
                    });
                });
                return;
            }
            base.Shown();
        }

        void Work(string title, Action<Action<string>> job)
        {
            if (busy)
                return;
            if (Game.IsRunning)
            {
                Window.ShowProblem("게임이 실행 중입니다. 게임을 닫은 뒤 다시 하세요.");
                return;
            }
            busy = true;
            log.Clear();
            log.Add(title);
            Window.Refresh();
            Action<string> progress = delegate(string line)
            {
                Window.Dispatcher.BeginInvoke((Action)delegate
                {
                    log.Add(line);
                    Window.Refresh();
                });
            };
            ThreadPool.QueueUserWorkItem(delegate
            {
                Exception failed = null;
                try
                {
                    job(progress);
                }
                catch (Exception error)
                {
                    failed = error;
                }
                Window.Dispatcher.BeginInvoke((Action)delegate
                {
                    busy = false;
                    if (failed != null)
                    {
                        log.Add("실패: " + failed.Message);
                        Window.Error(title + " 실패", failed);
                    }
                    else
                        log.Add("끝났습니다.");
                    Window.Refresh();
                });
            });
        }

        protected override FrameworkElement Build()
        {
            GameFiles files = GameFiles.Check(Launcher);
            SetupState s = Setup.Detect(Launcher, rpcs3);
            Func<bool, string, string, FrameworkElement> item = delegate(bool ok, string label, string value)
            {
                return Line(label, (ok ? "✓ " : "✕ ") + value, ok ? Ui.Brush("Good") : Ui.Brush("Warn"));
            };

            var now = new StackPanel();
            now.Children.Add(item(files.Eboot, "EBOOT 1.03", files.Eboot ? "준비됨" : "없음"));
            now.Children.Add(item(files.Firmware, "펌웨어 모듈", files.Firmware ? "libsre, libspurs_jq 준비됨" : "없음"));
            now.Children.Add(item(files.Disc, "디스크", files.Disc ? Launcher.Bdvd : "없음"));
            now.Children.Add(item(files.Patch, "1.03 업데이트", files.Patch ? "준비됨" : files.PatchVersion.Length > 0 ? files.PatchVersion + " (1.03 필요)" : "없음"));
            if (files.Ready)
                now.Children.Add(Ui.Text("게임을 실행할 수 있습니다. 홈에서 플레이를 누르세요.", "Body"));

            var rp = new StackPanel();
            if (finding)
                rp.Children.Add(Ui.Text("RPCS3를 찾는 중... (바탕화면, 문서, 다운로드, Program Files, 각 드라이브)", "Caption"));
            else if (rpcs3 == null)
            {
                rp.Children.Add(Ui.Text("이 PC에서 RPCS3를 찾지 못했습니다. RPCS3는 암호화된 EBOOT와 펌웨어를 복호화하는 데 씁니다. 설치(압축 해제)한 뒤 PS3 펌웨어와 게임의 1.03 업데이트를 넣고, 아래에서 폴더를 고르세요.", "Body"));
                rp.Children.Add(Ui.Row(
                    Ui.Button("RPCS3 폴더 고르기", PickRpcs3, "AccentButton", "", new Thickness(0, 8, 8, 0)),
                    Ui.Button("RPCS3 받기 (rpcs3.net)", delegate { Process.Start(Rpcs3.Download); }, null, "", new Thickness(0, 8, 0, 0))));
            }
            else
            {
                rp.Children.Add(Line("RPCS3", rpcs3));
                rp.Children.Add(item(s.FirmwareOk, "PS3 펌웨어", s.FirmwareOk ? "설치됨" : "설치 안 됨"));
                rp.Children.Add(item(s.UpdateOk, "1.03 업데이트", s.UpdateOk ? "설치됨" : s.UpdateVersion.Length > 0 ? s.UpdateVersion + " (1.03 필요)" : "설치 안 됨"));
                rp.Children.Add(item(s.DiscOk, "디스크", s.DiscOk ? s.Disc : "RPCS3 게임 목록에 없음"));
                var buttons = new WrapPanel { Margin = new Thickness(0, 6, 0, 0) };
                if (!s.FirmwareOk)
                    buttons.Children.Add(Ui.Button("펌웨어 설치 (PUP 고르기)", InstallFirmware, null, "", new Thickness(0, 0, 8, 8)));
                if (!s.UpdateOk)
                    buttons.Children.Add(Ui.Button("업데이트 설치 (PKG 고르기)", InstallUpdate, null, "", new Thickness(0, 0, 8, 8)));
                buttons.Children.Add(Ui.Button("디스크 폴더 고르기", PickDisc, null, "", new Thickness(0, 0, 8, 8)));
                buttons.Children.Add(Ui.Button("디스크 ISO 고르기", PickIso, null, "", new Thickness(0, 0, 8, 8)));
                buttons.Children.Add(Ui.Button("다른 RPCS3 고르기", PickRpcs3, null, "", new Thickness(0, 0, 8, 8)));
                rp.Children.Add(buttons);
                bool ready = s.FirmwareOk && s.UpdateOk && (s.DiscOk || files.Disc);
                var prepare = Ui.Button(files.Ready ? "다시 확인하고 준비" : "자동 준비", delegate
                {
                    SetupState state = s;
                    Work("자동 준비", delegate(Action<string> progress) { Setup.Prepare(Launcher, state, progress); });
                }, "AccentButton", "", new Thickness(0, 4, 0, 0));
                prepare.IsEnabled = ready && !busy;
                prepare.HorizontalAlignment = HorizontalAlignment.Left;
                rp.Children.Add(prepare);
                if (!ready)
                    rp.Children.Add(Ui.Text("위의 ✕ 항목을 먼저 채우면 자동 준비를 누를 수 있습니다.", "Caption"));
            }

            var other = new StackPanel();
            other.Children.Add(Ui.Text("이미 복호화한 파일(EBOOT.ELF 또는 EBOOT.elf, libsre.prx, libspurs_jq.prx)이 있으면 직접 고를 수 있습니다. 포트가 쓰는 파일과 같은지 해시로 확인합니다.", "Body"));
            var picks = new WrapPanel { Margin = new Thickness(0, 8, 0, 0) };
            picks.Children.Add(Ui.Button("복호화한 파일 고르기", PickDecrypted, null, "", new Thickness(0, 0, 8, 8)));
            picks.Children.Add(Ui.Button("디스크 폴더 고르기", PickDisc, null, "", new Thickness(0, 0, 8, 8)));
            picks.Children.Add(Ui.Button("디스크 ISO 고르기", PickIso, null, "", new Thickness(0, 0, 8, 8)));
            other.Children.Add(picks);
            other.Children.Add(Ui.Text("RPCS3 없이 할 때는 1.03 업데이트 폴더(BLJM61064)를 game\\dev_hdd0\\game 아래에 직접 넣어야 합니다.", "Caption"));

            var help = new StackPanel();
            foreach (string line in new[]
            {
                "필요한 것: 본인의 サカつく 디스크(BLJM61064, 폴더 또는 복호화된 ISO), 1.03 업데이트 PKG, PS3 펌웨어(PS3UPDAT.PUP), RPCS3.",
                "1. RPCS3에 펌웨어를 설치합니다 (RPCS3: 파일 > 펌웨어 설치, 또는 위 버튼). PUP는 PlayStation 공식 사이트의 PS3 시스템 소프트웨어 업데이트에서 받습니다.",
                "2. RPCS3에 1.03 업데이트 PKG를 설치합니다 (파일 > 패키지 설치, 또는 위 버튼).",
                "3. 디스크를 RPCS3 게임 목록에 추가하거나, 위에서 디스크 폴더·ISO를 고릅니다.",
                "4. 자동 준비를 누르면 RPCS3가 복호화하고, 런처가 확인해 game 폴더에 넣습니다. RPCS3의 파일은 바꾸지 않습니다.",
                "다른 PC로 옮길 때: 이 폴더를 통째로 옮기거나, 새 PC에서 같은 순서로 다시 준비합니다. 세이브는 gamedata 폴더에 있습니다.",
            })
            {
                var t = Ui.Text(line, "Caption");
                t.Margin = new Thickness(0, 0, 0, 6);
                help.Children.Add(t);
            }

            var page = new List<UIElement> { Ui.Card("지금 상태", now), Ui.Card("RPCS3로 준비", rp) };
            if (log.Count > 0)
            {
                var progressText = Ui.Text(string.Join("\n", log), "Body");
                page.Add(Ui.Card(busy ? "진행 중" : "결과", progressText));
            }
            page.Add(Ui.Card("직접 고르기", other));
            page.Add(Ui.Card("도움말", help));
            return Ui.Page("게임 파일", "본인의 디스크, 1.03 업데이트, PS3 펌웨어로 포트에 필요한 파일을 만듭니다. 복호화는 RPCS3가 합니다.", page.ToArray());
        }

        void PickRpcs3()
        {
            string dir = Ui.FolderDialog("rpcs3.exe가 있는 폴더를 고르세요", rpcs3 ?? Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory));
            if (dir == null)
                return;
            if (!File.Exists(Path.Combine(dir, "rpcs3.exe")))
            {
                Window.ShowProblem("그 폴더에 rpcs3.exe가 없습니다.");
                return;
            }
            rpcs3 = dir;
            Launcher.SetSetupValue("Rpcs3", dir);
            Window.Refresh();
        }

        void PickDisc()
        {
            string dir = Ui.FolderDialog("PS3_GAME 폴더가 들어 있는 디스크 폴더를 고르세요", Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory));
            if (dir == null)
                return;
            if (!Setup.IsDisc(dir) && Setup.IsDisc(Path.GetDirectoryName(dir)))
                dir = Path.GetDirectoryName(dir);   // PS3_GAME itself was chosen
            if (!Setup.IsDisc(dir))
            {
                Window.ShowProblem("그 폴더에 " + GameFiles.TitleId + "의 PS3_GAME 폴더가 없습니다.");
                return;
            }
            Launcher.SetSetupValue("DiscDir", dir);
            Window.Toast("디스크 위치를 저장했습니다");
            Window.Refresh();
        }

        void PickIso()
        {
            var dialog = new Microsoft.Win32.OpenFileDialog { Title = "디스크 ISO (복호화된 PS3 ISO)", Filter = "ISO (*.iso)|*.iso" };
            if (dialog.ShowDialog(Window) != true)
                return;
            string iso = dialog.FileName;
            Window.Confirm("ISO에서 디스크 복사", "ISO를 Windows로 잠시 연결해 PS3_GAME 폴더를 game\\dev_bdvd로 복사합니다 (약 3.4 GB). ISO 파일은 바뀌지 않습니다.", "복사", delegate
            {
                Work("ISO에서 디스크 복사", delegate(Action<string> progress) { Setup.CopyIso(Launcher, iso, progress); });
            });
        }

        void InstallFirmware()
        {
            var dialog = new Microsoft.Win32.OpenFileDialog { Title = "PS3 펌웨어 (PS3UPDAT.PUP)", Filter = "PS3 펌웨어 (*.PUP)|*.PUP" };
            if (dialog.ShowDialog(Window) != true)
                return;
            string pup = dialog.FileName, dir = rpcs3;
            Work("RPCS3에 펌웨어 설치", delegate(Action<string> progress)
            {
                progress("RPCS3 창에서 설치가 끝나면 RPCS3를 닫아 주세요.");
                Rpcs3.Run(dir, "--installfw \"" + pup + "\"", false, 30 * 60 * 1000);
            });
        }

        void InstallUpdate()
        {
            var dialog = new Microsoft.Win32.OpenFileDialog { Title = "1.03 업데이트 PKG", Filter = "PKG (*.pkg)|*.pkg" };
            if (dialog.ShowDialog(Window) != true)
                return;
            string pkg = dialog.FileName, dir = rpcs3;
            Work("RPCS3에 1.03 업데이트 설치", delegate(Action<string> progress)
            {
                progress("RPCS3 창에서 설치가 끝나면 RPCS3를 닫아 주세요.");
                Rpcs3.Run(dir, "--installpkg \"" + pkg + "\"", false, 30 * 60 * 1000);
            });
        }

        void PickDecrypted()
        {
            var dialog = new Microsoft.Win32.OpenFileDialog { Title = "복호화한 파일 (여러 개 고를 수 있음)", Filter = "EBOOT / PRX (*.elf;*.prx)|*.elf;*.prx|모든 파일|*.*", Multiselect = true };
            if (dialog.ShowDialog(Window) != true)
                return;
            string[] paths = dialog.FileNames;
            Work("복호화한 파일 넣기", delegate(Action<string> progress)
            {
                string problem = Setup.InstallDecrypted(Launcher, paths);
                if (problem != null)
                    throw new InvalidOperationException(problem);
                progress("확인하고 넣었습니다: " + string.Join(", ", paths.Select(Path.GetFileName)));
            });
        }
    }
}
