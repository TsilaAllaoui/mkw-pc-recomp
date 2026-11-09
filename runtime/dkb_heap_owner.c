/*
 * DKB runtime target: guest allocation provenance and matched deletion.
 *
 * This file is installed as runtime/dkb_heap_owner.c in an isolated DKB checkout.
 * It does NOT replace the guest's allocation algorithm, modify guest pointers,
 * use host malloc for guest storage, or suppress SDK allocator assertions.
 *
 * The paired source wrapper records the return value of the ORIGINAL lifted
 * guest allocation helper. Guest custom-heap allocations are reclaimed by the
 * ORIGINAL guest heap vtable's deallocation method through rt_call(); other
 * C++ deletes continue to the original guest SDK through rt_original().
 *
 * Intentionally pinned to the user-verified DKB 1.8.0 main NSO. The installer
 * verifies its SHA256 before enabling this module.
 */
#include <stdint.h>
#include <stddef.h>
#include "host.h"
#include "platform/platform.h"

#define DKB_VTABLE 0x700363a300ULL
#define DKB_CUSTOM_FREE 0x70001a58a0ULL
/* Original DKB resource-object destructor (main+0x1a24b0) uses this
 * registry to resolve owners before freeing its nested resource data. */
#define DKB_RESOURCE_VTABLE 0x700363ac40ULL
#define DKB_RESOURCE_MANAGER_SLOT 0x70037be6a8ULL
#define DKB_GUEST_OWNER_LOOKUP 0x70001a3c60ULL
#define DKB_OWNER_CAPACITY 1048576u
#define DKB_EMPTY 0ULL
#define DKB_TOMBSTONE UINT64_MAX

struct DkbOwnerEntry {
    uint64_t pointer;
    uint64_t owner;
    uint64_t requested_size;
};

static struct DkbOwnerEntry dkb_owners[DKB_OWNER_CAPACITY];
static PlatMutex dkb_owner_mutex = PLAT_MUTEX_INIT;
static uint64_t dkb_recorded;
static uint64_t dkb_routed;
static uint64_t dkb_passthrough;
static uint64_t dkb_derived_recorded;
static uint64_t dkb_rejected_owner_records;
static uint64_t dkb_size20_recorded;
static uint64_t dkb_size20_discarded;
static uint64_t dkb_unknown_base_deletes;
static volatile int32_t dkb_owner_full;

static uint32_t dkb_hash(uint64_t pointer) {
    uint64_t value = pointer >> 3;
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return (uint32_t)value & (DKB_OWNER_CAPACITY - 1u);
}

/* Uses Horizon's live memory map to avoid dereferencing arbitrary addresses. */
static int dkb_guest_readable(uint64_t address, uint64_t count) {
    RtMemoryInfo info;
    if (!address || !count || address >= RT_ADDRESS_LIMIT ||
        count > RT_ADDRESS_LIMIT - address || mem_query(address, &info) != 0)
        return 0;
    if (!(info.perm & 1u) || address < info.base ||
        info.size > RT_ADDRESS_LIMIT - info.base)
        return 0;
    return count <= info.base + info.size - address;
}

/* M14.7: accept an inherited heap vtable only when both audited guest
 * operation slots still resolve to the exact original allocation/free pair. */
static int dkb_owner_valid(uint64_t owner) {
    if ((owner & 7u) || !dkb_guest_readable(owner, sizeof(uint64_t)))
        return 0;
    uint64_t vtable = LD64(owner);
    if (vtable == DKB_VTABLE)
        return 1;
    if ((vtable & 7u) || !dkb_guest_readable(vtable, 0x40u))
        return 0;
    return LD64(vtable + 0x30u) == 0x70003f24c0ULL &&
           LD64(vtable + 0x38u) == DKB_CUSTOM_FREE;
}

