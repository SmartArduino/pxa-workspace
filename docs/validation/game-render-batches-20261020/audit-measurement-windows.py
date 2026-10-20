from pathlib import Path
import json,re
r=Path('docs/validation/game-render-batches-20261020');result={}
for name in ['quake-same-fw-baseline','quake-frame','quake-frame-blit','quake-frame-bulk','quake-frame-io']:
 p=r/(name+'-serial.jsonl')
 if not p.exists():continue
 records=[json.loads(line)for line in p.read_text().splitlines()]
 commands=[row for row in records if row.get('kind')=='COMMAND'and row.get('payload')=='PERF FPS'][:60]
 assert len(commands)==60,(name,len(commands))
 start=commands[0]['time']-1.5;end=commands[-1]['time']+.1
 unique={};replayed=0
 stamped=[(row,int(row.get('payload','').split('\t',1)[0])) for row in records if re.match(r'^\d+\t',row.get('payload',''))]
 clock_offset=min(row['time']-stamp/1000 for row,stamp in stamped)
 for row in records:
  text=row.get('payload','');m=re.search(r'^(\d+)\t.*PXA QUAKE ready=(\d+) hp=(-?\d+) xyz=([\d,-]+) n=(\d+)',text)
  if m and start<=row['time']<=end:
   stamp=int(m[1])
   if row['time']-stamp/1000-clock_offset>2:
    replayed+=1;continue
   unique.setdefault(stamp,{'stamp_ms':stamp,'host_time':row['time'],'ready':int(m[2]),'hp':int(m[3]),'xyz':m[4],'frames':int(m[5])})
 rows=list(unique.values());stamps=sorted(unique)
 passed=len(rows)>=10 and stamps[-1]-stamps[0]>=45000 and all(v['ready']==1 and v['hp']==100 and v['xyz']=='544,288,50'for v in rows)
 result[name]={'passed':passed,'replayed_or_delayed_records_excluded':replayed,'freshness_max_delay_seconds':2,'clock_offset_seconds':clock_offset,'sample_count':len(commands),'first_to_last_sample_seconds':commands[-1]['time']-commands[0]['time'],'unique_actual_game_records_in_window':len(rows),'device_record_span_ms':stamps[-1]-stamps[0]if stamps else 0,'all_records_ready_healthy_same_spawn':all(v['ready']==1 and v['hp']==100 and v['xyz']=='544,288,50'for v in rows),'records':rows}
 assert passed,(name,result[name])
(r/'measurement-window-audit.json').write_text(json.dumps(result,indent=2)+'\n')
print({k:{'passed':v['passed'],'unique_ready_records':v['unique_actual_game_records_in_window']}for k,v in result.items()})
