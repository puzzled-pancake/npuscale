// qnn_glue.c — M6a: the NPU probe INSIDE the game process.
// Ports the proven qnn_memrun.c sequence (phase1, RUNBOOK E4 "myth busted:
// libQnnHtp.so works fine in our own process") into the seam LAYER, so the
// Hexagon HTP is reached from the wine/box64 game-shaped process itself:
//   libcdsprpc (rpcmem) -> libQnnSystem (binaryInfo) -> libQnnHtp
//   (backend/device/contextCreateFromBinary on the production context
//   bin_cfnhwc.bin) -> rpcmem I/O registered as MEMHANDLE -> timed
//   graphExecute.
// Everything stays alive (no teardown — process exit cleans; the law).
// Every step is stepm-marked q6-* so a silent death names its exact site.
//
// ARMADA PORT (2026-10-09, plan §4.2 + the M3 pass record qnn/README.md):
//  - QNN dir: FSR4NPU_QNN_DIR env, default /var/home/armada/.local/share/
//    fsr4npu/qnn (the M3-proven bundle: patched libQnnHtp 11f83855…, stub,
//    system, skel, libcdsprpc with the ADSP_LIBRARY_PATH→DSP_LIBS_LOCATION
//    bridge, shell + DSP libc++ — qnn/dsp/ staging).
//  - libcdsprpc dlopened by ABSOLUTE path from the bundle (no vendor copy
//    exists on Armada — plan §5.2 inversion of the Android E0 law).
//  - ADSP_LIBRARY_PATH separator normalized to ';' (a surviving ':' breaks
//    the skel lookup like an fastrpc failure — plan §4.2).
//  - the M3-proven pre-steps from tools/m3_probe.c (reference
//    implementation): remote_session_control UNSIGNED_MODULE, the skel
//    selftest URI open, libatomic pre-load, and the V73 stub RTLD_GLOBAL
//    pre-load (HTP dlopens the stub by bare name; LD_LIBRARY_PATH is
//    loader-cached at process start, so setenv cannot fix it in-game).
//  - VOTE = the M3 attempt-3 winner, FIXED (mask=3 recipe): DCVS_V3 with
//    dcvsEnable=0, core+bus corners pinned 0xA0 (all three min/target/
//    max), sleepDisable, sleepLatency 40, PLUS RPC_CONTROL_LATENCY=100,
//    and NO RPC_POLLING_TIME (this AYN cdsp.mbn DSP-OS REFUSES the
//    polling option — QAIRT err 13006/0x32CE poisons the whole batch).
//    A setPowerConfig failure is NON-FATAL (loud q6-warn marker, keep
//    going) exactly like tools/m3_probe.c:430-438 — timing then shows
//    what held (unvoted class on this DSP-OS ≈ 9.4 ms, voted 3.17 ms).
#define _GNU_SOURCE   /* r60: cpu_set_t / sched_setaffinity */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>

#include "QnnInterface.h"
#include "System/QnnSystemInterface.h"
#include "QnnMem.h"
#include "HTP/QnnHtpDevice.h"
#include "HTP/QnnHtpPerfInfrastructure.h"

#ifndef SEAM_LOG_DIR
#define SEAM_LOG_DIR "/var/home/armada/fsr4npu-logs"
#endif

typedef QNN_INTERFACE_VER_TYPE     q6_iface_t;
typedef QNN_SYSTEM_INTERFACE_VER_TYPE q6_sysiface_t;
typedef Qnn_ErrorHandle_t (*q6_getproviders_fn)(const QnnInterface_t***, uint32_t*);
typedef Qnn_ErrorHandle_t (*q6_sysgetproviders_fn)(const QnnSystemInterface_t***, uint32_t*);

typedef void* (*q6_rpcmem_alloc_fn)(int, uint32_t, int);
typedef int   (*q6_rpcmem_tofd_fn)(void*);
typedef int   (*q6_remote_session_control_fn)(uint32_t, void*, uint32_t);
typedef int   (*q6_remote_handle64_open_fn)(const char*, uint64_t*);

/* the graph name captured at binaryInfo time (graphRetrieve happens after
 * the sysCtx free — keep it here) */
static char q6_graph_name_keep[128];

/* thread entry: run the probe against the bundle dir — FSR4NPU_QNN_DIR if
 * set (the M3 env recipe), else the Armada default
 * /var/home/armada/.local/share/fsr4npu/qnn (plan §4.2 replaces the
 * Android app-private run-as path) */
int qnn_glue_probe(const char* dir);
void* q6_probe_thread_shim(void* unused)
{
    const char* dir = getenv("FSR4NPU_QNN_DIR");
    if (!dir || !*dir) dir = "/var/home/armada/.local/share/fsr4npu/qnn";
    (void)unused;
    prctl(PR_SET_NAME, "fsr4npu-probe", 0, 0, 0);   /* r61: every thread reads the game exe otherwise */
    qnn_glue_probe(dir);
    return NULL;
}


volatile int q6_probe_ok = 0;   /* set at PROBE-PASS */

static void q6m(const char* step, long v)
{
#ifdef SEAM_LOG
    FILE* f = fopen(SEAM_LOG_DIR "/fsr4npu-SEAM.log", "a");
    if (f) { fprintf(f, "q6-%s = %ld (pid %d)\n", step, v, (int)getpid()); fclose(f); }
#else
    (void)step; (void)v;
#endif
}

static double q6_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* r61 beast-#2 instrumentation (round-12 plan, oracle B): worker state the
 * wedge forensics + heartbeat read at dump time. wait codes: 0 idle in the
 * eval futex wait, 1 input bridge, 2 INSIDE graphExecute, 3 output/rec/
 * publish. enter>exit at a stall = the DSP never returned (hypothesis C);
 * code==0 with frozen evals = evals stopped arriving, upstream of the NPU
 * (hypotheses A/B). */
volatile int q6_wait_code = -1;
volatile unsigned long long q6_exec_enter_seq = 0, q6_exec_exit_seq = 0;
volatile unsigned long long q6_pub_consumed = 0;
volatile double q6_exec_start_ms = 0;

static size_t q6_dtype_size(Qnn_DataType_t dt)
{
    switch (dt) {
        case QNN_DATATYPE_INT_8: case QNN_DATATYPE_UINT_8:
        case QNN_DATATYPE_SFIXED_POINT_8: case QNN_DATATYPE_UFIXED_POINT_8:
            return 1;
        case QNN_DATATYPE_INT_16: case QNN_DATATYPE_UINT_16:
        case QNN_DATATYPE_SFIXED_POINT_16: case QNN_DATATYPE_UFIXED_POINT_16:
        case QNN_DATATYPE_FLOAT_16: case QNN_DATATYPE_BFLOAT_16:
            return 2;
        case QNN_DATATYPE_INT_32: case QNN_DATATYPE_UINT_32:
        case QNN_DATATYPE_FLOAT_32:
            return 4;
        default: return 0;
    }
}
static const char* q6_tname(const Qnn_Tensor_t* t)  { return t->version == QNN_TENSOR_VERSION_1 ? t->v1.name : t->v2.name; }
static uint32_t    q6_trank(const Qnn_Tensor_t* t)  { return t->version == QNN_TENSOR_VERSION_1 ? t->v1.rank : t->v2.rank; }
static const uint32_t* q6_tdims(const Qnn_Tensor_t* t) { return t->version == QNN_TENSOR_VERSION_1 ? t->v1.dimensions : t->v2.dimensions; }
static Qnn_DataType_t q6_tdtype(const Qnn_Tensor_t* t) { return t->version == QNN_TENSOR_VERSION_1 ? t->v1.dataType : t->v2.dataType; }
static size_t q6_tbytes(const Qnn_Tensor_t* t)
{
    size_t n = q6_dtype_size(q6_tdtype(t));
    for (uint32_t i = 0; i < q6_trank(t); i++) n *= q6_tdims(t)[i];
    return n;
}

/* everything kept alive in statics — the falsifier process exits soon after */
static void *q6_rpc_lib, *q6_sys_lib, *q6_be_lib;
static q6_rpcmem_alloc_fn q6_alloc;
static q6_rpcmem_tofd_fn  q6_tofd;
static q6_iface_t  q6_iface;
static Qnn_BackendHandle_t q6_backend;
static Qnn_DeviceHandle_t  q6_device;
static Qnn_ContextHandle_t q6_context;
static Qnn_GraphHandle_t   q6_graph;
static void *q6_ctx_buf;
static long  q6_ctx_size;
static Qnn_Tensor_t* q6_in_info;  static uint32_t q6_n_in;
static Qnn_Tensor_t* q6_out_info; static uint32_t q6_n_out;
static void* q6_in_ptrs[8]; static int q6_in_fds[8]; static size_t q6_in_bytes[8];
static void* q6_out_ptrs[8]; static int q6_out_fds[8]; static size_t q6_out_bytes[8];

/* ---- RATIO ERA: the tier hot-swap ----
 * The seat detects the XeSS preset's ratio per frame (AMD's thresholds) and
 * publishes the active tier in NmFrame.reserved_u[0]; seam_nms's record path
 * calls q6_set_tier(); the WORKER swaps between executes. All five tiers
 * share tensor shapes + topology (weights/scales only — the extraction's
 * fact 2), so the swap reuses the registered rpcmem IO and only does:
 * contextFree(old) -> read bin -> contextCreateFromBinary -> graphRetrieve.
 * On load failure: keep the last-good context (log loud). On EXECUTE
 * failure after a swap: one revert to tier 3 (perf, the deployed class). */