/* Called only after the ORIGINAL lifted guest allocator has returned. */
void dkb_heap_owner_record(uint64_t pointer, uint64_t owner, uint64_t size, uint64_t align) {
    if (!pointer || !owner || (pointer & 7u) || size == 0 ||
        align == 0 || (align & (align - 1u)))
        return;
    if (!dkb_owner_valid(owner)) {
        uint64_t rejected_count = __atomic_add_fetch(
            &dkb_rejected_owner_records, 1ULL, __ATOMIC_RELAXED);
        if (size == 0x20u && rejected_count <= 256u) {
            uint64_t vtable = 0;
            uint64_t allocation_method = 0;
            uint64_t deallocation_method = 0;
            if (dkb_guest_readable(owner, sizeof(uint64_t))) {
                vtable = LD64(owner);
                if (!(vtable & 7u) && dkb_guest_readable(vtable, 0x40u)) {
                    allocation_method = LD64(vtable + 0x30u);
                    deallocation_method = LD64(vtable + 0x38u);
                }
            }
            rt_log("DKB heap-owner rejected 0x20 record: ptr=%#llx owner=%#llx "
                   "vtable=%#llx alloc=%#llx free=%#llx align=%#llx",
                   (unsigned long long)pointer, (unsigned long long)owner,
                   (unsigned long long)vtable,
                   (unsigned long long)allocation_method,
                   (unsigned long long)deallocation_method,
                   (unsigned long long)align);
        }
        return;
    }
    if (!dkb_guest_readable(pointer, 1))
        return;

    if (size == 0x20u) {
        uint64_t size20_count = __atomic_add_fetch(
            &dkb_size20_recorded, 1ULL, __ATOMIC_RELAXED);
        if (size20_count <= 256u)
            rt_log("DKB heap-owner 0x20 record: ptr=%#llx owner=%#llx "
                   "vtable=%#llx align=%#llx",
                   (unsigned long long)pointer, (unsigned long long)owner,
                   (unsigned long long)LD64(owner),
                   (unsigned long long)align);
    }

    uint64_t owner_vtable = LD64(owner);
    if (owner_vtable != DKB_VTABLE) {
        uint64_t derived_count = __atomic_add_fetch(
            &dkb_derived_recorded, 1ULL, __ATOMIC_RELAXED);
        if (derived_count <= 64u)
            rt_log("DKB heap-owner derived record: ptr=%#llx owner=%#llx "
                   "vtable=%#llx size=%#llx align=%#llx",
                   (unsigned long long)pointer, (unsigned long long)owner,
                   (unsigned long long)owner_vtable,
                   (unsigned long long)size, (unsigned long long)align);
    }

    if (__atomic_load_n(&dkb_owner_full, __ATOMIC_RELAXED))
        return;
    uint32_t first = dkb_hash(pointer);
    uint32_t available = DKB_OWNER_CAPACITY;
    uint32_t vacant = DKB_OWNER_CAPACITY;
    uint64_t logged_count = 0;
    plat_mutex_lock(&dkb_owner_mutex);
    for (uint32_t i = 0; i < DKB_OWNER_CAPACITY; i++) {
        uint32_t pos = (first + i) & (DKB_OWNER_CAPACITY - 1u);
        uint64_t found = dkb_owners[pos].pointer;
        if (found == pointer) {
            available = pos;
            break;
        }
        if (found == DKB_TOMBSTONE && vacant == DKB_OWNER_CAPACITY)
            vacant = pos;
        if (found == DKB_EMPTY) {
            available = vacant == DKB_OWNER_CAPACITY ? pos : vacant;
            break;
        }
    }
    if (available < DKB_OWNER_CAPACITY) {
        dkb_owners[available] = (struct DkbOwnerEntry){pointer, owner, size};
        dkb_recorded++;
        logged_count = dkb_recorded;
    }
    plat_mutex_unlock(&dkb_owner_mutex);
    if (available == DKB_OWNER_CAPACITY) {
        /* M15.8: an unrecorded allocation is deleted by DKB's own operator delete (M15.1, M15.2), as on hardware. */
        if (!__atomic_exchange_n(&dkb_owner_full, 1, __ATOMIC_RELAXED))
            rt_log("DKB heap-owner registry full at %#llx (%u entries): further allocations are not recorded; "
                   "their deletes run DKB's own operator delete", (unsigned long long)pointer, DKB_OWNER_CAPACITY);
        return;
    }
    if (logged_count <= 24u)
        rt_log("DKB heap-owner record: ptr=%#llx owner=%#llx size=%#llx align=%#llx",
               (unsigned long long)pointer, (unsigned long long)owner,
               (unsigned long long)size, (unsigned long long)align);
}

/* Only removes the requested pointer; never assumes an address-range owner. */
static uint64_t dkb_owner_take(uint64_t pointer, uint64_t *size) {
    if (!pointer || pointer == DKB_TOMBSTONE)
        return 0;
    uint32_t first = dkb_hash(pointer);
    uint64_t owner = 0;
    plat_mutex_lock(&dkb_owner_mutex);
    for (uint32_t i = 0; i < DKB_OWNER_CAPACITY; i++) {
        uint32_t pos = (first + i) & (DKB_OWNER_CAPACITY - 1u);
        if (dkb_owners[pos].pointer == DKB_EMPTY)
            break;
        if (dkb_owners[pos].pointer == pointer) {
            owner = dkb_owners[pos].owner;
            *size = dkb_owners[pos].requested_size;
            dkb_owners[pos].pointer = DKB_TOMBSTONE;
            dkb_owners[pos].owner = 0;
            dkb_owners[pos].requested_size = 0;
            break;
        }
    }
    plat_mutex_unlock(&dkb_owner_mutex);
    return owner;
}

