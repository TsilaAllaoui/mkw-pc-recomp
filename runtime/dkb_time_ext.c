// M15.13 (DKB): nn::time imports that ov_system.c leaves to the SDK, answered as an offline console.
//
// Observed after M15.12: svc 0x26 Break in nn::diag::detail::Abort from
// nn::time::StandardNetworkSystemClock::IsAccuracySufficient (sdk+0x5cd1f0), called at main+0x36a9b4. ov_system.c
// replaces nn::time::Initialize, so the SDK's time service objects never exist.
//
// DKB's uses (1.8.0 main):
//   main+0x36a9b0  IsAccuracySufficient; false sets a once-only flag and still reads the network clock.
//   main+0x8726a8  LoadTimeZoneRule(this+0xb8, "UTC"); the result is ignored and no imported function reads the rule.
//   main+0x16592a4 GetDeviceLocationName.
//   main+0xfbb150  AsyncEnsureNetworkClockAvailabilityTask: C1, Initialize(manual clear), StartTask,
//                  nn::async::Await(GetFinishEvent()), GetResult; a failing result is logged, not fatal. D1 at
//                  main+0xfbb544/0xfbb6c4.
// The SDK task wraps nn::ntc::shim::CorrectionNetworkClockAsyncTask (the ntc service) in 0x90 bytes plus an
// "initialized" byte at +0x90. Here those bytes hold a real SDK system event that StartTask signals: the task
// finishes at once with nifm 2110-3127 (internet connection not available), what an offline console reports.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

#define TASK_INITIALIZED 0x90
#define RESULT_NO_INTERNET (110u | (3127u << 9))

static uint64_t sdk_fn(const char *name) {
    uint64_t a = ldr_symbol(name);
    if (!a) rt_fatal("DKB-TIME-EXT: the SDK's %s is not loaded", name);
    return a;
}

static void dkb_time_accuracy(Cpu *c) { ret_x(c, 0); }

// Same zone name as ov_system.c's ToCalendarTime, which follows the host zone.
static void dkb_time_location(Cpu *c) {
    char *out = arg_p(c, 0);
    long west = plat_tz_west();
    memset(out, 0, 0x24);
    if (!west || west % 3600) snprintf(out, 0x24, "UTC");
    else snprintf(out, 0x24, "Etc/GMT%+ld", west / 3600);
    ret_x(c, 0);
}

static void dkb_time_load_rule(Cpu *c) {
    static volatile int32_t logged;
    if (!__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED))
        rt_log("DKB time: LoadTimeZoneRule(%#llx, \"%.36s\") answered without a rule; ToCalendarTime uses the host zone",
               (unsigned long long)arg_x(c, 0), (const char *)arg_p(c, 1));
    ret_x(c, 0);
}

static void dkb_task_ctor(Cpu *c) {
    memset(arg_p(c, 0), 0, TASK_INITIALIZED + 1);
    ret_x(c, arg_x(c, 0));
}

static void dkb_task_initialize(Cpu *c) {
    uint8_t *t = arg_p(c, 0);
    if (t[TASK_INITIALIZED] & 1) return;
    static uint64_t create;
    if (!create) create = sdk_fn("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    uint32_t r = (uint32_t)rt_call(c, create, 3, (uint64_t)(uintptr_t)t, arg_x(c, 1) & 0xffffffffu, (uint64_t)0);
    if (r) rt_fatal("DKB-TIME-EXT: CreateSystemEvent for the network clock task failed %#x", r);
    t[TASK_INITIALIZED] = 1;
}

static void dkb_task_start(Cpu *c) {
    uint8_t *t = arg_p(c, 0);
    if (!(t[TASK_INITIALIZED] & 1)) rt_fatal("DKB-TIME-EXT: StartTask on a network clock task not initialized");
    static uint64_t sig;
    if (!sig) sig = sdk_fn("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    rt_call(c, sig, 1, (uint64_t)(uintptr_t)t);
    rt_log("DKB time: EnsureNetworkClockAvailability task finishes at once with %#x (offline, 2110-3127)",
           RESULT_NO_INTERNET);
    ret_x(c, 0);
}

static void dkb_task_event(Cpu *c) { ret_x(c, arg_x(c, 0)); }
static void dkb_task_result(Cpu *c) { ret_x(c, RESULT_NO_INTERNET); }

static void dkb_task_dtor(Cpu *c) {
    uint8_t *t = arg_p(c, 0);
    if (t[TASK_INITIALIZED] & 1) {
        static uint64_t destroy;
        if (!destroy) destroy = sdk_fn("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
        rt_call(c, destroy, 1, (uint64_t)(uintptr_t)t);
        t[TASK_INITIALIZED] = 0;
    }
    ret_x(c, (uint64_t)(uintptr_t)t);
}

HOST_OVERRIDE("_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv", dkb_time_accuracy);
HOST_OVERRIDE("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", dkb_time_location);
HOST_OVERRIDE("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", dkb_time_load_rule);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC1Ev", dkb_task_ctor);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD1Ev", dkb_task_dtor);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask10InitializeENS_2os14EventClearModeE",
              dkb_task_initialize);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", dkb_task_start);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv", dkb_task_event);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", dkb_task_result);
