// seam_layer.c — REV13 SEAT-SEAM, layer side (the doorbell/mailbox/ack carrier).
//
// Protocol mirror of the seat's rev13 seam block (nvngx_vk_seat.c):
//   - the SEAT (nvngx.dll in-process under wine) creates two signature
//     buffers through winevulkan at CreateFeature time:
//       MAILBOX: size 0x940, STORAGE|TRANSFER_DST, HOST_VISIBLE|COHERENT,
//                mapped persistently, magic "SEAM" at +0
//       MARKER : size 0x2D0, TRANSFER_DST, init pattern at +0x10
//   - per eval the seat seqlock-publishes a 512B frame into a FREE slot,
//     then records vkCmdUpdateBuffer(InCmdList, marker, 0, 16, {magic, seq})
//   - THIS layer sees that update at RECORD TIME (same thread, inside the
//     seat's call): validates magic, finds the slot by seq, copies the
//     frame, writes ack through the same host-coherent pages.
//
// ACK WORK-BIT LAW (the zero-latency rule made mechanical):
//   ack = seq            -> carrier seen, NO Output work attached yet.
//                           The seat STILL returns Fail in-game (the game
//                           keeps its fallback) but logs carrier-seen.
//   ack = seq | (1<<63)  -> the layer recorded real Output work into the
//                           same command buffer. Only then may the seat
//                           return Success.
// This carrier build (falsifier milestone 1) always acks WITHOUT the work
// bit. The dispatch-appending build sets it.
//
// HANDLE DOCTRINE (P1/D): handles arrive as winevulkan GUEST values. We
// validate identity against our own create-map and report a verdict
// (identity | translated | unknown); ANY miss fails this frame closed —
// the carrier never guesses.
//
// Telemetry: stepm() lines only (buildB LOG_IO shape); ZERO file I/O in
// any other build shape.

#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

// ---- protocol constants (MUST match the seat exactly) ----
#define SEAM_MAILBOX_BYTES      0xB10ULL   /* header 0x210 + 4*0x240 slots (REV34; 0xAB0 was pre-REV34, 0x940 pre-v3) */
#define SEAM_MARKER_BYTES       0x2D0ULL
#define SEAM_MAILBOX_MAGIC      0x5345414D31344653ULL
#define SEAM_DOORBELL_MAGIC     0x444F4F52424F4E47ULL
#define SEAM_MARKER_INIT_PATTERN 0x4D41524B45523031ULL
#define SEAM_SLOT_FREE          0u
#define SEAM_SLOT_PUBLISHED     1u
#define SEAM_SLOT_CONSUMING     2u
#define SEAM_HEADER_BYTES       0x2D0   /* rev3c.6 NMS-3: plan[4] tail + stage2 pair (pair-locked with the seat) */
#define SEAM_SLOT_BYTES         0x240
#define SEAM_SLOTS              4
#define SEAM_ACK_WORK_BIT       0x8000000000000000ULL
/* REV34: verified against NDK vulkan_core.h — TRANSFER_DST is 0x2 (:2912),
 * STORAGE_BUFFER is 0x20 (:2916). The old 0x20/0x40 pair matched the seat's
 * equally-old wrong defines, so the marker signature still matches (both
 * sides fixed together); the values are now the real enum. */
#define SEAM_VK_BUFFER_USAGE_TRANSFER_DST_BIT   0x00000002u
#define SEAM_VK_BUFFER_USAGE_STORAGE_BUFFER_BIT 0x00000020u

typedef struct {
    uint64_t view, image;
    uint32_t aspect, base_mip, mip_count, base_layer, layer_count;
    uint32_t format, w, h, type, rw;
    uint32_t present;
    uint32_t _pad[1];
} SeamRes;                                   // 64 B

typedef struct {
    uint64_t frame_seq, publish_seq;
    uint64_t cmdlist, feature, device, out_handle_addr;
    float    jitter_x, jitter_y, mv_scale_x, mv_scale_y, sharpness;
    uint32_t width, height, out_width, out_height, reset;
    uint32_t reserved_u[3];
    SeamRes  res[5];
    char     _reserved[88];
} SeamFrame;                                 // 512 B

