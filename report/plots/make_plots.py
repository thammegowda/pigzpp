#!/usr/bin/env python3
"""Generate the whitepaper's bar charts from the benchmark data in ./data/*.tsv.

Every figure in the paper is produced here from checked-in numbers, so the plots
are fully reproducible and easy to update: edit the relevant TSV under
``plots/data/`` and re-run ``python make_plots.py`` (or ``make plots``).

Output: one vector PDF per benchmark in this directory (``plots/*.pdf``), which
``main.tex`` includes via ``\\includegraphics``.

Dependencies: matplotlib, pandas, seaborn.
"""
from __future__ import annotations

import pathlib

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
import seaborn as sns
from matplotlib import font_manager

HERE = pathlib.Path(__file__).resolve().parent
DATA = HERE / "data"
FONTS = HERE.parent / "fonts"

# ---- Palette (matches pigzpp-whitepaper.cls) -------------------------------
INK = "#1B1F24"
ACCENT = "#0B6E4F"   # deep green  -> pigzpp ISA-L backend
ACCENT2 = "#C1543B"  # terracotta  -> pigzpp zlib-ng backend
SLATE = "#51606B"    # muted       -> competitors / other tools
LIGHT = "#AEB8BF"    # light gray  -> secondary series (e.g. ZIP read)
GRID = "#D8DEE3"
RATIO = "#72519A"    # purple      -> compression ratio / relative output size

# ---- Use the paper's Libertinus fonts when they are available --------------
def _register_fonts() -> str:
    family = "DejaVu Serif"
    faces = {
        "LibertinusSerif-Regular.otf": None,
        "LibertinusSerif-Bold.otf": None,
        "LibertinusSans-Regular.otf": None,
    }
    found = False
    for name in faces:
        path = FONTS / name
        if path.exists():
            font_manager.fontManager.addfont(str(path))
            found = True
    if found:
        family = "Libertinus Serif"
    return family


def _base_style() -> None:
    family = _register_fonts()
    sns.set_theme(style="whitegrid")
    plt.rcParams.update({
        "font.family": family,
        "font.size": 10,
        "text.color": INK,
        "axes.labelcolor": INK,
        "axes.edgecolor": SLATE,
        "xtick.color": INK,
        "ytick.color": INK,
        "axes.grid": True,
        "grid.color": GRID,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "figure.dpi": 150,
        "savefig.bbox": "tight",
        "savefig.pad_inches": 0.02,
    })


def _color_for(label: str) -> str:
    """Backend color code: green = ISA-L, terracotta = zlib-ng, slate = other.

    pigzpp's WASM build and its PNG ``balanced``/``small`` presets run on
    zlib-ng, so they are terracotta; ``auto``/``isal``/``fast`` are ISA-L.
    """
    low = label.lower()
    if "pigzpp" not in low and "wasm" not in low:
        return SLATE
    if any(k in low for k in ("zlib", "wasm", "balanced", "small")):
        return ACCENT2
    return ACCENT


def _save(fig: plt.Figure, name: str) -> None:
    out = HERE / f"{name}.pdf"
    fig.savefig(out)
    plt.close(fig)
    print(f"  wrote {out.relative_to(HERE.parent)}")


def _barh_values(ax, bars, labels, fontsize=8.5) -> None:
    """Write the value at the end of each horizontal bar."""
    for rect, label in zip(bars, labels):
        w = rect.get_width()
        ax.text(w, rect.get_y() + rect.get_height() / 2, f" {label}",
                va="center", ha="left", fontsize=fontsize, color=INK)


def _secondary_metric(ax, positions, values, label, *, fmt="{:.2f}",
                      position_offset=0.22, annotate_values=True,
                      axis_min=None, axis_max=None):
    """Add a contrasting top x-axis for ratio or relative output size.

    The report uses horizontal bars to accommodate long implementation names,
    so the natural counterpart to the lower throughput x-axis is a top x-axis
    (rather than a right y-axis). Purple diamonds and labels encode the second
    metric independently of backend bar colors.
    """
    values = list(values)
    positions = [position + position_offset for position in positions]
    secondary = ax.twiny()
    low, high = min(values), max(values)
    span = high - low
    if span == 0:
        span = max(abs(high) * 0.08, 0.1)
    left = axis_min if axis_min is not None else low - span * 0.28
    right = axis_max if axis_max is not None else high + span * 0.48
    secondary.set_xlim(left, right)
    secondary.set_ylim(ax.get_ylim())
    secondary.scatter(values, positions, color=RATIO, marker="D", s=18,
                      edgecolor="white", linewidth=0.45, zorder=5)
    if annotate_values:
        for x, y in zip(values, positions):
            secondary.annotate(fmt.format(x), (x, y), xytext=(4, 0),
                               textcoords="offset points", va="center", ha="left",
                               fontsize=6.8, color=RATIO,
                               bbox={"facecolor": "white", "edgecolor": "none",
                                     "alpha": 0.78, "pad": 0.15})
    secondary.set_xlabel(label, fontsize=7.4, color=RATIO, labelpad=2)
    secondary.tick_params(axis="x", labelsize=6.8, colors=RATIO, pad=1,
                          length=2.5)
    secondary.tick_params(axis="y", left=False, right=False,
                          labelleft=False, labelright=False)
    secondary.grid(False)
    secondary.spines["top"].set_visible(True)
    secondary.spines["top"].set_color(RATIO)
    secondary.spines["bottom"].set_visible(False)
    secondary.spines["left"].set_visible(False)
    secondary.spines["right"].set_visible(False)
    return secondary


