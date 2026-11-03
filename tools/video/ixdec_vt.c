/*
 * ixdec_vt.c - hardware video decoding, stage 3 (b243): the Iris Xe's video engine as a VideoToolbox decoder.
 *
 * VideoToolbox side: a decoder plugin (CMBaseObject + VTVideoDecoderClass, registered in-process with
 * VTRegisterVideoDecoderWithInfo), the interface as published in WebKit's SPI headers (spi/CMBaseObjectSPI.h, BSD) and
 * as used by mickys/nvenc's vt/nvencvt_dec.c - the class layout, the session calls, the registration and the output
 * pixel buffer attributes here follow that file (MIT License, Copyright (c) 2026 mickys; see NOTICE).
 * Decoding side: FFmpeg's libavcodec with its Vulkan Video hwaccel on a Vulkan device made here through Mesa ANV (the
 * caller hands over ANV's vkGetInstanceProcAddr, so no Vulkan loader or ICD file is involved). Stage 2 (b242) showed
 * that path bit-exact against the CPU for H.264 on this GPU.
 *
 * Codecs: H.264 ('avc1', 'avc3'), VP9 ('vp09'), AV1 ('av01'), 8-bit 4:2:0 -> NV12 pixel buffers. Every sample's
 * frame goes back from its own decodeFrame call: H.264 in decode order (b248, the picture caught at its buffer
 * allocation), VP9 / AV1 as libavcodec outputs them (they do not reorder); a sample with nothing to show is dropped.
 * Pictures leave the GPU through a copy into the pixel buffer (stage 3b can make that zero-copy).
 *
 * Log: IXDEC_LOG=1, or create /tmp/ixdec.log (then each process writes /tmp/ixdec-<pid>.log).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

typedef struct CMBaseProtocolVTable CMBaseProtocolVTable;
typedef struct CMBaseProtocolTable CMBaseProtocolTable;
#include "spi/CMBaseObjectSPI.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
#include <libavutil/log.h>

/* ---- the VideoToolbox decoder SPI (as in WebKit's VTVideoDecoderSPI.h / mickys/nvenc) ---- */
typedef CMBaseObjectRef VTVideoDecoderRef;
typedef void *VTVideoDecoderSession;
typedef void *VTVideoDecoderFrame;
extern CMBaseClassID VTVideoDecoderGetClassID(void);
extern CVPixelBufferPoolRef VTDecoderSessionGetPixelBufferPool(VTVideoDecoderSession);
extern OSStatus VTDecoderSessionSetPixelBufferAttributes(VTVideoDecoderSession, CFDictionaryRef);
extern CFDictionaryRef VTDecoderSessionGetDestinationPixelBufferAttributes(VTVideoDecoderSession);
extern OSStatus VTDecoderSessionEmitDecodedFrame(VTVideoDecoderSession, VTVideoDecoderFrame, OSStatus, VTDecodeInfoFlags, CVImageBufferRef);

#pragma pack(push, 4)
typedef struct {
    uint32_t version;
    OSStatus (*startSession)(VTVideoDecoderRef, VTVideoDecoderSession, CMVideoFormatDescriptionRef);
    OSStatus (*decodeFrame)(VTVideoDecoderRef, VTVideoDecoderFrame, CMSampleBufferRef, VTDecodeFrameFlags, VTDecodeInfoFlags *);
    OSStatus (*copySupportedPropertyDictionary)(VTVideoDecoderRef, CFDictionaryRef *);
    OSStatus (*setProperties)(VTVideoDecoderRef, CFDictionaryRef);
    OSStatus (*copySerializableProperties)(VTVideoDecoderRef, CFAllocatorRef, CFDictionaryRef *);
    Boolean  (*canAcceptFormatDescription)(VTVideoDecoderRef, CMFormatDescriptionRef);
    OSStatus (*finishDelayedFrames)(VTVideoDecoderRef);
    void *reserved[3];
} DecoderClass;
#pragma pack(pop)
typedef struct { CMBaseVTable base; const DecoderClass *decoderClass; } DecoderVTable;

enum { CODEC_AVC1 = 'avc1', CODEC_AVC3 = 'avc3', CODEC_VP9 = 'vp09', CODEC_AV1 = 'av01' };

/* ---- log ---- */
static int gLog = -1; static FILE *gLogF;
static void ixlog_init(void)
{
    if (gLog >= 0) return;
    gLog = getenv("IXDEC_LOG") != NULL;
    /* b256: with IXDEC_LOG, FFmpeg says why its Vulkan decoder setup failed (capabilities, profile, sizes) - in Brave
       (11-02) only "hwaccel initialisation returned error" came out at the default level */
    if (gLog) av_log_set_level(AV_LOG_VERBOSE);
    if (access("/tmp/ixdec.log", F_OK) == 0) {
        char p[64]; snprintf(p, sizeof p, "/tmp/ixdec-%d.log", (int)getpid());
        if ((gLogF = fopen(p, "a"))) gLog = 1;
    }
}
/* b249: inside an app the plugin hands its own log (sandboxed apps cannot write /tmp) */
static void (*gHostLog)(const char *, ...);
#define LOG(...) do { if (gHostLog) { char m_[512]; snprintf(m_, sizeof m_, __VA_ARGS__); gHostLog("ixdec: %s", m_); \
                                      if (gLog > 0) fprintf(stderr, "ixdec[%s %d]: %s\n", getprogname(), getpid(), m_); } \
                      else if (gLog > 0) { FILE *f_ = gLogF ? gLogF : stderr; fprintf(f_, "ixdec[%s]: ", getprogname()); \
                      fprintf(f_, __VA_ARGS__); fputc('\n', f_); fflush(f_); } } while (0)
static double nowms(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e6; }

/* ---- one Vulkan device per process, made through ANV (shared by every session) ---- */
static pthread_mutex_t gDevLock = PTHREAD_MUTEX_INITIALIZER;
static PFN_vkGetInstanceProcAddr gGipa;
static AVBufferRef *gDev; static int gDevFailed;
/* b258: a decode that fails after the device was made (11-03: VP9 on Tiger Lake hung the video engine ->
   VK_ERROR_DEVICE_LOST, every later session in that process failed too) makes the next session build a new device;
   sessions still running keep the old one through their own reference */
static int gDevSuspect;
static VkVideoCodecOperationFlagsKHR gOps;      /* what the decode queue does */

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

/* Features FFmpeg's decoders use, each only where the device has it (the same struct is handed to FFmpeg, which reads
   what is enabled from it). Kept for the life of the process. */
