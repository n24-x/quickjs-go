// quickjs-binding.c is the single cgo bridge translation unit for the
// amalgamated QuickJS source.
//
//
// The rest of the file is the cgo-required C shim layer:
//   - macro wrappers      (cgo cannot call C macros)
//   - variadic wrappers   (cgo cannot call variadic functions)
//   - opaque helpers      (Go object <-> int id)
//   - property accessors
//   - promise rejection tracking + bounded await loop (async eval)
//   - execute-timeout state
//
// Functions that need Go callbacks (the //export go* functions exposed via
// _cgo_export.h) are intentionally NOT here yet; they will be added together
// with the Go side that provides the callbacks.

#include "third_party/quickjs-amalgam.c"

// QuickjsGoPollInterrupt exposes the internal static __js_poll_interrupts()
// to the rest of the bindings. It is needed because the custom C event loop
// in AwaitValue() polls for interrupts while no bytecode is executing, so the
// engine's automatic interrupt checking never runs.
int QuickjsGoPollInterrupt(JSContext *ctx) {
	return __js_poll_interrupts(ctx);
}

/* ==========================================================================
 * Platform includes + mutex abstraction
 * ========================================================================== */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#else
#include <pthread.h>
#endif

#ifdef _WIN32
typedef SRWLOCK qjsgo_mutex_t;
#define QJSGO_MUTEX_INITIALIZER SRWLOCK_INIT
#define qjsgo_mutex_lock(mu) AcquireSRWLockExclusive((mu))
#define qjsgo_mutex_unlock(mu) ReleaseSRWLockExclusive((mu))
#else
typedef pthread_mutex_t qjsgo_mutex_t;
#define QJSGO_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define qjsgo_mutex_lock(mu) pthread_mutex_lock((mu))
#define qjsgo_mutex_unlock(mu) pthread_mutex_unlock((mu))
#endif

/* ==========================================================================
 * Macro wrappers - cgo cannot call C macros
 * ========================================================================== */

JSValue JS_NewNull() { return JS_NULL; }
JSValue JS_NewUndefined() { return JS_UNDEFINED; }
JSValue JS_NewUninitialized() { return JS_UNINITIALIZED; }
JSValue JS_NewException() { return JS_EXCEPTION; }
JSValue JS_NewTrue() { return JS_TRUE; }
JSValue JS_NewFalse() { return JS_FALSE; }

/* ==========================================================================
 * Variadic error wrappers - cgo cannot call variadic functions
 * ========================================================================== */

JSValue ThrowSyntaxError(JSContext *ctx, const char *fmt) { return JS_ThrowSyntaxError(ctx, "%s", fmt); }
JSValue ThrowTypeError(JSContext *ctx, const char *fmt) { return JS_ThrowTypeError(ctx, "%s", fmt); }
JSValue ThrowReferenceError(JSContext *ctx, const char *fmt) { return JS_ThrowReferenceError(ctx, "%s", fmt); }
JSValue ThrowRangeError(JSContext *ctx, const char *fmt) { return JS_ThrowRangeError(ctx, "%s", fmt); }
JSValue ThrowInternalError(JSContext *ctx, const char *fmt) { return JS_ThrowInternalError(ctx, "%s", fmt); }

/* ==========================================================================
 * Value access helpers - cgo cannot call macros
 * ========================================================================== */

int ValueGetTag(JSValueConst v) {
    return JS_VALUE_GET_TAG(v);
}

void* JS_VALUE_GET_PTR_Wrapper(JSValue val) {
    return JS_VALUE_GET_PTR(val);
}

/* ==========================================================================
 * Opaque pointer helpers
 * ========================================================================== */

void* IntToOpaque(int32_t id) {
    return (void*)(intptr_t)id;
}

int32_t OpaqueToInt(void* opaque) {
    return (int32_t)(intptr_t)opaque;
}

/* ==========================================================================
 * Property accessors
 * ========================================================================== */

