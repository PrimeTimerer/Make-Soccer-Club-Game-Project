"""PS Vita VitaCheat list (PCSG00198.psv) -> the PS3 port's cheats/BLJM61064.txt, by category (YKTK3 launcher style).

The Vita game state (0x81BFxxxx-0x81CBxxxx) and the PS3 one (0x00EExxxx-0x00F9xxxx) have the same layout, one constant
apart: money Vita 0x81BFEB44 = PS3 0x00EE2844 (found by its value, the office's funds), the club roster Vita 0x81BFEC54
= PS3 0x00EE2954 (23 x 0x218-byte player records, IDs 0x25F..). Field widths and value ranges below were checked in the
PS3 code with REA's Ghidra (NOTES.md "Cheats", 2026-11-08): a byte write of the Vita (little-endian) lands in the low
byte of a wider field there, which is the last byte on the big-endian PS3, so such writes are rewritten for the field.
Player record: +0 u32 ID, +4 u8 awakening level (0-15), +5 u8 age, +7 u8 contract years, +0x0C 46 x u8 abilities
(0-100, func_005773A8 clamps), +0x3B 46 x u8 limits (0-100), +0x6A 46 x u16 ability experience, +0xC6 u16 awakening
points, +0xC8/+0xCA/+0xCC u16 condition/fatigue (0-1000), +0xE4 2 x u16 policy, +0x108 u16 star power (0-0xFFFE),
+0x202 u8, +0x208 u32 growth type (0-7, func_004C9880).
Output lines "#@cat", "#@note", "#@value n|label|min|max|kind", "#@once" are the launcher's metadata (the game's
cheat thread skips # lines). Kinds C/D are the port's "write only if memory holds a larger value" (saka_hooks.c).
Usage: psv_to_ps3.py PCSG00198.psv cheats/BLJM61064.txt"""
import re
import sys

DELTA = 0x81BFEB44 - 0x00EE2844
ROSTER, RECORD = 0x00EE2954, 0x218
# The youth team (2026-11-09, owner: "유스 선수들이 매 턴마다 각성을 하는 수준으로 성장"): 15 player records of the same
# 0x218-byte layout at 0x00EE86E0 (the game checks youth slots < 0xF; 15 x 0xA0 of youth data follow at 0x00EEA648;
# live: five 16-year-olds). The Vita cheats only wrote the 23 first-team records.
YOUTH, YOUTH_SLOTS = 0x00EE86E0, 15


def all_players(off, size, value, count=23, base=ROSTER):
    """A multi-write over the 23 roster records (or count records from base): size 1/2/4 at record offset off."""
    t = {1: "40", 2: "41", 4: "42"}[size]
    return ["$%s01 %08X %08X" % (t, base + off, value), "$%04X %08X 00000000" % (count, RECORD)]


def span(first, n, value, count=23, base=ROSTER):
    """n bytes of value from record offset first, as u32 writes and one u16/u8 tail, over the 23 records."""
    codes, off = [], first
    while n >= 4:
        codes += all_players(off, 4, value * 0x01010101, count, base)
        off += 4
        n -= 4
    if n >= 2:
        codes += all_players(off, 2, value * 0x0101, count, base)
        off += 2
        n -= 2
    if n:
        codes += all_players(off, 1, value, count, base)
    return codes


