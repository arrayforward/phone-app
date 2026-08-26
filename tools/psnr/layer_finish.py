# 方案A 分层实验收尾（Pillow only）：
#   手机端 --layer-test 已产出码流（tools/psnr/out/layer/），本脚本：
#   1) vpl_decode 解码全部码流（oneVPL 硬解，输出已按 SPS 裁剪到 886x1920）
#   2) 跨解码器确定性校验：host 解码 base vs 手机 MediaCodec recon（逐字节）
#   3) 增强层逆变换（direct: -128；camo: 逐行 mod256 前缀和）→ 与 recon 合成
#   4) 合成帧 I420→RGB（沿用 psnr_finish.single_to_rgb 同一方法）→ PSNR/SSIM vs ref.png
#   5) 汇总表：体积/压缩比/PSNR/SSIM + 与 single 直编同总字节对比
# 用法：python tools/psnr/layer_finish.py
import math
import os
import subprocess
import sys

from PIL import Image, ImageChops, ImageFilter, ImageMath, ImageStat

ROOT = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(ROOT, "out")
LAYER = os.path.join(OUT, "layer")
VPL = os.path.join(ROOT, "vpl_decode.exe")
TOOL = os.path.join(ROOT, "layer_tool.exe")
W, H = 886, 1920
BASE_BPS = ["2M", "4M"]
ENH_BPS = ["500k", "1M", "2M", "4M", "8M"]
# 手机端日志实测（docs/layer_test_report.md 引用）：Rice 无损熵编码残差体积
RICE_BYTES = {"2M": 1462013, "4M": 1421603}
TIMINGS = {  # 手机实测（SD865，886x1920/帧）
    "residual_ms": (9, 8), "camo_ms": (3, 5), "rice_ms": (88, 93),
    "base_enc_ms": (104, 70), "base_dec_ms": (115, 123),
    "enh_enc_ms_direct": (69, 95), "enh_enc_ms_camo": (87, 142),
}


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("+", " ".join(cmd))
        print(r.stdout, r.stderr)
        sys.exit(1)
    return r.stdout.strip()


def decode(name):
    """layer/<name>.h264 -> layer/<name>.i420（vpl 解码 + NV12→I420）"""
    h264 = os.path.join(LAYER, name + ".h264")
    nv12 = os.path.join(LAYER, name + ".nv12")
    i420 = os.path.join(LAYER, name + ".i420")
    if not os.path.exists(i420):
        if not os.path.exists(nv12):
            run([VPL, h264, nv12])
        run([TOOL, "crop", nv12, str(W), str(H), str(W), str(H), i420])
    return i420