static VkPhysicalDeviceFeatures2 gF2;
static VkPhysicalDeviceVulkan11Features gF11;
static VkPhysicalDeviceVulkan12Features gF12;
static VkPhysicalDeviceVulkan13Features gF13;
static VkPhysicalDeviceVideoMaintenance1FeaturesKHR gFvm1;
#ifdef VK_KHR_video_decode_vp9
static VkPhysicalDeviceVideoDecodeVP9FeaturesKHR gFvp9;
#endif
static const char *gExts[16]; static int gNExts; static int gHostMap;

static int make_device(void)
{
    if (gDev && gDevSuspect) { LOG("previous decode failed on this device - making a new one"); av_buffer_unref(&gDev); gDevSuspect = 0; }
    if (gDev) return 0;
    if (gDevFailed || !gGipa) return -1;
    gDevFailed = 1;   /* until it worked */
    PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)gGipa(NULL, "vkCreateInstance");
    if (!createInstance) { LOG("no vkCreateInstance"); return -1; }
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "ixdec", 1, "ixdec", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    VkInstance inst;
    VkResult r = createInstance(&ici, NULL, &inst);
    if (r) { LOG("vkCreateInstance -> %d", r); return -1; }
#define IP(n) PFN_##n n = (PFN_##n)gGipa(inst, #n)
    IP(vkEnumeratePhysicalDevices); IP(vkGetPhysicalDeviceProperties); IP(vkEnumerateDeviceExtensionProperties);
    IP(vkGetPhysicalDeviceQueueFamilyProperties2); IP(vkGetPhysicalDeviceFeatures2); IP(vkCreateDevice); IP(vkDestroyInstance);
    if (!vkEnumeratePhysicalDevices || !vkCreateDevice || !vkGetPhysicalDeviceQueueFamilyProperties2 || !vkGetPhysicalDeviceFeatures2) { LOG("instance functions missing"); return -1; }
    uint32_t n = 8; VkPhysicalDevice pds[8], pd = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(inst, &n, pds);
    for (uint32_t i = 0; i < n && !pd; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (p.vendorID == 0x8086) pd = pds[i]; }
    if (!pd) { LOG("no Intel GPU"); vkDestroyInstance(inst, NULL); return -1; }

    uint32_t nq = 16; VkQueueFamilyProperties2 qp[16]; VkQueueFamilyVideoPropertiesKHR qv[16];
    for (uint32_t i = 0; i < 16; i++) {
        qv[i] = (VkQueueFamilyVideoPropertiesKHR){ VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR, NULL, 0 };
        qp[i] = (VkQueueFamilyProperties2){ VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2, &qv[i] };
    }
    vkGetPhysicalDeviceQueueFamilyProperties2(pd, &nq, qp);
    int gq = -1, dq = -1;
    for (uint32_t i = 0; i < nq; i++) {
        const VkQueueFlags f = qp[i].queueFamilyProperties.queueFlags;
        if (gq < 0 && (f & VK_QUEUE_GRAPHICS_BIT) && (f & VK_QUEUE_COMPUTE_BIT)) gq = (int)i;
        if (dq < 0 && (f & VK_QUEUE_VIDEO_DECODE_BIT_KHR)) dq = (int)i;
    }
    if (gq < 0 || dq < 0) { LOG("queues: graphics %d, decode %d - no video engine", gq, dq); vkDestroyInstance(inst, NULL); return -1; }
    gOps = qv[dq].videoCodecOperations;
    const uint32_t ndq = qp[dq].queueFamilyProperties.queueCount > 2 ? 2 : qp[dq].queueFamilyProperties.queueCount;

    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ext = calloc(ne ? ne : 1, sizeof *ext);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);
    static const char *want[] = { "VK_KHR_video_queue", "VK_KHR_video_decode_queue", "VK_KHR_video_decode_h264", "VK_KHR_video_decode_h265",
                                  "VK_KHR_video_decode_av1", "VK_KHR_video_decode_vp9", "VK_KHR_video_maintenance1", "VK_KHR_video_maintenance2" };
    gNExts = 0;
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) if (has_ext(ext, ne, want[i])) gExts[gNExts++] = want[i];
    const int vm1 = has_ext(ext, ne, "VK_KHR_video_maintenance1"), vp9 = has_ext(ext, ne, "VK_KHR_video_decode_vp9");
    /* b259 (stage 3c): with VK_EXT_external_memory_host FFmpeg's download (av_hwframe_transfer_data) imports the
       destination - the CVPixelBuffer's IOSurface pages - as GPU memory (i915 userptr, pinned through NMXGPU) and the
       GPU copies the picture straight into it. Without it the GPU copies into a staging buffer and the processor
       copies that again (11-03: 2.8-3.9 ms per 1080p picture). FFmpeg falls back to staging by itself for any plane it
       cannot import. Off: /Library/GPUBundles/nvmtl-video-nohostmap or IXDEC_NO_HOSTMAP=1. */
    gHostMap = has_ext(ext, ne, "VK_EXT_external_memory_host") && !getenv("IXDEC_NO_HOSTMAP") &&
               access("/Library/GPUBundles/nvmtl-video-nohostmap", F_OK) != 0;
    if (gHostMap) gExts[gNExts++] = "VK_EXT_external_memory_host";
    LOG("picture download: %s", gHostMap ? "GPU writes into the pixel buffer (external_memory_host)" :
        has_ext(ext, ne, "VK_EXT_external_memory_host") ? "staging copy (host import switched off)" : "staging copy (driver has no external_memory_host)");
    free(ext);

    /* supported features -> the ones to enable */
    VkPhysicalDeviceVulkan11Features s11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceVulkan12Features s12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &s11 };
    VkPhysicalDeviceVulkan13Features s13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &s12 };
    VkPhysicalDeviceVideoMaintenance1FeaturesKHR svm1 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR, &s13 };
    VkPhysicalDeviceFeatures2 s2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, vm1 ? (void *)&svm1 : (void *)&s13 };
#ifdef VK_KHR_video_decode_vp9
    VkPhysicalDeviceVideoDecodeVP9FeaturesKHR svp9 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_DECODE_VP9_FEATURES_KHR, s2.pNext };
    if (vp9) s2.pNext = &svp9;
