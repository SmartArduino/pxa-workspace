import json,re,statistics,sys
from pathlib import Path
p=Path(sys.argv[1]);game=sys.argv[2]
r=json.loads((p/'measurement.json').read_text());records=[json.loads(s) for s in (p/'serial.jsonl').read_text().splitlines()]
commands=[x for x in records if x.get('kind')=='COMMAND' and x.get('payload')=='PERF FPS'][:60]
begin=r.get('window_begin_monotonic',commands[0]['time']-r['fps_samples'][0]['seconds']);end=r.get('window_end_monotonic',commands[-1]['time'])
device_begin=int(r.get('window_device_begin_us',r['newgame']['device_us']))//1000
window=[];seen=set()
for row in records:
 text=row.get('payload','');stamp=text.split('\t')[0]
 if row['kind']!='LOG' or not stamp.isdigit() or int(stamp)<device_begin or not begin<=row['time']<=end or (stamp,text) in seen:continue
 seen.add((stamp,text));window.append(row)
level=[x for x in window if ('PXA QUAKE ready=1' in x['payload'] if game=='quake' else 'PXA DUKE INPUT ' in x['payload'] and re.search(r'mode=4(?: |\x1b|$)',x['payload']))]
if len(level)<8 or level[-1]['time']-level[0]['time']<45:raise RuntimeError('Insufficient distinct level records')
def val(row,k):return int(re.search(r'\b'+k+r'=(\d+)',row['payload'])[1])
frames=[x for x in window if 'PXA GAME frames=' in x['payload']]
output={'fps':r['fps'],'samples':len(r['fps_samples']),'window_seconds':end-begin,'unique_level_records':len(level),'level_record_span_seconds':(int(level[-1]['payload'].split('\t')[0])-int(level[0]['payload'].split('\t')[0]))/1000,'ready_at_end':r['ready_recent_at_end'],'exact_pcm_negotiation':r['exact_source_pcm_observed'],'median_output_us_per_frame':statistics.median(val(x,'draw_us')/val(x,'frames') for x in frames),'controls_checks':r['controls_checks'],'cleanup_errors':r.get('cleanup_errors',[]),'error':r.get('error')}
if game=='quake':output['median_renderer_us']={k:statistics.median(val(x,k) for x in level) for k in ['view','surf','world','scan','snd']}
if all(re.search(r'\bhb=\d+',x['payload']) for x in level):
 hb=[val(x,'hb') for x in level];hr=[val(x,'hr') for x in level];hx=[val(x,'hx') for x in level]
 if game=='quake':
  output['host']={'counter_semantics':'five-second window deltas; hx is per engine frame','batches_in_records':sum(hb),'rejected_in_records':sum(hr),'every_frame_native':all(val(x,'hb')==val(x,'n') for x in level),'median_execution_us_per_engine_frame':statistics.median(hx),'median_validation_us_per_engine_frame':statistics.median(val(x,'hv') for x in level)}
 else:
  output['host']={'counter_semantics':'cumulative','batches_begin':hb[0],'batches_end':hb[-1],'max_rejected':max(hr),'execution_us_per_batch':(hx[-1]-hx[0])/(hb[-1]-hb[0]) if hb[-1]>hb[0] else None}
(p/'audit.json').write_text(json.dumps(output,indent=2)+'\n');print(json.dumps(output,indent=2))