typedef struct {
    volatile uint64_t seq;
    volatile uint32_t state;
    char _pad[4];
    SeamFrame frame;                          // @ +0x10 within the slot
} SeamSlotBody;                               // 0x210 used of 0x240 stride

typedef struct {                             // one stage's plan (REV28)
    volatile uint64_t comp_pipe, comp_layout, comp_set;
    volatile uint64_t comp_dispatch;         // dgx | dgy<<32
    volatile uint64_t comp_dgz_push;         // dgz | push_bytes<<32
    volatile uint32_t comp_push[11];
    volatile uint32_t comp_patch;
    char _pad[4];
} SeamPlan;                                   // 96 B (5 u64 + 44 + 4 + 4 = 92, 8-aligned)

typedef struct {
    volatile uint64_t magic, version, ack, ack_count, seat_hint, slot_bytes, slots;
    /* REV28: stage 0 = feat+pre0 (every doorbell), stage 1 = postA
     * (gated on npu_done). REV33: stage 2 = RCAS (final color, after
     * postA). REV34: stage 3 = route-write (ROUTE-V2: imageStores the
     * game's Output VIEW as a STORAGE IMAGE; recorded after RCAS).
     * Shared banks rawlr/rpj/hst/disto; hst is the temporal
     * feedback (postA writes, feat reads next eval, RCAS reads it too). */
    SeamPlan plan[4];
    volatile uint64_t outq_ptr;               // feat output, CPU-mapped
    volatile uint64_t npudst_ptr;             // postA binding-4 bank, CPU-mapped
    volatile uint64_t eval_seq;               // layer bumps per feat dispatch
    volatile uint64_t npu_done;               // worker bumps after copy-out
    volatile uint64_t outq_bytes;             // bank sizes (worker clamps)
    volatile uint64_t npudst_bytes;
    /* REV33 (M9): the output route (RCAS out8 -> probe img -> game Output) */
    volatile uint64_t route_buf;
    volatile uint64_t route_img;
    volatile uint32_t route_os_w, route_os_h;
    /* REV34 (ROUTE-V2): the transfer/blit route is RETIRED — plan[3]
     * (route_write.comp) imageStores the game's Output VIEW as a STORAGE
     * IMAGE instead. These two fields (the old _reserved[16]) hand the
     * layer the owning probe VkDevice (its per-eval vkUpdateDescriptorSets
     * target for binding 8's view) and the stage-3 descriptor set. */
    volatile uint64_t route_dev;              // probe VkDevice of the stage-3 objects
    volatile uint64_t route_set;              // plan[3]'s descriptor set
} SeamHeader;                                 // 0x210

// ---- tracking state ----
#define SEAM_MAX_TRACK 64
struct seam_ent {
    void*        buf;
    uint64_t     size;
    uint32_t     usage;
    int          kind;     // 1 mailbox-cand, 2 marker-cand, 3 mailbox, 4 marker,
                           // 5 outq-cand (r60h stage-3: NPU input bank source)
    void*        mem;
    void*        map;      // host pointer seen at the seat's vkMapMemory
};
static struct seam_ent g_seam_track[SEAM_MAX_TRACK];
static int g_seam_ntrack;
static uint64_t g_seam_doorbells, g_seam_acks, g_seam_badmagic, g_seam_noslot;
/* REV47 (PQ2, B1): the C8 miss-path discriminator — every vkCmdUpdateBuffer
 * call is counted; the cumulative set (calls / doorbells / shape-miss /
 * badmagic / noslot) is dumped every 4096 calls so a frozen-ack run still
 * shows which branch the doorbells die in. The per-miss detail lines are
 * throttled to 1-in-256 of their kind (they fired UNTHROTTLED before). */
static uint64_t g_seam_updcalls, g_seam_shape_miss;
static uint64_t g_seam_last_frame_seq, g_seam_last_seq;
static SeamFrame g_seam_last_frame;   // diagnostics/falsifier readback (copy of the last acked slot)