#endif
    vkGetPhysicalDeviceFeatures2(pd, &s2);
    gF11 = (VkPhysicalDeviceVulkan11Features){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    gF12 = (VkPhysicalDeviceVulkan12Features){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &gF11 };
    gF13 = (VkPhysicalDeviceVulkan13Features){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &gF12 };
    gF11.samplerYcbcrConversion = s11.samplerYcbcrConversion;
    gF12.timelineSemaphore = s12.timelineSemaphore; gF12.hostQueryReset = s12.hostQueryReset;
    gF12.bufferDeviceAddress = s12.bufferDeviceAddress; gF12.scalarBlockLayout = s12.scalarBlockLayout;
    gF13.synchronization2 = s13.synchronization2; gF13.maintenance4 = s13.maintenance4;
    void *chain = &gF13;
    if (vm1) { gFvm1 = (VkPhysicalDeviceVideoMaintenance1FeaturesKHR){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR, chain, svm1.videoMaintenance1 }; chain = &gFvm1; }
#ifdef VK_KHR_video_decode_vp9
    if (vp9) { gFvp9 = (VkPhysicalDeviceVideoDecodeVP9FeaturesKHR){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_DECODE_VP9_FEATURES_KHR, chain, svp9.videoDecodeVP9 }; chain = &gFvp9; }
#endif
    gF2 = (VkPhysicalDeviceFeatures2){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain };
    if (!gF12.timelineSemaphore || !gF13.synchronization2) { LOG("no timeline semaphores / synchronization2"); vkDestroyInstance(inst, NULL); return -1; }

    const float prio[2] = { 1.0f, 1.0f };
    VkDeviceQueueCreateInfo qci[2] = {
        { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, (uint32_t)gq, 1, prio },
        { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, (uint32_t)dq, ndq, prio } };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &gF2, 0, gq == dq ? 1u : 2u, qci, 0, NULL, (uint32_t)gNExts, gExts, NULL };
    VkDevice dev;
    r = vkCreateDevice(pd, &dci, NULL, &dev);
    if (r) { LOG("vkCreateDevice -> %d", r); vkDestroyInstance(inst, NULL); return -1; }

    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (!ref) return -1;
    AVHWDeviceContext *hd = (AVHWDeviceContext *)ref->data;
    AVVulkanDeviceContext *vk = hd->hwctx;
    vk->get_proc_addr = gGipa; vk->inst = inst; vk->phys_dev = pd; vk->act_dev = dev;
    vk->device_features = gF2;
    vk->enabled_dev_extensions = gExts; vk->nb_enabled_dev_extensions = gNExts;
    vk->qf[0] = (AVVulkanDeviceQueueFamily){ gq, 1, (VkQueueFlagBits)(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT), 0 };
    vk->qf[1] = (AVVulkanDeviceQueueFamily){ dq, (int)ndq, VK_QUEUE_VIDEO_DECODE_BIT_KHR, (VkVideoCodecOperationFlagBitsKHR)gOps };
    vk->nb_qf = 2;
#if FF_API_VULKAN_FIXED_QUEUES
    vk->queue_family_index = gq; vk->nb_graphics_queues = 1;
    vk->queue_family_comp_index = gq; vk->nb_comp_queues = 1;
    vk->queue_family_tx_index = gq; vk->nb_tx_queues = 1;
    vk->queue_family_decode_index = dq; vk->nb_decode_queues = (int)ndq;
    vk->queue_family_encode_index = -1; vk->nb_encode_queues = 0;
#endif
    int e = av_hwdevice_ctx_init(ref);
    if (e < 0) { LOG("av_hwdevice_ctx_init -> %d", e); av_buffer_unref(&ref); return -1; }
    gDev = ref; gDevFailed = 0;
    LOG("Vulkan device ready: graphics family %d, decode family %d (%u queues, ops 0x%x), %d extensions", gq, dq, ndq, (unsigned)gOps, gNExts);
    return 0;
}

/* ---- a decoder instance ---- */
typedef struct {
    pthread_mutex_t lock;
    VTVideoDecoderSession session;
    FourCharCode codec;
    AVCodecContext *av;
    AVPacket *pkt; AVFrame *frm;
    int w, h; OSType pf; bool full;
    CFDictionaryRef colour;
    uint8_t *buf; size_t cap;        /* the sample, padded for libavcodec */
    unsigned frames, dropped, errors; double tDec, tCopy;
    /* b244: pictures leave libavcodec in display order, up to the stream's reorder depth later than their sample went
       in (H.264 with B frames: 2 samples later in the 10-31 test), so each sample's VideoToolbox frame is kept under the
       number given as the packet's pts and emitted when the picture with that number comes out (b243 emitted each
       picture with the frame of the sample that happened to release it: right pictures, wrong timestamps) */
    struct { int64_t id; VTVideoDecoderFrame frame; } pend[64]; unsigned npend; int64_t nextId;
    /* b246: frames ready to go back to VideoToolbox. They are emitted only after s->lock is released: b244 emitted
       while holding it and the first run (11-01) hung without a line of output - VideoToolbox calls into the decoder
       holding its own session lock, and the emit takes that lock (lock-order inversion) */
    struct OutJob { VTVideoDecoderSession ses; VTVideoDecoderFrame frame; OSStatus res; VTDecodeInfoFlags fl; CVPixelBufferRef pb; } out[96];
    unsigned nout;
    /* b248 (H.264): the picture being decoded, caught when libavcodec asks for its buffer (ix_get_buffer2) and handed back
       with its own sample as soon as that sample is decoded - decode order, as VideoToolbox's decoders work and the
       client reorders by timestamp. b247 waited for libavcodec's display-order output and the last pictures only came
       out at finishDelayedFrames, which VideoToolbox never called: the test stopped at 297 of 300 pictures. */
    AVFrame *cur; int gotCur;
} DState;
typedef struct OutJob OutJob;
static DState *dstate(VTVideoDecoderRef o) { return *(DState **)CMBaseObjectGetDerivedStorage(o); }

static enum AVPixelFormat pick_vulkan(AVCodecContext *c, const enum AVPixelFormat *fmts)
{
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) if (*p == AV_PIX_FMT_VULKAN) {
        static int said; if (said++ < 4) LOG("get_format: Vulkan for %s %dx%d", c->codec ? c->codec->name : "?", c->coded_width, c->coded_height);
        return *p;
    }
    LOG("libavcodec offers no Vulkan format for this stream - refused (no CPU decoding behind a hardware decoder's name)");
    return AV_PIX_FMT_NONE;
}

