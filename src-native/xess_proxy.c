/* xess_proxy.c — XeSS SEAT for NMS (takeover build).
 *
 * Model (learning-run proven 2026-10-10): the game probes XeSS at boot via
 * xessVKCreateContext with its real Vulkan instance/pd/device. The real
 * Intel DLL whitelists vendors {Intel, NVIDIA, AMD} — Adreno 0x5143 gets
 * XESS_RESULT_ERROR_UNSUPPORTED_DEVICE (-1000), which the game surfaces as
 * an adapter error + hides the XeSS row. Under the old NVIDIA identity
 * spoof the whitelist passed and the row appeared — the gate was never the
 * game's.
 *
 * This build replaces the whitelist with US: CreateContext always succeeds
 * with our own context handle; the whole context-taking family is served
 * locally (forwarding a fake handle into Intel's DLL would crash); Init
 * captures the output res/quality/flags; Execute receives the complete
 * FSR4 workload contract (color/velocity/depth/exposure/mask + formats,
 * extents, jitter, input W/H) and — for THIS build — logs it and returns
 * success without writing the output (seam proof; the output texture stays
 * untouched so the visual result will be wrong until the carrier lands).
 *
 * Forward-if-not-fake is preserved on every wrapper, so a real context
 * (if Intel's DLL ever creates one) still routes to it.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -shared -o libxess.dll xess_proxy.c \
 *   xess_proxy.def -static-libgcc -Wl,--no-insert-timestamp
 * Deploy: Binaries/libxess.dll (this) + Binaries/libxess_real.dll (game's
 *   original, byte-identical copy).
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <io.h>              /* _dup: the ZCC2 import consumes the fd; dup keeps the worker's */
#include "vulkan_core.h"   /* repo vklocal copy — real types, no hand-rolled ABI */

#include "vulkan_core.h"   /* repo vklocal copy — real types, no hand-rolled ABI */

typedef uint64_t xess_context_handle_t;
typedef int32_t xess_result_t;
typedef struct { uint32_t x, y; } xess_2d_t;
typedef struct { uint32_t requiredDescriptorCount; uint64_t tempBufferHeapSize; uint64_t tempTextureHeapSize; } xess_properties_t;
typedef struct { uint16_t major, minor, patch; } xess_version_t;
typedef enum { XESS_MODEL_KPSS = 0, XESS_MODEL_SPLAT = 1 } xess_network_model_t;

typedef struct { void* imageView; void* image; uint32_t aspectMask, baseMipLevel, levelCount,
                 baseArrayLayer, layerCount; int32_t format; uint32_t width, height; } xess_vk_image_view_info;
typedef struct {
    xess_vk_image_view_info colorTexture, velocityTexture, depthTexture,
                            exposureScaleTexture, responsivePixelMaskTexture, outputTexture;
    float jitterOffsetX, jitterOffsetY, exposureScale;
    uint32_t resetHistory, inputWidth, inputHeight;
    xess_2d_t inputColorBase, inputMotionVectorBase, inputDepthBase,
              inputResponsiveMaskBase, reserved0, outputColorBase;
} xess_vk_execute_params_t;
typedef struct {
    xess_2d_t outputResolution; int32_t qualitySetting; uint32_t initFlags;
    uint32_t creationNodeMask, visibleNodeMask;
    VkDeviceMemory tempBufferHeap; uint64_t bufferHeapOffset;
    VkDeviceMemory tempTextureHeap; uint64_t textureHeapOffset;
    VkPipelineCache pipelineCache;
} xess_vk_init_params_t;

#define XESS_OK 0
/* learning-run value observed from the real DLL on Adreno: */
#define XESS_ERR_UNSUPPORTED_DEVICE (-1000)

/* our context cookie — never a plausible Intel handle */
#define XESS_SEAT_CTX 0x5E55C0DE5E55ULL

static HMODULE g_real;
static FILE* g_log;
static CRITICAL_SECTION g_cs;

/* seat state */
static VkInstance   g_vk_inst;
static VkPhysicalDevice g_vk_pd;
static VkDevice     g_vk_dev;
static int g_ctx_live;
static xess_vk_init_params_t g_init;
static float g_jitter_x = 1.0f, g_jitter_y = 1.0f;
static float g_vel_x = 1.0f, g_vel_y = 1.0f;
static float g_exposure = 1.0f;
static float g_mask_max = 0.1f;
static unsigned long long g_execs;

