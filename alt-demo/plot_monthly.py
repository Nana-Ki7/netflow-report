#!/usr/bin/env python3
"""netflow-report 出图替代方案 demo。

和 cpp/main.cpp 走的是同一条数据链路（读同一个 CSV），区别只在最后一步：

    cpp 版：手拼 HTML 字符串 -> 内嵌 ECharts -> 浏览器渲染（依赖 CDN）
    本 demo：数据直接交给画图库 -> 直接吐 .svg / .png（无 HTML、无浏览器、离线可用）

用法：
    python3 plot_monthly.py [csv路径] [输出前缀]
默认：
    csv    = cpp/data/sample_monthly.csv
    输出   = alt-demo/preview_monthly.svg + .png
"""
import csv
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager

# —— 中文字体（选第一个装了的） ——
for cand in ("Noto Sans CJK SC", "WenQuanYi Zen Hei", "Noto Sans CJK JP"):
    try:
        font_manager.findfont(cand, fallback_to_default=False)
        plt.rcParams["font.sans-serif"] = [cand]
        break
    except Exception:
        continue
plt.rcParams["axes.unicode_minus"] = False


def load(path):
    """读 CSV，跳过注释行（# 开头的行不算数据）。"""
    with open(path, encoding="utf-8") as fh:
        lines = [ln for ln in fh if not ln.lstrip().startswith("#")]
    return list(csv.DictReader(lines))


def num(row, key):
    try:
        return float(row.get(key) or 0)
    except ValueError:
        return 0.0


def main():
    src = Path(sys.argv[1] if len(sys.argv) > 1 else "cpp/data/sample_monthly.csv")
    out = Path(sys.argv[2] if len(sys.argv) > 2 else "alt-demo/preview_monthly")
    out.parent.mkdir(parents=True, exist_ok=True)

    rows = load(src)
    months = [r["month"] for r in rows]
    total = [num(r, "total_gb") for r in rows]
    up = [num(r, "upload_gb") for r in rows]
    down = [num(r, "download_gb") for r in rows]
    fee = [num(r, "should_pay_yuan") for r in rows]

    fig, ax = plt.subplots(figsize=(10, 4.6), dpi=140)
    x = list(range(len(months)))

    bars = ax.bar(x, total, width=0.55, color="#5b8ff9", label="总流量")
    for i, r in enumerate(rows):          # 超出套餐的月份标红
        if num(r, "over_usage_gb") > 0:
            bars[i].set_color("#e8684a")
    ax.bar_label(bars, fmt="%.1f", fontsize=8, padding=2)

    ax.plot(x, up, "-o", ms=4, color="#61ddaa", label="上传")
    ax.plot(x, down, "-o", ms=4, color="#f6bd16", label="下载")
    ax.set_xticks(x)
    ax.set_xticklabels(months, rotation=45, ha="right", fontsize=9)
    ax.set_ylabel("流量 (GB)")
    ax.grid(axis="y", ls=":", alpha=0.4)
    ax.set_axisbelow(True)

    ax2 = ax.twinx()
    ax2.plot(x, fee, "--s", ms=4, color="#e8684a", label="应缴费用")
    ax2.set_ylabel("费用 (元)")

    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, loc="upper left", ncol=5, fontsize=9, frameon=False)
    ax.set_title("校园网流量 · 按月（数据直出矢量图，全程没有 HTML）", fontsize=12)

    fig.tight_layout()
    fig.savefig(out.with_suffix(".svg"))     # 矢量：可缩放、可嵌博客
    fig.savefig(out.with_suffix(".png"))     # 预览：方便直接看/发
    for p in (out.with_suffix(".svg"), out.with_suffix(".png")):
        print(f"{p}  {p.stat().st_size / 1024:.1f} KB")


if __name__ == "__main__":
    main()
