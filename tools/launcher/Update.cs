// New versions of the port from GitHub Releases: the newest release's zip (Sakatsuku_PC_<exe time>.zip) against this
// folder's version (version.txt from tools\make_release.ps1, else the exe's own time). Installing replaces only the
// app files of the zip (launcher, exe and DLLs, shared cheat lists, README); game\, gamedata\, launcher.ini and the
// personal cheat settings are not in the zip and stay as they are. The old game exe is kept as
// build-release\SakatsukuRecomp.prev.exe. A git checkout (the developer's folder) is never overwritten.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows;

namespace SakatsukuLauncher
{
    public class ReleaseInfo
    {
        public string Tag, Name, Notes, PageUrl, ZipName, ZipUrl, Stamp;
        public long ZipSize;
        public bool Newer;
        public string Error;
    }

    public static class PortUpdate
    {
        public const string Repository = "PrimeTimerer/Make-Soccer-Club-Game-Project";
        public static string ReleasesPage { get { return "https://github.com/" + Repository + "/releases"; } }
        static readonly Regex ZipPattern = new Regex(@"^Sakatsuku_PC_(\d{8}-\d{4})\.zip$", RegexOptions.IgnoreCase);

        // This folder's version: version.txt (a release) or the game exe's time (as make_release.ps1 names the zip).
        public static string LocalStamp(Launcher launcher)
        {
            try
            {
                string file = Path.Combine(launcher.Dir, "version.txt");
                if (File.Exists(file))
                {
                    Match m = Regex.Match(File.ReadAllText(file), @"\d{8}-\d{4}");
                    if (m.Success)
                        return m.Value;
                }
                if (File.Exists(launcher.Exe))
                    return File.GetLastWriteTime(launcher.Exe).ToString("yyyyMMdd-HHmm");
            }
            catch (Exception)
            {
            }
            return "";
        }

        public static bool IsGitCheckout(Launcher launcher)
        {
            return Directory.Exists(Path.Combine(launcher.Dir, ".git"));
        }

        static string Get(string url)
        {
            ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;   // TLS 1.2
            using (var web = new WebClient())
            {
                web.Headers[HttpRequestHeader.UserAgent] = "SakatsukuLauncher";
                web.Headers[HttpRequestHeader.Accept] = "application/vnd.github+json";
                web.Encoding = Encoding.UTF8;
                return web.DownloadString(url);
            }
        }

        public static ReleaseInfo Check(Launcher launcher)
        {
            var info = new ReleaseInfo();
            try
            {
                var json = new JavaScriptSerializer { MaxJsonLength = 8 * 1024 * 1024 };
                var release = json.Deserialize<Dictionary<string, object>>(Get("https://api.github.com/repos/" + Repository + "/releases/latest"));
                info.Tag = release["tag_name"] as string;
                info.Name = release.ContainsKey("name") ? release["name"] as string : info.Tag;
                info.Notes = release.ContainsKey("body") ? release["body"] as string : "";
                info.PageUrl = release["html_url"] as string;
                foreach (object a in (ArrayList)release["assets"])
                {
                    var asset = (Dictionary<string, object>)a;
                    string name = asset["name"] as string;
                    Match m = ZipPattern.Match(name ?? "");
                    if (!m.Success)
                        continue;
                    info.ZipName = name;
                    info.ZipUrl = asset["browser_download_url"] as string;
                    info.ZipSize = Convert.ToInt64(asset["size"]);
                    info.Stamp = m.Groups[1].Value;
                }
                string local = LocalStamp(launcher);
                info.Newer = info.Stamp != null && string.CompareOrdinal(info.Stamp, local) > 0;
                launcher.SetState("LastUpdateCheck", DateTime.Now.ToString("yyyy-MM-dd"));
            }
            catch (Exception error)
            {
                info.Error = error.Message;
            }
            return info;
        }

        // Once a day when the launcher opens (no window if nothing is new).
        public static void CheckDaily(Launcher launcher, Action<ReleaseInfo> done)
        {
            if (launcher.State("LastUpdateCheck", "") == DateTime.Now.ToString("yyyy-MM-dd") || launcher.Setting("UpdateCheck") == "0")
                return;
            ThreadPool.QueueUserWorkItem(delegate
            {
                ReleaseInfo info = Check(launcher);
                if (Application.Current != null)
                    Application.Current.Dispatcher.BeginInvoke((Action)delegate { done(info); });
            });
        }

