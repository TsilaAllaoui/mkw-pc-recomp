// DKB runtime target: nn::nifm (network interface manager) on a host that has no network for the game.
//
// DKB's startup (main+0xd73044) runs nn::nifm::Initialize, whose SDK form opens the "nifm:u" service
// (ServiceProviderClient::GetUserServiceSharedPointer, sdk+0x57f824), which the host does not serve. The host answers
// as a console that is offline: nifm initializes, a network request is accepted and never satisfied, so
// IsNetworkAvailable stays false, which is what DKB checks before any online feature. Local network mode (local
// wireless play) only changes what a request asks for and is accepted. The calls that only make sense with a
// connection, or that open the network error applet, stop with a named fatal so their first use is seen.
#include <stdint.h>

#include "host.h"

static volatile int32_t dkb_nifm_logged, dkb_nifm_local;

// Result Initialize()
static void dkb_nifm_initialize(Cpu *c) {
    if (!__atomic_exchange_n(&dkb_nifm_logged, 1, __ATOMIC_ACQ_REL))
        rt_log("nifm: offline (no nifm:u service on this host); DKB sees no network");
    ret_x(c, 0);
}

// bool IsNetworkAvailable()
static void dkb_nifm_available(Cpu *c) { ret_x(c, 0); }

// void SetLocalNetworkMode(bool)
static void dkb_nifm_local_mode(Cpu *c) { __atomic_store_n(&dkb_nifm_local, (int32_t)(arg_x(c, 0) & 1), __ATOMIC_RELAXED); }

// void SubmitNetworkRequestAndWait(), void CancelNetworkRequest(): the request ends unsatisfied
static void dkb_nifm_request(Cpu *c) { (void)c; }

static void dkb_nifm_online_only(Cpu *c, const char *what) {
    char lr[96];
    rt_fatal("DKB-NIFM-OFFLINE: nn::nifm::%s needs a network connection or the error applet; the host is offline "
             "(IsNetworkAvailable is false, local mode %d) (lr %s)", what,
             (int)__atomic_load_n(&dkb_nifm_local, __ATOMIC_RELAXED), ldr_describe(c->x[30], lr, sizeof lr));
}

static void dkb_nifm_ip_config(Cpu *c) { dkb_nifm_online_only(c, "GetCurrentIpConfigInfo"); }
static void dkb_nifm_proxy(Cpu *c) { dkb_nifm_online_only(c, "GetCurrentProxySetting"); }
static void dkb_nifm_error_result(Cpu *c) { dkb_nifm_online_only(c, "HandleNetworkRequestErrorResult"); }

HOST_OVERRIDE("_ZN2nn4nifm10InitializeEv", dkb_nifm_initialize);
HOST_OVERRIDE("_ZN2nn4nifm18IsNetworkAvailableEv", dkb_nifm_available);
HOST_OVERRIDE("_ZN2nn4nifm19SetLocalNetworkModeEb", dkb_nifm_local_mode);
HOST_OVERRIDE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", dkb_nifm_request);
HOST_OVERRIDE("_ZN2nn4nifm20CancelNetworkRequestEv", dkb_nifm_request);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", dkb_nifm_ip_config);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentIpConfigInfoEPNS_6socket6InAddrES3_S3_S3_S3_", dkb_nifm_ip_config);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", dkb_nifm_proxy);
HOST_OVERRIDE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", dkb_nifm_error_result);
