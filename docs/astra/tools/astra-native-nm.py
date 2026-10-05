#!/usr/bin/env python3
import os,re,subprocess,sys
args=sys.argv[1:]
p=args[-1]
if p in ('[vdso]','[vsyscall]'):sys.exit(0)
if '-D' not in args and os.path.isfile(p) and '/lib' in p:
 r=subprocess.run(['readelf','-n',p],capture_output=True,text=True)
 m=re.search(r'Build ID: ([0-9a-f]+)',r.stdout)
 if m:
  h=m[1];debug='/tmp/astra-native-symbols/libc-debug/usr/lib/debug/.build-id/'+h[:2]+'/'+h[2:]+'.debug'
  if os.path.isfile(debug):args[-1]=debug
os.execv('/usr/bin/nm',['nm',*args])