static const char* const Q6_TIER_BIN[5] = {
    "bin_cfnhwc_native.bin", "bin_cfnhwc_quality.bin", "bin_cfnhwc_balanced.bin",
    "bin_cfnhwc_perf.bin", "bin_cfnhwc_ultraperf.bin",
};

/* ---- r62 VOLTAGE KNOB (fsr4npu-VCORNER): the HTP DCVS corner vote ----
 * Ladder (QnnHtpPerfInfrastructure.h): 0x20 MIN / 0x30 SVS2 / 0x40 SVS /
 * 0x50 SVS+ / 0x60 NOM / 0x70 NOM+ / 0x80 TURBO / 0x90 TURBO+ / 0xA0 MAX.
 * Live: the worker re-reads 1x/s and re-votes between executes. */
static QnnHtpDevice_PerfInfrastructure_t* q6_perf;
static uint32_t q6_power_id;
static uint32_t q6_read_vcorner(void)
{
    uint32_t vc = 0xA0;   /* DCVS_VOLTAGE_VCORNER_MAX — the M3 winner */
    FILE* f = fopen(SEAM_LOG_DIR "/fsr4npu-VCORNER", "r");
    if (f) {
        char ln[64];
        if (fgets(ln, sizeof ln, f)) {
            unsigned long v = strtoul(ln, NULL, 0);
            if (v >= 0x20 && v <= 0xA0) vc = (uint32_t)v;
        }
        fclose(f);
    }
    return vc;
}
/* re-vote the mask=3 recipe at a new corner (worker-thread timing, between
 * executes; a failure is loud + non-fatal — the old vote stays in force) */
static void q6_vote_corner(uint32_t vc)
{
    QnnHtpPerfInfrastructure_PowerConfig_t dcvs, rpc_lat;
    const QnnHtpPerfInfrastructure_PowerConfig_t* cfgs[3];
    if (!q6_perf || !q6_power_id) return;
    memset(&dcvs, 0, sizeof dcvs);
    memset(&rpc_lat, 0, sizeof rpc_lat);
    dcvs.option = QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_DCVS_V3;
    dcvs.dcvsV3Config.contextId = q6_power_id;
    dcvs.dcvsV3Config.setDcvsEnable = 1;
    dcvs.dcvsV3Config.dcvsEnable = 0;          /* fixed corner, no DCVS */
    dcvs.dcvsV3Config.setSleepDisable = 1;
    dcvs.dcvsV3Config.sleepDisable = 1;
    dcvs.dcvsV3Config.setSleepLatency = 1;
    dcvs.dcvsV3Config.sleepLatency = 40;
    dcvs.dcvsV3Config.setBusParams = 1;
    dcvs.dcvsV3Config.busVoltageCornerMin = vc;
    dcvs.dcvsV3Config.busVoltageCornerTarget = vc;
    dcvs.dcvsV3Config.busVoltageCornerMax = vc;
    dcvs.dcvsV3Config.setCoreParams = 1;
    dcvs.dcvsV3Config.coreVoltageCornerMin = vc;
    dcvs.dcvsV3Config.coreVoltageCornerTarget = vc;
    dcvs.dcvsV3Config.coreVoltageCornerMax = vc;
    rpc_lat.option = QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_RPC_CONTROL_LATENCY;
    rpc_lat.rpcControlLatencyConfig = 100;
    cfgs[0] = &dcvs; cfgs[1] = &rpc_lat; cfgs[2] = NULL;
    if (q6_perf->setPowerConfig(q6_power_id, cfgs) == 0)
        q6m("vcorner-vote", (long)vc);
    else
        q6m("vcorner-vote-fail", (long)vc);
}
static void q6_maybe_revote(void)
{
    static uint32_t cur = 0xA0;
    static unsigned long long last;
    unsigned long long now = (unsigned long long)time(NULL);
    if (now == last) return;                    /* 1x/s max */
    last = now;
    {   uint32_t vc = q6_read_vcorner();
        if (vc != cur) { q6_vote_corner(vc); cur = vc; } }
}
static char q6_dir_keep[512];
static q6_sysiface_t q6_sysif_keep;      /* for binaryInfo (graph name) at swap time */
static volatile int q6_tier_req = -1;
static int q6_tier_cur = -1;
static int q6_tier_good = -1;          /* last tier that executed cleanly */
static unsigned q6_tier_blacklist;     /* bit per tier: load- or execute-failed this session */
void q6_set_tier(int tier) { if (tier >= 0 && tier < 5) q6_tier_req = tier; }

/* memRegister is CONTEXT-scoped (the 2026-10-11 first-contact lesson: the
 * balanced swap loaded + retrieved fine, then EVERY post-swap graphExecute
 * failed — including the perf revert, a THIRD context — because the tensor
 * memHandles still named the ORIGINAL context; the worker died at loop
 * ~3000 and the image froze on mismatched weights). This re-registers the
 * SAME rpcmem allocations (the fds stay ours) against a new context.
 * ALL-OR-NOTHING: handles are staged and only patched into the tensors
 * when every registration succeeded, so a failure leaves the live context
 + fully intact. */
static int q6_rebind_one(Qnn_ContextHandle_t ctx, const Qnn_Tensor_t* t, int fd,
                         Qnn_MemHandle_t* out_handle)
{
    Qnn_MemDescriptor_t desc = QNN_MEM_DESCRIPTOR_INIT;
    desc.memShape.numDim = q6_trank(t);
    desc.memShape.dimSize = (uint32_t*)q6_tdims(t);
    desc.memShape.shapeConfig = NULL;
    desc.dataType = q6_tdtype(t);
    desc.memType = QNN_MEM_TYPE_ION;
    desc.ionInfo.fd = fd;
    *out_handle = NULL;
    if (q6_iface.memRegister(ctx, &desc, 1, out_handle) != 0 || !*out_handle) return -3;
    return 0;
}

static int q6_load_context_bin(int tier)
{
    char path[600], gname[128];
    FILE* f;
    long sz;
    void* buf;
    Qnn_ContextHandle_t nctx = NULL;
    Qnn_GraphHandle_t ngraph = NULL;
    if (tier < 0 || tier > 4 || !q6_dir_keep[0]) return 0;
    snprintf(path, sizeof path, "%s/%s", q6_dir_keep, Q6_TIER_BIN[tier]);
    f = fopen(path, "rb");
    if (!f) { q6m("tier-bin-open", tier); return 0; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(buf); q6m("tier-bin-read", tier); return 0;
    }
    fclose(f);
    /* the graph NAME differs per tier (_qdq_clipfree_<t>_640_nhwc_f16):
     * binaryInfo on the new blob via the (already-loaded) system lib */
    gname[0] = 0;
    {
        QnnSystemContext_Handle_t sysCtx = NULL;
        const QnnSystemContext_BinaryInfo_t* bi = NULL;
        Qnn_ContextBinarySize_t bis = 0;
        if (q6_sysif_keep.systemContextCreate &&
            q6_sysif_keep.systemContextCreate(&sysCtx) == 0 && sysCtx &&
            q6_sysif_keep.systemContextGetBinaryInfo(sysCtx, buf, (uint32_t)sz, &bi, &bis) == 0 && bi) {
            const QnnSystemContext_GraphInfo_t* graphs = NULL; uint32_t ng = 0;
            switch (bi->version) {
                case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1: graphs = bi->contextBinaryInfoV1.graphs; ng = bi->contextBinaryInfoV1.numGraphs; break;
                case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2: graphs = bi->contextBinaryInfoV2.graphs; ng = bi->contextBinaryInfoV2.numGraphs; break;
                case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3: graphs = bi->contextBinaryInfoV3.graphs; ng = bi->contextBinaryInfoV3.numGraphs; break;
                default: break;
            }
            if (graphs && ng >= 1) snprintf(gname, sizeof gname, "%s", graphs[0].graphInfoV1.graphName);
            if (sysCtx && q6_sysif_keep.systemContextFree) q6_sysif_keep.systemContextFree(sysCtx);
        }
        if (!gname[0]) { free(buf); q6m("tier-bininfo", tier); return 0; }
    }
    if (q6_iface.contextCreateFromBinary(q6_backend, q6_device, NULL, buf, (uint32_t)sz, &nctx, NULL) != 0 || !nctx) {
        free(buf); q6m("tier-ctx-create", tier); return 0;
    }
    if (q6_iface.graphRetrieve(nctx, gname, &ngraph) != 0 || !ngraph) {
        if (q6_iface.contextFree) q6_iface.contextFree(nctx, NULL);
        free(buf); q6m("tier-graph-retrieve", tier); return 0;
    }
    /* rebind the registered IO to the NEW context (staged all-or-nothing —
     * a mid-way failure frees the new context, whose partial registrations
     * die with it, leaving the live context + its handles untouched) */
    {
        Qnn_MemHandle_t nh[16];
        int r = 0;
        uint32_t i;
        memset(nh, 0, sizeof nh);
        for (i = 0; i < q6_n_in && !r; i++)
            r = q6_rebind_one(nctx, &q6_in_info[i], q6_in_fds[i], &nh[i]);
        for (i = 0; i < q6_n_out && !r; i++)
            r = q6_rebind_one(nctx, &q6_out_info[i], q6_out_fds[i], &nh[8 + i]);
        if (r) {
            if (q6_iface.contextFree) q6_iface.contextFree(nctx, NULL);
            free(buf); q6m("tier-rebind", (long)r); return 0;
        }
        for (i = 0; i < q6_n_in; i++) {
            if (q6_in_info[i].version == QNN_TENSOR_VERSION_1) q6_in_info[i].v1.memHandle = nh[i];
            else q6_in_info[i].v2.memHandle = nh[i];
        }
        for (i = 0; i < q6_n_out; i++) {
            if (q6_out_info[i].version == QNN_TENSOR_VERSION_1) q6_out_info[i].v1.memHandle = nh[8 + i];
            else q6_out_info[i].v2.memHandle = nh[8 + i];
        }
    }
    /* commit: old context stays live until the new one is complete */
    if (q6_context && q6_iface.contextFree) q6_iface.contextFree(q6_context, NULL);
    q6_context = nctx;
    q6_graph = ngraph;
    snprintf(q6_graph_name_keep, sizeof q6_graph_name_keep, "%s", gname);
    free(q6_ctx_buf); q6_ctx_buf = buf; q6_ctx_size = sz;
    return 1;
}
/* worker loop-top check (single-threaded with graphExecute — swap-safe) */
static int q6_pick_fallback(void)
{
    int t;
    if (q6_tier_good >= 0 && !(q6_tier_blacklist & (1u << q6_tier_good))) return q6_tier_good;
    if (!(q6_tier_blacklist & (1u << 3))) return 3;    /* the proven boot bin */
    for (t = 0; t < 5; t++) if (!(q6_tier_blacklist & (1u << t))) return t;
    return -1;
}
static void q6_tier_maybe_swap(void)
{
    int want = q6_tier_req;
    if (want < 0 || want == q6_tier_cur) return;
    if (q6_tier_blacklist & (1u << want)) return;      /* failed this session */
    q6m("tier-swap-req", want);
    if (q6_load_context_bin(want)) {
        q6_tier_cur = want;
        q6m("tier-swap-ok", want);
    } else {
        q6_tier_blacklist |= 1u << want;               /* never thrash a bad load */
        q6m("tier-swap-blacklist", want);
    }
}

