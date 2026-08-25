# PSNR 对比驱动（Pillow only）：
# 单 YUV（4:2:0 色度亚采样）@6Mbps vs 双 YUV（4:4:4 打包）@6M/9M/12Mbps，
# H.264 编解码往返走 mf_roundtrip（MF CBR），PSNR 对 ref.png。
import subprocess, os, sys
from PIL import Image, ImageChops, ImageStat

ROOT = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(ROOT, "out")
os.makedirs(OUT, exist_ok=True)
PACK = os.path.join(ROOT, "pack_tool.exe")
RT = os.path.join(ROOT, "mf_roundtrip.exe")

W, H = 886, 1920          # 逻辑尺寸（双 YUV 编码帧 1772x1920）
BITRATE_SINGLE = 6_000_000

def run(cmd):
    print("+", " ".join(str(c) for c in cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode != 0:
        print(r.stderr)
        sys.exit(1)

def psnr(a_img, b_img):
    diff = ImageChops.difference(a_img, b_img)
    st = ImageStat.Stat(diff)
    mses = [v * v for v in st.mean]  # st.mean 是差值均值；需要均方：用 rms！
    # ImageStat.Stat.rms 是各通道 RMS（差值的），MSE = rms^2
    mse_ch = [r * r for r in st.rms]
    mse = sum(mse_ch) / len(mse_ch)
    if mse == 0:
        return float("inf"), [float("inf")] * len(mse_ch)
    import math
    overall = 10 * math.log10(255.0 * 255.0 / mse)
    per = [10 * math.log10(255.0 * 255.0 / m) if m > 0 else float("inf") for m in mse_ch]
    return overall, per

def single_yuv_path(rgb_img, tag, bitrate):
    """RGB → BT.601 YCbCr 4:2:0 → 往返 → RGB png"""
    yc = rgb_img.convert("YCbCr")
    y, cb, cr = yc.split()
    cb_s = cb.resize((W // 2, H // 2), Image.BILINEAR)
    cr_s = cr.resize((W // 2, H // 2), Image.BILINEAR)
    yuv = y.tobytes() + cb_s.tobytes() + cr_s.tobytes()
    src = os.path.join(OUT, f"{tag}.yuv")
    with open(src, "wb") as f:
        f.write(yuv)
    dec = os.path.join(OUT, f"{tag}_dec.yuv")
    run([RT, src, str(W), str(H), str(bitrate), dec])
    n = W * H
    with open(dec, "rb") as f:
        data = f.read()
    y2 = Image.frombytes("L", (W, H), data[:n])
    cb2 = Image.frombytes("L", (W // 2, H // 2), data[n:n + n // 4]).resize((W, H), Image.BILINEAR)
    cr2 = Image.frombytes("L", (W // 2, H // 2), data[n + n // 4:]).resize((W, H), Image.BILINEAR)
    back = Image.merge("YCbCr", (y2, cb2, cr2)).convert("RGB")
    out_png = os.path.join(OUT, f"{tag}_rgb.png")
    back.save(out_png)
    return back, out_png

def dual_yuv_path(rgb_img, tag, bitrate):
    """RGB → RGBA → 双 YUV420 打包 → 往返 → 解包 → RGB png"""
    rgba = rgb_img.convert("RGBA").tobytes()
    src_rgba = os.path.join(OUT, f"{tag}.rgba")
    with open(src_rgba, "wb") as f:
        f.write(rgba)
    packed = os.path.join(OUT, f"{tag}_packed.yuv")
    run([PACK, "pack", src_rgba, str(W), str(H), packed])
    dec = os.path.join(OUT, f"{tag}_dec.yuv")
    run([RT, packed, str(W * 2), str(H), str(bitrate), dec])
    rgb_raw = os.path.join(OUT, f"{tag}_dec.rgb")
    run([PACK, "unpack", dec, str(W), str(H), rgb_raw])
    with open(rgb_raw, "rb") as f:
        data = f.read()
    back = Image.frombytes("RGB", (W, H), data)
    out_png = os.path.join(OUT, f"{tag}_rgb.png")
    back.save(out_png)
    return back, out_png

def main():
    ref = Image.open(os.path.join(OUT, "ref.png")).convert("RGB")
    results = []

    single, single_png = single_yuv_path(ref, "single_6M", BITRATE_SINGLE)
    p, per = psnr(ref, single)
    results.append(("single-yuv  @6Mbps", p, per, single_png))

    for mult, br in (("1.0x", 6_000_000), ("1.5x", 9_000_000), ("2.0x", 12_000_000)):
        dual, png = dual_yuv_path(ref, f"dual_{mult}", br)
        p, per = psnr(ref, dual)
        results.append((f"dual-yuv    @{br//1000000}Mbps({mult})", p, per, png))

    print("\n===== PSNR vs ref.png（RGB 三通道均值 + 总）=====")
    for name, p, per, png in results:
        per_s = " ".join(f"{c:.2f}" for c in per)
        print(f"{name}: PSNR={p:.2f} dB  [R/G/B: {per_s}]  ({png})")

if __name__ == "__main__":
    main()
