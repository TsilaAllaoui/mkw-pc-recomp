// DKB runtime target: Switch 2 GameChat (nn::chat) on a host that has no chat service.
//
// nn::chat::Initialize (sdk+0x6bbd70) opens the "chat:u" HIPC user service, which the host does not serve. DKB calls
// it during startup (main+0xd321c0), attaches the room-info event, builds its local camera video storage and asks
// IsChatting. The host answers as a console where GameChat is idle: initialized, never chatting, the room-info event
// never signalled, and GetRoomInfo an empty room (Eglantine NX2_NOTES row 123: chat:u returns a zero-filled 0xf0-byte
// RoomInfo outside GameChat; DKB's caller at main+0xd32264 reserves exactly 0xf0 bytes of stack for it). Sharing-key,
// screen-sharing and full-screen camera sections have no effect without a session and are accepted. Calls that only
// make sense inside a chat session or open a system applet stop with a named fatal.
//
// Every SDK chat entry point checks the library's initialized count first (only the real Initialize raises it), so the
// camera-video storage API is answered here too, the way hardware lays it out (Eglantine rows 125-126): the image pool
// lives in the buffer DKB allocates, one block-linear image after another, the buffer size is the image layout size
// times the image count, and image i of a pool has handle pool id + i. DKB then makes an NVN texture of each image
// (nvnNativeImageInitialize, nvnTextureInitializeFromNativeImage at main+0xd32330) and aborts if that fails. These are
// real host textures over that memory, GPU-owned like any image another process writes; with no camera nothing ever
// writes them, as on a console whose camera is not streaming.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"
#include "platform/platform.h"

#define DKB_CHAT_ROOM_INFO 0xf0u       // nn::chat::RoomInfo
#define DKB_CHAT_VIDEO_STORAGE 0xe0u   // LocalCameraVideoType / RemoteCameraVideoType, zeroed by the SDK on init
#define DKB_CHAT_POOLS 16
#define DKB_CHAT_POOL_IDS 8u           // handle stride per pool: at most 4 images
#define DKB_CHAT_HANDLE_BASE 0x100u
#define DKB_NATIVE_IMAGE_MAGIC MAGIC('N', 'I', 'M', 'G')

typedef struct {
    uint64_t storage, buffer, stride;  // guest storage object, DKB's buffer, bytes per image
    uint32_t width, height, count, base;
} DkbChatPool;

typedef struct {
    uint32_t magic, handle, width, height, format, pad;
    uint64_t mem;
} DkbNativeImage;                      // in the 0x1a0 bytes of NVNnativeImage

typedef struct {
    NVNdevice *dev;
    uint32_t handle, permission, format;
} DkbNativeImageBuilder;               // the SDK's builder setters: sdk+0x6bb070..0x6bb0b0

static DkbChatPool dkb_chat_pools[DKB_CHAT_POOLS];
static PlatMutex dkb_chat_lock = PLAT_MUTEX_INIT;
static volatile int32_t dkb_chat_initialized;

static void dkb_chat_fail(Cpu *c, const char *fmt, const char *what, uint64_t a, uint64_t b, uint64_t d) {
    char lr[96], msg[256];
    snprintf(msg, sizeof msg, fmt, (unsigned long long)a, (unsigned long long)b, (unsigned long long)d);
    rt_fatal("nn::chat::%s: %s (lr %s)", what, msg, ldr_describe(c->x[30], lr, sizeof lr));
}

// void Initialize()
static void dkb_chat_initialize(Cpu *c) {
    (void)c;
    if (!__atomic_exchange_n(&dkb_chat_initialized, 1, __ATOMIC_ACQ_REL))
        rt_log("chat: GameChat idle (no chat:u service on this host); DKB sees no chat session");
}

// bool IsChatting()
static void dkb_chat_is_chatting(Cpu *c) { ret_x(c, 0); }

// Result AttachToRoomInfoChangedEvent(os::SystemEvent *): its SystemEventType is at offset 0
static void dkb_chat_attach_room_event(Cpu *c) {
    uint64_t ev = arg_x(c, 0);
    if (!ev) rt_fatal("nn::chat::AttachToRoomInfoChangedEvent: null event");
    kern_bind_system_event(c, ev, 0);
    ret_x(c, 0);
}