static Qnn_Tensor_t* q6_copy_tensors(const Qnn_Tensor_t* src, uint32_t n)
{
    Qnn_Tensor_t* dst = (Qnn_Tensor_t*)calloc(n, sizeof(Qnn_Tensor_t));
    for (uint32_t i = 0; i < n; i++) {
        const char* name = q6_tname(&src[i]);
        uint32_t rank = q6_trank(&src[i]);
        dst[i] = src[i];
        char* nc = (char*)malloc(strlen(name) + 1); strcpy(nc, name);
        uint32_t* dm = (uint32_t*)malloc(rank * sizeof(uint32_t)); memcpy(dm, q6_tdims(&src[i]), rank * sizeof(uint32_t));
        if (dst[i].version == QNN_TENSOR_VERSION_1) { dst[i].v1.name = nc; dst[i].v1.dimensions = dm; }
        else                                       { dst[i].v2.name = nc; dst[i].v2.dimensions = dm; }
    }
    return dst;
}

static int q6_register_io(int idx, Qnn_Tensor_t* t, int is_in)
{
    size_t nbytes = q6_tbytes(t);
    void* ptr = q6_alloc(25 /*RPCMEM_HEAP_ID_SYSTEM*/, 1 /*cached*/, (int)nbytes);
    if (!ptr) return -1;
    int fd = q6_tofd(ptr);
    if (fd < 0) return -2;
    Qnn_MemDescriptor_t desc = QNN_MEM_DESCRIPTOR_INIT;
    desc.memShape.numDim = q6_trank(t);
    desc.memShape.dimSize = (uint32_t*)q6_tdims(t);
    desc.memShape.shapeConfig = NULL;
    desc.dataType = q6_tdtype(t);
    desc.memType = QNN_MEM_TYPE_ION;
    desc.ionInfo.fd = fd;
    Qnn_MemHandle_t handle = NULL;
    if (q6_iface.memRegister(q6_context, &desc, 1, &handle) != 0 || !handle) return -3;
    if (t->version == QNN_TENSOR_VERSION_1) {
        t->v1.memType = QNN_TENSORMEMTYPE_MEMHANDLE; t->v1.memHandle = handle;
    } else {
        t->v2.memType = QNN_TENSORMEMTYPE_MEMHANDLE; t->v2.memHandle = handle;
    }
    if (is_in)  { q6_in_ptrs[idx] = ptr;  q6_in_fds[idx] = fd;  q6_in_bytes[idx] = nbytes; }
    else        { q6_out_ptrs[idx] = ptr; q6_out_fds[idx] = fd; q6_out_bytes[idx] = nbytes; }
    return 0;
}

/* the probe: dir holds libQnnSystem.so, libQnnHtp.so, libQnnHtpV73{Stub,Skel}.so
 * and bin_cfnhwc.bin (the production context). Returns 1 when the NPU has
 * EXECUTED the production graph inside this process. */
