# RD 曲线绘制（PSNR vs 标称码率 Mbps，linear-x）—— PIL 手绘
# 数据：tools/psnr/out/ 各档实测（真机 MediaCodec 单帧 IDR），按标称码率线性插值得到连续曲线
# 重点：在同一标称码率档（X 轴）下对比 single vs ycocg，凸显 ~22-23 Mbps 拐点

from PIL import Image, ImageDraw, ImageFont

# 原始测试数据：(标称码率 Mbps, PSNR dB)
SINGLE_POINTS = [
    (2, 27.19), (6, 30.90), (12, 36.44),
    (24, 38.57), (40, 38.57), (100, 38.57),  # 24M 起封顶
]
YCOCG_POINTS = [
    (2, 28.17), (6, 29.58), (12, 32.64),
    (20, 36.65), (24, 40.34),
    (40, 44.98), (100, 44.98),  # 40M 起撞 QP 地板
]

X_MIN, X_MAX = 0, 50  # Mbps（线性 X 轴）
Y_MIN, Y_MAX = 26.0, 46.5  # dB

W, H = 1100, 720
LEFT, RIGHT, TOP, BOTTOM = 110, 60, 60, 110

FONT = "C:/Windows/Fonts/msyh.ttc"
f_label = ImageFont.truetype(FONT, 22)
f_tick = ImageFont.truetype(FONT, 18)
f_title = ImageFont.truetype(FONT, 28)
f_marker = ImageFont.truetype(FONT, 20)


def xy(mbps, psnr):
    x = LEFT + (mbps - X_MIN) / (X_MAX - X_MIN) * (W - LEFT - RIGHT)
    y = TOP + (Y_MAX - psnr) / (Y_MAX - Y_MIN) * (H - TOP - BOTTOM)
    return x, y


def interpolate(points, x_query):
    """线性插值：超出范围则钳制到端点"""
    pts = sorted(points)
    if x_query <= pts[0][0]:
        return pts[0][1]
    if x_query >= pts[-1][0]:
        return pts[-1][1]
    for i in range(len(pts) - 1):
        x0, y0 = pts[i]
        x1, y1 = pts[i + 1]
        if x0 <= x_query <= x1:
            return y0 + (y1 - y0) * (x_query - x0) / (x1 - x0)
    return pts[-1][1]


# 生成连续曲线（每 0.5 Mbps 一个点）
STEP = 0.5
x_values = [X_MIN + i * STEP for i in range(int((X_MAX - X_MIN) / STEP) + 1)]
single_curve = [(x, interpolate(SINGLE_POINTS, x)) for x in x_values]
ycocg_curve = [(x, interpolate(YCOCG_POINTS, x)) for x in x_values]

# 找拐点（ycocg - single 的符号变化点）
crossover = None
for i in range(len(x_values) - 1):
    x0 = x_values[i]
    x1 = x_values[i + 1]
    diff0 = interpolate(YCOCG_POINTS, x0) - interpolate(SINGLE_POINTS, x0)
    diff1 = interpolate(YCOCG_POINTS, x1) - interpolate(SINGLE_POINTS, x1)
    if diff0 < 0 and diff1 >= 0:
        # 在 x0~x1 之间找精确拐点
        for xx in [x0 + j * 0.01 for j in range(101)]:
            d_now = interpolate(YCOCG_POINTS, xx) - interpolate(SINGLE_POINTS, xx)
            if d_now >= 0:
                crossover = (xx, interpolate(SINGLE_POINTS, xx))
                break
        break

img = Image.new("RGB", (W, H), "white")
d = ImageDraw.Draw(img)

