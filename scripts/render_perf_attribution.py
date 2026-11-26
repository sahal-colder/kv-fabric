#!/usr/bin/env python3
"""Export README figures from archived counters and actual profiler intervals."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import sqlite3

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch
from summarize_nsys import merge


def save(fig, path):
    fig.savefig(path.with_suffix(".svg"), bbox_inches="tight")
    fig.savefig(path.with_suffix(".png"), dpi=180, bbox_inches="tight")
    plt.close(fig)


def architecture(out):
    fig, ax = plt.subplots(figsize=(13, 6.6))
    ax.set(xlim=(0, 13), ylim=(-.7, 6.7)); ax.axis("off")

    def box(x,y,w,h,text,color):
        ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle="round,pad=0.12",fc=color,ec="#26415b",lw=1.2))
        ax.text(x+w/2,y+h/2,text,ha="center",va="center",fontsize=11,color="#173047",linespacing=1.5)

    def arrow(a,b,text="",yoff=.15,color="#536d82"):
        ax.add_patch(FancyArrowPatch(a,b,arrowstyle="<->",mutation_scale=13,color=color,lw=1.5))
        ax.text((a[0]+b[0])/2,(a[1]+b[1])/2+yoff,text,ha="center",va="bottom",fontsize=9,color=color)

    box(.3,4.8,3,1.1,"vLLM scheduler\nPrefix lookup / admission", "#e8eff8")
    box(5,4.8,3,1.1,"Directory (TCP control)\nIdentity / replica / lease", "#e8eff8")
    box(.3,2.7,3,1.1,"GPU paged KV\nuint4 gather / scatter", "#dbece3")
    box(4.6,2.7,3.7,1.1,"Bounded pinned staging\n72 MiB × 4 load windows*", "#e3edf7")
    box(9.4,2.7,3.1,1.1,"Remote Worker\nTCP / rsockets stream", "#eee7f4")
    box(4.6,.4,3.7,1.3,"Physical ext4 NVMe file\n16 KiB KV + 4 KiB header\nCRC / version / generation", "#f5e7d4")
    box(.3,.4,3,1.3,"CPU resident cache\nAlternate source tier\nOwned snapshot reads", "#dbece3")
    box(9.4,.4,3.1,1.3,"Validation boundary\nSame-host RXE tested\nNo HCA / GPUDirect claim", "#f1f2f4")
    arrow((3.3,5.35),(5,5.35),"lookup / lease")
    arrow((3.3,3.25),(4.6,3.25),"H2D / D2H")
    arrow((8.3,3.25),(9.4,3.25),"cross-worker")
    arrow((6.45,2.7),(6.45,1.7),color="#b17a32")
    ax.text(6.65,2.2,"owned SSD read\n≤ io_depth lanes",ha="left",va="center",fontsize=9,color="#b17a32")
    arrow((2.4,1.7),(4.6,2.7),"owned CPU read",yoff=.04,color="#378766")
    arrow((7.7,4.8),(10.95,3.8),"register / validate",yoff=.06)
    ax.text(.2,6.35,"KV-Fabric: measured data paths and evidence boundary",fontsize=17,weight="bold",color="#173047")
    ax.text(.2,-.5,"* 16K input, 8K hit: 512 pages × 36 layers × 16 KiB = 288 MiB.\n"
                        "Four sequential windows reuse bounded staging; they are not four persistent replicas.",fontsize=10,color="#536d82")
    save(fig,out/"architecture")


def timeline(archive,out):
    fig, axes = plt.subplots(2,1,figsize=(13,7.8),sharex=True)
    colors={"read":"#b17a32","h2d":"#287bb5","compute":"#378766","d2h":"#8665a6"}
    for ax,name,label in zip(axes,("nsys-before-diagnostic","nsys-final-disk"),
                             ("Before: sequential SSD snapshots + loopback import", "After: owned SSD snapshots, up to 4 read lanes")):
        p=archive/name
        db=sqlite3.connect(p/"trace.sqlite")
        groups=defaultdict(list)
        for a,b,t,n in db.execute("SELECT n.start,n.end,n.globalTid,coalesce(n.text,s.value) FROM NVTX_EVENTS n LEFT JOIN StringIds s ON n.textId=s.id WHERE n.end>n.start"):
            groups[n].append((a,b,t))
        req=json.loads((p/"requests.json").read_text())["requests"][1]
        start=groups["phase2.measured.1"][0][0]; end=start+req["ttft_ms"]*1e6
        pub=groups["kvfabric.publish"]
        fetch=merge((a,b) for k in ("kvfabric.fetch","kvfabric.fetch.owned") for a,b,t in groups[k] if start<=a<end)
        reads=defaultdict(list)
        for a,b,t in groups["kvfabric.disk_io.read"]:
            if any(x<=a and b<=y for x,y in fetch) and not any(x<=a and b<=y and tid==t for x,y,tid in pub):
                reads[t].append((a,b))
        for lo,hi in fetch:
            local={tid:[(a,b) for a,b in spans if lo<=a and b<=hi] for tid,spans in reads.items()}
            local={tid:spans for tid,spans in local.items() if spans}
            for lane,(tid,spans) in enumerate(sorted(local.items(),key=lambda v:min(x for x,y in v[1]))):
                # New threads are created per 128-page window. Lanes are ordered
                # within each window, not presented as persistent thread IDs.
                y=5-lane
                ax.broken_barh([((a-start)/1e9,(b-a)/1e9) for a,b in spans],(y-.3,.6),facecolors=colors["read"])
        for a,b,n,k in db.execute("SELECT start,end,bytes,copyKind FROM CUPTI_ACTIVITY_KIND_MEMCPY"):
            if start<=a<end and n>=1<<20 and k in (1,2):
                ax.broken_barh([((a-start)/1e9,(b-a)/1e9)],(.9 if k==1 else -.9,.5),facecolors=colors["h2d" if k==1 else "d2h"])
        spans=[]
        for a,b,n in db.execute("SELECT k.start,k.end,s.value FROM CUPTI_ACTIVITY_KIND_KERNEL k LEFT JOIN StringIds s ON k.demangledName=s.id"):
            if start<=a<end and not any(x in (n or "") for x in ("gather_kv_pages","scatter_kv_pages")):
                spans.append((a,min(b,end)))
        ax.broken_barh([((a-start)/1e9,(b-a)/1e9) for a,b in merge(spans)],(-.05,.55),facecolors=colors["compute"])
        ax.set_yticks([5,4,3,2,1.15,.2,-.65], ["SSD lane 1","SSD lane 2","SSD lane 3","SSD lane 4","H2D payload","Model GPU","D2H save"])
        ax.set_title(f"{label} | request TTFT {req['ttft_ms']/1000:.3f} s",loc="left",fontsize=12)
        ax.set_ylim(-1.2,5.8); ax.grid(axis="x",alpha=.2); ax.spines[["top","right"]].set_visible(False)
        db.close()
    axes[-1].set_xlabel("Seconds since identical measured request began")
    fig.suptitle("Actual Nsight Systems intervals: 16K input / 8K KV hit / BF16",fontsize=15,weight="bold")
    fig.text(.01,.01,"I/O excludes background publication; each request reads 18,432 records and transfers exactly 288 MiB H2D.\n"
                        "CRC overlaps across SSD lanes; H2D and prefill remain dependent. This is a trace plot, not a GUI screenshot.",fontsize=9,color="#536d82")
    fig.tight_layout(rect=(0,.06,1,.96));save(fig,out/"nsys-read-concurrency")


def ncu(archive,out):
    if (archive/"ncu-audit.json").exists():
        data=json.loads((archive/"ncu-audit.json").read_text())[:2]
        source="native-linux-final/ncu-root/*.csv (one launch per kernel, 37 replay passes)"
    else:
        raw=json.loads((archive/"ncu-summary.json").read_text())
        data=[dict(kernel=r['kernel'], payload_one_way_GiB_s=r['effective_payload_GiB_s'],
                   dram_total_GiB_s=r['bandwidth_GiB_s'],dram_util_percent=r['dram_percent']['value'],
                   dram_clock_Hz=r['dram_clock']['value']*{'Ghz':1e9,'Mhz':1e6,'Hz':1}[r['dram_clock']['unit']],
                   read_bytes=r['dram_traffic']['read']['bytes'],write_bytes=r['dram_traffic']['write']['bytes']) for r in raw]
        source="ncu-summary.json; raw paths and actual replay counts are preserved in that file"
    fig,ax=plt.subplots(figsize=(12,4.4)); ax.axis("off")
    rows=[]
    for r in data:
        rows.append([r["kernel"],f"{r['payload_one_way_GiB_s']:.2f}",f"{r['dram_total_GiB_s']:.2f}",
                     f"{r['dram_util_percent']:.2f}%",f"{r['dram_clock_Hz']/1e9:.3f}",
                     f"{r['read_bytes']/2**20:.3f} / {r['write_bytes']/2**20:.3f}","100%"])
    table=ax.table(cellText=rows,colLabels=["Kernel","Payload GiB/s\nP / time","DRAM GiB/s\n(R + W) / time",
                                          "DRAM util.","Clock GHz","DRAM read / write\nMiB","Store sector\nutilization"],
                   cellLoc="center",loc="center",colWidths=[.12,.14,.16,.13,.12,.19,.14])
    table.auto_set_font_size(False);table.set_fontsize(10);table.scale(1,2.6)
    for (row,col),cell in table.get_celld().items():
        cell.set_edgecolor("white");cell.set_facecolor("#173047" if row==0 else "#e8eff5")
        if row==0:cell.set_text_props(color="white",weight="bold")
    ax.set_title("ncu bandwidth accounting: same 72 MiB payload, different memory clocks",fontsize=15,weight="bold",pad=12)
    fig.text(.06,.04,f"Source: {source}.\n"
                        "Both store 2,359,296 × 32 B = 72 MiB; no store-sector amplification. Raw reports are retained.\n"
                        "Historical counters re-rendered for comparison; not a fabricated Nsight Compute screenshot.",fontsize=10,color="#536d82")
    save(fig,out/"ncu-bandwidth-accounting")


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("archive",type=Path);a=p.parse_args()
    out=a.archive/"figures";out.mkdir(exist_ok=True)
    architecture(out);timeline(a.archive,out);ncu(a.archive,out)


if __name__=="__main__":main()
