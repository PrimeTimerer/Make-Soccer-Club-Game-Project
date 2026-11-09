"""cheats/playstyles.txt: play styles, skill types and original skills with Korean names, for the launcher's and the F2
menu's play style editor (2026-11-09, owner: "번역도 해줘. 스킬도 마찬가지고. 그리고 이런 스타일마다 어떤게 레벨이 높고
계열중에서 하이클래스인지도 구분 되어야해").

Japanese names: data/text2/playstyle_name.txb, skill_type_name.txb, skill_name.txb (1.03 update). Tier and family: the
play style master table 0x27 in the EBOOT (67 x 0x3C at 0x00E12B08, found through the master header array 0x008D4A9C):
+0 u32 id, +4 u32 family (the base style it comes from, 1-15), +8 u32 tier (1 base, 2 upper, 3 top), +0x10.. up to 5
u32 skill types of the style. Korean names: translated here (the Korean patch writes テクニカルトラップ as 기술 트랩).

Output lines (tab separated, UTF-8):
  style <id> <korean> <japanese> <tier 1-3> <family id> <skill types, comma separated>
  type  <id> <korean> <japanese> <level-7 korean> <level-7 japanese>
  orig  <id> <korean> <japanese>
usage: make_playstyles.py <port dir>"""
import struct
import sys

STYLE_KO = {
    1: "포스트 플레이어", 2: "스트라이커", 3: "세컨드 스트라이커", 4: "윙어", 5: "드리블러", 6: "공격형 MF", 7: "패서",
    8: "중앙 MF", 9: "수비형 MF", 10: "윙백", 11: "사이드백", 12: "스토퍼", 13: "스위퍼", 14: "공격형 GK", 15: "정통파 GK",
    16: "타깃맨", 17: "센터 포워드", 18: "피니셔", 19: "무빙 FW", 20: "찬스 메이커", 21: "컷인", 22: "사이드 어태커",
    23: "트릭스터", 24: "어태커", 25: "플레이메이커", 26: "다이너모", 27: "크래셔", 28: "앵커", 29: "아처", 30: "사이드 하프",
    31: "공격형 SB", 32: "수비형 SB", 33: "하드 마커", 34: "센터백", 35: "월", 36: "리베로", 37: "카운터 GK",
    38: "라스트 포트", 39: "하이 타워", 40: "에이스 스트라이커", 41: "퍼펙트 스트라이커", 42: "멀티 피니셔",
    43: "슈팅 스타", 44: "판타지스타", 45: "와이드 스트라이커", 46: "퍼펙트 윙어", 47: "슈퍼 드리블러", 48: "톱 밑",
    49: "사령탑", 50: "패스 마스터", 51: "올액션 MF", 52: "하드 워커", 53: "레지스타", 54: "윙 하프", 55: "사이드 마스터",
    56: "스나이퍼", 57: "밸런스형 SB", 58: "철벽 SB", 59: "에어 마스터", 60: "에이스 킬러", 61: "올라운드 DF",
    62: "난공불락 DF", 63: "프리롤", 64: "수호신", 65: "귀신(鬼神)", 66: "초공격형 GK",
}
TYPE_KO = {
    1: ("컨트롤 슛", "스나이퍼 샷"), 2: ("탄환 슛", "바주카 슛"), 3: ("점핑 발리", "퍼펙트 발리"),
    4: ("하이파워 헤더", "캐논 헤더"), 5: ("컨트롤 FK", "호밍 FK"), 6: ("벨벳 패스", "브릴리언트 패스"),
    7: ("고속 피드", "퍼펙트 피드"), 8: ("컨트롤 크로스", "호밍 크로스"), 9: ("고속 드리블", "배니시 드리블"),
    10: ("테크니컬 드리블", "일루전 드리블"), 11: ("기술 트랩", "매지컬 트랩"), 12: ("테크니컬 태클", "엑설런트 태클"),
    13: ("파워 차지", "바이슨 차지"), 14: ("대시 패스컷", "스파이럴 패스컷"), 15: ("슈퍼 세이브", "퍼펙트 세이브"),
    16: ("고속 전진", "음속 전진"),
}
ORIG_KO = {
    113: "루프 슛", 114: "페인트 슛", 115: "컨트롤 발리", 116: "컨트롤 오버헤드", 117: "캐논 슛", 118: "드라이브 슛",
    119: "슬라이딩 슛", 120: "오버헤드킥", 121: "다이빙 헤더", 122: "바주카 헤더", 123: "퍼펙트 PK", 124: "바주카 FK",
    125: "택티컬 패스", 126: "기프트 패스", 127: "브릴리언트 피드", 128: "기프트 피드", 129: "브릴리언트 크로스",
    130: "기프트 크로스", 131: "돌격 드리블", 132: "음속 드리블", 133: "알까기 드리블", 134: "판타스틱 드리블",
    135: "정밀 트랩", 136: "퍼펙트 트랩", 137: "마하 태클", 138: "면도날 태클", 139: "배니시 차지", 140: "데빌 차지",
    141: "엑설런트 패스컷", 142: "퍼펙트 패스컷", 143: "엑설런트 세이브", 144: "브릴리언트 세이브", 145: "위압의 전진",
    146: "혼신의 전진", 147: "장군의 지휘", 148: "지장의 지휘", 149: "황제의 지휘", 150: "맹장의 지휘", 151: "투장의 지휘",
    152: "투장의 고무", 153: "지장의 고무", 154: "황제의 고무", 155: "절대적 공격력", 156: "절대적 수비력",
    157: "절대적 기술", 158: "절대적 신체능력", 159: "절대적 체력", 160: "컨트롤 슈터", 161: "롱 슈터",
    162: "원터치 골러", 163: "파워 헤더", 164: "프리키커", 165: "쇼트 패서", 166: "롱 패서", 167: "크로서",
    168: "고속 드리블러", 169: "테크니컬 드리블러", 170: "테크니션", 171: "볼 헌터", 172: "파워풀 마커",
    173: "볼 스틸러", 174: "빅 세이버", 175: "고속 세이버", 176: "슈퍼 서브", 177: "판타지스타", 178: "워커",
    179: "유리 에이스", 180: "페어플레이어", 181: "그레이트 GK",
}