static void L(const char* fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

static void* real_fn(const char* name)
{
    return g_real ? (void*)GetProcAddress(g_real, name) : NULL;
}

static void img_log(const char* tag, const xess_vk_image_view_info* im)
{
    if (!im || (!im->imageView && !im->image)) { L("    %-9s (absent)", tag); return; }
    L("    %-9s view=%p img=%p fmt=%d %ux%u", tag, im->imageView, im->image,
      im->format, im->width, im->height);
}

/* quality -> legacy scale factor (pre-1.3.0 factors per SDK docs) */
static float qscale(int32_t q)
{
    switch (q) {
    case 100: return 3.0f;   /* ultra performance */
    case 101: return 2.0f;   /* performance */
    case 102: return 1.7f;   /* balanced */
    case 103: return 1.5f;   /* quality */
    case 104: return 1.3f;   /* ultra quality */
    case 105: return 1.2f;   /* ultra quality plus */
    default:  return 1.0f;   /* AA */
    }
}

/* ============ SEAT rev3: the NMS-1 doorbell (XeSS slot, FSR4NPU_NPU=1) ============
 * Replaces the blit carrier when armed: the layer (fsr4npu_seam rev3a) takes
 * the doorbell at RECORD time, records the staging copies + GPU stamp (and
 * the previous NPU generation's output copy — or its own blit fallback while
 * the NPU is cold) into the game's own command buffer, and acks through the
 * mailbox. Design of record: docs/NPU-AT-XESS-SLOT-DESIGN.md. PAIR LAW: this
 * seat ships only against the rev3a layer (mailbox header rev NMS-1 = 6).
 * Without FSR4NPU_NPU=1 the seat stays byte-equivalent to rev2a (blit). */
#define NMS_MB_BYTES     0xBD0ull
#define NMS_MB_HEADER    0x2D0ull
#define NMS_MB_SLOT      0x240ull
#define NMS_MB_SLOTS     4
#define NMS_MB_MAGIC     0x5345414D31344653ull
#define NMS_DB_MAGIC     0x444F4F52424F4E47ull
#define NMS_VERSION      7ull
#define NMS_ACK_WORK     0x8000000000000000ull
#define NMS_MARKER_BYTES 0x2D0ull

/* per-stage dispatch plan (R59 SeamPlan mirror, 96 B) */
typedef struct {
    volatile unsigned long long comp_pipe, comp_layout, comp_set;
    volatile unsigned long long comp_dispatch;      /* dgx | dgy<<32 */
    volatile unsigned long long comp_dgz_push;      /* dgz | push_bytes<<32 */
    volatile unsigned int      comp_push[11];
    volatile unsigned int      comp_patch;
    char _pad[4];
} NmsPlan;

static int nms_chassis_feat_posta(uint32_t type_idx);
static int nms_chassis_rcas(uint32_t type_idx);
static int nms_chassis_rec_bridge(uint32_t type_idx);

typedef struct {
    volatile unsigned long long magic, version, ack, ack_count, seat_hint, slot_bytes, slots;
    volatile unsigned long long eval_seq, npu_done, stamp_ptr;
    volatile unsigned long long stage_color_ptr, stage_vel_ptr, stage_depth_ptr, npudst_ptr;
    volatile unsigned long long stage_color_buf, stage_vel_buf, stage_depth_buf, stamp_buf, npudst_buf;
    volatile unsigned long long stage_bytes, npudst_bytes, frame_w, frame_h, out_w, out_h, nms_flags;
    /* rev3b chassis tail: plan[4]@0xD0 (4x96B) ends 0x250 */
    NmsPlan plan[4];                   /* 0=feat 1=postA 2=RCAS 3=exp8to16 (rec bridge) */
    volatile unsigned long long outq_ptr, outq_bytes;       /* the NPU in tensor map */
    volatile unsigned long long route_buf, route_img;       /* fp16 bank + probe image */
    /* rev3c.7 NMS-3: 3-slot npu-in ring + explicit (gen<<8)|slot publish
     * (the campaign's r59g law). Worker3 writes ring slots and publishes;
     * the layer copies the published slot -> npudst_buf before postA.
     * ORDER IS PAIR-LOCKED with the layer's NmHeader mirror: a_buf, b_buf,
     * a_ptr, b_ptr, c_buf, c_ptr, pub — the first ring-era seat shipped
     * c_buf in a_ptr's slot and the worker smashed memory through a
     * VkBuffer-handle-as-pointer (the random-module SIGILL/SEGV crash era,
     * 18:12-18:22). */
    volatile unsigned long long stage2a_buf, stage2b_buf;                /* ring slots 0,1 */
    volatile unsigned long long stage2a_ptr, stage2b_ptr;                /* worker maps 0,1 */
    volatile unsigned long long stage2c_buf, stage2c_ptr;                /* ring slot 2 */
    volatile unsigned long long npu_pub;                                 /* (gen<<8)|slot */
    /* 0x2A8/0x2B0: the parked prev-disocclusion slots, REPURPOSED for ZCC3
     * (the IN-side zero-copy; rename only — offsets unchanged, pair-locked
     * with the layer's mirror): layer publishes a spare dup of the worker's
     * IN rpcmem fd; this seat imports it onto the game device (same proven
     * path as ZCC2) and publishes the buffer back; the layer's game CB then
     * copies outq -> imported-IN and worker3's CPU input bridge retires. */
    volatile unsigned long long zcc3_in_fd, zcc3_imported_in_buf;
    /* rev3c.10 ZCC2-NMS handoff (append-only, pair-locked with the layer's
     * mirror, SAME ORDER): the layer publishes the worker's OUT rpcmem fd
     * here; THIS seat performs the game-device import (its nms_gipa device
     * path is the proven one — every layer-side resolution path failed,
     * runbook §23.17) and publishes the imported VkBuffer back.
     * zcc2_state: 0=idle, 1=fd seen (import running), 2=imported,
     * 3=failed-permanent. */
    volatile unsigned long long zcc2_out_fd, zcc2_imported_buf, zcc2_state;

} NmsHeader;

typedef struct {
    unsigned long long view, image;
    unsigned int aspect, base_mip, mip_count, base_layer, layer_count;
    unsigned int format, w, h, type, rw, present, _pad;
} NmsRes;

typedef struct {
    unsigned long long frame_seq, publish_seq, cmdlist, feature, device, out_handle_addr;
    float jitter_x, jitter_y, mv_scale_x, mv_scale_y, sharpness;
    unsigned int width, height, out_width, out_height, reset, reserved_u[3];
    NmsRes res[5];                       /* 0=Color 1=Depth 2=MV 3=Output 4=Exposure */
    char _pad88[88];
} NmsFrame;

typedef struct {
    volatile unsigned long long seq; volatile unsigned long state; char _p[4];
    NmsFrame f;
} NmsSlot;

static int g_nms_want, g_nms_armed;
static unsigned long long g_nms_stage_bytes, g_nms_npudst_bytes;
static void* g_nms_mb;
static VkBuffer g_nms_marker, g_nms_stc, g_nms_stv, g_nms_std, g_nms_npu, g_nms_stamp;
static VkDeviceMemory g_nms_mems[6];
static void* g_nms_maps[6];
static unsigned long long g_nms_seq, g_nms_frames, g_nms_ack_ok, g_nms_ack_nowork, g_nms_ack_bad;
static void (VKAPI_PTR *s_nms_upd)(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, const void*);
static VkBuffer g_nms_hst_buf = VK_NULL_HANDLE;    /* shared b10 hst bank (chassis) */
static VkBuffer g_nms_recout_buf = VK_NULL_HANDLE; /* postA's u8 recout */
static VkBuffer g_nms_recp_buf = VK_NULL_HANDLE;   /* wv1's fp16-pair recp */

static void* nms_gipa(const char* name)
{
    static void* (VKAPI_PTR *gipa)(VkInstance, const char*);
    if (!gipa) {
        HMODULE v = GetModuleHandleA("vulkan-1.dll");
        if (v) gipa = (void* (VKAPI_PTR *)(VkInstance, const char*))(void*)GetProcAddress(v, "vkGetInstanceProcAddr");
    }
    return (gipa && g_vk_inst) ? gipa(g_vk_inst, name) : NULL;
}

/* one dedicated host-visible allocation per bank (the layer's record path
 * uses the handles; the worker uses the CPU maps — both published in the
 * mailbox header). Called ONCE at xessVKInit (contract-safe timing: the real
 * XeSS creates its resources at Init too). */
static int nms_make_bank(uint32_t host_type, unsigned long long size, VkBufferUsageFlags usage,
                         VkBuffer* buf, VkDeviceMemory* mem, void** map)
{
    typedef VkResult (VKAPI_PTR *PFN_cb)(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer*);
    typedef void     (VKAPI_PTR *PFN_gbr)(VkDevice, VkBuffer, VkMemoryRequirements*);
    typedef VkResult (VKAPI_PTR *PFN_am)(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*);
    typedef VkResult (VKAPI_PTR *PFN_bb)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
    typedef VkResult (VKAPI_PTR *PFN_mm)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void**);
    static PFN_cb fcb; static PFN_gbr fgbr; static PFN_am fam; static PFN_bb fbb; static PFN_mm fmm;
    if (!fcb) {
        fcb  = (PFN_cb)nms_gipa("vkCreateBuffer");
        fgbr = (PFN_gbr)nms_gipa("vkGetBufferMemoryRequirements");
        fam  = (PFN_am)nms_gipa("vkAllocateMemory");
        fbb  = (PFN_bb)nms_gipa("vkBindBufferMemory");
        fmm  = (PFN_mm)nms_gipa("vkMapMemory");
        if (!fcb || !fgbr || !fam || !fbb || !fmm) return 0;
    }
    {
        VkBufferCreateInfo bi; VkMemoryRequirements mr; VkMemoryAllocateInfo ai;
        memset(&bi, 0, sizeof bi);
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = (VkDeviceSize)size; bi.usage = usage;
        if (fcb(g_vk_dev, &bi, NULL, buf) != VK_SUCCESS) return 0;
        fgbr(g_vk_dev, *buf, &mr);
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size; ai.memoryTypeIndex = host_type;
        if (fam(g_vk_dev, &ai, NULL, mem) != VK_SUCCESS) return 0;
        if (fbb(g_vk_dev, *buf, *mem, 0) != VK_SUCCESS) return 0;
        if (fmm(g_vk_dev, *mem, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS) return 0;
        return 1;
    }
}

static void nms_arm(const xess_vk_init_params_t* p)
{
    typedef void (VKAPI_PTR *PFN_gmp)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);
    PFN_gmp fgmp = (PFN_gmp)nms_gipa("vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t host_type = 0, i;
    float s = qscale(p->qualitySetting);
    unsigned long long iw, ih, w2, h2;
    if (!fgmp || !g_vk_pd || !g_vk_dev) { L("nms-arm: no chain"); return; }
    fgmp(g_vk_pd, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            { host_type = i; break; }
    /* bank sizing: the quality-table answer (+ margin — the game rounds the
     * render height up; observed 752x422 asked -> 752x432 rendered) */
    iw = p->outputResolution.x / s; ih = p->outputResolution.y / s;
    w2 = ((unsigned long long)iw + 127ull) & ~127ull;
    h2 = ((unsigned long long)ih + 63ull) & ~63ull;
    g_nms_stage_bytes  = w2 * h2 * 4ull;
    g_nms_npudst_bytes = (unsigned long long)p->outputResolution.x * p->outputResolution.y * 4ull;
    if (!nms_make_bank(host_type, NMS_MARKER_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g_nms_marker, &g_nms_mems[0], &g_nms_maps[0]) ||
        !nms_make_bank(host_type, g_nms_stage_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_nms_stc, &g_nms_mems[1], &g_nms_maps[1]) ||
        !nms_make_bank(host_type, g_nms_stage_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g_nms_stv, &g_nms_mems[2], &g_nms_maps[2]) ||
        !nms_make_bank(host_type, g_nms_stage_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g_nms_std, &g_nms_mems[3], &g_nms_maps[3]) ||
        !nms_make_bank(host_type, g_nms_npudst_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_nms_npu, &g_nms_mems[4], &g_nms_maps[4]) ||
        !nms_make_bank(host_type, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g_nms_stamp, &g_nms_mems[5], &g_nms_maps[5])) {
        L("nms-arm: bank create FAILED (stage=%llu npudst=%llu)", g_nms_stage_bytes, g_nms_npudst_bytes);
        return;
    }
    memset(g_nms_maps[0], 0, (size_t)NMS_MARKER_BYTES);
    *(unsigned long long*)((char*)g_nms_maps[0] + 0x10) = 0x4D41524B45523031ull;
    memset(g_nms_maps[5], 0, 64);
    memset(g_nms_maps[4], 0, (size_t)g_nms_npudst_bytes);
    g_nms_mb = VirtualAlloc(NULL, (SIZE_T)NMS_MB_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_nms_mb) { L("nms-arm: VirtualAlloc failed"); return; }
    memset(g_nms_mb, 0, (size_t)NMS_MB_BYTES);
    {
        NmsHeader* h = (NmsHeader*)g_nms_mb;
        h->magic = NMS_MB_MAGIC; h->version = NMS_VERSION;
        h->seat_hint = (unsigned long long)GetTickCount64();
        h->slot_bytes = NMS_MB_SLOT; h->slots = NMS_MB_SLOTS;
        h->stamp_ptr        = (unsigned long long)(uintptr_t)g_nms_maps[5];
        h->stage_color_ptr  = (unsigned long long)(uintptr_t)g_nms_maps[1];
        h->stage_vel_ptr    = (unsigned long long)(uintptr_t)g_nms_maps[2];
        h->stage_depth_ptr  = (unsigned long long)(uintptr_t)g_nms_maps[3];
        h->npudst_ptr       = (unsigned long long)(uintptr_t)g_nms_maps[4];
        h->stage_color_buf  = (unsigned long long)(uintptr_t)g_nms_stc;
        h->stage_vel_buf    = (unsigned long long)(uintptr_t)g_nms_stv;
        h->stage_depth_buf  = (unsigned long long)(uintptr_t)g_nms_std;
        h->stamp_buf        = (unsigned long long)(uintptr_t)g_nms_stamp;
        h->npudst_buf       = (unsigned long long)(uintptr_t)g_nms_npu;
        h->stage_bytes      = g_nms_stage_bytes;
        h->npudst_bytes     = g_nms_npudst_bytes;
        h->out_w = p->outputResolution.x; h->out_h = p->outputResolution.y;
        MemoryBarrier();
    }
    g_nms_armed = 1;
    L("nms-arm: OK mb=%p stage=%llu (w2=%llu h2=%llu) npudst=%llu out=%ux%u",
      g_nms_mb, g_nms_stage_bytes, w2, h2, g_nms_npudst_bytes,
      p->outputResolution.x, p->outputResolution.y);
    /* rev3b: the compute chassis (feat+postA+RCAS+route), AFTER the basic
     * banks. FSR4NPU_CHASSIS=0 kills it (transport mode, rev3a shape).
     * Failure here NEVER disarms the seam — the blit route remains. */
    {
        char e[8]; DWORD n = GetEnvironmentVariableA("FSR4NPU_CHASSIS", e, sizeof e);
        int want_chassis = !(n == 1 && e[0] == '0');
        if (want_chassis) {
            if (nms_chassis_feat_posta(host_type)) {
                nms_chassis_rcas(host_type);
                if (nms_chassis_rec_bridge(host_type)) L("nms-chassis: REC BRIDGE armed (plan3)");
            }
            else L("nms-chassis: feat stage failed — transport mode (rev3a shape)");
        } else L("nms-chassis: disabled by env (transport mode)");
    }
}

/* ============ SEAT rev3b: the compute chassis (feat + postA + RCAS) ============
 * Port of the R59 seat-side chassis (fsr4-npu/d1/proxy/nvngx_vk_seat.c
 * seam_comp_wv1_chassis + seam_comp_rcas_stage2, the REV22+ PRODUCTION
 * build) at its native geometry LR=640x360 -> OS=1280x720. Everything is
 * created ON THE GAME'S DEVICE at xessVKInit via the same nms_gipa route
 * that already built the marker/banks (sanctioned in-process client at
 * init; the rev19/21 second-instance killer avoided). Unlike the R59
 * original this port uses the REAL vulkan_core.h types (the R59 seat was
 * NDK headerless; we are not). Published through the NMS-1 header tail:
 * plan[0]=feat(wv1) plan[1]=postA(wv2a) plan[2]=RCAS(wv2b, out16=1) +
 * the NPU handshake (outq/npu-in maps) + the route (fp16 bank -> probe
 * image fmt 97). The layer records the dispatches + route at doorbell
 * time. LAWS carried: banks stay mapped; the route bank keeps the r58s
 * SLACK allocation (1920*1080*8 — exact-fit died at eval 64 3/3 boots);
 * failure never disarms the seam (the blit route remains); weights come
 * from pass0_wb.raw next to this DLL. */
#include "wv1_spv.h"
#include "wv2a_spv.h"
#include "wv2b_spv.h"
#include "exp8to16_spv.h"
#include <math.h>

/* ---- chassis helpers ---- */
static void nms_comp_publish(int stage, VkPipeline pipe, VkPipelineLayout layout,
                             VkDescriptorSet set, unsigned dgx, unsigned dgy,
                             const unsigned int* push_words, unsigned push_bytes, unsigned patch)
{
    NmsHeader* h = (NmsHeader*)g_nms_mb;
    NmsPlan* p;
    unsigned i;
    if (!h) return;
    p = &h->plan[stage];
    p->comp_pipe     = (unsigned long long)(uintptr_t)pipe;
    p->comp_layout   = (unsigned long long)(uintptr_t)layout;
    p->comp_set      = (unsigned long long)(uintptr_t)set;
    p->comp_dispatch = (unsigned long long)dgx | ((unsigned long long)dgy << 32);
    p->comp_dgz_push = 1ull | ((unsigned long long)push_bytes << 32);
    for (i = 0; i < 11 && i * 4 < push_bytes; i++) p->comp_push[i] = push_words[i];
    p->comp_patch = patch;
    MemoryBarrier();
    L("nms-chassis: PUBLISHED plan%d pipe=%p set=%p dispatch=%ux%u push=%uB patch=0x%X",
      stage, (void*)pipe, (void*)set, dgx, dgy, push_bytes, patch);
}

/* IEEE binary16 pack (test-pattern class — denorms clamp to 0) */
static unsigned int nms_f16(float v)
{
    unsigned int x, sign, h; int e;
    memcpy(&x, &v, 4);
    sign = (x >> 16) & 0x8000u;
    e = (int)((x >> 23) & 0xffu) - 127 + 15;
    if (((x >> 23) & 0xffu) == 0xffu) h = 0x7c00u;
    else if (((x >> 23) & 0xffu) == 0) h = 0;
    else if (e >= 31) h = 0x7bffu;
    else if (e <= 0) h = 0;
    else h = ((unsigned int)e << 10) | ((x & 0x7fffffu) >> 13);
    return sign | h;
}

static int nms_read_dll_file(const char* name, void* buf, unsigned long want)
{
    char p[MAX_PATH]; HMODULE m = GetModuleHandleA(NULL);
    /* the seat DLL's own dir (not the exe's): same place xess_proxy.log lives */
    static char dll_dir[MAX_PATH];
    static int have_dir;
    if (!have_dir) {
        HMODULE self = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&nms_f16, &self);
        if (self && GetModuleFileNameA(self, dll_dir, MAX_PATH)) {
            char* slash = strrchr(dll_dir, '\\');
            if (slash) *slash = 0;
            have_dir = 1;
        }
    }
    if (!have_dir) return 0;
    snprintf(p, sizeof p, "%s\\%s", dll_dir, name);
    { HANDLE hf = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
      unsigned long got = 0;
      if (hf == INVALID_HANDLE_VALUE) return 0;
      if (!ReadFile(hf, buf, want, &got, NULL) || got != want) { CloseHandle(hf); return 0; }
      CloseHandle(hf);
      return 1; }
    (void)m;
}

/* one STORAGE (+optional transfer) bank on the game device; STAYS MAPPED */
static int nms_chassis_bank(uint32_t type_idx, unsigned long long bytes, VkBufferUsageFlags usage,
                            VkBuffer* buf, VkDeviceMemory* mem, void** map)
{
    typedef VkResult (VKAPI_PTR *PFN_xcb)(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer*);
    typedef void     (VKAPI_PTR *PFN_xgbr)(VkDevice, VkBuffer, VkMemoryRequirements*);
    typedef VkResult (VKAPI_PTR *PFN_xam)(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*);
    typedef VkResult (VKAPI_PTR *PFN_xbb)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
    typedef VkResult (VKAPI_PTR *PFN_xmm)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void**);
    static PFN_xcb fcb; static PFN_xgbr fgbr; static PFN_xam fam; static PFN_xbb fbb; static PFN_xmm fmm;
    if (!fcb) {
        fcb  = (PFN_xcb)nms_gipa("vkCreateBuffer");
        fgbr = (PFN_xgbr)nms_gipa("vkGetBufferMemoryRequirements");
        fam  = (PFN_xam)nms_gipa("vkAllocateMemory");
        fbb  = (PFN_xbb)nms_gipa("vkBindBufferMemory");
        fmm  = (PFN_xmm)nms_gipa("vkMapMemory");
        if (!fcb || !fgbr || !fam || !fbb || !fmm) return 0;
    }
    { VkBufferCreateInfo bi; VkMemoryRequirements mr; VkMemoryAllocateInfo ai;
      memset(&bi, 0, sizeof bi);
      bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      bi.size = (VkDeviceSize)bytes; bi.usage = usage;
      if (fcb(g_vk_dev, &bi, NULL, buf) != VK_SUCCESS || !*buf) return 0;
      fgbr(g_vk_dev, *buf, &mr);
      memset(&ai, 0, sizeof ai);
      ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      ai.allocationSize = mr.size; ai.memoryTypeIndex = type_idx;
      if (fam(g_vk_dev, &ai, NULL, mem) != VK_SUCCESS || !*mem) return 0;
      if (fbb(g_vk_dev, *buf, *mem, 0) != VK_SUCCESS) return 0;
      if (map && fmm(g_vk_dev, *mem, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS) return 0;
      return 1; }
}

/* the pipeline-maker (module from SPV words, one binding set, push range) */
typedef struct {
    VkShaderModule mod; VkDescriptorSetLayout dsl; VkPipelineLayout playou;
    VkPipeline pipe; VkDescriptorPool pool; VkDescriptorSet set;
} NmsPipe;
static int nms_chassis_pipeline(const unsigned int* spv_words, unsigned spv_nwords,
                                const unsigned int* binds, unsigned nbinds,
                                unsigned push_bytes,
                                const VkDescriptorBufferInfo* bi,
                                NmsPipe* out)
{
    typedef VkResult (VKAPI_PTR *PFN_xsm)(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
    typedef VkResult (VKAPI_PTR *PFN_xdsl)(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout*);
    typedef VkResult (VKAPI_PTR *PFN_xpl)(VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout*);
    typedef VkResult (VKAPI_PTR *PFN_xcp)(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
    typedef VkResult (VKAPI_PTR *PFN_xpool)(VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool*);
    typedef VkResult (VKAPI_PTR *PFN_xads)(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*);
    typedef void     (VKAPI_PTR *PFN_xuds)(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*);
    static PFN_xsm fsm; static PFN_xdsl fdsl; static PFN_xpl fpl; static PFN_xcp fcp; static PFN_xpool fpool; static PFN_xads fads; static PFN_xuds fuds;
    if (!fsm) {
        fsm = (PFN_xsm)nms_gipa("vkCreateShaderModule");
        fdsl = (PFN_xdsl)nms_gipa("vkCreateDescriptorSetLayout");
        fpl = (PFN_xpl)nms_gipa("vkCreatePipelineLayout");
        fcp = (PFN_xcp)nms_gipa("vkCreateComputePipelines");
        fpool = (PFN_xpool)nms_gipa("vkCreateDescriptorPool");
        fads = (PFN_xads)nms_gipa("vkAllocateDescriptorSets");
        fuds = (PFN_xuds)nms_gipa("vkUpdateDescriptorSets");
        if (!fsm || !fdsl || !fpl || !fcp || !fpool || !fads || !fuds) return 0;
    }
    { VkShaderModuleCreateInfo smci; VkDescriptorSetLayoutBinding lb[16]; VkDescriptorSetLayoutCreateInfo dslci;
      VkPushConstantRange pcr; VkPipelineLayoutCreateInfo plci; VkComputePipelineCreateInfo cpci;
      VkPipelineShaderStageCreateInfo stg; VkDescriptorPoolSize psz; VkDescriptorPoolCreateInfo poolci;
      VkDescriptorSetAllocateInfo dsai; VkWriteDescriptorSet wr[16];
      VkDescriptorSetLayout setLayoutsArr[1]; VkDescriptorSet dsOut = VK_NULL_HANDLE;
      unsigned i;
      memset(&smci, 0, sizeof smci);
      smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      smci.codeSize = (size_t)spv_nwords * 4;
      smci.pCode = spv_words;
      if (fsm(g_vk_dev, &smci, NULL, &out->mod) != VK_SUCCESS || !out->mod) return 0;
      memset(lb, 0, sizeof lb);
      /* lb/wr cap raised 8->16 with the depth banks (binding 16/17 were
       * silently UNBOUND in the dilate era — harmless while DILATE=0 gated
       * the reads, but the descriptors must exist for any depth work) */
      for (i = 0; i < nbinds && i < 16; i++) {
          lb[i].binding = binds[i];
          lb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          lb[i].descriptorCount = 1;
          lb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      }
      memset(&dslci, 0, sizeof dslci);
      dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      dslci.bindingCount = nbinds; dslci.pBindings = lb;
      if (fdsl(g_vk_dev, &dslci, NULL, &out->dsl) != VK_SUCCESS || !out->dsl) return 0;
      memset(&pcr, 0, sizeof pcr);
      pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; pcr.offset = 0; pcr.size = push_bytes;
      setLayoutsArr[0] = out->dsl;
      memset(&plci, 0, sizeof plci);
      plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
      plci.setLayoutCount = 1; plci.pSetLayouts = setLayoutsArr;
      plci.pushConstantRangeCount = push_bytes ? 1 : 0; plci.pPushConstantRanges = &pcr;
      if (fpl(g_vk_dev, &plci, NULL, &out->playou) != VK_SUCCESS || !out->playou) return 0;
      memset(&stg, 0, sizeof stg);
      stg.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stg.stage = VK_SHADER_STAGE_COMPUTE_BIT; stg.module = out->mod; stg.pName = "main";
      memset(&cpci, 0, sizeof cpci);
      cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      cpci.stage = stg; cpci.layout = out->playou;
      if (fcp(g_vk_dev, VK_NULL_HANDLE, 1, &cpci, NULL, &out->pipe) != VK_SUCCESS || !out->pipe) return 0;
      memset(&psz, 0, sizeof psz);
      psz.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; psz.descriptorCount = nbinds;
      memset(&poolci, 0, sizeof poolci);
      poolci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      poolci.maxSets = 1; poolci.poolSizeCount = 1; poolci.pPoolSizes = &psz;
      if (fpool(g_vk_dev, &poolci, NULL, &out->pool) != VK_SUCCESS || !out->pool) return 0;
      memset(&dsai, 0, sizeof dsai);
      dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      dsai.descriptorPool = out->pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = setLayoutsArr;
      if (fads(g_vk_dev, &dsai, &dsOut) != VK_SUCCESS || !dsOut) return 0;
      out->set = dsOut;
      memset(wr, 0, sizeof wr);
      for (i = 0; i < nbinds && i < 16; i++) {
          wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          wr[i].dstSet = out->set; wr[i].dstBinding = binds[i];
          wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          wr[i].pBufferInfo = &bi[i];
      }
      fuds(g_vk_dev, nbinds, wr, 0, NULL);
      return 1; }
}

/* feat(wv1) + postA(wv2a) chassis at LR=640x360 OS=1280x720 — the R59
 * production shape; publishes plans 0/1 + the NPU handshake. Returns 1 on
 * the full chain (postA optional: plan1 absent = feat-only). */
/* rev3b.2 DIAGNOSTICS THREAD: samples the mapped banks every 2s and logs
 * compact stats (CLI-solvable image forensics — no screenshots needed):
 *   route (fp16 RGBA, the FINAL image RCAS wrote -> what the player sees)
 *   npuin (int8 pairs, the NPU's weights), outq (int8 features),
 *   rawlr (B10G11R11 words, decoded value range), mvec (f16 xy magnitude) */
static unsigned int* g_diag_route;      /* route bank map (fp16) */
static signed char*  g_diag_npuin;
static signed char*  g_diag_outq;
static unsigned int* g_diag_rawlr;
static unsigned int* g_diag_mvec;
static unsigned int* g_diag_dpt;       /* D32 depth words (inverted: larger=closer) */
static char g_nms_diag_dll_dir[MAX_PATH];  /* the diag thread's own DLL dir (markers: DUMP/AUTOEXPO pattern) */
static unsigned short* g_diag_hist;      /* mu-law fp16 pairs (postA out) */
static unsigned short* g_diag_recp;      /* linear fp16 pairs */

/* rev3c.3: dump one mapped bank NEXT TO THE DLL (the dir the log already
 * writes to — proven). CreateFileA + GetLastError so failures are LOUD; the
 * rev3c.2 code fopen'd "C:\fsr4npu_route.raw" where \f is a FORM-FEED escape,
 * failed silently, and still logged "DUMPED". */
static void nms_diag_dump(const char* dll_dir, const char* name, const void* p, unsigned long long bytes)
{
    char path[MAX_PATH + 32]; HANDLE hf; unsigned long got = 0;
    if (!p) { L("diag-dump: %s SKIPPED (map null)", name); return; }
    snprintf(path, sizeof path, "%s\\%s", dll_dir, name);
    hf = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        L("diag-dump: %s OPEN FAILED gle=%lu path=%s", name, (unsigned long)GetLastError(), path);
        return;
    }
    if (!WriteFile(hf, p, (unsigned long)bytes, &got, NULL) || got != (unsigned long)bytes)
        L("diag-dump: %s WRITE FAILED gle=%lu got=%lu want=%llu", name,
          (unsigned long)GetLastError(), got, bytes);
    else
        L("diag-dump: %s %lluB ok", name, bytes);
    CloseHandle(hf);
}

static DWORD WINAPI nms_diag_thread(LPVOID unused)
{
    (void)unused;
    { HMODULE self = NULL;
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)&nms_diag_thread, &self) && self)
          if (GetModuleFileNameA(self, g_nms_diag_dll_dir, MAX_PATH)) {
              char* slash = strrchr(g_nms_diag_dll_dir, '\\');
              if (slash) *slash = 0;
          } }
    for (;;) {
        Sleep(2000);
        /* rev3c.10 ZCC2-NMS: the seat-side import poller (the layer
         * published the worker's out-fd in the header tail; our device-fn
         * path is the proven one — 10 banks created every boot). Runs on
         * this quiet thread, once, idempotent via zcc2_state. */
        if (g_nms_mb && g_vk_dev) {
            NmsHeader* h = (NmsHeader*)g_nms_mb;
            if (h->zcc2_out_fd && h->zcc2_state < 2ull) {
                h->zcc2_state = 1ull;
                { typedef VkResult (VKAPI_PTR *PFN_zcb)(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer*);
                  typedef VkResult (VKAPI_PTR *PFN_zam)(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*);
                  typedef VkResult (VKAPI_PTR *PFN_zbb)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
                  typedef VkResult (VKAPI_PTR *PFN_zgfp)(VkDevice, VkExternalMemoryHandleTypeFlagBits, int, VkMemoryFdPropertiesKHR*);
                  static PFN_zcb zcb; static PFN_zam zam; static PFN_zbb zbb; static PFN_zgfp zgfp;
                  if (!zcb) {
                      zcb  = (PFN_zcb)nms_gipa("vkCreateBuffer");
                      zam  = (PFN_zam)nms_gipa("vkAllocateMemory");
                      zbb  = (PFN_zbb)nms_gipa("vkBindBufferMemory");
                      zgfp = (PFN_zgfp)nms_gipa("vkGetMemoryFdPropertiesKHR");   /* optional: NULL under wine is fine, type 0 fallback */
                  }
                  if (zcb && zam && zbb) {
                      int fd = (int)h->zcc2_out_fd;   /* the layer already published a native-side SPARE dup (wine's CRT _dup can't see Linux fds) */
                      uint32_t type_idx = 0;
                      if (fd >= 0) {
                          if (zgfp) { VkMemoryFdPropertiesKHR fdp;
                            memset(&fdp, 0, sizeof fdp); fdp.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
                            if (zgfp(g_vk_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdp) == VK_SUCCESS) {
                                uint32_t t;
                                for (t = 0; t < 32u; t++)
                                    if ((fdp.memoryTypeBits >> t) & 1u) { type_idx = t; break; }
                            } }
                          { VkExternalMemoryBufferCreateInfo xb; VkBufferCreateInfo bi;
                            VkImportMemoryFdInfoKHR imp; VkMemoryAllocateInfo ai;
                            VkBuffer zbuf = VK_NULL_HANDLE; VkDeviceMemory zmem = VK_NULL_HANDLE;
                            memset(&xb, 0, sizeof xb);
                            xb.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
                            xb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
                            memset(&bi, 0, sizeof bi);
                            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                            bi.pNext = &xb;
                            bi.size = (VkDeviceSize)h->npudst_bytes;
                            bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                            if (zcb(g_vk_dev, &bi, NULL, &zbuf) == VK_SUCCESS && zbuf) {
                                memset(&imp, 0, sizeof imp);
                                imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
                                imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
                                imp.fd = fd;
                                memset(&ai, 0, sizeof ai);
                                ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                                ai.pNext = &imp;
                                ai.allocationSize = (VkDeviceSize)h->npudst_bytes;
                                ai.memoryTypeIndex = type_idx;
                                if (zam(g_vk_dev, &ai, NULL, &zmem) == VK_SUCCESS && zmem &&
                                    zbb(g_vk_dev, zbuf, zmem, 0) == VK_SUCCESS) {
                                    h->zcc2_imported_buf = (unsigned long long)(uintptr_t)zbuf;
                                    h->zcc2_state = 2ull;
                                    L("nms-zcc2: seat import OK buf=%llu type=%u", h->zcc2_imported_buf, type_idx);
                                } else {
                                    h->zcc2_state = 3ull;
                                    L("nms-zcc2: seat import FAILED (alloc/bind)");
                                }
                            } else {
                                h->zcc2_state = 3ull;
                                L("nms-zcc2: seat import FAILED (buffer create)");
                            } }
                      } else { h->zcc2_state = 3ull; L("nms-zcc2: bad fd"); }
                  } else { h->zcc2_state = 3ull; L("nms-zcc2: fn resolve failed"); }
                }
            }
            /* ZCC3 (the IN side): import the worker's IN rpcmem (the layer
             * published a spare dup in zcc3_in_fd); the game CB's recorded
             * copy outq -> imported-IN retires the worker's CPU bridge. */
            if (h->zcc3_in_fd && !h->zcc3_imported_in_buf) {
                static int zcc3_done = 0;
                if (!zcc3_done) {
                    zcc3_done = 1;
                    typedef VkResult (VKAPI_PTR *PFN_zcb)(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer*);
                    typedef VkResult (VKAPI_PTR *PFN_zam)(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*);
                    typedef VkResult (VKAPI_PTR *PFN_zbb)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
                    static PFN_zcb zcb3; static PFN_zam zam3; static PFN_zbb zbb3;
                    if (!zcb3) {
                        zcb3 = (PFN_zcb)nms_gipa("vkCreateBuffer");
                        zam3 = (PFN_zam)nms_gipa("vkAllocateMemory");
                        zbb3 = (PFN_zbb)nms_gipa("vkBindBufferMemory");
                    }
                    if (zcb3 && zam3 && zbb3) {
                        int fd = (int)h->zcc3_in_fd;   /* the layer's spare dup; the import consumes it */
                        VkExternalMemoryBufferCreateInfo xb;
                        VkBufferCreateInfo bi;
                        VkImportMemoryFdInfoKHR imp;
                        VkMemoryAllocateInfo ai;
                        VkBuffer zbuf = VK_NULL_HANDLE; VkDeviceMemory zmem = VK_NULL_HANDLE;
                        memset(&xb, 0, sizeof xb);
                        xb.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
                        xb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
                        memset(&bi, 0, sizeof bi);
                        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                        bi.pNext = &xb;
                        bi.size = (VkDeviceSize)h->outq_bytes;
                        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                        if (zcb3(g_vk_dev, &bi, NULL, &zbuf) == VK_SUCCESS && zbuf) {
                            memset(&imp, 0, sizeof imp);
                            imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
                            imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
                            imp.fd = fd;
                            memset(&ai, 0, sizeof ai);
                            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                            ai.pNext = &imp;
                            ai.allocationSize = (VkDeviceSize)h->outq_bytes;
                            ai.memoryTypeIndex = 0;
                            if (zam3(g_vk_dev, &ai, NULL, &zmem) == VK_SUCCESS && zmem &&
                                zbb3(g_vk_dev, zbuf, zmem, 0) == VK_SUCCESS) {
                                h->zcc3_imported_in_buf = (unsigned long long)(uintptr_t)zbuf;
                                L("nms-zcc3: seat import OK buf=%llu", h->zcc3_imported_in_buf);
                            } else L("nms-zcc3: seat import FAILED (alloc/bind)");
                        } else L("nms-zcc3: seat import FAILED (buffer create)");
                    } else L("nms-zcc3: fn resolve failed");
                }
            }
        }
        if (g_diag_route) {
            unsigned short* w = (unsigned short*)g_diag_route;
            /* sample stride-911 texels, first 4 fp16 of each (r,g,b,a) */
            double sum = 0; long nz = 0, n = 0; float mx = 0;
            unsigned i;
            for (i = 0; i < 4096; i++) {
                unsigned short h = w[(unsigned long long)i * 900ull];       /* rev3c.6: stride INSIDE the 7.37MB used span (the old 2048*2B stride ran 188KB past the 16.6MB slack alloc — the OOB tail read adjacent-device fp16 and faked a "blown route" mean of ~22) */
                unsigned e = (h >> 10) & 0x1f; float v = 0;
                unsigned int b32; float f32;
                if (e == 31) v = 65504.0f;
                else if (e > 0) { b32 = ((unsigned int)(e - 15 + 127) << 23) | ((unsigned int)(h & 0x3ff) << 13); memcpy(&f32, &b32, 4); v = f32; }
                sum += v; n++;
                if (v != 0.0f) nz++;
                if (v > mx) mx = v;
            }
            L("diag-route: rch mean=%.4f max=%.3f nz=%ld/%ld", sum / n, mx, nz, n);
        }
        if (g_diag_npuin) {
            long sum = 0, nz = 0; int mn = 127, mx = -128; unsigned i;
            for (i = 0; i < 2048; i++) {
                int v = g_diag_npuin[(unsigned long long)i * 509ull];
                sum += v; if (v) nz++; if (v < mn) mn = v; if (v > mx) mx = v;
            }
            L("diag-npuin: mean=%.1f min=%d max=%d nz=%ld/2048", (double)sum / 2048.0, mn, mx, nz);
        }
        if (g_diag_outq) {
            long sum = 0, nz = 0; int mn = 127, mx = -128; unsigned i;
            for (i = 0; i < 2048; i++) {
                int v = g_diag_outq[(unsigned long long)i * 1801ull];
                sum += v; if (v) nz++; if (v < mn) mn = v; if (v > mx) mx = v;
            }
            L("diag-outq: mean=%.1f min=%d max=%d nz=%ld/2048", (double)sum / 2048.0, mn, mx, nz);
        }
        if (g_diag_rawlr) {
            /* decode R (low 11 bits) via f16-shift; report range/mean */
            double sum = 0; float mx = 0; long nz = 0; unsigned i;
            double logsum = 0; long logn = 0;
            for (i = 0; i < 2048; i++) {
                unsigned int w32 = g_diag_rawlr[(unsigned long long)i * 112ull];      /* 230,400 words */
                unsigned short h = (unsigned short)((w32 & 0x7ffu) << 4);
                unsigned e = (h >> 10) & 0x1f; float v = 0; unsigned int b32; float f32;
                if (e == 31) v = 65504.0f;
                else if (e > 0) { b32 = ((unsigned int)(e - 15 + 127) << 23) | ((unsigned int)(h & 0x3ff) << 13); memcpy(&f32, &b32, 4); v = f32; }
                sum += v; if (v != 0.0f) nz++; if (v > mx) mx = v;
                /* AMD SPD auto-exposure metric (spd_auto_exposure.hlsl, the
                 * EXACT chain): per-pixel Y -> log(max(eps,Y)); SPD averages
                 * the LOG -> geometric mean; EV100 = log2(Lavg*100/12.5);
                 * exposure = 1/(1.2*2^EV100). Y = the TRUE 3-channel luma
                 * (R-only proxy was 60x off — NMS is blue-dominated). */
                { float rr = v;   /* R from the decode above */
                  float gg, bb;
                  { unsigned short hg = (unsigned short)(((w32 >> 11) & 0x7ffu) << 4);
                    unsigned eg = (hg >> 10) & 0x1f; gg = 0;
                    if (eg == 31) gg = 65504.0f;
                    else if (eg > 0) { b32 = ((unsigned int)(eg - 15 + 127) << 23) | ((unsigned int)(hg & 0x3ff) << 13); memcpy(&f32, &b32, 4); gg = f32; } }
                  { unsigned short hb = (unsigned short)(((w32 >> 22) & 0x3ffu) << 5);
                    unsigned eb = (hb >> 10) & 0x1f; bb = 0;
                    if (eb == 31) bb = 65504.0f;
                    else if (eb > 0) { b32 = ((unsigned int)(eb - 15 + 127) << 23) | ((unsigned int)(hb & 0x3ff) << 13); memcpy(&f32, &b32, 4); bb = f32; } }
                  { float Y = 0.2126f * rr + 0.7152f * gg + 0.0722f * bb;
                    if (Y > 1e-6f && Y < 65504.0f) { logsum += log((double)Y); logn++; } } }
            }
            L("diag-rawlr-R: mean=%.3f max=%.3f nz=%ld/2048", sum / 2048.0, mx, nz);
            if (logn > 64) {
                /* AMD SPD auto-exposure (spd_auto_exposure.hlsl, exact
                 * chain), 3-CHANNEL luma (the R-only proxy was 60x off —
                 * NMS scenes are blue-dominated: meanRGB(0.004,0.004,0.247)).
                 * Runtime-killable (the strobe law): the marker is re-checked
                 * EVERY cycle; the 1->0 transition restores EXPO=4.0. Output
                 * clamped to [0.5,32] (validated range 1..8 + headroom). */
                /* AMD SPD auto-exposure v4 — delivery via the STAMP BANK
                 * tail (u64[6]=exposure bits, u64[7]=magic): shared memory,
                 * no filesystem, no wine drive mapping (the Z: path and
                 * the path-mangling history: runbook §23.19). Marker:
                 * dll-dir fsr4npu-AUTOEXPO (the DUMP-marker pattern),
                 * re-checked EVERY cycle (killable); disarm clears the
                 * magic so the layer falls back to the EXPO knob file. */
                {
                    static int autoexpo = -1;
                    static char aemark[300];
                    if (!aemark[0] && g_nms_diag_dll_dir[0])
                        snprintf(aemark, sizeof aemark, "%s\\fsr4npu-AUTOEXPO", g_nms_diag_dll_dir);
                    int want_auto = aemark[0] && GetFileAttributesA(aemark) != INVALID_FILE_ATTRIBUTES;
                    if (autoexpo < 0) autoexpo = want_auto;
                    unsigned long long* st6 = g_nms_maps[5] ? (unsigned long long*)g_nms_maps[5] : 0;
                    if (st6) {
                        if (autoexpo && !want_auto) {   /* disarm: clear the magic */
                            autoexpo = 0;
                            st6[7] = 0;
                            L("diag-autoexpo: disarmed (stamp magic cleared)");
                        }
                        if (autoexpo) {
                            double Lavg = exp(logsum / (double)logn);
                            double EV100 = log2(Lavg * 100.0 / 12.5);
                            double exposure = 1.0 / (1.2 * pow(2.0, EV100));
                            /* preExposure calibration: NMS delivers pre-tonemapped
                             * color; typical-scene metric 0.27 <-> validated 4.0
                             * => preExposure = 0.0675. Clamp [0.5,32]. */
                            exposure /= 0.0675;
                            if (exposure < 0.5) exposure = 0.5;
                            if (exposure > 32.0) exposure = 32.0;
                            { unsigned long long eb; float ef = (float)exposure;
                              memcpy(&eb, &ef, 4);
                              st6[6] = eb;
                              st6[7] = 0x4158504F455A4F43ull;   /* "AXPOEZOC" */
                              MemoryBarrier();
                              L("diag-autoexpo: Lavg=%.5f EV100=%.2f expo=%.4f", Lavg, EV100, exposure); }
                        }
                    }
                }
            }
        }
        {   /* rev3c.5 dumps: (a) exec-count stages at 1500/4500 (past the
             * cold-start transient — NOT diag cycles: the thread starts at
             * xessVKInit and boot screens delay the first Execute; cycle
             * gating dumped pre-Execute memsets), (b) MARKER dumps: create
             * Binaries/fsr4npu-DUMP -> immediate all-bank dump, marker
             * consumed. The expo/quality iteration instrument — no relaunch. */
            static int dumped1, dumped2;
            static const unsigned long long T1 = 1500, T2 = 4500;
            static char dll_dir[MAX_PATH];
            static int have_dir;
            unsigned long long ex = g_execs;
            int want = 0; const char* why = "";
            if (!have_dir) {
                HMODULE self2 = NULL; char dd[MAX_PATH];
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCSTR)&nms_f16, &self2) && self2 && GetModuleFileNameA(self2, dd, MAX_PATH)) {
                    char* slash = strrchr(dd, '\\');
                    if (slash) { *slash = 0; snprintf(dll_dir, sizeof dll_dir, "%s", dd); have_dir = 1; }
                }
            }
            if (have_dir) {
                if (!dumped1 && ex >= T1) { dumped1 = 1; want = 1; why = "stage1"; }
                else if (dumped1 && !dumped2 && ex >= T2) { dumped2 = 1; want = 1; why = "stage2"; }
                else {
                    char mp[MAX_PATH + 32];
                    HANDLE mf;
                    snprintf(mp, sizeof mp, "%s\\fsr4npu-DUMP", dll_dir);
                    mf = CreateFileA(mp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
                    if (mf != INVALID_HANDLE_VALUE) {
                        CloseHandle(mf); DeleteFileA(mp);
                        want = 1; why = "marker";
                    }
                }
                if (want) {
                    L("diag-dump: %s execs=%llu dir=%s", why, ex, dll_dir);
                    nms_diag_dump(dll_dir, "fsr4npu_route.raw", g_diag_route, 1280ull * 720ull * 8ull);
                    nms_diag_dump(dll_dir, "fsr4npu_npuout.raw", g_diag_npuin, 1280ull * 720ull * 8ull);
                    nms_diag_dump(dll_dir, "fsr4npu_outq.raw",  g_diag_outq,   640ull * 360ull * 16ull);
                    nms_diag_dump(dll_dir, "fsr4npu_rawlr.raw", g_diag_rawlr,  640ull * 368ull * 4ull);
                    nms_diag_dump(dll_dir, "fsr4npu_mvec.raw",  g_diag_mvec,   640ull * 368ull * 4ull);
                    nms_diag_dump(dll_dir, "fsr4npu_dpt.raw",   g_diag_dpt,    640ull * 368ull * 4ull);
                    nms_diag_dump(dll_dir, "fsr4npu_hist.raw",  g_diag_hist,   1280ull * 720ull * 8ull);
                    nms_diag_dump(dll_dir, "fsr4npu_recp.raw",  g_diag_recp,   1280ull * 720ull * 8ull);
                }
            }
        }
        if (g_diag_hist) {
            /* mu values are the SECOND fp16 of each pair-pack: word layout is
             * [r | g] [b | -] per texel; sample r and b channels */
            double sum = 0; float mx = 0; long over = 0; unsigned i;
            for (i = 0; i < 2048; i++) {
                unsigned short h = g_diag_hist[(unsigned long long)i * 1800ull];      /* 7.37MB bank: stride must fit */
                unsigned e = (h >> 10) & 0x1f; float v = 0; unsigned int b32; float f32;
                if (e == 31) v = 65504.0f;
                else if (e > 0) { b32 = ((unsigned int)(e - 15 + 127) << 23) | ((unsigned int)(h & 0x3ff) << 13); memcpy(&f32, &b32, 4); v = f32; }
                sum += v; if (v > mx) mx = v; if (v > 0.70f) over++;
            }
            L("diag-hist: mu mean=%.3f max=%.3f over0.70=%ld/2048", sum / 2048.0, mx, over);
        }
        if (g_diag_recp) {
            double sum = 0; float mx = 0; unsigned i;
            for (i = 0; i < 1024; i++) {
                unsigned short h = g_diag_recp[(unsigned long long)i * 1800ull];      /* 7.37MB bank */
                unsigned e = (h >> 10) & 0x1f; float v = 0; unsigned int b32; float f32;
                if (e == 31) v = 65504.0f;
                else if (e > 0) { b32 = ((unsigned int)(e - 15 + 127) << 23) | ((unsigned int)(h & 0x3ff) << 13); memcpy(&f32, &b32, 4); v = f32; }
                sum += v; if (v > mx) mx = v;
            }
            L("diag-recp: mean=%.3f max=%.3f", sum / 1024.0, mx);
        }
        if (g_diag_mvec) {
            double mag = 0; unsigned i;
            for (i = 0; i < 1024; i++) {
                unsigned int w32 = g_diag_mvec[(unsigned long long)i * 112ull];      /* 230,400 words */
                unsigned short hx = (unsigned short)(w32 & 0xffffu);
                /* f16 magnitude approx: exponent-only scale */
                mag += ((hx >> 10) & 0x1f) ? 1.0 : 0.0;
            }
            L("diag-mvec: nonzero-frac=%.3f", mag / 1024.0);
        }
        if (g_diag_dpt) {   /* stride 112 words * 4B * 1024 = 458,752 <= 942,080 bank */
            double sum = 0, mn = 1e30, mx = -1e30; unsigned i, n = 0;
            for (i = 0; i < 1024; i++) {
                unsigned int w32 = g_diag_dpt[(unsigned long long)i * 112ull];
                float d; memcpy(&d, &w32, 4);
                if (!(d == d)) continue;               /* NaN skip */
                sum += d; n++; if (d < mn) mn = d; if (d > mx) mx = d;
            }
            if (n) L("diag-dpt: mean=%.4f min=%.4f max=%.4f n=%u (inverted: larger=closer)",
                     sum / n, mn, mx, n);
            else L("diag-dpt: ALL NaN");
        }
    }
}