/* queue a frame for VideoToolbox (caller holds s->lock); the pixel buffer is retained for the queue */
static void queue_out(DState *s, VTVideoDecoderFrame frame, OSStatus res, VTDecodeInfoFlags fl, CVPixelBufferRef pb)
{
    if (s->nout == sizeof s->out / sizeof s->out[0]) { LOG("output queue full - a frame is lost"); return; }
    s->out[s->nout++] = (OutJob){ s->session, frame, res, fl, pb ? CVPixelBufferRetain(pb) : NULL };
}
static void emit_pending(DState *s, unsigned i, OSStatus res, VTDecodeInfoFlags fl, CVPixelBufferRef pb)
{
    queue_out(s, s->pend[i].frame, res, fl, pb);
    memmove(&s->pend[i], &s->pend[i + 1], (s->npend - i - 1) * sizeof s->pend[0]);
    s->npend--;
}
/* hand the queued frames to VideoToolbox - called WITHOUT s->lock held */
static void flush_out(DState *s)
{
    OutJob jobs[96]; unsigned n;
    pthread_mutex_lock(&s->lock);
    n = s->nout; memcpy(jobs, s->out, n * sizeof jobs[0]); s->nout = 0;
    pthread_mutex_unlock(&s->lock);
    for (unsigned i = 0; i < n; i++) {
        if (jobs[i].ses) VTDecoderSessionEmitDecodedFrame(jobs[i].ses, jobs[i].frame, jobs[i].res, jobs[i].fl, jobs[i].pb);
        if (jobs[i].pb) CVPixelBufferRelease(jobs[i].pb);
    }
}
static OSStatus b_invalidate(CMBaseObjectRef o)
{
    DState *s = dstate(o); if (!s) return noErr;
    pthread_mutex_lock(&s->lock);
    if (s->av || s->frames || s->errors)   /* b250: also a session that decoded nothing (Brave 11-02: AV1 started, no picture) */
        LOG("session end: %u pictures, %u dropped, %u errors, %lld samples in; per picture %.2f ms decode, %.2f ms copy",
                                    s->frames, s->dropped, s->errors, (long long)s->nextId, s->frames ? s->tDec / s->frames : 0, s->frames ? s->tCopy / s->frames : 0);
    while (s->npend) emit_pending(s, 0, noErr, kVTDecodeInfo_FrameDropped, NULL);   /* frames still waiting: dropped */
    avcodec_free_context(&s->av); av_packet_free(&s->pkt); av_frame_free(&s->frm); av_frame_free(&s->cur);
    if (s->colour) { CFRelease(s->colour); s->colour = NULL; }
    pthread_mutex_unlock(&s->lock);
    flush_out(s);
    return noErr;
}
static void b_finalize(CMBaseObjectRef o)
{
    DState *s = dstate(o); if (!s) return;
    b_invalidate(o); pthread_mutex_destroy(&s->lock); free(s->buf); free(s);
    *(DState **)CMBaseObjectGetDerivedStorage(o) = NULL;
}
static CFStringRef b_desc(CMBaseObjectRef o)
{
    const FourCharCode c = dstate(o)->codec;
    return c == CODEC_VP9 ? CFSTR("Iris Xe VP9 decoder (Vulkan Video)") : c == CODEC_AV1 ? CFSTR("Iris Xe AV1 decoder (Vulkan Video)")
                          : CFSTR("Iris Xe H.264 decoder (Vulkan Video)");
}
static OSStatus b_copyProperty(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef al, void *out)
{
    (void)o; (void)al;
    if (CFEqual(key, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder)) { *(CFTypeRef *)out = kCFBooleanTrue; return noErr; }
    static int said; if (said++ < 12) { char k[128] = "?"; CFStringGetCString(key, k, sizeof k, kCFStringEncodingUTF8); LOG("copyProperty %s: not available", k); }
    return kCMBaseObjectError_ValueNotAvailable;
}
static OSStatus b_setProperty(CMBaseObjectRef o, CFStringRef key, CFTypeRef v)
{
    (void)o; (void)v;
    static int said; if (said++ < 12) { char k[128] = "?"; CFStringGetCString(key, k, sizeof k, kCFStringEncodingUTF8); LOG("setProperty %s", k); }
    return noErr;
}

/* the session's output: NV12 of the picture's size, IOSurface-backed and Metal-compatible (as mickys/nvenc's set_output) */
static OSStatus set_output(DState *s)
{
    CFNumberRef npf = CFNumberCreate(NULL, kCFNumberSInt32Type, &s->pf), nw = CFNumberCreate(NULL, kCFNumberIntType, &s->w),
                nh = CFNumberCreate(NULL, kCFNumberIntType, &s->h);
    CFDictionaryRef ios = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *k[] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey,
                        kCVPixelBufferMetalCompatibilityKey };
    const void *v[] = { npf, nw, nh, ios, kCFBooleanTrue };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const OSStatus st = VTDecoderSessionSetPixelBufferAttributes(s->session, attrs);
    CFRelease(attrs); CFRelease(ios); CFRelease(npf); CFRelease(nw); CFRelease(nh);
    return st;
}