# ---- Individual figures ----------------------------------------------------
def plot_cli() -> None:
    df = pd.read_csv(DATA / "cli.tsv", sep="\t").sort_values("mbps", ascending=False)
    colors = [_color_for(t) for t in df["tool"]]
    # Taller, poster-like proportions: this is the first-page headline figure.
    fig, ax = plt.subplots(figsize=(5.8, 2.45))
    bars = ax.barh(df["tool"], df["mbps"], color=colors)
    _barh_values(ax, bars, [f"{v:.0f} MB/s" for v in df["mbps"]], fontsize=9)
    ax.set_xlabel("Compression throughput (MB/s) — 128 MB text, level 6, 8 workers",
                  fontsize=9)
    ax.tick_params(axis="both", labelsize=8.7)
    ax.set_xlim(0, df["mbps"].max() * 1.17)
    _secondary_metric(ax, range(len(df)), df["ratio"],
                      "Compression ratio (input/output; higher is smaller)",
                      axis_min=1.0, axis_max=3.0)
    fig.tight_layout()
    _save(fig, "cli")


def _hbar(ax, labels, values, ratios=None, xmax_pad=1.30,
          secondary_label="Compression ratio (input/output)",
          secondary_annotations=True):
    """Horizontal bars ordered slow->fast (slowest at top, fastest at bottom)."""
    order = sorted(range(len(values)), key=lambda i: values[i], reverse=True)
    labels = [labels[i] for i in order]
    values = [values[i] for i in order]
    ratios = [ratios[i] for i in order] if ratios is not None else None
    colors = [_color_for(l) for l in labels]
    bars = ax.barh(range(len(values)), values, color=colors)
    ax.set_yticks(range(len(values)))
    ax.set_yticklabels(labels, fontsize=8.5)
    labels_at_end = [f"{v:.0f}" if v >= 10 else f"{v:.1f}" for v in values]
    _barh_values(ax, bars, labels_at_end, fontsize=8)
    ax.set_xlim(0, max(values) * xmax_pad)
    if ratios is not None:
        _secondary_metric(ax, range(len(values)), ratios, secondary_label,
                          annotate_values=secondary_annotations,
                          axis_min=1.0, axis_max=3.0)


def plot_language() -> None:
    """One column-native figure per language, each with its competitor set."""
    panels = [
        ("Python — in-memory bytes API", "language_python.tsv", "language_python"),
        ("Go — cgo binding", "language_go.tsv", "language_go"),
        ("Rust — FFI binding", "language_rust.tsv", "language_rust"),
    ]
    for title, fname, output in panels:
        fig, ax = plt.subplots(figsize=(3.3, 2.55))
        df = pd.read_csv(DATA / fname, sep="\t")
        _hbar(ax, list(df["method"]), list(df["mbps"]), list(df["ratio"]),
              xmax_pad=1.38)
        ax.set_title(title, loc="left", fontsize=9, fontweight="bold",
                 color=INK, pad=24)
        ax.set_xlabel("Throughput (MB/s) — 128 MB text, L6, 8 workers", fontsize=8)
        ax.tick_params(axis="both", labelsize=7.2)
        fig.tight_layout()
        _save(fig, output)


def plot_docker() -> None:
    df = pd.read_csv(DATA / "docker.tsv", sep="\t").sort_values("mbps", ascending=False)
    display = df["method"].replace({
        "compress/gzip (Go stdlib)": "Go stdlib gzip",
        "klauspost/compress": "klauspost/gzip",
        "klauspost/pgzip (parallel)": "klauspost/pgzip",
        "pigzpp zlib (cgo)": "pigzpp zlib",
        "pigzpp isal (cgo)": "pigzpp isal",
    })
    colors = [_color_for(m) for m in df["method"]]
    fig, ax = plt.subplots(figsize=(3.3, 2.8))
    bars = ax.barh(display, df["mbps"], color=colors)
    _barh_values(ax, bars, [f"{v:.0f}  {s:.0f}x"
                            for v, s in zip(df["mbps"], df["speedup"])],
                 fontsize=7)
    ax.set_xlabel("Layer compression (MB/s) — 637 MB", fontsize=8)
    ax.tick_params(axis="both", labelsize=7.5)
    ax.set_xlim(0, df["mbps"].max() * 1.32)
    _secondary_metric(ax, range(len(df)), df["ratio"],
                      "Compression ratio (input/output)",
                      axis_min=1.0, axis_max=3.0)
    fig.tight_layout()
    _save(fig, "docker")


