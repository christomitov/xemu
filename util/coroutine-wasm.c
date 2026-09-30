/*
 * emscripten fiber coroutine initialization code
 * based on coroutine-ucontext.c
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 * Copyright (C) 2011  Kevin Wolf <kwolf@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.0 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/coroutine_int.h"
#include "qemu/coroutine-tls.h"

#include <emscripten/fiber.h>

typedef struct {
    Coroutine base;
    void *stack;
    size_t stack_size;

    void *asyncify_stack;
    size_t asyncify_stack_size;

    CoroutineAction action;

    pthread_t last_tid; /* thread-affinity tracker (wasm debug) */

    emscripten_fiber_t fiber;
} CoroutineEmscripten;

/**
 * Per-thread coroutine bookkeeping
 */
QEMU_DEFINE_STATIC_CO_TLS(Coroutine *, current);
QEMU_DEFINE_STATIC_CO_TLS(CoroutineEmscripten *, leader);
size_t leader_asyncify_stack_size = COROUTINE_STACK_SIZE;

static void coroutine_trampoline(void *co_)
{
    Coroutine *co = co_;

    while (true) {
        co->entry(co->entry_arg);
        qemu_coroutine_switch(co, co->caller, COROUTINE_TERMINATE);
    }
}

Coroutine *qemu_coroutine_new(void)
{
    CoroutineEmscripten *co;

    co = g_malloc0(sizeof(*co));

    co->stack_size = COROUTINE_STACK_SIZE;
    co->stack = qemu_alloc_stack(&co->stack_size);
#ifdef EMSCRIPTEN
    if ((uintptr_t)co->stack < 0x20000) {
        fprintf(stderr, "[co] qemu_alloc_stack returned %p\n", co->stack);
        abort();
    }
#endif

    co->asyncify_stack_size = COROUTINE_STACK_SIZE;
    co->asyncify_stack = g_malloc0(co->asyncify_stack_size);
    emscripten_fiber_init(&co->fiber, coroutine_trampoline, &co->base,
                          co->stack, co->stack_size, co->asyncify_stack,
                          co->asyncify_stack_size);

    return &co->base;
}

void qemu_coroutine_delete(Coroutine *co_)
{
    CoroutineEmscripten *co = DO_UPCAST(CoroutineEmscripten, base, co_);

    qemu_free_stack(co->stack, co->stack_size);
    g_free(co->asyncify_stack);
    g_free(co);
}

CoroutineAction qemu_coroutine_switch(Coroutine *from_, Coroutine *to_,
                      CoroutineAction action)
{
    CoroutineEmscripten *from = DO_UPCAST(CoroutineEmscripten, base, from_);
    CoroutineEmscripten *to = DO_UPCAST(CoroutineEmscripten, base, to_);

    set_current(to_);
    to->action = action;
#ifdef EMSCRIPTEN
    extern void xemu_wasm_lowmem_check(const char *);
    extern void xemu_wasm_dbg_ring_put(const char *, ...);
    xemu_wasm_lowmem_check("co switch pre");
    xemu_wasm_dbg_ring_put("[co] swap from=%p to=%p action=%d\n",
            (void*)(uintptr_t)from_, (void*)(uintptr_t)to_, (int)action);
    {
        emscripten_fiber_t *tf = &to->fiber, *ff = &from->fiber;
        if ((uintptr_t)tf->stack_base < 0x20000 ||
            (uintptr_t)ff->stack_base < 0x20000) {
            fprintf(stderr, "[co] BAD stack_base to=%p from=%p (action=%d)\n",
                    tf->stack_base, ff->stack_base, (int)action);
            abort();
        }
        if ((uintptr_t)tf->asyncify_data.stack_ptr == 0 ||
            (uintptr_t)tf->asyncify_data.stack_ptr >= (uintptr_t)tf->asyncify_data.stack_limit ||
            (uintptr_t)tf->asyncify_data.stack_limit == 0) {
            fprintf(stderr, "[co] BAD to asyncify_data ptr=%p limit=%p\n",
                    tf->asyncify_data.stack_ptr, tf->asyncify_data.stack_limit);
            abort();
        }
        if ((uintptr_t)ff->asyncify_data.stack_ptr == 0 ||
            (uintptr_t)ff->asyncify_data.stack_ptr >= (uintptr_t)ff->asyncify_data.stack_limit ||
            (uintptr_t)ff->asyncify_data.stack_limit == 0) {
            fprintf(stderr, "[co] BAD from asyncify_data ptr=%p limit=%p\n",
                    ff->asyncify_data.stack_ptr, ff->asyncify_data.stack_limit);
            abort();
        }
        /* asyncify bounds trace: watch for a fiber whose rewind region reaches
         * the main stack bottom (limit near 0) or has an odd 8-byte offset */
        xemu_wasm_dbg_ring_put("[co] ad from ptr=%p lim=%p | to ptr=%p lim=%p\n",
                (void*)(uintptr_t)ff->asyncify_data.stack_ptr,
                (void*)(uintptr_t)ff->asyncify_data.stack_limit,
                (void*)(uintptr_t)tf->asyncify_data.stack_ptr,
                (void*)(uintptr_t)tf->asyncify_data.stack_limit);
    }
    /* thread-affinity: record owner on switch-out, verify on switch-in */
    from->last_tid = pthread_self();
    xemu_wasm_lowmem_check("co switch ad");
#endif
    emscripten_fiber_swap(&from->fiber, &to->fiber);
#ifdef EMSCRIPTEN
    xemu_wasm_lowmem_check("co switch post");
    /*
     * QEMU may legitimately resume a coroutine on another thread (e.g. a
     * block request started on the main loop and completed while the vCPU
     * thread, holding the BQL, polls the main AioContext). Emscripten fibers
     * keep their stack and Asyncify data in shared memory, so migrating is
     * fine; this used to abort as a debugging tripwire. Log once.
     */
    if (to->last_tid != 0 && !pthread_equal(to->last_tid, pthread_self())) {
        static bool logged;
        if (!logged) {
            logged = true;
            fprintf(stderr, "[co] coroutine migrated between threads "
                    "(co=%p); continuing\n", (void *)(uintptr_t)to_);
        }
    }
#endif
    return from->action;
}

Coroutine *qemu_coroutine_self(void)
{
    Coroutine *self = get_current();

    if (!self) {
        CoroutineEmscripten *leaderp = get_leader();
        if (!leaderp) {
            leaderp = g_malloc0(sizeof(*leaderp));
            leaderp->asyncify_stack = g_malloc0(leader_asyncify_stack_size);
            leaderp->asyncify_stack_size = leader_asyncify_stack_size;
            emscripten_fiber_init_from_current_context(
                &leaderp->fiber,
                leaderp->asyncify_stack,
                leaderp->asyncify_stack_size);
            leaderp->stack = leaderp->fiber.stack_limit;
            leaderp->stack_size =
                leaderp->fiber.stack_base - leaderp->fiber.stack_limit;
            set_leader(leaderp);
        }
        self = &leaderp->base;
        set_current(self);
    }
    return self;
}

bool qemu_in_coroutine(void)
{
    Coroutine *self = get_current();

    return self && self->caller;
}
