#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual main_loop_wait: only dead diagnostic query changes, not poll/timers."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
src = (ROOT / 'util/main-loop.c').read_text()


def function(signature):
    start = src.index(signature)
    end = src.index('{', start) + 1
    depth = 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    return src[start:end]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#define EMSCRIPTEN 1
#define REPLAY_MODE_NONE 0
#define MAIN_LOOP_POLL_FILL 0
#define MAIN_LOOP_POLL_ERR 1
#define MAIN_LOOP_POLL_OK 2
#define SCALE_MS 1000000
#define XSTAT_INC(x) ((void)0)
#define XWLD_MAIN() 0
#define XWLD_BEGIN(k,id) 0
#define XWLD_END(t) ((void)(t))
static bool wasm_debug_deadline_restore;
static int replay_mode;
static int gpollfds, main_loop_poll_notifiers, main_loop_tlg;
typedef struct { int state; uint32_t timeout; int pollfds; } MainLoopPoll;
static int query_calls, poll_calls, timer_calls, warp_calls, notify_calls;
static int fixture_ret, fixture_icount, debug_calls;
static int64_t first_deadline, selected_timeout;
static uint32_t fill_timeout;
static void g_array_set_size(int f,int n) {(void)f;assert(!n);}
static void notifier_list_notify(int *n,MainLoopPoll *p) {
 (void)n;notify_calls++;
 if(p->state==MAIN_LOOP_POLL_FILL && p->timeout>fill_timeout)p->timeout=fill_timeout;
}
static int64_t qemu_soonest_timeout(int64_t a,int64_t b) {
 return a<0?b:b<0?a:a<b?a:b;
}
static int64_t timerlistgroup_deadline_ns(int *t) {
 (void)t;query_calls++;
 /* A later debug query returns a DIFFERENT value. It must never schedule. */
 return query_calls==1?first_deadline:99999999;
}
static int os_host_main_loop_wait(int64_t ns) {
 poll_calls++;selected_timeout=ns;return fixture_ret;
}
static bool icount_enabled(void) {return fixture_icount;}
static void icount_start_warp_timer(void) {warp_calls++;}
static void qemu_clock_run_all_timers(void) {timer_calls++;}
void xemu_wasm_dbg_ring_put(const char *fmt,...) {
 if(!strncmp(fmt,"[loop] poll timeout=",20))debug_calls++;
}
'''
body = '\n'.join(function(s) for s in (
    'static void wasm_debug_deadline_init(',
    'static bool wasm_debug_deadline_needed(',
    'void main_loop_wait('))
tests = r'''
int main(void) {
 const char *policy[]={NULL,"","0","bad","1","1yes"};
 uint32_t timeouts[]={UINT32_MAX,0,1,1000};
 int64_t deadlines[]={-1,0,1,10000000};
 unsigned cases=0;
 for(unsigned e=0;e<6;e++) {
  if(policy[e])setenv("XEMU_WASM_DEBUG_DEADLINE_RESTORE",policy[e],1);
  else unsetenv("XEMU_WASM_DEBUG_DEADLINE_RESTORE");
  wasm_debug_deadline_init();assert(wasm_debug_deadline_restore==(e>=4));
  for(replay_mode=0;replay_mode<3;replay_mode++)
  for(unsigned nb=0;nb<2;nb++)
  for(unsigned t=0;t<4;t++)
  for(unsigned d=0;d<4;d++)
  for(fixture_ret=-1;fixture_ret<2;fixture_ret++)
  for(fixture_icount=0;fixture_icount<2;fixture_icount++) {
   bool debug=(e>=4)||replay_mode;
#ifdef XEMU_WASM_TRIPWIRE
   debug=true;
#endif
   assert(wasm_debug_deadline_needed()==debug);
   query_calls=poll_calls=timer_calls=warp_calls=notify_calls=debug_calls=0;
   fill_timeout=timeouts[t];first_deadline=deadlines[d];
   int64_t timeout=nb?0:fill_timeout==UINT32_MAX?-1:(int64_t)fill_timeout*SCALE_MS;
   int64_t expected=qemu_soonest_timeout(timeout,first_deadline);
   main_loop_wait(nb);
   assert(query_calls==1+debug && debug_calls==debug);
   assert(poll_calls==1 && timer_calls==1 && notify_calls==2);
   assert(warp_calls==fixture_icount && selected_timeout==expected);
   cases++;
  }
 }
 printf("PASS: %u scheduling/restore/replay/tripwire cases\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='wasm-debug-deadline-') as td:
    d = Path(td)
    (d / 'test.c').write_text(prefix + body + tests)
    for tripwire in (False, True):
        flags = ['-DXEMU_WASM_TRIPWIRE'] if tripwire else []
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-O1',
                        '-Wall', '-Wextra', '-Werror', *flags, str(d / 'test.c'),
                        '-o', str(d / 'test')], check=True)
        subprocess.run([str(d / 'test')], check=True)