int qnn_glue_probe(const char* dir)
{
    char path[512];
    long t_start = (long)q6_now_ms();
    snprintf(q6_dir_keep, sizeof q6_dir_keep, "%s", dir);   /* tier swaps read here */

    /* the DSP-side loader searches ADSP_LIBRARY_PATH for the shell + skel.
     * ARMADA: ';' separator (plan §4.2) and any inherited ':' normalized
     * away — the skel lookup fails on a ':' in a way indistinguishable
     * from an fastrpc failure. LD_LIBRARY_PATH best-effort (r25k — the
     * loader caches it at process start; the stub pre-load below is the
     * reliable mechanism). */
    {
        const char* cur = getenv("ADSP_LIBRARY_PATH");
        char nv[2048];
        size_t o = (size_t)snprintf(nv, sizeof nv, "%s;", dir);
        if (cur && *cur) {
            for (const char* p = cur; *p && o + 1 < sizeof nv; p++)
                nv[o++] = (*p == ':') ? ';' : *p;
            nv[o] = 0;
        }
        setenv("ADSP_LIBRARY_PATH", nv, 1);
        setenv("LD_LIBRARY_PATH", dir, 1);   /* best-effort; loader may have cached */
        q6m("adsp-path-set", 1);
    }

    /* ARMADA: our own libcdsprpc from the bundle, ABSOLUTE path (no vendor
     * copy exists on this device — plan §4.2/:188). */
    snprintf(path, sizeof path, "%s/libcdsprpc.so", dir);
    q6_rpc_lib = dlopen(path, 2 /*RTLD_NOW|RTLD_LOCAL*/);
    q6m("rpc-lib", (long)(intptr_t)q6_rpc_lib);
    if (!q6_rpc_lib) return 0;
    q6_alloc = (q6_rpcmem_alloc_fn)dlsym(q6_rpc_lib, "rpcmem_alloc");
    q6_tofd  = (q6_rpcmem_tofd_fn)dlsym(q6_rpc_lib, "rpcmem_to_fd");
    {
        q6_remote_session_control_fn q6_rsc =
            (q6_remote_session_control_fn)(void*)dlsym(q6_rpc_lib, "remote_session_control");
        q6_remote_handle64_open_fn q6_rho =
            (q6_remote_handle64_open_fn)(void*)dlsym(q6_rpc_lib, "remote_handle64_open");
        q6m("rpc-fns", (q6_alloc && q6_tofd && q6_rsc && q6_rho) ? 1 : 0);
        /* M3-proven pre-steps (tools/m3_probe.c:259-274): unsigned-PD via
         * remote_SESSION_control req 2 (NOT remote_handle_control — req 2
         * via the latter silently no-ops), then the skel selftest URI open
         * with the REQUIRED ?..._handle_invoke&_modver=1.0&_dom=cdsp
         * suffix. Non-fatal marks — the QNN session below decides. */
        if (q6_rsc) {
            struct { int domain; int enable; } um = { 3, 1 };   /* 3 = CDSP */
            int rcu = q6_rsc(2 /*DSPRPC_CONTROL_UNSIGNED_MODULE*/, &um, sizeof um);
            q6m("session-control-unsignedpd", rcu);
        }
        if (q6_rho) {
            uint64_t rh = 0;
            int rc2 = q6_rho("file:///libQnnHtpV73Skel.so?qnn_2_50_0_skel_handle_invoke&_modver=1.0&_dom=cdsp", &rh);
            q6m("skel-selftest-open", rc2);
        }
    }
    if (!q6_alloc || !q6_tofd) return 0;

    /* libatomic pre-load (libQnnSystem DT_NEEDED libatomic.so.1,
     * LIBATOMIC_1.0 — tools/m3_probe.c:249-257): system copy first, then
     * the bundle dir. */
    {
        void* lah = dlopen("libatomic.so.1", 2 /*RTLD_NOW*/ | 0x100 /*RTLD_GLOBAL*/);
        if (!lah) { snprintf(path, sizeof path, "%s/libatomic.so.1", dir); lah = dlopen(path, 2 | 0x100); }
        q6m("libatomic-preload", lah ? 1 : 0);
    }

    /* system library (binaryInfo) */
    snprintf(path, sizeof path, "%s/libQnnSystem.so", dir);
    q6_sys_lib = dlopen(path, 2);
    q6m("sys-lib", (long)(intptr_t)q6_sys_lib);
    if (!q6_sys_lib) return 0;
    q6_sysgetproviders_fn sgp = (q6_sysgetproviders_fn)dlsym(q6_sys_lib, "QnnSystemInterface_getProviders");
    q6m("sys-getproviders-fn", (long)(intptr_t)sgp);
    if (!sgp) return 0;
    {
        QnnSystemInterface_t** sp = NULL; uint32_t n = 0;
        if (sgp((const QnnSystemInterface_t***)&sp, &n) != 0 || !n) { q6m("sys-getproviders", -1); return 0; }
        q6_sysiface_t si = sp[0]->QNN_SYSTEM_INTERFACE_VER_NAME;
        q6_sysif_keep = si;                     /* the tier swap re-does binaryInfo */
        QnnSystemContext_Handle_t sysCtx = NULL;
        if (si.systemContextCreate(&sysCtx) != 0) { q6m("sys-ctx-create", -1); return 0; }
        q6m("sys-ctx-create", 1);

        snprintf(path, sizeof path, "%s/bin_cfnhwc.bin", dir);
        FILE* f = fopen(path, "rb");
        if (!f) { q6m("ctx-file", 0); return 0; }
        fseek(f, 0, SEEK_END); q6_ctx_size = ftell(f); fseek(f, 0, SEEK_SET);
        q6_ctx_buf = malloc((size_t)q6_ctx_size);
        if (fread(q6_ctx_buf, 1, (size_t)q6_ctx_size, f) != (size_t)q6_ctx_size) { fclose(f); q6m("ctx-read", -1); return 0; }
        fclose(f);
        q6m("ctx-read", q6_ctx_size);

        const QnnSystemContext_BinaryInfo_t* bi = NULL;
        Qnn_ContextBinarySize_t bis = 0;
        if (si.systemContextGetBinaryInfo(sysCtx, q6_ctx_buf, (uint32_t)q6_ctx_size, &bi, &bis) != 0 || !bi) { q6m("bininfo", -1); return 0; }
        const QnnSystemContext_GraphInfo_t* graphs = NULL; uint32_t ng = 0;
        switch (bi->version) {
            case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1: graphs = bi->contextBinaryInfoV1.graphs; ng = bi->contextBinaryInfoV1.numGraphs; break;
            case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2: graphs = bi->contextBinaryInfoV2.graphs; ng = bi->contextBinaryInfoV2.numGraphs; break;
            case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3: graphs = bi->contextBinaryInfoV3.graphs; ng = bi->contextBinaryInfoV3.numGraphs; break;
            default: q6m("bininfo-version", -1); return 0;
        }
        if (!graphs || ng < 1) { q6m("bininfo-graphs", 0); return 0; }
        const char* graph_name = graphs[0].graphInfoV1.graphName;
        snprintf(q6_graph_name_keep, sizeof q6_graph_name_keep, "%s", graph_name);
        q6_n_in  = graphs[0].graphInfoV1.numGraphInputs;
        q6_n_out = graphs[0].graphInfoV1.numGraphOutputs;
        q6_in_info  = q6_copy_tensors(graphs[0].graphInfoV1.graphInputs, q6_n_in);
        q6_out_info = q6_copy_tensors(graphs[0].graphInfoV1.graphOutputs, q6_n_out);
        si.systemContextFree(sysCtx);
        q6m("bininfo-ok", (long)q6_n_in * 100 + q6_n_out);
        /* graph name hash for the log (name itself not stepm-able) */
        { unsigned h = 0; const char* s = graph_name; while (*s) h = h * 31u + (unsigned)*s++; q6m("graph-name-hash", (long)h); }
    }

    /* backend library */
    snprintf(path, sizeof path, "%s/libQnnHtp.so", dir);
    q6_be_lib = dlopen(path, 2);
    q6m("be-lib", (long)(intptr_t)q6_be_lib);
    if (!q6_be_lib) return 0;
    /* r25h/M3 stub pre-load: HTP dlopens libQnnHtpV73Stub.so by BARE name
     * at session time; LD_LIBRARY_PATH is loader-cached at process start,
     * so the explicit RTLD_GLOBAL pre-load is the reliable leg
     * (tools/m3_probe.c:341-346). */
    {
        snprintf(path, sizeof path, "%s/libQnnHtpV73Stub.so", dir);
        void* st = dlopen(path, 2 /*RTLD_NOW*/ | 0x100 /*RTLD_GLOBAL*/);
        q6m("stub-preload", st ? 1 : 0);
    }
    q6_getproviders_fn gp = (q6_getproviders_fn)dlsym(q6_be_lib, "QnnInterface_getProviders");
    q6m("be-getproviders-fn", (long)(intptr_t)gp);
    if (!gp) return 0;
    {
        QnnInterface_t** prov = NULL; uint32_t n = 0;
        if (gp((const QnnInterface_t***)&prov, &n) != 0 || !n) { q6m("be-getproviders", -1); return 0; }
        QnnInterface_t* matched = prov[0];
        for (uint32_t i = 0; i < n; i++)
            if (prov[i]->apiVersion.coreApiVersion.major == QNN_API_VERSION_MAJOR &&
                prov[i]->apiVersion.coreApiVersion.minor >= QNN_API_VERSION_MINOR)
                matched = prov[i];
        q6_iface = matched->QNN_INTERFACE_VER_NAME;
        q6m("be-iface", 1);
    }
    {
        Qnn_LogHandle_t log = NULL;
        if (q6_iface.logCreate(NULL, QNN_LOG_LEVEL_ERROR, &log) != 0) { q6m("log-create", -1); return 0; }
        if (q6_iface.backendCreate(log, NULL, &q6_backend) != 0) { q6m("backend-create", -1); return 0; }
        q6m("backend-create", 1);
        if (q6_iface.deviceCreate(log, NULL, &q6_device) != 0) { q6m("device-create", -1); return 0; }
        q6m("device-create", 1);
    }

    /* ARMADA VOTE (M3 attempt-3 winner, FIXED — the "mask=3" recipe,
     * tools/m3_probe.c reference + qnn/README.md "M3 attempt 3"):
     *   DCVS_V3  : dcvsEnable=0 (corners pinned, not DCVS), core+bus at the
     *              VCORNER knob's value (0xA0 MAX = the M3 winner) at
     *              min/target/max, sleepDisable, sleepLatency 40
     *   RPC_CONTROL_LATENCY = 100
     *   NO RPC_POLLING_TIME — this AYN cdsp.mbn DSP-OS REFUSES the polling
     *   option (QAIRT 13006 = 0x32CE) and it poisons the whole batch;
     *   DCVS_V3 and RPC_CONTROL_LATENCY are accepted singly AND together.
     * Without the vote the class is ~9.4 ms on this DSP-OS; voted 3.17 ms.
     * A setPowerConfig failure is NON-FATAL (M3R3 decouple): loud warn
     * marker, continue to contextCreateFromBinary — timing shows what
     * held. Do NOT return 0 here.
     * r62 VOLTAGE KNOB: ~/fsr4npu-logs/fsr4npu-VCORNER (hex corner, e.g.
     * 0x60 NOM / 0x70 NOM+ / 0x80 TURBO / 0xA0 MAX default) — read at
     * probe AND re-read 1x/s in the worker (q6_maybe_revote); the worker
     * is async (+2) with a full frame budget of headroom, so lower corners
     * trade exec latency for battery. */
    {
        QnnDevice_Infrastructure_t dev_infra = NULL;
        if (q6_iface.deviceGetInfrastructure(&dev_infra) != 0 || !dev_infra) { q6m("dev-infra", -1); return 0; }
        QnnHtpDevice_Infrastructure_t* htp_infra = (QnnHtpDevice_Infrastructure_t*)dev_infra;
        if (htp_infra->infraType != QNN_HTP_DEVICE_INFRASTRUCTURE_TYPE_PERF) { q6m("htp-infra-type", (long)htp_infra->infraType); return 0; }
        q6_perf = &htp_infra->perfInfra;           /* stashed for live re-votes */
        uint32_t power_id = 0;
        if (q6_perf->createPowerConfigId(0, 0, &power_id) != 0) { q6m("power-id", -1); return 0; }
        q6_power_id = power_id;
        {
            QnnHtpPerfInfrastructure_PowerConfig_t dcvs, rpc_lat;
            const QnnHtpPerfInfrastructure_PowerConfig_t* cfgs[3];
            uint32_t vc = q6_read_vcorner();
            memset(&dcvs, 0, sizeof dcvs);
            memset(&rpc_lat, 0, sizeof rpc_lat);
            dcvs.option = QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_DCVS_V3;
            dcvs.dcvsV3Config.contextId = power_id;
            dcvs.dcvsV3Config.setDcvsEnable = 1;
            dcvs.dcvsV3Config.dcvsEnable = 0;          /* fixed corners */
            dcvs.dcvsV3Config.setSleepDisable = 1;
            dcvs.dcvsV3Config.sleepDisable = 1;
            dcvs.dcvsV3Config.setSleepLatency = 1;
            dcvs.dcvsV3Config.sleepLatency = 40;
            dcvs.dcvsV3Config.setBusParams = 1;
            dcvs.dcvsV3Config.busVoltageCornerMin = vc;
            dcvs.dcvsV3Config.busVoltageCornerTarget = vc;
            dcvs.dcvsV3Config.busVoltageCornerMax = vc;
            dcvs.dcvsV3Config.setCoreParams = 1;
            dcvs.dcvsV3Config.coreVoltageCornerMin = vc;
            dcvs.dcvsV3Config.coreVoltageCornerTarget = vc;
            dcvs.dcvsV3Config.coreVoltageCornerMax = vc;
            rpc_lat.option = QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_RPC_CONTROL_LATENCY;
            rpc_lat.rpcControlLatencyConfig = 100;
            /* mask=3: DCVS_V3 + RPC_CONTROL_LATENCY only — no cfgs slot
             * for RPC_POLLING_TIME on this DSP-OS (see header comment). */
            cfgs[0] = &dcvs; cfgs[1] = &rpc_lat; cfgs[2] = NULL;
            Qnn_ErrorHandle_t pve = q6_perf->setPowerConfig(power_id, cfgs);
            if (pve != 0) {
                q6m("power-vote-warn", -(long)pve);   /* NON-FATAL (M3R3): timing shows what held */
            } else {
                q6m("power-vote-corner", (long)vc);
            }
        }
    }

    if (q6_iface.contextCreateFromBinary(q6_backend, q6_device, NULL,
                                         q6_ctx_buf, (uint32_t)q6_ctx_size, &q6_context, NULL) != 0) {
        q6m("context-create", -1); return 0;
    }
    q6m("context-create", 1);

    {
        if (q6_iface.graphRetrieve(q6_context, q6_graph_name_keep, &q6_graph) != 0 || !q6_graph) { q6m("graph-retrieve", -1); return 0; }
        q6m("graph-retrieve", 1);
    }

    if (q6_n_in > 8) q6_n_in = 8;
    if (q6_n_out > 8) q6_n_out = 8;
    for (uint32_t i = 0; i < q6_n_in; i++) {
        int r = q6_register_io((int)i, &q6_in_info[i], 1);
        if (r) { q6m("in-register", (long)i * 100 + r); return 0; }
        memset(q6_in_ptrs[i], 0x40, q6_in_bytes[i]);   /* mid-range int8 pattern */
    }
    for (uint32_t i = 0; i < q6_n_out; i++) {
        int r = q6_register_io((int)i, &q6_out_info[i], 0);
        if (r) { q6m("out-register", (long)i * 100 + r); return 0; }
    }
    q6m("io-registered", (long)q6_n_in * 100 + q6_n_out);
    { size_t tb = 0; for (uint32_t i = 0; i < q6_n_in; i++) tb += q6_in_bytes[i]; q6m("in-bytes", (long)tb); }

    /* warm + timed executes */
    for (int w = 0; w < 3; w++) {
        if (q6_iface.graphExecute(q6_graph, q6_in_info, q6_n_in, q6_out_info, q6_n_out, NULL, NULL) != 0) {
            q6m("execute-warm", -w - 1); return 0;
        }
    }
    q6m("execute-warm", 3);
    {
        double total = 0, best = 1e9, worst = 0;
        const int reps = 10;
        for (int r = 0; r < reps; r++) {
            double t0 = q6_now_ms();
            Qnn_ErrorHandle_t e = q6_iface.graphExecute(q6_graph, q6_in_info, q6_n_in, q6_out_info, q6_n_out, NULL, NULL);
            double t1 = q6_now_ms();
            if (e != 0) { q6m("execute-timed", -r - 1); return 0; }
            total += t1 - t0;
            if (t1 - t0 < best) best = t1 - t0;
            if (t1 - t0 > worst) worst = t1 - t0;
        }
        q6m("exec-mean-us", (long)(total / reps * 1000.0));
        q6m("exec-best-us", (long)(best * 1000.0));
        q6m("exec-worst-us", (long)(worst * 1000.0));
        /* output liveness: a few bytes + a checksum-ish accumulation */
        { unsigned acc = 0; unsigned char* p = (unsigned char*)q6_out_ptrs[0]; size_t nb = q6_out_bytes[0];
          for (size_t i = 0; i < nb && i < (size_t)4096; i++) acc = acc * 31u + p[i];
          q6m("out-check", (long)acc); }
    }
    q6m("PROBE-PASS", (long)(q6_now_ms() - t_start));   /* ms total incl. init */
    q6_probe_ok = 1;
    q6_tier_cur = 3;      /* the boot bin_cfnhwc.bin is the perf-tier deploy */
    q6_tier_req = 3;
    q6_tier_good = 3;
    return 1;
}


