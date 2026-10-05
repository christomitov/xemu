#!/usr/bin/env python3
"""Map actual main-module function-table addresses, report cumulative deltas."""
import collections, hashlib, json, pathlib, sys
wasm = pathlib.Path('/root/src/xemu-wasm/xemu-astra/build/qemu-system-i386.wasm')
b = wasm.read_bytes(); p = 8; names = {}; table = {}
def u():
 global p
 v = s = 0
 while True:
  c=b[p];p+=1;v|=(c&127)<<s;s+=7
  if c<128:return v
def st():
 global p
 n=u();v=b[p:p+n].decode();p+=n;return v
def offset():
 global p
 assert b[p]==0x41, ('nonconstant element offset',b[p]);p+=1
 v=u();assert b[p]==0x0b;p+=1;return v
while p<len(b):
 kind=b[p];p+=1;n=u();end=p+n
 if kind==0 and st()=='name':
  while p<end:
   sub=b[p];p+=1;n=u();nxt=p+n
   if sub==1:
    for _ in range(u()):i=u();names[i]=st()
   p=nxt
 elif kind==9:
  for _ in range(u()):
   flags=u();assert flags in (0,2),('element flags',flags)
   tab=u() if flags==2 else 0
   off=offset()
   if flags==2:assert u()==0
   for j in range(u()):table[tab,off+j]=u()
 p=end
D=json.load(open(sys.argv[1])); rows=D['rows'];lo=next(x for x in rows if x['t']>=20);hi=rows[-1];dt=hi['t']-lo['t']
def parse(s):
 detail=collections.defaultdict(lambda:[0,0]);wait=collections.defaultdict(lambda:[0,0]);other={}
 for l in s.splitlines():
  a=l.split('|')
  if a[0]=='detail':
   for i in range(2):detail[a[1]][i]+=int(a[i+2])
  elif a[0]=='wait':
   for i in range(2):wait[a[1],a[2]][i]+=int(a[i+3])
  elif len(a)==2:other[a[0]]=int(a[1])
 return detail,wait,other
a,wa,_=parse(lo['locks']);z,wz,overflow=parse(hi['locks']);h=collections.defaultdict(lambda:[0,0])
for key,v in wz.items():
 old=wa.get(key,[0,0]);h[key[1]][0]+=v[0]-old[0];h[key[1]][1]+=v[1]-old[1]
wakes=(z['ml:setup:0'][0]-a['ml:setup:0'][0])/dt
output=[]
for key,v in z.items():
 old=a.get(key,[0,0]);calls=(v[0]-old[0])/dt;ms=(v[1]-old[1])/dt/1e6
 cb=None
 if key.startswith(('ml:timer:','ml:bh:','ml:fd-')):
  idx=table.get((0,int(key.rsplit(':',1)[1])));cb=names.get(idx,f'UNRESOLVED function {idx}')
 output.append(dict(key=key,callback=cb,calls_per_s=calls,calls_per_wake=calls/wakes,held_ms_per_s=ms,observed_holder_wait_ms_per_s=h[key][1]/dt/1e6))
output.sort(key=lambda r:-r['held_ms_per_s'])
print('build',D['res']['commit'],'window',lo['t'],hi['t'],'overflows',overflow,'wakes/s',wakes)
for r in output:print(f"{r['key']:25} {r['calls_per_s']:9.3f}/s {r['calls_per_wake']:6.3f}/wake {r['held_ms_per_s']:9.4f} held-ms/s {r['observed_holder_wait_ms_per_s']:9.4f} observed-wait-ms/s {r['callback'] or ''}")
print('OTHER WAIT HOLDERS')
for k,v in sorted(h.items(),key=lambda x:-x[1][1]):
 if k not in z:print(k,v[0]/dt,v[1]/dt/1e6)
print('HELD TOTAL',sum(x['held_ms_per_s'] for x in output),'WAIT TOTAL',sum(x[1] for x in h.values())/dt/1e6)
r=[x for x in rows if x['t']>20];clocks={k:sum(x[k] for x in r)/len(r) for k in ['n_pit_tick','n_vblank','n_apu_frame']};print('CLOCKS',clocks)
report=dict(build=D['res']['commit'],wasm_sha256=hashlib.sha256(b).hexdigest(),window=[lo['t'],hi['t']],overflows=overflow,wakes_per_s=wakes,clocks=clocks,scopes=output)
pathlib.Path(sys.argv[1].replace('.json','-analysis.json')).write_text(json.dumps(report,indent=2))