/* Original guest custom-heap deallocation also invalidates provenance. */
static void dkb_owner_log_neighbors(uint64_t pointer) {
    struct DkbOwnerEntry lower = {0};
    struct DkbOwnerEntry upper = {0};
    plat_mutex_lock(&dkb_owner_mutex);
    for (uint32_t i = 0; i < DKB_OWNER_CAPACITY; i++) {
        struct DkbOwnerEntry entry = dkb_owners[i];
        if (entry.pointer == DKB_EMPTY || entry.pointer == DKB_TOMBSTONE)
            continue;
        if (entry.pointer < pointer &&
                (!lower.pointer || entry.pointer > lower.pointer))
            lower = entry;
        if (entry.pointer > pointer &&
                (!upper.pointer || entry.pointer < upper.pointer))
            upper = entry;
    }
    plat_mutex_unlock(&dkb_owner_mutex);
    rt_log("DKB heap-owner unknown neighbors: target=%#llx "
           "lower=%#llx delta=%#llx owner=%#llx size=%#llx "
           "upper=%#llx delta=%#llx owner=%#llx size=%#llx",
           (unsigned long long)pointer,
           (unsigned long long)lower.pointer,
           (unsigned long long)(lower.pointer ? pointer - lower.pointer : 0),
           (unsigned long long)lower.owner,
           (unsigned long long)lower.requested_size,
           (unsigned long long)upper.pointer,
           (unsigned long long)(upper.pointer ? upper.pointer - pointer : 0),
           (unsigned long long)upper.owner,
           (unsigned long long)upper.requested_size);
}

void dkb_heap_owner_discard(uint64_t pointer) {
    uint64_t size = 0;
    uint64_t owner = dkb_owner_take(pointer, &size);
    if (owner && size == 0x20u) {
        uint64_t discard_count = __atomic_add_fetch(
            &dkb_size20_discarded, 1ULL, __ATOMIC_RELAXED);
        if (discard_count <= 256u)
            rt_log("DKB heap-owner 0x20 discard: ptr=%#llx owner=%#llx",
                   (unsigned long long)pointer, (unsigned long long)owner);
    }
}

/*
 * M15.2: an allocation this table did not record is not inferred here. With the
 * Horizon rtld symbol order (M15.1) the original delete path reaches DKB's own
 * operator delete (main+0x1a4700), which searches the game's heap registry for
 * any pointer, including objects inside child heaps. M14.8 called the registry
 * lookup main+0x1a3c60 with the object pointer, but the original destructor
 * (main+0x1a24b0) passes only the nested buffer [object+8]; that fallback is
 * retired.
 */
static void dkb_guest_cpp_delete(Cpu *c) {
    uint64_t pointer = c->x[0];
    uint64_t requested_size = 0;
    uint64_t owner = dkb_owner_take(pointer, &requested_size);
    if (!owner) {
        int resource_candidate = !(pointer & 7u) &&
            dkb_guest_readable(pointer, sizeof(uint64_t)) &&
            LD64(pointer) == DKB_RESOURCE_VTABLE;
        if (resource_candidate) {
            uint64_t missing_count = __atomic_add_fetch(
                &dkb_unknown_base_deletes, 1ULL, __ATOMIC_RELAXED);
            if (missing_count <= 64u)
                rt_log("DKB heap-owner untracked resource delete: ptr=%#llx size=%#llx; "
                       "continues to original guest operator delete",
                       (unsigned long long)pointer, (unsigned long long)c->x[1]);
            if (missing_count == 1u)
                dkb_owner_log_neighbors(pointer);
        }
        __atomic_fetch_add(&dkb_passthrough, 1ULL, __ATOMIC_RELAXED);
        (void)rt_original(c);
        return;
    }
    /* M15.11: DKB's operator delete (M15.1) finds the heap that allocated the object itself, as on hardware. A
     * recorded owner is only the innermost ExpHeap: a heap that allocates from it (RootRepository's tracking heap)
     * must free through its own method, or its bookkeeping stays behind and its parent cannot adjust(). */
    uint64_t routing_count = __atomic_add_fetch(&dkb_routed, 1ULL, __ATOMIC_RELAXED);
    if (routing_count <= 48u) {
        rt_log("DKB heap-owner route: ptr=%#llx owner=%#llx allocated=%#llx delete_arg=%#llx; "
               "runs DKB's own operator delete (M15.11)",
               (unsigned long long)pointer, (unsigned long long)owner,
               (unsigned long long)requested_size, (unsigned long long)c->x[1]);
    }
    (void)rt_original(c);
}

/* Preserve ABI and ordinary SDK behavior for all allocations we did not record. */
HOST_OBSERVER("_ZdlPvm", dkb_guest_cpp_delete);
HOST_OBSERVER("_ZdlPv", dkb_guest_cpp_delete);