// Result GetRoomInfo(RoomInfo *): an empty room
static void dkb_chat_room_info(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::chat::GetRoomInfo: null RoomInfo");
    memset(out, 0, DKB_CHAT_ROOM_INFO);
    ret_x(c, 0);
}

// void SetSharingKey(const SharingKey &), void ClearSharingKey(), void SetScreenSharingMode(ScreenSharingMode),
// void BeginFullScreenWithLocalCameraAvailableSection(bool), void EndFullScreenWithLocalCameraAvailableSection()
static void dkb_chat_accept(Cpu *c) { (void)c; }

// The SDK's argument checks (sdk+0x6bf244, sdk+0x6bf340): width 32..1920, height 32..1080, 1..4 images.
static void dkb_chat_check_video(Cpu *c, const char *what, int64_t w, int64_t h, int64_t n) {
    if (w < 0x20 || w > 0x780 || h < 0x20 || h > 0x438 || n < 1 || n > 4)
        dkb_chat_fail(c, "%llux%llu, %llu images: outside the SDK's limits", what, (uint64_t)w, (uint64_t)h,
                      (uint64_t)n);
}

static uint64_t dkb_chat_image_bytes(uint32_t w, uint32_t h, int format) {
    nvn_texdesc d;
    nvn_texlayout L;
    memset(&d, 0, sizeof d);
    d.target = NVN_TEXTURE_TARGET_2D;
    d.format = format;
    d.width = w, d.height = h, d.depth = 1, d.levels = 1;
    nvn_tex_layout(&d, &L);
    uint64_t align = L.align > 0x1000 ? L.align : 0x1000;
    return (L.size + align - 1) & ~(align - 1);
}

// size_t Calculate{Local,Remote}CameraVideoBufferSize(int width, int height, int images)
static void dkb_chat_video_size(Cpu *c, const char *what) {
    int64_t w = (int32_t)arg_x(c, 0), h = (int32_t)arg_x(c, 1), n = (int32_t)arg_x(c, 2);
    dkb_chat_check_video(c, what, w, h, n);
    ret_x(c, dkb_chat_image_bytes((uint32_t)w, (uint32_t)h, NVN_FORMAT_RGBA8) * (uint64_t)n);
}

static void dkb_chat_local_size(Cpu *c) { dkb_chat_video_size(c, "CalculateLocalCameraVideoBufferSize"); }
static void dkb_chat_remote_size(Cpu *c) { dkb_chat_video_size(c, "CalculateRemoteCameraVideoBufferSize"); }

static DkbChatPool *dkb_chat_pool_of(uint64_t storage) {
    for (int i = 0; i < DKB_CHAT_POOLS; i++)
        if (dkb_chat_pools[i].storage == storage) return &dkb_chat_pools[i];
    return NULL;
}

// void Initialize{Local,Remote}CameraVideoStorage(VideoType *, int width, int height, int images, void *, size_t)
static void dkb_chat_video_init(Cpu *c, const char *what) {
    uint64_t storage = arg_x(c, 0), buffer = arg_x(c, 4), size = arg_x(c, 5);
    int64_t w = (int32_t)arg_x(c, 1), h = (int32_t)arg_x(c, 2), n = (int32_t)arg_x(c, 3);
    if (!storage || !buffer || (buffer & 0xfff) || (size & 0xfff))
        dkb_chat_fail(c, "storage %#llx, buffer %#llx, size %#llx: null or not 4 KiB aligned", what, storage, buffer,
                      size);
    dkb_chat_check_video(c, what, w, h, n);
    uint64_t stride = dkb_chat_image_bytes((uint32_t)w, (uint32_t)h, NVN_FORMAT_RGBA8);
    if (size < stride * (uint64_t)n)
        dkb_chat_fail(c, "buffer %#llx holds %#llx bytes, the image pool needs %#llx", what, buffer, size,
                      stride * (uint64_t)n);
    memset((void *)(uintptr_t)storage, 0, DKB_CHAT_VIDEO_STORAGE);
    plat_mutex_lock(&dkb_chat_lock);
    DkbChatPool *p = dkb_chat_pool_of(storage);
    if (!p) p = dkb_chat_pool_of(0);
    if (!p) {
        plat_mutex_unlock(&dkb_chat_lock);
        dkb_chat_fail(c, "more than %llu camera video storages at once (storage %#llx)", what, DKB_CHAT_POOLS,
                      storage, 0);
    }
    *p = (DkbChatPool){storage, buffer, stride, (uint32_t)w, (uint32_t)h, (uint32_t)n,
                       DKB_CHAT_HANDLE_BASE + (uint32_t)(p - dkb_chat_pools) * DKB_CHAT_POOL_IDS};
    plat_mutex_unlock(&dkb_chat_lock);
    rt_log("chat: %s %p: %lldx%lld x%lld images in DKB's buffer %#llx (%#llx bytes each), handles %#x.., no camera "
           "writes them", what, (void *)(uintptr_t)storage, (long long)w, (long long)h, (long long)n,
           (unsigned long long)buffer, (unsigned long long)stride, p->base);
}

