// seam_nms.c — NMS-1 (XeSS-slot) doorbell path + NPU worker glue, rev3a.
//
// Design of record: docs/NPU-AT-XESS-SLOT-DESIGN.md. Wire-compatible with the
// R59 carrier (magic/slots/seqlock/doorbell parsing in seam_layer.c is used
// UNCHANGED); the mailbox header rev 6 (NMS-1) replaces the plan[]/route tail
// with the XeSS-geometry banks. Seat + layer deploy AS A PAIR (pair law).
//
// Frame flow (pipelined, one NPU generation of latency — design §1):
//   doorbell N (record time, game thread):
//     [input barriers -> TRANSFER_SRC] [3x copyImageToBuffer -> staging banks]
//     [stamp write <- N] ; if an NPU generation exists for N-1:
//     [output GENERAL->TRANSFER_DST] [copyBufferToImage npudst->Output]
//     [->GENERAL]  (work bit) ; if the worker is cold/stalled and fallback is
//     armed, the rev2a BLIT carrier is recorded instead (the proven image).
//   worker thread (native): waits eval_seq + GPU stamp, converts staging ->
//     int8[1,360,640,16] (rev3a transport fill — documented approximate),
//     graphExecute, packs fp16-RGBA -> B10G11R11 into npudst, npu_done = N.
//
// Everything here arms ONLY on FSR4NPU_NPU=1 (the seat gates on the same env,
// same process — mismatch is structurally impossible). Without it the layer's
// behavior is byte-identical to the pre-rev3 build.

#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include "vulkan_core.h"   /* ../fsr4-npu/audit/vklocal via -I — never hand-offset */

// ---- protocol constants (mirror seam_layer.c / the seat's rev13 block) ----
#define NM_MAILBOX_BYTES   0xBD0ULL
#define NM_HEADER_BYTES    0x2D0
/* rev3c.5 staging-bank capacity (640*368*4 = the seat's rawlr/mvec banks) —
 * copy rows clamp to this so a taller game feed can never write past */
#define NM_STAGE_CAP_BYTES (640ull * 368ull * 4ull)
#define NM_SLOT_BYTES      0x240
#define NM_SLOTS           4
#define NM_SLOT_FREE       0u
#define NM_SLOT_PUBLISHED  1u

/* r60f C-2 NMS PORT (ZCC2, the OUT zero-copy): 1 once the layer imported
 * the worker's OUT rpcmem onto the GAME device AND the first GPU snapshot
 * copy is flowing. qnn_glue's worker3 hardens its skip-gate on this — the
 * CPU ring memcpy (7.37MB/eval) stops ONLY when the GPU path provably runs,
 * so a failed import can never blank the image. The m9/bench ZCC2 code in
 * minimal_seam_layer.c targets the dead r19 chassis and never sets this. */
volatile int m9_zcc2_nms_active = 0;
extern volatile int m9_zcc3_active;   /* defined in minimal_seam_layer.c; worker3's ZCC3 gate reads it */
#define NM_MAILBOX_MAGIC   0x5345414D31344653ULL
#define NM_VERSION         7ULL           /* NMS-3: 0x2D0 header, stage2 double-buffer */
#define NM_ACK_WORK_BIT    0x8000000000000000ULL

// Mailbox header, NMS-1 overlay. The R59 prefix (magic..slots) sits at the
// same offsets so seam_on_cmd_update_buffer's slot/seqlock logic is reused
// verbatim; the tail (0x38..) is the NMS geometry. Total stays <= 0x210 so
// SEAM_HEADER_BYTES (and thus the slot base) is unchanged.
typedef struct {
    volatile uint64_t comp_pipe, comp_layout, comp_set;
    volatile uint64_t comp_dispatch;                 /* dgx | dgy<<32 */
    volatile uint64_t comp_dgz_push;                 /* dgz | push_bytes<<32 */
    volatile uint32_t comp_push[11];
    volatile uint32_t comp_patch;
    char _pad[4];
} NmPlan;                                             /* 96 B (R59 SeamPlan) */

typedef struct {
    volatile uint64_t magic;          // +0x00
    volatile uint64_t version;        // +0x08  == 6
    volatile uint64_t ack;            // +0x10  layer -> seat
    volatile uint64_t ack_count;      // +0x18
    volatile uint64_t seat_hint;      // +0x20
    volatile uint64_t slot_bytes;     // +0x28
    volatile uint64_t slots;          // +0x30
    volatile uint64_t eval_seq;       // +0x38  layer bumps per doorbell
    volatile uint64_t npu_done;       // +0x40  worker bumps per completed frame
    volatile uint64_t stamp_ptr;      // +0x48  CPU map: [0]=frame, [1]=guard
    volatile uint64_t stage_color_ptr;// +0x50  CPU maps of the staging banks
    volatile uint64_t stage_vel_ptr;  // +0x58
    volatile uint64_t stage_depth_ptr;// +0x60
    volatile uint64_t npudst_ptr;     // +0x68  CPU map, packed B10G11R11 words
    volatile uint64_t stage_color_buf;// +0x70  VkBuffer handles (game device)
    volatile uint64_t stage_vel_buf;  // +0x78
    volatile uint64_t stage_depth_buf;// +0x80
    volatile uint64_t stamp_buf;      // +0x88
    volatile uint64_t npudst_buf;     // +0x90
    volatile uint64_t stage_bytes;    // +0x98  one staging bank size (w*h*4)
    volatile uint64_t npudst_bytes;   // +0xA0  ow*oh*4
    volatile uint64_t frame_w;        // +0xA8  input geometry published at init
    volatile uint64_t frame_h;        // +0xB0
    volatile uint64_t out_w;          // +0xB8
    volatile uint64_t out_h;          // +0xC0
    volatile uint64_t nms_flags;      // +0xC8  bit0 worker armed, bit1 fallback, bit2 chassis
    /* rev3b chassis tail: plan[4]@0xD0 (4x96B) ends 0x250 */
    NmPlan plan[4];                   /* 0=feat 1=postA 2=RCAS 3=exp8to16 rec bridge */
    volatile uint64_t outq_ptr;       // +0x250 NPU in-tensor CPU map
    volatile uint64_t outq_bytes;     // +0x258
    volatile uint64_t route_buf;      // +0x260 fp16 out16 bank
    volatile uint64_t route_img;      // +0x268 probe image (fmt 97)
    /* rev3c.7 NMS-3 tail: the 3-SLOT npu-in ring + explicit publish (the
     * campaign's r59g law — deriving the slot from a done-counter is the
     * documented "parity bug"; the worker stores npu_pub = (gen<<8)|slot
     * after each copy-out and never touches the two most recent slots).
     * The record path copies the PUBLISHED slot -> npudst_buf (fresh gens
     * only) and postA runs EVERY doorbell (the r59h re-fire law). */
    volatile uint64_t stage2a_buf;    // +0x270 ring slot 0 (VkBuffer)
    volatile uint64_t stage2b_buf;    // +0x278 ring slot 1
    volatile uint64_t stage2a_ptr;    // +0x280 worker write map 0
    volatile uint64_t stage2b_ptr;    // +0x288 worker write map 1
    volatile uint64_t stage2c_buf;    // +0x290 ring slot 2
    volatile uint64_t stage2c_ptr;    // +0x298 worker write map 2
    volatile uint64_t npu_pub;        // +0x2A0 (gen<<8)|slot, worker-written
    /* rev3c.9 prev-depth tail + rev3c.10 ZCC2 handoff (APPEND-ONLY, pair-
     * locked with the seat's NmsHeader IN THE SAME ORDER — law #1):
     * dpt_prev = the geometric-disocclusion banks (parked); zcc2_* = the
     * SEAT-side import handshake (the layer publishes the worker's OUT
     * rpcmem fd; the SEAT imports it onto the game device — its nms_gipa
     * path is the proven one, every layer-side path failed, §23.17 — and
     * publishes the VkBuffer back). state: 0 idle, 1 fd seen, 2 imported,
     * 3 failed-permanent. */
    volatile uint64_t zcc3_in_fd;     // +0x2A8 layer -> seat (IN rpcmem spare dup; the parked prev-disoc slots repurposed — rename only)
    volatile uint64_t zcc3_imported_in_buf; // +0x2B0 seat -> layer (imported VkBuffer)
    volatile uint64_t zcc2_out_fd;    // +0x2B8 layer -> seat (rpcmem fd)
    volatile uint64_t zcc2_imported_buf; // +0x2C0 seat -> layer (VkBuffer)
    volatile uint64_t zcc2_state;     // +0x2C8 handshake state
} NmHeader;                            /* 0x2D0 used == span; route extents = out_w/out_h */

