// M15.10 (DKB): a read-only look at the guest objects behind a write to the null page.
//
// Observed after M15.9: "access violation writing address 0" in main+0x86d5e0, a UTF-16 character table whose
// 200-entry buffer main+0x86d310 never stored: the heap's virtual alloc (vtable+0x30) returned null for 3200 bytes.
// The crash handler prints registers only; this first-chance handler logs the objects in x0, x19, x20 and x21 (and
// their vtables, and a free/used summary of any DKB expanded heap among them and its parent) once, then lets the
// exception continue to the crash handler unchanged. Writes above the null page
// (the memory mirror's write faults) never reach the dump.
#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "platform/platform.h"

static volatile LONG dkb_null_probe_done;

// DKB's expanded heap (vtable main+0x363a300, alloc main+0x3f24c0): name at +0x20, start +0x28, size +0x30, parent
// +0x38; free list node at +0xd0 (count +0xe0, node offset +0xe4), used list at +0xe8 (+0xf8, +0xfc); blocks have a
// 0x20-byte header with their size at +0x18.
#define DKB_EXP_HEAP_VTABLE 0x363a300u

static void dkb_heap_summary(uint64_t h);
static int dkb_heap_depth;

static int dkb_is_exp_heap(uint64_t h) {
    return h && !(h & 7) && plat_mem_readable(h, 0x100) && LD64(h) == ldr_module(0)->base + DKB_EXP_HEAP_VTABLE;
}

// The used block of heap h holding address p, with its first words: what owns a list that reaches into the tail.
static void dkb_heap_owner_of(uint64_t h, uint64_t p) {
    uint64_t head = h + 0xe8, n = 0;
    int32_t off = (int32_t)LD32(head + 0x14);
    for (uint64_t node = LD64(head + 8); node != head && n < 4000000; n++, node = LD64(node + 8)) {
        if (!node || (node & 7) || !plat_mem_readable(node - off, 0x20)) return;
        uint64_t blk = node - off, data = blk + 0x20, size = LD64(blk + 0x18);
        if (p < data || p >= data + size) continue;
        char d[160], v[160];
        uint64_t w0 = LD64(data);
        rt_log("DKB null-write probe:       %#llx is block %#llx+%#llx (size %#llx): %016llx %016llx %016llx %016llx "
               "(%s) at +off: %016llx (%s)", (unsigned long long)p, (unsigned long long)data,
               (unsigned long long)(p - data), (unsigned long long)size, (unsigned long long)w0,
               (unsigned long long)LD64(data + 8), (unsigned long long)LD64(data + 16),
               (unsigned long long)LD64(data + 24), ldr_describe(w0, d, sizeof d),
               (unsigned long long)LD64(p & ~7ull), ldr_describe(LD64(p & ~7ull), v, sizeof v));
        return;
    }
    rt_log("DKB null-write probe:       %#llx is in no used block of heap %#llx", (unsigned long long)p,
           (unsigned long long)h);
}

