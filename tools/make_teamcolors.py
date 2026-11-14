"""cheats/teamcolors.txt: the 70 team colors with Korean names, for the F2 menu's 팀컬러 tab (2026-11-14, owner: "팀컬러를
설정하는것에 따라서 선발 선수들의 스킬과 플레이스타일이 변하도록 하는 치트").

Japanese names: data/text/txt_static_data.arch table 19 (kind 23). Kind and what a color needs: the team color master
in the EBOOT (0x008D60A4 + id x 0x4C; see saka_hooks.c "TEAM COLOR FIT"): +2 bit 0 = the game never uses the color,
+4 five (style group, position group) pairs, +0x2C five (min, max) tactic ranges, +0x3A a count of starters by policy,
+0x3C an original skill for the KEY player. Korean names: translated here (짧은 카운터, 공간을 막아 and 측면강화 follow
the Korean patch's wording, which is what the game shows with it).

Output lines (tab separated, UTF-8):
  color <id> <korean> <japanese> <kind: line | tactics | policy> <unused 0|1> <original skill id or 0>
usage: make_teamcolors.py <port dir>"""
import glob
import os
import struct
import sys

KO = {
    1: "트리플 타워", 2: "트리플 스트라이커", 3: "트라이앵글 어택", 4: "트리플 드라이브·FW", 5: "사이드 어택·FW", 6: "고속 공격",
    7: "센터 어택·FW", 8: "박스의 패자", 9: "무자비한 3연포", 10: "1톱 2섀도", 11: "제로 톱", 12: "트리플 드라이브·MF",
    13: "더 드리블러즈", 14: "트리플 패서", 15: "티키타카", 16: "트리플 어택", 17: "라인 브레이커즈", 18: "트리플 엔진",
    19: "박스 투 박스", 20: "트리플 차지", 21: "다이너마이트 차지", 22: "측면강화·MF", 23: "철벽의 측면강화·MF",
    24: "사이드 어택·MF", 25: "노도의 사이드 어택·MF", 26: "밸런스·MF", 27: "센터 강화·MF", 28: "철벽의 센터 강화·MF",
    29: "센터 어택·MF", 30: "노도의 센터 어택·MF", 31: "패스 & 무브", 32: "매혹의 패스 & 무브", 33: "레지스타 시스템",
    34: "더블 레지스타 시스템", 35: "트리플 스토퍼", 36: "견고한 DF 콰르텟", 37: "트리플 스위퍼", 38: "신출귀몰한 DF 콰르텟",
    39: "트리플 리베로", 40: "변환자재의 DF 콰르텟", 41: "측면강화·DF", 42: "수비적인 DF 라인", 43: "난공불락의 DF 라인",
    44: "사이드 어택·DF", 45: "공격적인 DF 라인", 46: "용맹과감한 DF 라인", 47: "밸런스·DF", 48: "라인 수비",
    49: "스위퍼 시스템", 50: "짧은 카운터", 51: "롱 카운터", 52: "짧은 패스 축구", 53: "점유율 축구", 54: "포어 체크",
    55: "후퇴 수비", 56: "공간을 막아", 57: "내려서 수비", 58: "측면강화·ALL", 59: "사이드 어택·ALL", 60: "밸런스·ALL",
    61: "센터 강화·ALL", 62: "센터 어택·ALL", 63: "역습 중시", 64: "초·역습 중시", 65: "점유율 중시",
    66: "초·점유율 중시", 67: "개인능력 중시", 68: "초·개인능력 중시", 69: "조직력 중시", 70: "초·조직력 중시",
}


class Elf:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        phoff = struct.unpack_from(">Q", self.d, 0x20)[0]
        phentsize, phnum = struct.unpack_from(">HH", self.d, 0x36)
        self.segs = []
        for i in range(phnum):
            ptype, flags, off, vaddr, paddr, filesz, memsz = struct.unpack_from(">IIQQQQQ", self.d, phoff + i * phentsize)
            if ptype == 1:
                self.segs.append((vaddr, filesz, off))

    def at(self, a, n):
        for v, fs, off in self.segs:
            if v <= a and a + n <= v + fs:
                return self.d[off + a - v:off + a - v + n]
        raise ValueError(hex(a))


def static_table(path, index):
    d = open(path, "rb").read()
    n = struct.unpack_from("<I", d, 0)[0]
    t = 0x10 + 4 * n + struct.unpack_from("<I", d, 0x10 + 4 * index)[0]
    kind, count = struct.unpack_from("<HH", d, t)
    strings, out = t + 4 + 8 * count, {}
    for e in range(count):
        i, off = struct.unpack_from("<II", d, t + 4 + 8 * e)
        ln = struct.unpack_from("<H", d, strings + off)[0]
        out[i] = d[strings + off + 2:strings + off + ln].decode("utf-16-le", "replace").rstrip("\0")
    return out


def main(port):
    arch = [f for f in glob.glob(os.path.join(port, "game", "dev_bdvd", "**", "txt_static_data.arch"), recursive=True)]
    names = static_table(arch[0], 19)
    elf = Elf(os.path.join(port, "game", "EBOOT.ELF"))
    out = ["# 팀컬러 이름 (tools/make_teamcolors.py가 만듦): color id 한국어 일본어 종류(line 라인: 선발의 플레이스타일·포지션, "
           "tactics 전술 값, policy 선수 방침) 미사용(1 = 게임이 쓰지 않는 팀컬러) 키 플레이어(KEY)의 오리지널 스킬"]
    for i in range(1, 71):
        r = elf.at(0x008D60A4 + i * 0x4C, 0x4C)
        flags = struct.unpack_from(">H", r, 2)[0]
        line = any(struct.unpack_from(">I", r, 4 + 8 * k)[0] != 0x3D for k in range(5))
        kind = "line" if line else "policy" if r[0x3A] else "tactics"
        out.append("color\t%d\t%s\t%s\t%s\t%d\t%d" % (i, KO[i], names[i], kind, flags & 1, struct.unpack_from(">I", r, 0x3C)[0]))
    path = os.path.join(port, "cheats", "teamcolors.txt")
    open(path, "w", encoding="utf-8", newline="\r\n").write("\n".join(out) + "\n")
    print("wrote", path, len(out) - 1, "lines")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main(sys.argv[1])