# (category, Korean title, origin, codes or None = moved from the .psv as it is, note, values, once)
# once: the list replacements are written once when turned on unless 고정 is chosen (owner, 2026-11-08: no lock
# before the user asks for one).
LOCKABLE_SWITCHES = {"EDIT 99"}   # code switches whose hook honours 고정 / 갱신 (saka_hooks.c, switch 7)
CHEATS = [
    ("자금·구단", "자금 설정", "cheat1MONY", ["$0200 00EE2844 05F5E0FF"],
     "구단 자금 int32 (0x00EE2844), 만엔 단위. 99,999,999 = 9999억 9999만엔. 고정이면 게임이 돈을 바꾸는 세 곳 "
     "(func_0053F3A0 / func_0053F434 / func_00544558) 바로 뒤에서도 다시 넣어, 써도 줄지 않고 화면 금액도 그대로.",
     ["0|자금 (만엔)|0|99999999|number"], False),
    ("자금·구단", "클럽 우호도 최대", "UHODO", ["$4001 00F8E032 000000FF", "$0117 00000001 00000000"],
     "클럽 280개마다 1바이트 (0x00F8E032~). 0xFF는 게임이 '항상'으로 쓰는 값 (func_00518F48).", [], False),
    ("GM(주인공)", "GM(감독) 스타일 전부 해금", "PS3",
     ["$4201 00F8D018 00000001", "$0010 000000FC 00000000",    # +0xF4 bit 0: unlocked
      "$4001 00F8D01C 0000000A", "$0010 000000FC 00000000",    # +0xF8 u8 level 10 (func_00505F30)
      "$4201 00F8D020 000057E4", "$0005 000000FC 00000000",    # +0xFC u32 exp: styles 1-5 Lv10 = 22500
      "$4201 00F8D50C 0000AFC8", "$000B 000000FC 00000000"],   # styles 6-16 Lv10 = 45000 (func_00506300)
     "주인공 GM 데이터 (0x00F8CE28: 지금 스타일 번호, 스타일 i = 0x00F8CE28 + i x 0xFC). 스타일 1~16 전부: +0xF4 비트 0 '해금' "
     "(func_00505FE0 / func_00506284), +0xF8 레벨 10, +0xFC 경험치 Lv10 값 (레벨당 1~5번 2500, 6~16번 5000; 마스터 0x90). "
     "Lv.0 스타일은 효과가 없어서 (func_005063B0이 레벨 1~L의 스킬을 합침) 전부 Lv10으로. 스킬 반영은 게임이 GM 경험치를 "
     "줄 때(경기·턴 뒤)나 스타일을 바꿀 때.", [], False),
    ("에디트 선수", "에디트 선수 보너스 포인트 99 [코드 스위치]", "EDIT 99", ["$F007 00000000 00000063"],
     "타이틀의 EDIT > 선수 에디트에서 새 선수를 만들 때의 포인트 (0~99, 게임 최대 99). 메뉴 객체(힙) +0x8A8에 있어 고정 주소로는 "
     "못 씀: 시작값 50을 넣는 func_003CB8E8 뒤에서 이 값으로. 고정이면 항목 레벨(-10)·능력(-1)에 써도 그대로 (func_003CC0D4의 "
     "쓰기 5곳과 슬롯 보너스 func_003CAD38 뒤에서 다시 넣음). 고정을 끄면 새 선수 시작 때, 켤 때, 값을 바꿀 때, 갱신 때 한 번 "
     "넣고 에디트 화면 안이면 바로 반영.", ["0|포인트|0|99|number"], False),
    ("서포터", "서포터 수·코어 서포터 최대", "SUPPOTERS + CORE SUPPO",
     ["$0200 00EE7B04 00030D40", "$0200 00EE7B08 00002710"],
     "서포터 수 int32 (0x00EE7B04, 게임 최대 200,000: func_00542EE4), 코어 서포터 int32 (0x00EE7B08, 게임 최대 10,000: "
     "func_0054587C). 지금 구단의 값이라 GM이 구단을 옮기면 옮긴 구단에 적용.",
     ["0|서포터 수|0|200000|number", "1|코어 서포터|0|10000|number"], False),
    ("선수 전원", "피로 회복", "PLAYER 23 HIROU", None,
     "+0xC8 컨디션 1000, +0xCA 피로 0, +0xCC 1000 (u16, 게임 범위 0~1000).", [], False),
    ("선수 전원", "각성 포인트 최대", "GAKSUNG 100", None,
     "+0xC6 u16. 게임은 다음 각성 레벨 기준값까지만 인정하고 각성 대기 표시를 켬 (func_00568B08). AWAKENING POINT 게이지는 "
     "레벨 사이 비율이라 (func_00568C60, 기준값 표 0x54) 레벨이 15(끝)이면 늘 0%: '각성 레벨' 치트를 15로 같이 켜면 0%로 "
     "보이고 더 각성하지 않음. 각성 이벤트를 보려면 '각성 레벨'은 끄고 이것만 켬.", [], False),
    ("선수 전원", "각성 레벨", "PLAYER GRADE RATE", all_players(0x04, 1, 15),
     "+4 u8 (0~15, func_00568B08). 선수마다 정해진 최대 레벨을 넘으면 게임이 더 올리지 않음. 15는 끝 레벨이라 각성 포인트 "
     "게이지가 0%로 보이고 각성 이벤트가 더 없음 (게임 설계).",
     ["0|각성 레벨|0|15|number"], False),
    ("선수 전원", "방침 중앙", "POLICY CENTER", None, "+0xE4 u16 두 개 (0x7FFF, 0x4FFF).", [], False),
    ("선수 전원", "나이", "AGE 16", all_players(0x05, 1, 16),
     "+5 u8. 성장 곡선은 15~50세 (표 0x6D~0x6F, 51칸).", ["0|나이|15|50|number"], False),
    ("선수 전원", "계약 연수", "CONTRACT 3", all_players(0x07, 1, 3), "+7 u8.", ["0|계약 연수|1|9|number"], False),
    ("선수 전원", "능력치 최대 (100)", "STAT MAX", span(0x0C, 46, 100),
     "능력 46개 +0x0C~+0x39 (u8). 게임 최대 100 (func_005773A8). Vita 코드는 0xCD(205)를 +0x3A(계약 관련 칸)까지 썼음: 100으로, "
     "+0x3A는 빼고 수정.", [], False),
    ("선수 전원", "능력 한계 최대 (100)", "allplayer potential max", span(0x3B, 46, 100),
     "한계 46개 +0x3B~+0x68 (u8). 게임 최대 100 (func_00512910). Vita 코드의 205와 +0x69를 수정.", [], False),
    ("선수 전원", "능력 경험치 최대 1", "allplayer stat exp max 1", None, "+0x6A~ u16 (능력 0~25).", [], False),
    ("선수 전원", "능력 경험치 최대 2", "allplayer stat exp max 2", None, "+0x9E~ u16 (능력 26~45).", [], False),
    ("선수 전원", "스타성 최대", "PLAYER STARSHIP MAX", all_players(0x108, 2, 0xFFFE),
     "+0x108 u16. 게임 최대 0xFFFE (func_00590DC8이 0~0xFFFE로 자름).", [], False),
    ("선수 전원", "성장 타입", "PLAYER GROWTH TYPE", all_players(0x208, 4, 7),
     "+0x208 u32 성장 곡선 (0~7, func_004C9880).", ["0|성장 타입|0|7|number"], False),
    ("선수 전원", "쇠퇴 없음", "all player decrease -2(positive)",
     all_players(0x208, 4, 7) + all_players(0x202, 1, 0),
     "성장 타입 7 (+0x208 u32) + +0x202 = 0. 이전 변환은 4바이트 칸의 첫 바이트에 7을 써서 없는 타입(0x07000000+)이 됐음: 수정.",
     [], False),
    # 2026-11-09 (owner: "유스 선수들이 매 턴마다 각성을 하는 수준으로 성장해. psv파일에 봐바"): the .psv only wrote the 23
    # first-team records; the youth team is 15 records of the same layout at 0x00EE86E0 (REA Ghidra: func_00519BC0,
    # the youth menu copies 0x28C8 bytes from there; slots < 0xF).
    ("유스", "유스 각성 포인트 최대", "PS3 YOUTH",
     all_players(0xC6, 2, 0xFFFF, YOUTH_SLOTS, YOUTH),
     "유스 15칸 (0x00EE86E0, 1군과 같은 0x218바이트 선수 기록)의 +0xC6 u16. 게임은 다음 각성 기준값까지 인정하고 각성 대기 "
     "표시를 켬 (func_00568B08). '각성 포인트 배수' 스위치도 유스 선수에게 적용.", [], False),
    ("유스", "유스 능력 한계 최대 (100)", "PS3 YOUTH", span(0x3B, 46, 100, YOUTH_SLOTS, YOUTH),
     "유스 15칸의 한계 46개 +0x3B~+0x68 (u8, 게임 최대 100).", [], False),
    ("유스", "유스 능력 경험치 최대", "PS3 YOUTH", span(0x6A, 92, 0xFF, YOUTH_SLOTS, YOUTH),
     "유스 15칸의 능력 경험치 46개 +0x6A~ (u16 0xFFFF): 다음 성장 때 한계까지 오름.", [], False),
    ("유스", "유스 능력치 최대 (100)", "PS3 YOUTH", span(0x0C, 46, 100, YOUTH_SLOTS, YOUTH),
     "유스 15칸의 능력 46개 +0x0C~+0x39 (u8, 게임 최대 100).", [], False),
    ("유스", "유스 성장 타입", "PS3 YOUTH", all_players(0x208, 4, 7, YOUTH_SLOTS, YOUTH),
     "유스 15칸의 +0x208 u32 성장 곡선 (0~7).", ["0|성장 타입|0|7|number"], False),
    ("육성·연습", "SP(특별) 연습 1턴 완료", "SP TRA 1", ["$D001 00EE83B0 00000001", "$0003 0000000C 00000000"],
     "특별연습 3칸 (0x00EE83A8, 12바이트: 선수 ID, 프로그램, 남은 주 u8). 진행 중인 칸만 1로 줄여 다음 주에 끝남 (func_005177A0).",
     [], False),
    ("육성·연습", "비장의 아이(秘蔵っ子) 연습 1턴 완료", "HIZO TRA 1", ["$D001 00EE83D8 00000001", "$0003 00000020 00000000"],
     "秘蔵っ子 연습 3칸 (0x00EE83CC, 32바이트: 선수 ID, 종류, 프로그램, 남은 주 u8 +0xC). 진행 중인 칸만 1로.", [], False),
    ("육성·연습", "비장의 아이(秘蔵っ子) 연습 반드시 성공 [코드 스위치]", "PS3", ["$F004 00000000 00000000"],
     "끝날 때 새 개성·스킬을 뽑는 0~99 추첨 (func_00567A2C/func_00567B74)을 항상 첫 결과로. 이미 가진 결과면 그래도 빈손. "
     "특별연습은 추첨이 없고 선수 능력대로 결과가 나옴.", [], False),
    ("육성·연습", "유학 다음 달 귀국", "STUDY NEXT MONTH", ["$D001 00EE8490 00000001", "$0017 0000000C 00000000"],
     "유학 23칸 (0x00EE8488, 12바이트: 선수 ID, -, 남은 달 u8 +8, func_0052A3DC). 유학 중인 칸만 1로.", [], False),
    ("육성·연습", "유망주 발견 1턴", "WAKTE 1TURN",
     ["$C000 00F8EC58 00000001", "$C000 00F8EC70 00000001", "$C000 00F8EC88 00000001", "$C000 00F8ECA0 00000001",
      "$C000 00F8ECB8 00000001", "$C000 00F8EEE1 00000001"],
     "스카우트 칸 5개 (0x00F8EC58 + 칸 x 0x18)의 남은 턴과, 방문이 시작되면 func_0053557C가 그 값을 복사해 매 턴 줄이는 "
     "0x00F8EEE1 (func_0051EC6C)을 1로. 1보다 클 때만 써서 쉬는 칸(0)은 그대로. (2026-11-09 전에는 복사본을 안 써서 진행 중인 "
     "방문에 효과가 없었음)", [], False),
    ("육성·연습", "유망주 반드시 발견 [코드 스위치]", "WAKATE MUST FIND", ["$F001 00000000 00000000"],
     "스카우트 방문의 발견 확률 (func_0053557C)을 100으로.", [], False),
    # 2026-11-09 (owner: 비장의 아이·특별훈련을 아무에게나, 슬롯에 고정, 효과 배수, 일반 훈련 배수; REA Ghidra)
    ("육성·연습", "비장의 아이·특별연습 코스 조건 없음 [코드 스위치]", "COURSE ANY", ["$F00A 00000000 00000000"],
     "코스를 고를 때의 조건 판정 (func_005122DC: 플레이 스타일·스킬·개성·각성 레벨)을 늘 '맞음'으로, 能力覚醒 코스가 끝날 때의 "
     "각성 준비 비트 (+0xE8 0x100000)도 켠 것으로 읽음. 선수 고르기는 원래 나이 제한이 없고 중상·유학 중인 선수만 빠짐.",
     [], False),
    ("육성·연습", "비장의 아이·특별연습 슬롯 유지 [코드 스위치]", "SLOT KEEP", ["$F00B 00000000 00000000"],
     "특별연습 3칸 (0x00EE83A8)과 비장의 아이 3칸 (0x00EE83CC)의 계획이 끝나 게임이 칸을 비우면 (func_005177A0) 같은 선수·"
     "코스를 원래 기간으로 다시 넣음. 메뉴에서 직접 빼면 빠짐. 비장의 아이는 끝날 때 효과가 나므로 '1턴 완료' 치트와 같이 "
     "켜면 매주 효과. 칸 수는 세이브 구조상 3개씩 그대로.", [], False),
    ("육성·연습", "일반 연습 효과 배수 [코드 스위치]", "TRAINING X", ["$F00C 00000000 00000002"],
     "매주 일반 연습으로 얻는 능력 경험치 (func_00515548)를 배수만큼. 경험치는 게임이 0~65535로 자름.",
     ["0|배수|1|100|number"], False),
    ("육성·연습", "특별연습·비장의 아이 효과 배수 [코드 스위치]", "SPECIAL X", ["$F00D 00000000 00000002"],
     "특별연습 매주 경험치와 끝날 때 각성 포인트, 個性強化 코스의 경험치, 能力覚醒 코스의 한계 상승 (게임이 100에서 자름)을 "
     "배수만큼.", ["0|배수|1|100|number"], False),
    ("육성·연습", "각성 포인트 배수 [코드 스위치]", "AWAKENING X", ["$F00E 00000000 00000002"],
     "우리 구단 선수가 얻는 각성 포인트 (경기·연습·이벤트, func_00568B08)를 배수만큼. 다음 각성 기준값에 닿으면 게임이 각성 "
     "준비 표시를 켬.", ["0|배수|1|100|number"], False),
    ("시설", "경기장 공사 1주 완료", "QIU CHANG", ["$C000 00EE7CF1 00000001"],
     "공사 남은 주 u8 (0x00EE7CF1). 공사 중일 때만 1로 줄임 (0이면 쓰지 않으니 켜 두어도 됨).", [], False),
    ("시설", "클럽하우스 공사 1주 완료", "DA LOU", ["$C000 00EE7CF3 00000001"],
     "클럽하우스 증축 남은 주 u8 (0x00EE7CF3). 공사 중일 때만 1로.", [], False),
    ("시설", "경기장·클럽하우스 전부 해금", "PS3", ["$0200 00EE6E7C 00007FFE", "$0200 00EE6E80 00000007"],
     "고를 수 있는 경기장 (비트 1~14, func_00542490)과 클럽하우스 레벨 0~2 (func_00542430). 해금 소식이 한 번 뜸. 공사 1주 "
     "치트와 함께 쓰면 바로 최대로 지을 수 있음.", [], True),
    ("시설", "클럽하우스 레벨 최대 (즉시)", "PS3", ["$0200 00EE6E78 00000002"],
     "클럽하우스 레벨 u32 (0x00EE6E78, 0~2). 시설 용량(코스트)이 레벨에서 나옴 (func_004CE768).", [], True),
    ("시설", "시설 건설 조건·코스트 제한 없음 [코드 스위치]", "PS3",
     ["$F005 00000000 00000000",
      "$4001 00E25B36 00000000", "$0029 00000014 00000000",   # clubhouse facility sizes (table 0x2B, 41 x 0x14, +0xA)
      "$4001 00E25E72 00000000", "$0032 00000010 00000000"],  # stadium facility sizes (table 0x2A, 50 x 0x10, +0xA)
     "클럽하우스 시설 (27칸 0x00EE7C06, 판정 func_0054F92C)과 구장 시설 (27칸 0x00EE7B64, 판정 func_0054F130): 모든 시설의 "
     "크기를 0스페이스로 (마스터 표 0x2B 41개 x 0x14 at 0x00E25B2C +0xA, 0x2A 50개 x 0x10 at 0x00E25E68 +0xA; 사용 공간은 "
     "지은 시설 크기의 합이라 0) 하고 레벨·종류 조건 부족을 '건설 가능'으로. 자금 부족은 그대로. 끄면 크기는 게임을 다시 켤 "
     "때 원래대로. (2026-11-09 전에는 용량 초과를 '가능'으로 바꿔 SPACE 48/20처럼 넘치고 목록이 깨졌음)", [], False),
    ("시설", "시설 건설 1주 완료 [코드 스위치]", "PS3", ["$F006 00000000 00000000"],
     "새로 짓는 클럽하우스 시설 (func_0054220C)과 구장 시설 (func_00542100)의 공사 기간을 1주로.", [], False),
    ("계약·트레이드", "트레이드 반드시 성립 [코드 스위치]", "TRADE MUST", ["$F002 00000000 00000000"],
     "거절 이유 (func_0051F148)와 성립 확률 (func_0051F2B4).", [], False),
    ("계약·트레이드", "외국인 선수 제한 없음 [코드 스위치]", "FOREIGN PLAYER FRAME OVER", ["$F003 00000000 00000000"],
     "지금 구단의 외국인·아시아 빈칸 (func_00584388)을 23 이상으로. 출전 명단 (선발 11 + 교체 7)의 외국인 수 검사 "
     "(func_005864DC, 외국인 선수 정원 초과 / 外国籍選手枠をオーバー)도 통과.", [], False),
    ("계약·트레이드", "협상 반드시 성공 [코드 스위치]", "NEGOTIATE", ["$F00F 00000000 00000000"],
     "선수 연봉 협상과 이적료 협상 (EV_TRANSFER 장면)의 판정 func_0053115C를 늘 '계약'으로. 제시한 연봉 그대로 계약. "
     "에이전트를 통한 여러 주짜리 협상은 따로 (0x00EE77D4~).", [], False),
    ("계약·트레이드", "면담 무제한", "MYUNDAM MUHAN", ["$0000 00EECE27 00000000", "$F009 00000000 00000000"],
     "면담은 한 달 단위 제한 (今月は面談を行うことはできません). 이번 달에 한 일 플래그 u32 0x00EECE24의 면담 비트 (1, 0x20, "
     "0x40, 0x80, 마지막 바이트)를 0으로 쓰고, 코드 스위치 9가 판정 함수 func_00590B58에서도 그 비트를 늘 '안 함'으로 읽게 함 "
     "(스케줄 열 때 func_003F6338, 고를 때 func_003F73AC가 부름; 월 초기화 func_0058B004).", [], False),
    ("경기", "관중 만원 [코드 스위치]", "FULL HOUSE", ["$F010 00000000 00000000"],
     "우리 구단 경기만 (결과 0x00F99254): 경기 관중 계산 func_0053D7A8에서 게임의 만원 분기를 탐: 관중 = 경기장 좌석 수, 빈 좌석 0 (입장 수입도 그만큼). 다른 구단끼리의 경기는 그대로.",
     [], False),
    ("경기", "경기 후 관중 만족도 S [코드 스위치]", "SATISFACTION S", ["$F011 00000000 00000000"],
     "경기 뒤 서포터 설문 (func_00541640)의 네 평가 (경기 내용·선수·시설·프로모션)를 모두 S로: 종합 평가도 S, 얻는 "
     "서포터 수도 S 기준으로 계산.", [], False),
    ("경기", "출전 정지 없음 (레드카드·옐로 누적) [코드 스위치]", "NO SUSPENSION", ["$F012 00000000 00000000"],
     "대회별 선수 카드 기록 (0x80 = 다음 경기 출전 정지, 아래 7비트 = 옐로 누적)을 우리 구단 선수만 늘 0으로: 출전 판정 "
     "func_0049D020이 묻는 func_00572524에서 지움. 이미 걸린 출전 정지도 풀림. 경기 중 퇴장 자체는 그대로.", [], False),
    ("경기", "팀 스킬(試合指示) 횟수 무한 [코드 스위치]", "MATCH ORDERS INFINITE", ["$F013 00000000 00000000"],
     "경기 중 우리 팀이 試合指示를 써도 남은 횟수가 줄지 않음 (관리 객체 +0x2C914; 쓰는 곳 func_00239E78 / func_0023A6E8). "
     "상대 팀은 그대로.", [], False),
    ("경기", "경기 중 카드 없음 (옐로·레드) [코드 스위치]", "NO CARDS", ["$F014 00000000 00000000"],
     "우리 팀 선수가 반칙해도 심판이 카드를 꺼내지 않음: 플레이를 만드는 func_00205220 / func_00205568에서 반칙 이벤트 "
     "(record+0x28, 종류 9/10)의 심판 판정 코드 (+0x60, 3으로 나눈 나머지 1 = 옐로, 2 = 레드)를 카드 없는 같은 반칙으로 바꿈. "
     "반칙·프리킥·상대 부상은 그대로, 경고·퇴장·카드 장면·카드 기록이 없음. 상대 팀 카드는 그대로.", [], False),
    ("경기", "팀 스킬(試合指示) 횟수·효과 시간", "MATCH ORDERS",
     ["$0200 00EE6CF0 00000009", "$0200 00EE6CF4 0000005A"],
     "경기 중 試合指示를 쓸 수 있는 횟수 (u32 0x00EE6CF0: 기본 3, 코치 특기 試合指示力の向上로 4~6)와 효과 시간 연장 "
     "(u32 0x00EE6CF4, 분: 코치 특기 作戦指示期間を延長する采配力로 5 / 10)을 게임이 정하는 자리 그대로 고정 (func_005063B0이 "
     "주마다 다시 계산). 우리 팀만 (경기 팀 객체 0x00F8F158 / 0x00F9351C 중 우리 구단 쪽에만 들어감; 상대는 자기 값). +90분 = 경기 끝까지 유지. 횟수는 9를 넣어도 경기에서 6회 (게임 최대); 무한은 '팀 스킬 횟수 무한' 스위치.",
     ["0|경기당 횟수|1|99|number", "1|효과 시간 연장 (분)|0|90|number"], False),
    ("스태프·목록", "코치 후보 목록", "COACH LIST",
     ["$0200 00EE72A4 000001DD", "$0200 00EE72B0 000001DE", "$0200 00EE72BC 000001DF", "$0200 00EE72C8 000001E0"],
     "코치 후보 4칸 (0x00EE72A4, 12바이트, 스태프 ID 1~825, func_00528FD8). 런처의 목록 페이지에서 이름으로 고를 수 있음.",
     ["0|1번 칸|1|825|staff", "1|2번 칸|1|825|staff", "2|3번 칸|1|825|staff", "3|4번 칸|1|825|staff"], True),
    ("스태프·목록", "스카우트 후보 목록", "SCOUT LIST",
     ["$0200 00EE72D4 00000098", "$0200 00EE72E0 00000099", "$0200 00EE72EC 0000009A"],
     "스카우트 후보 4칸 중 3칸 (0x00EE72D4, 12바이트, ID 1~427).",
     ["0|1번 칸|1|427|scout", "1|2번 칸|1|427|scout", "2|3번 칸|1|427|scout"], True),
    ("스태프·목록", "유럽 선수 목록 교체", "EURO PLAYER LIST", None,
     "유럽 선수 목록 20칸 (0x00EE70E8, 20바이트)의 11칸과 그 선수들의 상태 기록 (0x00F0C4B8~). 원하는 선수는 목록 페이지에서.",
     [], True),
    ("기타", "프로모션 전부 보유", "ALL PROMOTION HAVE", None,
     "프로모션 81칸 (0x00EE7F08, 12바이트: +0 보유 u8, +8 u8).", [], False),
]
# Vita cheats left out, with the reason (shown by the launcher).
LEFT_OUT = {
    "player exchange max": "선수 교체 무제한: Vita ARM 코드, PS3 교체 횟수 판정을 아직 못 찾음",
    "CORE SUPPO 2": "시즌 보고용 코어 서포터 사본 (0x00F8E2FC, func_0050ED40)이라 써도 효과 없음: 뺌",
    "CORE SUPPO 3": "보고용 사본 (0x00F8E320, func_00506DB4)이라 효과 없음: 뺌",
    "RAIO SUPPO": "이번 주 서포터 증가 표시값 (0x00EECEF4, func_00542EE4)이라 효과 없음: 뺌",
}

