"""Repair v0.8 text without changing game IDs, save data or original assets.

Run from the port root. The first run backs up each changed overlay file;
subsequent runs always start from that backup, making the result repeatable.
"""
from pathlib import Path
from collections import defaultdict, Counter
import json
import re
from korean_text import tables, decode, encode, repack
from make_teamcolors import KO as TEAM_COLOR_KO   # the F2 팀컬러 tab's names: the game shows the same (2026-11-14)

ROOT = Path(__file__).resolve().parents[1]
OVERLAY = ROOT / 'game/mods/korean/data'
BACKUP = ROOT / 'logs/korean_resume_backup'

# Names/readings checked against Tokyo Verdy's 2013 roster and club profiles:
# https://www.verdy.co.jp/news/965
# https://www.avispa.co.jp/news/post-28185
PLAYER_NAMES = {
    '佐藤 優也': '사토 유야', '石神 直哉': '이시가미 나오야',
    '福井 諒司': '후쿠이 료지', '中後 雅喜': '나카고 마사키',
    '西 紀寛': '니시 노리히로', '鈴木 惇': '스즈키 준',
    '小池 純輝': '고이케 준키', '飯尾 一慶': '이이오 가즈노리',
    '常盤 聡': '도키와 사토시', '森 勇介': '모리 유스케',
    '関 光博': '세키 미쓰히로', '高原 直泰': '다카하라 나오히로',
    '曺 永哲': '조영철', 'マネ': '마네', 'カネ': '카네',
    'ドアス': '도아스', 'ネト': '네투',
}

J_CLUBS = [
    ('센다이', '베갈타 센다이'), ('가시마', '가시마 앤틀러스'),
    ('우라와', '우라와 레즈'), ('오미야', '오미야 아르디자'),
    ('가시와', '가시와 레이솔'), ('FC 도쿄', 'FC 도쿄'),
    ('가와사키', '가와사키 프론탈레'), ('요코하마 FM', '요코하마 F. 마리노스'),
    ('쇼난', '쇼난 벨마레'), ('고후', '반포레 고후'),
    ('니가타', '알비렉스 니가타'), ('시미즈', '시미즈 에스펄스'),
    ('이와타', '주빌로 이와타'), ('나고야', '나고야 그램퍼스'),
    ('C. 오사카', '세레소 오사카'), ('히로시마', '산프레체 히로시마'),
    ('도스', '사간 도스'), ('오이타', '오이타 트리니타'),
    ('삿포로', '콘사도레 삿포로'), ('야마가타', '몬테디오 야마가타'),
    ('미토', '미토 홀리호크'), ('도치기', '도치기 SC'),
    ('군마', '더스파 구사쓰 군마'), ('지바', '제프 유나이티드 지바'),
    ('도쿄 베르디', '도쿄 베르디'), ('요코하마 FC', '요코하마 FC'),
    ('마쓰모토', '마쓰모토 야마가'), ('도야마', '카탈레 도야마'),
    ('FC 기후', 'FC 기후'), ('교토', '교토 상가 FC'),
    ('G. 오사카', '감바 오사카'), ('고베', '비셀 고베'),
    ('돗토리', '가이나레 돗토리'), ('오카야마', '파지아노 오카야마'),
    ('도쿠시마', '도쿠시마 보르티스'), ('에히메', '에히메 FC'),
    ('후쿠오카', '아비스파 후쿠오카'), ('기타큐슈', '기라반츠 기타큐슈'),
    ('나가사키', 'V. 파렌 나가사키'), ('구마모토', '로아소 구마모토'),
]

