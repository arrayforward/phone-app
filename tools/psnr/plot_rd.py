# RD 曲线绘制（PSNR vs 实际码流字节，log-x）——PIL ImageDraw 手绘，无第三方依赖。
# 数据：tools/psnr/out/ 各档实测（真机 MediaCodec 单帧 IDR，oneVPL 硬解，新桌面基准）。
import math
from PIL import Image, ImageDraw, ImageFont

# (字节, PSNR) —— 与 docs/test_report.md §3.2 表一致（26 档去重后的真实水位）
SINGLE = [  # 蓝
    (45280, 27.19), (52032, 28.35), (91120, 30.90),
    (271440, 36.44), (478016, 38.57),   # 24M 起封顶（[24M,∞) 同水位）
]
DUAL_RAW = [  # 绿
    (221136, 31.05), (334752, 33.90),
]
DUAL_YCOCG = [  # 红
    (74448, 28.17), (84224, 29.58), (142528, 32.64),
    (273808, 36.65), (452144, 40.34),   # [24M,30M] 台阶
    (793680, 44.98),                    # [40M,∞) 封顶
]

W, H = 1100, 720
LEFT, RIGHT, TOP, BOTTOM = 110, 60, 60, 110
X_MIN, X_MAX = math.log10(40000), math.log10(900000)
Y_MIN, Y_MAX = 26.0, 46.0

FONT = "C:/Windows/Fonts/msyh.ttc"
f_label = ImageFont.truetype(FONT, 22)
f_tick = ImageFont.truetype(FONT, 18)
f_title = ImageFont.truetype(FONT, 28)


def xy(b, p):
    x = LEFT + (math.log10(b) - X_MIN) / (X_MAX - X_MIN) * (W - LEFT - RIGHT)
    y = TOP + (Y_MAX - p) / (Y_MAX - Y_MIN) * (H - TOP - BOTTOM)
    return x, y


img = Image.new("RGB", (W, H), "white")
d = ImageDraw.Draw(img)

d.text((W // 2, 16), "RD 曲线：PSNR vs 实际码流字节（新桌面，单帧 IDR）", font=f_title,
       fill="black", anchor="mt")
d.text((W // 2, H - 40), "实际产出字节（KB，log）", font=f_label, fill="black", anchor="mm")
d.text((LEFT + 10, TOP - 14), "PSNR (dB)", font=f_label, fill="black", anchor="lm")

# 网格与刻度
for kb in (50, 100, 200, 400, 800):
    x, _ = xy(kb * 1000, Y_MIN)
    d.line((x, TOP, x, H - BOTTOM), fill=(220, 220, 220))
    d.text((x, H - BOTTOM + 14), str(kb), font=f_tick, fill="black", anchor="mt")
for p in range(28, 46, 2):
    _, y = xy(40000, p)
    d.line((LEFT, y, W - RIGHT, y), fill=(220, 220, 220))
    d.text((LEFT - 14, y), str(p), font=f_tick, fill="black", anchor="rm")
d.rectangle((LEFT, TOP, W - RIGHT, H - BOTTOM), outline="black", width=2)


def draw_curve(points, color, marker, label, lx, ly):
    pts = [xy(b, p) for b, p in points]
    d.line(pts, fill=color, width=3)
    for x, y in pts:
        d.ellipse((x - 7, y - 7, x + 7, y + 7), fill=color, outline="black")
    d.ellipse((lx, ly, lx + 14, ly + 14), fill=color, outline="black")
    d.text((lx + 22, ly + 7), label, font=f_tick, fill="black", anchor="lm")


draw_curve(SINGLE, (30, 90, 220), "o", "single（4:2:0）", W - 320, H - 200)
draw_curve(DUAL_RAW, (30, 160, 60), "o", "dual-raw（§3.1）", W - 320, H - 165)
draw_curve(DUAL_YCOCG, (220, 40, 40), "o", "dual-ycocg（§3.2）", W - 320, H - 130)

# 拐点标注：ycocg 反超 single 的同字节交叉点（~272KB / 36.5dB）
cx, cy = xy(272000, 36.55)
d.ellipse((cx - 13, cy - 13, cx + 13, cy + 13), outline=(255, 140, 0), width=4)
d.text((cx + 20, cy - 34), "拐点：同字节反超\nycocg@20M ≈ single@12M", font=f_tick,
       fill=(200, 100, 0), anchor="lm")

# ycocg 高码率近透明点
hx, hy = xy(793680, 44.98)
d.text((hx - 18, hy + 26), "40M 档 45dB 近透明", font=f_tick, fill=(200, 100, 0),
       anchor="rm")

img.save("rd_curve.png")
print("saved rd_curve.png")
