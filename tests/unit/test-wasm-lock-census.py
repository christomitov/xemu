#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual census and mutex-wrapper code: filtering, counts, no extra probes."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(text, signature):
    begin = text.index(signature)
    brace = text.index('{', begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#define EMSCRIPTEN 1
#define EMSCRIPTEN_KEEPALIVE
#define unlikely(x) (x)
#define G_N_ELEMENTS(a) (sizeof(a)/sizeof((a)[0]))
static int64_t fixture_now;
static int now_calls;
static int64_t xemu_wasm_stats_now_ns(void) { now_calls++;return fixture_now; }
static bool bql_locked(void) { return true; }
#define qatomic_read(p) __atomic_load_n(p, __ATOMIC_RELAXED)
#define qatomic_set(p,v) __atomic_store_n(p,v,__ATOMIC_RELAXED)
#define qatomic_fetch_inc(p) __atomic_fetch_add(p,1,__ATOMIC_RELAXED)
#define qatomic_store_release(p,v) __atomic_store_n(p,v,__ATOMIC_RELEASE)
#define qatomic_load_acquire(p) __atomic_load_n(p,__ATOMIC_ACQUIRE)
__thread int xemu_wasm_is_vcpu;
'''
header = (ROOT / 'include/qemu/xemu-wasm-lock-census.h').read_text().replace(
    '#include "qemu/atomic.h"', '')
impl = (ROOT / 'util/qemu-wasm-lock-census.c.inc').read_text().replace(
    '#include <emscripten/emscripten.h>', '').replace(
    '#include "qemu-wasm-bql-detail.c.inc"',
    (ROOT / 'util/qemu-wasm-bql-detail.c.inc').read_text())
wrappers = (ROOT / 'util/qemu-thread-posix.c').read_text()
mock = r'''
typedef struct { bool initialized; int lock; } QemuMutex;
static int ntry, nlock, npre, npost, result;
static int mock_try(int *m) { (void)m; ntry++; return result; }
static int mock_lock(int *m) { (void)m; nlock++; return 0; }
#define pthread_mutex_trylock mock_try
#define pthread_mutex_lock mock_lock
static void error_exit(int e,const char *s) { (void)e; (void)s; abort(); }
#define qemu_mutex_pre_lock(m,f,l) npre++
#define qemu_mutex_post_lock(m,f,l) npost++
#define XPHASE_VCPU 0
static const char *xemu_wasm_phase[2];
#define XPHASE_SET(i,s) (xemu_wasm_phase[i]=(s))
static const char *wait_site(const char *k,const char *f,int l)
{ (void)k; (void)f; (void)l; return "wait"; }
'''
body = function(wrappers, 'void qemu_mutex_lock_impl(') + '\n' + function(
    wrappers, 'int qemu_mutex_trylock_impl(')
common = (ROOT / 'util/qemu-thread-common.h').read_text()
body += r'''
#undef qemu_mutex_post_lock
#define trace_qemu_mutex_unlock(m,f,l) do {(void)m;(void)f;(void)l;} while(0)
#define trace_qemu_mutex_locked(m,f,l) do {(void)m;(void)f;(void)l;} while(0)
static QemuMutex *fixture_bql;
static bool holder_tls;
static bool mutex_is_bql(QemuMutex *m) { return m==fixture_bql; }
static void bql_update_status(bool b) { holder_tls=b; }
''' + function(common, 'static inline void qemu_mutex_post_lock(') + '\n' + function(
    common, 'static inline void qemu_mutex_pre_unlock(')
test = r'''
static void *publisher(void *unused) {
 (void)unused;
 for(unsigned i=0;i<100000;i++) {
  xemu_wasm_lock_census_publish((const char *)(uintptr_t)0x12345678,123);
  xemu_wasm_lock_census_publish((const char *)(uintptr_t)0x87654321,456);
 }
 return NULL;
}

static unsigned sum(unsigned kind,bool busy) {
 unsigned n=0;
 for(unsigned i=0;i<XWLC_SLOTS;i++) if(xwlc_sites[i].file &&
     xwlc_sites[i].kind==kind)n+=busy?xwlc_sites[i].busy:xwlc_sites[i].calls;
 return n;
}
int main(void) {
 unsetenv("XEMU_WASM_BQL_DETAIL");
 const char *values[]={NULL,"","0","invalid","1","1yes"};
 for(unsigned i=0;i<6;i++) {
  if(values[i])setenv("XEMU_WASM_LOCK_CENSUS",values[i],1);
  else unsetenv("XEMU_WASM_LOCK_CENSUS");
  xemu_wasm_lock_census_init();
  assert(xemu_wasm_lock_census_enabled==(i>=4));
 }
 for(unsigned enabled=0;enabled<2;enabled++)
 for(unsigned vcpu=0;vcpu<2;vcpu++)
 for(unsigned busy=0;busy<2;busy++) {
  memset(xwlc_sites,0,sizeof(xwlc_sites)); xwlc_overflow=0;
  xemu_wasm_lock_census_enabled=enabled; xemu_wasm_is_vcpu=vcpu;
  result=busy?EBUSY:0; ntry=nlock=npre=npost=0;
  QemuMutex m={.initialized=true};
  const char *old="original"; xemu_wasm_phase[0]=old;
  qemu_mutex_lock_impl(&m,"fixture.c",123);
  assert(ntry==(int)vcpu && nlock==(!vcpu||busy) && npre==1 && npost==1);
  assert(xemu_wasm_phase[0]==old);
  assert(sum(XWLC_LOCK,false)==(enabled&&vcpu));
  assert(sum(XWLC_LOCK,true)==(enabled&&vcpu&&busy));
  ntry=nlock=npre=npost=0;
  assert(qemu_mutex_trylock_impl(&m,"fixture.c",124)==(busy?-EBUSY:0));
  assert(ntry==1 && !nlock && !npre && npost==!busy);
  assert(sum(XWLC_TRY,false)==(enabled&&vcpu));
  assert(sum(XWLC_TRY,true)==(enabled&&vcpu&&busy));
  XWLC_NOTE(XWLC_BQL,"io.c",42,false);
  XWLC_NOTE(XWLC_BQL_WAIT,"io.c",42,true);
  assert(sum(XWLC_BQL,false)==(enabled&&vcpu));
  assert(sum(XWLC_BQL_WAIT,true)==(enabled&&vcpu));
  assert(strlen(xemu_wasm_lock_census_top())<65536);
 }
 memset(xwlc_sites,0,sizeof(xwlc_sites));
 for(unsigned i=0;i<XWLC_SLOTS;i++)
  xemu_wasm_lock_census_note(XWLC_LOCK,"full.c",i,false);
 assert(sum(XWLC_LOCK,false)==XWLC_SLOTS);
 xemu_wasm_lock_census_note(XWLC_TRY,"extra.c",1,false);
 assert(xwlc_overflow==1);
 xwlc_sites[0].calls=UINT32_MAX;
 xemu_wasm_lock_census_note(xwlc_sites[0].kind,xwlc_sites[0].file,
                           xwlc_sites[0].line,true);
 assert(xwlc_overflow==2 && xwlc_sites[0].calls==UINT32_MAX);
 assert(strstr(xemu_wasm_lock_census_top(),"overflow|2\n"));
 /* Wasm32 site tokens, represented as opaque 32-bit addresses here. */
 const char *file=(const char *)(uintptr_t)0x12345678;
 xemu_wasm_lock_census_enabled=true;xemu_wasm_is_vcpu=0;
 QemuMutex m={.initialized=true};fixture_bql=&m;
 qemu_mutex_post_lock(&m,file,123);
 assert(holder_tls && xemu_wasm_lock_census_holder()==0x123456780000007bULL);
 qemu_mutex_pre_unlock(&m,file,123);
 assert(!holder_tls && !xemu_wasm_lock_census_holder());
 /* Timed waits use these same publications without changing local TLS. */
 holder_tls=true;XWLC_MUTEX_HOLDER(&m,NULL,0);
 assert(holder_tls && !xemu_wasm_lock_census_holder());
 XWLC_MUTEX_HOLDER(&m,file,123);
 assert(holder_tls && xemu_wasm_lock_census_holder()==0x123456780000007bULL);
 pthread_t thread;assert(!pthread_create(&thread,NULL,publisher,NULL));
 for(unsigned i=0;i<100000;i++) {
  uint64_t h=xemu_wasm_lock_census_holder();
  assert(h==0x123456780000007bULL || h==0x87654321000001c8ULL);
 }
 assert(!pthread_join(thread,NULL));
 xemu_wasm_lock_census_publish(NULL,0);
 xemu_wasm_lock_census_wait("waiter.c",77,0,1234);
 xemu_wasm_lock_census_wait("waiter.c",77,0,5678);
 xemu_wasm_lock_census_wait("waiter.c",77,0,-100);
 assert(strstr(xemu_wasm_lock_census_top(),"wait|waiter.c:77|?:0|3|6912"));
 assert(!now_calls); /* Legacy census alone adds no clock calls. */
 unsetenv("XEMU_WASM_LOCK_CENSUS");
 for(unsigned i=0;i<6;i++) {
  if(values[i])setenv("XEMU_WASM_BQL_DETAIL",values[i],1);
  else unsetenv("XEMU_WASM_BQL_DETAIL");
  xemu_wasm_lock_census_init();
  assert(xemu_wasm_bql_detail_enabled==(i>=4));
  assert(xemu_wasm_lock_census_enabled==(i>=4));
  assert(!xemu_wasm_lock_calls_enabled);
 }
 assert(!xemu_wasm_bql_detail_begin(XWLD_BH_CB,1) && !now_calls);
 fixture_now=100;unsigned root=xemu_wasm_bql_detail_main();
 fixture_now=110;unsigned timer=XWLD_BEGIN(XWLD_TIMER_CB,7);
 fixture_now=120;unsigned bh=XWLD_BEGIN(XWLD_BH_CB,9);
 fixture_now=130;xemu_wasm_lock_census_publish(NULL,0);
 assert(!xemu_wasm_lock_census_holder());
 fixture_now=1130;xemu_wasm_lock_census_publish(file,123);
 assert(xemu_wasm_lock_census_holder()==xwld_activity());
 fixture_now=1140;XWLD_END(bh);
 fixture_now=1150;XWLD_END(timer);
 fixture_now=1160;XWLD_END(root);
 fixture_now=1170;xemu_wasm_lock_census_publish(NULL,0);
 assert(xwld_sites[xwld_slot(XWLD_SETUP,0)].ns==20);
 assert(xwld_sites[xwld_slot(XWLD_TIMER_CB,7)].ns==20);
 assert(xwld_sites[xwld_slot(XWLD_BH_CB,9)].ns==20);
 assert(xwld_sites[xwld_slot(XWLD_OUTER,0)].ns==10);
 assert(xwld_sites[xwld_slot(XWLD_BH_CB,9)].calls==1);
 assert(strstr(xemu_wasm_lock_census_top(),"detail|ml:bh:9|1|20"));
 /* Released 1000 ns excluded; nested callbacks are not double counted. */
 uint64_t total=0;for(unsigned i=0;i<XWLD_SLOTS;i++)total+=xwld_sites[i].ns;
 assert(total==70);
 for(unsigned i=0;i<XWLD_SLOTS;i++) {
  unsigned t=XWLD_BEGIN(XWLD_TIMER_CB,1000+i);XWLD_END(t);
 }
 assert(xwld_overflow==4);
 puts("PASS: census probes/pairing, nested exclusive held-time scopes, 1000ns release exclusion, detail policy/overflow");
}
'''
with tempfile.TemporaryDirectory(prefix='wasm-lock-census-') as d:
    d = Path(d)
    (d / 'test.c').write_text(prefix + header + impl + mock + body + test)
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-O1',
                    '-Wall', '-Wextra', '-Werror', '-pthread', str(d / 'test.c'),
                    '-o', str(d / 'test')], check=True)
    subprocess.run([str(d / 'test')], check=True)
