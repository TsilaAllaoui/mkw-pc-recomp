// DKB_HID_JCMS_M14_2: the Switch 2 HID JCMS service subset used during startup.
#pragma once

#include "ipc.h"

void hid_server_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response);
