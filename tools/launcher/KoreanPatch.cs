// The Korean fan patch, kept beside the game files instead of over them (owner, 2026-11-08: "원본은 유지하고
// 한글패치만 따로 분리해놓고 작동").
//
// The patch exe is never run. It carries a zip after a 0x174C-byte stub (offsets relative to the zip, so the
// stream starts there): manifest.json (format "sakatsuku-xor-v2") and one XOR delta per known original of
// each of its files. A patched file is (original XOR delta) cut to target_size, checked against target_sha256.
// Install writes every patched file to game\mods\korean\data\<path>; with the "KoreanPatch" setting on, the
// launcher passes PS3_USRDIR_OVERLAY=game\mods\korean and the port reads a file under USRDIR from there first,
// for the disc copy (/dev_bdvd, e.g. font.par at boot) and the update copy (/dev_hdd0) alike. The originals
// stay as they are, so the launcher and the F2 menu keep reading them for names.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;

namespace SakatsukuLauncher
{
    public static class KoreanPatch
    {
        public static string Dir(Launcher launcher) { return Path.Combine(launcher.GameDir, "mods", "korean"); }

        static string Marker(Launcher launcher) { return Path.Combine(Dir(launcher), "installed.txt"); }

        public static bool Installed(Launcher launcher) { return File.Exists(Marker(launcher)); }

        public static string Info(Launcher launcher)
        {
            try { return File.ReadAllText(Marker(launcher), Encoding.UTF8).Trim(); }
            catch (Exception) { return ""; }
        }

        public static void Remove(Launcher launcher)
        {
            string dir = Dir(launcher);
            if (Directory.Exists(dir))
                Directory.Delete(dir, true);
        }

        // Builds game\mods\korean from the patch exe; returns a short report. Nothing is changed unless every file
        // of the patch could be made: it is written to a ".new" folder first and swapped in at the end.
        public static string Install(Launcher launcher, string patchExe)
        {
            byte[] exe = File.ReadAllBytes(patchExe);
            int start = ZipStart(exe);
            using (var zip = new ZipArchive(new MemoryStream(exe, start, exe.Length - start, false), ZipArchiveMode.Read))
            {
                ZipArchiveEntry manifestEntry = zip.GetEntry("manifest.json");
                if (manifestEntry == null)
                    throw new InvalidDataException("manifest.json이 없습니다. 사카츠쿠 한글패치 파일이 아닙니다.");
                var manifest = (Dictionary<string, object>)new JavaScriptSerializer { MaxJsonLength = 64 << 20 }.DeserializeObject(ReadText(manifestEntry));
                object value;
                string format = manifest.TryGetValue("format", out value) ? value as string : null;
                if (format != "sakatsuku-xor-v2")
                    throw new InvalidDataException("모르는 패치 형식입니다: " + (format ?? "(없음)"));
                string version = manifest.TryGetValue("version", out value) ? value as string ?? "?" : "?";
                var files = (object[])manifest["files"];

                string dir = Dir(launcher), temp = dir + ".new";
                if (Directory.Exists(temp))
                    Directory.Delete(temp, true);
                string[] originals =
                {
                    Path.Combine(launcher.Hdd0, "game", "BLJM61064", "USRDIR", "data"),
                    Path.Combine(launcher.Bdvd, "PS3_GAME", "USRDIR", "data"),
                };
                int made = 0;
                using (SHA256 sha = SHA256.Create())
                {
                    foreach (object item in files)
                    {
                        var file = (Dictionary<string, object>)item;
                        string rel = ((string)file["path"]).Replace('/', '\\');
                        int targetSize = Convert.ToInt32(file["target_size"]);
                        string targetHash = ((string)file["target_sha256"]).ToLowerInvariant();
                        byte[] result = null;
                        foreach (string root in originals)
                        {
                            string path = Path.Combine(root, rel);
                            if (!File.Exists(path))
                                continue;
                            byte[] original = File.ReadAllBytes(path);
                            string hash = Hex(sha.ComputeHash(original));
                            if (hash == targetHash) { result = original; break; } // this copy is already the Korean one
                            foreach (object v in (object[])file["variants"])
                            {
                                var variant = (Dictionary<string, object>)v;
                                if (((string)variant["sha256"]).ToLowerInvariant() != hash)
                                    continue;
                                ZipArchiveEntry entry = zip.GetEntry((string)variant["payload"]);
                                if (entry == null)
                                    break;
                                byte[] delta = ReadBytes(entry);
                                var output = new byte[targetSize];
                                for (int i = 0; i < targetSize; i++)
                                    output[i] = (byte)((i < original.Length ? original[i] : 0) ^ (i < delta.Length ? delta[i] : 0));
                                if (Hex(sha.ComputeHash(output)) == targetHash)
                                    result = output;
                                break;
                            }
                            if (result != null)
                                break;
                        }
                        if (result == null)
                        {
                            if (Directory.Exists(temp))
                                Directory.Delete(temp, true);
                            throw new InvalidDataException(rel + ": 게임 원본 파일이 이 패치가 아는 버전과 달라 한글 파일을 만들 수 없습니다. 아무것도 바꾸지 않았습니다.");
                        }
                        string target = Path.Combine(temp, "data", rel);
                        Directory.CreateDirectory(Path.GetDirectoryName(target));
                        File.WriteAllBytes(target, result);
                        made++;
                    }
                }
                File.WriteAllText(Path.Combine(temp, "installed.txt"),
                                  "한글패치 v" + version + ", 파일 " + made + "개 (" + Path.GetFileName(patchExe) + ", " + DateTime.Now.ToString("yyyy-MM-dd HH:mm") + ")\r\n",
                                  new UTF8Encoding(false));
                if (Directory.Exists(dir))
                    Directory.Delete(dir, true);
                Directory.Move(temp, dir);
                return "한글패치 v" + version + ", 파일 " + made + "개";
            }
        }

        // Where the zip starts: end-of-central-directory position minus the directory's size and stored offset.
        static int ZipStart(byte[] b)
        {
            for (int i = b.Length - 22; i >= Math.Max(0, b.Length - 22 - 65535); i--)
            {
                if (b[i] != 0x50 || b[i + 1] != 0x4B || b[i + 2] != 0x05 || b[i + 3] != 0x06)
                    continue;
                long size = BitConverter.ToUInt32(b, i + 12), offset = BitConverter.ToUInt32(b, i + 16);
                long start = i - size - offset;
                return start > 0 && start < b.Length ? (int)start : 0;
            }
            throw new InvalidDataException("zip 데이터가 없습니다. 사카츠쿠 한글패치 파일이 아닙니다.");
        }

        static string ReadText(ZipArchiveEntry entry)
        {
            using (var reader = new StreamReader(entry.Open(), Encoding.UTF8))
                return reader.ReadToEnd();
        }

        static byte[] ReadBytes(ZipArchiveEntry entry)
        {
            using (Stream stream = entry.Open())
            using (var memory = new MemoryStream())
            {
                stream.CopyTo(memory);
                return memory.ToArray();
            }
        }

        static string Hex(byte[] hash)
        {
            var text = new StringBuilder(hash.Length * 2);
            foreach (byte b in hash)
                text.Append(b.ToString("x2"));
            return text.ToString();
        }
    }
}