static void dkb_heap_list(const char *what, uint64_t h, uint64_t head_off) {
    uint64_t head = h + head_off, top[8] = {0}, top_size[8] = {0}, last[6] = {0}, total = 0, n = 0;
    int32_t count = (int32_t)LD32(head + 0x10), off = (int32_t)LD32(head + 0x14);
    for (uint64_t node = LD64(head + 8); node != head && n < 4000000; n++) {
        if (!node || (node & 7) || !plat_mem_readable(node - off, 0x20)) {
            rt_log("DKB null-write probe:     %s list broken at node %#llx after %llu blocks", what,
                   (unsigned long long)node, (unsigned long long)n);
            return;
        }
        uint64_t blk = node - off, size = LD64(blk + 0x18);
        total += size;
        for (int i = 0; i < 8; i++)
            if (size > top_size[i]) {
                for (int j = 7; j > i; j--) top[j] = top[j - 1], top_size[j] = top_size[j - 1];
                top[i] = blk, top_size[i] = size;
                break;
            }
        for (int i = 0; i < 6; i++)
            if (blk > last[i]) {
                for (int j = 5; j > i; j--) last[j] = last[j - 1];
                last[i] = blk;
                break;
            }
        node = LD64(node + 8);
    }
    // adjust() can only give back a free block at the heap's end: the highest blocks show what holds the tail.
    if (head_off == 0xe8)
        for (int i = 0; i < 6 && last[i]; i++)
            for (int k = 0; k < 2; k++) {
                uint64_t p = LD64(last[i] + 0x20 + 8 * k);
                if (p >= last[5] && p < last[0] + 0x60) continue;
                dkb_heap_owner_of(h, p);
            }
    for (int i = 0; i < 6 && last[i]; i++) {
        char d[160];
        uint64_t data = last[i] + 0x20;
        rt_log("DKB null-write probe:       %s tail block %#llx size %#llx flags %04x pad %04x: %016llx %016llx "
               "%016llx %016llx (%s)", what, (unsigned long long)last[i], (unsigned long long)LD64(last[i] + 0x18),
               LD16(last[i] + 0x10), LD16(last[i] + 0x12), (unsigned long long)LD64(data),
               (unsigned long long)LD64(data + 8), (unsigned long long)LD64(data + 16),
               (unsigned long long)LD64(data + 24), ldr_describe(LD64(data), d, sizeof d));
    }
    rt_log("DKB null-write probe:     %s: %llu blocks (count field %d), %llu bytes", what, (unsigned long long)n,
           count, (unsigned long long)total);
    for (int i = 0; i < 8 && top_size[i]; i++) {
        char d[160], name[48] = "";
        uint64_t data = top[i] + 0x20, w0 = LD64(data), np = plat_mem_readable(data, 0x40) ? LD64(data + 0x20) : 0;
        if (dkb_is_exp_heap(data) && np && plat_mem_readable(np, sizeof name)) {
            memcpy(name, (const void *)(uintptr_t)np, sizeof name - 1);
            name[sizeof name - 1] = 0;
        }
        rt_log("DKB null-write probe:       %s block %#llx size %#llx: %016llx %016llx (%s)%s%s%s", what,
               (unsigned long long)top[i], (unsigned long long)top_size[i], (unsigned long long)w0,
               (unsigned long long)LD64(data + 8), ldr_describe(w0, d, sizeof d),
               name[0] ? " expanded heap \"" : "", name, name[0] ? "\"" : "");
    }
    for (int i = 0; i < 8 && top_size[i]; i++)
        if (dkb_is_exp_heap(top[i] + 0x20)) dkb_heap_summary(top[i] + 0x20);
}

static void dkb_heap_summary(uint64_t h) {
    if (dkb_heap_depth >= 3) return;
    dkb_heap_depth++;
    char name[64] = "?";
    uint64_t np = LD64(h + 0x20);
    if (np && plat_mem_readable(np, sizeof name)) {
        memcpy(name, (const void *)(uintptr_t)np, sizeof name - 1);
        name[sizeof name - 1] = 0;
    }
    rt_log("DKB null-write probe:   heap %#llx \"%s\" start %#llx size %#llx parent %#llx", (unsigned long long)h,
           name, (unsigned long long)LD64(h + 0x28), (unsigned long long)LD64(h + 0x30),
           (unsigned long long)LD64(h + 0x38));
    dkb_heap_list("free", h, 0xd0);
    dkb_heap_list("used", h, 0xe8);
    dkb_heap_depth--;
}

static void dkb_null_probe_dump(const char *reg, uint64_t a) {
    char d[160];
    if (!a || (a & 7) || !plat_mem_readable(a, 0x80)) {
        rt_log("DKB null-write probe: %s=%#llx unreadable", reg, (unsigned long long)a);
        return;
    }
    rt_log("DKB null-write probe: %s=%#llx vtable %s", reg, (unsigned long long)a, ldr_describe(LD64(a), d, sizeof d));
    for (int i = 0; i < 0x80; i += 0x20)
        rt_log("DKB null-write probe:   %s+%#04x %016llx %016llx %016llx %016llx", reg, i,
               (unsigned long long)LD64(a + i), (unsigned long long)LD64(a + i + 8),
               (unsigned long long)LD64(a + i + 16), (unsigned long long)LD64(a + i + 24));
    if (dkb_is_exp_heap(a)) {
        dkb_heap_summary(a);
        if (dkb_is_exp_heap(LD64(a + 0x38))) dkb_heap_summary(LD64(a + 0x38));
    }
    uint64_t vt = LD64(a);
    if (!vt || (vt & 7) || !plat_mem_readable(vt, 0x60)) return;
    for (int i = 0; i < 0x60; i += 8)
        rt_log("DKB null-write probe:   %s vtable+%#04x %s", reg, i, ldr_describe(LD64(vt + i), d, sizeof d));
}