/* ---- REV28 (M6b-2): the T_fsr4 NPU worker ----
 * Per eval: copy the feat outq (GPU-written, coherent mapping) into the
 * rpcmem input, execute the production graph, copy the output into the
 * postA npu-in bank, publish npu_done. The falsifier's per-frame waitIdle
 * makes the 1-eval lag exactly safe (outq at doorbell N holds eval N-1's
 * features); the real game upgrades this with a QueueSubmit trampoline. */
struct q6_dmabuf_sync_s { uint64_t flags; };
#define Q6_DMABUF_SYNC_READ  (1 << 0)
#define Q6_DMABUF_SYNC_WRITE (2 << 0)
#define Q6_DMABUF_SYNC_RW    (Q6_DMABUF_SYNC_READ | Q6_DMABUF_SYNC_WRITE)
#define Q6_DMABUF_SYNC_START (0 << 2)
#define Q6_DMABUF_SYNC_END   (1 << 2)
#define Q6_DMABUF_IOCTL_SYNC _IOW('b', 0, struct q6_dmabuf_sync_s)

static void q6_sync(int fd, uint64_t flags)
{
    struct q6_dmabuf_sync_s s; s.flags = flags;
    (void)!ioctl(fd, Q6_DMABUF_IOCTL_SYNC, &s);
}

/* r59f→r59g: sigmoid-LUT fp16 pack — rec = 1/(1+e^-x) over the NPU's 4
 * recurrent bytes, precomputed to fp16 BIT PATTERNS (256-entry table).
 * r59g: produced in the WORKER from the CACHED rpcmem output (inside the
 * dmabuf sync bracket) — NEVER from the GPU-mapped ring bank (the uncached
 * read was 128 ns/texel = the 118 ms loop that starved postA). Layer-side
 * pipeline creation for a GPU expansion is closed by the REV43 law (the
 * layer NEVER creates on the game device; boot 23 re-confirmed). */
static unsigned short q6_rec16[256];
static int q6_rec16_ok = 0;
static void q6_rec16_init(void)
{
    int k;
    for (k = 0; k < 256; k++) {
        float rv = (float)(k >= 128 ? k - 256 : k) * 0.303211569786f;
        float v = 1.0f / (1.0f + expf(-rv));
        unsigned int x; memcpy(&x, &v, 4);
        {   unsigned int sign = (x >> 16) & 0x8000u;
            int e = (int)((x >> 23) & 0xff) - 127 + 15;
            unsigned int m = x & 0x7fffffu;
            unsigned short h;
            if (e <= 0) h = (unsigned short)sign;
            else if (e >= 31) h = (unsigned short)(sign | 0x7c00u);
            else h = (unsigned short)(sign | ((unsigned int)e << 10) | (m >> 13));
            q6_rec16[k] = h;
        }
    }
    q6_rec16_ok = 1;
}
static void q6_wait_eval(volatile unsigned long long* seq, unsigned long long need)
{
    unsigned long long cur = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    if (cur >= need) return;
    if (syscall(98 /* SYS_futex, arm64 */, (const void*)seq, 128 /* FUTEX_WAIT_PRIVATE */,
                (int)(unsigned int)(cur & 0xffffffffull), 0, 0, 0) != 0)
        usleep(200);
}

/* the r59g RING worker: THREE slots, EXPLICIT publish. After each copy-out
 * it stores *pub = (gen<<8) | slot and never writes the two most recent
 * slots, so the layer's snapshot copies always read a quiet bank. The rec
 * feed is produced HERE from the CACHED rpcmem (inside the sync bracket —
 * the old loop read the GPU-mapped bank at 128 ns/texel = 118 ms and starved
 * postA; the cached read is ~3 ms). npu_done is still published for the
 * seat's logs. */
/* rev3c.6 LIVE WEIGHT BIAS: int (ascii) files in /var/home/armada/fsr4npu-logs
 * fsr4npu-P3BIAS / fsr4npu-P0BIAS add a signed count to the NPU output's
 * channel 3 (blend) / channel 0 (correlation) bytes [NHWC s8, 8ch]. The blend
 * experiment: our content parks the model at blend~0.2 (expo 4 peak); bias
 * answers "does forcing temporal accumulation stabilize the image?".
 * Re-read at most 1x/s; 0 (absent) = model verbatim. */
static int q6_file_bias(const char* name)
{
    static int cached[2]; static unsigned long long last[2];
    static int idx_inited;
    int idx = (name[1] == '3') ? 0 : 1;             /* P3BIAS vs P0BIAS */
    unsigned long long now = (unsigned long long)time(NULL);
    if (!idx_inited) { cached[0] = 0; cached[1] = 0; idx_inited = 1; }
    if (now != last[idx] || !last[idx]) {
        char p[256]; FILE* f;
        last[idx] = now;
        snprintf(p, sizeof p, "/var/home/armada/fsr4npu-logs/fsr4npu-%s", name);
        f = fopen(p, "r");
        if (f) { int v = 0; if (fscanf(f, "%d", &v) == 1 && v >= -64 && v <= 64) cached[idx] = v; fclose(f); }
        else cached[idx] = 0;
    }
    return cached[idx];
}