# 标题
d.text((W // 2, 16), "RD 曲线：PSNR vs 标称码率（同码率档对比，插值曲线）",
       font=f_title, fill="black", anchor="mt")
d.text((W // 2, H - 40), "标称码率（Mbps，线性）", font=f_label, fill="black", anchor="mm")
d.text((LEFT + 10, TOP - 14), "PSNR (dB)", font=f_label, fill="black", anchor="lm")

# 网格 + X 轴刻度（每 5 Mbps）
for mbps in range(0, 51, 5):
    x, _ = xy(mbps, Y_MIN)
    d.line((x, TOP, x, H - BOTTOM), fill=(230, 230, 230))
    d.text((x, H - BOTTOM + 14), str(mbps), font=f_tick, fill="black", anchor="mt")

# Y 轴刻度（每 2 dB）
for p in range(28, 47, 2):
    _, y = xy(X_MIN, p)
    d.line((LEFT, y, W - RIGHT, y), fill=(230, 230, 230))
    d.text((LEFT - 14, y), str(p), font=f_tick, fill="black", anchor="rm")

d.rectangle((LEFT, TOP, W - RIGHT, H - BOTTOM), outline="black", width=2)

# 画 single 曲线（蓝）
single_pts_screen = [xy(mbps, p) for mbps, p in single_curve]
d.line(single_pts_screen, fill=(30, 90, 220), width=3)

# 画 ycocg 曲线（红）
ycocg_pts_screen = [xy(mbps, p) for mbps, p in ycocg_curve]
d.line(ycocg_pts_screen, fill=(220, 40, 40), width=3)

# 在原始测试点上画空心圆（标记真实数据）
for mbps, p in SINGLE_POINTS:
    if mbps <= X_MAX:
        x, y = xy(mbps, p)
        d.ellipse((x - 7, y - 7, x + 7, y + 7), outline=(30, 90, 220), width=2, fill="white")
for mbps, p in YCOCG_POINTS:
    if mbps <= X_MAX:
        x, y = xy(mbps, p)
        d.ellipse((x - 7, y - 7, x + 7, y + 7), outline=(220, 40, 40), width=2, fill="white")

# 图例
lx, ly = W - 280, H - 200
d.ellipse((lx, ly, lx + 14, ly + 14), outline=(30, 90, 220), width=2, fill="white")
d.text((lx + 22, ly + 7), "single（YUV420，~38.57 dB 封顶）", font=f_tick, fill="black", anchor="lm")
d.ellipse((lx, ly + 30, lx + 14, ly + 44), outline=(220, 40, 40), width=2, fill="white")
d.text((lx + 22, ly + 37), "double-ycocg（YCoCg，~45 dB 撞 QP 地板）", font=f_tick, fill="black", anchor="lm")
d.ellipse((lx, ly + 60, lx + 14, ly + 74), outline=(100, 100, 100), width=2, fill="white")
d.text((lx + 22, ly + 67), "空心圆 = 原始测试点（未插值）", font=f_tick, fill=(80, 80, 80), anchor="lm")

# 拐点标注
if crossover is not None:
    cx, c_mbps = xy(crossover[0], crossover[1])
    d.ellipse((cx - 14, c_mbps - 14, cx + 14, c_mbps + 14), outline=(255, 140, 0), width=4)
    d.text((cx + 22, c_mbps - 32),
           f"拐点：~{crossover[0]:.1f} Mbps\nycocg 反超 single\n（同码率档对决）",
           font=f_marker, fill=(200, 100, 0), anchor="lm")

# 标注 24 Mbps 撞顶
xx24, yy24 = xy(24, 38.57)
d.text((xx24 + 12, yy24 - 18), "single @ 24 Mbps 撞顶", font=f_tick, fill=(30, 90, 220), anchor="lm")

# 标注 40 Mbps 撞 QP 地板
xx40, yy40 = xy(40, 44.98)
d.text((xx40 + 12, yy40 + 6), "ycocg @ 40 Mbps 撞 QP 地板", font=f_tick, fill=(220, 40, 40), anchor="lm")

# 阴影区：single 封顶后到 ycocg 封顶
d.rectangle((LEFT, TOP, W - RIGHT, H - BOTTOM), outline="black", width=2)
# 用半透明色覆盖 24+ Mbps 区域（single 已无意义）
xx24_screen = LEFT + (24 - X_MIN) / (X_MAX - X_MIN) * (W - LEFT - RIGHT)
d.rectangle((xx24_screen, TOP, W - RIGHT, H - BOTTOM),
            fill=(220, 220, 220, 50), outline=None)
d.text((xx24_screen + 10, TOP + 14), "single 封顶区：\n加码率无意义", font=f_tick,
       fill=(120, 120, 120), anchor="lm")

img.save("rd_curve_same_bitrate.png")
print(f"saved rd_curve_same_bitrate.png, crossover at {crossover[0]:.2f} Mbps" if crossover else "saved")