/* ---- RATIO ERA: the tier table (AMD's own selector, ffx_provider_fsr4_dx12
 * :1248-1255 — ratio >= 1.49 Quality, >= 1.69 Balanced, >= 1.99 Performance,
 * >= 2.99 UltraPerf, else NativeAA; seven XeSS presets collapse onto five
 * weight sets: Native/UQ+/UQ all take NativeAA). The NPU input/output grids
 * are IDENTICAL shapes for every tier (same topology, weights only), so a
 * tier swap is: pass0 raw -> wb bank, rcps -> feat push word 10, s_out ->
 * postA push word 9, and the layer swaps the context bin (bin_cfnhwc_<t>).
 * rcps = 1/NPU-input-quant-scale (full float64 from each build's verify
 * JSON); quality keeps its validated authored literal 66.65824. */
static const struct { const char* name; const char* pass0; float rcps; float s_out; }
NMS_TIER[5] = {
    { "native",    "pass0_native.raw",    68.45285451054991f, 0.3037116527557373f  },
    { "quality",   "pass0_quality.raw",   66.65824f,          0.303211569786f      },
    { "balanced",  "pass0_balanced.raw",  80.15899365374828f, 0.28242796659469604f },
    { "perf",      "pass0_perf.raw",      67.5152934071938f,  0.32062485814094543f },
    { "ultraperf", "pass0_ultraperf.raw", 83.67477504464507f, 0.2512109577655792f  },
};
/* max game-LR across presets: UQ+ asks 1066x600 (q105), the game rounds the
 * render height UP (observed +10) — 1152x640 covers every preset + margin */
