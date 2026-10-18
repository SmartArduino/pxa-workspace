import argparse,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('wasm',type=Path);p.add_argument('--output',type=Path);a=p.parse_args();data=a.wasm.read_bytes()
def uleb(b,pos):
 n=shift=0
 while True:
  v=b[pos];pos+=1;n|=(v&127)<<shift;shift+=7
  if v<128:return n,pos

def string(b,pos):
 n,pos=uleb(b,pos);return b[pos:pos+n].decode('utf8'),pos+n
sections=[];names={};bodies=[];imports=0;pos=8
while pos<len(data):
 ident=data[pos];size,start=uleb(data,pos+1);body=data[start:start+size];pos=start+size
 sections.append({'id':ident,'bytes':size})
 if ident==2:
  count,q=uleb(body,0)
  for _ in range(count):
   _,q=string(body,q);_,q=string(body,q);kind=body[q];q+=1
   if kind!=0:raise ValueError('expected function-only imports')
   _,q=uleb(body,q);imports+=1
 if ident==10:
  count,q=uleb(body,0)
  for i in range(count):
   n,q=uleb(body,q);bodies.append({'function':imports+i,'bytes':n});q+=n
 if ident==0:
  name,q=string(body,0)
  if name=='name':
   while q<len(body):
    sub=body[q];n,start=uleb(body,q+1);end=start+n;q=end
    if sub==1:
     count,r=uleb(body,start)
     for _ in range(count):
      idx,r=uleb(body,r);names[idx],r=string(body,r)
for item in bodies:item['name']=names.get(item['function'],'?')
bodies.sort(key=lambda item:item['bytes'],reverse=True)
try:
 raw='\n'.join(item['name'] for item in bodies).encode()
 demangled=subprocess.run(['c++filt'],input=raw,stdout=subprocess.PIPE,check=True).stdout.decode().splitlines()
 for item,name in zip(bodies,demangled):item['name']=name
except FileNotFoundError:pass
report={'wasm':str(a.wasm),'total':len(data),'sections':sections,'functions':len(bodies),'largest':bodies[:35]}
if a.output:a.output.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
