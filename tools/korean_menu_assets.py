"""Localize the main-menu atlas, centering labels in their existing UV boxes.

Only text pixels change. PAR member offsets, DDS format, sprite coordinates
and every other pixel are preserved. Requires Pillow in the local venv.
"""
from pathlib import Path
from io import BytesIO
from PIL import Image, ImageDraw, ImageFont
from saka_par import entries, replace_payloads
from korean_refine import ROOT, OVERLAY, BACKUP, original_path


def main():
    relative = Path('sb/main_menu.par')
    destination, backup = OVERLAY / relative, BACKUP / relative
    source_path = backup if backup.exists() else destination if destination.exists() else original_path(relative)
    source = source_path.read_bytes()
    payload = next(v for n, _, _, _, v in entries(source) if n == 'main_menu.dds')
    im = Image.open(BytesIO(payload)).convert('RGBA')
    assert im.size == (1024, 1024) and len(payload) == 128 + 1024 * 1024 * 4
    areas = [(0, y0, 96, y1, label) for y0, y1, label in [
        (327,345,'정보'), (347,365,'일정'), (367,385,'인사'),
        (387,405,'일정 진행'), (407,425,'훈련'), (427,445,'건설'), (447,465,'설정')]]
    areas += [(400,405,452,426,'경기로'), (950,765,1004,787,'감독')]
    draw = ImageDraw.Draw(im)
    for x0, y0, x1, y1, label in areas:
        box = im.getchannel('A').crop((x0,y0,x1,y1)).getbbox()
        assert box, label
        left, top, right, bottom = x0+box[0], y0+box[1], x0+box[2], y0+box[3]
        font = ImageFont.truetype('C:/Windows/Fonts/malgunbd.ttf', 15)
        while True:
            bounds = draw.textbbox((0,0),label,font=font)
            w,h = bounds[2]-bounds[0], bounds[3]-bounds[1]
            if w <= x1-x0-2 and h <= y1-y0-2:break
            font = ImageFont.truetype('C:/Windows/Fonts/malgunbd.ttf', font.size-1)
        draw.rectangle((x0,y0,x1-1,y1-1),fill=(0,0,0,0))
        x=round((left+right-w)/2)-bounds[0]
        y=round((top+bottom-h)/2)-bounds[1]
        draw.text((x,y),label,font=font,fill=(44,44,40,255))
    new_dds = payload[:128] + im.tobytes('raw','BGRA')
    result = replace_payloads(source, {'main_menu.dds': new_dds})
    if not backup.exists():
        backup.parent.mkdir(parents=True,exist_ok=True)
        backup.write_bytes(source)
    destination.parent.mkdir(parents=True,exist_ok=True)
    destination.write_bytes(result)
    im.save(ROOT/'logs/korean_mainmenu_atlas.png')
    print('Centered 9 Korean menu labels; unchanged PAR/DDS sizes and sprite coordinates')


if __name__ == '__main__':
    main()
