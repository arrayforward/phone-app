# PSNR 对比收尾（Pillow）：
#   单 YUV420 @6M vs 双 YUV420 打包 @6M/9M/12M（真机 MediaCodec H.264 已编好，
#   vpl_decode.exe 已解成 NV12 raw）→ 还原 RGB png → 对 ref.png 算 PSNR。
# 用法：python tools/psnr/psnr_finish.py
import math
import os
import subprocess
import sys

from PIL import Image, ImageChops, ImageStat

ROOT = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(ROOT, "out")
PACK = os.path.join(ROOT, "pack_tool.exe")
W, H = 886, 1920  # 逻辑尺寸；双 YUV 编码帧 1772x1920


def nv12_to_i420(nv12, w, h):
    n = w * h
    y = nv12[:n]
    uv = nv12[n:n + n // 2]
    return y + uv[0::2] + uv[1::2]


def dual_to_rgb(nv12_path, mode="unpack"):
    """双 YUV420 → RGB：NV12 → I420 → pack_tool unpack（§3.1 raw）/ unpack2（§3.2 YCoCg）。"""
    with open(nv12_path, "rb") as f:
        i420 = nv12_to_i420(f.read(), W * 2, H)
    tmp = os.path.join(OUT, "_tmp_dual.i420")
    rgb_raw = os.path.join(OUT, "_tmp_dual.rgb")
    with open(tmp, "wb") as f:
        f.write(i420)
    r = subprocess.run([PACK, mode, tmp, str(W), str(H), rgb_raw],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout, r.stderr)
        sys.exit(1)
    with open(rgb_raw, "rb") as f:
        img = Image.frombytes("RGB", (W, H), f.read())
    os.remove(tmp)
    os.remove(rgb_raw)
    return img


def single_to_rgb(nv12_path):
    """单 YUV420 → RGB：BT.601 limited range → 全量程（LUT）→ Pillow YCbCr→RGB。"""
    with open(nv12_path, "rb") as f:
        i420 = nv12_to_i420(f.read(), W, H)
    n = W * H
    y = Image.frombytes("L", (W, H), i420[:n])
    cb = Image.frombytes("L", (W // 2, H // 2), i420[n:n + n // 4]).resize((W, H), Image.BILINEAR)
    cr = Image.frombytes("L", (W // 2, H // 2), i420[n + n // 4:]).resize((W, H), Image.BILINEAR)
    # limited→full：Y'=(Y-16)*255/219，C'=(C-128)*255/224+128
    lut_y = [min(255, max(0, round((i - 16) * 255 / 219))) for i in range(256)]
    lut_c = [min(255, max(0, round((i - 128) * 255 / 224 + 128))) for i in range(256)]
    ycbcr = Image.merge("YCbCr", (y.point(lut_y), cb.point(lut_c), cr.point(lut_c)))
    return ycbcr.convert("RGB")


def psnr(a, b):
    """对两 RGB 图算 PSNR：返回 (总, [R, G, B])，dB。"""
    st = ImageStat.Stat(ImageChops.difference(a, b))
    mses = [r * r for r in st.rms]
    mse = sum(mses) / len(mses)
    peak = 255.0 * 255.0
    overall = float("inf") if mse == 0 else 10 * math.log10(peak / mse)
    per = [float("inf") if m == 0 else 10 * math.log10(peak / m) for m in mses]
    return overall, per


def fmt(v):
    return "inf" if v == float("inf") else f"{v:.2f}"


def main():
    ref = Image.open(os.path.join(OUT, "ref.png")).convert("RGB")
    rows = []
    # kind: single=BT.601 4:2 0；raw=§3.1 双 YUV 原样搬运；ycocg=§3.2 YCoCg 打包
    paths = [("single_6M", "single"), ("single_12M", "single"), ("single_40M", "single")] + \
            [(f"dual_{b}M", "raw") for b in (6, 9, 12)] + \
            [(f"dual_ycocg_{b}M", "ycocg") for b in (6, 9, 12, 20, 40)]
    for tag, kind in paths:
        nv12 = os.path.join(OUT, f"{tag}.nv12")
        if kind == "single":
            img = single_to_rgb(nv12)
        else:
            img = dual_to_rgb(nv12, "unpack2" if kind == "ycocg" else "unpack")
        png = os.path.join(OUT, f"{tag}.png")
        img.save(png)
        overall, per = psnr(ref, img)
        h264 = os.path.join(OUT, "h264", f"{tag}.h264")
        size = os.path.getsize(h264)
        rows.append((tag, size, overall, per))
        print(f"{tag}: {png}  h264={size}B  PSNR={fmt(overall)}dB")

    print()
    print(f"{'path':<16} {'h264 bytes':>10} {'PSNR(all)':>10} "
          f"{'R':>7} {'G':>7} {'B':>7}")
    for tag, size, overall, per in rows:
        print(f"{tag:<16} {size:>10} {fmt(overall):>9} "
              f"{fmt(per[0]):>7} {fmt(per[1]):>7} {fmt(per[2]):>7}")


if __name__ == "__main__":
    main()