# Exact original strings only: never substitute inside player names or tags.
TERMS = {
    '右足': '오른발', '左足': '왼발', '両足': '양발',
    'ゴール数': '득점 수', 'アシスト数': '도움 수', '平均評価点': '평균 평점',
    '移籍金': '이적료', '年俸': '연봉', '契約年数': '계약 기간',
    '出場数': '출전 수', '支払回数': '분할 납부', '来場者数': '관중 수',
    '観客数': '관중 수', '今回観客数': '이번 관중 수', '全サポーター': '전체 서포터',
    '内訳：': '내역:', '内訳': '내역', '引き分け': '무승부',
    'ポゼッション': '점유율 축구', 'ポゼッション重視': '점유율 중시',
    'カウンター': '역습', 'カウンター重視': '역습 중시',
    'サイドアタック': '측면 공격', 'リトリート': '후퇴 수비',
    'ラインディフェンス': '라인 수비', 'スイーパーシステム': '스위퍼 시스템',
    'フィジカル': '신체', 'メンタル': '정신', 'オフェンス': '공격',
    'ディフェンス': '수비', 'タクティクス': '전술', 'テクニック': '기술',
    'エディット選手': '에디트 선수', 'トップチーム': '1군', 'ユースチーム': '유스팀',
    'ユース': '유스', 'スタッフ': '스태프', 'コールドスリープ': '콜드슬립',
    'トップ昇格': '1군 승격', 'フレンドリーマッチ': '친선 경기',
    '体重': '체중', '身長': '키', '利き足': '주발', '利き足：': '주발:',
    '細い': '마른 체형', 'がっちり': '건장한 체형', '決定': '확인',
    '若手': '유망주', 'レギュラー': '주전', 'スタメン': '선발',
    'セーブ': '저장', 'ロード': '불러오기', 'セーブ・ロード': '저장 / 불러오기',
    '設定終了': '설정 완료', 'もどる': '뒤로', '設定なし': '설정 안 함',
    'シュート（枠内）': '슛(유효 슛)', '支配率': '점유율', '前試合比': '직전 경기 대비',
    'パスで揺さぶれ！': '패스로 흔들어라!', '積極的にシュート！': '적극적으로 슛해라!',
    'この条件で移籍します。よろしいですか？': '이 조건으로 이적할까요?',
    '契約します。よろしいですか？': '이 조건으로 계약할까요?',
    'この選手にオファーしますか？': '이 선수에게 영입 제안을 할까요?',
    'この選手を自由契約にしますか？': '이 선수를 방출할까요?',
    'セーブデータをロードします。よろしいですか？': '저장 데이터를 불러올까요?',
    '再開するセーブデータを選択してください。': '불러올 저장 데이터를 선택하세요.',
    'エディット選手を2人以上チームに所属させることはできません。': '에디트 선수는 최대 3명까지 소속시킬 수 있습니다.',
    # 2026-11-14: the traits and abilities drawn on the edit-result badges (korean_ui_more.py), as on the badges
    'キャプテンシー': '캡틴십', 'イクメン': '육아남', 'ダービー男': '더비남', 'ムードメーカー': '무드 메이커',
    '未来のコーチ': '미래의 코치', '未来の監督': '미래의 감독', '順応性が高い': '적응력 높음', '説得力がある': '설득력 있음',
    '戦術理解が早い': '전술이해 빠름', 'システム理解が早い': '시스템이해 빠름', 'プロ意識が高い': '프로의식 높음',
    '面倒見が良い': '잘 챙겨줌', '逆境に強い': '역경에 강함', '大舞台に強い': '큰 무대 강함', '二段階成長': '2단계 성장',
    '長期安定成長': '장기 안정성장', 'セービング': '세이빙', '飛び出し': '전진',
}

STATIC = {
    (5, 29): '1군 승격', (5, 61): '유망주', (5, 65): '점유율 축구',
    (5, 79): '국가 선택', (5, 95): '선발 구성 위임',
    (5, 110): '피로가 쌓이면 휴식', (5, 111): '적절히 휴식',
    (5, 160): '저장 / 불러오기', (5, 214): '체형', (5, 217): '마른 체형',
    (5, 219): '건장한 체형', (5, 220): '주발', (5, 221): '오른발',
    (5, 302): '친선 경기', (5, 322): '확인',
    (5, 348): '원거리 1', (5, 349): '원거리 2', (5, 350): '근거리 1', (5, 351): '근거리 2',
    (7, 3): '엔', (7, 30): '무', (7, 41): '주발:',
    (20, 13): '별도 지출', (20, 22): '내역:', (20, 65): '무',
    (20, 85): '슛(유효 슛)', (20, 90): '점유율', (20, 98): '불필요',
}