#define NMS_LR_BANK_W 1152ull
#define NMS_LR_BANK_H 640ull
#define NMS_LR_BANK_BYTES (NMS_LR_BANK_W * NMS_LR_BANK_H * 4ull)
static int g_nms_tier = 3;                 /* boot default: Performance (the deployed bin) */
static int g_nms_tier_forced_reset;        /* executes remaining with reset=1 after a swap */
static void* g_nms_wb_map, *g_nms_recp_map, *g_nms_hst_map, *g_nms_disto_map, *g_nms_rpj_map;

static int nms_ratio_tier(float ratio)
{
    if (ratio >= 2.99f) return 4;
    if (ratio >= 1.99f) return 3;
    if (ratio >= 1.69f) return 2;
    if (ratio >= 1.49f) return 1;
    return 0;
}
/* cold history for a fresh net (or boot): the chassis-init fills, factored */
static void nms_seed_history(void)
{
    const unsigned int IN_W = 1280, IN_H = 720;
    unsigned int x, y;
    if (g_nms_recp_map) {
        unsigned int* w = (unsigned int*)g_nms_recp_map;
        for (y = 0; y < IN_H; y++) for (x = 0; x < IN_W; x++) {
            float r = (float)x / (float)IN_W, g = (float)y / (float)IN_H;
            unsigned int o = (y * IN_W + x) * 2u;
            w[o] = nms_f16(r) | (nms_f16(g) << 16);
            w[o + 1u] = nms_f16(0.2f);
        }
    }
    if (g_nms_hst_map) {
        unsigned int* w = (unsigned int*)g_nms_hst_map;
        for (y = 0; y < IN_H; y++) for (x = 0; x < IN_W; x++) {
            float r = (float)x / (float)IN_W, g = (float)y / (float)IN_H;
            float mr = (float)log(1.0 + 150.0 * (double)r) / 8.51788;
            float mg = (float)log(1.0 + 150.0 * (double)g) / 8.51788;
            unsigned int o = (y * IN_W + x) * 2u;
            w[o] = nms_f16(mr) | (nms_f16(mg) << 16);
            w[o + 1u] = nms_f16(0.25f);
        }
    }
    if (g_nms_disto_map) memset(g_nms_disto_map, 0, (size_t)(640ull * 360ull * 4ull));
    if (g_nms_rpj_map)   memset(g_nms_rpj_map, 0, (size_t)(1280ull * 720ull * 8ull));
}
/* the tier swap (render-thread timing, between records; the wb-bank write
 * races at most one in-flight feat dispatch — menu-transition frame class) */
