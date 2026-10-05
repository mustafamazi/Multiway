"""Plots the fractional delay scans (MultiwayTests --scan) for the paper, together with the theoretical
cos(pi r / N) curve. Two figures:
  - fractional_delay_gain: floor, round and (if present) round + gain compensation (the one in the paper).
  - fractional_delay_floor_vs_round: floor and round only (the comparison before compensation).

    python3 plugin/Tests/plot_fractional_delay.py

Reads the CSVs under results/ (44.1 kHz only) and writes each figure twice:
  - <name>.pdf: IEEE single column, 3.5 x 3.2 in, 8 pt, tight bbox.
  - <ad>.png: 300 dpi.
The top panel is the impulse peak amplitude (the worst of 8 impulse positions), the bottom panel the energy
outside ±H of the expected position (wrap-around / aliasing). Opens fractional_delay_gain.png with "open" after saving.
"""

import csv
import math
import subprocess
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

RESULTS = Path(__file__).resolve().parent / "results"
N = 2048
SAMPLE_RATE = "44100"

# seaborn "colorblind" palette: tab10 order (blue, orange, green), colourblind-friendly tones.
# The markers differ too, so the distinction doesn't rely on colour alone.
METHODS = [
    ("fractional_delay_floor.csv",      "floor",         "#de8f05", "o"),
    ("fractional_delay_round.csv",      "round",         "#0173b2", "s"),
    ("fractional_delay_round_comp.csv", "round + comp.", "#029e73", "^"),
]
THEORY_COLOUR = "0.45"

# (file name, methods to plot)
FIGURES = [
    ("fractional_delay_gain",           ["floor", "round", "round + comp."]),
    ("fractional_delay_floor_vs_round", ["floor", "round"]),
]

STYLE = {
    "font.size": 8, "axes.labelsize": 8, "axes.titlesize": 8, "legend.fontsize": 8,
    "xtick.labelsize": 8, "ytick.labelsize": 8,
    "font.family": "serif", "font.serif": ["Times New Roman", "Times", "Nimbus Roman", "DejaVu Serif"],
    "mathtext.fontset": "stix",
    "pdf.fonttype": 42,   # embedded TrueType (IEEE PDF eXpress doesn't accept Type 3)
    "lines.linewidth": 1.5, "lines.markersize": 3,
    "axes.titlepad": 3, "axes.labelpad": 2,
    "xtick.major.pad": 2, "ytick.major.pad": 2,
}


def read_rows(path):
    with open(path, newline="") as f:
        return [row for row in csv.DictReader(f) if row["sample_rate"] == SAMPLE_RATE]


def plot_figure(stem, method_labels):
    fig, (gain_ax, alias_ax) = plt.subplots(2, 1, figsize=(3.5, 3.2), sharex=True, layout="constrained",
                                            gridspec_kw={"height_ratios": [3, 2]})
    fig.get_layout_engine().set(h_pad=0.01, hspace=0.02)

    for filename, label, colour, marker in METHODS:
        path = RESULTS / filename
        if label not in method_labels or not path.exists():
            continue

        rows = read_rows(path)
        x = [float(r["r_over_N"]) for r in rows]
        gain_ax.plot(x, [abs(float(r["peak_gain"])) for r in rows], color=colour, marker=marker, label=label)
        alias_ax.plot(x, [float(r["alias_energy_db"]) for r in rows], color=colour, marker=marker, label=label)

    theory_x = [i / 1000 * 0.25 for i in range(1001)]
    gain_ax.plot(theory_x, [math.cos(math.pi * v) for v in theory_x], "--", color=THEORY_COLOUR,
                 linewidth=1.0, label=r"theory $\cos(\pi r / N)$")

    gain_ax.set_title(rf"Peak gain vs. fractional delay ($N = {N}$)")
    gain_ax.set_ylabel("Peak gain")
    gain_ax.set_ylim(0.68, 1.04)

    # Region where the data is empty: right of floor's steep rise at r = 0, below the falling curves.
    gain_ax.legend(loc="lower left", bbox_to_anchor=(0.09, 0.0), frameon=True, framealpha=0.6,
                   edgecolor="none", borderpad=0.3, labelspacing=0.25, handlelength=2.0, borderaxespad=0.3)

    alias_ax.set_title(r"Energy outside $\pm H$ of the impulse")
    alias_ax.set_ylabel("Aliasing energy (dB)")
    alias_ax.set_xlabel(r"Fractional delay $r/N$")
    alias_ax.set_ylim(-100, -10)
    alias_ax.set_yticks([-90, -60, -30])
    alias_ax.set_xlim(-0.006, 0.256)

    fig.align_ylabels((gain_ax, alias_ax))

    pdf = RESULTS / f"{stem}.pdf"
    png = RESULTS / f"{stem}.png"
    fig.savefig(pdf, bbox_inches="tight", pad_inches=0.02)
    fig.savefig(png, dpi=300, bbox_inches="tight", pad_inches=0.02)
    plt.close(fig)

    print(pdf)
    print(png)
    return png


def main():
    plt.style.use("seaborn-v0_8-darkgrid")
    plt.rcParams.update(STYLE)

    pngs = [plot_figure(stem, labels) for stem, labels in FIGURES]

    if sys.platform == "darwin":
        subprocess.run(["open", str(pngs[0])], check=False)


if __name__ == "__main__":
    main()