void qnn_glue_worker3(volatile unsigned long long* eval_seq,
                      volatile unsigned long long* npu_done,
                      volatile unsigned long long* pub,
                      const void* outq_src,
                      void* ring0, void* ring1, void* ring2,
                      void* rec0, void* rec1, void* rec2,
                      unsigned long long in_bytes, unsigned long long out_bytes)
{
    void* ring[3] = { ring0, ring1, ring2 };
    void* rec[3] = { rec0, rec1, rec2 };
    unsigned long long consumed = 0, gen = 0, loops = 0;
    double exec_total = 0, copy_total = 0, rec_total = 0, exec_max = 0;
    unsigned long long in_n  = in_bytes  < q6_in_bytes[0]  ? in_bytes  : q6_in_bytes[0];
    unsigned long long out_n = out_bytes < q6_out_bytes[0] ? out_bytes : q6_out_bytes[0];
    if (!q6_rec16_ok) q6_rec16_init();
    q6m("t7-worker-start", 4);
    q6m("t7-worker-bytes", (long)((in_n >> 10) & 0xffff) | (long)((out_n >> 10) << 16));
    {   /* r60 feed9w: fsr4npu-AFFINITY pins the worker to the A510 cluster
         * (cpu0-2) — the FEX translation owns the big cores at ~89%. */
        if (access(SEAM_LOG_DIR "/fsr4npu-AFFINITY", F_OK) == 0) {
            cpu_set_t am; CPU_ZERO(&am); CPU_SET(0, &am); CPU_SET(1, &am); CPU_SET(2, &am);
            q6m("t7-affinity-ok", sched_setaffinity(0, sizeof am, &am) == 0 ? 1 : 0);
        }
    }
    {   /* r60 feed9w: the zero-copy import harness (oracle rounds 8-9), gated
         * by fsr4npu-ZCHARN inside. Phase 1 before the eval loop. */
        extern void m9_zc_harness_begin(int, void*, unsigned long long, int, void*, unsigned long long);
        m9_zc_harness_begin(q6_in_fds[0], q6_in_ptrs[0], q6_in_bytes[0],
                            q6_out_fds[0], q6_out_ptrs[0], q6_out_bytes[0]);
    }
    for (;;) {
        q6_tier_maybe_swap();   /* ratio era: between-executes context swap */
        q6_maybe_revote();      /* r62: live VCORNER re-vote (1x/s gated) */
        unsigned long long e = __atomic_load_n(eval_seq, __ATOMIC_ACQUIRE);
        double t0, t1, t2;
        unsigned long long slot = gen % 3ull;   /* captured BEFORE the copy — the published slot must name the bank actually written */
        if (e < consumed + 2) { q6_wait_code = 0; q6_wait_eval(eval_seq, consumed + 2); continue; }
        t0 = q6_now_ms();
        q6_wait_code = 1;   /* r61: input bridge phase */
        {   /* r60h STAGE 3 (fsr4npu-ZCC3 + the layer's GPU copy armed = the
             * input bridge DELETED): the game CB's recorded copy
             * outq -> imported IN bank replaces this 3.69MB memcpy + sync
             * bracket (t7-copies' 5.8ms). Falls back to the CPU bridge until
             * the layer confirms its first recorded copy — never both
             * skipped. */
            extern volatile int m9_zcc3_active;
            static int zcc3w = -1;
            if (zcc3w < 0) zcc3w = access(SEAM_LOG_DIR "/fsr4npu-ZCC3", F_OK) == 0;
            if (!(zcc3w && m9_zcc3_active)) {
                q6_sync(q6_in_fds[0], Q6_DMABUF_SYNC_RW | Q6_DMABUF_SYNC_START);
                memcpy(q6_in_ptrs[0], outq_src, (size_t)in_n);
                q6_sync(q6_in_fds[0], Q6_DMABUF_SYNC_WRITE | Q6_DMABUF_SYNC_END);
            }
        }
        t1 = q6_now_ms();
        q6_exec_enter_seq = e; q6_exec_start_ms = t1; q6_wait_code = 2;   /* r61: entering graphExecute */
        if (q6_iface.graphExecute(q6_graph, q6_in_info, q6_n_in, q6_out_info, q6_n_out, NULL, NULL) != 0) {
            q6m("t7-execute-fail", (long)consumed);
            /* containment: blacklist the failing bin for the session, fall
             * back to the last-good (rebind makes the fallback execute —
             * the first-contact build died here instead) */
            q6_tier_blacklist |= 1u << q6_tier_cur;
            { int fb = q6_pick_fallback();
              if (fb >= 0 && fb != q6_tier_cur && q6_load_context_bin(fb)) {
                  q6_tier_cur = fb; q6_tier_good = fb;
                  q6m("tier-exec-fail-fallback", fb);
                  continue;
              } }
            return;
        }
        q6_tier_good = q6_tier_cur;
        q6_exec_exit_seq = e;   /* r61: graphExecute returned */
        q6_wait_code = 3;       /* r61: output/rec/publish phase */
        t2 = q6_now_ms();
        {   /* r60f C-2 STAGE 2 (fsr4npu-ZCC2 present = zero-copy OUT): the GPU
             * reads the imported rpcmem directly; otherwise the 7.37MB ring
             * memcpy rides INSIDE the RW bracket, and the r59g rec-LUT (when
             * rec slots exist) reads the cached rpcmem within the SAME
             * bracket. rev3c.7 NMS: rec=NULL (the exp8to16 GPU bridge owns
             * the loop) and the live P3/P0 weight-bias knobs patch the ring
             * copy before publish.
             * NMS-PORT GATE HARDENING: the skip requires the LAYER's
             * m9_zcc2_nms_active (set once the record path's import + GPU
             * snapshot copies flow) — the marker alone can NEVER blank the
             * image (the bench-era m9 chassis path never sets the flag). */
            extern volatile int m9_zcc2_nms_active;
            static int zcc2w = -1;
            if (zcc2w < 0) zcc2w = access(SEAM_LOG_DIR "/fsr4npu-ZCC2", F_OK) == 0;
            if (!(zcc2w && m9_zcc2_nms_active)) {
                int b3 = q6_file_bias("P3BIAS"), b0 = q6_file_bias("P0BIAS");
                q6_sync(q6_out_fds[0], Q6_DMABUF_SYNC_RW | Q6_DMABUF_SYNC_START);
                memcpy(ring[slot], q6_out_ptrs[0], (size_t)out_n);
                if (b3 || b0) {   /* NHWC s8 8ch: patch ch3 (+ch0), saturating */
                    signed char* p = (signed char*)ring[slot];
                    unsigned long long i, n8 = out_n >> 3;
                    if (b3) for (i = 0; i < n8; i++) {
                        int v = p[i * 8 + 3] + b3;
                        p[i * 8 + 3] = (signed char)(v < -128 ? -128 : (v > 127 ? 127 : v));
                    }
                    if (b0) for (i = 0; i < n8; i++) {
                        int v = p[i * 8] + b0;
                        p[i * 8] = (signed char)(v < -128 ? -128 : (v > 127 ? 127 : v));
                    }
                }
                if (rec[0]) {
                    const unsigned int* s = (const unsigned int*)q6_out_ptrs[0];
                    unsigned long long* d = (unsigned long long*)rec[slot];
                    unsigned long long n = out_n / 8ull, i;
                    for (i = 0; i < n; i++) {
                        unsigned int w1 = s[i * 2ull + 1ull];
                        unsigned int lo = (unsigned int)q6_rec16[w1 & 0xff] | ((unsigned int)q6_rec16[(w1 >> 8) & 0xff] << 16);
                        unsigned int hi = (unsigned int)q6_rec16[(w1 >> 16) & 0xff] | ((unsigned int)q6_rec16[w1 >> 24] << 16);
                        d[i] = (unsigned long long)lo | ((unsigned long long)hi << 32);
                    }
                }
                q6_sync(q6_out_fds[0], Q6_DMABUF_SYNC_READ | Q6_DMABUF_SYNC_END);
            }
        }
        {   /* r60 feed9w: one-shot harness phase 2 — the DSP->GPU adjudication
             * + negative control, gated by fsr4npu-ZCHARN inside. */
            extern void m9_zc_harness_postexec(void);
            static int zc2; if (!zc2) { zc2 = 1; m9_zc_harness_postexec(); }
        }
        consumed = e - 1;
        gen++;
        __atomic_store_n(npu_done, consumed, __ATOMIC_RELEASE);
        __atomic_store_n(pub, (gen << 8) | slot, __ATOMIC_RELEASE);
        q6_pub_consumed = consumed;   /* r61: plain store — forensics reads it without atomics */
        loops++;
        exec_total += t2 - t1;
        copy_total += (t1 - t0);
        rec_total += q6_now_ms() - t2;
        if (t2 - t1 > exec_max) exec_max = t2 - t1;
        if ((loops & 0xFF) == 1) {
            q6m("t7-loops", (long)loops);
            q6m("t7-gen", (long)gen);
            q6m("t7-exec-mean-us", (long)(exec_total / (double)loops * 1000.0));
            q6m("t7-exec-max-us", (long)(exec_max * 1000.0));
            q6m("t7-copies-mean-us", (long)(copy_total / (double)loops * 1000.0));
            q6m("t7-rec-mean-us", (long)(rec_total / (double)loops * 1000.0));
            {   /* r59g (oracle round-3 Q3): the crash discriminator — if the
                 * ~eval-1000 death is lmkd/cgroup-OOM, VmRSS/VmHWM trend up. */
                char lb[256]; long rss = 0, hwm = 0;
                FILE* sf = fopen("/proc/self/status", "r");
                if (sf) {
                    while (fgets(lb, sizeof lb, sf)) {
                        if (!strncmp(lb, "VmRSS:", 6)) rss = atol(lb + 6);
                        else if (!strncmp(lb, "VmHWM:", 6)) hwm = atol(lb + 6);
                    }
                    fclose(sf);
                }
                q6m("t7-vmrss-kb", rss);
                q6m("t7-vmhwm-kb", hwm);
            }
            {   /* r60 feed9w: retire-marker lag (oracle round-9 Q3), gated */
                extern volatile unsigned* m9_zc_retire_word(void);
                volatile unsigned* rw = m9_zc_retire_word();
                if (rw) q6m("t7-retire-lag", (long)((unsigned long long)consumed - (unsigned long long)*rw));
            }
        }
    }
}



