#!/usr/bin/env python3
"""Plot latency samples written by ./latency.

Usage:
    python3 plot_latency.py other.csv fifo.csv fifo-stress.csv

Each file becomes one series. The plot is saved next to the first file as
<name>.png (there is no window: the lab machine has no display). Copy it to
your computer with scp to look at it.

Needs python3-matplotlib and python3-numpy, both installed on the lab machine
and in the Docker image. Do not 'pip install' them: Debian blocks that.
"""

import os
import sys

import matplotlib

matplotlib.use("Agg")  # render to a file, no display needed
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402


def load(path):
    data = np.loadtxt(path, comments="#", ndmin=2)
    if data.size == 0:
        sys.exit(f"{path}: no samples")
    return data[:, 1]  # latency in microseconds


def main(paths):
    series = [(os.path.splitext(os.path.basename(p))[0], load(p)) for p in paths]

    fig, (ax_time, ax_hist) = plt.subplots(1, 2, figsize=(13, 5))
    for name, lat in series:
        ax_time.plot(lat, linewidth=0.6, label=name)
        # Log scale on the counts: the rare, large values are the interesting ones.
        ax_hist.hist(lat, bins=100, histtype="step", log=True, label=name)
        print(f"{name:>20}: n={lat.size} min={lat.min():.0f} avg={lat.mean():.0f} "
              f"p99={np.percentile(lat, 99):.0f} max={lat.max():.0f} us")

    ax_time.set(title="Latency per wake-up", xlabel="iteration", ylabel="latency (us)")
    ax_hist.set(title="Distribution (log count)", xlabel="latency (us)", ylabel="count")
    for ax in (ax_time, ax_hist):
        ax.grid(alpha=0.3)
        ax.legend()
    fig.tight_layout()

    out = os.path.splitext(paths[0])[0] + ".png"
    fig.savefig(out, dpi=120)
    print(f"plot saved to {out}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
