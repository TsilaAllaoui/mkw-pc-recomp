// NVN proc table. Every name the game's proc loader asks for gets a synthetic host
// address through hostfn_register before hostfn_seal; nvnBootstrapLoader and nvnDeviceGetProcAddress hand them out.
// nvnp_<name> is the implementation of one proc: the weak definitions here stop with the proc's name, nvn_api_*.c
// and nvn_ext.c define the strong ones. The name list is runtime/nvn_procs.inc (tools/gen_nvn_procs.py).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "cfg.h"
#include "platform/platform.h"

// The strong nvnp_nvnDeviceGetProcAddress below replaces the stand-in generated for the same row.
#define nvnp_nvnDeviceGetProcAddress nvnp_stub_nvnDeviceGetProcAddress

#define NVN_PROC(n) void nvnp_##n(Cpu *c);
#include "nvn_procs.inc"
#undef NVN_PROC

#define NVN_PROC(n)                                                                                                  \
    __attribute__((weak)) void nvnp_##n(Cpu *c) {                                                                     \
        (void)c;                                                                                                     \
        rt_fatal("NVN " #n " is not implemented");                                                                   \
    }
#include "nvn_procs.inc"
#undef NVN_PROC
#undef nvnp_nvnDeviceGetProcAddress
void nvnp_nvnDeviceGetProcAddress(Cpu *c);
/* DKB_GRAPHICS_MILESTONES_M12: opt-in reporting of FIRST invocation only; no shader or API state modified. */
static void dkb_gfx_nvnDeviceInitialize(Cpu *c) {
    static volatile int32_t first;
    int log_this = !__atomic_exchange_n(&first, 1, __ATOMIC_RELAXED);
    const char *flag = log_this ? cfg_get("DKB_GFX_TRACE") : NULL;
    log_this = log_this && flag && !strcmp(flag, "1");
    if (log_this)
        rt_log("DKB GFX milestone DEVICE_INIT CALL nvnDeviceInitialize x0=%#llx x1=%#llx x2=%#llx",
               (unsigned long long)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)c->x[2]);
    nvnp_nvnDeviceInitialize(c);
    if (log_this)
        rt_log("DKB GFX milestone DEVICE_INIT RETURN nvnDeviceInitialize x0_after=%#llx (not proof of a frame)",
               (unsigned long long)c->x[0]);
}
static void dkb_gfx_nvnQueueInitialize(Cpu *c) {
    static volatile int32_t first;
    int log_this = !__atomic_exchange_n(&first, 1, __ATOMIC_RELAXED);
    const char *flag = log_this ? cfg_get("DKB_GFX_TRACE") : NULL;
    log_this = log_this && flag && !strcmp(flag, "1");
    if (log_this)
        rt_log("DKB GFX milestone QUEUE_INIT CALL nvnQueueInitialize x0=%#llx x1=%#llx x2=%#llx",
               (unsigned long long)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)c->x[2]);
    nvnp_nvnQueueInitialize(c);
    if (log_this)
        rt_log("DKB GFX milestone QUEUE_INIT RETURN nvnQueueInitialize x0_after=%#llx (not proof of a frame)",
               (unsigned long long)c->x[0]);
}
static void dkb_gfx_nvnWindowInitialize(Cpu *c) {
    static volatile int32_t first;
    int log_this = !__atomic_exchange_n(&first, 1, __ATOMIC_RELAXED);
    const char *flag = log_this ? cfg_get("DKB_GFX_TRACE") : NULL;
    log_this = log_this && flag && !strcmp(flag, "1");
    if (log_this)
        rt_log("DKB GFX milestone WINDOW_INIT CALL nvnWindowInitialize x0=%#llx x1=%#llx x2=%#llx",
               (unsigned long long)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)c->x[2]);
    nvnp_nvnWindowInitialize(c);
    if (log_this)
        rt_log("DKB GFX milestone WINDOW_INIT RETURN nvnWindowInitialize x0_after=%#llx (not proof of a frame)",
               (unsigned long long)c->x[0]);
}
static void dkb_gfx_nvnProgramSetShaders(Cpu *c) {
    static volatile int32_t first;
    int log_this = !__atomic_exchange_n(&first, 1, __ATOMIC_RELAXED);
    const char *flag = log_this ? cfg_get("DKB_GFX_TRACE") : NULL;
    log_this = log_this && flag && !strcmp(flag, "1");
    if (log_this)
        rt_log("DKB GFX milestone SHADER_SET CALL nvnProgramSetShaders x0=%#llx x1=%#llx x2=%#llx",
               (unsigned long long)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)c->x[2]);
    nvnp_nvnProgramSetShaders(c);
    if (log_this)
        rt_log("DKB GFX milestone SHADER_SET RETURN nvnProgramSetShaders x0_after=%#llx (not proof of a frame)",
               (unsigned long long)c->x[0]);
}
static void dkb_gfx_nvnProgramSetShadersExt(Cpu *c) {
    static volatile int32_t first;
    int log_this = !__atomic_exchange_n(&first, 1, __ATOMIC_RELAXED);
    const char *flag = log_this ? cfg_get("DKB_GFX_TRACE") : NULL;
    log_this = log_this && flag && !strcmp(flag, "1");
    if (log_this)
        rt_log("DKB GFX milestone SHADER_SET_EXT CALL nvnProgramSetShadersExt x0=%#llx x1=%#llx x2=%#llx",
               (unsigned long long)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)c->x[2]);
    nvnp_nvnProgramSetShadersExt(c);
    if (log_this)
        rt_log("DKB GFX milestone SHADER_SET_EXT RETURN nvnProgramSetShadersExt x0_after=%#llx (not proof of a frame)",
               (unsigned long long)c->x[0]);
}
static void dkb_gfx_nvnCommandBufferBindProgram(Cpu *c) {
    static volatile int32_t first;
    int log_this = !__atomic_exchange_n(&first, 1, __ATOMIC_RELAXED);
    const char *flag = log_this ? cfg_get("DKB_GFX_TRACE") : NULL;
    log_this = log_this && flag && !strcmp(flag, "1");
    if (log_this)
        rt_log("DKB GFX milestone PROGRAM_BIND CALL nvnCommandBufferBindProgram x0=%#llx x1=%#llx x2=%#llx",
               (unsigned long long)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)c->x[2]);
    nvnp_nvnCommandBufferBindProgram(c);
    if (log_this)
        rt_log("DKB GFX milestone PROGRAM_BIND RETURN nvnCommandBufferBindProgram x0_after=%#llx (not proof of a frame)",
               (unsigned long long)c->x[0]);
}
static void dkb_gfx_nvnWindowAcquireTexture(Cpu *c) {
    static volatile int32_t first;
    int log_this = !__atomic_exchange_n(&first, 1, __ATOMIC_RELAXED);
    const char *flag = log_this ? cfg_get("DKB_GFX_TRACE") : NULL;
    log_this = log_this && flag && !strcmp(flag, "1");
    if (log_this)
        rt_log("DKB GFX milestone SWAPCHAIN_ACQUIRE CALL nvnWindowAcquireTexture x0=%#llx x1=%#llx x2=%#llx",
               (unsigned long long)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)c->x[2]);
    nvnp_nvnWindowAcquireTexture(c);
    if (log_this)
        rt_log("DKB GFX milestone SWAPCHAIN_ACQUIRE RETURN nvnWindowAcquireTexture x0_after=%#llx (not proof of a frame)",
               (unsigned long long)c->x[0]);
}
static void dkb_gfx_nvnCommandBufferDrawArrays(Cpu *c) {
    static volatile int32_t first;
    int log_this = !__atomic_exchange_n(&first, 1, __ATOMIC_RELAXED);
    const char *flag = log_this ? cfg_get("DKB_GFX_TRACE") : NULL;
    log_this = log_this && flag && !strcmp(flag, "1");
    if (log_this)
        rt_log("DKB GFX milestone DRAW_ARRAYS CALL nvnCommandBufferDrawArrays x0=%#llx x1=%#llx x2=%#llx",
               (unsigned long long)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)c->x[2]);
    nvnp_nvnCommandBufferDrawArrays(c);
    if (log_this)
        rt_log("DKB GFX milestone DRAW_ARRAYS RETURN nvnCommandBufferDrawArrays x0_after=%#llx (not proof of a frame)",
               (unsigned long long)c->x[0]);
}
static void dkb_gfx_nvnCommandBufferDrawElements(Cpu *c) {
    static volatile int32_t first;
    int log_this = !__atomic_exchange_n(&first, 1, __ATOMIC_RELAXED);
    const char *flag = log_this ? cfg_get("DKB_GFX_TRACE") : NULL;
    log_this = log_this && flag && !strcmp(flag, "1");
    if (log_this)
        rt_log("DKB GFX milestone DRAW_ELEMENTS CALL nvnCommandBufferDrawElements x0=%#llx x1=%#llx x2=%#llx",
               (unsigned long long)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)c->x[2]);
    nvnp_nvnCommandBufferDrawElements(c);
    if (log_this)
        rt_log("DKB GFX milestone DRAW_ELEMENTS RETURN nvnCommandBufferDrawElements x0_after=%#llx (not proof of a frame)",
               (unsigned long long)c->x[0]);
}
static void dkb_gfx_nvnCommandBufferDispatchCompute(Cpu *c) {
    static volatile int32_t first;
    int log_this = !__atomic_exchange_n(&first, 1, __ATOMIC_RELAXED);
    const char *flag = log_this ? cfg_get("DKB_GFX_TRACE") : NULL;
    log_this = log_this && flag && !strcmp(flag, "1");
    if (log_this)
        rt_log("DKB GFX milestone DISPATCH_COMPUTE CALL nvnCommandBufferDispatchCompute x0=%#llx x1=%#llx x2=%#llx",
               (unsigned long long)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)c->x[2]);
    nvnp_nvnCommandBufferDispatchCompute(c);
    if (log_this)
        rt_log("DKB GFX milestone DISPATCH_COMPUTE RETURN nvnCommandBufferDispatchCompute x0_after=%#llx (not proof of a frame)",
               (unsigned long long)c->x[0]);
}
static void dkb_gfx_nvnQueueSubmitCommands(Cpu *c) {
    static volatile int32_t first;
    int log_this = !__atomic_exchange_n(&first, 1, __ATOMIC_RELAXED);
    const char *flag = log_this ? cfg_get("DKB_GFX_TRACE") : NULL;
    log_this = log_this && flag && !strcmp(flag, "1");
    if (log_this)
        rt_log("DKB GFX milestone COMMAND_SUBMIT CALL nvnQueueSubmitCommands x0=%#llx x1=%#llx x2=%#llx",
               (unsigned long long)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)c->x[2]);
    nvnp_nvnQueueSubmitCommands(c);
    if (log_this)
        rt_log("DKB GFX milestone COMMAND_SUBMIT RETURN nvnQueueSubmitCommands x0_after=%#llx (not proof of a frame)",
               (unsigned long long)c->x[0]);
}
static void dkb_gfx_nvnQueuePresentTexture(Cpu *c) {
    static volatile int32_t first;
    int log_this = !__atomic_exchange_n(&first, 1, __ATOMIC_RELAXED);
    const char *flag = log_this ? cfg_get("DKB_GFX_TRACE") : NULL;
    log_this = log_this && flag && !strcmp(flag, "1");
    if (log_this)
        rt_log("DKB GFX milestone PRESENT_REQUEST CALL nvnQueuePresentTexture x0=%#llx x1=%#llx x2=%#llx",
               (unsigned long long)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)c->x[2]);
    nvnp_nvnQueuePresentTexture(c);
    if (log_this)
        rt_log("DKB GFX milestone PRESENT_REQUEST RETURN nvnQueuePresentTexture x0_after=%#llx (not proof of a frame)",
               (unsigned long long)c->x[0]);
}
/* Weak NVN stubs remain fatal; unknown procedure names still return NULL. */
static void (*dkb_gfx_proc(const char *name, void (*original)(Cpu *)))(Cpu *) {

    if (!strcmp(name, "nvnDeviceInitialize")) return dkb_gfx_nvnDeviceInitialize;
    if (!strcmp(name, "nvnQueueInitialize")) return dkb_gfx_nvnQueueInitialize;
    if (!strcmp(name, "nvnWindowInitialize")) return dkb_gfx_nvnWindowInitialize;
    if (!strcmp(name, "nvnProgramSetShaders")) return dkb_gfx_nvnProgramSetShaders;
    if (!strcmp(name, "nvnProgramSetShadersExt")) return dkb_gfx_nvnProgramSetShadersExt;
    if (!strcmp(name, "nvnCommandBufferBindProgram")) return dkb_gfx_nvnCommandBufferBindProgram;
    if (!strcmp(name, "nvnWindowAcquireTexture")) return dkb_gfx_nvnWindowAcquireTexture;
    if (!strcmp(name, "nvnCommandBufferDrawArrays")) return dkb_gfx_nvnCommandBufferDrawArrays;
    if (!strcmp(name, "nvnCommandBufferDrawElements")) return dkb_gfx_nvnCommandBufferDrawElements;
    if (!strcmp(name, "nvnCommandBufferDispatchCompute")) return dkb_gfx_nvnCommandBufferDispatchCompute;
    if (!strcmp(name, "nvnQueueSubmitCommands")) return dkb_gfx_nvnQueueSubmitCommands;
    if (!strcmp(name, "nvnQueuePresentTexture")) return dkb_gfx_nvnQueuePresentTexture;
    return original;
}


