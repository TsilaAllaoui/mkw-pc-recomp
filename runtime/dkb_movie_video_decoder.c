// M15.19 (DKB): movie::VideoDecoder (subsdk0) answered on the host with libvpx.
//
// Observed after M15.18: svc 0x26 Break in nn::diag::detail::Abort from movie::detail::
// InitializeDecoderByTransferMemory (subsdk0+0xf9a8) under movie::VideoDecoder::Configure (main+0x8099dc), when a
// Grand Prix starts (the cup's opening movie) and from the title screen's idle movie. subsdk0's decoder runs on the
// Switch 2 nn::codec video server, which this host does not have.
// Approach: answer the public VideoDecoder methods DKB imports, as nx2navi (github.com/tangerineturtle1/nx2navi,
// runtime/ov_movie2.c) does with dav1d for another Switch 2 title. This file is our own code; its VP9 to NV12
// conversion follows runtime/ov_movie.c (adapted from MacNativeGames, MIT).
// DKB's movies (romfs Movie/*.webm, 58 files) are VP9 video only: 2560x1440 (cup openings, manuals, staff roll) and
// 640x360 (course ad boards). The SDK's nn::movie::MediaExtractor demuxes them as guest code.
// Interface, read from the 1.8.0 ExeFS:
//   main+0x8099a8..0x809a94  Initialize(memory, size, mime, output format 3, mode 1), Configure (neither result is
//                            checked), N VideoFrames (operator new 0x90, SetPixelStorageType(1), SetBuffer of
//                            w * h * 3 / 2 bytes: NV12), RegisterVideoFrame, GetInputReadyEvent, GetOutputReadyEvent.
//   main+0x808ebc, 0x809498  DecodeAsync; any non-zero result keeps the sample for the next input-ready wake.
//   main+0x809578            AcquireVideoFrame until it yields no frame (subsdk0 answers 0x7b, subsdk0+0x3d7c).
//   VideoFrame               (subsdk0+0x5040..0x5230) +0x08 + 8 i plane i data, +0x28 + 8 i plane i filled size,
//                            +0x48 buffer capacity, +0x78 time stamp, +0x84 storage type, +0x88 end of stream.
//   nn::movie::Buffer        (sdk+0x4b7c80..0x4b7d40) +0x00 data, +0x10 filled size, +0x18 time stamp, +0x20 flags;
//                            flag bit 3 is end of stream (main+0x808ed4).
// One host worker per decoder runs libvpx, so a heavy 1440p frame never stalls the game's movie loop. Events: input
// ready while few samples and pictures wait, output ready while a picture waits. The worker only signals (any
// thread); clears go through the SDK's ClearSystemEvent on the game's thread.
#include <emmintrin.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"

#define NO_FRAME 0x7bu
#define QUEUE_FULL 0x7du
#define FLAG_EOS 0x8u
enum { SAMPLES = 8, PICTURES = 4, SLOTS = PICTURES + 3, READY_BELOW = 3, FRAMES = 16, EVENT_BYTES = 0x80 };
enum { VPX_THREADS = 8 };

typedef struct { uint8_t *data; size_t size; int64_t ts; int eos; } Sample;
typedef struct { uint8_t *nv12; int w, h; int64_t ts; int eos; } Picture;

typedef struct Vd {
    _Alignas(16) uint8_t ev_in[EVENT_BYTES];   // nn::os::SystemEventType each (state byte at +0x28)
    _Alignas(16) uint8_t ev_out[EVENT_BYTES];
    struct Vd *next_free;
    int bound;              // both events made by the SDK; they stay bound when the record is reused
    PlatMutex lock;
    PlatCond wake;          // worker: a sample, a free picture slot, or quit
    PlatThread *worker;
    int quit, reset;        // reset: the worker reopens libvpx before its next sample (after Flush)
    uint64_t generation;    // bumped by Flush: pictures of samples taken before it are dropped
    Sample in[SAMPLES];
    int in_head, in_n;
    Picture out[SLOTS];     // the worker takes a sample while fewer than PICTURES wait; one sample may add up to 3
    int out_head, out_n;
    int eos_in;             // an end-of-stream sample was queued; later samples are dropped until Flush
    uint64_t frame[FRAMES];
    uint8_t held[FRAMES];
    int nframes;
    vpx_codec_ctx_t vpx;
    int vpx_live;
    uint64_t decoded, shown;
} Vd;