static void nms_apply_tier(int tier)
{
    NmsHeader* h = (NmsHeader*)g_nms_mb;
    unsigned bits;
    if (tier < 0 || tier > 4 || !h || !g_nms_wb_map) return;
    g_nms_tier = tier;
    g_nms_tier_forced_reset = 12;
    bits = 0; memcpy(&bits, &NMS_TIER[tier].rcps, 4);
    h->plan[0].comp_push[10] = bits;         /* feat u_rcps (word 10, 44B block) */
    bits = 0; memcpy(&bits, &NMS_TIER[tier].s_out, 4);
    h->plan[1].comp_push[9] = bits;          /* postA s_out (word 9) */
    if (!nms_read_dll_file(NMS_TIER[tier].pass0, g_nms_wb_map, 264u * 4u))
        L("nms-tier: %s MISSING next to DLL — bank keeps prior weights", NMS_TIER[tier].pass0);
    nms_seed_history();
    MemoryBarrier();
    L("nms-tier: SWAP -> %s (rcps=%.5f s_out=%.9f, history re-seeded)",
      NMS_TIER[tier].name, NMS_TIER[tier].rcps, NMS_TIER[tier].s_out);
}

static int nms_chassis_feat_posta(uint32_t type_idx)
{
    static const unsigned int BINDS[10] = { 1, 2, 3, 5, 9, 10, 12, 15, 16, 17 };
    static const unsigned int PB[6] = { 1, 4, 5, 9, 10, 15 };
    const unsigned int LR_W = 640, LR_H = 360, IN_W = 1280, IN_H = 720;
    unsigned long long bytes[10], pbytes[6];
    VkBuffer bufs[10]; VkDeviceMemory mems[10]; void* maps[10];
    VkBuffer pbufs[6]; VkDeviceMemory pmems[6]; void* pmaps[6];
    VkDescriptorBufferInfo bi[10], pbi[6];
    NmsPipe feat, posta;
    unsigned i, x, y, gw0 = 640, gh0 = 360;
    NmsHeader* h = (NmsHeader*)g_nms_mb;
    memset(bufs, 0, sizeof bufs); memset(mems, 0, sizeof mems); memset(maps, 0, sizeof maps);
    memset(pbufs, 0, sizeof pbufs); memset(pmems, 0, sizeof pmems); memset(pmaps, 0, sizeof pmaps);
    memset(&feat, 0, sizeof feat); memset(&posta, 0, sizeof posta);

    bytes[0] = (unsigned long long)LR_W * LR_H * 4ull * 4ull;  /* b1  outq  (int8 16ch tensor) */
    bytes[1] = 264ull * 4ull;                                  /* b2  wb    (pass0 weights)   */
    bytes[2] = (unsigned long long)IN_W * IN_H * 8ull;         /* b3  recp  fp16 RGBA pairs   */
    bytes[3] = NMS_LR_BANK_BYTES;                              /* b5  rawlr R11G11B10F words  */
    bytes[4] = (unsigned long long)IN_W * IN_H * 8ull;         /* b9  rpj   fp16 pairs        */
    bytes[5] = (unsigned long long)IN_W * IN_H * 8ull;         /* b10 hst   mu-law fp16 pairs */
    bytes[6] = NMS_LR_BANK_BYTES;                              /* b12 mvec  fp16 xy words     */
    bytes[7] = (unsigned long long)LR_W * LR_H * 4ull;         /* b15 disto (bftau-gated)     */
    bytes[8] = NMS_LR_BANK_BYTES;                              /* b16 dpt   D32 words (dilate)*/
    bytes[9] = NMS_LR_BANK_BYTES;                              /* b17 dptp  prev D32 (disoc)  */
    /* rawlr/mvec/dpt/dptp sized for the WIDEST preset (UQ+ 1066x600 + the
     * game's height round-up) — ratio era: the game render size now varies
     * per XeSS preset and wv1 indexes these banks by the per-frame game
     * dims (LR_W/LR_H pushes). The 368-row era sizing lived and died with
     * the single-geometry quality-lock. */
    for (i = 0; i < 10; i++) {
        /* rawlr/mvec/dpt also take TRANSFER_DST: the LAYER records the game's
         * color/velocity/depth copies into them at doorbell time (rev3b).
         * dpt additionally SRC (the layer's dpt->dpt_prev copy) and dptp
         * DST for the same (rev3c.9 disocclusion). */
        VkBufferUsageFlags u = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (i == 3 || i == 6 || i == 9) u |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (i == 8) u |= VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (!nms_chassis_bank(type_idx, bytes[i], u, &bufs[i], &mems[i], &maps[i])) {
            L("nms-chassis: bank b%u failed", BINDS[i]);
            return 0;
        }
    }
    g_nms_hst_buf = bufs[5];    /* b10 shared with postA + RCAS */
    g_nms_recp_buf = bufs[2];   /* b3 recp (fp16 pairs) */
    /* ratio era: stash the maps the tier swap touches */
    g_nms_wb_map = maps[1]; g_nms_recp_map = maps[2]; g_nms_hst_map = maps[5];
    g_nms_disto_map = maps[7]; g_nms_rpj_map = maps[4];
    /* the boot tier follows the game's init quality (AMD's thresholds on the
     * legacy scale factor == the ratio); per-frame detection corrects this
     * within the first executes regardless */
    { float s0 = qscale(g_init.qualitySetting);
      g_nms_tier = nms_ratio_tier(s0);
      gw0 = g_init.outputResolution.x ? ((unsigned)(g_init.outputResolution.x / s0) & ~1u) : 640u;
      gh0 = g_init.outputResolution.y ? ((unsigned)(g_init.outputResolution.y / s0) & ~1u) : 360u; }
    /* fills: weights (REQUIRED, per-boot-tier); synthetic initial state */
    if (!nms_read_dll_file(NMS_TIER[g_nms_tier].pass0, maps[1], 264u * 4u) &&
        !nms_read_dll_file("pass0_wb.raw", maps[1], 264u * 4u)) {
        L("nms-chassis: pass0 (%s + fallback) MISSING next to DLL — zeros (feat output meaningless)",
          NMS_TIER[g_nms_tier].pass0);
        memset(maps[1], 0, 264u * 4u);
    } else L("nms-chassis: weights loaded (264 words, tier=%s)", NMS_TIER[g_nms_tier].name);
    { unsigned int* w = (unsigned int*)maps[3];
      for (y = 0; y < LR_H; y++) for (x = 0; x < LR_W; x++) {
          float r = 0.0625f + 0.75f * (float)x / (float)LR_W;
          float g = 0.0625f + 0.75f * (float)y / (float)LR_H;
          float b = 0.125f + 0.5f * ((float)x / (float)LR_W * (float)y / (float)LR_H);
          w[y * LR_W + x] = (nms_f16(r) >> 4) | ((nms_f16(g) >> 4) << 11) | ((nms_f16(b) >> 5) << 22);
      } }
    nms_seed_history();      /* recp/hst synthetic fills + disto/rpj zeros (factored — the tier swap reuses it) */
    memset(maps[6], 0, (size_t)bytes[6]);
    memset(maps[8], 0, (size_t)bytes[8]);   /* dpt seed zeros = far (inverted) */
    memset(maps[9], 0, (size_t)bytes[9]);   /* dptp seed zeros = far */

    for (i = 0; i < 10; i++) { bi[i].buffer = bufs[i]; bi[i].offset = 0; bi[i].range = (VkDeviceSize)bytes[i]; }
    if (!nms_chassis_pipeline(wv1_spv, WV1_SPV_WORDS, BINDS, 10, 44, bi, &feat)) {
        L("nms-chassis: feat pipeline failed");
        return 0;
    }
    L("nms-chassis: FEAT pipeline ok (fused feat+pre0)");

    /* postA: b1 recout + b4 npu-in are NEW; b5/9/10/15 SHARED with feat */
    pbytes[0] = (unsigned long long)IN_W * IN_H * 4ull;   /* b1 recout u8 words */
    pbytes[1] = (unsigned long long)IN_W * IN_H * 8ull;   /* b4 npu-in 2 words/texel */
    pbytes[2] = bytes[3]; pbytes[3] = bytes[4]; pbytes[4] = bytes[5]; pbytes[5] = bytes[7];
    pbufs[2] = bufs[3]; pbufs[3] = bufs[4]; pbufs[4] = bufs[5]; pbufs[5] = bufs[7];
    pmems[2] = mems[3]; pmems[3] = mems[4]; pmems[4] = mems[5]; pmems[5] = mems[7];
    for (i = 0; i < 2; i++)
        if (!nms_chassis_bank(type_idx, pbytes[i],
                              (i == 1) ? (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)  /* npu-in: also the handoff copy dst (rev3c.6) */
                                       : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              &pbufs[i], &pmems[i], &pmaps[i])) {
            L("nms-chassis: postA bank b%u failed", PB[i]);
            pmaps[1] = NULL;
            break;
        }
    g_nms_recout_buf = pbufs[0];    /* the rec-bridge reads this (u8 recout) */
    if (pmaps[1]) {
        for (i = 0; i < 6; i++) { pbi[i].buffer = pbufs[i]; pbi[i].offset = 0; pbi[i].range = (VkDeviceSize)pbytes[i]; }
        if (nms_chassis_pipeline(wv2a_spv, WV2A_SPV_WORDS, PB, 6, 40, pbi, &posta)) {
            unsigned int push[11]; float f;
            memset(push, 0, sizeof push);
            f = 1.0f; memcpy(&push[4], &f, 4);        /* expo */
            push[5] = IN_W; push[6] = IN_H; push[7] = gw0; push[8] = gh0;   /* GAME dims (ratio era) */
            f = NMS_TIER[g_nms_tier].s_out; memcpy(&push[9], &f, 4);       /* s_out (per-tier dequant) */
            nms_comp_publish(1, posta.pipe, posta.playou, posta.set,
                             (IN_W + 7u) / 8u, (IN_H + 7u) / 8u, push, 40, 3);
            L("nms-chassis: POSTA pipeline ok (temporal loop closer)");
        } else pmaps[1] = NULL;
    }

    { unsigned int push[11]; float f;
      memset(push, 0, sizeof push);
      f = 1.0f; memcpy(&push[3], &f, 4);              /* expo */
      push[5] = IN_W; push[6] = IN_H; push[7] = gw0; push[8] = gh0;   /* GAME dims (ratio era) */
      f = 0.0f; memcpy(&push[9], &f, 4);              /* u_dilate: 0 = legacy center
                                                        * MV read; layer patches this
                                                        * word live from the DILATE
                                                        * knob (AMD dilate_mv port) */
      f = NMS_TIER[g_nms_tier].rcps; memcpy(&push[10], &f, 4);       /* u_rcps (per-tier input grid) */
      nms_comp_publish(0, feat.pipe, feat.playou, feat.set,
                       (LR_W + 7u) / 8u, (LR_H + 7u) / 8u, push, 44, 11);
    }
    /* NPU handshake: outq (worker source) + npu-in (worker dest) */
    h->outq_ptr   = (unsigned long long)(uintptr_t)maps[0];
    h->outq_bytes = bytes[0];
    h->npudst_ptr = pmaps[1] ? (unsigned long long)(uintptr_t)pmaps[1] : 0;
    h->npudst_bytes = pmaps[1] ? pbytes[1] : 0;
    /* rev3c.6b: npudst_buf must follow the CHASSIS bank (postA binding 4) —
     * nms_arm's transport-era pointer survived here, so the layer's
     * queue-ordered handoff copied into a dead bank and postA read ZEROS
     * (neutral weights = plain-gaussian image). */
    if (pmaps[1]) h->npudst_buf = (unsigned long long)(uintptr_t)pbufs[1];
    /* rev3c.7: the 3-SLOT ring + explicit (gen<<8)|slot publish (the
     * campaign's r59g law — counter-derived slots are the "parity bug").
     * Worker3 writes ring[slot], publishes; the layer's snapshot copy reads
     * only the published slot. */
    if (pmaps[1]) {
        VkBuffer sb[3]; VkDeviceMemory sm[3]; void* smap[3]; int k, ok2 = 1;
        for (k = 0; k < 3; k++) {
            smap[k] = NULL;
            if (!nms_chassis_bank(type_idx, pbytes[1], VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  &sb[k], &sm[k], &smap[k])) { ok2 = 0; break; }
        }
        if (ok2) {
            for (k = 0; k < 3; k++) memset(smap[k], 0, (size_t)pbytes[1]);
            h->stage2a_buf = (unsigned long long)(uintptr_t)sb[0];
            h->stage2b_buf = (unsigned long long)(uintptr_t)sb[1];
            h->stage2c_buf = (unsigned long long)(uintptr_t)sb[2];
            h->stage2a_ptr = (unsigned long long)(uintptr_t)smap[0];
            h->stage2b_ptr = (unsigned long long)(uintptr_t)smap[1];
            h->stage2c_ptr = (unsigned long long)(uintptr_t)smap[2];
            h->npu_pub = 0;
            L("nms-chassis: stage2 3-slot ring armed (r59g publish)");
        } else L("nms-chassis: stage2 ring FAILED — direct npu-in write (race risk)");
    }
    /* the layer's input copies retarget the chassis banks */
    h->stage_color_buf = (unsigned long long)(uintptr_t)bufs[3];   /* rawlr */
    h->stage_vel_buf   = (unsigned long long)(uintptr_t)bufs[6];   /* mvec  */
    h->stage_depth_buf = (unsigned long long)(uintptr_t)bufs[8];   /* dpt (b16, AMD dilate_mv 2026-10-11:
                                                                    * the layer's stage-1 i==1 branch copies
                                                                    * the XeSS depth here — was 0/unused) */
    h->stage_color_ptr = (unsigned long long)(uintptr_t)maps[3];
    h->stage_vel_ptr   = (unsigned long long)(uintptr_t)maps[6];
    h->stage_depth_ptr = (unsigned long long)(uintptr_t)maps[8];
    /* 0x2A8/0x2B0 are ZCC3 handshake slots now (the dptp bank handles are
     * no longer published; wv1 binding 17 stays bound to the zero bank). */
    h->zcc3_in_fd = 0; h->zcc3_imported_in_buf = 0;
    /* ZCC3: the outq VkBuffer handoff — the header is FULL (0x2D0), so the
     * free stamp-bank tail carries it: stamp u64[2] = outq VkBuffer,
     * u64[3] = 0x5A4351AF4C554251 magic ("the outq buffer lives here").
     * The stamp oracle only reads u64[0]/[1]; 16..63 were always spare. */
    { unsigned long long* st = (unsigned long long*)g_nms_maps[5];   /* the FIRST-ARM stamp bank (what the layer's stamp_ptr names) — the chassis-local maps[5] is the HIST bank */
      if (st) { st[2] = (unsigned long long)(uintptr_t)bufs[0]; st[3] = 0x5A4351AF4C554251ull;
                /* prev-disocclusion handoff (stamp u64[4]/[5], was free):
                 * the dptp bank handle (binding 17, zero-seeded until the
                 * layer's PDISOC-gated copy fills it with prev depth) */
                st[4] = (unsigned long long)(uintptr_t)bufs[9]; st[5] = 0x44505450434F5349ull;   /* "DPTPCOSI" */
                MemoryBarrier(); } }
    h->stage_bytes     = bytes[3];
    MemoryBarrier();
    g_diag_outq  = (signed char*)maps[0];
    g_diag_npuin = (signed char*)pmaps[1];
    g_diag_rawlr = (unsigned int*)maps[3];
    g_diag_mvec  = (unsigned int*)maps[6];
    g_diag_dpt   = (unsigned int*)maps[8];
    g_diag_hist  = (unsigned short*)maps[5];
    g_diag_recp  = (unsigned short*)maps[2];
    h->nms_flags |= 4ull;                            /* chassis armed */
    MemoryBarrier();
    L("nms-chassis: FEAT+POSTA armed outq=%p npuin=%p (%s)",
      maps[0], pmaps[1], pmaps[1] ? "T_fsr4 READY" : "feat-only");
    return 1;
}

