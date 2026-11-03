// vtdectest <file> [frames] - hardware video decoding, stage 3 (b243): decodes a video through VideoToolbox, the way
// QuickTime, Safari and Chromium do, twice: once requiring a hardware decoder (on this Mac only ixdec, registered here)
// and once in software (Apple's own decoder); compares every picture and times both. Changes nothing on the Mac.
// Needs libvulkan_intel.dylib (a Mesa build with video codecs) and libixdec.dylib + FFmpeg's libraries in the same
// folder; run it through run-vtdec.sh.
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <sys/resource.h>

/* b246: progress counters (the watchdog below reads them) */
static volatile int gFed, gPics; static const char *volatile gPass = "start"; static volatile int gLastSeen = -1, gIdle;
typedef void *(*GipaFn)(void *, const char *);
typedef int (*RegFn)(GipaFn, int);

typedef struct { NSMutableDictionary<NSNumber *, NSString *> *hash; int pictures, errors; BOOL noHash; } Out;

static void cb(void *ref, void *src, OSStatus st, VTDecodeInfoFlags fl, CVImageBufferRef img, CMTime pts, CMTime dur)
{
    (void)src; (void)dur;
    Out *o = ref;
    if (st || !img) { if (st) o->errors++; return; }
    o->pictures++; gPics++;
    if (o->noHash) return;
    CVPixelBufferRef pb = (CVPixelBufferRef)img;
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    CC_MD5_CTX m; CC_MD5_Init(&m);
    const size_t np = CVPixelBufferGetPlaneCount(pb);
    for (size_t p = 0; p < np; p++) {   // visible rows only (strides differ between decoders)
        const uint8_t *b = CVPixelBufferGetBaseAddressOfPlane(pb, p);
        const size_t bpr = CVPixelBufferGetBytesPerRowOfPlane(pb, p), h = CVPixelBufferGetHeightOfPlane(pb, p);
        const size_t row = CVPixelBufferGetWidthOfPlane(pb, p) * (p ? 2 : 1);
        for (size_t y = 0; y < h; y++) CC_MD5_Update(&m, b + y * bpr, (CC_LONG)row);
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    unsigned char d[16]; CC_MD5_Final(d, &m);
    NSMutableString *h = [NSMutableString string]; for (int i = 0; i < 16; i++) [h appendFormat:@"%02x", d[i]];
    @synchronized (o->hash) { o->hash[@(pts.value * 1000000 / (pts.timescale ? pts.timescale : 1))] = h; }
}

/* b244: Apple's software decoder runs in VTDecoderXPCService, outside this process: its processor time from ps */
static double xpc_cpu_s(void)
{
    FILE *f = popen("ps -axo time=,comm= 2>/dev/null", "r"); if (!f) return 0;
    char line[512]; double t = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, "VTDecoderXPCService")) continue;
        double a = 0, b = 0, c = 0; int n = sscanf(line, "%lf:%lf:%lf", &a, &b, &c);
        t += n == 3 ? a * 3600 + b * 60 + c : n == 2 ? a * 60 + b : a;
    }
    pclose(f); return t;
}
/* b246: progress, and a way out - the b244 run hung with no output at all */
static void watchdog_start(void)
{
    /* kept in a static: under ARC a local dispatch source is released (and its timer gone) when the function returns -
       b246's progress lines never came */
    static dispatch_source_t t;
    t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC, NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler(t, ^{
        const int now = gFed * 100000 + gPics;
        if (now == gLastSeen) gIdle += 5; else gIdle = 0;
        gLastSeen = now;
        printf("       ... %s: %d samples in, %d pictures out%s\n", gPass, gFed, gPics, gIdle ? " (no progress)" : "");
        if (gIdle >= 30) { printf("RESULT: STUCK in %s after %d samples / %d pictures - send this log and /tmp/ixdec-*.log\n", gPass, gFed, gPics); fflush(stdout); _exit(3); }
    });
    dispatch_resume(t);
}
static double cpu_s(void) { struct rusage u; getrusage(RUSAGE_SELF, &u); return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6; }

