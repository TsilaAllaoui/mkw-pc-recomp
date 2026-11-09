// DKB runtime target: the nn::account calls DKB makes beyond the ones ov_system.c answers.
//
// ov_system.c replaces nn::account::Initialize, so the SDK's account service object is never created and any account
// call left to the SDK dereferences a null ObjectHolder (ListAllUsers -> GetAccountServiceObjectBase -> GetBase, read
// at address 0). This file answers DKB's remaining imports from the same offline model: one local user (account_uid)
// and handles issued by the host's TryOpenPreselectedUser, so CloseUser/GetUserId keep validating them.
//
// The host has no network service account. IsNetworkServiceAccountAvailable reports that truthfully; the calls that
// would need one stop with a named fatal instead of running SDK code against the missing service.
#include <stdint.h>
#include <string.h>

#include "host.h"

#define DKB_ACCOUNT_R_INVALID 0x7c67u   // ov_system.c ACCOUNT_R_INVALID: unknown user handle or uid
#define DKB_OPEN_PRESELECTED "_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE"

typedef struct { uint64_t lo, hi; } DkbUid;

static int dkb_is_local(const DkbUid *u) {
    uint64_t local[2];
    account_uid(local);
    return u && u->lo == local[0] && u->hi == local[1];
}

// Result ListAllUsers(int *count, Uid *out, int capacity): DKB passes capacity 8 and then asks GetNickname per user.
static void dkb_list_all_users(Cpu *c) {
    int32_t *count = arg_p(c, 0);
    DkbUid *out = arg_p(c, 1);
    int32_t capacity = (int32_t)arg_x(c, 2);
    if (!count || (capacity > 0 && !out)) {
        ret_x(c, DKB_ACCOUNT_R_INVALID);
        return;
    }
    uint64_t local[2];
    account_uid(local);
    if (capacity > 0) {
        memset(out, 0, (size_t)capacity * sizeof *out);
        out[0] = (DkbUid){local[0], local[1]};
    }
    *count = 1;
    ret_x(c, 0);
}

// Result OpenUser(UserHandle *out, const Uid &): the local user opens like the preselected one, other uids do not.
static void dkb_open_user(Cpu *c) {
    void *out = arg_p(c, 0);
    if (!out || !dkb_is_local(arg_p(c, 1))) {
        ret_x(c, DKB_ACCOUNT_R_INVALID);
        return;
    }
    GuestFn open = hostfn_override(DKB_OPEN_PRESELECTED);
    if (!open) rt_fatal("nn::account::OpenUser: the host TryOpenPreselectedUser is not linked");
    open(c);   // x0 = out: bool, a handle token the host's CloseUser and GetUserId accept
    ret_x(c, (c->x[0] & 0xff) ? 0 : DKB_ACCOUNT_R_INVALID);
}

// Result IsNetworkServiceAccountAvailable(bool *out, const UserHandle &)
static void dkb_nsa_available(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (!out) {
        ret_x(c, DKB_ACCOUNT_R_INVALID);
        return;
    }
    *out = 0;
    ret_x(c, 0);
}

static void dkb_nsa_required(Cpu *c, const char *what) {
    char lr[96];
    rt_fatal("DKB-ACCOUNT-NSA-UNSUPPORTED: nn::account::%s needs a network service account; the offline host has none "
             "(IsNetworkServiceAccountAvailable reported false) (lr %s)", what, ldr_describe(c->x[30], lr, sizeof lr));
}

static void dkb_nsa_ensure(Cpu *c) { dkb_nsa_required(c, "EnsureNetworkServiceAccountAvailable"); }
static void dkb_nsa_id(Cpu *c) { dkb_nsa_required(c, "GetNetworkServiceAccountId"); }
static void dkb_nsa_token_load(Cpu *c) { dkb_nsa_required(c, "LoadNetworkServiceAccountIdTokenCache"); }
static void dkb_nsa_token_async(Cpu *c) { dkb_nsa_required(c, "EnsureNetworkServiceAccountIdTokenCacheAsync"); }

HOST_OVERRIDE("_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi", dkb_list_all_users);
HOST_OVERRIDE("_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE", dkb_open_user);
HOST_OVERRIDE("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", dkb_nsa_available);
HOST_OVERRIDE("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", dkb_nsa_ensure);
HOST_OVERRIDE("_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE",
              dkb_nsa_id);
HOST_OVERRIDE("_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", dkb_nsa_token_load);
HOST_OVERRIDE("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE",
              dkb_nsa_token_async);