typedef struct { const char *name; uint64_t addr; } Proc;

#define NVN_PROC(n) +1
enum { PROC_COUNT = 0
#include "nvn_procs.inc"
};
#undef NVN_PROC

static Proc procs[PROC_COUNT];
static int nprocs;

static int by_name(const void *a, const void *b) { return strcmp(((const Proc *)a)->name, ((const Proc *)b)->name); }

void nvn_procs_register(void) {
    if (nprocs) return;
#define NVN_PROC(n) procs[nprocs++] = (Proc){#n, hostfn_register("nvn:" #n, dkb_gfx_proc(#n, nvnp_##n))};
#include "nvn_procs.inc"
#undef NVN_PROC
    qsort(procs, (size_t)nprocs, sizeof *procs, by_name);
}

uint64_t nvn_proc_address(const char *name) {
    if (!name) return 0;
    const Proc *p = bsearch(&(Proc){name, 0}, procs, (size_t)nprocs, sizeof *procs, by_name);
    if (p) return p->addr;
    // Unknown names get NULL, as on hardware; each distinct one is logged once.
    static PlatMutex lock = PLAT_MUTEX_INIT;
    static char seen[32][96];
    static int nseen;
    plat_mutex_lock(&lock);
    int fresh = 1;
    for (int i = 0; i < nseen; i++)
        if (!strncmp(seen[i], name, sizeof seen[i] - 1)) fresh = 0;
    if (fresh && nseen < 32) {
        strncpy(seen[nseen], name, sizeof seen[0] - 1);
        nseen++;
    } else {
        fresh = 0;
    }
    plat_mutex_unlock(&lock);
    if (fresh) rt_log("nvn: no such proc \"%s\", returning NULL", name);
    return 0;
}

// PFNNVNGENERICFUNC nvnDeviceGetProcAddress(const NVNdevice *device, const char *name)
void nvnp_nvnDeviceGetProcAddress(Cpu *c) {
    const char *name = arg_p(c, 1);
    ret_x(c, nvn_proc_address(name));
}
