#!/usr/bin/env python3
"""Static README figures, derived only from exported measurements/NSys spans."""
import argparse
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('evidence',type=Path);a=p.parse_args()
    s=json.loads((a.evidence/'summary.json').read_text());out=a.evidence/'figures';out.mkdir(exist_ok=True)
    plt.rcParams.update({'font.size':10,'axes.spines.top':False,'axes.spines.right':False,'savefig.bbox':'tight'})
    def save(fig,name):
        for ext in ('png','svg'):fig.savefig(out/(name+'.'+ext),dpi=180)
        plt.close(fig)
    fig,ax=plt.subplots(figsize=(11,4.5));ax.set(xlim=(0,11),ylim=(0,4.5));ax.axis('off')
    def box(x,y,w,h,label,color):
        ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle='round,pad=0.08',fc=color,ec='#455469'))
        ax.text(x+w/2,y+h/2,label,ha='center',va='center',fontsize=10)
    box(.3,.9,3.2,2.8,'ns1   10.10.0.1\nLive source instance\n8K prefix / 288 MiB real BF16 KV\nCPU immutable snapshots\nVersion + CRC + lease checks','#deedfc')
    box(4.5,3.6,2.5,.55,'Directory / lease control','#e5e7eb')
    box(7.6,.9,3.1,2.8,'ns2   10.10.0.2\nCold destination Worker\nPinned ring: 2 x 72 MiB\nH2D + CUDA Scatter + Event\nCompute only 8K suffix','#dbf5e6')
    ax.annotate('',xy=(7.45,2.2),xytext=(3.65,2.2),arrowprops=dict(arrowstyle='->',lw=2,color='#2478aa'))
    ax.text(5.55,2.5,'veth1 <-> veth2 / MTU 9000\nStrict rsockets over patched SIW\n4.5 MiB frames, no TCP fallback',ha='center',fontsize=9)
    for start,end in [((3.5,3.4),(4.4,3.85)),((7.6,3.4),(7.1,3.85))]:
        ax.annotate('',xy=end,xytext=start,arrowprops=dict(arrowstyle='<->',color='#667085'))
    ax.text(5.5,.4,'One Ubuntu host, two exclusive network namespaces. GPU source prefill and destination inference run sequentially.\nSoftware iWARP testbed; no physical RoCE HCA, GPUDirect, or extrapolated 200G/400G results.',ha='center',fontsize=9,color='#455469')
    ax.set_title('Cross-instance KV reuse: transfer an 8K prefix instead of recomputing it',loc='left',fontweight='bold')
    save(fig,'architecture')
    fig,axes=plt.subplots(2,1,figsize=(11,5.2),sharex=True)
    for ax,name,title in zip(axes,('baseline_tuned_serial','pipeline_tuned'),('Serial bridge (same tuned buffers)','Direct pinned receive + double-buffer H2D')):
        r=s['profiles'][name][2];timeline=r['timeline']
        for j,(key,color)in enumerate((('receive','#267fb5'),('h2d','#ed8936'),('scatter','#38a169'))):
            spans=[(lo/1e6,(hi-lo)/1e6)for lo,hi in timeline[key]]
            ax.broken_barh(spans,(j*.8,.5),facecolors=color)
        ax.set_yticks([.25,1.05,1.85],['Receive payload','H2D DMA','Scatter kernel'])
        ax.set_ylim(-.2,2.5);ax.set_xlim(0,470);ax.grid(axis='x',alpha=.15)
        ax.set_title(title+f" | import {r['import_wall_ms']:.1f} ms | DMA+Scatter hidden by receive {r['gpu_hidden_percent']:.1f}%",loc='left',fontsize=10)
    axes[-1].set_xlabel('Time from profiled request start (ms); actual GPU activity, not API-submit duration')
    fig.suptitle('Nsight Systems interval export: all samples import exactly 288 MiB',x=.13,ha='left',fontweight='bold')
    fig.tight_layout();save(fig,'nsys-rdma-h2d')
    g=s['groups'];labels=['Cold recompute','RDMA baseline','RDMA optimized','CPU hot KV']
    values=[g[k]['ttft_p50_ms']/1000 for k in ('cold','baseline','pipeline_tuned','cpu_hot')]
    fig,ax=plt.subplots(figsize=(9,4));bars=ax.barh(labels,values,color=['#98a2b3','#79a8cd','#267fb5','#38a169'])
    ax.invert_yaxis();ax.bar_label(bars,labels=[f'{x:.3f} s'for x in values],padding=5);ax.set_xlim(0,6.3)
    ax.set_xlabel('Unprofiled TTFT P50 (s), 15 identical requests per mode')
    ax.set_title('Qwen2.5-3B BF16 | 16K input / 8K shared / 16 output\nOptimized RDMA vs cold: -36.9%; all output IDs match',loc='left',fontweight='bold')
    ax.text(0,-.23,'Baseline retains 1/15 safe recompute fallback (P95 5.593 s); optimized has 0/15 failures.\nSingle host / two netns / patched SIW software iWARP; RTX 4060 Laptop 8 GiB.',transform=ax.transAxes,fontsize=9,color='#455469')
    save(fig,'ttft')
if __name__=='__main__':main()
