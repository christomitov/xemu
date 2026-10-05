// Offline V8 log processor only: stock Node truncates nm output at 1 MiB.
const cp=require('child_process');
const spawn=cp.spawnSync;
cp.spawnSync=function(command,args,options={}) {
 const r=spawn(command,args,{maxBuffer:128*1024*1024,...options});
 if(r.error || r.signal) console.error('symbol command failed',command,r.error,r.signal);
 return r;
};