def txb(path):
    d = open(path, "rb").read()
    kind, count = struct.unpack_from("<HH", d, 0)
    strings, out = 4 + 8 * count, {}
    for k in range(count):
        i, off = struct.unpack_from("<II", d, 4 + 8 * k)
        p = strings + off
        ln = struct.unpack_from("<H", d, p)[0]
        out[i] = d[p + 2:p + ln].decode("utf-16-le", "replace").rstrip("\0")
    return out


class Elf:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        phoff = struct.unpack_from(">Q", self.d, 0x20)[0]
        phentsize, phnum = struct.unpack_from(">HH", self.d, 0x36)
        self.segs = []
        for i in range(phnum):
            p = phoff + i * phentsize
            ptype, flags, off, vaddr, paddr, filesz, memsz = struct.unpack_from(">IIQQQQQ", self.d, p)
            if ptype == 1:
                self.segs.append((vaddr, filesz, off))

    def u32(self, a):
        for v, fs, off in self.segs:
            if v <= a and a + 4 <= v + fs:
                return struct.unpack_from(">I", self.d, off + a - v)[0]
        raise ValueError(hex(a))


def main(port):
    text = port + r"\game\dev_hdd0\game\BLJM61064\USRDIR\data\text2\\"
    styles, types, skills = txb(text + "playstyle_name.txb"), txb(text + "skill_type_name.txb"), txb(text + "skill_name.txb")
    elf = Elf(port + r"\game\EBOOT.ELF")
    hdr = elf.u32(0x008D4A9C + 0x27 * 4)
    count, base = elf.u32(hdr), elf.u32(hdr + 4)
    assert count == 67, count
    out = ["# 플레이스타일·스킬 이름 (tools/make_playstyles.py가 만듦): style id 한국어 일본어 등급(1 기본, 2 상위, 3 최상위) "
           "계열(기본 스타일 id) 스킬 종류 / type id 한국어 일본어 Lv7한국어 Lv7일본어 / orig id 한국어 일본어"]
    for i in range(1, 67):
        r = base + i * 0x3C
        assert elf.u32(r) == i
        family, tier = elf.u32(r + 4), elf.u32(r + 8)
        kinds = [elf.u32(r + 0x10 + 4 * k) for k in range(5)]
        out.append("style\t%d\t%s\t%s\t%d\t%d\t%s" % (i, STYLE_KO[i], styles[i], tier, family,
                                                     ",".join(str(k) for k in kinds if k)))
    for t in range(1, 17):
        out.append("type\t%d\t%s\t%s\t%s\t%s" % (t, TYPE_KO[t][0], types[t], TYPE_KO[t][1], skills[(t - 1) * 7 + 7]))
    for o in range(113, 182):
        out.append("orig\t%d\t%s\t%s" % (o, ORIG_KO[o], skills[o]))
    path = port + r"\cheats\playstyles.txt"
    open(path, "w", encoding="utf-8", newline="\r\n").write("\n".join(out) + "\n")
    print("wrote", path, len(out) - 1, "lines")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main(sys.argv[1])
