"""Redraw the Japanese labels of UI textures in Korean (2026-11-13, owner: "허접한 한글화를 완성", scope "main screens
first"). Data-driven: LABELS below lists, per PAR archive and DDS member, an approximate region around each label
and its Korean text. For each label the tool finds the label's own pixels in the region (alpha for sprites on a
transparent background, contrast against the region's border colour on opaque panels), erases them (transparent,
a flat fill or a patch copied from blank texture nearby) and draws the Korean text in the label's colour, fitted to
the same box. DXT5 textures are re-encoded only in the 4x4 blocks that changed; every other block keeps its original
bytes. PAR member offsets, sizes and the DDS headers stay as they were. Source: the Korean overlay copy when there is
one (game/mods/korean/data), else the game's; backups of overwritten overlay files go to logs/korean_resume_backup.

usage: .venv/Scripts/python tools/korean_ui_labels.py [--preview] [archive.par ...]   (no names: all in LABELS)
Needs Pillow with DXT5 writing (the project's .venv)."""
import struct
import sys
from io import BytesIO
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from saka_par import compress, entries, replace_payloads  # noqa: E402
from korean_refine import ROOT, OVERLAY, BACKUP, original_path  # noqa: E402

sys.stdout.reconfigure(encoding="utf-8")


def replace_relocating(data, replacements):
    """replace_payloads, but a member whose packed data outgrows its allocation moves to the end of the archive
    (2 KB aligned, as the members are): its table row's packed size and offset change, nothing else."""
    try:
        return replace_payloads(data, replacements)
    except AssertionError:
        pass
    out = bytearray(data)
    rows = list(entries(data))
    for name, table, off, packed, payload in rows:
        if name not in replacements:
            continue
        new = replacements[name]
        assert len(new) == len(payload)
        flags = struct.unpack_from(">I", out, table)[0]
        encoded = compress(new) if flags & 0x80000000 else new
        capacity = min([p for _, _, p, _, _ in rows if p > off] + [len(data)]) - off
        if len(encoded) <= capacity:
            out[off:off + capacity] = encoded + bytes(capacity - len(encoded))
            struct.pack_into(">I", out, table + 8, len(encoded))
        else:
            at = (len(out) + 0x7FF) & ~0x7FF
            out += bytes(at - len(out)) + encoded
            out += bytes(((len(out) + 0x7FF) & ~0x7FF) - len(out))
            struct.pack_into(">I", out, table + 8, len(encoded))
            struct.pack_into(">I", out, table + 12, at)
            print("    %s moved to the end (%d > %d bytes)" % (name, len(encoded), capacity))
    for name, _, _, _, payload in entries(bytes(out)):
        old = next(v for n, _, _, _, v in rows if n == name)
        assert payload == replacements.get(name, old)
    return bytes(out)

FONTS = {"bold": "C:/Windows/Fonts/malgunbd.ttf", "regular": "C:/Windows/Fonts/malgun.ttf"}
PREVIEW = ROOT / "logs/korean_ui"


def L(region, text, mode="clear", **kw):
    """region (x0, y0, x1, y1) around ONE label (no other drawing inside); mode: clear (transparent background),
    fill (flat colour of the region's border), patch (copy from the same-size area at offset kw['src'] = (dx, dy)),
    inpaint (only the label's own pixels, replaced by the median of the nearest background pixels).
    Options: vertical (one character per line), align (center/left/right), shear (italic slant, e.g. 0.18),
    rotate (degrees counter-clockwise, for tilted badge text), grow (inpaint: px added around the label's pixels),
    weight (bold/regular), color (r, g, b), size (font px), pad (erase margin, default 2), thr (contrast)."""
    return dict(region=region, text=text, mode=mode, **kw)


