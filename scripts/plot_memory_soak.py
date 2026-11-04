#!/usr/bin/env python3
"""Summarize archived lifetime samples and draw the memory time series."""
import argparse,csv,json,statistics
from pathlib import Path


def summarize(root):
 result={}
 for mode in ('native','system'):
  with (root/f'{mode}-samples.csv').open(newline='') as stream: rows=list(csv.DictReader(stream))
  phases=json.loads((root/f'{mode}-result.json').read_text())['phases']
  output=[]
  for phase in phases:
   name=phase['phase'];selected=[r for r in rows if r['phase']==name]
   regular=[r for r in selected if r['event']=='sample']
   first=float(selected[0]['elapsed_s']);last=max(float(r['elapsed_s']) for r in regular)
   early=[r for r in regular if float(r['elapsed_s'])<=first+60]
   late=[r for r in regular if float(r['elapsed_s'])>=last-60]
   median=lambda group,key: statistics.median(int(r[key])/1024 for r in group)
   points=[(float(r['elapsed_s'])/60,int(r['vms_kib'])/1024) for r in regular if float(r['elapsed_s'])>=first+120]
   slope=statistics.linear_regression([p[0] for p in points],[p[1] for p in points]).slope if len(points)>1 else None
   recovery=[float(r['recovery_ms']) for r in selected if r['event']=='slow_drained']
   phase=dict(phase,early_rss_mib=median(early,'rss_kib'),late_rss_mib=median(late,'rss_kib'),
     peak_sampled_rss_mib=max(int(r['rss_kib'])/1024 for r in selected),
     early_vms_mib=median(early,'vms_kib'),late_vms_mib=median(late,'vms_kib'),
     vms_mib_per_minute_after_two_minutes=slope,
     peak_pending_sample=max(int(r['pending']) for r in selected),
     recovery_median_ms=statistics.median(recovery) if recovery else None,
     recovery_max_ms=max(recovery) if recovery else None,
     phase_start_s=first,phase_end_s=float(selected[-1]['elapsed_s']))
   output.append(phase)
  result[mode]=output
 return result


def plot(root,output):
 import matplotlib
 matplotlib.use('Agg')
 import matplotlib.pyplot as plt
 from matplotlib.patches import Patch
 colors={'steady':'#dbeafe','slow_reader':'#fef3c7','thread_churn':'#ede9fe'}
 titles={'native':'Native (OccSTM uses TierAlloc)','system':'System allocator'}
 labels={'steady':'Fixed workers','slow_reader':'Slow reader','thread_churn':'Thread churn'}
 fig,axes=plt.subplots(3,2,figsize=(13,9),sharex=True,sharey='row',layout='constrained')
 for column,mode in enumerate(('native','system')):
  with (root/f'{mode}-samples.csv').open(newline='') as stream: rows=list(csv.DictReader(stream))
  times=[float(r['elapsed_s'])/60 for r in rows]
  for phase,color in colors.items():
   selected=[r for r in rows if r['phase']==phase]
   for ax in axes[:,column]:
    ax.axvspan(float(selected[0]['elapsed_s'])/60,float(selected[-1]['elapsed_s'])/60,color=color,alpha=.7,zorder=0)
  axes[0,column].plot(times,[int(r['rss_kib'])/1024 for r in rows],color='#1d4ed8',linewidth=1,label='RSS')
  axes[0,column].plot(times,[int(r['hwm_kib'])/1024 for r in rows],color='#64748b',linewidth=.8,linestyle='--',label='Process RSS high-water mark')
  axes[1,column].plot(times,[int(r['vms_kib'])/1024 for r in rows],color='#7c3aed',linewidth=1)
  axes[2,column].plot(times,[int(r['pending']) for r in rows],color='#b45309',linewidth=.8)
  axes[2,column].set_yscale('symlog',linthresh=100)
  axes[2,column].axhline(0,color='#475569',linewidth=.6)
  axes[0,column].set_title(titles[mode],fontsize=12)
  axes[0,column].legend(fontsize=8,loc='upper left')
  axes[2,column].set_xlabel('Process elapsed time (minutes)')
  for ax in axes[:,column]:ax.grid(True,alpha=.2);ax.set_xlim(left=0)
 axes[0,0].set_ylabel('Resident memory (MiB)')
 axes[1,0].set_ylabel('Virtual memory (MiB)')
 axes[2,0].set_ylabel('Retired - destroyed payloads\n(asynchronous sample, symlog)')
 duration=json.loads((root/'native-result.json').read_text())['phase_seconds']*3/60
 fig.suptitle(f'CaSTM memory and reclamation validation — {duration:g} active minutes per allocator',fontsize=14)
 fig.legend(handles=[Patch(facecolor=colors[p],label=labels[p]) for p in colors],loc='outside lower center',ncol=3,frameon=False)
 output.parent.mkdir(parents=True,exist_ok=True)
 fig.savefig(output.with_suffix('.png'),dpi=180)
 fig.savefig(output.with_suffix('.svg'))
 plt.close(fig)


if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('directory',type=Path);parser.add_argument('--summary',type=Path,required=True)
 parser.add_argument('--plot',type=Path)
 args=parser.parse_args();summary=summarize(args.directory)
 args.summary.write_text(json.dumps(summary,indent=2)+'\n')
 if args.plot:plot(args.directory,args.plot)
 print(json.dumps(summary,indent=2))
