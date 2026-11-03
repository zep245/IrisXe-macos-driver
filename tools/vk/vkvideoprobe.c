/*
 * vkvideoprobe - hardware video decoding, stage 1 (b240): can Mesa ANV decode video on this Iris Xe through Vulkan Video?
 *
 * Changes nothing on the Mac. Prints, step by step (OK / FAIL / -- for "not offered"):
 *   1. the Intel GPU and every device extension with "video" in its name
 *   2. the queue families, and which video operations a decode queue offers
 *   3. for H.264, H.265 (HEVC), AV1 and VP9 decode (8-bit 4:2:0): the largest picture, reference slots and output formats
 *   4. a real check of the video engine: a device with the decode queue, an H.264 1920x1080 video session with its
 *      memory bound, and one (empty) video command buffer submitted to the video engine and waited for
 * Step 4 is the first time anything runs on the GPU's video engine (VCS) through NMXGPU; if the Mac misbehaves there,
 * the kernel side is where the work is. Run it through run-vkvideo.sh (not with sudo).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

/* b241: the Vulkan loader is opened at run time (libvulkan.1.dylib from the folder run-vkvideo.sh points at), so this
   tool also builds where no Vulkan library is installed (the NMXGPU build) */
#define VKFN(X) X(vkGetInstanceProcAddr) X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkCreateDevice) X(vkGetDeviceQueue) X(vkGetDeviceProcAddr) X(vkAllocateMemory) X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkQueueSubmit) \
    X(vkWaitForFences) X(vkDestroyFence) X(vkDestroyCommandPool) X(vkDestroyDevice) X(vkDestroyInstance)
#define DECL(n) static PFN_##n n;
VKFN(DECL)
static int load_vulkan(void)
{
    void *h = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("[FAIL] open libvulkan.1.dylib: %s (run through run-vkvideo.sh inside the nmx-vulkan folder)\n", dlerror()); return -1; }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr) { printf("[FAIL] libvulkan has no vkGetInstanceProcAddr\n"); return -1; }
    vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    return vkCreateInstance ? 0 : -1;
}
static void load_instance_fns(VkInstance in)
{
#define LD(n) if (!n) n = (PFN_##n)vkGetInstanceProcAddr(in, #n);
    VKFN(LD)
}

#define LINE(ok, ...) do { printf("[%s] ", ok); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static VkInstance inst;
#define IPROC(name) PFN_##name p##name = (PFN_##name)vkGetInstanceProcAddr(inst, #name)

typedef struct {
    const char *name, *ext; VkVideoCodecOperationFlagBitsKHR op;
    union { VkVideoDecodeH264ProfileInfoKHR h264; VkVideoDecodeH265ProfileInfoKHR h265;
#ifdef VK_KHR_video_decode_av1
            VkVideoDecodeAV1ProfileInfoKHR av1;
#endif
#ifdef VK_KHR_video_decode_vp9
            VkVideoDecodeVP9ProfileInfoKHR vp9;
#endif
          } p;
} codec;