def original_path(relative):
    for folder in ('game/dev_hdd0/game/BLJM61064/USRDIR/data', 'game/dev_bdvd/PS3_GAME/USRDIR/data'):
        p = ROOT / folder / relative
        if p.exists():
            return p
    raise FileNotFoundError(relative)


def refine_static(jp, ko):
    for key, name in list(jp.items()):
        if key[0] == 0 and name in PLAYER_NAMES:
            ko[key] = PLAYER_NAMES[name]
    surnames = defaultdict(Counter)
    for (table, player), text in jp.items():
        if table != 0 or not text:
            continue
        short = jp.get((1, player), '')
        full = ko[(0, player)]
        if short == text and full:
            surnames[short][full] += 1
        elif text.startswith(short + ' ') and ' ' in full:
            first = full.split(' ')[0]
            if 1 < len(first) <= 6:
                surnames[short][first] += 1
    for (table, player), short in jp.items():
        if table != 1 or not short:
            continue
        full_jp, full_ko = jp.get((0, player), ''), ko.get((0, player), '')
        if full_jp in PLAYER_NAMES:
            ko[(1, player)] = full_ko.split(' ')[0]
        elif full_jp == short and full_ko:
            ko[(1, player)] = full_ko
        elif short in surnames:
            candidate = surnames[short].most_common(1)[0][0]
            current = ko[(1, player)]
            # Recover only a truncated prefix. A complete existing surname
            # may have a better reading than the machine-translated full name.
            if full_ko.startswith(candidate) and candidate.startswith(current):
                ko[(1, player)] = candidate
    # Single-syllable Korean surnames should not replace the player's name.
    for player in range(18082):
        short, full = ko.get((1, player), ''), ko.get((0, player), '')
        jp_short = jp.get((1, player), '')
        if (len(short) == 1 and jp_short and all('\u30a0' <= c <= '\u30ff' for c in jp_short)
                and len(full.replace(' ', '')) in (2, 3, 4) and full.startswith(short)):
            ko[(1, player)] = full
    for club, (short, full) in enumerate(J_CLUBS, 1):
        ko[(3, club)], ko[(4, club)] = short, full
    ko.update(STATIC)
    for color, name in TEAM_COLOR_KO.items():   # table 19: the team color names (the font has ・ and ＆)
        if (19, color) in ko:
            ko[(19, color)] = name.replace('·', '・').replace(' & ', '＆')


def main():
    report = []
    for path in sorted(OVERLAY.rglob('*')):
        if path.suffix not in ('.arch', '.txb'):
            continue
        relative = path.relative_to(OVERLAY)
        if relative.parts[0] not in ('text', 'text2', 'text3'):
            continue
        backup = BACKUP / relative
        source = (backup if backup.exists() else path).read_bytes()
        standalone = path.suffix == '.txb'
        try:
            original_data = original_path(relative).read_bytes()
            raw = {(t, k): s for t, k, _, _, s in tables(source, standalone)}
            jp = {(t, k): s for t, k, _, _, s in tables(original_data, standalone)}
        except (ValueError, UnicodeError, IndexError):
            continue
        ko = {key: decode(value) for key, value in raw.items()}
        for key, original in jp.items():
            if original in TERMS and key in ko:
                ko[key] = TERMS[original]
        if relative.as_posix() == 'text/txt_static_data.arch':
            refine_static(jp, ko)
        changed = {key: encode(value) for key, value in ko.items() if value != decode(raw[key])}
        if not changed:
            continue
        result = repack(source, changed, standalone)
        assert len(result) <= max(len(source), len(original_data)), str(relative)
        if not backup.exists():
            backup.parent.mkdir(parents=True, exist_ok=True)
            backup.write_bytes(source)
        path.write_bytes(result)
        for (table, key), value in changed.items():
            report.append({'file': relative.as_posix(), 'table': table, 'id': key,
                           'original': jp.get((table, key)), 'before': decode(raw[(table, key)]), 'after': decode(value)})
    (ROOT / 'logs/korean_refinement_report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print('Corrected', len(report), 'strings in', len(set(x['file'] for x in report)), 'files')


if __name__ == '__main__':
    main()