int SetPropertyByNameLen(JSContext *ctx, JSValueConst obj, const char *name, size_t name_len, JSValue val) {
    JSAtom atom = JS_NewAtomLen(ctx, name, name_len);
    if (atom == JS_ATOM_NULL) {
        JS_FreeValue(ctx, val);
        return -1;
    }
    int rc = JS_SetProperty(ctx, obj, atom, val);
    JS_FreeAtom(ctx, atom);
    return rc;
}

JSValue GetPropertyByNameLen(JSContext *ctx, JSValueConst obj, const char *name, size_t name_len) {
    JSAtom atom = JS_NewAtomLen(ctx, name, name_len);
    if (atom == JS_ATOM_NULL) {
        return JS_EXCEPTION;
    }
    JSValue ret = JS_GetProperty(ctx, obj, atom);
    JS_FreeAtom(ctx, atom);
    return ret;
}

JSValue CallPropertyByNameLen(JSContext *ctx, JSValueConst obj, const char *name, size_t name_len, int argc, JSValue *argv) {
    JSAtom atom = JS_NewAtomLen(ctx, name, name_len);
    if (atom == JS_ATOM_NULL) {
        return JS_EXCEPTION;
    }

    JSValue fn = JS_GetProperty(ctx, obj, atom);
    JS_FreeAtom(ctx, atom);
    if (JS_IsException(fn)) {
        return fn;
    }

    JSValue ret = JS_Call(ctx, fn, obj, argc, argv);
    JS_FreeValue(ctx, fn);
    return ret;
}

int DetectModuleSourceWithProbe(JSContext *ctx, const char *code, size_t code_len) {
    if (!JS_DetectModule(code, code_len)) {
        return 0;
    }

    static const char *probe_filename = "<module-detect>";
    int probe_flags = JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY;
    JSValue probe = JS_Eval(ctx, code, code_len, probe_filename, probe_flags);
    if (JS_IsException(probe)) {
        JSValue exception = JS_GetException(ctx);
        JS_FreeValue(ctx, exception);
        return 1;
    }

    JS_FreeValue(ctx, probe);
    return 0;
}

/* ==========================================================================
 * Await loop config + thread id
 * ========================================================================== */

static int g_await_poll_slice_ms = 10;
static qjsgo_mutex_t g_await_poll_slice_mu = QJSGO_MUTEX_INITIALIZER;

int GetAwaitPollSliceMs(void) {
    int value;
    qjsgo_mutex_lock(&g_await_poll_slice_mu);
    value = g_await_poll_slice_ms;
    qjsgo_mutex_unlock(&g_await_poll_slice_mu);
    return value;
}

void SetAwaitPollSliceMs(int timeout_ms) {
    if (timeout_ms <= 0) {
        return;
    }

    qjsgo_mutex_lock(&g_await_poll_slice_mu);
    g_await_poll_slice_ms = timeout_ms;
    qjsgo_mutex_unlock(&g_await_poll_slice_mu);
}

uint64_t CurrentThreadID(void) {
#ifdef _WIN32
    return (uint64_t)GetCurrentThreadId();
#elif defined(__APPLE__)
    uint64_t tid = 0;
    (void)pthread_threadid_np(NULL, &tid);
    return tid;
#elif defined(__linux__)
    return (uint64_t)syscall(SYS_gettid);
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}

/* ==========================================================================
 * Promise rejection tracking
 * ========================================================================== */

typedef struct RejectionEntry {
    JSValue promise;
    JSValue reason;
    struct RejectionEntry *next;
} RejectionEntry;

typedef struct RejectionStateEntry {
    JSRuntime *rt;
    RejectionEntry *head;
    RejectionEntry *tail;
    struct RejectionStateEntry *next;
} RejectionStateEntry;

static RejectionStateEntry *g_rejection_states = NULL;
static qjsgo_mutex_t g_rejection_states_mu = QJSGO_MUTEX_INITIALIZER;