static void dkb_chat_local_init(Cpu *c) { dkb_chat_video_init(c, "InitializeLocalCameraVideoStorage"); }
static void dkb_chat_remote_init(Cpu *c) { dkb_chat_video_init(c, "InitializeRemoteCameraVideoStorage"); }

// void Finalize{Local,Remote}CameraVideoStorage(VideoType *): no image was ever produced, nothing to wait for
static void dkb_chat_video_fini(Cpu *c) {
    uint64_t storage = arg_x(c, 0);
    if (!storage) return;
    plat_mutex_lock(&dkb_chat_lock);
    DkbChatPool *p = dkb_chat_pool_of(storage);
    if (p) memset(p, 0, sizeof *p);
    plat_mutex_unlock(&dkb_chat_lock);
    memset((void *)(uintptr_t)storage, 0, DKB_CHAT_VIDEO_STORAGE);
}

// gs::NativeImageHandle Get{Local,Remote}CameraImageHandle(const VideoType *, int index)
static void dkb_chat_image_handle(Cpu *c, const char *what) {
    uint64_t storage = arg_x(c, 0);
    int32_t index = (int32_t)arg_x(c, 1);
    plat_mutex_lock_shared(&dkb_chat_lock);
    const DkbChatPool *p = storage ? dkb_chat_pool_of(storage) : NULL;
    uint32_t base = p ? p->base : 0, count = p ? p->count : 0;
    plat_mutex_unlock_shared(&dkb_chat_lock);
    if (!p || index < 0 || (uint32_t)index >= count)
        dkb_chat_fail(c, "storage %#llx image %llu: not an initialized storage or index (%llu images)", what, storage,
                      (uint64_t)(int64_t)index, count);
    ret_x(c, base + (uint32_t)index);
}

static void dkb_chat_local_image(Cpu *c) { dkb_chat_image_handle(c, "GetLocalCameraImageHandle"); }
static void dkb_chat_remote_image(Cpu *c) { dkb_chat_image_handle(c, "GetRemoteCameraImageHandle"); }

// NVNboolean nvnNativeImageInitialize(NVNnativeImage *, const NVNnativeImageBuilder *)
static void dkb_native_image_init(Cpu *c) {
    DkbNativeImage *img = arg_p(c, 0);
    const DkbNativeImageBuilder *b = arg_p(c, 1);
    if (!img || !b) rt_fatal("nvnNativeImageInitialize: null image %p or builder %p", (void *)img, (const void *)b);
    uint32_t handle = b->handle, at = handle - DKB_CHAT_HANDLE_BASE;
    DkbNativeImage out = {0};
    plat_mutex_lock_shared(&dkb_chat_lock);
    const DkbChatPool *p = at / DKB_CHAT_POOL_IDS < DKB_CHAT_POOLS ? &dkb_chat_pools[at / DKB_CHAT_POOL_IDS] : NULL;
    if (p && p->storage && at % DKB_CHAT_POOL_IDS < p->count)
        out = (DkbNativeImage){DKB_NATIVE_IMAGE_MAGIC, handle, p->width, p->height, b->format, 0,
                               p->buffer + p->stride * (at % DKB_CHAT_POOL_IDS)};
    plat_mutex_unlock_shared(&dkb_chat_lock);
    if (!out.magic) {
        char lr[96];
        rt_fatal("DKB-NATIVE-IMAGE-UNKNOWN: nvnNativeImageInitialize: handle %#x is not a GameChat camera image; the "
                 "host has no graphics server that made it (lr %s)", handle, ldr_describe(c->x[30], lr, sizeof lr));
    }
    memset(img, 0, sizeof *img);
    *img = out;
    ret_x(c, 1);
}