# ---- the labels ------------------------------------------------------------------------------------------------
END_TAPE = L((360, 18, 470, 46), "종료 시기", "fill", shear=0.12, thr=60)
LABELS = {
    "sb/practice_hizou.par": {"tex_practice_hizou.dds": [
        END_TAPE,
        L((255, 148, 300, 202), "비", "clear"), L((255, 258, 300, 300), "비", "clear"), L((318, 258, 368, 300), "비", "clear"),
        L((630, 630, 790, 712), "능력각성\n코스", "patch", src=(0, 82), thr=70),
        L((820, 630, 985, 712), "개성강화\n코스", "patch", src=(0, 82), thr=70),
    ]},
    "sb/practice_special.par": {"tex_practice_special.dds": [END_TAPE]},
    "sb/practice_all.par": {"tex_practice_all.dds": [
        L((302, 60, 410, 90), "밸런스형", shear=0.18, align="left"),
        L((302, 100, 435, 130), "공격 강화 중시", shear=0.18, align="left"),
        L((302, 138, 435, 168), "수비 강화 중시", shear=0.18, align="left"),
        L((302, 176, 492, 206), "테크닉 강화 중시", shear=0.18, align="left"),
        L((302, 214, 488, 244), "피지컬 강화 중시", shear=0.18, align="left"),
        L((302, 254, 435, 284), "전술 강화 중시", shear=0.18, align="left"),
        L((302, 292, 472, 322), "멘탈 강화 중시", shear=0.18, align="left"),
    ]},
    "sb/practice_autorest.par": {"tex_practice_autorest.dds": [
        L((272, 92, 440, 120), "지치면 바로 쉰다", shear=0.18, align="left"),
        L((272, 120, 440, 148), "적당히 쉰다", shear=0.18, align="left"),
        L((272, 148, 440, 176), "한계까지 연습한다", shear=0.18, align="left"),
        L((272, 176, 440, 204), "설정하지 않음", shear=0.18, align="left"),
    ]},
    # the office's top bar (date "5/MAY 2週 前 1年目", money "9999億 4999万円") and the money/year pickers
    "sb/core_parts.par": {
        "tex_top_bar_ed3.dds": [
            L((156, 126, 178, 145), "억", shear=0.15), L((156, 145, 178, 160), "만", shear=0.15), L((156, 160, 178, 176), "엔", shear=0.15),
            L((425, 155, 446, 176), "억", shear=0.15), L((449, 142, 470, 158), "만", shear=0.15), L((449, 158, 470, 176), "엔", shear=0.15),
            L((95, 416, 114, 432), "년"), L((95, 432, 114, 447), "차"),
            L((62, 448, 88, 468), "주"), L((2, 484, 23, 510), "전"), L((34, 484, 58, 510), "후"),
        ],
        "tex_money_select.dds": [
            L((30, 9, 49, 26), "억", shear=0.15), L((30, 26, 49, 37), "만", shear=0.15), L((30, 37, 50, 49), "엔", shear=0.15),
            L((60, 94, 80, 112), "년", shear=0.15), L((61, 111, 85, 131), "회", shear=0.15),
        ],
    },
    # player detail (big and small cards)
    "sb/player_detail.par": {"tex_player_detail_01.dds": [
        L((440, 163, 508, 189), "출전 수", align="right"), L((418, 188, 508, 212), "평균 평점", align="right"),
        L((430, 211, 508, 235), "골 수", align="right"), L((418, 233, 508, 258), "어시스트 수", align="right"),
        L((538, 307, 603, 332), "출전 수", align="right"), L((505, 331, 603, 356), "평균 평점", align="right"),
        L((515, 354, 603, 379), "골 수", align="right"), L((505, 378, 603, 403), "어시스트 수", align="right"),
        L((614, 116, 638, 138), "세"), L((699, 116, 752, 138), "주발"),
        L((519, 142, 543, 186), "계약", vertical=True), L((549, 142, 573, 186), "잔여", vertical=True),
        L((602, 161, 628, 184), "년"), L((519, 187, 543, 228), "연봉", vertical=True), L((519, 228, 543, 269), "성적", vertical=True),
        L((589, 204, 610, 225), "억", shear=0.15), L((684, 192, 706, 225), "만엔", vertical=True, shear=0.15),
        L((546, 418, 624, 438), "아시아 쿼터", "fill", thr=60), L((546, 450, 626, 470), "외국인 쿼터", "fill", thr=60),
        L((0, 499, 19, 518), "우"), L((21, 499, 42, 518), "좌"), L((43, 499, 64, 518), "양"),
    ]},
    "sb/player_detail_s.par": {
        "tex_player_detail.dds": [
            L((445, 165, 510, 189), "출전 수", align="right"), L((418, 188, 510, 212), "평균 평점", align="right"),
            L((428, 211, 510, 235), "골 수", align="right"), L((418, 233, 510, 257), "어시스트 수", align="right"),
        ],
        "tex_player_detail_S_add.dds": [
            L((4, 7, 87, 26), "아시아 쿼터", "fill", thr=60), L((4, 35, 88, 54), "외국인 쿼터", "fill", thr=60),
        ],
    },
    # tactics: the top tabs, the substitution slots, the opponent, the game plan and the in-match orders
    "sb/tactics_top.par": {
        "tex_tactics_top01.dds": [
            L((381, 13, 477, 35), "게임 플랜", "fill", thr=60), L((495, 13, 565, 35), "자동 휴양", "fill", thr=60),
            L((186, 802, 262, 830), "교체 슬롯"),
        ],
        "tex_tactics_top02.dds": [L((285, 484, 395, 512), "대전 상대")],
    },
    "sb/game_plan.par": {"tex_game_plan.dds": [
        L((272, 0, 360, 21), "선수 교체1", shear=0.15), L((272, 48, 362, 71), "선수 교체2", shear=0.15),
        L((368, 48, 460, 71), "선수 교체3", shear=0.15),
        L((265, 83, 345, 107), "전술 변경", shear=0.15), L((350, 83, 445, 107), "타이밍", shear=0.15),
        L((274, 358, 334, 382), "선수A", shear=0.15), L((377, 358, 437, 382), "선수B", shear=0.15),
        L((272, 388, 333, 412), "공격적", shear=0.15), L((377, 388, 437, 412), "수비적", shear=0.15),
        L((272, 411, 333, 433), "수비적", shear=0.15), L((377, 411, 437, 433), "공격적", shear=0.15),
        L((280, 435, 326, 457), "피로", shear=0.15), L((377, 435, 437, 457), "같은POS", shear=0.15),
        L((268, 458, 335, 482), "베테랑", shear=0.15), L((383, 458, 428, 482), "신예", shear=0.15),
        L((316, 483, 394, 507), "설정 없음", shear=0.15),
        L((572, 28, 636, 48), "공격적", "fill", thr=60), L((654, 28, 712, 48), "수비적", "fill", thr=60),
        L((730, 28, 790, 48), "같은POS", "fill", thr=60), L((812, 28, 856, 48), "피로", "fill", thr=60),
        L((876, 28, 940, 48), "베테랑", "fill", thr=60), L((962, 28, 1006, 48), "신예", "fill", thr=60),
        L((943, 90, 1022, 114), "설정 없음", shear=0.15),
        # the four grunge stamps at (490-730, 140-382) ("全ての試合で" ...) stay Japanese: their text is cut into a
        # slanted, textured bar, which a flat fill destroyed (2026-11-13 check).
        L((732, 257, 778, 281), "늦게", shear=0.15), L((808, 257, 858, 281), "보통", shear=0.15),
        L((862, 331, 910, 355), "빠르게", shear=0.15),
        L((955, 384, 1000, 404), "빠르게", shear=0.15), L((955, 404, 1000, 423), "보통", shear=0.15),
        L((955, 423, 1000, 443), "늦게", shear=0.15),
    ]},
    "sb/tactics_game_siji.par": {"tex_tactics_game_siji01.dds": [L((961, 6, 1024, 28), "공수특")]},
    # personnel: the top menu (scouting orders), coach and scout details (terms as the Korean patch's text)
    "sb/person_top.par": {"tex_person_top.dds": [
        L((3, 44, 124, 68), "스카우트 지시 중", shear=0.15, align="left"),
        L((258, 91, 442, 120), "리스트업 강화", shear=0.15, align="left"),
        L((303, 125, 400, 152), "신예발굴", shear=0.15, align="left"),
        L((303, 152, 400, 179), "비밀협상", shear=0.15, align="left"),
        L((306, 189, 372, 210), "최대 잔여"), L((400, 189, 438, 210), "잔여"),
        L((343, 219, 371, 246), "회"), L((423, 219, 453, 246), "주"),
    ]},
    "sb/coach_detail.par": {"tex_coach_detail_01.dds": [
        L((786, 8, 832, 32), "연봉"),
        L((830, 68, 868, 92), "육성"), L((874, 68, 914, 92), "인망"), L((918, 68, 957, 92), "지휘"), L((962, 68, 1001, 92), "전술"),
        L((1002, 52, 1024, 118), "잔여년", vertical=True),
        L((690, 151, 738, 172), "중앙", shear=0.15, align="right"), L((770, 151, 824, 172), "사이드", shear=0.15, align="left"),
        L((650, 175, 738, 196), "점유율", shear=0.15, align="right"), L((770, 175, 810, 196), "속공", shear=0.15, align="left"),
        L((652, 198, 738, 219), "후퇴 수비", shear=0.15, align="right"), L((770, 198, 824, 219), "프레스", shear=0.15, align="left"),
        L((680, 221, 738, 242), "커버", shear=0.15, align="right"), L((770, 221, 838, 242), "플랫", shear=0.15, align="left"),
        L((684, 434, 705, 455), "억", shear=0.15), L((784, 428, 803, 458), "만엔", vertical=True, shear=0.15),
    ]},
    "sb/scout_detail.par": {"scout_detail_tex01.dds": [
        L((24, 120, 48, 144), "년"), L((48, 118, 72, 160), "잔여", vertical=True),
        L((22, 160, 70, 184), "일본", align="left"), L((22, 185, 120, 209), "아시아 동부", align="left"),
        L((22, 209, 120, 233), "아시아 서부", align="left"), L((22, 233, 126, 257), "잉글랜드", align="left"),
        L((22, 257, 96, 281), "프랑스", align="left"), L((22, 281, 82, 305), "독일", align="left"),
        L((22, 305, 96, 329), "이탈리아", align="left"),
        L((128, 160, 204, 184), "스페인", align="left"), L((128, 185, 204, 209), "네덜란드", align="left"),
        L((128, 209, 236, 233), "서유럽", align="left"), L((128, 233, 236, 257), "동유럽", align="left"),
        L((128, 257, 176, 281), "남미", align="left"), L((128, 281, 202, 305), "아프리카", align="left"),
        L((128, 305, 192, 329), "북중미", align="left"),
        L((234, 295, 271, 314), "선수", shear=0.15), L((273, 295, 309, 314), "교섭", shear=0.15),
        L((309, 295, 343, 314), "신예", shear=0.15), L((232, 313, 290, 334), "코치", shear=0.15),
        L((342, 298, 359, 317), "억", shear=0.15), L((359, 296, 377, 327), "만엔", vertical=True, shear=0.15),
        L((376, 298, 416, 322), "연봉", shear=0.15),
    ]},
    # schedule: week halves, home/away, the promotion panel
    "sb/schedule.par": {"tex_schedule.dds": [
        L((68, 115, 90, 138), "전", "fill", thr=60), L((68, 161, 92, 189), "후", "fill", thr=60),
        L((41, 149, 63, 173), "주", "patch", src=(0, -30), thr=40),
        L((955, 207, 1003, 232), "중립"),
        L((687, 330, 768, 355), "홈 경기", shear=0.15), L((770, 330, 847, 355), "원정", shear=0.15),
        L((848, 330, 891, 355), "중립", shear=0.15),
        L((779, 549, 962, 581), "영업부 조언", "patch", src=(0, 40), thr=45),
        L((784, 779, 956, 806), "보유 프로모션", "patch", src=(0, -40), thr=45),
        L((612, 1005, 866, 1024), "※캠프 마지막 주에 경기가 열립니다.", align="left"),
    ]},
    # match result: supporters, income report, player ranking, match report
    "sb/game_result.par": {"game_result_tex.dds": [
        L((312, 133, 494, 157), "서포터 획득 정보"),
        L((501, 136, 579, 155), "경기 내용", "fill", thr=60), L((585, 136, 613, 155), "선수", "fill", thr=60),
        L((616, 136, 649, 155), "시설", "fill", thr=60),
        L((312, 159, 399, 177), "이번 관중", "fill", thr=60), L((404, 156, 518, 178), "이번 일반 관객"),
        L((526, 159, 659, 177), "프로모션", "fill", thr=60),
        L((310, 179, 447, 201), "이번 획득 서포터"), L((448, 179, 579, 201), "전체 서포터 수"),
        L((579, 179, 662, 201), "종합 평가"),
        L((310, 203, 568, 216), "※위 결과는 관중 설문을 집계한 것입니다.", align="left"),
        L((762, 229, 820, 249), "골 수", align="right"),
        L((672, 249, 714, 270), "순위"), L((714, 249, 768, 270), "선수명"), L((768, 249, 820, 270), "클럽"),
        L((722, 318, 820, 339), "평가 코멘트", align="right"), L((760, 337, 818, 353), "평점", align="right"),
        L((760, 352, 818, 369), "선수명", align="right"),
        L((10, 326, 152, 351), "경기 후 수지 보고", "fill", thr=60), L((152, 326, 226, 351), "시설 수입", "fill", thr=60),
        L((27, 360, 102, 381), "수입 정보", "fill", thr=60), L((163, 360, 240, 381), "기본 정보", "fill", thr=60),
        L((0, 385, 72, 409), "수지 합계", "fill", thr=60), L((72, 385, 162, 409), "티켓 수입", "fill", thr=60),
        L((162, 385, 271, 409), "기타 수입", "fill", thr=60),
        L((0, 408, 109, 429), "기타 지출", "fill", thr=60), L((110, 408, 221, 429), "티켓 요금", "fill", thr=60),
        L((0, 429, 74, 449), "관중 수", "fill", thr=60),
        L((103, 447, 123, 467), "억", shear=0.15), L((199, 433, 223, 467), "만엔", vertical=True, shear=0.15),
        L((187, 470, 230, 494), "지출"), L((230, 470, 275, 494), "수입"),
        L((314, 458, 382, 480), "골 수"), L((383, 458, 484, 480), "어시스트 수"), L((484, 458, 541, 480), "평점"),
        L((541, 458, 669, 480), "매치 리포트"), L((669, 458, 808, 480), "HOME 선수 평가"),
        L((505, 483, 613, 505), "슈팅(유효)"), L((613, 483, 659, 505), "골"), L((660, 483, 808, 505), "AWAY 선수 평가"),
        L((5, 893, 139, 915), "매치 리포트", shear=0.15), L((139, 893, 180, 915), "경기 결과", shear=0.15),
        L((7, 968, 68, 990), "골 수", shear=0.15), L((82, 968, 156, 990), "어시스트 수", shear=0.15),
        L((174, 968, 223, 990), "평점", shear=0.15),
        L((853, 830, 948, 850), "오프사이드", align="left"),
        L((783, 853, 889, 875), "경고(퇴장)"), L((889, 853, 940, 875), "점유율"),
    ]},
}
# the remaining screens (2026-11-14, owner: "남은 화면 한글화도 이어서"): generated label data in korean_ui_more.py
try:
    from korean_ui_more import MORE  # noqa: E402