static void freeRejectionEntry(JSRuntime *rt, RejectionEntry *entry) {
    if (!entry) {
        return;
    }
    JS_FreeValueRT(rt, entry->promise);
    JS_FreeValueRT(rt, entry->reason);
    free(entry);
}

static RejectionStateEntry *getRejectionState(JSRuntime *rt, int create) {
    RejectionStateEntry *current = g_rejection_states;
    while (current) {
        if (current->rt == rt) {
            return current;
        }
        current = current->next;
    }

    if (!create) {
        return NULL;
    }

    RejectionStateEntry *state = malloc(sizeof(RejectionStateEntry));
    if (!state) {
        return NULL;
    }

    state->rt = rt;
    state->head = NULL;
    state->tail = NULL;
    state->next = g_rejection_states;
    g_rejection_states = state;
    return state;
}

static void clearRejectionState(JSRuntime *rt) {
    qjsgo_mutex_lock(&g_rejection_states_mu);

    RejectionStateEntry *prev = NULL;
    RejectionStateEntry *current = g_rejection_states;
    while (current) {
        if (current->rt == rt) {
            RejectionEntry *entry = current->head;
            while (entry) {
                RejectionEntry *next = entry->next;
                freeRejectionEntry(rt, entry);
                entry = next;
            }

            if (prev) {
                prev->next = current->next;
            } else {
                g_rejection_states = current->next;
            }
            free(current);
            break;
        }
        prev = current;
        current = current->next;
    }

    qjsgo_mutex_unlock(&g_rejection_states_mu);
}

static void QuickjsGoPromiseRejectionTracker(JSContext *ctx,
                                             JSValueConst promise,
                                             JSValueConst reason,
                                             bool is_handled,
                                             void *opaque) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    (void)opaque;

    qjsgo_mutex_lock(&g_rejection_states_mu);
    RejectionStateEntry *state = getRejectionState(rt, 1);
    if (!state) {
        qjsgo_mutex_unlock(&g_rejection_states_mu);
        return;
    }

    RejectionEntry *prev = NULL;
    RejectionEntry *current = state->head;
    while (current) {
        if (JS_IsSameValue(ctx, current->promise, promise)) {
            break;
        }
        prev = current;
        current = current->next;
    }

    if (is_handled) {
        if (current) {
            if (prev) {
                prev->next = current->next;
            } else {
                state->head = current->next;
            }
            if (state->tail == current) {
                state->tail = prev;
            }
            freeRejectionEntry(rt, current);
        }
        qjsgo_mutex_unlock(&g_rejection_states_mu);
        return;
    }

    if (current) {
        JS_FreeValueRT(rt, current->reason);
        current->reason = JS_DupValueRT(rt, reason);
        qjsgo_mutex_unlock(&g_rejection_states_mu);
        return;
    }

    RejectionEntry *entry = malloc(sizeof(RejectionEntry));
    if (!entry) {
        qjsgo_mutex_unlock(&g_rejection_states_mu);
        return;
    }

    entry->promise = JS_DupValueRT(rt, promise);
    entry->reason = JS_DupValueRT(rt, reason);
    entry->next = NULL;

    if (state->tail) {
        state->tail->next = entry;
    } else {
        state->head = entry;
    }
    state->tail = entry;

    qjsgo_mutex_unlock(&g_rejection_states_mu);
}

void SetPromiseRejectionTracker(JSRuntime *rt, int enabled) {
    if (!rt) {
        return;
    }

    if (enabled) {
        JS_SetHostPromiseRejectionTracker(rt, QuickjsGoPromiseRejectionTracker, NULL);
        return;
    }

    JS_SetHostPromiseRejectionTracker(rt, NULL, NULL);
    clearRejectionState(rt);
}