// void nvnNativeImageFinalize(NVNnativeImage *)
static void dkb_native_image_fini(Cpu *c) {
    DkbNativeImage *img = arg_p(c, 0);
    if (img) img->magic = 0;
}

// NVNboolean nvnTextureInitializeFromNativeImage(NVNtexture *, NVNnativeImage *)
static void dkb_texture_from_native_image(Cpu *c) {
    tex_t *o = arg_p(c, 0);
    const DkbNativeImage *img = arg_p(c, 1);
    if (!o || !img || img->magic != DKB_NATIVE_IMAGE_MAGIC)
        rt_fatal("nvnTextureInitializeFromNativeImage: texture %p, image %p is not an initialized native image",
                 (void *)o, (const void *)img);
    if (o->magic == NVN_TEX_MAGIC && o->serial) gfx_backend()->texture_release(o);
    memset(o, 0, sizeof *o);
    o->magic = NVN_TEX_MAGIC;
    o->d.target = NVN_TEXTURE_TARGET_2D;
    o->d.format = (int)img->format;
    o->d.width = img->width, o->d.height = img->height, o->d.depth = 1, o->d.levels = 1;
    o->d.swizzle_r = 2, o->d.swizzle_g = 3, o->d.swizzle_b = 4, o->d.swizzle_a = 5;
    nvn_tex_layout(&o->d, &o->L);
    if (dkb_chat_image_bytes(img->width, img->height, (int)img->format) >
        dkb_chat_image_bytes(img->width, img->height, NVN_FORMAT_RGBA8))
        rt_fatal("nvnTextureInitializeFromNativeImage: format %#x needs more than the %ux%u camera image slot",
                 img->format, img->width, img->height);
    o->mem = (uint8_t *)(uintptr_t)img->mem;
    o->serial = nvn_new_serial();
    NVN_LOG("Texture %p from native image %#x: %ux%u fmt=%#x at %p", (void *)o, img->handle, img->width, img->height,
            img->format, (void *)o->mem);
    ret_x(c, 1);
}

static void dkb_chat_session_only(Cpu *c, const char *what) {
    char lr[96];
    rt_fatal("DKB-CHAT-UNSUPPORTED: nn::chat::%s needs a GameChat session or applet; the host has none (IsChatting is "
             "false) (lr %s)", what, ldr_describe(c->x[30], lr, sizeof lr));
}

static void dkb_chat_show_start(Cpu *c) { dkb_chat_session_only(c, "ShowChatStart"); }
static void dkb_chat_show_guide(Cpu *c) { dkb_chat_session_only(c, "ShowCameraSharingGuide"); }
static void dkb_chat_show_config(Cpu *c) { dkb_chat_session_only(c, "ShowLocalCameraConfigurationVersion0"); }
static void dkb_chat_local_start(Cpu *c) { dkb_chat_session_only(c, "StartLocalCameraStreaming"); }
static void dkb_chat_local_stop(Cpu *c) { dkb_chat_session_only(c, "StopLocalCameraStreaming"); }
static void dkb_chat_remote_start(Cpu *c) { dkb_chat_session_only(c, "StartRemoteCameraStreaming"); }
static void dkb_chat_remote_stop(Cpu *c) { dkb_chat_session_only(c, "StopRemoteCameraStreaming"); }
static void dkb_chat_local_next(Cpu *c) { dkb_chat_session_only(c, "RequestNextLocalCameraImage"); }
static void dkb_chat_remote_next(Cpu *c) { dkb_chat_session_only(c, "RequestNextRemoteCameraImage"); }

