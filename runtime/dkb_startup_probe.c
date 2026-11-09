/* DKB M14 guest-startup callback-state snapshot.
 * Read-only; does not synthesize a callback, skip any guest operation,
 * alter any register, or claim a frame. SHA-pinned by the installer.
 */
#include <stdint.h>
#include "host.h"

#define DKB_GLOBAL_PTR_SLOT 0x70036d3a48ULL

static int dkb_guest_word(uint64_t address, uint64_t *value) {
    RtMemoryInfo info;
    if (!address || (address & 7u) || address >= RT_ADDRESS_LIMIT ||
        sizeof(uint64_t) > RT_ADDRESS_LIMIT - address ||
        mem_query(address, &info) != 0 || !(info.perm & 1u) ||
        info.base > address || info.size > RT_ADDRESS_LIMIT - info.base ||
        sizeof(uint64_t) > info.base + info.size - address)
        return 0;
    *value = LD64(address);
    return 1;
}

static uint64_t dkb_probe_relative(uint64_t base, uint64_t offset, int *ok) {
    uint64_t value = 0;
    *ok = base && base <= UINT64_MAX - offset &&
          dkb_guest_word(base + offset, &value);
    return value;
}

void dkb_startup_probe_snapshot(Cpu *c) {
    static uint32_t calls;
    if (__atomic_fetch_add(&calls, 1u, __ATOMIC_RELAXED) >= 24u)
        return;
    uint64_t global_slot = 0, global_object = 0, callback20 = 0;
    uint64_t fallback28 = 0, context38 = 0, context40 = 0;
    int slot_ok = dkb_guest_word(DKB_GLOBAL_PTR_SLOT, &global_slot);
    int object_ok = slot_ok && dkb_guest_word(global_slot, &global_object);
    int cb_ok = 0, fallback_ok = 0, c38_ok = 0, c40_ok = 0;
    if (object_ok && global_object) {
        callback20 = dkb_probe_relative(global_object, 0x20, &cb_ok);
        fallback28 = dkb_probe_relative(global_object, 0x28, &fallback_ok);
    }
    context38 = dkb_probe_relative(c->x[0], 0x38, &c38_ok);
    context40 = dkb_probe_relative(c->x[0], 0x40, &c40_ok);
    rt_log("DKB startup probe main+0x69bfd0: this=%#llx lr=%#llx global_slot=%#llx slot_readable=%d global_object=%#llx object_readable=%d callback20=%#llx callback_readable=%d fallback28=%#llx fallback_readable=%d context38=%#llx context38_readable=%d context40=%#llx context40_readable=%d",
           (unsigned long long)c->x[0], (unsigned long long)c->x[30],
           (unsigned long long)global_slot, slot_ok, (unsigned long long)global_object,
           object_ok, (unsigned long long)callback20, cb_ok,
           (unsigned long long)fallback28, fallback_ok,
           (unsigned long long)context38, c38_ok,
           (unsigned long long)context40, c40_ok);
    if (object_ok && global_object == 0)
        rt_log("DKB startup probe WARNING: global callback/state object NULL before main+0x69c020 read [object+0x20]; no guest state changed");
    else if (slot_ok && !object_ok)
        rt_log("DKB startup probe WARNING: global callback/state slot unreadable; cannot diagnose without later trace");
}
