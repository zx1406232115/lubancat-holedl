#!/usr/bin/env python3
"""
render_evidence.py - 把板端输出的文本证据渲染成终端风格 PNG

为什么需要它：
    开发板没有接显示器，无法对板端终端截图。这里把**真实的板端输出**
    逐字渲染成图片，便于放进报告。图片内容与文本文件完全一致，不做任何
    修改；报告里会明确标注「由板端输出渲染」而非屏幕截图。

字体处理：
    ASCII 用 Consolas（一字符宽），中文用 SimHei 并占两字符宽 ——
    与真实终端里 CJK 占两格的表现一致，不会出现方框。

用法:
    python tools/render_evidence.py
在 docs/evidence/ 下生成同名 .png
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.abspath(os.path.join(HERE, "..", "docs", "evidence"))

BG = (30, 30, 36)
FG = (222, 222, 222)
BAR_BG = (58, 58, 68)
BAR_FG = (205, 205, 210)

MONO_CANDIDATES = [
    r"C:\Windows\Fonts\consola.ttf",
    r"C:\Windows\Fonts\CascadiaMono.ttf",
    r"C:\Windows\Fonts\lucon.ttf",
]
CJK_CANDIDATES = [
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\Deng.ttf",
    r"C:\Windows\Fonts\simsun.ttc",
]
TITLE_CANDIDATES = [
    r"C:\Windows\Fonts\segoeui.ttf",
    r"C:\Windows\Fonts\arial.ttf",
]


def load(cands, size):
    for p in cands:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except Exception:
                pass
    return ImageFont.load_default()


def is_wide(ch):
    """CJK / 全角标点按两格宽度处理"""
    o = ord(ch)
    return (
        0x1100 <= o <= 0x115F
        or 0x2E80 <= o <= 0xA4CF
        or 0xAC00 <= o <= 0xD7A3
        or 0xF900 <= o <= 0xFAFF
        or 0xFE30 <= o <= 0xFE6F
        or 0xFF00 <= o <= 0xFF60
        or 0xFFE0 <= o <= 0xFFE6
        or 0x20000 <= o <= 0x3FFFD
    )


def render(txt_path, png_path, title, max_cols=100):
    with open(txt_path, "r", encoding="utf-8", errors="replace") as f:
        raw = f.read().replace("\r\n", "\n").rstrip("\n")

    # 按「显示宽度」折行：中文算 2
    lines = []
    for ln in raw.split("\n"):
        cur, w = "", 0
        for ch in ln:
            cw = 2 if is_wide(ch) else 1
            if w + cw > max_cols:
                lines.append(cur)
                cur, w = "    ", 4
            cur += ch
            w += cw
        lines.append(cur)

    fm = load(MONO_CANDIDATES, 15)
    fc = load(CJK_CANDIDATES, 15)
    ft = load(TITLE_CANDIDATES, 13)

    box = fm.getbbox("M")
    cell_w = box[2] - box[0]
    cell_h = box[3] - box[1]
    cell_w = max(cell_w, 1)
    line_h = cell_h + 8

    max_w = max((sum(2 if is_wide(c) else 1 for c in ln) for ln in lines), default=1)
    pad_x, pad_y, bar_h = 18, 14, 34

    width = pad_x * 2 + cell_w * max_w + 4
    height = bar_h + pad_y * 2 + line_h * len(lines)

    img = Image.new("RGB", (width, height), BG)
    d = ImageDraw.Draw(img)

    d.rectangle([0, 0, width, bar_h], fill=BAR_BG)
    d.text((pad_x, 10), title, font=ft, fill=BAR_FG)
    for i, col in enumerate([(255, 95, 86), (255, 189, 46), (39, 201, 63)]):
        cx = width - pad_x - (2 - i) * 20
        d.ellipse([cx - 6, bar_h // 2 - 6, cx + 6, bar_h // 2 + 6], fill=col)

    y = bar_h + pad_y
    for ln in lines:
        x = pad_x
        for ch in ln:
            if is_wide(ch):
                d.text((x, y), ch, font=fc, fill=FG)
                x += cell_w * 2
            else:
                d.text((x, y), ch, font=fm, fill=FG)
                x += cell_w
        y += line_h

    img.save(png_path, "PNG")
    return width, height


JOBS = [
    ("01-board-and-sparse.txt", "01-board-and-sparse.png",
     "lubancat (aarch64) - board info, native build, sparse-file mechanism"),
    ("02-resume.txt", "02-resume.png",
     "lubancat - real interrupt + resume (process killed, then continue)"),
    ("03-http-download.txt", "03-http-download.png",
     "lubancat - end-to-end HTTP download with Range requests"),
    ("04-selftest.log", "04-selftest.png",
     "lubancat - full self-test suite"),
]


def main():
    made = 0
    for src, dst, title in JOBS:
        sp, dp = os.path.join(OUT_DIR, src), os.path.join(OUT_DIR, dst)
        if not os.path.exists(sp):
            print("skip (missing): %s" % sp)
            continue
        w, h = render(sp, dp, title)
        print("rendered %-28s %dx%d" % (dst, w, h))
        made += 1
    return 0 if made else 1


if __name__ == "__main__":
    sys.exit(main())