static Vd *g_free;
static PlatMutex g_free_lock = PLAT_MUTEX_INIT;

static uint64_t sdk_fn(const char *name) {
    uint64_t a = ldr_symbol(name);
    if (!a) rt_fatal("DKB-MOVIE-VP9: the SDK's %s is not loaded", name);
    return a;
}

static uint64_t addr_of(const void *p) { return (uint64_t)(uintptr_t)p; }
static Vd *vd_of(Cpu *c) { return *(Vd **)arg_p(c, 0); }
static int input_ready(const Vd *v) { return !v->eos_in && v->in_n + v->out_n < READY_BELOW; }

// under v->lock, any thread
static void raise_events(Vd *v) {
    if (!v->bound) return;
    if (input_ready(v)) kern_signal_system_event(addr_of(v->ev_in));
    if (v->out_n) kern_signal_system_event(addr_of(v->ev_out));
}

// Game thread, v->lock held (dropped around the SDK calls): each event signalled exactly while its condition holds. A
// clear can erase a signal the worker raised meanwhile, so the conditions are read again after it.
static void settle_events(Cpu *c, Vd *v) {
    if (!v->bound) return;
    static uint64_t clear;
    if (!clear) clear = sdk_fn("_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE");
    int in = input_ready(v), out = v->out_n > 0;
    raise_events(v);
    if (in && out) return;
    plat_mutex_unlock(&v->lock);
    if (!in) rt_call(c, clear, 1, addr_of(v->ev_in));
    if (!out) rt_call(c, clear, 1, addr_of(v->ev_out));
    plat_mutex_lock(&v->lock);
    raise_events(v);
}

// under v->lock
static void drop_queues(Vd *v) {
    for (; v->in_n; v->in_n--, v->in_head = (v->in_head + 1) % SAMPLES) free(v->in[v->in_head].data);
    for (; v->out_n; v->out_n--, v->out_head = (v->out_head + 1) % SLOTS) free(v->out[v->out_head].nv12);
    v->in_head = v->out_head = 0;
    v->eos_in = 0;
}

// I420 (8 bits, or more stored in 16) -> NV12 with stride = width: Y rows, then rows of interleaved U, V.
static void to_nv12(const vpx_image_t *img, uint8_t *dst, int w, int h) {
    int hbd = (img->fmt & VPX_IMG_FMT_HIGHBITDEPTH) != 0, shift = hbd ? (int)img->bit_depth - 8 : 0, cw = w / 2;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = img->planes[VPX_PLANE_Y] + (size_t)y * img->stride[VPX_PLANE_Y];
        if (!hbd) memcpy(dst + (size_t)y * w, row, (size_t)w);
        else for (int x = 0; x < w; x++) dst[(size_t)y * w + x] = (uint8_t)(((const uint16_t *)row)[x] >> shift);
    }
    uint8_t *uv = dst + (size_t)w * h;
    for (int y = 0; y < h / 2; y++) {
        const uint8_t *u = img->planes[VPX_PLANE_U] + (size_t)y * img->stride[VPX_PLANE_U];
        const uint8_t *v = img->planes[VPX_PLANE_V] + (size_t)y * img->stride[VPX_PLANE_V];
        uint8_t *o = uv + (size_t)y * w;
        int x = 0;
        if (!hbd)
            for (; x + 16 <= cw; x += 16) {
                __m128i a = _mm_loadu_si128((const __m128i *)(u + x)), b = _mm_loadu_si128((const __m128i *)(v + x));
                _mm_storeu_si128((__m128i *)(o + 2 * x), _mm_unpacklo_epi8(a, b));
                _mm_storeu_si128((__m128i *)(o + 2 * x + 16), _mm_unpackhi_epi8(a, b));
            }
        for (; x < cw; x++) {
            o[2 * x] = hbd ? (uint8_t)(((const uint16_t *)u)[x] >> shift) : u[x];
            o[2 * x + 1] = hbd ? (uint8_t)(((const uint16_t *)v)[x] >> shift) : v[x];
        }
    }
}

