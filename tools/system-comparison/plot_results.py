#!/usr/bin/env python3
"""Render standalone charts from verified local measurements."""
import argparse
import json
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("summary",type=Path)
parser.add_argument("--output",type=Path,required=True)
args=parser.parse_args();data=json.loads(args.summary.read_text())
fig,axes=plt.subplots(1,2,figsize=(13,5.2),layout="constrained")
colors={"PXA":"#2563eb","Aster":"#e89519","PXA release":"#5995e8"}
labels=["Integer mix","Float recurrence","Sequential memory","Pointer chase","64x64 matrix"]
y=np.arange(5);ax=axes[0]
for delta,name in [(-.18,"PXA"),(.18,"Aster")]:
    values=[r["median_ms"] for r in data["cpu"][name.lower()]]
    bars=ax.barh(y+delta,values,height=.34,label=name,color=colors[name])
    ax.bar_label(bars,fmt="%.1f",padding=3,fontsize=9)
ax.set(yticks=y,yticklabels=labels,xlabel="Milliseconds (lower is better)",title="Same compiled compute kernel")
ax.invert_yaxis();ax.set_xlim(0,300);ax.legend(loc="lower right");ax.grid(axis="x",alpha=.18);ax.set_axisbelow(True)
ax=axes[1];y=np.arange(4)
series=[("pxa_timer","PXA",-.25),("pxa_release","PXA release",0),("aster","Aster",.25)]
for key,name,delta in series:
    if key not in data["display"]:continue
    values=[r["median_fps"] for r in data["display"][key]]
    bars=ax.barh(y+delta,values,height=.23,label=name,color=colors[name])
    ax.bar_label(bars,fmt="%.1f",padding=3,fontsize=9)
ax.set(yticks=y,yticklabels=["Constant fill","Changing-color fill","Pixel effect","128 rectangles"],
       xlabel="Completed LCD transfers / second (higher is better)",title="296 x 240 RGB565, 80 MHz SPI")
ax.invert_yaxis();ax.set_xlim(0,50);ax.legend(loc="upper right");ax.grid(axis="x",alpha=.18);ax.set_axisbelow(True)
for ax in axes:
    ax.spines[["top","right"]].set_visible(False)
fig.suptitle("Pai Touch: PXA vs Aster IDF port",fontsize=17,fontweight="bold")
fig.savefig(args.output,dpi=170)
print(args.output)
