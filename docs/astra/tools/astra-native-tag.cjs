const fs=require('fs');
const wt=require('worker_threads');
if(process.env.ASTRA_PROFILE_DIR) {
 const tag=JSON.parse(require('/tmp/astra-v8tag.node').tag());
 fs.appendFileSync(process.env.ASTRA_PROFILE_DIR+'/threads.jsonl',JSON.stringify({...tag,threadId:wt.threadId,main:wt.isMainThread,time_ms:Date.now()})+'\n');
}
