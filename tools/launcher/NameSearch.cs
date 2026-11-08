// English search over the game's Japanese names (owner, 2026-11-08: "선수나 감독 스카우트 목록을 검색할때 영어로도
// 검색이 가능하도록"). The game has no alphabet names (txt_static_data.arch: full and short names only), so katakana is
// romanized (Hepburn, small kana folded into the syllable before) and both the romaji and the typed English are
// folded to one loose spelling: sh/ch/th/ph/ck/qu/gu/nh digraphs, c/k/s, v→b, l→r, j→z, y→i, no h, doubled letters
// once. A name matches when that spelling contains the query's within a few edits, where a vowel edit costs half
// (Japanese adds vowels: "zidane" ~ ジダン, "cruyff" ~ クライフ, "ronaldo" ~ ロナウド). Names in kanji have no
// reading in the game data and are still found by Japanese text. saka_overlay.cpp (F2 menu) carries the same rules.
using System;
using System.Collections.Generic;
using System.Text;

namespace SakatsukuLauncher
{
    public static class NameSearch
    {
        static readonly string[] Katakana =
        {
            // U+30A1 ァ .. U+30F6 ヶ
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

        static bool IsVowel(char c)
        {
            return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
        }

        // Romaji of the kana in a name; other characters (kanji, spaces, ・) separate words.
        public static string Romaji(string name)
        {
            var o = new StringBuilder();
            foreach (char raw in name)
            {
                char c = raw >= 'ぁ' && raw <= 'ゖ' ? (char)(raw + 0x60) : raw;   // hiragana → katakana
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
                {
                    o.Append(char.ToLowerInvariant(c));                   // Latin letters (a legend tag's real name) as they are
                    continue;
                }
                if (c == 'ー' || c == 'ッ')   // ー and ッ: the doubled letters fold away anyway
                    continue;
                if (c < 'ァ' || c > 'ヶ')
                {
                    if (o.Length > 0 && o[o.Length - 1] != ' ')
                        o.Append(' ');
                    continue;
                }
                string r = Katakana[c - 'ァ'];
                bool smallVowel = c == 'ァ' || c == 'ィ' || c == 'ゥ' || c == 'ェ' || c == 'ォ';
                bool smallY = c == 'ャ' || c == 'ュ' || c == 'ョ';
                int n = o.Length;
                if (smallVowel && n > 0)
                {
                    if (o[n - 1] == 'u' && (n == 1 || !char.IsLetter(o[n - 2])))
                        o[n - 1] = 'w';                                   // ウィ wi, ウェ we, ウォ wo
                    else if (IsVowel(o[n - 1]))
                        o.Length = n - 1;                                 // ファ fa, ティ ti, ヴァ va, シェ she
                    o.Append(r);
                }
                else if (smallY && n > 0)
                {
                    string s = o.ToString();
                    if (s.EndsWith("shi") || s.EndsWith("chi") || s.EndsWith("ji"))
                    {
                        o.Length = n - 1;                                 // シャ sha, チュ chu, ジョ jo
                        o.Append(r.Substring(1));
                    }
                    else if (IsVowel(o[n - 1]))
                    {
                        o.Length = n - 1;                                 // キャ kya, デュ dyu
                        o.Append(r);
                    }
                    else
                        o.Append(r);
                }
                else
                    o.Append(r);
            }
            return o.ToString().Trim();
        }

        // The loose spelling romaji and English are compared in.
        public static string Key(string text)
        {
            var b = new StringBuilder();
            foreach (char c in text.ToLowerInvariant())
                if (c >= 'a' && c <= 'z')
                    b.Append(c);
            string t = b.ToString();
            t = t.Replace("sch", "s").Replace("tch", "C").Replace("ch", "C").Replace("sh", "s").Replace("ph", "f").Replace("th", "s")
                 .Replace("ck", "k").Replace("dj", "z").Replace("dz", "z").Replace("tz", "s").Replace("ts", "s").Replace("nh", "n");
            var k = new StringBuilder();
            for (int i = 0; i < t.Length; i++)
            {
                char c = t[i];
                char next = i + 1 < t.Length ? t[i + 1] : ' ';
                char after = i + 2 < t.Length ? t[i + 2] : ' ';
                if ((c == 'q' || c == 'g') && next == 'u' && (after == 'e' || after == 'i'))
                {
                    k.Append(c == 'q' ? 'k' : 'g');                       // Enrique エンリケ, Miguel ミゲル
                    i++;
                    continue;
                }
                switch (c)
                {
                    case 'c': k.Append(next == 'e' || next == 'i' || next == 'y' ? 's' : 'k'); break;
                    case 'C': k.Append('c'); break;
                    case 'q': k.Append('k'); break;
                    case 'x': k.Append("ks"); break;
                    case 'v': k.Append('b'); break;
                    case 'l': k.Append('r'); break;
                    case 'j': k.Append('z'); break;
                    case 'y': k.Append('i'); break;
                    case 'h': break;
                    default: k.Append(c); break;
                }
            }
            var o = new StringBuilder();
            foreach (char c in k.ToString())
                if (o.Length == 0 || o[o.Length - 1] != c)
                    o.Append(c);
            return o.ToString();
        }

        // Edits to find query q inside t (any start, any end); a vowel edit costs 1, any other 2.
        static int Distance(string q, string t)
        {
            int m = q.Length, n = t.Length;
            var prev = new int[n + 1];
            var cur = new int[n + 1];
            for (int i = 1; i <= m; i++)
            {
                int qc = IsVowel(q[i - 1]) ? 1 : 2;
                cur[0] = prev[0] + qc;
                for (int j = 1; j <= n; j++)
                {
                    int tc = IsVowel(t[j - 1]) ? 1 : 2;
                    int sub = q[i - 1] == t[j - 1] ? 0 : Math.Max(qc, tc);
                    cur[j] = Math.Min(Math.Min(prev[j] + qc, cur[j - 1] + tc), prev[j - 1] + sub);
                }
                var swap = prev; prev = cur; cur = swap;
            }
            int best = int.MaxValue;
            for (int j = 0; j <= n; j++)
                best = Math.Min(best, prev[j]);
            return best;
        }

        public static bool IsEnglish(string query)
        {
            bool letter = false;
            foreach (char c in query)
            {
                if (c > 127)
                    return false;
                letter |= char.IsLetter(c);
            }
            return letter;
        }

        static readonly Dictionary<string, string[]> keys = new Dictionary<string, string[]>();

        // [0] the whole name's spelling, then each word's.
        static string[] NameKeys(string name)
        {
            string[] k;
            if (!keys.TryGetValue(name, out k))
            {
                string romaji = Romaji(name);
                var list = new List<string> { Key(romaji) };
                foreach (string word in romaji.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries))
                    list.Add(Key(word));
                keys[name] = k = list.ToArray();
            }
            return k;
        }

        // -1 = no match, else an order (lower first): a whole word 0, a word's start 1, inside a word 2 + 4 per edit cost,
        // across words 4 + 4 per edit cost. "zidane" finds ジダン before ジョーダン・ヘンダーソン ("zodan endason").
        public static int Score(string name, string query)
        {
            string q = Key(query);
            if (q.Length < 2)
                return -1;
            string[] k = NameKeys(name);
            if (k[0].Length == 0)
                return -1;
            int tolerance = q.Length <= 3 ? 0 : q.Length <= 4 ? 1 : q.Length <= 7 ? 2 : q.Length <= 10 ? 3 : 4;
            int best = -1;
            for (int w = 1; w < k.Length; w++)
            {
                int order;
                if (k[w] == q)
                    order = 0;
                else if (k[w].StartsWith(q, StringComparison.Ordinal))
                    order = 1;
                else
                {
                    int d = Distance(q, k[w]);
                    if (d > tolerance)
                        continue;
                    order = 2 + 4 * d;
                }
                if (best < 0 || order < best)
                    best = order;
            }
            if (best < 0)
            {
                int d = Distance(q, k[0]);
                if (d <= tolerance)
                    best = 4 + 4 * d;
            }
            return best;
        }
    }
}