// Frame payload mirror (R59 SeamFrame, 512 B inside each slot — the fields
// this side reads; byte layout MUST match the seat's publish; offsets below
// are verified by the static asserts against the seat-side struct order).
typedef struct {
    uint64_t frame_seq, publish_seq;
    uint64_t cmdlist, feature, device, out_handle_addr;
    float    jitter_x, jitter_y, mv_scale_x, mv_scale_y, sharpness;
    uint32_t width, height, out_width, out_height, reset;
    uint32_t reserved_u[3];
    struct {
        uint64_t view, image;
        uint32_t aspect, base_mip, mip_count, base_layer, layer_count;
        uint32_t format, w, h, type, rw;
        uint32_t present;
        uint32_t _pad[1];
    } res[5];                          /* 0=Color 1=Depth 2=MV 3=Output 4=Exposure */
    char     _reserved[88];
} NmFrame;

_Static_assert(sizeof(NmFrame) == 512, "NmFrame must match the 512B slot payload");
_Static_assert(offsetof(NmFrame, res) == 104, "res[] offset must match R59 SeamFrame (u64-aligned after reserved_u)");
_Static_assert(sizeof(NmHeader) <= NM_HEADER_BYTES, "NmHeader must fit the header span");
_Static_assert(offsetof(NmHeader, nms_flags) == 0xC8, "NmHeader tail layout pinned");


/* rev3b.1 LIVE TUNING (the R59 marker pattern): the jitter mode and the MV
 * copy are flip-able DURING a session via one-digit files in SEAM_LOG_DIR,
 * re-read at most once a second:
 *   fsr4npu-JMODE : 0=pass-through (default) 1=zero 2=negate 3=half 4=double
 *   fsr4npu-MVMODE: 0=record vel copies (default) 1=skip (mvec stays zero)
 * Lets one launch sweep the jitter-convention hypothesis space. */
static int nm_file_mode(const char* name)
{
    static int cached[2]; static unsigned long long last_ms[2];
    static int idx_inited;
    unsigned long long now = (unsigned long long)time(NULL);
    int idx = name[9] == 'J' ? 0 : 1;              /* JMODE vs MVMODE */
    if (!idx_inited) { cached[0] = 0; cached[1] = 0; idx_inited = 1; }
    if (now != last_ms[idx] || !last_ms[idx]) {
        char p[256]; FILE* f;
        last_ms[idx] = now;
        snprintf(p, sizeof p, SEAM_LOG_DIR "/fsr4npu-%s", name);
        f = fopen(p, "r");
        if (f) { int v = fgetc(f) - '0'; fclose(f); cached[idx] = (v >= 0 && v <= 9) ? v : 0; }
        else cached[idx] = 0;
    }
    return cached[idx];
}

/* rev3b.1 LIVE EXPO: a float (ascii) in SEAM_LOG_DIR/fsr4npu-EXPO scales the
 * expo push word (feat word3; postA/rcas word4) — the mu-law input-encoding
 * scale knob. Default 1.0 (absent file). Re-read at most 1x/s. */
/* the seat auto-exposure handoff: stamp u64[6]=float bits, u64[7]=magic
 * (set by the record path once the header is known). Valid magic
 * OVERRIDES the knob file; the seat clears it on disarm. */
static const unsigned long long* nm_stamp_map;
static float nm_file_expo(void)
{
    static float cached = 4.0f;   /* baked: the knob-file-deletion revert
                                   * class — the user-validated image runs
                                   * EXPO 4.0 (campaign-era sweep peak) */
    static unsigned long long last;
    unsigned long long now = (unsigned long long)time(NULL);
    if (now != last) {
        char p[256]; FILE* f;
        last = now;
        snprintf(p, sizeof p, SEAM_LOG_DIR "/fsr4npu-EXPO");
        f = fopen(p, "r");
        if (f) { float v = 1.0f; if (fscanf(f, "%f", &v) == 1 && v > 0.0f && v < 100.0f) cached = v; fclose(f); }
        else cached = 4.0f;
    }
            if (nm_stamp_map && __atomic_load_n(&nm_stamp_map[7], __ATOMIC_ACQUIRE) == 0x4158504F455A4F43ull) {
            uint32_t bits = (uint32_t)(__atomic_load_n(&nm_stamp_map[6], __ATOMIC_ACQUIRE) & 0xffffffffull);
            float v; memcpy(&v, &bits, 4);
            if (v > 0.0f && v < 100.0f) cached = v;
        }
    return cached;
}

/* rev3c.8 LIVE KNOB (generic float): SEAM_LOG_DIR/fsr4npu-<name>, 1x/s.
 * BFLOOR/BFTAU drive wv2a's blend-floor machinery (the campaign's R18-M
 * verdict: floor 0.75 = "wow much better", 0.9 = blurry; bftau 0.35 =
 * dist-decay exercised). BFTAU ALSO gates wv1's disto write — without it
 * the floor never decays on real changes = the ghosting path. */
static float nm_file_knob(const char* name, float def)
{
    char p[256]; FILE* f; float v;
    snprintf(p, sizeof p, SEAM_LOG_DIR "/fsr4npu-%s", name);
    f = fopen(p, "r");
    if (!f) return def;
    if (fscanf(f, "%f", &v) != 1) v = def;
    fclose(f);
    return v;
}

/* rev3c.1 LIVE SHARP: float in SEAM_LOG_DIR/fsr4npu-SHARP overrides the
 * RCAS sharpness push word (default 0.5). sharp=-2 collapses the RCAS lobe
 * to ~0 = pure passthrough (the singularity diagnostic). */