/* RCAS stage-2 (wv2b) at OS=1280x720, reading the SHARED hst bank; the
 * out16 fp16 route bank keeps the r58s SLACK allocation. Publishes plan 2
 * + route (bank + probe image fmt 97). */
static int nms_chassis_rcas(uint32_t type_idx)
{
    const unsigned int OS_W = 1280, OS_H = 720;
    static const unsigned int B[3] = { 10, 11, 14 };
    unsigned long long bytes[3];
    VkBuffer bufs[3]; VkDeviceMemory mems[3]; void* maps[3];
    VkDescriptorBufferInfo bi[3];
    NmsPipe rp;
    unsigned i;
    NmsHeader* h = (NmsHeader*)g_nms_mb;
    memset(bufs, 0, sizeof bufs); memset(mems, 0, sizeof mems); memset(maps, 0, sizeof maps);

    /* binding 10 = the SHARED hst bank from the feat chassis */
    if (!g_nms_hst_buf) { L("nms-chassis: rcas: no shared hst bank — skip"); return 0; }

    bytes[0] = (unsigned long long)OS_W * OS_H * 8ull;   /* b10 hst (shared) */
    bytes[1] = (unsigned long long)OS_W * OS_H * 4ull;   /* b11 RGBA8 out    */
    bytes[2] = 1920ull * 1080ull * 8ull;                 /* b14 fp16 out16 — r58s SLACK law */
    bufs[0] = g_nms_hst_buf;
    for (i = 1; i < 3; i++)
        if (!nms_chassis_bank(type_idx, bytes[i],
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              &bufs[i], &mems[i], &maps[i])) {
            L("nms-chassis: rcas bank %u failed", B[i]);
            return 0;
        }
    memset(maps[1], 0, (size_t)bytes[1]);
    memset(maps[2], 0, (size_t)1920ull * 1080ull * 8ull);   /* whole slack alloc */
    for (i = 0; i < 3; i++) { bi[i].buffer = bufs[i]; bi[i].offset = 0; bi[i].range = (VkDeviceSize)((i < 2) ? bytes[i] : (unsigned long long)OS_W * OS_H * 8ull); }
    if (!nms_chassis_pipeline(wv2b_spv, WV2B_SPV_WORDS, B, 3, 44, bi, &rp)) {
        L("nms-chassis: rcas pipeline failed");
        return 0;
    }
    { unsigned int push[11]; float f;
      memset(push, 0, sizeof push);
      f = 1.0f; memcpy(&push[4], &f, 4);              /* expo */
      push[5] = OS_W; push[6] = OS_H; push[7] = 640; push[8] = 360;
      f = 0.5f; memcpy(&push[9], &f, 4);              /* sharp */
      f = 1.0f; memcpy(&push[10], &f, 4);             /* out16=1 -> binding 14 */
      nms_comp_publish(2, rp.pipe, rp.playou, rp.set, (OS_W + 7u) / 8u, (OS_H + 7u) / 8u, push, 44, 7);
    }
    /* the probe image (fmt 97 fp16) + route publication */
    { typedef VkResult (VKAPI_PTR *PFN_xci)(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage*);
      typedef void     (VKAPI_PTR *PFN_xir)(VkDevice, VkImage, VkMemoryRequirements*);
      typedef VkResult (VKAPI_PTR *PFN_xam)(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*);
      typedef VkResult (VKAPI_PTR *PFN_xib)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
      PFN_xci fci = (PFN_xci)nms_gipa("vkCreateImage");
      PFN_xir fir = (PFN_xir)nms_gipa("vkGetImageMemoryRequirements");
      PFN_xam fam = (PFN_xam)nms_gipa("vkAllocateMemory");
      PFN_xib fib = (PFN_xib)nms_gipa("vkBindImageMemory");
      if (fci && fir && fam && fib) {
          VkImageCreateInfo ii; VkMemoryRequirements mr; VkMemoryAllocateInfo ai;
          VkImage img = VK_NULL_HANDLE; VkDeviceMemory imgmem;
          memset(&ii, 0, sizeof ii);
          ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
          ii.imageType = VK_IMAGE_TYPE_2D; ii.format = (VkFormat)97;   /* R16G16B16A16_SFLOAT */
          ii.extent.width = OS_W; ii.extent.height = OS_H; ii.extent.depth = 1;
          ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
          ii.tiling = VK_IMAGE_TILING_OPTIMAL;
          ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
          ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          if (fci(g_vk_dev, &ii, NULL, &img) == VK_SUCCESS && img) {
              fir(g_vk_dev, img, &mr);
              memset(&ai, 0, sizeof ai);
              ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
              ai.allocationSize = mr.size; ai.memoryTypeIndex = type_idx;
              if (fam(g_vk_dev, &ai, NULL, &imgmem) == VK_SUCCESS && fib(g_vk_dev, img, imgmem, 0) == VK_SUCCESS) {
                  h->route_buf = (unsigned long long)(uintptr_t)bufs[2];
                  h->route_img = (unsigned long long)(uintptr_t)img;

                  MemoryBarrier();
                  g_diag_route = (unsigned int*)maps[2];
                  { static HANDLE dt; if (!dt) dt = CreateThread(NULL, 0, nms_diag_thread, NULL, 0, NULL); }
                  L("nms-chassis: RCAS+ROUTE armed buf=%p img=%p (%ux%u)",
                    (void*)bufs[2], (void*)img, OS_W, OS_H);
                  return 1;
              }
          }
      }
      L("nms-chassis: probe image failed — RCAS armed without route (blit stays)");
    }
    return 1;
}



/* rev3c: the REC BRIDGE — exp8to16 (u8 recout -> fp16-pair recp), plan[3].
 * The R59 pipeline's missing-in-port stage: postA writes rec history as u8;
 * wv1's frozen binding 3 reads fp16 pairs. Dispatched right after postA. */
static int nms_chassis_rec_bridge(uint32_t type_idx)
{
    static const unsigned int B[2] = { 0, 1 };
    VkDescriptorBufferInfo bi[2];
    NmsPipe ep;
    const unsigned int OS_W = 1280, OS_H = 720;
    (void)type_idx;
    if (!g_nms_recout_buf || !g_nms_recp_buf) { L("nms-bridge: banks missing"); return 0; }
    bi[0].buffer = g_nms_recout_buf; bi[0].offset = 0; bi[0].range = VK_WHOLE_SIZE;
    bi[1].buffer = g_nms_recp_buf;   bi[1].offset = 0; bi[1].range = VK_WHOLE_SIZE;
    if (!nms_chassis_pipeline(exp8to16_spv, EXP8TO16_SPV_WORDS, B, 2, 0, bi, &ep)) {
        /* push_bytes=0 invalid — use 4-byte dummy range if rejected */
        L("nms-bridge: pipeline failed");
        return 0;
    }
    { unsigned int push0[1] = { 0 };
      nms_comp_publish(3, ep.pipe, ep.playou, ep.set, (OS_W * OS_H + 63u) / 64u, 1, push0, 0, 0); }
    return 1;
}
/* per-execute: seqlock publish + 24B doorbell + ack check. Runs on the
 * game's render thread inside xessVKExecute — recording only, no creates. */