static int popUnhandledPromiseRejection(JSContext *ctx, JSValue *reason_out) {
    JSRuntime *rt = JS_GetRuntime(ctx);

    qjsgo_mutex_lock(&g_rejection_states_mu);
    RejectionStateEntry *state = getRejectionState(rt, 0);
    if (!state || !state->head) {
        qjsgo_mutex_unlock(&g_rejection_states_mu);
        return 0;
    }

    RejectionEntry *entry = state->head;
    state->head = entry->next;
    if (!state->head) {
        state->tail = NULL;
    }

    *reason_out = entry->reason;
    JS_FreeValueRT(rt, entry->promise);
    free(entry);

    qjsgo_mutex_unlock(&g_rejection_states_mu);
    return 1;
}

static int ThrowUnhandledPromiseRejectionIfAny(JSContext *ctx) {
    JSValue reason;
    if (!popUnhandledPromiseRejection(ctx, &reason)) {
        return 0;
    }

    JS_Throw(ctx, reason);
    return 1;
}

/* ==========================================================================
 * Execute timeout state
 * ========================================================================== */

typedef struct {
    time_t start;
    time_t timeout;
} TimeoutStruct;

typedef struct TimeoutStateEntry {
    JSRuntime *rt;
    TimeoutStruct *state;
    struct TimeoutStateEntry *next;
} TimeoutStateEntry;

static TimeoutStateEntry *g_timeout_states = NULL;
static qjsgo_mutex_t g_timeout_states_mu = QJSGO_MUTEX_INITIALIZER;

static int isExecuteTimeoutExceeded(JSRuntime *rt) {
    time_t start = 0;
    time_t timeout = 0;
    int found = 0;

    qjsgo_mutex_lock(&g_timeout_states_mu);
    TimeoutStateEntry *current = g_timeout_states;
    while (current) {
        if (current->rt == rt && current->state != NULL) {
            start = current->state->start;
            timeout = current->state->timeout;
            found = 1;
            break;
        }
        current = current->next;
    }
    qjsgo_mutex_unlock(&g_timeout_states_mu);

    if (!found || timeout <= 0) {
        return 0;
    }

    return (time(NULL) - start) > timeout;
}

static TimeoutStruct *takeTimeoutState(JSRuntime *rt) {
    qjsgo_mutex_lock(&g_timeout_states_mu);

    TimeoutStateEntry *prev = NULL;
    TimeoutStateEntry *current = g_timeout_states;
    while (current) {
        if (current->rt == rt) {
            TimeoutStruct *state = current->state;
            if (prev) {
                prev->next = current->next;
            } else {
                g_timeout_states = current->next;
            }
            free(current);
            qjsgo_mutex_unlock(&g_timeout_states_mu);
            return state;
        }
        prev = current;
        current = current->next;
    }

    qjsgo_mutex_unlock(&g_timeout_states_mu);
    return NULL;
}

static int setTimeoutState(JSRuntime *rt, TimeoutStruct *state) {
    qjsgo_mutex_lock(&g_timeout_states_mu);

    TimeoutStateEntry *current = g_timeout_states;
    while (current) {
        if (current->rt == rt) {
            if (current->state != NULL && current->state != state) {
                free(current->state);
            }
            current->state = state;
            qjsgo_mutex_unlock(&g_timeout_states_mu);
            return 0;
        }
        current = current->next;
    }

    TimeoutStateEntry *entry = malloc(sizeof(TimeoutStateEntry));
    if (!entry) {
        qjsgo_mutex_unlock(&g_timeout_states_mu);
        return -1;
    }
    entry->rt = rt;
    entry->state = state;
    entry->next = g_timeout_states;
    g_timeout_states = entry;

    qjsgo_mutex_unlock(&g_timeout_states_mu);
    return 0;
}

static void clearTimeoutState(JSRuntime *rt) {
    TimeoutStruct *state = takeTimeoutState(rt);
    if (state) {
        free(state);
    }
}