def plot_wasm() -> None:
    """Separate column-native engine-comparison and worker-scaling figures."""
    single = pd.read_csv(DATA / "wasm_single.tsv", sep="\t")
    fig, ax1 = plt.subplots(figsize=(3.3, 2.65))
    _hbar(ax1, list(single["engine"]), list(single["mbps"]),
            list(single["ratio"]), xmax_pad=1.45,
            secondary_annotations=False)
    ax1.set_title("Single worker — 16 MB", loc="left", fontsize=9,
                  fontweight="bold", color=INK, pad=24)
    ax1.set_xlabel("Throughput (MB/s)", fontsize=8)
    ax1.tick_params(axis="both", labelsize=7.2)
    fig.tight_layout()
    _save(fig, "wasm_single")

    scale = pd.read_csv(DATA / "wasm_scaling.tsv", sep="\t")
    fig, ax2 = plt.subplots(figsize=(3.3, 2.35))
    ax2.bar([str(t) for t in scale["threads"]], scale["mbps"],
            width=0.56, color=ACCENT2)
    ax2.bar_label(ax2.containers[0],
                  labels=[f"{m:.0f}\n{sp:.1f}x"
                          for m, sp in zip(scale["mbps"], scale["speedup"])],
                  fontsize=7.5, color=INK, padding=2)
    ax2.set_title("pigzpp-wasm scaling — 128 MB", loc="left",
                  fontsize=9, fontweight="bold", color=INK)
    ax2.set_xlabel("worker threads", fontsize=8)
    ax2.set_ylabel("MB/s", fontsize=8)
    ax2.tick_params(axis="both", labelsize=7.2)
    ax2.set_ylim(0, scale["mbps"].max() * 1.22)
    fig.tight_layout()
    _save(fig, "wasm_scaling")


def plot_png() -> None:
    df = pd.read_csv(DATA / "png.tsv", sep="\t").sort_values("imgs_per_s", ascending=False)
    colors = [_color_for(e) for e in df["encoder"]]
    fig, ax = plt.subplots(figsize=(3.3, 4.05))
    bars = ax.barh(df["encoder"], df["imgs_per_s"], color=colors)
    labels = [(f"{v:.0f}" if v >= 10 else f"{v:.1f}") + " img/s"
              for v in df["imgs_per_s"]]
    _barh_values(ax, bars, labels, fontsize=6.5)
    ax.set_xlabel("PNG encoding (images/s) — Kodak RGB", fontsize=8)
    ax.tick_params(axis="both", labelsize=7)
    ax.set_xlim(0, df["imgs_per_s"].max() * 1.30)
    _secondary_metric(ax, range(len(df)), df["size"],
                      "Relative output size (lower is smaller)",
                      annotate_values=False, axis_min=0.5, axis_max=1.5)
    fig.tight_layout()
    _save(fig, "png")


def plot_zip() -> None:
    df = pd.read_csv(DATA / "zip.tsv", sep="\t")
    tidy = df.melt(id_vars="writer", value_vars=["write_mbps", "read_mbps"],
                   var_name="op", value_name="mbps")
    tidy["op"] = tidy["op"].map({"write_mbps": "write", "read_mbps": "read"})
    order = ["zipfile (stdlib)", "pigzpp zlib", "pigzpp isal"]
    fig, ax = plt.subplots(figsize=(3.3, 2.7))
    sns.barplot(data=tidy, y="writer", x="mbps", hue="op", order=order,
                hue_order=["write", "read"], palette=[SLATE, LIGHT], ax=ax)
    ax.set_xlabel("MB/s — 128 MB text, 8 threads")
    ax.set_ylabel("")
    legend = ax.get_legend()
    if legend is not None:
        legend.remove()
    for c, operation in zip(ax.containers, ["write", "read"]):
        labels = [f"{operation} {bar.get_width():.0f}" for bar in c]
        ax.bar_label(c, labels=labels, padding=2, fontsize=6.8, color=INK)
    ax.set_xlim(0, tidy["mbps"].max() * 1.18)
    ratios = [float(df.loc[df["writer"] == writer, "ratio"].iloc[0])
              for writer in order]
    _secondary_metric(ax, range(len(order)), ratios,
                      "Archive compression ratio (input/output)",
                      position_offset=0, axis_min=1.0, axis_max=3.0)
    _save(fig, "zip")