text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
psv = {}
for block in re.split(r"\n(?=_V[01] )", text):
    lines = [l.strip() for l in block.strip().splitlines() if l.strip()]
    if lines and lines[0].startswith("_V"):
        psv[lines[0][4:].strip()] = [l for l in lines[1:] if l.startswith("$")]


def moved(name):
    """A .psv cheat moved by the address offset, values as they are."""
    out, codes, i = [], [c.split() for c in psv[name]], 0
    while i < len(codes):
        t, a, v = codes[i]
        out.append("$%s %08X %s" % (t[1:], int(a, 16) - DELTA, v))
        if int(t[1:], 16) >> 12 == 4:
            out.append(" ".join(codes[i + 1]))
            i += 1
        i += 1
    return out


out, category = [], None
for cat, title, origin, codes, note, values, once in CHEATS:
    if cat != category:
        out.append("#@cat %s" % cat)
        category = cat
    out.append("_V0 %s (%s)" % (title, origin))
    if note:
        out.append("#@note %s" % note)
    out.extend("#@value %s" % v for v in values)
    if origin in LOCKABLE_SWITCHES:   # a code switch whose hook honours 고정 (the launcher/F2 show 고정 and 갱신)
        out.append("#@lock")
    if once:
        out.append("#@once")
    out.extend(codes if codes is not None else moved(origin))
    out.append("")

