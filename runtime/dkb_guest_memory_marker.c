// M15.15 link marker. The constructor's empty compiler barrier keeps this source and string in the linked executable
// without performing host work before main; runtime/cpu.h contains the actual volatile guest-memory change.
static const char dkb_guest_memory_m15_15_marker[] =
    "DKB guest memory: volatile shared loads/stores enabled (M15.15)";

__attribute__((constructor)) static void dkb_guest_memory_m15_15_link_marker(void) {
    __asm__ volatile("" : : "r"(dkb_guest_memory_m15_15_marker) : "memory");
}