HOST_OVERRIDE("_ZN2nn4chat10InitializeEv", dkb_chat_initialize);
HOST_OVERRIDE("_ZN2nn4chat10IsChattingEv", dkb_chat_is_chatting);
HOST_OVERRIDE("_ZN2nn4chat28AttachToRoomInfoChangedEventEPNS_2os11SystemEventE", dkb_chat_attach_room_event);
HOST_OVERRIDE("_ZN2nn4chat11GetRoomInfoEPNS0_8RoomInfoE", dkb_chat_room_info);
HOST_OVERRIDE("_ZN2nn4chat13SetSharingKeyERKNS0_10SharingKeyE", dkb_chat_accept);
HOST_OVERRIDE("_ZN2nn4chat15ClearSharingKeyEv", dkb_chat_accept);
HOST_OVERRIDE("_ZN2nn4chat20SetScreenSharingModeENS0_17ScreenSharingModeE", dkb_chat_accept);
HOST_OVERRIDE("_ZN2nn4chat46BeginFullScreenWithLocalCameraAvailableSectionEb", dkb_chat_accept);
HOST_OVERRIDE("_ZN2nn4chat44EndFullScreenWithLocalCameraAvailableSectionEv", dkb_chat_accept);
HOST_OVERRIDE("_ZN2nn4chat35CalculateLocalCameraVideoBufferSizeEiii", dkb_chat_local_size);
HOST_OVERRIDE("_ZN2nn4chat36CalculateRemoteCameraVideoBufferSizeEiii", dkb_chat_remote_size);
HOST_OVERRIDE("_ZN2nn4chat33InitializeLocalCameraVideoStorageEPNS0_20LocalCameraVideoTypeEiiiPvm", dkb_chat_local_init);
HOST_OVERRIDE("_ZN2nn4chat34InitializeRemoteCameraVideoStorageEPNS0_21RemoteCameraVideoTypeEiiiPvm",
              dkb_chat_remote_init);
HOST_OVERRIDE("_ZN2nn4chat31FinalizeLocalCameraVideoStorageEPNS0_20LocalCameraVideoTypeE", dkb_chat_video_fini);
HOST_OVERRIDE("_ZN2nn4chat32FinalizeRemoteCameraVideoStorageEPNS0_21RemoteCameraVideoTypeE", dkb_chat_video_fini);
HOST_OVERRIDE("_ZN2nn4chat25GetLocalCameraImageHandleEPKNS0_20LocalCameraVideoTypeEi", dkb_chat_local_image);
HOST_OVERRIDE("_ZN2nn4chat26GetRemoteCameraImageHandleEPKNS0_21RemoteCameraVideoTypeEi", dkb_chat_remote_image);
HOST_OVERRIDE("_Z24nvnNativeImageInitializeP14NVNnativeImagePK21NVNnativeImageBuilder", dkb_native_image_init);
HOST_OVERRIDE("_Z22nvnNativeImageFinalizeP14NVNnativeImage", dkb_native_image_fini);
HOST_OVERRIDE("_Z35nvnTextureInitializeFromNativeImageP10NVNtextureP14NVNnativeImage", dkb_texture_from_native_image);
HOST_OVERRIDE("_ZN2nn4chat13ShowChatStartERKNS_7account3UidE", dkb_chat_show_start);
HOST_OVERRIDE("_ZN2nn4chat22ShowCameraSharingGuideEv", dkb_chat_show_guide);
HOST_OVERRIDE("_ZN2nn4chat36ShowLocalCameraConfigurationVersion0ENS0_25CameraImageProcessingModeE",
              dkb_chat_show_config);
HOST_OVERRIDE("_ZN2nn4chat25StartLocalCameraStreamingEv", dkb_chat_local_start);
HOST_OVERRIDE("_ZN2nn4chat24StopLocalCameraStreamingEv", dkb_chat_local_stop);
HOST_OVERRIDE("_ZN2nn4chat26StartRemoteCameraStreamingERKNS_7account23NetworkServiceAccountIdE",
              dkb_chat_remote_start);
HOST_OVERRIDE("_ZN2nn4chat25StopRemoteCameraStreamingERKNS_7account23NetworkServiceAccountIdE", dkb_chat_remote_stop);
HOST_OVERRIDE("_ZN2nn4chat27RequestNextLocalCameraImageEPNS_2gs16NativeSyncHandleEPNS0_20LocalCameraVideoTypeEiS2_",
              dkb_chat_local_next);
HOST_OVERRIDE("_ZN2nn4chat28RequestNextRemoteCameraImageEPNS_2gs16NativeSyncHandleEPNS0_21RemoteCameraVideoTypeEiS2_RKNS_7account23NetworkServiceAccountIdE",
              dkb_chat_remote_next);