static int ix_get_buffer2(AVCodecContext *c, AVFrame *f, int flags)
{
    const int r = avcodec_default_get_buffer2(c, f, flags);
    DState *s = c->opaque;
    if (r >= 0 && s && s->cur) { av_frame_unref(s->cur); if (av_frame_ref(s->cur, f) >= 0) s->gotCur = 1; }
    return r;
}
static OSStatus d_startSession(VTVideoDecoderRef o, VTVideoDecoderSession session, CMVideoFormatDescriptionRef fmt)
{
    DState *s = dstate(o);
    pthread_mutex_lock(&s->lock);
    s->session = session;
    OSStatus res = noErr;
    pthread_mutex_lock(&gDevLock); const int dev = make_device(); pthread_mutex_unlock(&gDevLock);
    const CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(fmt);
    enum AVCodecID id = s->codec == CODEC_VP9 ? AV_CODEC_ID_VP9 : s->codec == CODEC_AV1 ? AV_CODEC_ID_AV1 : AV_CODEC_ID_H264;
    const VkVideoCodecOperationFlagsKHR need = id == AV_CODEC_ID_VP9 ? 0x8 /* VP9 decode */ : id == AV_CODEC_ID_AV1 ? VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR
                                                                                            : VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    CFDictionaryRef atoms = CMFormatDescriptionGetExtension(fmt, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
    const char *box = id == AV_CODEC_ID_H264 ? "avcC" : id == AV_CODEC_ID_AV1 ? "av1C" : "vpcC";
    CFStringRef bk = CFStringCreateWithCString(NULL, box, kCFStringEncodingASCII);
    CFDataRef cfg = atoms && CFGetTypeID(atoms) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(atoms, bk) : NULL;
    CFRelease(bk);
    if (cfg && CFGetTypeID(cfg) != CFDataGetTypeID()) cfg = NULL;
    /* 8-bit 4:2:0 only (NV12 out): H.264 profile from avcC, VP9 depth/subsampling from vpcC, AV1 from av1C */
    bool ok = dev == 0 && (gOps & need) && dim.width >= 16 && dim.height >= 16 && dim.width <= 4096 && dim.height <= 4096;
    s->full = false;
    if (ok && cfg && id == AV_CODEC_ID_H264 && CFDataGetLength(cfg) >= 4) {
        const uint8_t pr = CFDataGetBytePtr(cfg)[1];
        ok = pr == 66 || pr == 77 || pr == 100;   /* Baseline/Constrained, Main, High: 8-bit 4:2:0 */
    }
    if (ok && cfg && id == AV_CODEC_ID_VP9 && CFDataGetLength(cfg) >= 8) {
        const uint8_t b = CFDataGetBytePtr(cfg)[6]; ok = (b >> 4) == 8 && ((b >> 1) & 7) <= 1; s->full = b & 1;
    }
    if (ok && cfg && id == AV_CODEC_ID_AV1 && CFDataGetLength(cfg) >= 4) {
        const uint8_t b = CFDataGetBytePtr(cfg)[2]; ok = !((b >> 6) & 1) && ((b >> 3) & 1) && ((b >> 2) & 1);   /* 8-bit, 4:2:0 */
    }
    LOG("startSession %s %dx%d, config box %s (%ld bytes) -> %s", box, dim.width, dim.height, cfg ? "yes" : "no", cfg ? (long)CFDataGetLength(cfg) : 0L,
        ok ? "taken" : dev ? "REFUSED (no Vulkan device)" : "REFUSED (not supported here)");
    if (!ok) { pthread_mutex_unlock(&s->lock); return kVTVideoDecoderUnsupportedDataFormatErr; }

    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec || !(s->av = avcodec_alloc_context3(codec))) { res = kVTVideoDecoderMalfunctionErr; goto out; }
    s->av->hw_device_ctx = av_buffer_ref(gDev);
    s->av->get_format = pick_vulkan;
    if (id == AV_CODEC_ID_H264) { s->av->opaque = s; s->av->get_buffer2 = ix_get_buffer2; s->cur = av_frame_alloc(); }
    s->av->thread_count = 1;
    /* b257: FFmpeg asks the video engine for AV1 WITH film grain synthesis unless grain is exported as side data, and
       ANV on Tiger Lake has no film grain in its AV1 decode profile: every AV1 stream in Brave (11-03) failed with
       'Unable to initialize video session: av1 profile "Main" not supported!'. Exported (not applied) the profile is
       plain Main; streams that carry grain play without the added grain. */
    if (id == AV_CODEC_ID_AV1) s->av->export_side_data |= AV_CODEC_EXPORT_DATA_FILM_GRAIN;
    s->av->width = dim.width; s->av->height = dim.height;
    if (cfg && id != AV_CODEC_ID_VP9) {                /* avcC / av1C: what libavcodec expects as extradata */
        const size_t n = (size_t)CFDataGetLength(cfg);
        s->av->extradata = av_mallocz(n + AV_INPUT_BUFFER_PADDING_SIZE);
        if (s->av->extradata) { memcpy(s->av->extradata, CFDataGetBytePtr(cfg), n); s->av->extradata_size = (int)n; }
    }
    LOG("opening the %s decoder (%d bytes of config)", codec->name, s->av->extradata_size);   /* b251: AV1 in Brave stopped here or later */
    const double to = nowms();
    if (avcodec_open2(s->av, codec, NULL) < 0) { LOG("avcodec_open2 failed"); res = kVTVideoDecoderMalfunctionErr; goto out; }
    LOG("decoder open in %.1f ms", nowms() - to);
    s->pkt = av_packet_alloc(); s->frm = av_frame_alloc();
    if (!s->pkt || !s->frm) { res = kVTVideoDecoderMalfunctionErr; goto out; }
    s->w = dim.width; s->h = dim.height;
    s->pf = s->full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFMutableDictionaryRef col = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const CFStringRef ck[] = { kCMFormatDescriptionExtension_ColorPrimaries, kCMFormatDescriptionExtension_TransferFunction,
                               kCMFormatDescriptionExtension_YCbCrMatrix };
    for (int i = 0; i < 3; i++) { CFTypeRef x = CMFormatDescriptionGetExtension(fmt, ck[i]); if (x) CFDictionarySetValue(col, ck[i], x); }
    s->colour = col;
    res = set_output(s);
    if (res) LOG("pixel buffer attributes -> %d", (int)res);
    res = noErr;
out:
    if (res) avcodec_free_context(&s->av);
    pthread_mutex_unlock(&s->lock);
    return res;
}

static void nofree(void *opaque, uint8_t *data) { (void)opaque; (void)data; }

