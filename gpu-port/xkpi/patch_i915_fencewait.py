#!/usr/bin/env python3
"""patch_i915_fencewait.py <i915 display/intel_display.c>: b216 - the commit fence wait cannot hang the boot.
Tahoe hung at the Apple logo twice (10-28 cold start, 10-29 16:29): the hung boot stopped after step "07 commit tail:
waiting for the commit fence" of the very first mode set - inside dma_fence_wait_timeout() on the boot framebuffer's
fence. That wait has a timeout (i915_fence_timeout), yet the Mac sat there for 90 s: on this port the sleep/wake-up
behind it can be missed that early in the boot, so neither the signal nor the timeout ended the wait.
Now the wait polls: dma_fence_is_signaled() (which also asks the fence itself - the hardware seqno for a GPU request,
no interrupt needed) every 1 ms (a page flip waits at most ~1 ms longer than before), at most 1500 ms per fence; a fence still unsignaled after that is logged and the
commit goes on (at worst one frame shows the buffer before the GPU finished it). Applied after patch_i915_committrace.py
(its step markers stay where they are). Idempotent; exits non-zero when the function is not found exactly once."""
import sys, pathlib
p = pathlib.Path(sys.argv[1])
s = p.read_text()
if "NMX_FENCEWAIT" in s:
    print("already patched"); sys.exit(0)
old = """		if (new_plane_state->fence) {
			ret = dma_fence_wait_timeout(new_plane_state->fence, false,
						     i915_fence_timeout(i915));
			if (ret <= 0)
				break;

			dma_fence_put(new_plane_state->fence);
			new_plane_state->fence = NULL;
		}"""
if s.count(old) != 1:
    print("ERROR: intel_atomic_commit_fence_wait() body not found exactly once"); sys.exit(1)
new = """		if (new_plane_state->fence) {
			/* NMX_FENCEWAIT b216: poll, bounded - see patch_i915_fencewait.py */
			int nmx_ms = 0;
			(void)i915;
			while (!dma_fence_is_signaled(new_plane_state->fence) && nmx_ms < 1500) {
				msleep(1);
				nmx_ms += 1;
			}
			ret = dma_fence_is_signaled(new_plane_state->fence) ? 1 : 0;
			if (!ret) {
				printk("NullMoth commit: plane fence NOT signalled after %d ms - committing anyway (b216)\\n", nmx_ms);
				break;
			} else if (nmx_ms >= 100)
				printk("NullMoth commit: plane fence took %d ms (b216)\\n", nmx_ms);

			dma_fence_put(new_plane_state->fence);
			new_plane_state->fence = NULL;
		}"""
s = s.replace(old, new, 1)
fn = "static void intel_atomic_commit_fence_wait(struct intel_atomic_state *intel_state)"
if s.count(fn) != 1:
    print("ERROR: intel_atomic_commit_fence_wait() definition not found exactly once"); sys.exit(1)
s = s.replace(fn, "#include <linux/delay.h>   /* NMX_FENCEWAIT: msleep */\n" + fn, 1)
s = "/* NMX_FENCEWAIT */\n" + s
p.write_text(s)
print("patched", p)