#ifdef SEAM_LOG
// deployed (buildB LOG_IO) shape: append one line to the probe telemetry dir
#include <fcntl.h>
#ifndef SEAM_LOG_DIR
#define SEAM_LOG_DIR "/var/home/armada/fsr4npu-logs"
#endif
static void stepm(const char* what, long v)
{
    FILE* f = fopen(SEAM_LOG_DIR "/fsr4npu-SEAM.log", "a");
    if (f) { fprintf(f, "%s = %ld (pid %d)\n", what, v, (int)getpid()); fclose(f); }
}
#else
static void stepm(const char* what, long v) { (void)what; (void)v; }
#endif

static struct seam_ent* seam_find_buf(void* buf) {
    for (int i = 0; i < g_seam_ntrack; i++)
        if (g_seam_track[i].buf == buf) return &g_seam_track[i];
    return NULL;
}

// called from the vkCreateBuffer trampoline AFTER down() succeeds
void seam_on_create_buffer(int32_t result, uint64_t size, uint32_t usage, void* buf)
{
    if (result != 0 || !buf || g_seam_ntrack >= SEAM_MAX_TRACK) return;
    if (size == SEAM_MAILBOX_BYTES &&
        (usage & SEAM_VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        g_seam_track[g_seam_ntrack] = (struct seam_ent){ buf, size, usage, 1, NULL, NULL };
        g_seam_ntrack++;
        stepm("seam-buf-mailbox-cand", (long)(intptr_t)buf);
    } else if (size == SEAM_MARKER_BYTES &&
               (usage & SEAM_VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        g_seam_track[g_seam_ntrack] = (struct seam_ent){ buf, size, usage, 2, NULL, NULL };
        g_seam_ntrack++;
        stepm("seam-buf-marker-cand", (long)(intptr_t)buf);
    } else if (size == 3686400ull) {
        /* r60h STAGE 3: the seat's outq (feat's STORAGE output, the NPU input
         * bank's GPU-side producer) is exactly the input tensor size; identity
         * is confirmed later by pointer match against header->outq_ptr (the
         * mailbox header publishes the CPU mapping the seat itself took). */
        g_seam_track[g_seam_ntrack] = (struct seam_ent){ buf, size, usage, 5, NULL, NULL };
        g_seam_ntrack++;
        stepm("seam-buf-outq-cand", (long)(intptr_t)buf);
    }
}

// vkBindBufferMemory trampoline: associate memory with a tracked buffer
void seam_on_bind_buffer(int32_t result, void* buf, void* mem)
{
    struct seam_ent* e;
    if (result != 0) return;
    e = seam_find_buf(buf);
    if (e && !e->mem) e->mem = mem;
}

// vkMapMemory trampoline: capture the seat's mailbox host pointer and
// CONFIRM identity by magic (the signature alone is never the contract)
void seam_on_map_memory(int32_t result, void* mem, void** ppData)
{
    if (result != 0 || !ppData || !*ppData) return;
    for (int i = 0; i < g_seam_ntrack; i++) {
        struct seam_ent* e = &g_seam_track[i];
        if (e->mem != mem || e->kind != 1) continue;
        SeamHeader* h = (SeamHeader*)*ppData;
        if (h->magic == SEAM_MAILBOX_MAGIC) {
            e->map = *ppData;
            e->kind = 3;   // CONFIRMED mailbox
            stepm("seam-mailbox-confirmed", (long)(intptr_t)e->buf);
        } else {
            e->kind = 0;   // same size, wrong magic: not ours, stop tracking
            stepm("seam-mailbox-magic-miss", 0);
        }
        return;
    }
    for (int i = 0; i < g_seam_ntrack; i++) {
        struct seam_ent* e = &g_seam_track[i];
        if (e->mem != mem || e->kind != 5 || e->map) continue;
        /* r60h STAGE 3: outq candidate mapped — keep the pointer; the header
         * match in seam_outq_buf_by_ptr() is the identity contract. */
        e->map = *ppData;
        stepm("seam-buf-outq-mapped", (long)(intptr_t)e->buf);
        return;
    }
}

// r60h STAGE 3: resolve the seat's outq VkBuffer from the CPU pointer the
// seat published in the mailbox header (outq_ptr). 0 = not (yet) resolvable.
uint64_t seam_outq_buf_by_ptr(uint64_t outq_ptr)
{
    if (!outq_ptr) return 0;
    for (int i = 0; i < g_seam_ntrack; i++) {
        struct seam_ent* e = &g_seam_track[i];
        if (e->kind != 5 || !e->map) continue;
        if ((uint64_t)(uintptr_t)e->map == outq_ptr) return (uint64_t)(uintptr_t)e->buf;
    }
    return 0;
}

static struct seam_ent* seam_marker(void) {
    for (int i = 0; i < g_seam_ntrack; i++)
        if (g_seam_track[i].kind == 4) return &g_seam_track[i];
    return NULL;
}
static struct seam_ent* seam_mailbox(void) {
    for (int i = 0; i < g_seam_ntrack; i++)
        if (g_seam_track[i].kind == 3) return &g_seam_track[i];
    return NULL;
}

// The doorbell. Runs at RECORD TIME on the GAME/seat thread, inside the
// seat's vkCmdUpdateBuffer call (the seat is spinning on ack meanwhile).
// Returns the ack word decision; writes the ack itself (carrier build:
// WITHOUT the work bit — see header law).
int seam_on_cmd_update_buffer(void* cmdBuffer, void* dstBuffer,
                               uint64_t dstOffset, uint32_t dataSize,
                               const void* pData,
                               SeamHeader** out_h, uint64_t* out_seq)
{
    const uint64_t* db;
    uint64_t magic, seq;
    SeamHeader* h = NULL;
    int si = -1;

    (void)cmdBuffer;   // REV13e: magic is the ONLY authority —
                       // no marker-mailbox tracking required
    *out_h = NULL; *out_seq = 0;
    /* REV47 (PQ2, B1): call-rate counters + the 4096-call heartbeat dump. */
    if ((++g_seam_updcalls % 4096ull) == 0ull) {
        stepm("seam-upd-calls", (long)(g_seam_updcalls & 0xffffffff));
        stepm("seam-upd-doorbells", (long)(g_seam_doorbells & 0xffffffff));
        stepm("seam-upd-shapemiss", (long)(g_seam_shape_miss & 0xffffffff));
        stepm("seam-upd-badmagic", (long)(g_seam_badmagic & 0xffffffff));
        stepm("seam-upd-noslot", (long)(g_seam_noslot & 0xffffffff));
    }
    if (dstOffset != 0 || !pData || (dataSize != 16 && dataSize != 24)) {
        /* REV47 (PQ2, B1): throttled miss line +, when dstBuffer is a
         * TRACKED marker candidate (kind 2/4), the detail burst — settles
         * 'our doorbell arrives but parses wrong' vs 'never reaches the
         * hook'. db[0] read only when the data can hold it. */
        uint64_t m = ++g_seam_shape_miss;
        if ((m & 255ull) == 1ull) {
            struct seam_ent* e = seam_find_buf(dstBuffer);
            stepm("seam-doorbell-shape-miss", (long)dataSize);
            if (e && (e->kind == 2 || e->kind == 4)) {
                stepm("seam-miss-marker-dst", (long)(intptr_t)dstBuffer);
                stepm("seam-miss-size", (long)dataSize);
                if (pData && dataSize >= 8)
                    stepm("seam-miss-db0", (long)(uint32_t)((*(const uint64_t*)pData) & 0xffffffffu));
            }
        }
        return 0;
    }

    db = (const uint64_t*)pData;
    magic = db[0]; seq = dataSize == 24 ? db[1] : db[1];
    g_seam_doorbells++;
    if (magic != SEAM_DOORBELL_MAGIC) {
        g_seam_badmagic++;
        if ((g_seam_badmagic & 255ull) == 1ull) {   /* REV47: throttled 1-in-256 (was unthrottled) */
            struct seam_ent* e = seam_find_buf(dstBuffer);
            stepm("seam-badmagic", (long)magic);
            if (e && (e->kind == 2 || e->kind == 4)) {
                stepm("seam-miss-marker-dst", (long)(intptr_t)dstBuffer);
                stepm("seam-miss-size", (long)dataSize);
                stepm("seam-miss-db0", (long)(uint32_t)db[0]);
            }
        }
        return 0;
    }

    if (dataSize == 24) {
        /* REV13e: the doorbell carries the mailbox POINTER — same process */
        h = (SeamHeader*)(uintptr_t)db[2];
        if (h && h->magic != SEAM_MAILBOX_MAGIC) {
            stepm("seam-doorbell-ptr-magic-miss", (long)(intptr_t)h);
            h = NULL;
        }
    } else {
        struct seam_ent* mb = seam_mailbox();
        if (mb && mb->map) h = (SeamHeader*)mb->map;
    }
    if (!h) { stepm("seam-doorbell-no-mailbox", 0); return 0; }

    // find the PUBLISHED slot with this seq (seqlock acquire: state then seq)
    for (int i = 0; i < SEAM_SLOTS; i++) {
        SeamSlotBody* s = (SeamSlotBody*)((char*)h + SEAM_HEADER_BYTES + (uint64_t)i * SEAM_SLOT_BYTES);
        if (s->state == SEAM_SLOT_PUBLISHED && s->seq == (seq * 2ULL + 2ULL)) {
            // stable: even seq. Copy the frame out (the CONSUMING flip lets
            // the seat reclaim only after we are done — we are done now,
            // synchronous copy).
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            g_seam_last_frame = s->frame;
            g_seam_last_frame_seq = s->frame.frame_seq;
            g_seam_last_seq = seq;
            /* REV14: the copy is DONE (synchronous) — recycle to FREE so the
             * seat's ring never starves. CONSUMING would deadlock the ring at
             * 4 acks (observed: eval_ok=4 then all-busy fails). */
            s->state = SEAM_SLOT_FREE;
            __atomic_thread_fence(__ATOMIC_RELEASE);
            si = i;
            break;
        }
    }
    if (si < 0) { g_seam_noslot++; stepm("seam-doorbell-noslot", (long)seq); }

    // validated + copied: report back; the CALLER records the in-CB work
    // and then acks (with or without the work bit per what it recorded).
    *out_h = h; *out_seq = seq;
    return 1;
}

void seam_write_ack(SeamHeader* h, uint64_t seq, int work_attached)
{
    if (!h) return;
    // ACK — work bit set ONLY when real Output work was recorded into the
    // doorbell's command buffer (the zero-latency law, mechanical).
    // __atomic_store with RELEASE pairs with the seat's acquire spin.
    uint64_t a = seq | (work_attached ? SEAM_ACK_WORK_BIT : 0ull);
    __atomic_store_n(&h->ack, a, __ATOMIC_RELEASE);
    __atomic_fetch_add(&h->ack_count, 1, __ATOMIC_RELAXED);
    g_seam_acks++;
    if ((g_seam_acks % 500) == 1)
        stepm("seam-acks", (long)g_seam_acks);
}

/* REV35 ack-debug accessor: minimal_seam_layer.c sees only the opaque tag —
 * the run-1/2 contradiction (dispatches 2049 vs ack_count 1) needs the REAL
 * post-store counter readable from the doorbell tail. */
uint64_t seam_ack_count(const SeamHeader* h) { return h ? h->ack_count : 0; }

/* REV47 (PQ2, B1/L2): miss-path counters for the minimal layer's doorbell
 * heartbeat — the frozen-ack discriminator (doorbells climbing vs frozen). */
uint64_t seam_dbg_badmagic(void) { return g_seam_badmagic; }
uint64_t seam_dbg_doorbells(void) { return g_seam_doorbells; }
uint64_t seam_dbg_shapemiss(void) { return g_seam_shape_miss; }

// falsifier/diagnostic readback of the last copied frame (raw struct copy
// for the host to hash; NOT used in the game path)
const void* seam_last_frame(unsigned long long* frame_seq, unsigned long long* seq)
{
    if (frame_seq) *frame_seq = g_seam_last_frame_seq;
    if (seq) *seq = g_seam_last_seq;
    return (const void*)&g_seam_last_frame;
}

// REV28: the T_fsr4 NPU handshake — worker source/dest pointers, the
// counter addresses, and bank sizes. Returns 0 when wiring is off.
int seam_npu_handshake(SeamHeader* h, const void** outq, void** npudst,
                       volatile uint64_t** eval_seq, volatile uint64_t** npu_done,
                       uint64_t* outq_bytes, uint64_t* npudst_bytes)
{
    if (!h || h->magic != SEAM_MAILBOX_MAGIC) return 0;
    if (!h->outq_ptr || !h->npudst_ptr) return 0;
    if (outq)         *outq         = (const void*)(uintptr_t)h->outq_ptr;
    if (npudst)       *npudst       = (void*)(uintptr_t)h->npudst_ptr;
    if (eval_seq)     *eval_seq     = &h->eval_seq;
    if (npu_done)     *npu_done     = &h->npu_done;
    if (outq_bytes)   *outq_bytes   = h->outq_bytes;
    if (npudst_bytes) *npudst_bytes = h->npudst_bytes;
    return 1;
}

int seam_route_read(SeamHeader* h, uint64_t* buf, uint64_t* img, uint32_t* w, uint32_t* hgt)
{
    if (!h || h->magic != SEAM_MAILBOX_MAGIC) return 0;
    if (!h->route_buf || !h->route_img) return 0;
    if (buf) *buf = h->route_buf;
    if (img) *img = h->route_img;
    if (w) *w = h->route_os_w;
    if (hgt) *hgt = h->route_os_h;
    return 1;
}

// REV34 (ROUTE-V2): the stage-3 route-write objects — the probe VkDevice
// owning plan[3]'s descriptor set (the layer's per-eval vkUpdateDescriptorSets
// target for the STORAGE_IMAGE binding 8) and the set itself.
int seam_route_stage3(SeamHeader* h, uint64_t* dev, uint64_t* set)
{
    if (!h || h->magic != SEAM_MAILBOX_MAGIC) return 0;
    if (!h->route_dev || !h->route_set) return 0;
    if (dev) *dev = h->route_dev;
    if (set) *set = h->route_set;
    return 1;
}

void seam_npu_bump_eval(SeamHeader* h)
{
    if (h) __atomic_fetch_add(&h->eval_seq, 1, __ATOMIC_RELEASE);
}

// r59g: futex wake on eval_seq — the NPU worker sleeps in FUTEX_WAIT on this
// word instead of usleep(200) polling (5000 wakeups/s competing with the
// FEX-translated game for cores). Same-process word, PRIVATE flag valid.
void seam_npu_eval_wake(SeamHeader* h)
{
    if (h) syscall(98 /* SYS_futex, arm64 */, (const void*)&h->eval_seq,
                   129 /* FUTEX_WAKE_PRIVATE */, 1, 0, 0, 0);
}

uint64_t seam_npu_done(SeamHeader* h)
{
    return h ? __atomic_load_n(&h->npu_done, __ATOMIC_ACQUIRE) : 0;
}

// REV24/28: per-stage dispatch plan read (stage 0 feat / 1 postA / 2 RCAS /
// 3 route-write — REV34 ROUTE-V2). Returns the pipeline handle; 0 when not
// published.
uint64_t seam_comp_plan(int stage, SeamHeader* h, uint64_t* layout, uint64_t* set,
                        uint64_t* dispatch, uint32_t* dgz, uint32_t* push_bytes,
                        uint32_t* push_words /*[11]*/, uint32_t* patch)
{
    SeamPlan* p;
    if (!h || h->magic != SEAM_MAILBOX_MAGIC || stage < 0 || stage > 3) return 0;
    p = (SeamPlan*)&h->plan[stage];
    uint64_t pipe_ = __atomic_load_n(&p->comp_pipe, __ATOMIC_ACQUIRE);
    if (!pipe_) return 0;
    if (layout)   *layout   = __atomic_load_n(&p->comp_layout, __ATOMIC_ACQUIRE);
    if (set)      *set      = __atomic_load_n(&p->comp_set,    __ATOMIC_ACQUIRE);
    if (dispatch) *dispatch = __atomic_load_n(&p->comp_dispatch, __ATOMIC_ACQUIRE);
    uint64_t dp = __atomic_load_n(&p->comp_dgz_push, __ATOMIC_ACQUIRE);
    if (dgz)         *dgz         = (uint32_t)dp;
    if (push_bytes)  *push_bytes  = (uint32_t)(dp >> 32);
    if (push_words) { int i; for (i = 0; i < 11; i++) push_words[i] = p->comp_push[i]; }
    if (patch)       *patch       = p->comp_patch;
    return pipe_;
}