static BOOL run(NSURL *url, int maxFrames, BOOL hw, Out *o, double *cpu, double *wall, NSString **used)
{
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
    AVAssetTrack *tr = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
    if (!tr) { printf("[FAIL] no video track (AVFoundation cannot read this file)\n"); return NO; }
    NSError *err = nil;
    AVAssetReader *rd = [AVAssetReader assetReaderWithAsset:asset error:&err];
    AVAssetReaderTrackOutput *to = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:tr outputSettings:nil];   // compressed samples
    [rd addOutput:to];
    if (![rd startReading]) { printf("[FAIL] reading: %s\n", rd.error.description.UTF8String); return NO; }
    CMVideoFormatDescriptionRef fmt = (__bridge CMVideoFormatDescriptionRef)tr.formatDescriptions.firstObject;
    NSDictionary *spec = hw ? @{ (id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES }
                            : @{ (id)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder: @NO };
    NSDictionary *dst = @{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
                           (id)kCVPixelBufferIOSurfacePropertiesKey: @{} };
    VTDecompressionOutputCallbackRecord rec = { cb, o };
    VTDecompressionSessionRef ses = NULL;
    OSStatus st = VTDecompressionSessionCreate(NULL, fmt, (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)dst, &rec, &ses);
    if (st) { printf("[FAIL] %s decoder session: %d%s\n", hw ? "hardware" : "software", (int)st,
                     hw && st == kVTCouldNotFindVideoDecoderErr ? " (no hardware decoder took this format)" : ""); return NO; }
    CFBooleanRef h = NULL;
    VTSessionCopyProperty(ses, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL, &h);
    *used = h == kCFBooleanTrue ? @"hardware" : @"software"; if (h) CFRelease(h);
    gPass = hw ? "hardware decode" : "software decode"; gFed = 0; gPics = 0;
    const double c0 = cpu_s(), x0 = xpc_cpu_s(); NSDate *t0 = [NSDate date]; int n = 0;
    CMSampleBufferRef sb;
    while ((maxFrames <= 0 || n < maxFrames) && (sb = [to copyNextSampleBuffer])) {
        VTDecodeInfoFlags fl = 0;
        st = VTDecompressionSessionDecodeFrame(ses, sb, kVTDecodeFrame_EnableTemporalProcessing, NULL, &fl);   /* as AVFoundation */
        if (st) o->errors++;
        CFRelease(sb); n++; gFed++;
    }
    gPass = hw ? "hardware: finishing" : "software: finishing";
    VTDecompressionSessionFinishDelayedFrames(ses);
    VTDecompressionSessionWaitForAsynchronousFrames(ses);
    *wall = -[t0 timeIntervalSinceNow]; *cpu = cpu_s() - c0 + (xpc_cpu_s() - x0);
    VTDecompressionSessionInvalidate(ses); CFRelease(ses);
    [rd cancelReading];
    return YES;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);   /* each line at once, also through tee */
        watchdog_start();
        if (argc < 2) { printf("usage: vtdectest <video file> [frames, 0 = all]\n"); return 1; }
        const int maxFrames = argc > 2 ? atoi(argv[2]) : 0;
        NSURL *url = [NSURL fileURLWithPath:@(argv[1])];
        printf("vtdectest (b248) - VideoToolbox decoding: Iris Xe video engine against Apple's software decoder\n");
        void *anv = dlopen("./libvulkan_intel.dylib", RTLD_NOW | RTLD_LOCAL);
        GipaFn gipa = anv ? (GipaFn)dlsym(anv, "vkGetInstanceProcAddr") : NULL;
        if (anv && !gipa) gipa = (GipaFn)dlsym(anv, "vk_icdGetInstanceProcAddr");
        printf("[%s] Mesa ANV (libvulkan_intel.dylib) %s\n", gipa ? " OK " : "FAIL", anv ? "opened" : dlerror());
        if (!gipa) return 1;
        void *ix = dlopen("./libixdec.dylib", RTLD_NOW | RTLD_LOCAL);
        RegFn reg = ix ? (RegFn)dlsym(ix, "IXDec_Register") : NULL;
        if (!reg) { printf("[FAIL] libixdec.dylib: %s\n", dlerror()); return 1; }
        const int nreg = reg(gipa, 1000);
        printf("[%s] ixdec registered for %d codec(s) (H.264, VP9, AV1)\n", nreg ? " OK " : "FAIL", nreg);
        CMVideoCodecType ct[] = { kCMVideoCodecType_H264, 'vp09', 'av01' }; const char *cn[] = { "H.264", "VP9", "AV1" };
        for (int i = 0; i < 3; i++) printf("       VTIsHardwareDecodeSupported(%s) = %d\n", cn[i], VTIsHardwareDecodeSupported(ct[i]));

        const int cmpN = maxFrames > 0 ? maxFrames : 300;
        Out sw = { [NSMutableDictionary dictionary], 0, 0, NO }, hw = { [NSMutableDictionary dictionary], 0, 0, NO };
        double c, w; NSString *usw = @"?", *uhw = @"?";
        printf("--- 1. compare the first %d pictures\n", cmpN);
        const BOOL okS = run(url, cmpN, NO, &sw, &c, &w, &usw);
        if (okS) printf("[ OK ] software (Apple's decoder): %d pictures, %d errors (%s)\n", sw.pictures, sw.errors, usw.UTF8String);
        const BOOL okH = run(url, cmpN, YES, &hw, &c, &w, &uhw);
        if (okH) printf("[%s] hardware (Iris Xe video engine through ixdec): %d pictures, %d errors (%s)\n", hw.pictures && !hw.errors ? " OK " : "FAIL",
                        hw.pictures, hw.errors, uhw.UTF8String);
        int same = 0, diff = 0, missing = 0;
        if (okS && okH) {
            for (NSNumber *k in sw.hash) { NSString *b = hw.hash[k]; if (!b) missing++; else if ([b isEqualToString:sw.hash[k]]) same++; else diff++; }
            printf("       %d identical, %d different, %d missing from the hardware decode\n", same, diff, missing);
        }
        printf("--- 2. speed over the whole file (processor time includes Apple's decoder service; no picture checking)\n");
        Out ts = { nil, 0, 0, YES }, th = { nil, 0, 0, YES }; double cs = 0, ws = 0, ch = 0, wh = 0;
        if (run(url, 0, NO, &ts, &cs, &ws, &usw)) printf("       software: %d pictures, processor %.2f s, clock %.2f s\n", ts.pictures, cs, ws);
        if (run(url, 0, YES, &th, &ch, &wh, &uhw)) printf("       hardware: %d pictures, processor %.2f s, clock %.2f s\n", th.pictures, ch, wh);
        if (okS && okH && same && !diff && !missing)
            printf("RESULT: VideoToolbox decodes this file on the Iris Xe video engine, every picture identical (processor %.2f s vs %.2f s in software)\n", ch, cs);
        else if (okS && okH) printf("RESULT: the hardware decode differs from the software one - send this log\n");
        else printf("RESULT: %s\n", okH ? "software decode failed" : "the hardware decode did not run - send this log");
    }
    return 0;
}
