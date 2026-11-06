/* cgmodes - lists every desktop size macOS offers for each display (b263/b266, HiDPI work).
     ./cgmodes            public list (CoreGraphics) and WindowServer's own full list (CGS, includes hidden sizes)
     ./cgmodes set <n>    switch the main display to WindowServer's mode number n (as "Retina Display Menu" does)
   A HiDPI ("Retina") size shows pixels = 2x points (density 2.0). No root needed. */
#include <ApplicationServices/ApplicationServices.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* WindowServer's private mode record (layout as used by Retina Display Menu / displayplacer) */
typedef struct {
    uint32_t modeNumber, flags, width, height, depth;
    uint8_t unknown[170];
    uint16_t freq;
    uint8_t more[16];
    float density;
} CGSMode;
typedef CGError (*NumFn)(CGDirectDisplayID, int *);
typedef CGError (*DescFn)(CGDirectDisplayID, int, CGSMode *, int);
typedef CGError (*CfgFn)(CGDisplayConfigRef, CGDirectDisplayID, int);

int main(int argc, char **argv)
{
    void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOW);
    NumFn num = cg ? (NumFn)dlsym(cg, "CGSGetNumberOfDisplayModes") : NULL;
    DescFn desc = cg ? (DescFn)dlsym(cg, "CGSGetDisplayModeDescriptionOfLength") : NULL;
    CfgFn cfg = cg ? (CfgFn)dlsym(cg, "CGSConfigureDisplayMode") : NULL;
    if (argc == 3 && !strcmp(argv[1], "set")) {
        if (!cfg) { printf("CGSConfigureDisplayMode not found\n"); return 1; }
        CGDisplayConfigRef c; CGBeginDisplayConfiguration(&c);
        CGError e = cfg(c, CGMainDisplayID(), atoi(argv[2]));
        CGError e2 = CGCompleteDisplayConfiguration(c, kCGConfigurePermanently);
        printf("mode %d -> %d / %d\n", atoi(argv[2]), e, e2);
        return e || e2;
    }
    CGDirectDisplayID ids[8]; uint32_t n = 0;
    if (CGGetOnlineDisplayList(8, ids, &n) != kCGErrorSuccess) { printf("no displays\n"); return 1; }
    const void *k[] = { kCGDisplayShowDuplicateLowResolutionModes }; const void *v[] = { kCFBooleanTrue };
    CFDictionaryRef opt = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (uint32_t d = 0; d < n; d++) {
        CGDisplayModeRef cur = CGDisplayCopyDisplayMode(ids[d]);
        printf("display 0x%x (vendor 0x%x product 0x%x)%s: now %zux%zu points, %zux%zu pixels\n", ids[d], CGDisplayVendorNumber(ids[d]),
               CGDisplayModelNumber(ids[d]), CGDisplayIsBuiltin(ids[d]) ? " built-in" : "",
               cur ? CGDisplayModeGetWidth(cur) : 0, cur ? CGDisplayModeGetHeight(cur) : 0,
               cur ? CGDisplayModeGetPixelWidth(cur) : 0, cur ? CGDisplayModeGetPixelHeight(cur) : 0);
        printf(" public list (CoreGraphics):\n");
        CFArrayRef modes = CGDisplayCopyAllDisplayModes(ids[d], opt);
        for (CFIndex i = 0; modes && i < CFArrayGetCount(modes); i++) {
            CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
            const size_t w = CGDisplayModeGetWidth(m), h = CGDisplayModeGetHeight(m), pw = CGDisplayModeGetPixelWidth(m), ph = CGDisplayModeGetPixelHeight(m);
            printf("  %4zux%-4zu points  %4zux%-4zu pixels  %5.1f Hz  IO flags 0x%08x  id %d%s%s%s\n", w, h, pw, ph,
                   CGDisplayModeGetRefreshRate(m), CGDisplayModeGetIOFlags(m), CGDisplayModeGetIODisplayModeID(m),
                   pw == 2 * w ? "  HiDPI" : "", CGDisplayModeIsUsableForDesktopGUI(m) ? "" : "  (not for desktop)",
                   cur && CGDisplayModeGetIODisplayModeID(m) == CGDisplayModeGetIODisplayModeID(cur) && w == CGDisplayModeGetWidth(cur) ? "  <- now" : "");
        }
        if (modes) CFRelease(modes);
        if (cur) CGDisplayModeRelease(cur);
        int cnt = 0;
        if (num && desc && num(ids[d], &cnt) == 0) {
            printf(" WindowServer's full list (CGS, %d modes; 'cgmodes set <n>' switches to one):\n", cnt);
            for (int i = 0; i < cnt; i++) {
                CGSMode m; memset(&m, 0, sizeof m);
                if (desc(ids[d], i, &m, (int)sizeof m)) continue;
                printf("  n %3u  %4ux%-4u points  density %.1f  flags 0x%08x  depth %u%s\n", m.modeNumber, m.width, m.height, m.density,
                       m.flags, m.depth, m.density > 1.5f ? "  HiDPI" : "");
            }
        } else printf(" (WindowServer's own list not readable here)\n");
    }
    CFRelease(opt);
    return 0;
}