static void nms_execute(VkCommandBuffer cb, const xess_vk_execute_params_t* p)
{
    NmsHeader* h = (NmsHeader*)g_nms_mb;
    NmsSlot* s = NULL;
    unsigned long long db[3], ack;
    unsigned long i;
    if (!s_nms_upd) s_nms_upd = (void (VKAPI_PTR*)(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, const void*))
        nms_gipa("vkCmdUpdateBuffer");
    if (!s_nms_upd) return;
    for (i = 0; i < NMS_MB_SLOTS; i++) {
        NmsSlot* c = (NmsSlot*)((char*)g_nms_mb + NMS_MB_HEADER + (unsigned long long)i * NMS_MB_SLOT);
        if (c->state == 0 && (c->seq & 1ull) == 0ull) { s = c; break; }
    }
    if (!s) { if ((g_nms_frames & 1023ull) == 0ull) L("nms: no free slot!"); return; }
    g_nms_seq += 1ull; g_nms_frames += 1ull;
    s->seq = g_nms_seq * 2ull + 1ull; MemoryBarrier();
    memset(&s->f, 0, sizeof s->f);
    s->f.frame_seq = g_nms_frames; s->f.publish_seq = g_nms_seq;
    s->f.cmdlist = (unsigned long long)(uintptr_t)cb;
    s->f.device  = (unsigned long long)(uintptr_t)g_vk_dev;
    s->f.jitter_x = p->jitterOffsetX; s->f.jitter_y = p->jitterOffsetY;
    s->f.mv_scale_x = g_vel_x; s->f.mv_scale_y = g_vel_y;
    s->f.sharpness = 0.5f;
    s->f.width = p->inputWidth; s->f.height = p->inputHeight;
    s->f.out_width = p->outputTexture.width; s->f.out_height = p->outputTexture.height;
    s->f.reset = p->resetHistory;
    s->f.res[0] = (NmsRes){ (unsigned long long)(uintptr_t)p->colorTexture.imageView, (unsigned long long)(uintptr_t)p->colorTexture.image,
                            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                            (unsigned)p->colorTexture.format, p->colorTexture.width, p->colorTexture.height, 0, 0,
                            (p->colorTexture.image != 0), 0 };
    s->f.res[1] = (NmsRes){ (unsigned long long)(uintptr_t)p->depthTexture.imageView, (unsigned long long)(uintptr_t)p->depthTexture.image,
                            VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1,
                            (unsigned)p->depthTexture.format, p->depthTexture.width, p->depthTexture.height, 0, 0,
                            (p->depthTexture.image != 0), 0 };
    s->f.res[2] = (NmsRes){ (unsigned long long)(uintptr_t)p->velocityTexture.imageView, (unsigned long long)(uintptr_t)p->velocityTexture.image,
                            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                            (unsigned)p->velocityTexture.format, p->velocityTexture.width, p->velocityTexture.height, 0, 0,
                            (p->velocityTexture.image != 0), 0 };
    s->f.res[3] = (NmsRes){ (unsigned long long)(uintptr_t)p->outputTexture.imageView, (unsigned long long)(uintptr_t)p->outputTexture.image,
                            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                            (unsigned)p->outputTexture.format, p->outputTexture.width, p->outputTexture.height, 0, 0,
                            (p->outputTexture.image != 0), 0 };
    /* ---- RATIO ERA: per-frame tier + game-geometry publication (BEFORE the
     * slot publish — the seqlock reader must see these with the frame) ---- */
    if (p->inputWidth && p->outputTexture.width) {
        /* tier selection: the PRESET's NOMINAL ratio when the game re-inited
         * with a known quality (it re-inits on every preset change —
         * observed), else the actual width ratio. Why nominal: the game
         * rounds renders to its own granularity (UP asks 426, renders
         * 432x240 -> width ratio 2.963 misses AMD's >=2.99 ultraperf cutoff
         * and ran PERF weights outside their 2x training domain — the
         * "edges and image stability are bad" verdict, 2026-10-11). Nominal
         * == AMD's CreateContext selection; actual stays the fallback. */
        float ratio = (float)p->outputTexture.width / (float)p->inputWidth;
        int tier = (g_init.qualitySetting >= 100 && g_init.qualitySetting <= 106)
                 ? nms_ratio_tier(qscale(g_init.qualitySetting))
                 : nms_ratio_tier(ratio);
        static int pend = 3, pend_n = 0;
        /* debounce 3 same-tier frames: preset transitions can execute at
         * transient geometries mid-menu */
        if (tier != g_nms_tier) {
            if (tier == pend) { if (++pend_n >= 3) { nms_apply_tier(tier); pend_n = 0; } }
            else pend = tier, pend_n = 1;
        } else pend_n = 0, pend = tier;
        /* live game dims into the feat/postA plan words 7/8 (word-sized
         * stores; the layer's memcpy picks up the current pair per frame) */
        h->plan[0].comp_push[7] = p->inputWidth;
        h->plan[0].comp_push[8] = p->inputHeight;
        h->plan[1].comp_push[7] = p->inputWidth;
        h->plan[1].comp_push[8] = p->inputHeight;
        /* the layer's bin swap rides this word (worker hot-swap) */
        s->f.reserved_u[0] = (unsigned)(g_nms_tier + 1);
        /* cold-history frames after a swap: wv1 rebuilds from current */
        if (g_nms_tier_forced_reset > 0) { g_nms_tier_forced_reset--; s->f.reset = 1; }
        if (g_nms_frames <= 3)
            L("nms-ratio: in=%ux%u ratio=%.4f tier=%s", p->inputWidth, p->inputHeight,
              ratio, NMS_TIER[g_nms_tier].name);
    }
    s->state = 1; MemoryBarrier();
    s->seq = g_nms_seq * 2ull + 2ull; MemoryBarrier();
    /* keep the worker's geometry snapshot current (it clamps to the graph) */
    h->frame_w = p->inputWidth; h->frame_h = p->inputHeight;

    db[0] = NMS_DB_MAGIC; db[1] = g_nms_seq; db[2] = (unsigned long long)(uintptr_t)g_nms_mb;
    s_nms_upd(cb, g_nms_marker, 0, 24, db);

    ack = h->ack;
    if (ack == (g_nms_seq | NMS_ACK_WORK)) g_nms_ack_ok++;
    else if (ack == g_nms_seq) g_nms_ack_nowork++;
    else g_nms_ack_bad++;
    if (g_nms_frames <= 3 || (g_nms_frames % 500ull) == 0ull)
        L("nms: frame %llu doorbell seq=%llu ack=0x%llx (ok=%llu nowork=%llu bad=%llu npu_done=%llu)",
          g_nms_frames, g_nms_seq, ack, g_nms_ack_ok, g_nms_ack_nowork, g_nms_ack_bad, h->npu_done);
}

/* ===================== wrapped exports ===================== */

xess_result_t __cdecl xessGetVersion(xess_version_t* v)
{
    if (v) { v->major = 1; v->minor = 3; v->patch = 1; }
    L("xessGetVersion -> 0 {1.3.1 (seat)}");
    return XESS_OK;
}

xess_result_t __cdecl xessIsOptimalDriver(xess_context_handle_t h)
{
    L("xessIsOptimalDriver h=0x%llx -> 0 (seat: no)", (unsigned long long)h);
    return 0; /* false — forces the portable DP4a-style path expectations */
}

xess_result_t __cdecl xessGetIntelXeFXVersion(xess_context_handle_t h, xess_version_t* v)
{
    if (v) { v->major = 0; v->minor = 0; v->patch = 0; }
    L("xessGetIntelXeFXVersion h=0x%llx -> 0 {0.0.0 (non-Intel, seat)}", (unsigned long long)h);
    return XESS_OK;
}

xess_result_t __cdecl xessVKCreateContext(VkInstance i, VkPhysicalDevice pd, VkDevice d, xess_context_handle_t* h)
{
    /* THE SEAM: accept every device. The real DLL's vendor whitelist rejected
     * Adreno (-1000, observed); the game surfaced that as an adapter error
     * and hid the XeSS row. With our acceptance the row returns and every
     * later call lands here. */
    g_vk_inst = i; g_vk_pd = pd; g_vk_dev = d; g_ctx_live = 1;
    if (h) *h = XESS_SEAT_CTX;
    L("xessVKCreateContext inst=%p pd=%p dev=%p -> 0 (SEAT CTX 0x%llx) [real DLL said -1000 on Adreno]",
      i, pd, d, (unsigned long long)XESS_SEAT_CTX);
    return XESS_OK;
}

xess_result_t __cdecl xessVKGetRequiredInstanceExtensions(uint32_t* c, const char* const** e, uint32_t* v)
{
    if (c) *c = 0;
    if (e) *e = NULL;
    if (v) *v = (1u << 22) | (3u << 12); /* VK_API_VERSION_1_3 */
    L("xessVKGetRequiredInstanceExtensions -> 0 (seat: none, api 1.3)");
    return XESS_OK;
}

xess_result_t __cdecl xessVKGetRequiredDeviceExtensions(VkInstance i, VkPhysicalDevice pd, uint32_t* c, const char* const** e)
{
    if (c) *c = 0;
    if (e) *e = NULL;
    L("xessVKGetRequiredDeviceExtensions inst=%p pd=%p -> 0 (seat: none)", i, pd);
    return XESS_OK;
}

xess_result_t __cdecl xessVKGetRequiredDeviceFeatures(VkInstance i, VkPhysicalDevice pd, void** feats)
{
    static uint64_t zero[32];
    if (feats) *feats = (void*)zero;
    L("xessVKGetRequiredDeviceFeatures inst=%p pd=%p -> 0 (seat: zero blob)", i, pd);
    return XESS_OK;
}

xess_result_t __cdecl xessVKBuildPipelines(xess_context_handle_t h, VkPipelineCache pc, int blocking, uint32_t flags)
{
    L("xessVKBuildPipelines h=0x%llx pc=%p blocking=%d flags=0x%x -> 0 (seat: no-op)",
      (unsigned long long)h, pc, blocking, flags);
    return XESS_OK;
}

xess_result_t __cdecl xessVKGetInitParams(xess_context_handle_t h, xess_vk_init_params_t* p)
{
    if (h == XESS_SEAT_CTX) {
        if (p) *p = g_init;
        L("xessVKGetInitParams -> 0 (seat echo: out=%ux%u q=%d flags=0x%x)",
          g_init.outputResolution.x, g_init.outputResolution.y, g_init.qualitySetting, g_init.initFlags);
        return XESS_OK;
    }
    typedef xess_result_t (__cdecl fn)(xess_context_handle_t, xess_vk_init_params_t*);
    fn* f = (fn*)real_fn("xessVKGetInitParams");
    return f ? f(h, p) : XESS_ERR_UNSUPPORTED_DEVICE;
}

xess_result_t __cdecl xessVKInit(xess_context_handle_t h, const xess_vk_init_params_t* p)
{
    if (h == XESS_SEAT_CTX) {
        if (p) {
            g_init = *p;
            L("xessVKInit SEAT out=%ux%u quality=%d flags=0x%x nodeMasks=%u/%u bufs=%p+%llu tex=%p+%llu pc=%p -> 0",
              p->outputResolution.x, p->outputResolution.y, p->qualitySetting, p->initFlags,
              p->creationNodeMask, p->visibleNodeMask, p->tempBufferHeap,
              (unsigned long long)p->bufferHeapOffset, p->tempTextureHeap,
              (unsigned long long)p->textureHeapOffset, p->pipelineCache);
            /* rev3: arm the NMS-1 carrier ONCE, at the contract-safe moment
             * (the real XeSS creates its resources at Init too). DEFAULT ON:
             * this DLL is installed for exactly one purpose — FSR4NPU_NPU=0
             * is the opt-OUT kill switch; no env is required to opt in. */
            if (!g_nms_want) {
                char e[8]; DWORD n = GetEnvironmentVariableA("FSR4NPU_NPU", e, sizeof e);
                g_nms_want = !(n >= 1 && e[0] == '0');
                L("nms-want=%d (default on; FSR4NPU_NPU=0 opts out)", g_nms_want);
            }
            if (g_nms_want && !g_nms_armed && g_ctx_live) nms_arm(p);
        }
        return XESS_OK;
    }
    typedef xess_result_t (__cdecl fn)(xess_context_handle_t, const xess_vk_init_params_t*);
    fn* f = (fn*)real_fn("xessVKInit");
    return f ? f(h, p) : XESS_ERR_UNSUPPORTED_DEVICE;
}