void qnn_glue_worker2(volatile unsigned long long* eval_seq,
                      volatile unsigned long long* npu_done,
                      const void* outq_src, void* npu_dstA, void* npu_dstB,
                      unsigned long long in_bytes, unsigned long long out_bytes)
{
    unsigned long long consumed = 0, loops = 0;
    double exec_total = 0, copy_total = 0, exec_max = 0;
    /* rev3c.6 RACE FIX: the NMS port collapsed r59e's two-bank ping-pong into
     * ONE npu-in bank (dstA==dstB) — the worker's 7.37MB CPU memcpy raced the
     * GPU's postA read of the same bank = the "occasional texture glitch".
     * The queue-ordered handoff replaces it: the worker writes a CPU staging
     * map (stage2_ptr, passed as dstA by the NMS layer) and the LAYER records
     * vkCmdCopyBuffer(staging->npu-in) BEFORE the postA dispatch. When both
     * dst pointers are the same bank (bench/legacy callers), the old direct
     * write behavior stands. */
    unsigned long long in_n  = in_bytes  < q6_in_bytes[0]  ? in_bytes  : q6_in_bytes[0];
    unsigned long long out_n = out_bytes < q6_out_bytes[0] ? out_bytes : q6_out_bytes[0];
    q6m("t7-worker-start", 2);
    q6m("t7-worker-bytes", (long)((in_n >> 10) & 0xffff) | (long)((out_n >> 10) << 16));
    for (;;) {
        q6_tier_maybe_swap();   /* ratio era: between-executes context swap */
        unsigned long long e = __atomic_load_n(eval_seq, __ATOMIC_ACQUIRE);
        double t0, t1, t2;
        void* dst = ((e - 1ull) & 1ull) ? npu_dstB : npu_dstA;   /* rev3c.6: generation parity — matches the reader's done&1 even if evals are skipped */
        int b3 = q6_file_bias("P3BIAS"), b0 = q6_file_bias("P0BIAS");
        if (e < consumed + 2) { usleep(200); continue; }
        t0 = q6_now_ms();
        q6_sync(q6_in_fds[0], Q6_DMABUF_SYNC_RW | Q6_DMABUF_SYNC_START);
        memcpy(q6_in_ptrs[0], outq_src, (size_t)in_n);
        q6_sync(q6_in_fds[0], Q6_DMABUF_SYNC_WRITE | Q6_DMABUF_SYNC_END);
        t1 = q6_now_ms();
        if (q6_iface.graphExecute(q6_graph, q6_in_info, q6_n_in, q6_out_info, q6_n_out, NULL, NULL) != 0) {
            q6m("t7-execute-fail", (long)consumed);
            return;
        }
        t2 = q6_now_ms();
        q6_sync(q6_out_fds[0], Q6_DMABUF_SYNC_RW | Q6_DMABUF_SYNC_START);
        memcpy(dst, q6_out_ptrs[0], (size_t)out_n);
        q6_sync(q6_out_fds[0], Q6_DMABUF_SYNC_READ | Q6_DMABUF_SYNC_END);
        if (b3 || b0) {   /* NHWC s8 8ch: patch ch3 (+ch0), saturating */
            signed char* p = (signed char*)dst;
            unsigned long long i, n8 = out_n >> 3;
            if (b3) for (i = 0; i < n8; i++) {
                int v = p[i * 8 + 3] + b3;
                p[i * 8 + 3] = (signed char)(v < -128 ? -128 : (v > 127 ? 127 : v));
            }
            if (b0) for (i = 0; i < n8; i++) {
                int v = p[i * 8] + b0;
                p[i * 8] = (signed char)(v < -128 ? -128 : (v > 127 ? 127 : v));
            }
        }
        consumed = e - 1;
        __atomic_store_n(npu_done, consumed, __ATOMIC_RELEASE);
        loops++;
        exec_total += t2 - t1;
        copy_total += (t1 - t0) + (q6_now_ms() - t2);
        if (t2 - t1 > exec_max) exec_max = t2 - t1;
        if ((loops & 0xFF) == 1) {
            q6m("t7-loops", (long)loops);
            q6m("t7-exec-mean-us", (long)(exec_total / (double)loops * 1000.0));
            q6m("t7-exec-max-us", (long)(exec_max * 1000.0));
            q6m("t7-copies-mean-us", (long)(copy_total / (double)loops * 1000.0));
        }
    }
}

/* legacy single-bank entry (kept for compatibility) */
void qnn_glue_worker(volatile unsigned long long* eval_seq,
                     volatile unsigned long long* npu_done,
                     const void* outq_src, void* npu_dst,
                     unsigned long long in_bytes, unsigned long long out_bytes)
{
    qnn_glue_worker2(eval_seq, npu_done, outq_src, npu_dst, npu_dst, in_bytes, out_bytes);
}

/* r60e C-2 stage 1: the output rpcmem fd for the game-device import */
int q6_out_fd0(void) { return q6_out_fds[0]; }

/* r60h STAGE 3: the input rpcmem fd for the game-device import */
int q6_in_fd0(void) { return q6_in_fds[0]; }

/* ==========================================================================
 * NMS-1 worker (XeSS slot, rev3a) — see docs/NPU-AT-XESS-SLOT-DESIGN.md §1/§4.
 * Pipelined loop: per target frame N (npu_done+1), wait for the layer's
 * eval_seq >= N (doorbell recorded) AND the GPU stamp == N (the staging
 * copies EXECUTED — the explicit completion oracle), then:
 *   bridge   : memcpy the three staging banks -> cached scratch (the WC-read
 *              law: never loop over GPU-mapped memory),
 *   convert  : scratch -> int8[1,360,640,16] tensor (rev3a TRANSPORT fill —
 *              ch0..2 = mu-law(color) via 2048/1024-entry LUTs (the wv1
 *              u_expo=1 observance), ch3..4 = velocity f16->int8 (scale
 *              placeholder), ch5 = depth f32->int8, ch6..15 = 0; the REAL
 *              16ch semantics are the learned pre0 (wv1.comp) — rev3b),
 *   execute  : rpcmem in <- tensor (dmabuf bracket), graphExecute, bracket,
 *   pack     : rpcmem out (fp16 RGBA) -> B10G11R11 words into npudst
 *              (GPU-mapped bank; streaming writes),
 *   publish  : npu_done = N (release); the NEXT doorbell records the copy.
 * graphExecute failure: one immediate retry (the QNN-4xx exemption), a
 * second consecutive failure parks the worker (the record path's blit
 * fallback keeps the game's image alive).
 * ========================================================================== */
static float q6_h2f(unsigned short h)
{
    unsigned int sign = (unsigned int)(h >> 15) << 31;
    unsigned int e = (unsigned int)((h >> 10) & 0x1f);
    unsigned int m = (unsigned int)(h & 0x3ff);
    unsigned int b; float f;
    if (e == 0) {                       /* half denormals: m * 2^-24 */
        if (!m) { b = sign; }
        else { /* normalize */
            unsigned int shift = 0; unsigned int mm = m;
            while (!(mm & 0x400u)) { mm <<= 1; shift++; }
            b = sign | ((unsigned int)(127 - 15 + 1 - (int)shift) << 23) | ((mm & 0x3ffu) << 13);
        }
    } else if (e == 31) {
        b = sign | 0x7f800000u | (m << 13);
    } else {
        b = sign | ((e - 15 + 127) << 23) | (m << 13);
    }
    memcpy(&f, &b, 4);
    return f;
}

/* f11/f10 pattern -> float, mirroring wv1.comp's shift-into-f16 decode
 * (pattern<<4 / <<5 then unpack; exact for normals AND denormals). */
static float q6_f11(unsigned int p) { return q6_h2f((unsigned short)(p << 4)); }
static float q6_f10(unsigned int p) { return q6_h2f((unsigned short)(p << 5)); }

/* mu-law int8 LUTs over the 2048/1024 discrete field patterns
 * (q = 0.1174 * ln(1 + 150*max(c,0)), the wv1 input observance; scaled
 * x190 into [0,127] — DOCUMENTED rev3a transport mapping, not trained). */
static signed char q6_mu11[2048];
static signed char q6_mu10[1024];
static int q6_mu_ok = 0;
static void q6_mu_init(void)
{
    unsigned int p;
    for (p = 0; p < 2048; p++) {
        float c = q6_f11(p);
        float q = c > 0.0f ? 0.1174f * logf(1.0f + 150.0f * c) : 0.0f;
        float s = q * 190.0f;
        q6_mu11[p] = (signed char)(s > 127.0f ? 127 : s < 0.0f ? 0 : (signed char)(s + 0.5f));
    }
    for (p = 0; p < 1024; p++) {
        float c = q6_f10(p);
        float q = c > 0.0f ? 0.1174f * logf(1.0f + 150.0f * c) : 0.0f;
        float s = q * 190.0f;
        q6_mu10[p] = (signed char)(s > 127.0f ? 127 : s < 0.0f ? 0 : (signed char)(s + 0.5f));
    }
    q6_mu_ok = 1;
}

/* float >= 0 -> packed 11-bit / 10-bit field (round-to-nearest-even, denormal
 * and overflow handled; > max becomes inf per the format). */
