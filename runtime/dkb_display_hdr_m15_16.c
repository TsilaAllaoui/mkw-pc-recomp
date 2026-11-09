// DKB runtime target: Switch 2 per-frame layer HDR metadata on an SDR host window.
//
// Immediately after the first real nvnQueuePresentTexture, main+0x959a44 and main+0x959a5c call
// SetLayerNextHdrContentGamutMetadata, SetLayerNextHdrToSdrToneMappingMetadata and then the related
// SetLayerNextHdrAdjustableBezierMetadata. The original DKB host predates these imports, leaving
// their PLT slots null. The public DKB recomp accepts layer metadata without mutating the Layer
// because its host swapchain is SDR; use the same ABI for the complete sequence here.
#include <stdint.h>

#include "host.h"

static void dkb_vi_accept_sdr_metadata(Cpu *c) {
    static volatile uint32_t logged;
    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_ACQ_REL))
        rt_log("DKB-VI-HDR-METADATA-SDR: accepted layer metadata for the SDR host swapchain");
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE",
              dkb_vi_accept_sdr_metadata);
HOST_OVERRIDE("_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE",
              dkb_vi_accept_sdr_metadata);
HOST_OVERRIDE("_ZN2nn2vi39SetLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerEfff",
              dkb_vi_accept_sdr_metadata);