static void profile_for(codec *c, VkVideoProfileInfoKHR *pr)
{
    memset(&c->p, 0, sizeof c->p);
    switch ((int)c->op) {
    case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
        c->p.h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
        c->p.h264.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
        c->p.h264.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR; break;
    case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR:
        c->p.h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
        c->p.h265.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN; break;
#ifdef VK_KHR_video_decode_av1
    case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR:
        c->p.av1.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR;
        c->p.av1.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN; c->p.av1.filmGrainSupport = VK_FALSE; break;
#endif
#ifdef VK_KHR_video_decode_vp9
    case VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR:
        c->p.vp9.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PROFILE_INFO_KHR;
        c->p.vp9.stdProfile = STD_VIDEO_VP9_PROFILE_0; break;
#endif
    }
    *pr = (VkVideoProfileInfoKHR){ VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR, &c->p, c->op,
        VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR, VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR, VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR };
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    const int infoOnly = argc > 1 && !strcmp(argv[1], "info");   /* steps 1-3 only: nothing runs on the GPU */
    printf("vkvideoprobe (b241) - hardware video decoding check%s\n", infoOnly ? " (info only)" : "");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkvideoprobe", 1, NULL, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    if (load_vulkan()) return 1;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    LINE(r ? "FAIL" : " OK ", "create instance (%d)", r);
    if (r) return 1;
    load_instance_fns(inst);

    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice pds[8], pd = VK_NULL_HANDLE; if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(inst, &n, pds);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        printf("       device %u: %s (0x%04x:0x%04x)\n", i, p.deviceName, p.vendorID, p.deviceID);
        if (p.vendorID == 0x8086 && !pd) pd = pds[i];
    }
    LINE(pd ? " OK " : "FAIL", "find the Intel GPU");
    if (!pd) return 1;

    /* 1. extensions */
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ext = calloc(ne ? ne : 1, sizeof *ext);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);
    int anyvideo = 0;
    for (uint32_t i = 0; i < ne; i++) if (strstr(ext[i].extensionName, "video")) { printf("       extension: %s (v%u)\n", ext[i].extensionName, ext[i].specVersion); anyvideo = 1; }
    LINE(has_ext(ext, ne, "VK_KHR_video_decode_queue") ? " OK " : "FAIL", "VK_KHR_video_decode_queue offered%s",
         anyvideo ? "" : " - no video extension at all (Mesa built without video codecs, or ANV keeps video off: see run-vkvideo.sh)");

    /* 2. queue families */
    IPROC(vkGetPhysicalDeviceQueueFamilyProperties2);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL); if (nq > 16) nq = 16;
    VkQueueFamilyProperties2 qp[16]; VkQueueFamilyVideoPropertiesKHR qv[16];
    for (uint32_t i = 0; i < nq; i++) {
        qv[i] = (VkQueueFamilyVideoPropertiesKHR){ VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR, NULL, 0 };
        qp[i] = (VkQueueFamilyProperties2){ VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2, &qv[i] };
    }
    if (pvkGetPhysicalDeviceQueueFamilyProperties2) pvkGetPhysicalDeviceQueueFamilyProperties2(pd, &nq, qp);
    uint32_t dq = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkQueueFlags f = qp[i].queueFamilyProperties.queueFlags;
        printf("       queue family %u: %u queue(s)%s%s%s%s%s  video ops 0x%x\n", i, qp[i].queueFamilyProperties.queueCount,
               f & VK_QUEUE_GRAPHICS_BIT ? " graphics" : "", f & VK_QUEUE_COMPUTE_BIT ? " compute" : "",
               f & VK_QUEUE_TRANSFER_BIT ? " transfer" : "", f & VK_QUEUE_VIDEO_DECODE_BIT_KHR ? " VIDEO-DECODE" : "",
               f & VK_QUEUE_VIDEO_ENCODE_BIT_KHR ? " video-encode" : "", qv[i].videoCodecOperations);
        if ((f & VK_QUEUE_VIDEO_DECODE_BIT_KHR) && dq == UINT32_MAX) dq = i;
    }
    LINE(dq != UINT32_MAX ? " OK " : "FAIL", "a video decode queue (the GPU's video engine reached through NMXGPU)");

    /* 3. per codec */
    codec cs[] = {
        { "H.264 High", "VK_KHR_video_decode_h264", VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR },
        { "H.265 Main (HEVC)", "VK_KHR_video_decode_h265", VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR },
#ifdef VK_KHR_video_decode_av1
        { "AV1 Main", "VK_KHR_video_decode_av1", VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR },
#endif
#ifdef VK_KHR_video_decode_vp9
        { "VP9 Profile 0", "VK_KHR_video_decode_vp9", (VkVideoCodecOperationFlagBitsKHR)VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR },
#endif
    };
    IPROC(vkGetPhysicalDeviceVideoCapabilitiesKHR);
    IPROC(vkGetPhysicalDeviceVideoFormatPropertiesKHR);
    VkExtensionProperties h264hdr = {0};
    for (size_t k = 0; k < sizeof cs / sizeof cs[0]; k++) {
        codec *c = &cs[k];
        if (!has_ext(ext, ne, c->ext)) { LINE(" -- ", "%s: %s not offered", c->name, c->ext); continue; }
        if (dq == UINT32_MAX || !(qv[dq].videoCodecOperations & c->op)) { LINE("FAIL", "%s: extension offered but the decode queue lacks the operation", c->name); continue; }
        VkVideoProfileInfoKHR pr; profile_for(c, &pr);
        /* b241: the codec's own capability struct must be in the chain - ANV writes into it without checking (b240's
           probe stopped silently right here, 11-01) */
        union { VkBaseOutStructure b; VkVideoDecodeH264CapabilitiesKHR h264; VkVideoDecodeH265CapabilitiesKHR h265;
#ifdef VK_KHR_video_decode_av1
                VkVideoDecodeAV1CapabilitiesKHR av1;
#endif
#ifdef VK_KHR_video_decode_vp9
                VkVideoDecodeVP9CapabilitiesKHR vp9;
#endif
              } cc;
        memset(&cc, 0, sizeof cc);
        switch ((int)c->op) {
        case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR: cc.b.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR; break;
        case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR: cc.b.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR; break;
#ifdef VK_KHR_video_decode_av1
        case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR: cc.b.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR; break;
#endif
#ifdef VK_KHR_video_decode_vp9
        case VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR: cc.b.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_CAPABILITIES_KHR; break;
#endif
        }
        VkVideoDecodeCapabilitiesKHR dc = { VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR, &cc, 0 };
        VkVideoCapabilitiesKHR vc = { VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, &dc };
        r = pvkGetPhysicalDeviceVideoCapabilitiesKHR ? pvkGetPhysicalDeviceVideoCapabilitiesKHR(pd, &pr, &vc) : VK_ERROR_EXTENSION_NOT_PRESENT;
        if (r) { LINE("FAIL", "%s: capabilities (%d)", c->name, r); continue; }
        if (c->op == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) h264hdr = vc.stdHeaderVersion;
        VkVideoProfileListInfoKHR pl = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR, NULL, 1, &pr };
        VkPhysicalDeviceVideoFormatInfoKHR fi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR, &pl,
                                                  VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR };
        uint32_t nf = 0; char fmts[160] = "";
        if (pvkGetPhysicalDeviceVideoFormatPropertiesKHR && pvkGetPhysicalDeviceVideoFormatPropertiesKHR(pd, &fi, &nf, NULL) == VK_SUCCESS && nf) {
            VkVideoFormatPropertiesKHR fp[8]; if (nf > 8) nf = 8;
            for (uint32_t i = 0; i < nf; i++) fp[i] = (VkVideoFormatPropertiesKHR){ VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR };
            pvkGetPhysicalDeviceVideoFormatPropertiesKHR(pd, &fi, &nf, fp);
            for (uint32_t i = 0; i < nf; i++) snprintf(fmts + strlen(fmts), sizeof fmts - strlen(fmts), "%s%d%s", i ? "," : "",
                                                       fp[i].format, fp[i].format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM ? "(NV12)" : "");
        }
        LINE(" OK ", "%s: up to %ux%u, %u reference slots, %u active refs, output formats %s, decode flags 0x%x",
             c->name, vc.maxCodedExtent.width, vc.maxCodedExtent.height, vc.maxDpbSlots, vc.maxActiveReferencePictures,
             fmts[0] ? fmts : "none", dc.flags);
    }

    /* 4. the video engine itself: device with the decode queue, an H.264 session, one empty video command buffer */
    if (infoOnly) { printf("RESULT: info only - run without \"info\" to test the video engine\n"); return 0; }
    if (dq == UINT32_MAX || !has_ext(ext, ne, "VK_KHR_video_decode_h264")) {
        LINE(" -- ", "video engine test skipped (no H.264 decode)");
        printf("RESULT: no hardware video decoding offered by this Mesa build\n");
        return 0;
    }
    const char *dexts[] = { "VK_KHR_video_queue", "VK_KHR_video_decode_queue", "VK_KHR_video_decode_h264" };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, dq, 1, &prio };
    VkPhysicalDeviceSynchronization2Features s2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, NULL, VK_TRUE };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &s2, 0, 1, &qci, 0, NULL, 3, dexts, NULL };
    VkDevice dev;
    r = vkCreateDevice(pd, &dci, NULL, &dev);
    LINE(r ? "FAIL" : " OK ", "create a device with the video decode queue (%d)", r);
    if (r) return 1;
    VkQueue q; vkGetDeviceQueue(dev, dq, 0, &q);
