// M15.14 (DKB): nn::hid::GetTouchScreenStates<16>, the batched touch read ov_input.c leaves to the SDK.
//
// Observed after M15.13: access violation reading 0x18 in nn::hid::detail::GetTouchScreenStates<16>, from
// nn::hid::GetTouchScreenStates<16> (sdk+0x6d6804) at main+0x293c3e4. ov_input.c answers InitializeTouchScreen and
// GetTouchScreenState<1> (no touch screen on the host), so the SDK's hid shared memory never exists. The batched
// read is answered the same way: one sample, zero touches.
#include <stdint.h>
#include <string.h>

#include "host.h"

#define TOUCH_STATE16_SIZE (0x10 + 16 * 0x28)   // int64 sampling number, int32 count, int32 reserved, 16 TouchStates

// int GetTouchScreenStates<16>(TouchScreenState<16> *out, int count)
static void dkb_touch_states16(Cpu *c) {
    static volatile int64_t sampling;
    uint8_t *out = arg_p(c, 0);
    const int32_t count = (int32_t)arg_x(c, 1);
    if (!out || count < 0) rt_fatal("DKB-TOUCH-EXT: GetTouchScreenStates<16>(%p, %d)", (void *)out, count);
    if (!count) { ret_x(c, 0); return; }
    memset(out, 0, TOUCH_STATE16_SIZE);
    int64_t n = __atomic_add_fetch(&sampling, 1, __ATOMIC_RELAXED);
    memcpy(out, &n, 8);
    ret_x(c, 1);
}

HOST_OVERRIDE("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi", dkb_touch_states16);
