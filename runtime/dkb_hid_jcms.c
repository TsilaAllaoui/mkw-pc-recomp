/*
 * DKB_HID_JCMS_M14_2: minimal Switch 2 IHidServer support for Joy-Con mouse (JCMS).
 *
 * DKB runtime target calls nn::hid::InitializeJcms during controller
 * startup.  Its SDK sends command 3110 with the application resource user ID,
 * a 0x4000-byte size and one copied transfer-memory handle.  The public input
 * overrides still provide the host controller and touch state; this service
 * only gives the unchanged SDK a valid, empty JCMS shared-memory area.  Empty
 * rings mean that no Joy-Con mouse is connected on the host.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "ipc.h"
#include "dkb_hid_jcms.h"

enum {
    HID_CMD_INITIALIZE_JCMS = 3110,
    HID_CMD_FINALIZE_JCMS = 3111,
    HID_CMD_START_JCMS = 3112,
    HID_CMD_STOP_JCMS = 3113,
    HID_JCMS_SHARED_MEMORY_SIZE = 0x4000,
};

static void require_request(int condition, const IpcSession *session,
                            const IpcRequest *request, const char *message) {
    if (condition) return;
    fflush(stdout);
    ipc_backtrace();
    rt_fatal("ipc: %s command %u: %s", session->name, request->cmd, message);
}

void hid_server_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    (void)response;
    switch (request->cmd) {
    case HID_CMD_INITIALIZE_JCMS: {
        require_request(request->has_pid, session, request, "missing process id");
        require_request(request->ncopy == 1, session, request,
                        "expected one copied transfer-memory handle");
        uint64_t aruid = IPC_ARG(request, uint64_t, 0);
        uint64_t size = IPC_ARG(request, uint64_t, 8);
        IpcTransferMemory *memory = kern_handle_get(request->copy[0], KERN_TMEM);
        require_request(memory != NULL, session, request, "copied handle is not transfer memory");
        require_request(size == HID_JCMS_SHARED_MEMORY_SIZE, session, request,
                        "unexpected JCMS shared-memory size");
        require_request(memory->size >= size, session, request,
                        "transfer memory is smaller than the stated JCMS size");
        memset(ipc_guest(memory->addr, size), 0, (size_t)size);
        rt_log("hid: initialized empty JCMS shared memory %#llx+%#llx (aruid %#llx)",
               (unsigned long long)memory->addr, (unsigned long long)size,
               (unsigned long long)aruid);
        return;
    }
    case HID_CMD_FINALIZE_JCMS:
        (void)IPC_ARG(request, uint64_t, 0);   // application resource user ID
        rt_log("hid: finalized JCMS shared memory");
        return;
    case HID_CMD_START_JCMS:
    case HID_CMD_STOP_JCMS:
        (void)IPC_ARG(request, uint64_t, 0);   // application resource user ID
        (void)IPC_ARG(request, uint32_t, 8);   // JcmsHandle
        // No Joy-Con mouse is connected, so starting or stopping its empty ring
        // has no host-side work.  The SDK continues to read zero available states.
        return;
    default:
        ipc_unimplemented(session, request);
    }
}
