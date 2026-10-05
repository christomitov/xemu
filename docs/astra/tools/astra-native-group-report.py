#!/usr/bin/env python3
import collections,json,pathlib
D=pathlib.Path('/tmp/astra-native-perf-2');r=json.loads((D/'native-self-analysis.json').read_text());n=r['total'];counts=collections.Counter();ca=collections.defaultdict(collections.Counter);clock=collections.Counter();mem=0;gc=0
for row in r['rows']:
 name=row['name']
 if any(x in name for x in ['memcpy','memmove','memset','memory_copy_wrapper','memory_fill_wrapper']):mem+=row['samples']
 for entry in row['callers']:
  frames=[x[2] for x in entry['frames']];text=' > '.join([name,*frames]);v=entry['samples']
  if any(x in text for x in ['Scavenger::','Heap::CollectGarbage','MarkCompactCollector','MinorMarkSweep']):gc+=v
  if any(x in text for x in ['FastPerformanceNow','clock_gettime','uv__hrtime']):
   group='host clock reads'
   if 'helper_rdtsc' in text:clock['helper_rdtsc']+=v
   elif 'xemu_wasm_stats_now_ns' in text:
    if 'bql_lock_impl' in text:clock['BQL wait timing']+=v
    elif any(x in text for x in ['memory_region','mmu_','io_writex','io_readx']):clock['MMIO timing']+=v
    else:clock['other stats timing']+=v
   else:clock['other clock / truncated caller']+=v
  elif any(x in text for x in ['__emscripten_throw_longjmp','emscripten_longjmp','Runtime_Throw','UnwindAndFindExceptionHandler']):group='exception / longjmp runtime'
  elif any(x in text for x in ['wasm32_instantiate','WebAssemblyModule','WebAssemblyInstance','WasmCompileLazy','CompileToNativeModule','ExecuteCompilation','TriggerTierUp']):group='Wasm compile / instantiate / tiering'
  elif any(x in text for x in ['WebAssemblyTable','WasmTableObject']):group='Wasm table get/set'
  elif any(x in text for x in ['FindOrderedHashMapEntry','MapPrototypeGet','MapPrototypeHas']):group='JS Map lookup'
  elif any(x in text for x in ['emscripten_futex','AtomicWait','AtomicNotify','__pthread','pthread_cond','futex','lll_lock']):group='futex / mutex / notify'
  elif any(x in name for x in ['memcpy','memmove','memset','memory_copy_wrapper','memory_fill_wrapper']):group='other bulk memory'
  else:group='other native'
  counts[group]+=v;ca[group][name]+=v
print('total',n)
for k,v in counts.most_common():
 print(k,v,round(v/n*100,4));print(' ',ca[k].most_common(7))
print('CLOCK CALLERS',[(k,v,round(v/n*100,4)) for k,v in clock.most_common()]);print('all bulk memory self',mem,100*mem/n,'GC-containing native stacks',gc,100*gc/n)
monitor=[json.loads(l) for l in pathlib.Path('/tmp/astra-lock-native-perf-2-rss.jsonl').read_text().splitlines()];t=[x for x in monitor if 22<=x['t']<=58 and 'vcpu_stime' in x];a,b=t[0],t[-1];u=b['vcpu_utime']-a['vcpu_utime'];s=b['vcpu_stime']-a['vcpu_stime'];print('CPU user/system ticks',u,s,'system share',s/(s+u),'monitor window',a['t'],b['t'])
(D/'native-groups.json').write_text(json.dumps(dict(samples=n,groups={k:dict(samples=v,pct=100*v/n,top_self=ca[k].most_common(10)) for k,v in counts.items()},clock_callers={k:dict(samples=v,pct=100*v/n) for k,v in clock.items()},bulk_memory_self=dict(samples=mem,pct=100*mem/n),gc_native_stack=dict(samples=gc,pct=100*gc/n),proc_cpu_system_share=s/(s+u)),indent=2))