static float nm_file_sharp(void)
{
    static float cached = 0.5f;
    static unsigned long long last;
    unsigned long long now = (unsigned long long)time(NULL);
    if (now != last) {
        char p[256]; FILE* f;
        last = now;
        snprintf(p, sizeof p, SEAM_LOG_DIR "/fsr4npu-SHARP");
        f = fopen(p, "r");
        if (f) { float v; if (fscanf(f, "%f", &v) == 1 && v > -4.0f && v < 4.0f) cached = v; fclose(f); }
        else cached = 0.5f;
    }
    return cached;
}
static void nm_apply_jmode(int mode, float* jx, float* jy)
{
    switch (mode) {
    case 1: *jx = 0.0f; *jy = 0.0f; break;
    case 2: *jx = -*jx; *jy = -*jy; break;
    case 3: *jx *= 0.5f; *jy *= 0.5f; break;
    case 4: *jx *= 2.0f; *jy *= 2.0f; break;
    default: break;
    }
}

// ---- logging (same shape as seam_layer.c's guarded stepm) ----
#ifdef SEAM_LOG
#ifndef SEAM_LOG_DIR
#define SEAM_LOG_DIR "/var/home/armada/fsr4npu-logs"
#endif
static void nm_log(const char* what, long v)
{
    FILE* f = fopen(SEAM_LOG_DIR "/fsr4npu-SEAM.log", "a");
    if (f) { fprintf(f, "%s = %ld (nms pid %d)\n", what, v, (int)getpid()); fclose(f); }
}
#else
static void nm_log(const char* what, long v) { (void)what; (void)v; }
#endif

// layer bridge: chain-resolved device fn table (dfn() is static in
// minimal_seam_layer.c; msl_dfn() is the exported wrapper).
extern void* msl_dfn(const char* name);
extern void* msl_dfn_dev(void* dev, const char* name);
extern void* msl_ires_dev(const char* name);
extern const void* seam_last_frame(unsigned long long*, unsigned long long*);
extern volatile int q6_probe_ok;

static void nm_wake(volatile uint64_t* w)
{
    syscall(98 /* SYS_futex, arm64 */, (const void*)w,
            129 /* FUTEX_WAKE_PRIVATE */, 1, 0, 0, 0);
}

// ---- the NPU worker thread (body lives in qnn_glue.c with the QNN statics) ----
extern void qnn_glue_worker_nms(volatile unsigned long long* eval_seq,
                                volatile unsigned long long* npu_done,
                                volatile unsigned long long* stamp,
                                const void* stage_color, const void* stage_vel,
                                const void* stage_depth, void* npudst,
                                unsigned long long fw, unsigned long long fh,
                                unsigned long long ow, unsigned long long oh,
                                unsigned long long stage_bytes,
                                unsigned long long npudst_bytes);

static void* nm_worker_thread(void* arg)
{
    NmHeader* h = *(NmHeader**)arg;
    int waited_ms = 0;
    pthread_setname_np(pthread_self(), "fsr4npu-nmswork");
    while (!q6_probe_ok && waited_ms < 30000) { usleep(1000); waited_ms++; }
    if (!q6_probe_ok) { nm_log("nms-worker-no-probe", waited_ms); return NULL; }
    if (!h->stamp_ptr || !h->stage_color_ptr || !h->npudst_ptr) {
        nm_log("nms-worker-no-banks", 0);
        return NULL;
    }
    h->nms_flags |= 1ull;              /* worker armed */
    if (h->outq_ptr && h->nms_flags & 4ull) {
        /* rev3b chassis mode: the RAW worker (outq tensor -> rpcmem in,
         * rpcmem out -> 3-slot ring verbatim + explicit publish — the
         * campaign's r59g production shape; our exp8to16 GPU bridge covers
         * the r59f rec feed, so rec slots pass NULL). Legacy 2-bank fallback
         * keeps worker2 (pre-NMS-3 seats / bench). */
        nm_log("nms-worker-start", 2);   /* 2 = raw/chassis worker */
        {   /* rev3c.7b: worker2 (2-slot parity) is the DEFAULT — worker3's
             * first run on this device SIGILL'd the process at its first
             * loop (coredump 18:12:41, thread at QNN activity; the zc-harness
             * phase is the unproven element). Opt back in with the
             * ~/fsr4npu-logs/fsr4npu-W3 marker. */
            static int w3 = -1;
            struct stat st;
            if (w3 < 0) w3 = (stat("/var/home/armada/fsr4npu-logs/fsr4npu-W3", &st) == 0) ? 1 : 0;
            if (w3 && h->stage2a_ptr && h->stage2b_ptr && h->stage2c_ptr) {
            extern void qnn_glue_worker3(volatile unsigned long long*, volatile unsigned long long*,
                                         volatile unsigned long long*, const void*,
                                         void*, void*, void*, void*, void*, void*,
                                         unsigned long long, unsigned long long);
            nm_log("nms-worker-staging", 3);   /* 3-slot ring + publish */
            qnn_glue_worker3(&h->eval_seq, &h->npu_done, &h->npu_pub,
                             (const void*)(uintptr_t)h->outq_ptr,
                             (void*)(uintptr_t)h->stage2a_ptr, (void*)(uintptr_t)h->stage2b_ptr,
                             (void*)(uintptr_t)h->stage2c_ptr,
                             NULL, NULL, NULL,
                             h->outq_bytes, h->npudst_bytes);
        } else {
            extern void qnn_glue_worker2(volatile unsigned long long*, volatile unsigned long long*,
                                         const void*, void*, void*, unsigned long long, unsigned long long);
            void* wa = (void*)(uintptr_t)(h->stage2a_ptr ? h->stage2a_ptr : h->npudst_ptr);
            void* wb = (void*)(uintptr_t)(h->stage2b_ptr ? h->stage2b_ptr : h->npudst_ptr);
            nm_log("nms-worker-staging", 2);   /* legacy pair */
            qnn_glue_worker2(&h->eval_seq, &h->npu_done,
                             (const void*)(uintptr_t)h->outq_ptr,
                             wa, wb,
                             h->outq_bytes, h->npudst_bytes);
        }
        }
    } else {
        nm_log("nms-worker-start", 1);   /* 1 = transport worker (rev3a) */
        qnn_glue_worker_nms(&h->eval_seq, &h->npu_done,
                            (volatile unsigned long long*)(uintptr_t)h->stamp_ptr,
                            (const void*)(uintptr_t)h->stage_color_ptr,
                            (const void*)(uintptr_t)h->stage_vel_ptr,
                            (const void*)(uintptr_t)h->stage_depth_ptr,
                            (void*)(uintptr_t)h->npudst_ptr,
                            h->frame_w, h->frame_h, h->out_w, h->out_h,
                            h->stage_bytes, h->npudst_bytes);
    }
    nm_log("nms-worker-exit", 0);
    h->nms_flags &= ~1ull;
    return NULL;
}