        // Leftovers of the last update (the renamed launcher and the download), removed at the next start.
        public static void CleanUp(Launcher launcher)
        {
            string old = Path.Combine(launcher.Dir, "Sakatsuku Launcher.old.exe");
            string work = Path.Combine(launcher.Dir, "update");
            for (int i = 0; i < 10 && (File.Exists(old) || Directory.Exists(work)); i++)   // the old launcher may still be closing
            {
                try
                {
                    if (File.Exists(old))
                        File.Delete(old);
                }
                catch (Exception)
                {
                }
                try
                {
                    if (Directory.Exists(work))
                        Directory.Delete(work, true);
                }
                catch (Exception)
                {
                }
                if (File.Exists(old) || Directory.Exists(work))
                    Thread.Sleep(200);
            }
        }

        // Downloads and installs the release; returns the new launcher's path (start it, then close this one).
        public static string Install(Launcher launcher, ReleaseInfo info, Action<string> progress)
        {
            if (IsGitCheckout(launcher))
                throw new InvalidOperationException("이 폴더는 git 저장소(개발 폴더)라 자동 설치를 하지 않습니다. 릴리스 zip을 다른 폴더에 받아 쓰세요.");
            if (Game.IsRunning)
                throw new InvalidOperationException("게임이 실행 중입니다. 게임을 닫은 뒤 다시 하세요.");
            if (info.ZipUrl == null)
                throw new InvalidOperationException("최신 릴리스에 zip이 없습니다.");
            string work = Path.Combine(launcher.Dir, "update");
            if (Directory.Exists(work))
                Directory.Delete(work, true);
            Directory.CreateDirectory(work);
            string zip = Path.Combine(work, info.ZipName);
            progress("받는 중: " + info.ZipName + " (" + PcInfo.Size(info.ZipSize) + ")");
            ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
            using (var web = new WebClient())
            {
                web.Headers[HttpRequestHeader.UserAgent] = "SakatsukuLauncher";
                web.DownloadFile(info.ZipUrl, zip);
            }
            if (new FileInfo(zip).Length != info.ZipSize)
                throw new InvalidDataException("받은 파일 크기가 다릅니다. 다시 시도하세요.");
            progress("압축을 푸는 중...");
            string unpacked = Path.Combine(work, "new");
            ZipFile.ExtractToDirectory(zip, unpacked);
            string root = unpacked;
            string[] tops = Directory.GetDirectories(unpacked);
            if (!File.Exists(Path.Combine(root, Launcher.ExeName)) && tops.Length == 1)
                root = tops[0];
            if (!File.Exists(Path.Combine(root, Launcher.ExeName)) || !File.Exists(Path.Combine(root, "build-release", Game.Exe)))
                throw new InvalidDataException("zip 안에 런처와 게임 실행 파일이 없습니다.");

            progress("이전 실행 파일을 백업하는 중...");
            string exe = Path.Combine(launcher.Dir, "build-release", Game.Exe);
            if (File.Exists(exe))
                File.Copy(exe, Path.Combine(launcher.Dir, "build-release", "SakatsukuRecomp.prev.exe"), true);
            string me = Path.Combine(launcher.Dir, Launcher.ExeName), old = Path.Combine(launcher.Dir, "Sakatsuku Launcher.old.exe");
            if (File.Exists(old))
                File.Delete(old);
            File.Move(me, old);   // a running exe may be renamed, not overwritten
            try
            {
                progress("새 파일로 바꾸는 중...");
                foreach (string file in Directory.GetFiles(root, "*", SearchOption.AllDirectories))
                {
                    string relative = file.Substring(root.Length).TrimStart('\\');
                    string target = Path.Combine(launcher.Dir, relative);
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    File.Copy(file, target, true);
                }
            }
            catch (Exception)
            {
                if (!File.Exists(me))
                    File.Move(old, me);
                throw;
            }
            File.WriteAllText(Path.Combine(launcher.Dir, "version.txt"), info.Stamp);   // older zips have none
            progress("설치 완료: " + info.Tag);
            return me;
        }
    }
}
