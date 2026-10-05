#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real pthread mutex/condwait tests of the production BQL hint primitives.

Not a guest-clock or Wasm Asyncify oracle. The emulator gate is separate.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(text, signature):
    begin = text.index(signature)
    pos = text.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[begin:pos]


cpus = (ROOT / 'system/cpus.c').read_text()
threads = (ROOT / 'util/qemu-thread-posix.c').read_text()
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#define EMSCRIPTEN 1
#define TSA_NO_TSA
#define qatomic_read(p) __atomic_load_n(p,__ATOMIC_RELAXED)
#define qatomic_set(p,v) __atomic_store_n(p,v,__ATOMIC_RELAXED)
#define XWLC_NOTE(...) do {} while(0)
#define XPHASE_VCPU 0
#define XPHASE_SET(t,s) (xemu_wasm_phase[t]=(s))
static const char *xemu_wasm_phase[2];
static __thread bool local_held;
static bool bql_locked(void) { return local_held; }
static void set_bql_locked(bool v) { local_held=v; }
typedef struct { bool initialized; pthread_mutex_t lock; } QemuMutex;
typedef struct { bool initialized; pthread_cond_t cond; } QemuCond;
static QemuMutex bql;
bool xemu_wasm_bql_spin_read;
static uint32_t bql_wasm_held;
static bool mutex_is_bql(QemuMutex *m) { return m==&bql; }
static __thread int xemu_wasm_is_vcpu;
static const char *wait_site(const char *k,const char *f,int l)
{ (void)k; (void)f; (void)l; return "wait"; }
static void error_exit(int e,const char *s) { (void)e;(void)s;abort(); }
#define trace_qemu_mutex_unlock(m,f,l) do {} while(0)
#define trace_qemu_mutex_locked(m,f,l) do {} while(0)
'''
source = '\n'.join(function(cpus, s) for s in [
    'void bql_update_status(', 'void bql_wasm_cond_wait_hint(',
    'static bool bql_wasm_spin_busy('])
source += r'''
static void qemu_mutex_pre_unlock(QemuMutex *m,const char *f,int l) {
 (void)f; (void)l; if(mutex_is_bql(m))bql_update_status(false);
}
static void qemu_mutex_post_lock(QemuMutex *m,const char *f,int l) {
 (void)f; (void)l; if(mutex_is_bql(m))bql_update_status(true);
}
'''
source += '\n'.join(function(threads, s) for s in [
    'int qemu_mutex_trylock_impl(', 'void qemu_mutex_unlock_impl(',
    'void qemu_cond_wait_impl(', 'qemu_cond_timedwait_ts('])
# The timed-wait signature starts after its return type in the source.
source = source.replace('\nqemu_cond_timedwait_ts(', '\nstatic bool qemu_cond_timedwait_ts(')
test = r'''
static QemuCond cond;
static unsigned shared, ready;
static void acquire(bool retry_only) {
 unsigned checks=0;
 if(!retry_only && !qemu_mutex_trylock_impl(&bql,__FILE__,__LINE__))return;
 while(bql_wasm_spin_busy(&checks) ||
       qemu_mutex_trylock_impl(&bql,__FILE__,__LINE__)) {}
 assert(local_held);
}
static void release(void) {qemu_mutex_unlock_impl(&bql,__FILE__,__LINE__);}
static void *worker(void *unused) {
 (void)unused;
 for(unsigned i=0;i<10000;i++) {
  acquire(false); shared++; release();
 }
 return NULL;
}
static void *signal_worker(void *unused) {
 (void)unused; acquire(false); ready=1;
 assert(!pthread_cond_signal(&cond.cond)); release(); return NULL;
}
int main(void) {
 assert(!pthread_mutex_init(&bql.lock,NULL));bql.initialized=true;
 assert(!pthread_cond_init(&cond.cond,NULL));cond.initialized=true;
 for(unsigned enabled=0;enabled<2;enabled++) {
  xemu_wasm_bql_spin_read=enabled;
  qatomic_set(&bql_wasm_held,0);shared=0;
  pthread_t ids[4];
  for(unsigned i=0;i<4;i++)assert(!pthread_create(&ids[i],NULL,worker,NULL));
  for(unsigned i=0;i<4;i++)assert(!pthread_join(ids[i],NULL));
  assert(shared==40000 && !qatomic_read(&bql_wasm_held));
  for(unsigned timed=0;timed<2;timed++) {
   for(unsigned n=0;n<50;n++) {
    acquire(false);ready=0;
    assert(!pthread_create(&ids[0],NULL,signal_worker,NULL));
    while(!ready) {
     if(timed) {
      struct timespec ts;assert(!clock_gettime(CLOCK_REALTIME,&ts));ts.tv_sec+=5;
      assert(qemu_cond_timedwait_ts(&cond,&bql,&ts,__FILE__,__LINE__));
     } else qemu_cond_wait_impl(&cond,&bql,__FILE__,__LINE__);
     assert(local_held && (!enabled || qatomic_read(&bql_wasm_held)));
    }
    release();assert(!pthread_join(ids[0],NULL));
   }
  }
  /* Timeout must also restore the hint and preserve coroutine-local TLS. */
  acquire(false);struct timespec ts={0};
  assert(!qemu_cond_timedwait_ts(&cond,&bql,&ts,__FILE__,__LINE__));
  assert(local_held && (!enabled || qatomic_read(&bql_wasm_held)));release();
 }
 xemu_wasm_bql_spin_read=true;
 /* A stale held hint must not prevent a real probe on the 64th check. */
 qatomic_set(&bql_wasm_held,1);unsigned checks=0;
 for(unsigned i=0;i<63;i++)assert(bql_wasm_spin_busy(&checks));
 assert(!bql_wasm_spin_busy(&checks));
 acquire(true);assert(local_held);release();
 /* A false hint never grants entry: the actual mutex remains authoritative. */
 acquire(false);qatomic_set(&bql_wasm_held,0);checks=0;
 assert(!bql_wasm_spin_busy(&checks));
 assert(qemu_mutex_trylock_impl(&bql,__FILE__,__LINE__)==-EBUSY);
 release();
 puts("PASS: 80000 protected increments, 200 condwait handoffs, timeouts, stale true/false hints");
}
'''
with tempfile.TemporaryDirectory(prefix='wasm-bql-spin-') as d:
    d = Path(d)
    (d / 'test.c').write_text(prefix + source + test)
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-O1', '-pthread',
                    '-Wall', '-Wextra', '-Wno-unused-parameter',
                    str(d / 'test.c'), '-o', str(d / 'test')], check=True)
    subprocess.run([str(d / 'test')], check=True, timeout=30)
