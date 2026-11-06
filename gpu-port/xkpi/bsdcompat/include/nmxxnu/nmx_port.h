/* The kext's interface to the port (implemented in src/nmx_port.c); plain types only, shared with the C++ side. */
#ifndef NMX_PORT_H
#define NMX_PORT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int nmx_port_start(void);
void nmx_port_stop(void);
/* device-node opens still outstanding: the kext must not stop while this is nonzero */
int nmx_port_busy(void);
void *nmx_port_attach(void *iopci);
void *nmx_port_attach_pci(void *iopci);
void nmx_port_detach(void *dev);
int nmx_port_power(void *dev, int on);
int nmx_port_shutdown(void *dev);

/* the display bridge (override/src/nmx_fb_bridge.c): one scan-out buffer on every lit output of the device */
struct nmx_fb_info {
    int ok;
    uint32_t width, height, pitch;      /* the buffer: XRGB8888, 'pitch' bytes per row */
    uint64_t phys, size;                /* its physical range (contiguous, through the GPU aperture) */
    uint64_t kva;                       /* its kernel mapping */
    uint32_t pclk_khz;                  /* timing of the largest output's mode */
    uint32_t htotal, vtotal, hsync_start, hsync_end, vsync_start, vsync_end;
    uint32_t buf_w, buf_h;              /* b265: the scan-out buffers' size (larger than width x height for HiDPI
                                           desktops bigger than the panel, shrunk by a plane scaler) */
};
/* before the driver attaches: settings the bridge needs from the driver (scan-out buffers in contiguous VRAM) */
void nmx_fb_before_attach(void);
int nmx_fb_open(void *dev, struct nmx_fb_info *out);
void nmx_fb_close(void *dev);
int nmx_fb_power(void *dev, int on);
/* desktop size w x h shown full-screen: smaller than the panel by the panel fitter, larger (up to buf_w x buf_h, b265)
   by a plane scaler; 0 x 0 = native */
int nmx_fb_set_size(void *dev, uint32_t w, uint32_t h);
/* b89: aperture-backed buffers for the accelerator ("VRAM"); 0 = ok, phys/kva of one contiguous run */
int nmx_fb_vram_alloc(void *dev, uint64_t size, uint64_t *phys, void **kva, void **handle);
void nmx_fb_vram_free(void *handle);
/* b94: hardware cursor on the display engine's cursor plane. init: two dim x dim ARGB8888 images (premultiplied),
   CPU-writable through kva[0]/kva[1]; 0 = ok (tested on the outputs). apply: show image 'img' (0/1) with its top-left
   at x,y in desktop coordinates, or hide it (visible 0); remembered and put back after every mode set. */
int nmx_fb_cursor_init(void *dev, uint32_t *dim, void **kva0, void **kva1);
int nmx_fb_cursor_apply(void *dev, int img, int32_t x, int32_t y, int visible);
/* b125 (R2): double-buffered scan-out. back: the CPU view of the buffer not on the panel (desktop size w x h);
   commit: flip the primary plane to it at the next vblank (blocking); home: back to the IOFramebuffer's buffer. 0 = ok */
int nmx_fb_flip_back(void *dev, void **kva, uint32_t *pitch, uint32_t *w, uint32_t *h);
/* b164: boot screen hand-over - the first mode set waits for WindowServer (nmx_fb_go); the kext sets the knob first */
int nmx_fb_go(void *dev, const char *why);
int nmx_fb_deferred(void *dev);      /* 1 = still showing the firmware screen */
int nmx_fb_go_on_flip(void *dev, const char *why);  /* b174: hand over at the next flip */
int nmx_fb_back_content(void *dev);  /* b169: lit pixels of 64 in the newest held frame, -1 = none */
extern int nmx_fb_defer_wanted;
extern int nmx_fb_ext_wanted;          /* b180: external outputs mirror the panel; -nmxnoext = 0 */
extern int nmx_fb_psr_wanted;          /* b170: PSR once frames flip; -nmxnopsr = 0 */
extern int nmx_fb_big_wanted;          /* b265: scan-out buffers up to 21/16 of the panel (HiDPI step 2) */
int nmx_fb_flip_commit(void *dev);
/* b267: the built-in panel's EDID as i915 read it (bytes copied, 0 = none) */
int nmx_fb_edid(void *dev, uint8_t *buf, uint32_t max);
int nmx_fb_flip_home(void *dev);
#ifdef __cplusplus
}
#endif
#endif