int GetTimeoutOpaqueCount(void) {
    int count = 0;

    qjsgo_mutex_lock(&g_timeout_states_mu);
    TimeoutStateEntry *current = g_timeout_states;
    while (current) {
        if (current->state != NULL) {
            count++;
        }
        current = current->next;
    }
    qjsgo_mutex_unlock(&g_timeout_states_mu);

    return count;
}

int timeoutHandler(JSRuntime *rt, void *opaque) {
    TimeoutStruct* ts = (TimeoutStruct*)opaque;
    time_t timeout = ts->timeout;
    time_t start = ts->start;
    if (timeout <= 0) {
        return 0;
    }

    time_t now = time(NULL);
    if (now - start > timeout) {
        return 1;
    }

    return 0;
}

void SetExecuteTimeout(JSRuntime *rt, time_t timeout) {
    if (timeout <= 0) {
        JS_SetInterruptHandler(rt, NULL, NULL);
        clearTimeoutState(rt);
        return;
    }

    TimeoutStruct* ts = malloc(sizeof(TimeoutStruct));
    if (!ts) {
        JS_SetInterruptHandler(rt, NULL, NULL);
        clearTimeoutState(rt);
        return;
    }

    ts->start = time(NULL);
    ts->timeout = timeout;
    JS_SetInterruptHandler(rt, timeoutHandler, ts);

    if (setTimeoutState(rt, ts) != 0) {
        JS_SetInterruptHandler(rt, NULL, NULL);
        free(ts);
        clearTimeoutState(rt);
    }
}

/* ==========================================================================
 * Bounded await loop
 * ========================================================================== */

JSValue AwaitValue(JSContext *ctx, JSValue obj) {
    JSRuntime *rt = JS_GetRuntime(ctx);

    for (;;) {
        if (ThrowUnhandledPromiseRejectionIfAny(ctx)) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }

        if (isExecuteTimeoutExceeded(rt)) {
            JS_FreeValue(ctx, obj);
            JS_ThrowInternalError(ctx, "interrupted");
            return JS_EXCEPTION;
        }

        int state = JS_PromiseState(ctx, obj);
        if (state == JS_PROMISE_FULFILLED) {
            JSValue ret = JS_PromiseResult(ctx, obj);
            JS_FreeValue(ctx, obj);
            return ret;
        }

        if (state == JS_PROMISE_REJECTED) {
            JSValue ret = JS_Throw(ctx, JS_PromiseResult(ctx, obj));
            JS_FreeValue(ctx, obj);
            return ret;
        }

        if (state != JS_PROMISE_PENDING) {
            return obj;
        }

        JSContext *ctx1 = NULL;
        int err = JS_ExecutePendingJob(rt, &ctx1);
        if (err < 0) {
            if (ctx1 != NULL && ctx1 != ctx) {
                JSValue ex = JS_GetException(ctx1);
                JS_Throw(ctx, ex);
            }
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }

        /* Bound host IO polling to avoid an uninterruptible blocking wait. */
        if (err == 0) {
            /*
             * Drive timers/microtasks so promises resolved by setTimeout can
             * progress while awaiting.
             */
            int loop_once_ret = js_std_loop_once(ctx);
            if (loop_once_ret == -2) {
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }
            if (loop_once_ret == 0) {
                continue;
            }

            int poll_timeout_ms = GetAwaitPollSliceMs();
            if (loop_once_ret > 0 && loop_once_ret < poll_timeout_ms) {
                poll_timeout_ms = loop_once_ret;
            }

            int poll_ret = js_std_poll_io(ctx, poll_timeout_ms);
            if (poll_ret < 0) {
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }

            if (QuickjsGoPollInterrupt(ctx) < 0) {
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }
        }
    }
}

JSValue EvalAndAwait(JSContext *ctx, const char *input, size_t input_len, const char *filename, int eval_flags) {
    JSValue eval_result = JS_Eval(ctx, input, input_len, filename, eval_flags);
    return AwaitValue(ctx, eval_result);
}
