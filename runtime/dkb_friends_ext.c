// DKB runtime target: nn::friends on a host that has no friends service.
//
// DKB's startup (main+0xd6cd74) runs nn::friends::Initialize, whose SDK form (ShimLibraryGlobal::Initialize ->
// detail::ipc::Initialize, sdk+0x452ad0) opens "friend:u", which the host does not serve. The host answers as an
// offline console with no friend list downloaded: the friend list is unavailable and empty, and the one asynchronous
// request DKB can start (EnsureFriendListAvailable) ends at once, so AsyncContext::HasDone reports it done. Play history
// is local bookkeeping with nothing to share and is accepted. SetAllocator, the AsyncContext constructor and destructor
// and the Friend constructor stay the SDK's (allocator, mutex and memset only, no service). Creating a play-history
// registration key needs the service and stops with a named fatal.
#include <stdint.h>

#include "host.h"

static volatile int32_t dkb_friends_logged;

// void Initialize()
static void dkb_friends_initialize(Cpu *c) {
    (void)c;
    if (!__atomic_exchange_n(&dkb_friends_logged, 1, __ATOMIC_ACQ_REL))
        rt_log("friends: offline (no friend:u service on this host); DKB sees no friend list");
}

// Result CheckFriendListAvailability(bool *available, const account::Uid &)
static void dkb_friends_check_list(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::friends::CheckFriendListAvailability: null output");
    *out = 0;
    ret_x(c, 0);
}

// Result EnsureFriendListAvailable(AsyncContext *, const account::Uid &): nothing to download offline
static void dkb_friends_ensure_list(Cpu *c) {
    if (!arg_x(c, 0)) rt_fatal("nn::friends::EnsureFriendListAvailable: null AsyncContext");
    ret_x(c, 0);
}

// Result AsyncContext::HasDone(bool *done)
static void dkb_friends_has_done(Cpu *c) {
    uint8_t *out = arg_p(c, 1);
    if (!out) rt_fatal("nn::friends::AsyncContext::HasDone: null output");
    *out = 1;
    ret_x(c, 0);
}

// Result GetFriendList(int *count, Friend *, const account::Uid &, int offset, int capacity, const FriendFilter &)
static void dkb_friends_list(Cpu *c) {
    int32_t *count = arg_p(c, 0);
    if (!count) rt_fatal("nn::friends::GetFriendList: null count");
    *count = 0;
    ret_x(c, 0);
}

// Result AddPlayHistory(const account::Uid &, const PlayHistoryRegistrationKey &, const InAppScreenName &, ...)
static void dkb_friends_play_history(Cpu *c) { ret_x(c, 0); }

// Result GetPlayHistoryRegistrationKey(PlayHistoryRegistrationKey *, const account::Uid &, bool)
static void dkb_friends_history_key(Cpu *c) {
    char lr[96];
    rt_fatal("DKB-FRIENDS-OFFLINE: nn::friends::GetPlayHistoryRegistrationKey needs the friends service to make a "
             "key; the host has none (lr %s)", ldr_describe(c->x[30], lr, sizeof lr));
}

HOST_OVERRIDE("_ZN2nn7friends10InitializeEv", dkb_friends_initialize);
HOST_OVERRIDE("_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE", dkb_friends_check_list);
HOST_OVERRIDE("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE",
              dkb_friends_ensure_list);
HOST_OVERRIDE("_ZN2nn7friends12AsyncContext7HasDoneEPb", dkb_friends_has_done);
HOST_OVERRIDE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE",
              dkb_friends_list);
HOST_OVERRIDE("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_",
              dkb_friends_play_history);
HOST_OVERRIDE("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb",
              dkb_friends_history_key);