except ImportError:
    MORE = {}
for _rel, _members in MORE.items():
    LABELS.setdefault(_rel, {}).update(_members)


# ---- image work --------------------------------------------------------------------------------------------------
def median(values):
    s = sorted(values)
    return s[len(s) // 2] if s else 0


def find_ink(im, lab):
    """The label's pixels in its region: (mask set of (x, y) absolute, bbox, ink colour, background colour)."""
    x0, y0, x1, y1 = lab["region"]
    px = im.load()
    pts = [(x, y) for y in range(y0, y1) for x in range(x0, x1)]
    if lab.get("ink") == "light":   # light text on a dark opaque disc/tile (badges): bright opaque pixels only
        lum = lab.get("lum", 150)
        ink = {p for p in pts if px[p][3] > 128 and (px[p][0] * 3 + px[p][1] * 6 + px[p][2]) // 10 > lum}
        if not ink:
            raise ValueError("no label pixels in %s (%s)" % (lab["region"], lab["text"]))
        rest = [px[p] for p in pts if p not in ink and px[p][3] > 128] or [px[p] for p in pts]
        bg = tuple(median([c[k] for c in rest]) for k in range(4))
        xs, ys = [q[0] for q in ink], [q[1] for q in ink]
        color = lab.get("color") or tuple(median([px[q][k] for q in ink]) for k in range(3))
        return ink, (min(xs), min(ys), max(xs) + 1, max(ys) + 1), color, bg
    if lab["mode"] == "clear":
        # the background: the border's median colour WITH its alpha (fully transparent, or a faint panel such as
        # the coach detail's tactic bars at alpha 24-51), so erasing restores the panel instead of punching a hole
        border = [px[x, y0] for x in range(x0, x1)] + [px[x, y1 - 1] for x in range(x0, x1)] + \
                 [px[x0, y] for y in range(y0, y1)] + [px[x1 - 1, y] for y in range(y0, y1)]
        bg = tuple(median([c[k] for c in border]) for k in range(4))
        cut = max(60, min(250, bg[3] + 50))
        ink = {(x, y) for (x, y) in pts if px[x, y][3] > cut}
        strong = [px[p] for p in ink if px[p][3] > 200] or [px[p] for p in ink]
    else:
        border = [px[x, y0] for x in range(x0, x1)] + [px[x, y1 - 1] for x in range(x0, x1)] + \
                 [px[x0, y] for y in range(y0, y1)] + [px[x1 - 1, y] for y in range(y0, y1)]
        bg = tuple(median([c[k] for c in border]) for k in range(4))
        thr = lab.get("thr", 50)

        def diff(c):
            return max(abs(c[k] - bg[k]) for k in range(3))
        ink = {p for p in pts if diff(px[p]) > thr}
        if lab.get("lines"):   # drop strokes that run out of the region (ribbon outlines around arched text)
            left, seen = set(ink), set()
            for start in list(ink):
                if start in seen:
                    continue
                comp, stack = [], [start]
                seen.add(start)
                while stack:
                    x, y = stack.pop()
                    comp.append((x, y))
                    for q in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1), (x + 1, y + 1), (x - 1, y - 1), (x + 1, y - 1), (x - 1, y + 1)):
                        if q in ink and q not in seen:
                            seen.add(q)
                            stack.append(q)
                if any(x in (x0, x1 - 1) or y in (y0, y1 - 1) for x, y in comp):
                    left -= set(comp)
            ink = left
        ranked = sorted(ink, key=lambda p: -diff(px[p]))
        strong = [px[p] for p in ranked[: max(1, len(ranked) // 3)]]
    if not ink:
        raise ValueError("no label pixels in %s (%s)" % (lab["region"], lab["text"]))
    xs, ys = [p[0] for p in ink], [p[1] for p in ink]
    bbox = (min(xs), min(ys), max(xs) + 1, max(ys) + 1)
    color = lab.get("color") or tuple(median([c[k] for c in strong]) for k in range(3))
    return ink, bbox, color, bg


def inpaint(im, lab, ink, bg):
    """Replace only the label's pixels (grown by `grow` px for anti-aliased edges) with the per-channel median of
    the nearest non-label pixels, so textured paper, grunge and semi-transparent panels keep their look instead of
    getting a flat patch. Pixels that look like ink (same test as the label's) are never sampled, so neighbouring
    labels outside the region do not bleed in."""
    x0, y0, x1, y1 = lab["region"]
    W, H = im.size
    px = im.load()
    mask = set(ink)
    for _ in range(lab.get("grow", 1)):
        mask |= {(x + dx, y + dy) for (x, y) in mask for dx in (-1, 0, 1) for dy in (-1, 0, 1)}
    mask = {(x, y) for (x, y) in mask if x0 <= x < x1 and y0 <= y < y1}
    transparent = lab.get("transparent")
    if lab.get("ink") == "light":
        lum = lab.get("lum", 150)

        def inky(c):
            return c[3] > 128 and (c[0] * 3 + c[1] * 6 + c[2]) // 10 > lum
    elif transparent:
        cut = max(60, min(250, bg[3] + 50))

        def inky(c):
            return c[3] > cut
    else:
        thr = lab.get("thr", 50)

        def inky(c):
            return max(abs(c[k] - bg[k]) for k in range(3)) > thr
    src = im.copy()
    spx = src.load()
    for (x, y) in mask:
        vals = []
        for r in (2, 3, 4, 6, 8, 12):
            vals = [spx[i, j] for j in range(max(0, y - r), min(H, y + r + 1)) for i in range(max(0, x - r), min(W, x + r + 1))
                    if (i, j) not in mask and not inky(spx[i, j]) and (transparent or spx[i, j][3] >= 128)]
            if len(vals) >= 6:
                break
        if vals:
            px[x, y] = tuple(sorted(v[k] for v in vals)[len(vals) // 2] for k in range(4))
        else:
            px[x, y] = bg if bg[3] >= 8 else (0, 0, 0, 0)


def erase(im, lab, bbox, bg):
    x0, y0, x1, y1 = lab["region"]
    pad = lab.get("pad", 2)
    bx0, by0, bx1, by1 = max(x0, bbox[0] - pad), max(y0, bbox[1] - pad), min(x1, bbox[2] + pad), min(y1, bbox[3] + pad)
    px = im.load()
    if lab["mode"] == "clear":
        fillc = bg if bg[3] >= 8 else (0, 0, 0, 0)
        for y in range(by0, by1):
            for x in range(bx0, bx1):
                px[x, y] = fillc
    elif lab["mode"] == "fill":
        for y in range(by0, by1):
            for x in range(bx0, bx1):
                px[x, y] = bg
    else:   # patch: copy from blank texture at an offset
        dx, dy = lab["src"]
        src = im.crop((bx0 + dx, by0 + dy, bx1 + dx, by1 + dy))
        im.paste(src, (bx0, by0))
    return (bx0, by0, bx1, by1)


def text_layer(lab, size, color):
    font = ImageFont.truetype(FONTS[lab.get("weight", "bold")], size)
    lines = list(lab["text"]) if lab.get("vertical") else lab["text"].split("\n")
    probe = ImageDraw.Draw(Image.new("L", (1, 1)))
    boxes = [probe.textbbox((0, 0), ln, font=font) for ln in lines]
    lh = max(b[3] - b[1] for b in boxes)
    gap = max(1, size // 8)
    w = max(b[2] - b[0] for b in boxes)
    h = lh * len(lines) + gap * (len(lines) - 1)
    # the glyphs as a coverage mask, slanted as a mask, then one solid colour: interpolating RGBA directly gave the
    # nearly transparent edge pixels stray colours (e.g. (127, 0, 0, 2)) that spoiled whole DXT5 blocks
    mask = Image.new("L", (w + 8, h + 8), 0)
    d = ImageDraw.Draw(mask)
    align = lab.get("align", "center")
    for i, (ln, b) in enumerate(zip(lines, boxes)):
        lw = b[2] - b[0]
        x = 4 + (0 if align == "left" else w - lw if align == "right" else (w - lw) // 2) - b[0]
        y = 4 + i * (lh + gap) - b[1]
        d.text((x, y), ln, font=font, fill=255)
    shear = lab.get("shear", 0)
    if shear:
        extra = int(abs(shear) * mask.height) + 1
        wide = Image.new("L", (mask.width + extra, mask.height), 0)
        wide.paste(mask, (0, 0))
        mask = wide.transform(wide.size, Image.AFFINE, (1, shear, -shear * mask.height, 0, 1, 0), Image.BICUBIC)
    if lab.get("rotate"):   # degrees, counter-clockwise (badges with tilted text)
        mask = mask.rotate(lab["rotate"], resample=Image.BICUBIC, expand=True)
    layer = Image.new("RGBA", mask.size, tuple(color) + (0,))
    layer.putalpha(mask)
    box = mask.getbbox()
    return layer.crop(box) if box else layer


def draw_label(im, lab):
    if lab.get("draw_only"):   # text only, on background other labels of the list already cleared
        x0, y0, x1, y1 = lab["region"]
        size = lab["size"]
        layer = text_layer(lab, size, lab["color"])
        while (layer.width > x1 - x0 or layer.height > y1 - y0) and size > 8:
            size -= 1
            layer = text_layer(lab, size, lab["color"])
        im.alpha_composite(layer, ((x0 + x1 - layer.width) // 2, (y0 + y1 - layer.height) // 2))
        return lab["region"], size, lab["color"]
    if lab["mode"] == "inpaint":   # sprite on transparency (gaps between strokes) -> alpha ink, else colour ink
        x0, y0, x1, y1 = lab["region"]
        px = im.load()
        tr = lab.get("ink") != "light" and             sum(1 for y in range(y0, y1) for x in range(x0, x1) if px[x, y][3] < 8) > (x1 - x0) * (y1 - y0) * 0.15
        ink, bbox, color, bg = find_ink(im, dict(lab, mode="clear" if tr else "fill"))
        inpaint(im, dict(lab, transparent=tr), ink, bg)
    else:
        ink, bbox, color, bg = find_ink(im, lab)
        erase(im, lab, bbox, bg)
    if not lab["text"]:   # erase only (the text comes from a draw_only label)
        return bbox, 0, color
    x0, y0, x1, y1 = lab["region"]
    bw, bh = bbox[2] - bbox[0], bbox[3] - bbox[1]
    n = len(lab["text"]) if lab.get("vertical") else lab["text"].count("\n") + 1
    size = lab.get("size") or max(8, int(bh / n * 1.05))
    while True:   # fit: the ink box height, the region's width
        layer = text_layer(lab, size, color)
        if (layer.height <= bh + 2 and layer.width <= (x1 - x0)) or size <= 8:
            break
        size -= 1
    align = lab.get("align", "center")
    cx = bbox[0] if align == "left" else bbox[2] - layer.width if align == "right" else (bbox[0] + bbox[2] - layer.width) // 2
    cx = max(x0, min(cx, x1 - layer.width))
    cy = (bbox[1] + bbox[3] - layer.height) // 2
    im.alpha_composite(layer, (cx, max(y0, cy)))
    return bbox, size, color


# ---- DDS ---------------------------------------------------------------------------------------------------------
def dds_info(payload):
    h, w = struct.unpack_from("<II", payload, 12)
    fourcc = payload[84:88]
    return w, h, fourcc


def encode_like(original, im):
    """New DDS payload: same header; DXT5 blocks re-encoded only where the 4x4 pixels changed."""
    w, h, fourcc = dds_info(original)
    if fourcc == b"\0\0\0\0":   # uncompressed BGRA (main_menu)
        bits = struct.unpack_from("<I", original, 88)[0]
        assert bits == 32 and len(original) >= 128 + w * h * 4
        return original[:128] + im.tobytes("raw", "BGRA") + original[128 + w * h * 4:]
    assert fourcc == b"DXT5", fourcc
    old = Image.open(BytesIO(original)).convert("RGBA")
    opx, npx = old.load(), im.load()
    bw = w // 4
    blocks = [(bx, by) for by in range(h // 4) for bx in range(bw)
              if any(opx[bx * 4 + i, by * 4 + j] != npx[bx * 4 + i, by * 4 + j] for j in range(4) for i in range(4))]
    # the encoder weighs every pixel's colour alike: give the nearly transparent pixels of a changed block the
    # block's alpha-weighted colour, so their (invisible) RGB cannot pull the block's two colour endpoints away
    clean = im.copy()
    cpx = clean.load()
    for bx, by in blocks:
        pts = [(bx * 4 + i, by * 4 + j) for j in range(4) for i in range(4)]
        heavy = [npx[p] for p in pts if npx[p][3] >= 32]
        if heavy and len(heavy) < 16:
            wt = sum(c[3] for c in heavy)
            mean = tuple(int(round(sum(c[k] * c[3] for c in heavy) / wt)) for k in range(3))
            for p in pts:
                if npx[p][3] < 32:
                    cpx[p] = mean + (npx[p][3],)
    buf = BytesIO()
    clean.save(buf, "DDS", pixel_format="DXT5")
    enc = buf.getvalue()[128:128 + (w // 4) * (h // 4) * 16]
    out = bytearray(original)
    for bx, by in blocks:
        k = (by * bw + bx) * 16
        out[128 + k:128 + k + 16] = enc[k:k + 16]
    return bytes(out), len(blocks)


CREATED = PREVIEW / "created.txt"   # overlay files this tool made (no Korean patch file under them)


def source_of(rel):
    """The untouched source: the game's file for an overlay this tool made, else the Korean patch's overlay (kept in
    the backup folder the first time), else the game's file. So a second run never starts from its own output."""
    dest, backup = OVERLAY / rel, BACKUP / rel
    created = set(CREATED.read_text(encoding="utf-8").split()) if CREATED.exists() else set()
    if rel in created:
        return original_path(rel).read_bytes()
    if backup.exists():
        return backup.read_bytes()
    if dest.exists():
        backup.parent.mkdir(parents=True, exist_ok=True)
        backup.write_bytes(dest.read_bytes())
        return backup.read_bytes()
    PREVIEW.mkdir(parents=True, exist_ok=True)
    with CREATED.open("a", encoding="utf-8") as f:
        f.write(rel + "\n")
    return original_path(rel).read_bytes()


def process(rel, members, preview):
    dest = OVERLAY / rel
    source = source_of(rel)
    repl = {}
    for name, _, _, _, payload in entries(source):
        if name not in members:
            continue
        im = Image.open(BytesIO(payload)).convert("RGBA")
        before = im.copy()
        notes = []
        for lab in members[name]:
            try:
                bbox, size, color = draw_label(im, lab)
            except ValueError as e:   # one bad region must not stop the archive (the label stays Japanese)
                print("    SKIPPED", e)
                continue
            notes.append("%s %s %dpx" % (lab["text"].replace("\n", "/"), bbox, size))
        result = encode_like(payload, im)
        new, changed = result if isinstance(result, tuple) else (result, -1)
        repl[name] = new
        print("%s %s: %d labels, %s blocks re-encoded" % (rel, name, len(members[name]), changed))
        for n in notes:
            print("   ", n)
        if preview:
            PREVIEW.mkdir(parents=True, exist_ok=True)
            check = Image.open(BytesIO(new)).convert("RGBA")
            side = Image.new("RGBA", (im.width * 2 + 8, im.height), (90, 96, 104, 255))
            for i, pic in enumerate((before, check)):
                bgp = Image.new("RGBA", pic.size, (90, 96, 104, 255))
                bgp.alpha_composite(pic)
                side.paste(bgp, (i * (im.width + 8), 0))
            side.convert("RGB").save(PREVIEW / ("%s__%s.png" % (Path(rel).stem, Path(name).stem)))
    missing = set(members) - set(repl)
    assert not missing, missing
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(replace_relocating(source, repl))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    preview = "--preview" in sys.argv
    for rel, members in LABELS.items():
        if not args or rel in args or Path(rel).name in args:
            process(rel, members, preview)


if __name__ == "__main__":
    main()