used = {o for _, _, o, _, _, _, _ in CHEATS}
left = ["%s: %s" % (n, r) for n, r in LEFT_OUT.items()]
left += ["%s: 변환 규칙 없음" % n for n in psv if n not in used and n not in LEFT_OUT and n.split(" + ")[0] not in used
         and n not in ("CORE SUPPO", "SUPPOTERS", "WAKATE MUST FIND", "TRADE MUST", "FOREIGN PLAYER FRAME OVER",
                       "SUPPORTER S")]   # SUPPORTER S = 경기 후 관중 만족도 S (switch 17)

header = """# サカつく プロサッカークラブをつくろう！ (BLJM61064 1.03) - cheats of the PS3 port (tools/psv_to_ps3.py).
# VitaCheat code syntax with PS3 addresses; the port writes the values big-endian ten times a second.
# Moved from the PS Vita version's PCSG00198.psv: the game state has the same layout on both, Vita = PS3 + 0x80D1C300
# (money 81BFEB44 = 00EE2844, club roster 81BFEC54 = 00EE2954, 23 x 0x218-byte players). Field widths and ranges were
# checked in the PS3 code with REA's Ghidra (NOTES.md "Cheats"); the notes (#@note) name the functions.
# All of these write the user's club, which the game rebuilds at 0x00EE2934 when the GM moves club: they follow it.
# The launcher's cheat page switches them (cheats/enabled.txt) and sets values (cheats/values.txt).
#   $0000 addr value  8-bit   $0100 16-bit   $0200 32-bit   $4T0x addr value + $CCCC step vstep: multi-write
#   $CT00 / $DT0x: the same, written only where memory holds a larger value   $F0nn: code switch (saka_hooks.c)

"""
footer = "# Not moved yet (Vita cheats without a PS3 equivalent so far):\n" + "".join("#   %s\n" % n for n in left)
open(sys.argv[2], "w", encoding="utf-8", newline="\r\n").write(header + "\n".join(out) + "\n" + footer)
print("cheats %d, left %d" % (sum(1 for l in out if l.startswith("_V")), len(left)))