xess_result_t __cdecl xessVKExecute(xess_context_handle_t h, VkCommandBuffer cb, const xess_vk_execute_params_t* p)
{
    if (h == XESS_SEAT_CTX) {
        g_execs++;
        if (g_execs <= 10 || (g_execs % 500) == 0) {
            L("xessVKExecute #%llu cmd=%p", g_execs, cb);
            if (p) {
                img_log("color",    &p->colorTexture);
                img_log("velocity", &p->velocityTexture);
                img_log("depth",    &p->depthTexture);
                img_log("exposure", &p->exposureScaleTexture);
                img_log("mask",     &p->responsivePixelMaskTexture);
                img_log("OUTPUT",   &p->outputTexture);
                L("    jitter=(%f,%f) expo=%f reset=%u in=%ux%u",
                  p->jitterOffsetX, p->jitterOffsetY, p->exposureScale,
                  p->resetHistory, p->inputWidth, p->inputHeight);
            }
        }
        /* SEAT rev3 — NMS-1 DOORBELL: when armed, hand the frame to the
         * layer's record-time carrier (staging copies + NPU output route /
         * layer-side blit fallback) and be done — the seat's OWN blit below
         * must NOT also run (double output writes). */
        if (g_nms_armed && cb && p && p->colorTexture.image && p->outputTexture.image) {
            nms_execute(cb, p);
            return XESS_OK;
        }
        /* SEAT rev2 — BLIT CARRIER: record a scaled copy color->output into
         * the command buffer the game itself gave us (the game's queue
         * submits + presents it). Same source/dest format observed (122 =
         * B10G11R11_UFLOAT_PACK32), so no conversion; src stays in its
         * documented SHADER_READ_ONLY layout, output transitions
         * GENERAL->TRANSFER_DST->GENERAL around the blit. Visual-proof
         * stage: the image shown is the raw low-res render upscaled by the
         * hardware sampler. The NPU graph replaces this blit at this exact
         * slot. Real Vulkan types via vulkan_core.h — no hand-rolled ABI. */
        if (cb && p && p->colorTexture.image && p->outputTexture.image
            && p->colorTexture.format == p->outputTexture.format
            && p->inputWidth && p->inputHeight
            && p->outputTexture.width && p->outputTexture.height) {
            typedef void* (VKAPI_PTR *PFN_gipa)(VkInstance, const char*);
            static PFN_gipa s_gipa;
            static void (VKAPI_PTR *s_barrier)(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
                                               VkDependencyFlags, uint32_t, const VkMemoryBarrier*,
                                               uint32_t, const VkBufferMemoryBarrier*,
                                               uint32_t, const VkImageMemoryBarrier*);
            static void (VKAPI_PTR *s_blit)(VkCommandBuffer, VkImage, VkImageLayout,
                                            VkImage, VkImageLayout, uint32_t,
                                            const VkImageBlit*, VkFilter);
            if (!s_gipa && g_vk_inst) {
                HMODULE v = GetModuleHandleA("vulkan-1.dll");
                if (v) s_gipa = (PFN_gipa)(void*)GetProcAddress(v, "vkGetInstanceProcAddr");
            }
            if (s_gipa && g_vk_inst) {
                if (!s_barrier) s_barrier = (void (VKAPI_PTR*)(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
                                                               VkDependencyFlags, uint32_t, const VkMemoryBarrier*,
                                                               uint32_t, const VkBufferMemoryBarrier*,
                                                               uint32_t, const VkImageMemoryBarrier*))
                    (void*)s_gipa((VkInstance)g_vk_inst, "vkCmdPipelineBarrier");
                if (!s_blit) s_blit = (void (VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout,
                                                         VkImage, VkImageLayout, uint32_t,
                                                         const VkImageBlit*, VkFilter))
                    (void*)s_gipa((VkInstance)g_vk_inst, "vkCmdBlitImage");
            }
            if (s_barrier && s_blit) {
                /* input acquire: wait for the game's color render to land
                 * before the blit reads it (rev2a — torn reads showed as
                 * texture flicker; Intel's real XeSS does this internally) */
                VkImageSubresourceRange inRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                VkImageMemoryBarrier acq;
                memset(&acq, 0, sizeof acq);
                acq.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                acq.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                acq.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                acq.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                acq.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                acq.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                acq.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                acq.image = (VkImage)p->colorTexture.image;
                acq.subresourceRange = inRange;

                VkImageSubresourceRange outRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                VkImageMemoryBarrier toDst;
                memset(&toDst, 0, sizeof toDst);
                toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toDst.srcAccessMask = 0;
                toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toDst.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toDst.image = (VkImage)p->outputTexture.image;
                toDst.subresourceRange = outRange;
                VkImageMemoryBarrier toGen = toDst;
                toGen.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toGen.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;

                VkImageBlit bi;
                memset(&bi, 0, sizeof bi);
                bi.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                bi.srcSubresource.layerCount = 1;
                bi.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                bi.dstSubresource.layerCount = 1;
                bi.srcOffsets[1].x = (int32_t)p->inputWidth;
                bi.srcOffsets[1].y = (int32_t)p->inputHeight;
                bi.srcOffsets[1].z = 1;
                bi.dstOffsets[1].x = (int32_t)p->outputTexture.width;
                bi.dstOffsets[1].y = (int32_t)p->outputTexture.height;
                bi.dstOffsets[1].z = 1;

                s_barrier((VkCommandBuffer)cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &acq);
                s_barrier((VkCommandBuffer)cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &toDst);
                s_blit((VkCommandBuffer)cb, (VkImage)p->colorTexture.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       (VkImage)p->outputTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &bi, VK_FILTER_LINEAR);
                s_barrier((VkCommandBuffer)cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          0, 0, NULL, 0, NULL, 1, &toGen);
                if (g_execs <= 3) L("    blit carrier: %ux%u -> %ux%u recorded", p->inputWidth, p->inputHeight,
                                    p->outputTexture.width, p->outputTexture.height);
            }
        }
        return XESS_OK;
    }
    typedef xess_result_t (__cdecl fn)(xess_context_handle_t, VkCommandBuffer, const xess_vk_execute_params_t*);
    fn* f = (fn*)real_fn("xessVKExecute");
    return f ? f(h, cb, p) : XESS_ERR_UNSUPPORTED_DEVICE;
}

xess_result_t __cdecl xessGetProperties(xess_context_handle_t h, const xess_2d_t* out, xess_properties_t* pr)
{
    if (h == XESS_SEAT_CTX) {
        if (pr) { pr->requiredDescriptorCount = 64; pr->tempBufferHeapSize = 0; pr->tempTextureHeapSize = 0; }
        L("xessGetProperties SEAT out=%ux%u -> 0 {desc=64, heaps=0}",
          out ? out->x : 0, out ? out->y : 0);
        return XESS_OK;
    }
    typedef xess_result_t (__cdecl fn)(xess_context_handle_t, const xess_2d_t*, xess_properties_t*);
    fn* f = (fn*)real_fn("xessGetProperties");
    return f ? f(h, out, pr) : XESS_ERR_UNSUPPORTED_DEVICE;
}

static void fill_res(const xess_2d_t* o, int32_t q, xess_2d_t* in)
{
    /* RATIO ERA 2026-10-11: the GRAPH LOCK is GONE. The NPU graph stays
     * 640x360 for EVERY preset (AMD's architecture: the model always runs
     * at HR/2 -> HR); what changes per preset is the game's render size
     * (wv1's runtime inv_scale) and the tier WEIGHTS (seat hot-swaps
     * pass0/rcps/s_out; the layer swaps the context bin). The 2026-10-10
     * freeze was the fixed-geometry pipeline meeting 752x432 — the
     * generalized wv1/wv2a + tier table make every geometry live. */
    float s = qscale(q);
    in->x = (uint32_t)(o->x / s) & ~1u;
    in->y = (uint32_t)(o->y / s) & ~1u;
}

xess_result_t __cdecl xessGetInputResolution(xess_context_handle_t h, const xess_2d_t* o, int32_t q, xess_2d_t* in)
{
    if (h == XESS_SEAT_CTX) {
        fill_res(o, q, in);
        L("xessGetInputResolution SEAT out=%ux%u q=%d -> in=%ux%u",
          o ? o->x : 0, o ? o->y : 0, q, in ? in->x : 0, in ? in->y : 0);
        return XESS_OK;
    }
    typedef xess_result_t (__cdecl fn)(xess_context_handle_t, const xess_2d_t*, int32_t, xess_2d_t*);
    fn* f = (fn*)real_fn("xessGetInputResolution");
    return f ? f(h, o, q, in) : XESS_ERR_UNSUPPORTED_DEVICE;
}

xess_result_t __cdecl xessGetOptimalInputResolution(xess_context_handle_t h, const xess_2d_t* o, int32_t q,
                                                    xess_2d_t* opt, xess_2d_t* mn, xess_2d_t* mx)
{
    if (h == XESS_SEAT_CTX) {
        fill_res(o, q, opt);
        if (mn) { mn->x = (o ? o->x : 1280) / 4; mn->y = (o ? o->y : 720) / 4; }
        if (mx) *mx = *o;
        L("xessGetOptimalInputResolution SEAT out=%ux%u q=%d -> opt=%ux%u",
          o ? o->x : 0, o ? o->y : 0, q, opt ? opt->x : 0, opt ? opt->y : 0);
        return XESS_OK;
    }
    typedef xess_result_t (__cdecl fn)(xess_context_handle_t, const xess_2d_t*, int32_t, xess_2d_t*, xess_2d_t*, xess_2d_t*);
    fn* f = (fn*)real_fn("xessGetOptimalInputResolution");
    return f ? f(h, o, q, opt, mn, mx) : XESS_ERR_UNSUPPORTED_DEVICE;
}

xess_result_t __cdecl xessSelectNetworkModel(xess_context_handle_t h, xess_network_model_t m)
{
    L("xessSelectNetworkModel h=0x%llx model=%d -> 0 (seat: stored)", (unsigned long long)h, m);
    return XESS_OK;
}

/* ---- context-state getters/setters: seat-served for our handle ---- */
xess_result_t __cdecl xessSetJitterScale(xess_context_handle_t h, float x, float y)
{ if (h == XESS_SEAT_CTX) { g_jitter_x = x; g_jitter_y = y; L("xessSetJitterScale(%f,%f) -> 0", x, y); return XESS_OK; }
  typedef xess_result_t (__cdecl fn)(xess_context_handle_t, float, float); fn* f = (fn*)real_fn("xessSetJitterScale"); return f ? f(h, x, y) : XESS_ERR_UNSUPPORTED_DEVICE; }

xess_result_t __cdecl xessGetJitterScale(xess_context_handle_t h, float* x, float* y)
{ if (h == XESS_SEAT_CTX) { if (x) *x = g_jitter_x; if (y) *y = g_jitter_y; L("xessGetJitterScale -> 0 (%f,%f)", g_jitter_x, g_jitter_y); return XESS_OK; }
  typedef xess_result_t (__cdecl fn)(xess_context_handle_t, float*, float*); fn* f = (fn*)real_fn("xessGetJitterScale"); return f ? f(h, x, y) : XESS_ERR_UNSUPPORTED_DEVICE; }

xess_result_t __cdecl xessSetVelocityScale(xess_context_handle_t h, float x, float y)
{ if (h == XESS_SEAT_CTX) { g_vel_x = x; g_vel_y = y; L("xessSetVelocityScale(%f,%f) -> 0", x, y); return XESS_OK; }
  typedef xess_result_t (__cdecl fn)(xess_context_handle_t, float, float); fn* f = (fn*)real_fn("xessSetVelocityScale"); return f ? f(h, x, y) : XESS_ERR_UNSUPPORTED_DEVICE; }

xess_result_t __cdecl xessGetVelocityScale(xess_context_handle_t h, float* x, float* y)
{ if (h == XESS_SEAT_CTX) { if (x) *x = g_vel_x; if (y) *y = g_vel_y; L("xessGetVelocityScale -> 0 (%f,%f)", g_vel_x, g_vel_y); return XESS_OK; }
  typedef xess_result_t (__cdecl fn)(xess_context_handle_t, float*, float*); fn* f = (fn*)real_fn("xessGetVelocityScale"); return f ? f(h, x, y) : XESS_ERR_UNSUPPORTED_DEVICE; }

xess_result_t __cdecl xessSetExposureMultiplier(xess_context_handle_t h, float s)
{ if (h == XESS_SEAT_CTX) { g_exposure = s; L("xessSetExposureMultiplier(%f) -> 0", s); return XESS_OK; }
  typedef xess_result_t (__cdecl fn)(xess_context_handle_t, float); fn* f = (fn*)real_fn("xessSetExposureMultiplier"); return f ? f(h, s) : XESS_ERR_UNSUPPORTED_DEVICE; }

xess_result_t __cdecl xessGetExposureMultiplier(xess_context_handle_t h, float* s)
{ if (h == XESS_SEAT_CTX) { if (s) *s = g_exposure; L("xessGetExposureMultiplier -> 0 (%f)", g_exposure); return XESS_OK; }
  typedef xess_result_t (__cdecl fn)(xess_context_handle_t, float*); fn* f = (fn*)real_fn("xessGetExposureMultiplier"); return f ? f(h, s) : XESS_ERR_UNSUPPORTED_DEVICE; }

xess_result_t __cdecl xessSetMaxResponsiveMaskValue(xess_context_handle_t h, float v)
{ if (h == XESS_SEAT_CTX) { g_mask_max = v; L("xessSetMaxResponsiveMaskValue(%f) -> 0", v); return XESS_OK; }
  typedef xess_result_t (__cdecl fn)(xess_context_handle_t, float); fn* f = (fn*)real_fn("xessSetMaxResponsiveMaskValue"); return f ? f(h, v) : XESS_ERR_UNSUPPORTED_DEVICE; }

xess_result_t __cdecl xessGetMaxResponsiveMaskValue(xess_context_handle_t h, float* v)
{ if (h == XESS_SEAT_CTX) { if (v) *v = g_mask_max; L("xessGetMaxResponsiveMaskValue -> 0 (%f)", g_mask_max); return XESS_OK; }
  typedef xess_result_t (__cdecl fn)(xess_context_handle_t, float*); fn* f = (fn*)real_fn("xessGetMaxResponsiveMaskValue"); return f ? f(h, v) : XESS_ERR_UNSUPPORTED_DEVICE; }

xess_result_t __cdecl xessSetLoggingCallback(xess_context_handle_t h, int lvl, void* cb)
{ L("xessSetLoggingCallback h=0x%llx lvl=%d cb=%p -> 0 (seat: ignored)", (unsigned long long)h, lvl, cb);
  return XESS_OK; }

xess_result_t __cdecl xessForceLegacyScaleFactors(xess_context_handle_t h, int force)
{ L("xessForceLegacyScaleFactors h=0x%llx force=%d -> 0 (seat: already legacy)", (unsigned long long)h, force);
  return XESS_OK; }

xess_result_t __cdecl xessGetPipelineBuildStatus(xess_context_handle_t h)
{
    if (h == XESS_SEAT_CTX) { L("xessGetPipelineBuildStatus -> 0 (done)"); return XESS_OK; }
    typedef xess_result_t (__cdecl fn)(xess_context_handle_t); fn* f = (fn*)real_fn("xessGetPipelineBuildStatus");
    return f ? f(h) : XESS_ERR_UNSUPPORTED_DEVICE;
}

xess_result_t __cdecl xessDestroyContext(xess_context_handle_t h)
{
    if (h == XESS_SEAT_CTX) { g_ctx_live = 0; L("xessDestroyContext SEAT (execs=%llu) -> 0", g_execs); return XESS_OK; }
    typedef xess_result_t (__cdecl fn)(xess_context_handle_t); fn* f = (fn*)real_fn("xessDestroyContext");
    return f ? f(h) : XESS_ERR_UNSUPPORTED_DEVICE;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID unused)
{
    (void)unused;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls((HMODULE)h);
        InitializeCriticalSection(&g_cs);
        char path[MAX_PATH + 32];
        GetModuleFileNameA((HMODULE)h, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) strcpy(slash + 1, "xess_proxy.log"); else strcpy(path, "xess_proxy.log");
        g_log = fopen(path, "a");
        g_real = LoadLibraryA("libxess_real.dll");
        L("xess SEAT attached self=%p real=%p — takeover build (CreateContext accepts all devices)",
          (void*)h, (void*)g_real);
    } else if (reason == DLL_PROCESS_DETACH) {
        L("xess SEAT detach (execs=%llu)", g_execs);
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