def plot_decompression() -> None:
    """Two compact panels: native CLI and Python in-memory decompression."""
    df = pd.read_csv(DATA / "decompression.tsv", sep="\t")
    fig, axes = plt.subplots(2, 1, figsize=(3.3, 3.65))
    for ax, scope, title in zip(axes, ["CLI", "Python"],
                                ["Native CLI", "Python in-memory API"]):
        part = df[df["scope"] == scope]
        _hbar(ax, list(part["method"]), list(part["mbps"]), xmax_pad=1.28)
        ax.set_title(title, loc="left", fontsize=9, fontweight="bold", color=INK)
        ax.tick_params(axis="both", labelsize=7.2)
    axes[-1].set_xlabel("Decompression throughput (MB/s) — 128 MB", fontsize=8)
    fig.tight_layout(h_pad=1.0)
    _save(fig, "decompression")


def plot_native_scaling() -> None:
    """Native compression throughput by worker count."""
    df = pd.read_csv(DATA / "native_scaling.tsv", sep="\t")
    fig, ax = plt.subplots(figsize=(3.3, 2.5))
    for method in ["pigz", "pigzpp zlib", "pigzpp isal"]:
        part = df[df["method"] == method].sort_values("workers")
        ax.plot(part["workers"], part["mbps"], marker="o", markersize=3.5,
                linewidth=1.7, label=method, color=_color_for(method))
    ax.axvline(5, color=GRID, linewidth=1.0, linestyle="--")
    ax.text(5.1, ax.get_ylim()[1] * 0.90, "5 physical cores",
            fontsize=6.7, color=SLATE, rotation=90, va="top")
    ax.set_xlabel("worker threads", fontsize=8)
    ax.set_ylabel("MB/s", fontsize=8)
    ax.tick_params(axis="both", labelsize=7.2)
    ax.legend(frameon=False, fontsize=7, loc="upper left")
    fig.tight_layout()
    _save(fig, "native_scaling")


def plot_robustness() -> None:
    """Speed-versus-ratio at levels 1/6/9 for text and random corpora."""
    df = pd.read_csv(DATA / "robustness.tsv", sep="\t")
    fig, axes = plt.subplots(1, 2, figsize=(6.6, 2.55))
    for ax, corpus, title in zip(axes, ["txt", "bin"],
                                 ["Multilingual text", "Incompressible random"]):
        part = df[df["corpus"] == corpus]
        for method in ["pigz", "pigzpp zlib", "pigzpp isal"]:
            rows = part[part["method"] == method].sort_values("level")
            # Random-data streams can be a few bytes larger than their input,
            # yielding ratio < 1. Keep the common 1–3 scale and place those
            # points on its left boundary with a left-facing marker.
            plot_ratio = rows["ratio"].clip(lower=1.0, upper=3.0)
            ax.plot(plot_ratio, rows["mbps"], marker="o", markersize=4,
                    linewidth=1.5, label=method, color=_color_for(method))
            for _, row in rows.iterrows():
                x = min(3.0, max(1.0, row["ratio"]))
                if row["ratio"] < 1.0:
                    ax.scatter([x], [row["mbps"]], marker="<", s=22,
                               color=_color_for(method), clip_on=False, zorder=4)
                ax.annotate(f"L{int(row['level'])}", (x, row["mbps"]),
                            xytext=(3, 3), textcoords="offset points", fontsize=6.5)
        ax.set_title(title, loc="left", fontsize=9, fontweight="bold", color=INK)
        ax.set_xlabel("compression ratio (input/output)", fontsize=8, color=RATIO)
        ax.set_xlim(1.0, 3.0)
        ax.tick_params(axis="x", labelsize=7.2, colors=RATIO)
        ax.tick_params(axis="y", labelsize=7.2)
        ax.spines["bottom"].set_color(RATIO)
    axes[0].set_ylabel("compression throughput (MB/s)", fontsize=8)
    axes[0].legend(frameon=False, fontsize=7, loc="best")
    fig.tight_layout(w_pad=1.4)
    _save(fig, "robustness")


def main() -> None:
    _base_style()
    # Remove obsolete composites when upgrading from older plot layouts.
    for obsolete in (HERE / "language.pdf", HERE / "wasm.pdf"):
        obsolete.unlink(missing_ok=True)
    print("Generating report figures:")
    plot_cli()
    plot_language()
    plot_docker()
    plot_wasm()
    plot_png()
    plot_zip()
    plot_decompression()
    plot_native_scaling()
    plot_robustness()
    print("Done.")


if __name__ == "__main__":
    main()