// ---- the record path (runs INSIDE the game's vkCmdUpdateBuffer call) ----
// Returns the ack work bit: 1 iff Output work was recorded into the doorbell's
// command buffer (NPU copy or the blit fallback).
int seam_nms_doorbell(void* hh, uint64_t seq, void* cb)
{
    static int armed = -1;
    static int fallback = -1;
    static void (*barrier)(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
                           VkDependencyFlags, uint32_t, const VkMemoryBarrier*,
                           uint32_t, const VkBufferMemoryBarrier*,
                           uint32_t, const VkImageMemoryBarrier*);
    static void (*cpy_i2b)(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer,
                           uint32_t, const VkBufferImageCopy*);
    static void (*cpy_b2i)(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout,
                           uint32_t, const VkBufferImageCopy*);
    static void (*upd)(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, const void*);
    static void (*blit)(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
                        uint32_t, const VkImageBlit*, VkFilter);
    static NmHeader* last_h;
    static uint64_t fallbacks, out_copies, cold_frames;
    NmHeader* h = (NmHeader*)hh;
    const NmFrame* f;
    unsigned long long fseq = 0, sseq = 0;

    if (h->magic != NM_MAILBOX_MAGIC || h->version != NM_VERSION) return 0;
    if (armed < 0) {
        const char* a = getenv("FSR4NPU_NPU");
        armed = (a && !strcmp(a, "1")) ? 1 : 0;
        nm_log("nms-armed", armed);
    }
    if (!armed) return 0;              /* carrier off: no records, no work bit */

    if (!barrier) {
        barrier  = (void (*)(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
                             VkDependencyFlags, uint32_t, const VkMemoryBarrier*,
                             uint32_t, const VkBufferMemoryBarrier*,
                             uint32_t, const VkImageMemoryBarrier*))msl_dfn("vkCmdPipelineBarrier");
        cpy_i2b  = (void (*)(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer,
                             uint32_t, const VkBufferImageCopy*))msl_dfn("vkCmdCopyImageToBuffer");
        cpy_b2i  = (void (*)(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout,
                             uint32_t, const VkBufferImageCopy*))msl_dfn("vkCmdCopyBufferToImage");
        upd      = (void (*)(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, const void*))msl_dfn("vkCmdUpdateBuffer");
        blit     = (void (*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
                             uint32_t, const VkImageBlit*, VkFilter))msl_dfn("vkCmdBlitImage");
        nm_log("nms-fns-ok", barrier && cpy_i2b && cpy_b2i && upd && blit ? 1 : 0);
        if (!barrier || !cpy_i2b || !cpy_b2i || !upd) { nm_log("nms-fns-miss", 0); return 0; }
    }
    if (fallback < 0) {
        const char* a = getenv("FSR4NPU_NPU_FALLBACK");
        fallback = (a && !strcmp(a, "0")) ? 0 : 1;   /* default ON */
    }

    // worker launch at the FIRST doorbell (after this the layer's q6 probe
    // thread may still be initializing; the worker thread waits for it).
    if (last_h != h) {
        pthread_t th;
        static NmHeader* spawn_h;
        spawn_h = h; last_h = h;
        if (pthread_create(&th, NULL, nm_worker_thread, &spawn_h) == 0)
            pthread_detach(th);
        else
            nm_log("nms-worker-spawn-fail", 0);
    }

    f = (const NmFrame*)seam_last_frame(&fseq, &sseq);
    if (!f) { nm_log("nms-no-frame", 0); return 0; }

    // ---- ratio era: the seat's active tier (reserved word; 1..5, 0=old seat) ----
    // The worker hot-swaps the QNN context bin between executes (qnn_glue).
    {
        static unsigned s_tier_seen = 0;
        unsigned t = f->reserved_u[0];
        if (t >= 1u && t <= 5u && t != s_tier_seen) {
            extern void q6_set_tier(int);
            q6_set_tier((int)t - 1);
            nm_log("nms-tier-frame", (long)t);
            s_tier_seen = t;
        }
    }

    // ---- stage 1: inputs -> staging banks + the GPU stamp ----
    {
        static const uint32_t asp[3] = { VK_IMAGE_ASPECT_COLOR_BIT,
                                         VK_IMAGE_ASPECT_DEPTH_BIT,
                                         VK_IMAGE_ASPECT_COLOR_BIT };
        static const uint64_t* bank_ptr[3];
        int i;
        bank_ptr[0] = &h->stage_color_buf;
        bank_ptr[1] = &h->stage_depth_buf;
        bank_ptr[2] = &h->stage_vel_buf;
        for (i = 0; i < 3; i++) {
            VkImage img = (VkImage)(uintptr_t)f->res[i].image;
            VkBuffer buf = (VkBuffer)(uintptr_t)*bank_ptr[i];
            VkImageMemoryBarrier b;
            VkBufferImageCopy c;
            uint32_t w = f->res[i].w, ht = f->res[i].h;
            if (i == 0 || i == 1 || i == 2) {
                /* clamp rows to the bank capacity. Ratio era: the seat sizes
                 * rawlr/mvec/dpt/dptp at the MAX preset (1152x640) and
                 * publishes that size as stage_bytes — all three banks take
                 * it (the old i==0-only + compile-time 640x368 cap truncated
                 * depth/vel the moment the game rendered past 368 rows). */
                uint64_t cap = h->stage_bytes ? h->stage_bytes
                             : (uint64_t)NM_STAGE_CAP_BYTES;
                uint64_t maxrows = cap / ((uint64_t)(w ? w : 1u) * 4ull);
                if ((uint64_t)ht > maxrows) ht = (uint32_t)maxrows;
            }
            if (i == 2 && nm_file_mode("MVMODE") == 1)
                continue;              /* live-tune: freeze mvec at zeros */
            if (!f->res[i].present || !img || !buf || !w || !ht) {
                if (i == 0) { nm_log("nms-color-miss", 0); return 0; }
                continue;              /* vel/depth optional in rev3a */
            }
            if (i == 1 && h->stage_depth_buf && nm_file_mode("PDISOC") == 1) {
                /* prev-depth disocclusion (rev3c.11, runbook §23.15/§23.18):
                 * keep the PREVIOUS frame's depth before this i2b overwrites
                 * dpt — wv1's (already-deployed) prev-vs-current test fires
                 * the AMD disocclusion signature at exposed strips; wv2a's
                 * bit-clamp outputs pure current there. The dptp VkBuffer
                 * arrives via the stamp-bank tail (u64[4]=buf, u64[5]=magic).
                 * LIVE-GATED by fsr4npu-PDISOC (1s cache): arm = touch,
                 * disarm = rm (disarm mid-run leaves stale prev = brief
                 * false disocclusions until re-armed or restarted). */
                static unsigned long long s_dptp_buf = 0;
                static int s_dptp_tried = 0;
                if (!s_dptp_buf && !s_dptp_tried) {
                    s_dptp_tried = 1;
                    const unsigned long long* stmap = (const unsigned long long*)(uintptr_t)h->stamp_ptr;
                    if (stmap && stmap[5] == 0x44505450434F5349ull) {
                        s_dptp_buf = stmap[4];
                        if (s_dptp_buf) nm_log("nms-pdisoc-bank", (long)(s_dptp_buf & 0xffffffffull));
                    }
                }
                if (s_dptp_buf) {
                    static void (*cpbD)(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy*);
                    if (!cpbD) cpbD = (void (*)(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy*))msl_dfn("vkCmdCopyBuffer");
                    if (cpbD) {
                        VkBufferCopy bcd;
                        memset(&bcd, 0, sizeof bcd);
                        bcd.size = (VkDeviceSize)(h->stage_bytes ? h->stage_bytes : (uint64_t)NM_STAGE_CAP_BYTES);
                        cpbD((VkCommandBuffer)cb, (VkBuffer)(uintptr_t)h->stage_depth_buf,
                             (VkBuffer)(uintptr_t)s_dptp_buf, 1, &bcd);
                    }
                }
            }
            memset(&b, 0, sizeof b);
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img;
            b.subresourceRange.aspectMask = f->res[i].aspect ? f->res[i].aspect : asp[i];
            b.subresourceRange.baseMipLevel = f->res[i].base_mip;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.baseArrayLayer = f->res[i].base_layer;
            b.subresourceRange.layerCount = 1;
            barrier((VkCommandBuffer)cb,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, NULL, 0, NULL, 1, &b);

            memset(&c, 0, sizeof c);
            c.imageSubresource.aspectMask = b.subresourceRange.aspectMask;
            c.imageSubresource.mipLevel = b.subresourceRange.baseMipLevel;
            c.imageSubresource.baseArrayLayer = b.subresourceRange.baseArrayLayer;
            c.imageSubresource.layerCount = 1;
            c.imageExtent.width = w;
            c.imageExtent.height = ht;
            c.imageExtent.depth = 1;
            cpy_i2b((VkCommandBuffer)cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    buf, 1, &c);

            b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier((VkCommandBuffer)cb,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    0, 0, NULL, 0, NULL, 1, &b);
        }
        // the stamp: {seq, guard} — the worker's GPU-execution oracle.
        if (upd && h->stamp_buf) {
            uint64_t st[2];
            st[0] = seq;
            st[1] = 0xC0FFEE00ull ^ seq;
            upd((VkCommandBuffer)cb, (VkBuffer)(uintptr_t)h->stamp_buf, 0, 16, st);
        }
    }

    // publish the eval to the worker (futex wake on OUR header word).
    __atomic_fetch_add(&h->eval_seq, 1, __ATOMIC_RELEASE);
    nm_wake(&h->eval_seq);

    // ---- stage 2: Output route ----
    {
        /* rev3a.1 ROUTE LAW: the graph's raw output is int8 CHANNEL WEIGHTS
         * (wv2a.comp: 2 words/texel, p0..p3+rv, decoded *S_OUT), NOT RGB —
         * the visible image is FORMED by the postA reconstruction (NPU
         * weights x mu-law LR color x history). Until that lands (rev3b)
         * the npudst copy would paint black-by-construction, so the BLIT is
         * the default visible route while the whole NPU pipeline (copies,
         * stamp, worker, graphExecute) keeps running behind it. Opt into
         * the raw route for testing with FSR4NPU_NPU_OUT=1. */
        static int raw_route = -1;
        uint64_t done;
        int have_npu;
        if (raw_route < 0) {
            const char* a = getenv("FSR4NPU_NPU_OUT");
            raw_route = (a && !strcmp(a, "1")) ? 1 : 0;
            nm_log("nms-route", raw_route);   /* 0 = blit (rev3a default) */
        }
        done = __atomic_load_n((volatile uint64_t*)&h->npu_done, __ATOMIC_ACQUIRE);
        have_npu = raw_route && (done >= 1ull);
        VkImage out_img = (VkImage)(uintptr_t)f->res[3].image;
        if (!f->res[3].present || !out_img) { nm_log("nms-output-miss", 0); return 0; }

        /* ---- rev3b: the chassis path (feat -> [postA -> RCAS -> route]) ----
         * feat dispatches EVERY doorbell (it consumes this frame's rawlr/
         * mvec copies + the rec/hist state and writes the NPU tensor). postA
         * + RCAS + the out16->probe->blit route record only on a FRESH NPU
         * generation (single npu-in bank: no tears at the cost of NPU-rate
         * image updates); otherwise fall to the blit (cold start). */
        if ((h->nms_flags & 4ull) && h->plan[0].comp_pipe) {
            static void (*bindp)(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
            static void (*binds_)(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t,
                                  uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*);
            static void (*pushc)(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void*);
            static void (*disp)(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
            static unsigned long long s_last_gen, s_feat_dispatches, s_full_routes;
            int have_full = (h->plan[1].comp_pipe && h->plan[2].comp_pipe && h->route_buf && h->route_img);
            if (!bindp) {
                bindp  = (void (*)(VkCommandBuffer, VkPipelineBindPoint, VkPipeline))msl_dfn("vkCmdBindPipeline");
                binds_ = (void (*)(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t,
                                   uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*))msl_dfn("vkCmdBindDescriptorSets");
                pushc  = (void (*)(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void*))msl_dfn("vkCmdPushConstants");
                disp   = (void (*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t))msl_dfn("vkCmdDispatch");
                nm_log("nms-chassis-fns", bindp && binds_ && pushc && disp ? 1 : 0);
            }
            if (bindp && binds_ && pushc && disp) {
                if (!nm_stamp_map && h->stamp_ptr)
                    nm_stamp_map = (const unsigned long long*)(uintptr_t)h->stamp_ptr;
                /* stage 0: feat — every doorbell, patch jitter/reset current */
                uint32_t push[11];
                NmPlan* p;
                float jx = f->jitter_x, jy = f->jitter_y, sharp = f->sharpness;
                float rf = f->reset ? 1.0f : 0.0f;
                nm_apply_jmode(nm_file_mode("JMODE"), &jx, &jy);
                sharp = nm_file_sharp();
                p = (NmPlan*)&h->plan[0];
                memcpy(push, (const void*)p->comp_push, sizeof push);
                { float e = nm_file_expo() * 1.0f; memcpy(&push[3], &e, 4); }   /* feat expo */
                { float bf = nm_file_knob("BFTAU", 0.35f); memcpy(&push[4], &bf, 4); } /* feat u_bftau: gates the disto write (the floor's decay input) */
                { float dl = nm_file_knob("DILATE", 1.0f); memcpy(&push[9], &dl, 4); }  /* AMD default: ALWAYS dilate */ /* feat u_dilate (word9, block 40B): AMD dilate_mv — 0 = legacy center read */
                if (p->comp_patch & 1u) memcpy(&push[0], &jx, 4);
                if (p->comp_patch & 2u) memcpy(&push[1], &jy, 4);
                if (p->comp_patch & 8u) memcpy(&push[2], &rf, 4);
                bindp((VkCommandBuffer)cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipeline)(uintptr_t)p->comp_pipe);
                binds_((VkCommandBuffer)cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipelineLayout)(uintptr_t)p->comp_layout,
                       0, 1, (const VkDescriptorSet*)&p->comp_set, 0, NULL);
                pushc((VkCommandBuffer)cb, (VkPipelineLayout)(uintptr_t)p->comp_layout,
                      VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)(p->comp_dgz_push >> 32), push);
                disp((VkCommandBuffer)cb, (uint32_t)(p->comp_dispatch & 0xffffffffull),
                     (uint32_t)(p->comp_dispatch >> 32), (uint32_t)p->comp_dgz_push);
                s_feat_dispatches++;
                if ((s_feat_dispatches & 0x3FF) == 1) nm_log("nms-feat-dispatches", (long)s_feat_dispatches);

                /* rev3c.7 — the campaign's r59g/r59h laws, ported:
                 * - SNAPSHOT COPY only on a FRESH PUBLISHED generation, from
                 *   the PUBLISHED 3-slot ring slot (never derive the slot
                 *   from a counter — the documented "parity bug").
                 * - postA/bridge/RCAS/route run EVERY doorbell once any
                 *   generation exists (the r59h re-fire fix: gating on fresh
                 *   generations re-presents stale banks through the always-on
                 *   copy = the explosion "re-fire" / odd tile flashes). */
                {
                unsigned long long pub = __atomic_load_n((volatile unsigned long long*)&h->npu_pub, __ATOMIC_ACQUIRE);
                unsigned long long pgen = pub >> 8, pslot = pub & 0xffull;
                static unsigned long long s_copied_gen;
                /* ZCC2-NMS rev3c.10 — THE SEAT DOES THE IMPORT (§23.17: every
                 * layer-side resolution path for game-device creation fns
                 * failed: chain-gdpa NULL, null-instance gipa NULL,
                 * captured-instance gipa NULL; and msl_dnf fns are
                 * g_chain_dev-dispatch — calling with the game device SEGVs
                 * and wine swallows the thread death). Handoff: publish the
                 * worker's OUT rpcmem fd in the header tail; the seat's diag
                 * poller imports it with its PROVEN nms_gipa path; we pick
                 * the VkBuffer up here and snapshot-copy from it. */
                static unsigned long long zcc2_nms_buf;
                static int zcc2_nms_st = -1;   /* -1 idle, 0 fd published, 1 live, -2 failed */
                if (zcc2_nms_st == -1) {
                    FILE* zf = fopen(SEAM_LOG_DIR "/fsr4npu-ZCC2", "r");
                    if (zf) fclose(zf);
                    if (zf && q6_probe_ok) {
                        extern int q6_out_fd0(void);
                        int fd = q6_out_fd0();
                        if (fd >= 0) {
                            int spare = dup(fd);   /* the seat's import CONSUMES the fd it gets; publish a native-side SPARE (wine's CRT _dup can't see Linux fds — gle=2, §23.17) */
                            if (spare >= 0) {
                                zcc2_nms_st = 0;
                                h->zcc2_out_fd = (uint64_t)spare;
                                nm_log("nms-zcc2-fd-pub", (long)spare);
                            }
                        }
                    }
                }
                if (zcc2_nms_st == 0) {
                    uint64_t st = __atomic_load_n((volatile uint64_t*)&h->zcc2_state, __ATOMIC_ACQUIRE);
                    if (st == 2ull && h->zcc2_imported_buf) {
                        zcc2_nms_buf = h->zcc2_imported_buf;
                        zcc2_nms_st = 1;
                        nm_log("nms-zcc2-imported", (long)(zcc2_nms_buf & 0xffffffffull));
                    } else if (st == 3ull) {
                        zcc2_nms_st = -2;
                        nm_log("nms-zcc2-seat-fail", 1);
                    }
                }
                /* ZCC3 (the IN side): publish the worker's IN rpcmem spare
                 * dup; the seat imports it; the recorded copy below feeds
                 * the NPU directly from the game CB. */
                static unsigned long long zcc3_nms_buf;
                static int zcc3_pub = 0;
                if (!zcc3_pub) {
                    FILE* zf3 = fopen(SEAM_LOG_DIR "/fsr4npu-ZCC3", "r");
                    if (zf3) fclose(zf3);
                    if (zf3 && q6_probe_ok) {
                        extern int q6_in_fd0(void);
                        int ifd = q6_in_fd0();
                        if (ifd >= 0) {
                            int spare3 = dup(ifd);
                            if (spare3 >= 0) {
                                h->zcc3_in_fd = (uint64_t)spare3;
                                zcc3_pub = 1;
                                nm_log("nms-zcc3-fd-pub", (long)spare3);
                            }
                        }
                    }
                }
                if (zcc3_pub && !zcc3_nms_buf && h->zcc3_imported_in_buf) {
                    zcc3_nms_buf = h->zcc3_imported_in_buf;
                    nm_log("nms-zcc3-imported", (long)(zcc3_nms_buf & 0xffffffffull));
                }
                if (zcc3_nms_buf) {
                    /* outq -> imported-IN on the GAME CB, right after the
                     * feat dispatch wrote outq (the worker consumes it at
                     * +2 pacing — the stamp oracle guarantees execution).
                     * Implicit dmabuf sync rides the dma-resv (the
                     * campaign's AHBSYNC=0 law, bench-validated class). */
                    static void (*cpyb3)(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy*);
                    if (!cpyb3) cpyb3 = (void (*)(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy*))msl_dfn("vkCmdCopyBuffer");
                    if (cpyb3) {
                        VkMemoryBarrier mb3;
                        VkBufferCopy bc3;
                        memset(&mb3, 0, sizeof mb3);
                        mb3.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                        mb3.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                        mb3.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                        barrier((VkCommandBuffer)cb,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                0, 1, &mb3, 0, NULL, 0, NULL);
                        /* the outq VkBuffer arrives via the stamp-bank tail
                         * (stamp u64[2], magic u64[3] — the header is full) */
                        static unsigned long long s_outq_buf = 0;
                        const unsigned long long* stmap = (const unsigned long long*)(uintptr_t)h->stamp_ptr;
                        if (!s_outq_buf && stmap && stmap[3] == 0x5A4351AF4C554251ull)
                            s_outq_buf = stmap[2];
                        if (s_outq_buf) {
                            memset(&bc3, 0, sizeof bc3);
                            bc3.size = (VkDeviceSize)h->outq_bytes;
                            cpyb3((VkCommandBuffer)cb, (VkBuffer)(uintptr_t)s_outq_buf,
                                  (VkBuffer)(uintptr_t)zcc3_nms_buf, 1, &bc3);
                        }
                        mb3.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        mb3.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                        barrier((VkCommandBuffer)cb,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                0, 1, &mb3, 0, NULL, 0, NULL);
                        if (s_outq_buf) {
                            { static int zc3a = 0; if (!zc3a) { zc3a = 1; nm_log("nms-zcc3-copy-armed", 1); } }
                            m9_zcc3_active = 1;   /* releases worker3's CPU input bridge */
                        }
                    }
                }
                if (have_full && (pgen >= 1ull || done >= 1ull)) {
                    /* snapshot copy: pub-mode when worker3 publishes
                     * (npu_pub nonzero), else the worker2 done-parity scheme
                     * (dst = (gen&1) bank; matches worker2's (e-1)&1 write) */
                    int do_copy = 0;
                    uint64_t srcb = 0;
                    if (zcc2_nms_st == 1 && pgen >= 1ull && pgen != s_copied_gen) {
                        srcb = zcc2_nms_buf;   /* GPU reads the imported rpcmem directly */
                        do_copy = 1;
                    } else if (pgen >= 1ull && pgen != s_copied_gen && pslot < 3ull && h->stage2c_buf) {
                        srcb = (pslot == 0ull) ? h->stage2a_buf
                             : (pslot == 1ull) ? h->stage2b_buf : h->stage2c_buf;
                        do_copy = 1;
                    } else if (!pgen && done >= 1ull && done != s_copied_gen && h->stage2b_buf) {
                        srcb = (done & 1ull) ? h->stage2b_buf : h->stage2a_buf;
                        do_copy = 1;
                    }
                    if (do_copy && h->stage2a_buf && h->npudst_buf) {
                        static void (*cpyb)(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy*);
                        if (!cpyb) cpyb = (void (*)(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy*))msl_dfn("vkCmdCopyBuffer");
                        if (cpyb) {
                            VkBufferCopy bc;
                            VkBufferMemoryBarrier nb;
                            memset(&bc, 0, sizeof bc);
                            bc.size = (VkDeviceSize)h->npudst_bytes;
                            cpyb((VkCommandBuffer)cb, (VkBuffer)(uintptr_t)srcb,
                                 (VkBuffer)(uintptr_t)h->npudst_buf, 1, &bc);
                            memset(&nb, 0, sizeof nb);
                            nb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                            nb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                            nb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                            nb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            nb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            nb.buffer = (VkBuffer)(uintptr_t)h->npudst_buf;
                            nb.size = VK_WHOLE_SIZE;
                            barrier((VkCommandBuffer)cb,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    0, 0, NULL, 1, &nb, 0, NULL);
                            s_copied_gen = pgen ? pgen : done;
                            if (zcc2_nms_st == 1) m9_zcc2_nms_active = 1;   /* release worker3's 7.37MB CPU ring memcpy (r60f C-2) */
                        }
                    }
                    /* r59d fix-4a: execution+memory barrier BEFORE each stage —
                     * back-to-back dispatches let later stages read stale caches */
                    {   VkMemoryBarrier mb;
                        memset(&mb, 0, sizeof mb);
                        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                        barrier((VkCommandBuffer)cb,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                0, 1, &mb, 0, NULL, 0, NULL);
                    }
                    /* stage 1: postA (EVERY doorbell once any gen — r59h) */
                    p = (NmPlan*)&h->plan[1];
                    memcpy(push, (const void*)p->comp_push, sizeof push);
                    { float e = nm_file_expo() * 1.0f; memcpy(&push[4], &e, 4); }   /* postA expo */
                    { float bf = nm_file_knob("BFLOOR", 0.0f); memcpy(&push[2], &bf, 4); }  /* AMD default: NO floor (knob up for shimmer insurance) */  /* wv2a word2: blend floor (campaign R18-M: 0.75) */
                    { float bt = nm_file_knob("BFTAU", 0.35f); memcpy(&push[3], &bt, 4); }  /* wv2a word3: dist-decay tau (campaign: 0.35) */
                    if (p->comp_patch & 1u) memcpy(&push[0], &jx, 4);
                    if (p->comp_patch & 2u) memcpy(&push[1], &jy, 4);
                    bindp((VkCommandBuffer)cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipeline)(uintptr_t)p->comp_pipe);
                    binds_((VkCommandBuffer)cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipelineLayout)(uintptr_t)p->comp_layout,
                           0, 1, (const VkDescriptorSet*)&p->comp_set, 0, NULL);
                    pushc((VkCommandBuffer)cb, (VkPipelineLayout)(uintptr_t)p->comp_layout,
                          VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)(p->comp_dgz_push >> 32), push);
                    disp((VkCommandBuffer)cb, (uint32_t)(p->comp_dispatch & 0xffffffffull),
                         (uint32_t)(p->comp_dispatch >> 32), (uint32_t)p->comp_dgz_push);

                    /* stage 3: the REC BRIDGE (exp8to16: u8 recout ->
                     * fp16-pair recp for NEXT frame's wv1) — right after
                     * postA, queue-ordered (the R59 law). No push. */
                    {   VkMemoryBarrier mb;
                        memset(&mb, 0, sizeof mb);
                        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                        barrier((VkCommandBuffer)cb,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                0, 1, &mb, 0, NULL, 0, NULL);
                    }
                    p = (NmPlan*)&h->plan[3];
                    if (p->comp_pipe) {
                        bindp((VkCommandBuffer)cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipeline)(uintptr_t)p->comp_pipe);
                        binds_((VkCommandBuffer)cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipelineLayout)(uintptr_t)p->comp_layout,
                               0, 1, (const VkDescriptorSet*)&p->comp_set, 0, NULL);
                        disp((VkCommandBuffer)cb, (uint32_t)(p->comp_dispatch & 0xffffffffull),
                             (uint32_t)(p->comp_dispatch >> 32), (uint32_t)p->comp_dgz_push);
                    }

                    /* stage 2: RCAS (hst -> out16 route bank) */
                    {   VkMemoryBarrier mb;
                        memset(&mb, 0, sizeof mb);
                        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                        barrier((VkCommandBuffer)cb,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                0, 1, &mb, 0, NULL, 0, NULL);
                    }
                    p = (NmPlan*)&h->plan[2];
                    memcpy(push, (const void*)p->comp_push, sizeof push);
                    { float e = nm_file_expo() * 1.0f; memcpy(&push[4], &e, 4); }   /* rcas expo */
                    if (p->comp_patch & 1u) memcpy(&push[0], &jx, 4);
                    if (p->comp_patch & 2u) memcpy(&push[1], &jy, 4);
                    if (p->comp_patch & 4u) memcpy(&push[9], &sharp, 4);
                    bindp((VkCommandBuffer)cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipeline)(uintptr_t)p->comp_pipe);
                    binds_((VkCommandBuffer)cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipelineLayout)(uintptr_t)p->comp_layout,
                           0, 1, (const VkDescriptorSet*)&p->comp_set, 0, NULL);
                    pushc((VkCommandBuffer)cb, (VkPipelineLayout)(uintptr_t)p->comp_layout,
                          VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)(p->comp_dgz_push >> 32), push);
                    disp((VkCommandBuffer)cb, (uint32_t)(p->comp_dispatch & 0xffffffffull),
                         (uint32_t)(p->comp_dispatch >> 32), (uint32_t)p->comp_dgz_push);

                    /* the ROUTE: out16 bank -> probe image (fmt 97) -> blit
                     * into the game's Output (hardware fp16->B10G11R11
                     * conversion — the r58s PROVEN carrier shape). */
                    {
                        static void (*cpyB)(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy*);
                        static int probe_layout;   /* 0=UNDEFINED 1=TRANSFER_DST 2=TRANSFER_SRC */
                        if (!cpyB) cpyB = (void (*)(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy*))msl_dfn("vkCmdCopyBufferToImage");
                        if (cpyB) {
                            VkBufferMemoryBarrier bb;
                            VkImageMemoryBarrier ib;
                            VkBufferImageCopy c;
                            VkImageBlit bi;
                            VkBuffer rb = (VkBuffer)(uintptr_t)h->route_buf;
                            VkImage pi = (VkImage)(uintptr_t)h->route_img;
                            memset(&bb, 0, sizeof bb);
                            bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                            bb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                            bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                            bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            bb.buffer = rb;
                            bb.size = VK_WHOLE_SIZE;
                            { void (*barB)(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
                                           VkDependencyFlags, uint32_t, const VkMemoryBarrier*,
                                           uint32_t, const VkBufferMemoryBarrier*,
                                           uint32_t, const VkImageMemoryBarrier*) = barrier;
                              barB((VkCommandBuffer)cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, 0, NULL, 1, &bb, 0, NULL); }
                            memset(&ib, 0, sizeof ib);
                            ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                            ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                            ib.oldLayout = probe_layout == 2 ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                         : probe_layout == 0 ? VK_IMAGE_LAYOUT_UNDEFINED
                                         : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                            ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                            ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            ib.image = pi;
                            ib.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                            ib.subresourceRange.levelCount = 1;
                            ib.subresourceRange.layerCount = 1;
                            barrier((VkCommandBuffer)cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    0, 0, NULL, 0, NULL, 1, &ib);
                            probe_layout = 1;
                            memset(&c, 0, sizeof c);
                            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                            c.imageSubresource.layerCount = 1;
                            c.imageExtent.width  = (uint32_t)h->out_w;
                            c.imageExtent.height = (uint32_t)h->out_h;
                            c.imageExtent.depth = 1;
                            cpyB((VkCommandBuffer)cb, rb, pi, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
                            ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                            ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                            ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                            ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                            barrier((VkCommandBuffer)cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    0, 0, NULL, 0, NULL, 1, &ib);
                            probe_layout = 2;
                            /* output GENERAL->TRANSFER_DST, blit, back (the
                             * proven rev2a shape) */
                            memset(&ib, 0, sizeof ib);
                            ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                            ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                            ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                            ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                            ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            ib.image = out_img;
                            ib.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                            ib.subresourceRange.levelCount = 1;
                            ib.subresourceRange.layerCount = 1;
                            barrier((VkCommandBuffer)cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    0, 0, NULL, 0, NULL, 1, &ib);
                            memset(&bi, 0, sizeof bi);
                            bi.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                            bi.srcSubresource.layerCount = 1;
                            bi.srcOffsets[1].x = (int32_t)h->out_w;
                            bi.srcOffsets[1].y = (int32_t)h->out_h;
                            bi.srcOffsets[1].z = 1;
                            bi.dstSubresource = bi.srcSubresource;
                            bi.dstOffsets[1].x = (int32_t)(f->res[3].w ? f->res[3].w : (uint32_t)h->out_w);
                            bi.dstOffsets[1].y = (int32_t)(f->res[3].h ? f->res[3].h : (uint32_t)h->out_h);
                            bi.dstOffsets[1].z = 1;
                            blit((VkCommandBuffer)cb, pi, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out_img,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bi, VK_FILTER_LINEAR);
                            memset(&ib, 0, sizeof ib);
                            ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                            ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                            ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                            ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                            ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            ib.image = out_img;
                            ib.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                            ib.subresourceRange.levelCount = 1;
                            ib.subresourceRange.layerCount = 1;
                            barrier((VkCommandBuffer)cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    0, 0, NULL, 0, NULL, 1, &ib);
                            s_last_gen = done;
                            s_full_routes++;
                            if ((s_full_routes & 0x3FF) == 1) nm_log("nms-full-routes", (long)s_full_routes);
                            return 1;      /* work bit: the real image route */
                        }
                    }
                }
                }
                /* chassis on but no generation yet: fall through to
                 * the blit below (cold start) */
            } else {
                nm_log("nms-chassis-fns-miss", 0);
            }
        }

        if (have_npu && cpy_b2i && h->npudst_buf) {
            VkImageMemoryBarrier b;
            VkBufferImageCopy c;
            memset(&b, 0, sizeof b);
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = out_img;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = 1;
            barrier((VkCommandBuffer)cb,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, NULL, 0, NULL, 1, &b);

            memset(&c, 0, sizeof c);
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.layerCount = 1;
            c.imageExtent.width = f->res[3].w ? f->res[3].w : (uint32_t)h->out_w;
            c.imageExtent.height = f->res[3].h ? f->res[3].h : (uint32_t)h->out_h;
            c.imageExtent.depth = 1;
            cpy_b2i((VkCommandBuffer)cb, (VkBuffer)(uintptr_t)h->npudst_buf, out_img,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);

            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = 0;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier((VkCommandBuffer)cb,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    0, 0, NULL, 0, NULL, 1, &b);
            out_copies++;
            if ((out_copies & 0x3FF) == 1) nm_log("nms-out-copies", (long)out_copies);
            return 1;                  /* work bit */
        }

        // fallback: the rev2a blit carrier (the proven visible image). Arms
        // while the worker is cold (first frames) or stalled; disable with
        // FSR4NPU_NPU_FALLBACK=0 (bench "pure" mode).
        if (fallback && blit) {
            VkImageMemoryBarrier b;
            VkImageBlit bi;
            memset(&b, 0, sizeof b);
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = out_img;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = 1;
            barrier((VkCommandBuffer)cb,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, NULL, 0, NULL, 1, &b);

            memset(&bi, 0, sizeof bi);
            bi.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bi.srcSubresource.layerCount = 1;
            bi.srcOffsets[1].x = (int32_t)f->res[0].w;
            bi.srcOffsets[1].y = (int32_t)f->res[0].h;
            bi.srcOffsets[1].z = 1;
            bi.dstSubresource = bi.srcSubresource;
            bi.dstOffsets[1].x = (int32_t)(f->res[3].w ? f->res[3].w : (uint32_t)h->out_w);
            bi.dstOffsets[1].y = (int32_t)(f->res[3].h ? f->res[3].h : (uint32_t)h->out_h);
            bi.dstOffsets[1].z = 1;
            blit((VkCommandBuffer)cb, (VkImage)(uintptr_t)f->res[0].image,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, out_img,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bi, VK_FILTER_LINEAR);

            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = 0;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier((VkCommandBuffer)cb,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    0, 0, NULL, 0, NULL, 1, &b);
            if (done >= 1ull) fallbacks++; else cold_frames++;
            if (((fallbacks + cold_frames) & 0x3FF) == 1)
                nm_log("nms-blit-fallback", (long)(fallbacks + cold_frames));
            return 1;                  /* work bit (real Output work recorded) */
        }
        if (!fallback) cold_frames++;
    }
    return 0;
}