// ---- worker thread (the only user of v->vpx while it runs)
static void codec_open(Vd *v) {
    if (v->vpx_live) vpx_codec_destroy(&v->vpx);
    vpx_codec_dec_cfg_t cfg = {.threads = VPX_THREADS};
    if (vpx_codec_dec_init(&v->vpx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK)
        rt_fatal("DKB-MOVIE-VP9: libvpx VP9 decoder initialisation failed");
    vpx_codec_control(&v->vpx, VP9D_SET_ROW_MT, 1);
    v->vpx_live = 1;
}

static int collect(Vd *v, int64_t ts, Picture *got, int n, int max) {
    vpx_codec_iter_t it = NULL;
    const vpx_image_t *img;
    while ((img = vpx_codec_get_frame(&v->vpx, &it)) != NULL) {
        int w = (int)img->d_w, h = (int)img->d_h;
        if ((img->fmt & ~VPX_IMG_FMT_HIGHBITDEPTH) != VPX_IMG_FMT_I420 || ((w | h) & 1) || n >= max) {
            static volatile int32_t logged;
            if (!plat_xchg32(&logged, 1))
                rt_log("DKB-MOVIE-VP9: picture %dx%d format %#x dropped (4:2:0 with even sides only)", w, h, img->fmt);
            continue;
        }
        uint8_t *nv12 = malloc((size_t)w * h * 3 / 2);
        if (!nv12) rt_fatal("DKB-MOVIE-VP9: out of memory for a %dx%d picture", w, h);
        to_nv12(img, nv12, w, h);
        got[n++] = (Picture){nv12, w, h, ts, 0};
    }
    return n;
}

static int decode(Vd *v, const Sample *s, Picture *got, int max) {
    int n = 0, room = max - (s->eos != 0);   // the end-of-stream marker keeps its slot
    if (s->size) {
        vpx_codec_err_t e = vpx_codec_decode(&v->vpx, s->data, (unsigned)s->size, NULL, 0);
        if (e == VPX_CODEC_OK) {
            n = collect(v, s->ts, got, n, room);
        } else {
            static volatile int32_t logged;
            const char *detail = vpx_codec_error_detail(&v->vpx);
            if (!plat_xchg32(&logged, 1))
                rt_log("DKB-MOVIE-VP9: libvpx rejected a %llu-byte sample: %s", (unsigned long long)s->size,
                       detail ? detail : vpx_codec_err_to_string(e));
        }
    }
    if (s->eos) {
        vpx_codec_decode(&v->vpx, NULL, 0, NULL, 0);
        n = collect(v, s->ts, got, n, room);
        got[n++] = (Picture){NULL, 0, 0, s->ts, 1};
    }
    return n;
}

static void worker(void *arg) {
    Vd *v = arg;
    plat_mutex_lock(&v->lock);
    for (;;) {
        while (!v->quit && (!v->in_n || v->out_n >= PICTURES)) plat_cond_wait(&v->wake, &v->lock);
        if (v->quit) break;
        Sample s = v->in[v->in_head];
        v->in_head = (v->in_head + 1) % SAMPLES;
        v->in_n--;
        uint64_t gen = v->generation;
        int reset = v->reset;
        v->reset = 0;
        raise_events(v);
        plat_mutex_unlock(&v->lock);
        if (reset) codec_open(v);
        Picture got[SLOTS - PICTURES];
        int n = decode(v, &s, got, SLOTS - PICTURES);
        free(s.data);
        plat_mutex_lock(&v->lock);
        for (int i = 0; i < n; i++) {
            if (v->generation != gen) {
                free(got[i].nv12);
                continue;
            }
            v->out[(v->out_head + v->out_n) % SLOTS] = got[i];
            v->out_n++;
            v->decoded += !got[i].eos;
        }
        raise_events(v);
    }
    plat_mutex_unlock(&v->lock);
}

// ---- records: reused, so the SDK events each one bound stay in use (the host binds at most 512 system events)
static Vd *vd_get(void) {
    plat_mutex_lock(&g_free_lock);
    Vd *v = g_free;
    if (v) g_free = v->next_free;
    plat_mutex_unlock(&g_free_lock);
    if (v) return v;
    v = calloc(1, sizeof *v);
    if (!v) rt_fatal("DKB-MOVIE-VP9: out of memory for a video decoder");
    plat_mutex_init(&v->lock);
    plat_cond_init(&v->wake);
    return v;
}

static void vd_stop(Vd *v) {
    if (v->worker) {
        plat_mutex_lock(&v->lock);
        v->quit = 1;
        plat_cond_broadcast(&v->wake);
        plat_mutex_unlock(&v->lock);
        plat_thread_join(v->worker);
        v->worker = NULL;
        v->quit = 0;
    }
    if (v->vpx_live) vpx_codec_destroy(&v->vpx);
    v->vpx_live = 0;
    plat_mutex_lock(&v->lock);
    drop_queues(v);
    v->generation++;
    v->reset = 0;
    v->nframes = 0;
    memset(v->held, 0, sizeof v->held);
    plat_mutex_unlock(&v->lock);
}

// ---- movie::VideoDecoder: the object's first 8 bytes (subsdk0's implementation pointer) hold the record
static void vd_ctor(Cpu *c) {
    *(Vd **)arg_p(c, 0) = NULL;
    ret_x(c, arg_x(c, 0));
}

static void vd_dtor(Cpu *c) {
    Vd **slot = arg_p(c, 0), *v = *slot;
    if (v) {
        vd_stop(v);
        plat_mutex_lock(&g_free_lock);
        v->next_free = g_free;
        g_free = v;
        plat_mutex_unlock(&g_free_lock);
        *slot = NULL;
    }
    ret_x(c, arg_x(c, 0));
}

// Result Initialize(void *memory, size_t size, MediaMimeType, VideoDecoderOutputFormat, VideoCodecMode)
static void vd_initialize(Cpu *c) {
    Vd **slot = arg_p(c, 0), *v = *slot;
    if (!v) *slot = v = vd_get();
    vd_stop(v);
    v->decoded = v->shown = 0;
    codec_open(v);
    v->worker = plat_thread_new(worker, v, "dkb movie vp9");
    if (!v->worker) rt_fatal("DKB-MOVIE-VP9: cannot start the decoder thread");
    rt_log("DKB-MOVIE-VP9: video decoder %p up (libvpx %s; mime %u, output format %u, mode %u)", (void *)v,
           vpx_codec_version_str(), (unsigned)arg_x(c, 3), (unsigned)arg_x(c, 4), (unsigned)arg_x(c, 5));
    ret_x(c, 0);
}

// Configure / Start / Stop: VP9 carries what libvpx needs in band
static void vd_ok(Cpu *c) { ret_x(c, 0); }

static void vd_finalize(Cpu *c) {
    Vd *v = vd_of(c);
    if (v) {
        rt_log("DKB-MOVIE-VP9: video decoder %p finalised (%llu pictures decoded, %llu shown)", (void *)v,
               (unsigned long long)v->decoded, (unsigned long long)v->shown);
        vd_stop(v);
    }
    ret_x(c, 0);
}

// Result RegisterVideoFrame(VideoFrame **frames, int count): each buffer starts as NV12 black
static void vd_register(Cpu *c) {
    Vd *v = vd_of(c);
    const uint64_t *frames = arg_p(c, 1);
    int n = (int)arg_x(c, 2);
    if (!v || n < 0 || n > FRAMES || (n && !frames)) rt_fatal("DKB-MOVIE-VP9: RegisterVideoFrame of %d frames", n);
    plat_mutex_lock(&v->lock);
    for (int i = 0; i < n; i++) {
        uint8_t *fr = (uint8_t *)(uintptr_t)frames[i], *buf = NULL;
        uint64_t cap = 0;
        v->frame[i] = frames[i];
        v->held[i] = 0;
        if (fr) memcpy(&buf, fr + 0x08, 8), memcpy(&cap, fr + 0x48, 8);
        if (!buf || cap < 3 || cap > (64u << 20)) continue;
        memset(buf, 16, (size_t)(cap / 3 * 2));
        memset(buf + cap / 3 * 2, 128, (size_t)(cap - cap / 3 * 2));
        nvn_guest_written(addr_of(buf), cap);
    }
    v->nframes = n;
    plat_mutex_unlock(&v->lock);
    ret_x(c, 0);
}

// Result GetInputReadyEvent(SystemEventType **out) / GetOutputReadyEvent(SystemEventType **out)
static void ready_event(Cpu *c, int input) {
    Vd *v = vd_of(c);
    uint64_t *out = arg_p(c, 1);
    if (!v || !out) rt_fatal("DKB-MOVIE-VP9: Get%sReadyEvent before Initialize", input ? "Input" : "Output");
    if (!v->bound) {
        kern_bind_system_event(c, addr_of(v->ev_in), 0);
        kern_bind_system_event(c, addr_of(v->ev_out), 0);
        plat_mutex_lock(&v->lock);
        v->bound = 1;
        plat_mutex_unlock(&v->lock);
    }
    plat_mutex_lock(&v->lock);
    settle_events(c, v);
    plat_mutex_unlock(&v->lock);
    *out = addr_of(input ? v->ev_in : v->ev_out);
    ret_x(c, 0);
}
static void vd_input_event(Cpu *c) { ready_event(c, 1); }
static void vd_output_event(Cpu *c) { ready_event(c, 0); }

// Result Flush(): samples and pictures not yet acquired are dropped; a seek or a restart follows
static void vd_flush(Cpu *c) {
    Vd *v = vd_of(c);
    if (v) {
        plat_mutex_lock(&v->lock);
        v->generation++;
        drop_queues(v);
        v->reset = 1;
        settle_events(c, v);
        plat_mutex_unlock(&v->lock);
    }
    ret_x(c, 0);
}

// Result DecodeAsync(const nn::movie::Buffer *sample): a copy is queued for the worker
static void vd_decode(Cpu *c) {
    Vd *v = vd_of(c);
    const uint8_t *b = arg_p(c, 1);
    if (!v || !b) rt_fatal("DKB-MOVIE-VP9: DecodeAsync(%p) on decoder %p", (const void *)b, (void *)v);
    const uint8_t *data;
    uint64_t size;
    int64_t ts;
    uint32_t flags;
    memcpy(&data, b, 8), memcpy(&size, b + 0x10, 8), memcpy(&ts, b + 0x18, 8), memcpy(&flags, b + 0x20, 4);
    int eos = (flags & FLAG_EOS) != 0;
    plat_mutex_lock(&v->lock);
    if (v->eos_in) {
        plat_mutex_unlock(&v->lock);
        ret_x(c, 0);
        return;
    }
    if (v->in_n == SAMPLES || (!eos && !input_ready(v))) {
        settle_events(c, v);
        plat_mutex_unlock(&v->lock);
        ret_x(c, QUEUE_FULL);
        return;
    }
    Sample s = {NULL, 0, ts, eos};
    if (data && size) {
        if (!(s.data = malloc((size_t)size))) rt_fatal("DKB-MOVIE-VP9: out of memory for a %llu-byte sample",
                                                        (unsigned long long)size);
        memcpy(s.data, data, (size_t)size);
        s.size = (size_t)size;
    }
    v->in[(v->in_head + v->in_n) % SAMPLES] = s;
    v->in_n++;
    v->eos_in = eos;
    plat_cond_signal(&v->wake);
    settle_events(c, v);
    plat_mutex_unlock(&v->lock);
    ret_x(c, 0);
}

// Result AcquireVideoFrame(VideoFrame **out): the oldest picture, in a registered frame the game does not hold
static void vd_acquire(Cpu *c) {
    Vd *v = vd_of(c);
    uint64_t *out = arg_p(c, 1);
    if (!v || !out) {
        ret_x(c, NO_FRAME);
        return;
    }
    plat_mutex_lock(&v->lock);
    int f = 0;
    while (f < v->nframes && v->held[f]) f++;
    if (!v->out_n || f == v->nframes) {
        settle_events(c, v);
        plat_mutex_unlock(&v->lock);
        *out = 0;
        ret_x(c, NO_FRAME);
        return;
    }
    Picture p = v->out[v->out_head];
    v->out_head = (v->out_head + 1) % SLOTS;
    v->out_n--;
    v->held[f] = 1;
    v->shown += !p.eos;
    uint64_t shown = v->shown, frame = v->frame[f];
    settle_events(c, v);
    plat_mutex_unlock(&v->lock);
    uint8_t *fr = (uint8_t *)(uintptr_t)frame, *buf;
    uint64_t cap, ysz = (uint64_t)p.w * p.h, csz = ysz / 2, zero = 0;
    memcpy(&buf, fr + 0x08, 8), memcpy(&cap, fr + 0x48, 8);
    if (p.nv12 && buf && cap >= ysz + csz) {
        memcpy(buf, p.nv12, (size_t)(ysz + csz));
        nvn_guest_written(addr_of(buf), ysz + csz);
        uint64_t chroma = addr_of(buf + ysz);
        memcpy(fr + 0x10, &chroma, 8);
        memcpy(fr + 0x28, &ysz, 8), memcpy(fr + 0x30, &csz, 8);
    } else {
        memcpy(fr + 0x28, &zero, 8), memcpy(fr + 0x30, &zero, 8);
        if (p.nv12) {
            static volatile int32_t logged;
            if (!plat_xchg32(&logged, 1))
                rt_log("DKB-MOVIE-VP9: frame buffer of %#llx bytes is smaller than a %dx%d NV12 picture",
                       (unsigned long long)cap, p.w, p.h);
        }
    }
    memcpy(fr + 0x78, &p.ts, 8);
    fr[0x88] = (uint8_t)p.eos;
    if (p.nv12 && shown == 1) rt_log("DKB-MOVIE-VP9: decoder %p first picture %dx%d shown", (void *)v, p.w, p.h);
    if (p.eos) rt_log("DKB-MOVIE-VP9: decoder %p reached the end of its movie after %llu pictures", (void *)v,
                      (unsigned long long)shown);
    free(p.nv12);
    *out = frame;
    ret_x(c, 0);
}

// Result ReleaseVideoFrame(VideoFrame *)
static void vd_release(Cpu *c) {
    Vd *v = vd_of(c);
    if (v) {
        plat_mutex_lock(&v->lock);
        for (int i = 0; i < v->nframes; i++)
            if (v->frame[i] == arg_x(c, 1)) v->held[i] = 0;
        settle_events(c, v);
        plat_mutex_unlock(&v->lock);
    }
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN5movie12VideoDecoderC1Ev", vd_ctor);
HOST_OVERRIDE("_ZN5movie12VideoDecoderD1Ev", vd_dtor);
HOST_OVERRIDE("_ZN5movie12VideoDecoder10InitializeEPvmN2nn5movie13MediaMimeTypeENS_24VideoDecoderOutputFormatENS_14VideoCodecModeE",
              vd_initialize);
HOST_OVERRIDE("_ZN5movie12VideoDecoder9ConfigureEPKN2nn5movie13MediaPropertyEPKvm", vd_ok);
HOST_OVERRIDE("_ZN5movie12VideoDecoder8FinalizeEv", vd_finalize);
HOST_OVERRIDE("_ZN5movie12VideoDecoder18RegisterVideoFrameEPPNS_10VideoFrameEi", vd_register);
HOST_OVERRIDE("_ZN5movie12VideoDecoder18GetInputReadyEventEPPN2nn2os15SystemEventTypeE", vd_input_event);
HOST_OVERRIDE("_ZN5movie12VideoDecoder19GetOutputReadyEventEPPN2nn2os15SystemEventTypeE", vd_output_event);
HOST_OVERRIDE("_ZN5movie12VideoDecoder5StartEv", vd_ok);
HOST_OVERRIDE("_ZN5movie12VideoDecoder4StopEv", vd_ok);
HOST_OVERRIDE("_ZN5movie12VideoDecoder5FlushEv", vd_flush);
HOST_OVERRIDE("_ZN5movie12VideoDecoder11DecodeAsyncEPKN2nn5movie6BufferE", vd_decode);
HOST_OVERRIDE("_ZN5movie12VideoDecoder17AcquireVideoFrameEPPNS_10VideoFrameE", vd_acquire);
HOST_OVERRIDE("_ZN5movie12VideoDecoder17ReleaseVideoFrameEPNS_10VideoFrameE", vd_release);