static LONG CALLBACK dkb_null_probe(EXCEPTION_POINTERS *e) {
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 ||
        r->ExceptionInformation[0] != 1 || r->ExceptionInformation[1] >= 0x10000)
        return EXCEPTION_CONTINUE_SEARCH;
    Cpu *c = kern_cpu();
    if (!c || InterlockedExchange(&dkb_null_probe_done, 1)) return EXCEPTION_CONTINUE_SEARCH;
    rt_log("DKB null-write probe: write to %#llx on a guest thread (lr %#llx); the crash handler follows",
           (unsigned long long)r->ExceptionInformation[1], (unsigned long long)c->x[30]);
    dkb_null_probe_dump("x0", c->x[0]);
    dkb_null_probe_dump("x19", c->x[19]);
    dkb_null_probe_dump("x20", c->x[20]);
    dkb_null_probe_dump("x21", c->x[21]);
    return EXCEPTION_CONTINUE_SEARCH;
}

// DKB_HEAP_TAIL_PROBE=1: who allocates at the end of "RootRepository" before its adjust()? Observed after M15.9:
// adjust() could not shrink it (six 0x20-byte used blocks at its tail), so RootRepositoryHeap had no room for the
// RaceFont tables. The probe wraps the expanded heap's alloc slot (vtable+0x30) and logs allocations landing in the
// heap's last 64 KiB with their guest callers; the allocation itself is the original call, unchanged.
#define DKB_EXP_HEAP_ALLOC 0x3f24c0u
uint64_t hostfn_register_observer(const char *name, GuestFn fn, uint64_t original);
static uint64_t dkb_tail_probe_fn;
static volatile LONG dkb_tail_probe_logs;

static void dkb_tail_alloc_obs(Cpu *c) {
    uint64_t heap = c->x[0], size = c->x[1], lr = c->x[30], fp = c->x[29];
    int32_t align = (int32_t)c->x[2];
    uint64_t r = rt_original(c);
    if (!r || !plat_mem_readable(heap, 0x40)) return;
    uint64_t np = LD64(heap + 0x20), end = LD64(heap + 0x28) + LD64(heap + 0x30);
    if (r + 0x10000 < end || !np || !plat_mem_readable(np, 16) ||
        memcmp((const void *)(uintptr_t)np, "RootRepository", 15))
        return;
    if (InterlockedIncrement(&dkb_tail_probe_logs) > 64) return;
    char d[160];
    rt_log("DKB heap-tail probe: RootRepository alloc size %#llx align %d -> %#llx (end %#llx) lr %s",
           (unsigned long long)size, align, (unsigned long long)r, (unsigned long long)end,
           ldr_describe(lr, d, sizeof d));
    for (int n = 0; n < 8 && fp && !(fp & 7) && plat_mem_readable(fp, 16); n++) {
        uint64_t next = LD64(fp);
        rt_log("DKB heap-tail probe:   frame %d: %s", n, ldr_describe(LD64(fp + 8), d, sizeof d));
        if (next <= fp) break;
        fp = next;
    }
}

static void dkb_tail_probe_install(void *arg) {
    (void)arg;
    for (int i = 0; i < 60000; i++, plat_sleep_ms(1)) {
        if (ldr_count() < 1) continue;
        uint64_t main_base = ldr_module(0)->base, slot = main_base + DKB_EXP_HEAP_VTABLE + 0x30;
        if (!plat_mem_readable(slot, 8) || LD64(slot) != main_base + DKB_EXP_HEAP_ALLOC) continue;
        uint64_t page = slot & ~0xfffull;
        if (!plat_mem_protect(page, 0x1000, PLAT_PROT_RW)) {
            rt_log("DKB heap-tail probe: cannot make the expanded heap vtable writable");
            return;
        }
        *(volatile uint64_t *)(uintptr_t)slot = dkb_tail_probe_fn;
        plat_mem_protect(page, 0x1000, PLAT_PROT_READ);
        rt_log("DKB heap-tail probe: expanded heap alloc slot main+%#x wrapped", DKB_EXP_HEAP_VTABLE + 0x30);
        return;
    }
}

__attribute__((constructor)) static void dkb_null_probe_install(void) {
    AddVectoredExceptionHandler(1, dkb_null_probe);
    const char *e = getenv("DKB_HEAP_TAIL_PROBE");
    if (e && *e == '1') {
        dkb_tail_probe_fn = hostfn_register_observer("DKB heap-tail probe (expanded heap alloc)", dkb_tail_alloc_obs,
                                                     0x7000000000ull + DKB_EXP_HEAP_ALLOC);
        plat_thread_spawn(dkb_tail_probe_install, NULL, "dkb-tail-probe", 0);
    }
}
#endif
