#!/usr/bin/env python3
"""Self-IP + bounded frame-pointer analysis of this capture's perf sample ABI.
No inferred native symbol ranges: require the PC to fall inside nm's size.
DWARF-independent fallback; missing frame chains stay explicitly missing.
"""
import bisect, collections, ctypes, json, mmap, pathlib, re, struct, subprocess, sys
D=pathlib.Path(sys.argv[1] if len(sys.argv)>1 else '/tmp/astra-native-perf-1')
PID=json.loads((D/'target.json').read_text())['pid']
ksyms=[]
for line in pathlib.Path('/proc/kallsyms').read_text().splitlines():
 fields=line.split()
 if len(fields)>=3 and fields[1] in 'tTwW' and int(fields[0],16):
  ksyms.append((int(fields[0],16),fields[2]))
ksyms.sort();kaddrs=[k[0] for k in ksyms]
# Same-kernel vDSO, symbols are relative to its mapping, not this process's ASLR.
for line in pathlib.Path('/proc/self/maps').read_text().splitlines():
 if '[vdso]' in line:
  a,z=(int(x,16) for x in line.split()[0].split('-'))
  (D/'vdso.elf').write_bytes(ctypes.string_at(a,z-a));break
maps=[]
for line in pathlib.Path('/tmp/astra-lock-'+D.name.removeprefix('astra-')+'-smaps-30.txt').read_text().splitlines():
 m=re.match(r'([0-9a-f]+)-([0-9a-f]+) ([-rwxps]+) ([0-9a-f]+) \S+ \d+\s*(.*)',line)
 if m and 'x' in m[3]:maps.append((int(m[1],16),int(m[2],16),int(m[4],16),m[5]))
ms=[x[0] for x in maps];symbols={}
def syms(path):
 if path in symbols:return symbols[path]
 original=path
 if path=='[vdso]':path=str(D/'vdso.elf')
 if not pathlib.Path(path).is_file():symbols[original]=([],[]);return symbols[original]
 notes=subprocess.check_output(['readelf','-n',path],text=True,stderr=subprocess.DEVNULL)
 bid=re.search(r'Build ID: ([0-9a-f]+)',notes)
 if bid:
  h=bid[1];debug=pathlib.Path('/tmp/astra-native-symbols/libc-debug/usr/lib/debug/.build-id')/h[:2]/(h[2:]+'.debug')
  if debug.exists():path=str(debug)
 rows=[]
 proc=subprocess.Popen(['nm','-n','-S','-C',*(['-D'] if original=='[vdso]' else []),path],stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True)
 for l in proc.stdout:
  m=re.match(r'([0-9a-f]+) ([0-9a-f]+) [tTwW] (.*)',l.rstrip())
  if m and int(m[2],16):rows.append((int(m[1],16),int(m[2],16),m[3]))
 proc.wait();rows.sort();symbols[original]=([x[0] for x in rows],rows);return symbols[original]
# jitdump names are timestamped; reused/moved address ranges must not be static.
jit_pages=collections.defaultdict(list);by_index={};jit_counts=collections.Counter()
with (D/f'jit-{PID}.dump').open('rb') as f:
 j=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ);q=struct.unpack_from('<I',j,8)[0]
 while q<len(j):
  kind,size,ts=struct.unpack_from('<IIQ',j,q);jit_counts[kind]+=1
  if not size:raise ValueError(q)
  row=None
  if kind==0:
   pid,tid,vma,addr,n,idx=struct.unpack_from('<IIQQQQ',j,q+16)
   end=j.find(b'\0',q+56,q+size);name=j[q+56:end].decode(errors='replace')
   row=[addr,n,name,ts,2**64-1];by_index[idx]=row
  elif kind==1:
   pid,tid,vma,old,addr,n,idx=struct.unpack_from('<IIQQQQQ',j,q+16)
   before=by_index.get(idx)
   if before:
    before[4]=ts;row=[addr,n,before[2],ts,2**64-1];by_index[idx]=row
  if row:
   for page in range(row[0]>>12,((row[0]+row[1]-1)>>12)+1):jit_pages[page].append(row)
  q+=size
 j.close()
