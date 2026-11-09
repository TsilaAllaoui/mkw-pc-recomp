// DKB runtime target: the graphics-system check and the camera context DKB's camera layer opens at startup.
//
// main+0xcdebc0 first calls nn::gs::sdk::CheckGraphicsSystemInitialized("...", 0x17), which aborts unless
// nn::gs::sdk::BootGraphicsSystem ran (flag sdk+0xedb828). On this host the graphics system is the host NVN layer,
// which replaces everything BootGraphicsSystem brings up (gsv, gpu, vi2 sessions), so the SDK boot never runs and the
// check is answered as what it asks: the graphics system is up.
//
// It then calls nn::camera::sdk::CreateCameraContext(gsdrv::UniqueNativeHandle *), whose SDK form asks the graphics
// server for a camera context session (Eglantine NX2_NOTES row 127: gsv 601, 602). DKB wraps the handle in its own
// CMIF client (vtable main+0x352b3c0) and sends command 3 with a 16-byte name ("Default") and a u64 reply, stored at
// main+0x378ec98; success marks its camera layer initialized, and the caller (main+0xcdc344) ignores the result. The
// host has no camera: the session answers command 3 with 0, as Eglantine's camera stub session (after which 1.8.0
// plays) and Hookshot's generic handle do. main+0xcdc560 then sends command 21 with that u64 (client main+0x1c6de40)
// and attaches the copy handle of the reply as a readable system event (aborting on failure): an event that never
// fires, as nothing about a camera ever changes here. Once title loading starts it sends command 29 every frame with
// a 56-byte picture-quality/config record (the first u32 is 0x180, the width of DKB's 384x216 camera images). The
// public Eglantine Switch 2 implementation identifies the same request and returns empty success; this offline host
// does likewise. Any other command stops with its number (ipc_unimplemented).
#include <stdint.h>
#include <string.h>

#include "host.h"
#include "ipc.h"

static volatile uint32_t dkb_camera_event;   // writable end, shared by every context; never signalled

static void dkb_camera_context_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 3: {   // (name[16]) -> u64
        char name[17] = {0};
        memcpy(name, ipc_arg(rq, 0, 16), 16);
        uint64_t v = 0;
        rt_log("camera: context command 3 \"%s\" -> 0 (no camera on this host)", name);
        IPC_OUT(rs, v);
        return;
    }
    case 21: {   // (u64) -> copy handle: readable event
        uint32_t ev = __atomic_load_n(&dkb_camera_event, __ATOMIC_ACQUIRE);
        if (!ev) {
            uint32_t made = kern_event_new(), expected = 0;
            ev = __atomic_compare_exchange_n(&dkb_camera_event, &expected, made, 0, __ATOMIC_ACQ_REL,
                                             __ATOMIC_ACQUIRE) ? made : expected;
        }
        rs->copy[rs->ncopy++] = kern_event_readable(ev);
        rt_log("camera: context command 21 (%#llx) -> an event that never fires",
               (unsigned long long)IPC_ARG(rq, uint64_t, 0));
        return;
    }
    case 29: {   // (56-byte camera config) -> void; polled every frame
        const uint8_t *config = ipc_arg(rq, 0, 56);
        uint32_t image_width;
        static volatile uint32_t logged;
        memcpy(&image_width, config, sizeof(image_width));
        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_ACQ_REL)) {
            rt_log("camera: context command 29 width %u -> success (no camera on this host)", image_width);
        }
        return;
    }
    default:
        ipc_unimplemented(s, rq);
    }
}

// void nn::gs::sdk::CheckGraphicsSystemInitialized(const char *who, size_t length)
static void dkb_gs_check_initialized(Cpu *c) { (void)c; }

// Result nn::camera::sdk::CreateCameraContext(gsdrv::UniqueNativeHandle *out)
static void dkb_camera_create_context(Cpu *c) {
    uint32_t *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::camera::sdk::CreateCameraContext: null handle");
    *out = ipc_session_new("camera context", dkb_camera_context_handler, NULL, NULL);
    rt_log("camera: CreateCameraContext -> session %#x (no camera on this host)", *out);
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn2gs3sdk30CheckGraphicsSystemInitializedEPKcm", dkb_gs_check_initialized);
HOST_OVERRIDE("_ZN2nn6camera3sdk19CreateCameraContextEPNS_5gsdrv18UniqueNativeHandleE", dkb_camera_create_context);