static unsigned int q6_pk11(float v)
{
    unsigned int x; memcpy(&x, &v, 4);
    unsigned int e = (x >> 23) & 0xffu, m = x & 0x7fffffu;
    if (!e) return 0;
    if (e == 0xffu) return 0x7C0u;
    { int E = (int)e - 127 + 15;
      if (E >= 31) return 0x7C0u;
      if (E <= 0) {
          double dv = (double)v;
          long M = (long)(dv * 1048576.0 + 0.5);      /* M * 2^-20 */
          if (M >= 64) return 1u << 6;                /* rounded to min normal */
          return (unsigned int)M;
      }
      { unsigned int mm = m >> 17;
        unsigned int round = (m >> 16) & 1u;
        unsigned int sticky = (m & 0xffffu) ? 1u : 0u;
        mm += round & (sticky | (mm & 1u));
        if (mm == 64u) { mm = 0; E++; if (E >= 31) return 0x7C0u; }
        return ((unsigned int)E << 6) | mm; } }
}
static unsigned int q6_pk10(float v)
{
    unsigned int x; memcpy(&x, &v, 4);
    unsigned int e = (x >> 23) & 0xffu, m = x & 0x7fffffu;
    if (!e) return 0;
    if (e == 0xffu) return 0x3E0u;
    { int E = (int)e - 127 + 15;
      if (E >= 31) return 0x3E0u;
      if (E <= 0) {
          double dv = (double)v;
          long M = (long)(dv * 524288.0 + 0.5);       /* M * 2^-19 */
          if (M >= 32) return 1u << 5;
          return (unsigned int)M;
      }
      { unsigned int mm = m >> 18;
        unsigned int round = (m >> 17) & 1u;
        unsigned int sticky = (m & 0x1ffffu) ? 1u : 0u;
        mm += round & (sticky | (mm & 1u));
        if (mm == 32u) { mm = 0; E++; if (E >= 31) return 0x3E0u; }
        return ((unsigned int)E << 5) | mm; } }
}

void qnn_glue_worker_nms(volatile unsigned long long* eval_seq,
                         volatile unsigned long long* npu_done,
                         volatile unsigned long long* stamp,
                         const void* stage_color, const void* stage_vel,
                         const void* stage_depth, void* npudst,
                         unsigned long long fw, unsigned long long fh,
                         unsigned long long ow, unsigned long long oh,
                         unsigned long long stage_bytes,
                         unsigned long long npudst_bytes)
{
    unsigned long long done = 0, loops = 0, stamp_stalls = 0;
    double exec_total = 0, bridge_total = 0, pack_total = 0, exec_max = 0, stamp_max = 0;
    int consecutive_fails = 0;
    void *sc = NULL, *sv = NULL, *sd = NULL, *tensor = NULL;
    unsigned long long in_n = 3686400ull;
    unsigned long long out_n = ow * oh * 8ull;             /* fp16 RGBA bytes */
    unsigned int cw = fw < 640ull ? (unsigned int)fw : 640u;
    unsigned int chh = fh < 360ull ? (unsigned int)fh : 360u;

    (void)npudst_bytes;
    if (!q6_mu_ok) q6_mu_init();
    if (q6_in_bytes[0] && q6_in_bytes[0] < in_n) in_n = q6_in_bytes[0];
    if (q6_out_bytes[0] && q6_out_bytes[0] < out_n) out_n = q6_out_bytes[0];
    if (npu_done) { done = __atomic_load_n(npu_done, __ATOMIC_ACQUIRE); }
    sc = malloc(stage_bytes ? (size_t)stage_bytes : 1);
    sv = malloc(stage_bytes ? (size_t)stage_bytes : 1);
    sd = malloc(stage_bytes ? (size_t)stage_bytes : 1);
    tensor = malloc(3686400ull);
    if (!sc || !sv || !sd || !tensor) { q6m("nms-w-scratch-fail", 0); goto out; }
    q6m("nms-w-start", 1);
    q6m("nms-w-geom", (long)(((unsigned long long)cw << 16) | chh));

    for (;;) {
        unsigned long long target = done + 1ull, e;
        double t0, t1, t2, t3;
        /* 1) wait for the doorbell (futex like the r59g worker) */
        e = __atomic_load_n(eval_seq, __ATOMIC_ACQUIRE);
        if (e < target) { q6_wait_eval(eval_seq, target); continue; }
        /* 2) wait for the GPU stamp: the staging copies EXECUTED */
        {
            double tw0 = q6_now_ms();
            while (stamp[0] < target) {
                usleep(50);
                if (q6_now_ms() - tw0 > 250.0) {        /* stall: process with what we have */
                    stamp_stalls++;
                    if ((stamp_stalls & 63ull) == 1ull) q6m("nms-w-stamp-stall", (long)target);
                    break;
                }
            }
            { double dw = q6_now_ms() - tw0;
              if (dw > stamp_max) stamp_max = dw; }
        }
        t0 = q6_now_ms();
        /* 3) bridge: staging -> cached scratch (single linear pass each) */
        memcpy(sc, stage_color, (size_t)stage_bytes);
        memcpy(sv, stage_vel, (size_t)stage_bytes);
        memcpy(sd, stage_depth, (size_t)stage_bytes);
        /* 4) convert -> int8 tensor (rev3a transport fill) */
        {
            signed char* t = (signed char*)tensor;
            const unsigned int* wc = (const unsigned int*)sc;
            const unsigned int* wv = (const unsigned int*)sv;
            const unsigned int* wd = (const unsigned int*)sd;
            unsigned int y, x;
            memset(tensor, 0, 3686400ull);
            for (y = 0; y < chh; y++) {
                const unsigned int* crow = wc + (size_t)y * (size_t)fw;
                const unsigned int* vrow = wv + (size_t)y * (size_t)fw;
                const unsigned int* drow = wd + (size_t)y * (size_t)fw;
                signed char* trow = t + ((size_t)y * 640ull) * 16ull;
                for (x = 0; x < cw; x++) {
                    unsigned int w = crow[x];
                    signed char* tp = trow + (size_t)x * 16ull;
                    tp[0] = q6_mu11[w & 0x7ffu];            /* R (fmt122: R low) */
                    tp[1] = q6_mu11[(w >> 11) & 0x7ffu];    /* G */
                    tp[2] = q6_mu10[(w >> 22) & 0x3ffu];    /* B */
                    { float vx = q6_h2f((unsigned short)(vrow[x] & 0xffffu));
                      float vy = q6_h2f((unsigned short)(vrow[x] >> 16));
                      float sx = vx * 32.0f, sy = vy * 32.0f;   /* placeholder scale */
                      tp[3] = (signed char)(sx > 127.0f ? 127 : sx < -128.0f ? -128 : (signed char)sx);
                      tp[4] = (signed char)(sy > 127.0f ? 127 : sy < -128.0f ? -128 : (signed char)sy); }
                    { float d; memcpy(&d, &drow[x], 4);
                      float sdd = d * 127.0f;
                      tp[5] = (signed char)(sdd > 127.0f ? 127 : sdd < 0.0f ? 0 : (signed char)sdd); }
                }
            }
        }
        t1 = q6_now_ms();
        /* 5) NPU execute */
        q6_sync(q6_in_fds[0], Q6_DMABUF_SYNC_RW | Q6_DMABUF_SYNC_START);
        memcpy(q6_in_ptrs[0], tensor, (size_t)in_n);
        q6_sync(q6_in_fds[0], Q6_DMABUF_SYNC_WRITE | Q6_DMABUF_SYNC_END);
        if (q6_iface.graphExecute(q6_graph, q6_in_info, q6_n_in, q6_out_info, q6_n_out, NULL, NULL) != 0) {
            if (++consecutive_fails == 1) {               /* one immediate retry (QNN-4xx exemption) */
                q6m("nms-w-exec-retry", (long)target);
                continue;                                  /* re-run target: scratch still valid */
            }
            q6m("nms-w-exec-fail-park", (long)target);
            goto out;                                      /* worker parks; record path falls back */
        }
        consecutive_fails = 0;
        t2 = q6_now_ms();
        /* 6) pack: fp16 RGBA -> B10G11R11 words into npudst (streaming writes) */
        {
            const unsigned short* s = (const unsigned short*)q6_out_ptrs[0];
            unsigned int* d = (unsigned int*)npudst;
            unsigned long long n = out_n / 8ull, i;
            for (i = 0; i < n; i++) {
                float r = q6_h2f(s[i * 4ull]);
                float g = q6_h2f(s[i * 4ull + 1ull]);
                float b = q6_h2f(s[i * 4ull + 2ull]);
                if (r < 0.0f) r = 0.0f;
                if (g < 0.0f) g = 0.0f;
                if (b < 0.0f) b = 0.0f;
                d[i] = q6_pk11(r) | (q6_pk11(g) << 11) | (q6_pk10(b) << 22);
            }
        }
        q6_sync(q6_out_fds[0], Q6_DMABUF_SYNC_READ | Q6_DMABUF_SYNC_END);
        t3 = q6_now_ms();
        done = target;
        __atomic_store_n(npu_done, done, __ATOMIC_RELEASE);
        loops++;
        exec_total += t2 - t1;
        bridge_total += t1 - t0;
        pack_total += t3 - t2;
        if (t2 - t1 > exec_max) exec_max = t2 - t1;
        if ((loops & 31ull) == 1ull) {
            q6m("nms-w-loops", (long)loops);
            q6m("nms-w-done", (long)done);
            q6m("nms-w-exec-mean-us", (long)(exec_total / (double)loops * 1000.0));
            q6m("nms-w-exec-max-us", (long)(exec_max * 1000.0));
            q6m("nms-w-bridge-mean-us", (long)(bridge_total / (double)loops * 1000.0));
            q6m("nms-w-pack-mean-us", (long)(pack_total / (double)loops * 1000.0));
            q6m("nms-w-stamp-stalls", (long)stamp_stalls);
            q6m("nms-w-stamp-max-ms", (long)(stamp_max * 1000.0));
        }
    }
out:
    free(sc); free(sv); free(sd); free(tensor);
    q6m("nms-w-exit", (long)loops);
}