for rows in jit_pages.values():rows.sort(key=lambda r:r[3])
def symbol(ip,ts):
 if ip>=0xffff800000000000:
  k=bisect.bisect_right(kaddrs,ip)-1
  return ('kernel','[kernel]',ksyms[k][1] if k>=0 else 'UNRESOLVED',ip-kaddrs[k] if k>=0 else 0)
 i=bisect.bisect_right(ms,ip)-1
 if i>=0 and ip<maps[i][1]:
  a,z,off,path=maps[i]
  if path and not path.startswith('[anon'):
   key=ip-a+off+(0x400000 if path=='/usr/bin/node' else 0)
   starts,rows=syms(path);k=bisect.bisect_right(starts,key)-1
   # Check aliases with same start; never stretch a symbol over a gap.
   while k>=0:
    start,n,name=rows[k]
    if key<start+n:return ('native',path,name,key-start)
    if k==0 or rows[k-1][0]!=start:break
    k-=1
   return ('native',path,'UNRESOLVED@'+hex(key),0)
 for a,n,name,t,end in reversed(jit_pages.get(ip>>12,[])):
  if t<=ts<end and a<=ip<a+n:return ('jit','jit',name,ip-a)
 return ('unknown','unknown',hex(ip),0)
counts=collections.Counter();caller=collections.defaultdict(collections.Counter);total=0;lost=0;types=collections.Counter();tids=collections.Counter();frame_coverage=collections.Counter();first=None
regs_indices=[i for i in range(64) if (0xff0fff>>i)&1]
with (D/'perf.data').open('rb') as f:
 m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ);off,n=struct.unpack_from('<QQ',m,40);q=off
 while q<off+n:
  kind,misc,size=struct.unpack_from('<IHH',m,q)
  if kind==2:lost+=struct.unpack_from('<Q',m,q+16)[0]
  if kind==9:
   ip,pid,tid,ts,addr,nr=struct.unpack_from('<QIIQQQ',m,q+8)
   if first is None:first=ts
   # Drop first two seconds of attachment: safely after benchmark warmup.
   if ts>=first+2000000000:
    total+=1;tids[tid]+=1;s=symbol(ip,ts);key=s[:3];counts[key]+=1;types[s[0]]+=1
    if s[0] in ('native','kernel'):
     p=q+8+40+nr*8;abi=struct.unpack_from('<Q',m,p)[0];p+=8
     assert abi==2
     vals=struct.unpack_from('<'+'Q'*len(regs_indices),m,p);p+=len(vals)*8;regs=dict(zip(regs_indices,vals))
     assert s[0]=='kernel' or regs[8]==ip
     nstack=struct.unpack_from('<Q',m,p)[0];p+=8;dyn=struct.unpack_from('<Q',m,p+nstack)[0]
     sp,bp=regs[7],regs[6];frames=[symbol(regs[8],ts)[:3]] if s[0]=='kernel' else []
     for _ in range(48):
      if not(sp<=bp and bp+16<=sp+dyn) or bp%8:break
      prev,ret=struct.unpack_from('<QQ',m,p+bp-sp)
      if ret:frames.append(symbol(ret-1,ts)[:3])
      if prev<=bp:break
      bp=prev
     caller[key][tuple(frames)]+=1
     frame_coverage['native_with_jit_frame' if any(x[0]=='jit' for x in frames) else 'native_no_jit_frame']+=1
  q+=size
 m.close()
print('total',total,'lost',lost,'TIDs',dict(tids),'types',dict(types),'frame coverage',dict(frame_coverage),'jit records',dict(jit_counts))
rows=[]
for key,n in counts.most_common():
 if key[0] not in ('native','kernel'):continue
 r=dict(type=key[0],dso=key[1],name=key[2],samples=n,pct=100*n/total,callers=[dict(samples=v,frames=k) for k,v in caller[key].most_common()]);rows.append(r)
 if len(rows)<=35:
  print(n,round(r['pct'],3),key[1],key[2][:180])
  for c in r['callers'][:2]:print(' ',c['samples'],' > '.join(x[2] for x in c['frames'][:9]))
jit_rows=[dict(name=k[2],samples=n,pct=100*n/total) for k,n in counts.most_common() if k[0]=='jit']
print('TOP JIT',jit_rows[:16])
(D/'native-self-analysis.json').write_text(json.dumps(dict(total=total,lost=lost,tids=dict(tids),types=dict(types),frame_coverage=dict(frame_coverage),rows=rows,jit_rows=jit_rows),indent=2))