/* the picture (a Vulkan frame) into a pixel buffer of the session's pool: libavcodec downloads straight into its planes */
static OSStatus to_pixel_buffer(DState *s, const AVFrame *hw, int W, int H, CVPixelBufferRef *out)
{
    *out = NULL;
    if (W != s->w || H != s->h) {   /* a new picture size (VP9 / AV1 can change it): new attributes */
        LOG("picture size %dx%d -> %dx%d", s->w, s->h, W, H);
        s->w = W; s->h = H; set_output(s);
    }
    CVPixelBufferRef pb = NULL;
    CVPixelBufferPoolRef pool = VTDecoderSessionGetPixelBufferPool(s->session);
    if (!pool || CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb) || !pb || (int)CVPixelBufferGetWidth(pb) != W ||
        (int)CVPixelBufferGetHeight(pb) != H || CVPixelBufferGetPixelFormatType(pb) != s->pf) {
        if (pb) { CVPixelBufferRelease(pb); pb = NULL; }
        CFDictionaryRef ios = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        const void *k[] = { kCVPixelBufferIOSurfacePropertiesKey, kCVPixelBufferMetalCompatibilityKey }; const void *v[] = { ios, kCFBooleanTrue };
        CFDictionaryRef a = CFDictionaryCreate(NULL, k, v, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CVPixelBufferCreate(NULL, (size_t)W, (size_t)H, s->pf, a, &pb);
        CFRelease(a); CFRelease(ios);
        if (!pb) return kVTVideoDecoderMalfunctionErr;
    }
    CVPixelBufferLockBaseAddress(pb, 0);
    AVFrame *dst = av_frame_alloc();
    int e = -1;
    if (dst) {
        dst->format = AV_PIX_FMT_NV12; dst->width = W; dst->height = H;
        for (int p = 0; p < 2; p++) {
            dst->data[p] = CVPixelBufferGetBaseAddressOfPlane(pb, (size_t)p);
            dst->linesize[p] = (int)CVPixelBufferGetBytesPerRowOfPlane(pb, (size_t)p);
            /* av_hwframe_transfer_data allocates its own frame when dst has no buffer: give it these planes, unowned */
            dst->buf[p] = av_buffer_create(dst->data[p], (size_t)dst->linesize[p] * CVPixelBufferGetHeightOfPlane(pb, (size_t)p), nofree, NULL, 0);
        }
        if (dst->data[0] && dst->data[1] && dst->buf[0] && dst->buf[1]) e = av_hwframe_transfer_data(dst, hw, 0);
        av_frame_free(&dst);
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    if (e < 0) { LOG("download of the picture failed (%d)", e); CVPixelBufferRelease(pb); return kVTVideoDecoderMalfunctionErr; }
    if (s->colour && CFDictionaryGetCount(s->colour)) CVBufferSetAttachments(pb, s->colour, kCVAttachmentMode_ShouldPropagate);
    *out = pb;
    return noErr;
}

/* every picture libavcodec has ready: to a pixel buffer, emitted with the frame of the sample it came from */
static OSStatus drain(DState *s)
{
    OSStatus res = noErr;
    for (;;) {
        const int e = avcodec_receive_frame(s->av, s->frm);
        if (e == AVERROR(EAGAIN) || e == AVERROR_EOF) break;
        if (e < 0) { LOG("receive_frame -> %d", e); res = kVTVideoDecoderBadDataErr; break; }
        if (s->cur) { av_frame_unref(s->frm); continue; }   /* H.264: pictures go back in decode order (s->cur) */
        const int64_t id = s->frm->pts != AV_NOPTS_VALUE ? s->frm->pts : s->frm->best_effort_timestamp;
        unsigned at = s->npend;
        for (unsigned i = 0; i < s->npend; i++) if (s->pend[i].id == id) { at = i; break; }
        if (at == s->npend) { LOG("picture %lld has no waiting frame - not shown", (long long)id); av_frame_unref(s->frm); continue; }
        CVPixelBufferRef pb = NULL; OSStatus r = noErr;
        if (s->frm->format != AV_PIX_FMT_VULKAN) { r = kVTVideoDecoderMalfunctionErr; LOG("picture is not on the GPU (%s)", av_get_pix_fmt_name(s->frm->format)); }
        else { const double t1 = nowms(); r = to_pixel_buffer(s, s->frm, s->frm->width, s->frm->height, &pb); s->tCopy += nowms() - t1; }
        av_frame_unref(s->frm);
        if (r) s->errors++; else s->frames++;
        emit_pending(s, at, r, 0, pb);
        if (pb) CVPixelBufferRelease(pb);
        /* frames older than the stream's reorder depth that still have no picture never get one (a hidden VP9/AV1
           frame, a skipped one): dropped */
        while (s->npend && s->pend[0].id < id - 16) { s->dropped++; emit_pending(s, 0, noErr, kVTDecodeInfo_FrameDropped, NULL); }
    }
    return res;
}

static OSStatus d_decodeFrame(VTVideoDecoderRef o, VTVideoDecoderFrame frame, CMSampleBufferRef sb, VTDecodeFrameFlags flags, VTDecodeInfoFlags *info)
{
    DState *s = dstate(o);
    if (info) *info = 0;
    pthread_mutex_lock(&s->lock);
    OSStatus res = noErr;
    CMBlockBufferRef bb = sb ? CMSampleBufferGetDataBuffer(sb) : NULL;
    const size_t len = bb ? CMBlockBufferGetDataLength(bb) : 0;
    if (s->nextId < 2) LOG("decodeFrame #%lld: %zu bytes in (flags 0x%x)", (long long)s->nextId, len, (unsigned)flags);   /* b250: before any work */
    if (!s->av || !len) { res = kVTVideoDecoderBadDataErr; LOG("decodeFrame: %s - bad data", !s->av ? "no decoder open" : "empty sample"); }
    else {
        if (s->cap < len + AV_INPUT_BUFFER_PADDING_SIZE) {
            free(s->buf); s->cap = len + AV_INPUT_BUFFER_PADDING_SIZE; s->buf = malloc(s->cap);
            if (!s->buf) s->cap = 0;
        }
        if (!s->buf || CMBlockBufferCopyDataBytes(bb, 0, len, s->buf)) res = kVTVideoDecoderBadDataErr;
    }
    if (!res && s->npend == sizeof s->pend / sizeof s->pend[0]) {   /* should not happen: the oldest is given up */
        s->dropped++; emit_pending(s, 0, noErr, kVTDecodeInfo_FrameDropped, NULL);
    }
    if (res) {
        s->errors++;
        queue_out(s, frame, res, 0, NULL);
        pthread_mutex_unlock(&s->lock);
        flush_out(s);
        return res;
    }
    memset(s->buf + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    const int64_t id = s->nextId++;
    s->pend[s->npend].id = id; s->pend[s->npend].frame = frame; s->npend++;
    av_packet_unref(s->pkt);
    s->pkt->data = s->buf; s->pkt->size = (int)len; s->pkt->pts = s->pkt->dts = id;
    const double t0 = nowms();
    s->gotCur = 0;
    int e = avcodec_send_packet(s->av, s->pkt);
    if (e == AVERROR(EAGAIN)) { drain(s); e = avcodec_send_packet(s->av, s->pkt); }
    if (e < 0) {
        LOG("send_packet %zu bytes -> %d", len, e); s->errors++;
        pthread_mutex_lock(&gDevLock); gDevSuspect = 1; pthread_mutex_unlock(&gDevLock);
        for (unsigned i = 0; i < s->npend; i++) if (s->pend[i].id == id) { emit_pending(s, i, kVTVideoDecoderBadDataErr, 0, NULL); break; }
        pthread_mutex_unlock(&s->lock);
        flush_out(s);
        return kVTVideoDecoderBadDataErr;
    }
    res = drain(s);
    s->tDec += nowms() - t0;
    unsigned at = s->npend;
    for (unsigned i = 0; i < s->npend; i++) if (s->pend[i].id == id) { at = i; break; }
    if (s->cur && at < s->npend) {
        /* H.264: this sample's own picture, decoded by the time send_packet returned (the download waits for the video
           engine's semaphore) - back now, with this sample's frame */
        CVPixelBufferRef pb = NULL; OSStatus r = noErr; VTDecodeInfoFlags fl = 0;
        if (s->gotCur) {
            const double t1 = nowms();
            r = to_pixel_buffer(s, s->cur, s->av->width, s->av->height, &pb);
            s->tCopy += nowms() - t1;
            if (r) s->errors++; else s->frames++;
        } else { fl = kVTDecodeInfo_FrameDropped; s->dropped++; }
        av_frame_unref(s->cur); s->gotCur = 0;
        emit_pending(s, at, r, fl, pb);
        if (pb) CVPixelBufferRelease(pb);
    } else if (at < s->npend) {
        /* VP9 / AV1 do not reorder: a sample whose picture did not come out now has none to show (a hidden frame) */
        s->dropped++; emit_pending(s, at, noErr, kVTDecodeInfo_FrameDropped, NULL);
    }
    /* b247: no kVTDecodeInfo_Asynchronous. b246 set it while frames waited for their pictures, and for a client that
       decodes synchronously (vtdectest, most players) VideoToolbox then blocks in DecodeFrame until that frame is
       emitted - which needs the next sample: the test hung after 6 samples (11-01 sample: main thread in
       VTDecompressionSessionWaitForAsynchronousFrames). A frame held back for reordering is emitted by a later call or by
       finishDelayedFrames, as kVTDecodeFrame_EnableTemporalProcessing allows. */
    if (s->npend && !(flags & kVTDecodeFrame_EnableTemporalProcessing)) {
        static int said; if (!said++) LOG("client did not allow temporal processing - pictures still come in display order, a little later");
    }
    if (s->frames + s->dropped <= 3) LOG("decodeFrame: %zu bytes, %u frame(s) waiting for their picture", len, s->npend);
    pthread_mutex_unlock(&s->lock);
    flush_out(s);
    return noErr;
}

static OSStatus d_copySupported(VTVideoDecoderRef o, CFDictionaryRef *out)
{
    (void)o;
    const void *pk[] = { kVTPropertyTypeKey, kVTPropertyReadWriteStatusKey };
    const void *pv[] = { kVTPropertyType_Boolean, kVTPropertyReadWriteStatus_ReadOnly };
    CFDictionaryRef ro = CFDictionaryCreate(NULL, pk, pv, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *k[] = { kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder };
    const void *v[] = { ro };
    *out = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(ro);
    return noErr;
}
static OSStatus d_setProperties(VTVideoDecoderRef o, CFDictionaryRef d)
{
    (void)o; static int said; if (said++ < 6) LOG("setProperties: %ld key(s)", d ? (long)CFDictionaryGetCount(d) : 0L);
    return noErr;
}
static OSStatus d_copySerializable(VTVideoDecoderRef o, CFAllocatorRef al, CFDictionaryRef *out)
{
    (void)o; (void)al;
    if (out) *out = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    return noErr;
}
static Boolean d_canAccept(VTVideoDecoderRef o, CMFormatDescriptionRef f)
{
    (void)o; const CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(f);
    LOG("canAcceptFormatDescription %dx%d -> no (a new session is made)", d.width, d.height);
    return false;
}
/* the pictures libavcodec still holds back (the last ones before the reorder depth): out, then ready for more */
static OSStatus d_finishDelayed(VTVideoDecoderRef o)
{
    DState *s = dstate(o);
    pthread_mutex_lock(&s->lock);
    if (s->av) {
        if (avcodec_send_packet(s->av, NULL) >= 0) drain(s);
        avcodec_flush_buffers(s->av);
    }
    while (s->npend) { s->dropped++; emit_pending(s, 0, noErr, kVTDecodeInfo_FrameDropped, NULL); }
    pthread_mutex_unlock(&s->lock);
    flush_out(s);
    return noErr;
}

static CMBaseClass gBase = { .version = 1, .derivedStorageSize = sizeof(void *), .invalidate = b_invalidate, .finalize = b_finalize,
                             .copyDebugDescription = b_desc, .copyProperty = b_copyProperty, .setProperty = b_setProperty };
static const DecoderClass gDec = { .version = 1, .startSession = d_startSession, .decodeFrame = d_decodeFrame,
                                   .copySupportedPropertyDictionary = d_copySupported, .setProperties = d_setProperties,
                                   .copySerializableProperties = d_copySerializable, .canAcceptFormatDescription = d_canAccept,
                                   .finishDelayedFrames = d_finishDelayed };
static const DecoderVTable gVT = { .base = { NULL, &gBase }, .decoderClass = &gDec };

__attribute__((visibility("default"))) OSStatus IXDec_CreateInstance(FourCharCode codec, CFAllocatorRef al, VTVideoDecoderRef *out)
{
    if (codec == CODEC_AVC3) codec = CODEC_AVC1;
    if (codec != CODEC_AVC1 && codec != CODEC_VP9 && codec != CODEC_AV1) return kVTCouldNotFindVideoDecoderErr;
    CMBaseObjectRef obj = NULL;
    OSStatus st = CMDerivedObjectCreate(al, &gVT.base, VTVideoDecoderGetClassID(), &obj);
    if (st) return st;
    DState *s = calloc(1, sizeof *s);
    if (!s) { CFRelease(obj); return kVTAllocationFailedErr; }
    pthread_mutex_init(&s->lock, NULL);
    s->codec = codec;
    *(DState **)CMBaseObjectGetDerivedStorage(obj) = s;
    *out = obj;
    return noErr;
}

/* Registers the decoders in this process. gipa: Mesa ANV's vkGetInstanceProcAddr (the Metal plugin's, or the test
   tool's). Returns how many codecs were registered. The Vulkan device is made at the first session. */
__attribute__((visibility("default"))) void IXDec_SetLog(void (*fn)(const char *, ...)) { gHostLog = fn; }

/* b249: which codecs the video engine decodes, asked once when registering (an instance, no device): an older driver,
   or one built without video, has no decode queue and nothing is registered, so apps keep Apple's decoder */
static VkVideoCodecOperationFlagsKHR probe_ops(void)
{
    PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)gGipa(NULL, "vkCreateInstance");
    if (!createInstance) return 0;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "ixdec-probe", 1, "ixdec", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    VkInstance inst;
    if (createInstance(&ici, NULL, &inst)) { LOG("probe: vkCreateInstance failed"); return 0; }
    IP(vkEnumeratePhysicalDevices); IP(vkGetPhysicalDeviceProperties); IP(vkGetPhysicalDeviceQueueFamilyProperties2); IP(vkDestroyInstance);
    VkVideoCodecOperationFlagsKHR ops = 0;
    if (vkEnumeratePhysicalDevices && vkGetPhysicalDeviceProperties && vkGetPhysicalDeviceQueueFamilyProperties2) {
        uint32_t n = 8; VkPhysicalDevice pds[8];
        vkEnumeratePhysicalDevices(inst, &n, pds);
        for (uint32_t i = 0; i < n; i++) {
            VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
            if (p.vendorID != 0x8086) continue;
            uint32_t nq = 16; VkQueueFamilyProperties2 qp[16]; VkQueueFamilyVideoPropertiesKHR qv[16];
            for (uint32_t j = 0; j < 16; j++) {
                qv[j] = (VkQueueFamilyVideoPropertiesKHR){ VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR, NULL, 0 };
                qp[j] = (VkQueueFamilyProperties2){ VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2, &qv[j] };
            }
            vkGetPhysicalDeviceQueueFamilyProperties2(pds[i], &nq, qp);
            for (uint32_t j = 0; j < nq; j++)
                if (qp[j].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) ops |= qv[j].videoCodecOperations;
            break;
        }
    }
    if (vkDestroyInstance) vkDestroyInstance(inst, NULL);
    return ops;
}

__attribute__((visibility("default"))) int IXDec_Register(PFN_vkGetInstanceProcAddr gipa, int rating)
{
    ixlog_init();
    if (!gipa) return 0;
    gGipa = gipa;
    const VkVideoCodecOperationFlagsKHR ops = probe_ops();
    LOG("video engine decodes: ops 0x%x (H.264 %s, VP9 %s, AV1 %s)", (unsigned)ops, (ops & 0x1) ? "yes" : "no",
        (ops & 0x8) ? "yes" : "no", (ops & 0x4) ? "yes" : "no");
    if (!ops) { LOG("no decode queue (driver without video, or ANV_VIDEO_DECODE not set) - nothing registered"); return 0; }
    typedef OSStatus (*RegFn)(FourCharCode, CFDictionaryRef, void *);
    RegFn reg = (RegFn)dlsym(RTLD_DEFAULT, "VTRegisterVideoDecoderWithInfo");
    if (!reg) { LOG("VTRegisterVideoDecoderWithInfo not found"); return 0; }
    CFNumberRef nr = CFNumberCreate(NULL, kCFNumberIntType, &rating);
    const struct { FourCharCode c; CFStringRef id, name; } t[] = {
        { CODEC_AVC1, CFSTR("org.irisxe.videodecoder.avc"), CFSTR("Iris Xe - H.264 (Vulkan Video)") },
        { CODEC_AVC3, CFSTR("org.irisxe.videodecoder.avc"), CFSTR("Iris Xe - H.264 (Vulkan Video)") },
        { CODEC_VP9,  CFSTR("org.irisxe.videodecoder.vp9"), CFSTR("Iris Xe - VP9 (Vulkan Video)") },
        { CODEC_AV1,  CFSTR("org.irisxe.videodecoder.av1"), CFSTR("Iris Xe - AV1 (Vulkan Video)") } };
    int ok = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        const char *no = t[i].c == CODEC_VP9 ? "IXDEC_NO_VP9" : t[i].c == CODEC_AV1 ? "IXDEC_NO_AV1" : "IXDEC_NO_H264";
        const char *nof = t[i].c == CODEC_VP9 ? "/Library/GPUBundles/nvmtl-novideo-vp9" : t[i].c == CODEC_AV1 ? "/Library/GPUBundles/nvmtl-novideo-av1" : "/Library/GPUBundles/nvmtl-novideo-h264";
        const unsigned bit = t[i].c == CODEC_VP9 ? 0x8 : t[i].c == CODEC_AV1 ? 0x4 : 0x1;
        if (getenv(no) || access(nof, F_OK) == 0 || !(ops & bit)) continue;
        /* b258: VP9 hangs Tiger Lake's video engine (11-03, Brave: VK_ERROR_DEVICE_LOST after the second frame, 6.6 s per
           picture) - off unless asked for with /Library/GPUBundles/nvmtl-video-vp9 or IXDEC_VP9=1 */
        if (t[i].c == CODEC_VP9 && !getenv("IXDEC_VP9") && access("/Library/GPUBundles/nvmtl-video-vp9", F_OK) != 0) {
            LOG("VP9 not registered (hangs the video engine on this GPU; opt in: /Library/GPUBundles/nvmtl-video-vp9)"); continue; }
        const void *k[] = { CFSTR("CMClassImplementationID"), CFSTR("CMClassImplementationName"), CFSTR("VTRating"), CFSTR("VTIsHardwareAccelerated") };
        const void *v[] = { t[i].id, t[i].name, nr, kCFBooleanTrue };
        CFDictionaryRef d = CFDictionaryCreate(NULL, k, v, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        const OSStatus st = reg(t[i].c, d, (void *)IXDec_CreateInstance);
        CFRelease(d);
        char cc[5] = { (char)(t[i].c >> 24), (char)(t[i].c >> 16), (char)(t[i].c >> 8), (char)t[i].c, 0 };
        LOG("registered %s -> %d (rating %d)", cc, (int)st, rating);
        if (!st) ok++;
    }
    CFRelease(nr);
    /* b244: VideoToolbox answers VP9 (and AV1) questions only for processes that opted in (10-31: VTIsHardwareDecodeSupported(VP9)
       was 0 with the decoder registered), as mickys/nvenc does */
    typedef void (*SuppFn)(CMVideoCodecType);
    SuppFn supp = (SuppFn)dlsym(RTLD_DEFAULT, "VTRegisterSupplementalVideoDecoderIfAvailable");
    if (supp) {
        if (!getenv("IXDEC_NO_VP9") && (ops & 0x8) && access("/Library/GPUBundles/nvmtl-novideo-vp9", F_OK) != 0 &&
            (getenv("IXDEC_VP9") || access("/Library/GPUBundles/nvmtl-video-vp9", F_OK) == 0)) supp(CODEC_VP9);
        if (!getenv("IXDEC_NO_AV1") && (ops & 0x4) && access("/Library/GPUBundles/nvmtl-novideo-av1", F_OK) != 0) supp(CODEC_AV1);
        LOG("opted in to VP9/AV1 decoding");
    }
    return ok;
}