#define DPROC(name) PFN_##name p##name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    DPROC(vkCreateVideoSessionKHR); DPROC(vkGetVideoSessionMemoryRequirementsKHR); DPROC(vkBindVideoSessionMemoryKHR);
    DPROC(vkDestroyVideoSessionKHR); DPROC(vkCmdBeginVideoCodingKHR); DPROC(vkCmdEndVideoCodingKHR); DPROC(vkCmdControlVideoCodingKHR);
    if (!pvkCreateVideoSessionKHR || !pvkCmdBeginVideoCodingKHR) { LINE("FAIL", "video functions missing"); return 1; }

    codec c264 = cs[0]; VkVideoProfileInfoKHR pr; profile_for(&c264, &pr);
    VkVideoSessionCreateInfoKHR sci = { VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR, NULL, dq, 0, &pr,
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, { 1920, 1088 }, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, 4, 4, &h264hdr };
    VkVideoSessionKHR vs;
    r = pvkCreateVideoSessionKHR(dev, &sci, NULL, &vs);
    LINE(r ? "FAIL" : " OK ", "create an H.264 1920x1080 video session (%d)", r);
    if (r) return 1;

    uint32_t nm = 0; pvkGetVideoSessionMemoryRequirementsKHR(dev, vs, &nm, NULL); if (nm > 16) nm = 16;
    VkVideoSessionMemoryRequirementsKHR mr[16];
    for (uint32_t i = 0; i < nm; i++) mr[i] = (VkVideoSessionMemoryRequirementsKHR){ VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR };
    pvkGetVideoSessionMemoryRequirementsKHR(dev, vs, &nm, mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkBindVideoSessionMemoryInfoKHR bi[16]; int bad = 0; unsigned long long total = 0;
    for (uint32_t i = 0; i < nm; i++) {
        uint32_t mt = UINT32_MAX;
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++) if (mr[i].memoryRequirements.memoryTypeBits & (1u << t)) { mt = t; break; }
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr[i].memoryRequirements.size, mt };
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (mt == UINT32_MAX || vkAllocateMemory(dev, &ai, NULL, &m)) { bad = 1; break; }
        total += mr[i].memoryRequirements.size;
        bi[i] = (VkBindVideoSessionMemoryInfoKHR){ VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR, NULL, mr[i].memoryBindIndex, m, 0, mr[i].memoryRequirements.size };
    }
    r = bad ? VK_ERROR_OUT_OF_DEVICE_MEMORY : pvkBindVideoSessionMemoryKHR(dev, vs, nm, bi);
    LINE(r ? "FAIL" : " OK ", "bind the session's memory (%u block(s), %llu KB) (%d)", nm, total / 1024, r);
    if (r) return 1;

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, dq };
    VkCommandPool pool; vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb; vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &cbi);
    VkVideoBeginCodingInfoKHR bci = { VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR, NULL, 0, vs, VK_NULL_HANDLE, 0, NULL };
    pvkCmdBeginVideoCodingKHR(cb, &bci);
    VkVideoCodingControlInfoKHR cci = { VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR, NULL, VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR };
    pvkCmdControlVideoCodingKHR(cb, &cci);
    VkVideoEndCodingInfoKHR eci = { VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
    pvkCmdEndVideoCodingKHR(cb, &eci);
    r = vkEndCommandBuffer(cb);
    LINE(r ? "FAIL" : " OK ", "record a video command buffer (reset the session) (%d)", r);
    if (r) return 1;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fe; vkCreateFence(dev, &fci, NULL, &fe);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
    printf("       submitting to the video engine now (if the Mac stops here, tell me)\n"); fflush(stdout);
    r = vkQueueSubmit(q, 1, &si, fe);
    LINE(r ? "FAIL" : " OK ", "submit to the video engine (%d)", r);
    if (r) return 1;
    r = vkWaitForFences(dev, 1, &fe, VK_TRUE, 5ull * 1000 * 1000 * 1000);
    LINE(r ? "FAIL" : " OK ", "the video engine finished the command buffer (%s, %d)", r == VK_TIMEOUT ? "TIMED OUT after 5 s" : r ? "error" : "in time", r);
    vkDestroyFence(dev, fe, NULL); vkDestroyCommandPool(dev, pool, NULL);
    pvkDestroyVideoSessionKHR(dev, vs, NULL); vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
    printf("RESULT: %s\n", r ? "the video engine did not answer - kernel work needed before decoding" :
                               "hardware video decoding is reachable - stage 2 (decode a real frame) can start");
    return r ? 1 : 0;
}
