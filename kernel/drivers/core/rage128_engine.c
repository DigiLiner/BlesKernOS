#include "../../include/rage128_engine.h"
#include "../../include/klock.h"
#include "../../include/perfmon.h"
#include "../../libc/include/stdio.h"

static const r128_engine_2d_ops_t *g_r128_2d;
static kmutex_t g_r128_lock = KMUTEX_INITIALIZER;
static r128_engine_context_t g_r128_context;
static uint32_t g_r128_switch_2d;
static uint32_t g_r128_switch_3d;
static uint32_t g_r128_present_hw;
static uint32_t g_r128_present_fail;

/* Disabling preemption only serializes the local CPU. The shared 2D/3D
 * engine needs the kernel's atomic, recursive, per-task mutex on SMP too. */
static void r128_lock(void) {
    uint64_t started = perfmon_scope_begin();
    kmutex_lock(&g_r128_lock);
    perfmon_scope_end(PERF_SCOPE_ATI_LOCK, started);
    perfmon_scope_note(PERF_SCOPE_ATI_LOCK, 0U, 0U, 0U);
}

static void r128_unlock(void) {
    kmutex_unlock(&g_r128_lock);
}

bool r128_engine_register_2d(const r128_engine_2d_ops_t *ops) {
    if (!ops || ops->abi_version != BK_R128_ENGINE_2D_ABI_VERSION ||
        ops->descriptor_size != sizeof(*ops) || !ops->name ||
        !ops->enter_2d || !ops->wait_2d_idle || !ops->blit_vram32 ||
        !ops->reserved_vram_end)
        return false;

    r128_lock();
    if (g_r128_2d && g_r128_2d != ops) {
        r128_unlock();
        return false;
    }
    g_r128_2d = ops;
    r128_unlock();
    kprintf("[R128:ENGINE] coordinador 2D registrado: %s\n", ops->name);
    return true;
}

void r128_engine_unregister_2d(const r128_engine_2d_ops_t *ops) {
    r128_lock();
    if (g_r128_2d == ops) {
        if (g_r128_context == R128_ENGINE_CONTEXT_2D)
            (void)g_r128_2d->wait_2d_idle();
        g_r128_2d = NULL;
        g_r128_context = R128_ENGINE_CONTEXT_NONE;
    }
    r128_unlock();
}

bool r128_engine_acquire_2d(void) {
    r128_lock();
    if (!g_r128_2d) {
        r128_unlock();
        return false;
    }
    if (g_r128_context != R128_ENGINE_CONTEXT_2D) {
        uint64_t started = perfmon_scope_begin();
        bool entered = g_r128_2d->enter_2d();
        perfmon_scope_end(PERF_SCOPE_ATI_SWITCH_2D, started);
        if (!entered) {
            r128_unlock();
            return false;
        }
        g_r128_context = R128_ENGINE_CONTEXT_2D;
        g_r128_switch_2d++;
        perfmon_scope_note(PERF_SCOPE_ATI_SWITCH_2D, 0U, 0U, 0U);
    }
    return true;
}

bool r128_engine_acquire_3d(void) {
    r128_lock();
    if (g_r128_context == R128_ENGINE_CONTEXT_2D && g_r128_2d &&
        !g_r128_2d->wait_2d_idle()) {
        r128_unlock();
        return false;
    }
    if (g_r128_context != R128_ENGINE_CONTEXT_3D) {
        g_r128_context = R128_ENGINE_CONTEXT_3D;
        g_r128_switch_3d++;
        perfmon_scope_note(PERF_SCOPE_ATI_SWITCH_3D, 0U, 0U, 0U);
    }
    return true;
}

void r128_engine_release_2d(void) {
    r128_unlock();
}

void r128_engine_release_3d(void) {
    r128_unlock();
}

bool r128_engine_present_vram32(uint32_t source_offset,
                                uint32_t source_pitch_bytes,
                                int src_x, int src_y,
                                int dst_x, int dst_y,
                                int width, int height) {
    bool ok;

    if (width <= 0 || height <= 0 || !source_pitch_bytes)
        return false;
    if (!r128_engine_acquire_2d()) {
        g_r128_present_fail++;
        return false;
    }
    ok = g_r128_2d->blit_vram32(source_offset, source_pitch_bytes,
                                src_x, src_y, dst_x, dst_y,
                                width, height);
    if (ok) ok = g_r128_2d->wait_2d_idle();
    if (ok) g_r128_present_hw++;
    else g_r128_present_fail++;
    r128_engine_release_2d();
    return ok;
}

uint32_t r128_engine_reserved_vram_end(void) {
    const r128_engine_2d_ops_t *ops = g_r128_2d;
    return ops && ops->reserved_vram_end ? ops->reserved_vram_end() : 0U;
}

r128_engine_context_t r128_engine_context(void) {
    return g_r128_context;
}

void r128_engine_report(const char *reason) {
    const char *context = "none";
    if (g_r128_context == R128_ENGINE_CONTEXT_2D) context = "2D";
    else if (g_r128_context == R128_ENGINE_CONTEXT_3D) context = "3D";
    kprintf("[R128:ENGINE] %s context=%s switch2D=%u switch3D=%u "
            "lockwait=%u presentHW=%u presentFail=%u\n",
            reason ? reason : "estado", context,
            g_r128_switch_2d, g_r128_switch_3d, g_r128_lock.contentions,
            g_r128_present_hw, g_r128_present_fail);
}