def i420_to_rgb(path):
    """与 psnr_finish.single_to_rgb 同一方法：BT.601 limited→full LUT + Pillow YCbCr→RGB。"""
    with open(path, "rb") as f:
        data = f.read()
    n = W * H
    y = Image.frombytes("L", (W, H), data[:n])
    cb = Image.frombytes("L", (W // 2, H // 2), data[n:n + n // 4]).resize((W, H), Image.BILINEAR)
    cr = Image.frombytes("L", (W // 2, H // 2), data[n + n // 4:]).resize((W, H), Image.BILINEAR)
    lut_y = [min(255, max(0, round((i - 16) * 255 / 219))) for i in range(256)]
    lut_c = [min(255, max(0, round((i - 128) * 255 / 224 + 128))) for i in range(256)]
    ycbcr = Image.merge("YCbCr", (y.point(lut_y), cb.point(lut_c), cr.point(lut_c)))
    return ycbcr.convert("RGB")


def psnr(a, b):
    st = ImageStat.Stat(ImageChops.difference(a, b))
    mses = [r * r for r in st.rms]
    mse = sum(mses) / len(mses)
    if mse == 0:
        return float("inf")
    return 10 * math.log10(255.0 * 255.0 / mse)


def ssim(a, b):
    """numpy SSIM（7x7 均匀窗局部统计，RGB 三通道均值）。"""
    import numpy as np
    C1, C2 = 6.5025, 58.5225
    def box7(m):
        # 7x7 均匀窗（中心对齐，edge 填充）：前置零行/列的积分图差分
        p = np.pad(m, 3, mode="edge")
        ii = np.zeros((p.shape[0] + 1, p.shape[1] + 1))
        ii[1:, 1:] = p.cumsum(0).cumsum(1)  # ii[i,j] = sum p[0..i-1, 0..j-1]
        h, w = m.shape
        # 输出 (r,c) 的窗 = padded 行 r..r+6（ii 区间 [r, r+7)）
        return (ii[7:7 + h, 7:7 + w] - ii[0:h, 7:7 + w]
                - ii[7:7 + h, 0:w] + ii[0:h, 0:w]) / 49.0

    total = 0.0
    for ch in range(3):
        x = np.asarray(a.split()[ch], dtype=np.float64)
        y = np.asarray(b.split()[ch], dtype=np.float64)
        mu_x, mu_y = box7(x), box7(y)
        sx = box7(x * x) - mu_x * mu_x
        sy = box7(y * y) - mu_y * mu_y
        sxy = box7(x * y) - mu_x * mu_y
        num = (2 * mu_x * mu_y + C1) * (2 * sxy + C2)
        den = (mu_x ** 2 + mu_y ** 2 + C1) * (sx + sy + C2)
        total += float(np.mean(num / den))
    return total / 3


def fmt(v):
    return "inf" if v == float("inf") else f"{v:.2f}"


def main():
    ref = Image.open(os.path.join(OUT, "ref.png")).convert("RGB")
    report = []
    rows = []

    # 1) 跨解码器确定性校验 + base-only 基线
    report.append("## 1. 跨解码器确定性校验（闭环成立前提）\n")
    for bb in BASE_BPS:
        recon_host = decode(f"layer_base_{bb}")
        out = run([TOOL, "reconcmp", recon_host,
                   os.path.join(LAYER, f"layer_recon_{bb}.i420"), str(W), str(H)])
        report.append(f"- base@{bb}：oneVPL(host) vs MediaCodec(手机) 重建 → `{out}`")
    report.append("")

    # 2) 分层合成 PSNR/SSIM
    report.append("## 2. 分层合成质量（基础层 + 增强层，PSNR/SSIM vs ref）\n")
    report.append("| 基础层 | 增强 | 增强码率 | 增强字节 | 总字节 | PSNR(dB) | SSIM |")
    report.append("|---|---|---|---|---|---|---|")
    for bb in BASE_BPS:
        recon_phone = os.path.join(LAYER, f"layer_recon_{bb}.i420")
        base_rgb = i420_to_rgb(recon_phone)
        base_psnr = psnr(ref, base_rgb)
        base_ssim = ssim(ref, base_rgb)
        base_bytes = os.path.getsize(os.path.join(LAYER, f"layer_base_{bb}.h264"))
        report.append(f"| {bb} | （无增强） | - | 0 | {base_bytes//1024}K "
                      f"| {fmt(base_psnr)} | {base_ssim:.4f} |")
        rows.append((bb, "base", "-", 0, base_bytes, base_psnr, base_ssim))
        for kind in ("direct", "camo"):
            for be in ENH_BPS:
                name = f"layer_{kind}_{bb}_{be}"
                enh = decode(name)
                if kind == "camo":
                    un = os.path.join(LAYER, name + "_un.i420")
                    run([TOOL, "uncamo", enh, str(W), str(H), un])
                    enh = un
                comp = os.path.join(LAYER, name + "_comp.i420")
                run([TOOL, "composite", recon_phone, enh, str(W), str(H), comp])
                img = i420_to_rgb(comp)
                p = psnr(ref, img)
                s = ssim(ref, img)
                eb = os.path.getsize(os.path.join(LAYER, name + ".h264"))
                total = base_bytes + eb
                report.append(f"| {bb} | {kind} | {be} | {eb//1024}K | {total//1024}K "
                              f"| {fmt(p)} | {s:.4f} |")
                rows.append((bb, kind, be, eb, total, p, s))
                # 关键档位保存文字区域放大对比图（报告配图）
                if (bb, kind, be) in (("4M", "direct", "2M"), ("4M", "direct", "8M"),
                                      ("4M", "camo", "8M")):
                    crop_png = os.path.join(LAYER, f"cmp_{kind}_{bb}_{be}_text.png")
                    img.crop((120, 500, 620, 1100)).resize(
                        (1000, 1200), Image.NEAREST).save(crop_png)
        # base-only 也存一张对照
        base_rgb.crop((120, 500, 620, 1100)).resize((1000, 1200), Image.NEAREST).save(
            os.path.join(LAYER, f"cmp_base_{bb}_text.png"))

    # 3) 压缩比：同档 direct vs camo
    report.append("")
    report.append("## 3. 码流体积对比（差分伪装 vs 直偏置）\n")
    report.append("| 基础层 | 增强码率 | direct 字节 | camo 字节 | camo/direct |")
    report.append("|---|---|---|---|---|")
    for bb in BASE_BPS:
        for be in ENH_BPS:
            d = os.path.getsize(os.path.join(LAYER, f"layer_direct_{bb}_{be}.h264"))
            c = os.path.getsize(os.path.join(LAYER, f"layer_camo_{bb}_{be}.h264"))
            report.append(f"| {bb} | {be} | {d//1024}K | {c//1024}K | {c/d:.2f}x |")
    report.append("")
    report.append(f"Rice 无损熵编码残差体积（对照）：2M 基础 {RICE_BYTES['2M']//1024}K、"
                  f"4M 基础 {RICE_BYTES['4M']//1024}K。")
    report.append("")

    # 4) 耗时（手机实测）
    report.append("## 4. 各环节耗时（SD865，886x1920/帧，实测）\n")
    t = TIMINGS
    report.append(f"- 残差计算（native）：{t['residual_ms'][0]}~{t['residual_ms'][1]}ms")
    report.append(f"- 迷彩差分（native）：{t['camo_ms'][0]}~{t['camo_ms'][1]}ms")
    report.append(f"- 基础层编码：{t['base_enc_ms'][0]}~{t['base_enc_ms'][1]}ms；"
                  f"闭环解码：{t['base_dec_ms'][0]}~{t['base_dec_ms'][1]}ms")
    report.append(f"- 增强层编码：direct {t['enh_enc_ms_direct'][0]}~{t['enh_enc_ms_direct'][1]}ms，"
                  f"camo {t['enh_enc_ms_camo'][0]}~{t['enh_enc_ms_camo'][1]}ms")
    report.append(f"- Rice 熵编码（对照）：{t['rice_ms'][0]}~{t['rice_ms'][1]}ms")
    report.append("")

    with open(os.path.join(OUT, "layer_results.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(report) + "\n")
    print("\n".join(report))
    print("==> written", os.path.join(OUT, "layer_results.md"))


if __name__ == "__main__":
    main()
