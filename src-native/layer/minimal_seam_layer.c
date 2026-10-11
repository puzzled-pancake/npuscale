// minimal_seam_layer.c — REV13 seam carrier, v2 (post-panel).
//
// v1 had two fatal flaws the panels caught:
//  - link structs put pfnGIPA at offset 0; the real (header-verified,
//    live-proven) layout is pNext@0 / pfnNextGIPA@8 / pfnNextGPDPA@16
//    (probe_layer.c:393-400) — v1 called a data pointer.
//  - dlsym(RTLD_DEFAULT) fallbacks + cached-instance forwarding — the
//    reentrancy/wine-symbol class the probe tree bans.
// v2 uses the verified layouts verbatim, performs the loader-protocol
// link-advance write-back (probe_layer.c:505-509 shape — required), has
// NO fallbacks, forwards the CALLER's handles, resolves the four seam
// downs per-call through the caller's device, and intercepts nothing else.
//
// Game-facing behavior: identical to an unlayered run except four
// buffer-ops calls gain a signature check + the doorbell ack.

#define _GNU_SOURCE
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>
#include "wv0_spv.h"   /* proven storage-buffer compute: out[i]=in[i]*mult+add */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include <stddef.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <fcntl.h>

typedef uint32_t VkResult;
typedef void* VkInstance;
typedef void* VkPhysicalDevice;
typedef void* VkDevice;

#define VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO 47
#define VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO   48

// ---- verified link layouts (probe_layer.c:393-400, live-proven r22+) ----
typedef struct VkLayerInstanceLink_ { struct VkLayerInstanceLink_* pNext;
                                      void* pfnNextGetInstanceProcAddr;
                                      void* pfnNextGetPhysicalDeviceProcAddr; } VkLayerInstanceLink__;
typedef struct { uint32_t sType; void* pNext; uint32_t function; VkLayerInstanceLink__* pLayerInfo; } VkLayerInstanceCreateInfo__;
typedef struct VkLayerDeviceLink_ { struct VkLayerDeviceLink_* pNext;
                                    void* pfnNextGetInstanceProcAddr;
                                    void* pfnNextGetDeviceProcAddr; } VkLayerDeviceLink__;
typedef struct { uint32_t sType; void* pNext; uint32_t function; VkLayerDeviceLink__* pLayerInfo; } VkLayerDeviceCreateInfo__;

static void* (*g_down_gipa)(void*, const char*);
static void* (*g_down_gdpa)(void*, const char*);
static void* (*down_gipa_for_device)(void*, const char*);
static void* g_chain_dev;    // captured ONCE at create; never overwritten
static void* g_chain_pd;     // r59g: the game physical device (memprops query)
static void* g_chain_inst;   // r59g: the game instance (instance-level fn resolution)

/* ---- REV14 work-bit machinery: the in-CB append ----
 * At the first doorbell the layer creates a small TRANSFER_DST target and
 * records vkCmdFillBuffer into the DOORBELL'S OWN command buffer — a real,
 * GPU-executing operation appended by the layer (falsifier criterion (c);
 * the EASU compute stage replaces the fill in the FSR4 build). */
static void* g_fill_buf;
static void* g_fill_map;   /* REV49: mapped fill buffer for the submission test */
static int    g_fill_state;   /* 0=untired 1=ok -1=failed */
static void* g_cbuf_a;      /* compute in  (STORAGE, garbage ok) */
static void* g_cbuf_b;      /* compute out */
static void* g_cmodule, *g_cdsl, *g_cplayout, *g_cpipe, *g_cdpool, *g_cset;
static int    g_comp_state; /* 0=untried 1=ok -1=failed */

// seam tracker (seam_layer.c)
extern void seam_on_create_buffer(int32_t, uint64_t, uint32_t, void*);
extern void seam_on_bind_buffer(int32_t, void*, void*);
extern void seam_on_map_memory(int32_t, void*, void**);
typedef struct SeamHeader_ SeamHeader__;   /* seam_layer.c layout */
extern int  seam_on_cmd_update_buffer(void*, void*, uint64_t, uint32_t, const void*, SeamHeader__**, uint64_t*);
extern void seam_write_ack(SeamHeader__*, uint64_t, int);
extern uint64_t seam_comp_plan(int stage, SeamHeader__*, uint64_t*, uint64_t*, uint64_t*,
                               uint32_t*, uint32_t*, uint32_t*, uint32_t*); /* REV28 */
extern int seam_route_stage3(SeamHeader__*, uint64_t* dev, uint64_t* set);  /* REV34 (ROUTE-V2) */
extern const void* seam_last_frame(unsigned long long*, unsigned long long*);
extern int seam_npu_handshake(SeamHeader__*, const void**, void**,
                              volatile unsigned long long**, volatile unsigned long long**,
                              unsigned long long*, unsigned long long*);
extern void seam_npu_bump_eval(SeamHeader__*);
extern unsigned long long seam_npu_done(SeamHeader__*);
extern void seam_npu_eval_wake(SeamHeader__*);

#ifdef SEAM_LOG
#ifndef SEAM_LOG_DIR
#define SEAM_LOG_DIR "/var/home/armada/fsr4npu-logs"
#endif
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
static void stepm(const char* what, long v)
{
    FILE* f = fopen(SEAM_LOG_DIR "/fsr4npu-SEAM.log", "a");
    if (f) { fprintf(f, "%s = %ld (pid %d tid %d)\n", what, v, (int)getpid(), (int)syscall(SYS_gettid)); fclose(f); }
}
/* r59r probe: text-line logging (extension names, image census) */
static void stept(const char* what, const char* s)
{
    FILE* f = fopen(SEAM_LOG_DIR "/fsr4npu-SEAM.log", "a");
    if (f) { fprintf(f, "%s = %s (pid %d tid %d)\n", what, s && *s ? s : "(empty)", (int)getpid(), (int)syscall(SYS_gettid)); fclose(f); }
}
/* vk interrogation tracer gate + line writer (web-agent round 2 instrument;
 * defined early — used from m_vkCreateDevice and the spoof wrappers) */
static int m_vktrace(void)
{
    static int on = -1;
    if (on < 0) {
        const char* k = getenv("FSR4NPU_VKTRACE");
        on = (k && k[0] == '1') ? 1 : 0;
    }
    return on;
}
static int spoof_cfg(int* fields_out);   /* defined in the spoof section (~line 3300) */
static void m_tr(const char* fmt, ...)
{
    static unsigned n = 0;
    if (n >= 4000) return;   /* cap: trace never starves the disk */
    n++;
    FILE* f = fopen(SEAM_LOG_DIR "/fsr4npu-SEAM.log", "a");
    if (!f) return;
    fprintf(f, "vt[%u] (pid %d) ", n, (int)getpid());
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* r59u: wedge forensics — an independent monitor thread that captures WHAT
 * every thread is sleeping in when the eval stream stalls >30s mid-bench.
 * (Oracle round-5 gap: we only ever observed thread STATES, never the
 * syscall/wchan. Pure /proc/self reads + log writes; touches no Vulkan.
 * Same-process /proc reads need no privileges.) */
static volatile long g_m9_lastev_sec;
static volatile long long g_m9_wedge_evals;   /* r60x: the eval counter dumped at wedge */
static volatile long long g_m9_doorbells;     /* r61: doorbells that reached the eval path entry */
static volatile int g_m9_doorbell_tid;        /* r61: the game thread that last carried evals */

/* r61: read one kgsl sysfs scalar into out (64 bytes; "?" on failure). */
static void m9_read_gpubusy(char* out)
{
    FILE* g = fopen("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", "r");
    if (g) { if (fgets(out, 64, g)) out[strcspn(out, "\n")] = 0; fclose(g); }
    else snprintf(out, 64, "?");
}

/* r61 beast-#2 heartbeat (round-12 plan): one rolling line every 15s once
 * the eval stream starts — the PRE-STALL timeline the 4 forensics dumps
 * can't give. doorbells advancing with evals frozen = layer-internal gate;
 * both frozen + retire frozen = game stopped submitting; retire advancing
 * past evals = GPU alive, pacing upstream. Retire=0 means MARKER is off. */
static void m9_hb_line(void)
{
    FILE* f = fopen(SEAM_LOG_DIR "/fsr4npu-SEAM.log", "a");
    if (!f) return;
    {
        extern volatile int q6_wait_code;
        extern volatile unsigned long long q6_exec_enter_seq, q6_exec_exit_seq, q6_pub_consumed;
        extern volatile unsigned* m9_zc_retire_word(void);
        volatile unsigned* rw = m9_zc_retire_word();
        fprintf(f, "m9-hb t=%ld doorbells=%lld evals=%lld retire=%u wcode=%d wenter=%llu wexit=%llu wpub=%llu\n",
                (long)time(NULL), (long long)g_m9_doorbells, (long long)g_m9_wedge_evals,
                rw ? *rw : 0u, (int)q6_wait_code,
                (unsigned long long)q6_exec_enter_seq, (unsigned long long)q6_exec_exit_seq,
                (unsigned long long)q6_pub_consumed);
    }
    fclose(f);
}

static void m9_wedge_forensics(void)
{
    DIR* d = opendir("/proc/self/task");
    FILE* f = fopen(SEAM_LOG_DIR "/fsr4npu-SEAM.log", "a");
    if (!d || !f) { if (d) closedir(d); if (f) fclose(f); stepm("m9-wedge-forensics-fail", 0); return; }
    struct dirent* e;
    int n = 0;
    fprintf(f, "m9-wedge-forensics-begin (pid %d)\n", (int)getpid());
    /* r60x: the state snapshot the bug reports want — GPU sysfs + rpcmem
     * fdinfo + eval counter + retire word, all root-free, all at wedge time */
    {
        const char* kgsl[] = { "gpu_busy_percentage", "devfreq/cur_freq", "devfreq/min_freq",
                               "devfreq/max_freq", "gpuclk", "thermal_pwrlevel", "force_clk_on" };
        unsigned int k;
        for (k = 0; k < sizeof kgsl / sizeof kgsl[0]; k++) {
            char p[128], lb[128];
            snprintf(p, sizeof p, "/sys/class/kgsl/kgsl-3d0/%s", kgsl[k]);
            FILE* g = fopen(p, "r");
            if (g) { if (fgets(lb, sizeof lb, g)) { lb[strcspn(lb, "\n")] = 0; fprintf(f, "m9-wedge-gpu %s=%s\n", kgsl[k], lb); } fclose(g); }
        }
        { char lb[256]; FILE* g = fopen("/proc/meminfo", "r");
          if (g) { unsigned int ln = 0;
            while (fgets(lb, sizeof lb, g) && ln < 6) {
                if (strstr(lb, "MemFree") || strstr(lb, "MemAvailable") || strstr(lb, "SwapFree") || strstr(lb, "SwapTotal")) {
                    lb[strcspn(lb, "\n")] = 0; fprintf(f, "m9-wedge-mem %s", lb); fprintf(f, "\n"); ln++; } }
            fclose(g); } }
        fprintf(f, "m9-wedge-evals %lld\n", (long long)g_m9_wedge_evals);
        { extern volatile unsigned* m9_zc_retire_word(void);
          volatile unsigned* rw = m9_zc_retire_word();
          if (rw) fprintf(f, "m9-wedge-retire %u\n", *rw); }
        /* r61 beast-#2 discriminators (round-12 plan):
         * (a) doorbells vs evals — doorbells advancing while evals are frozen
         *     = the stall is INSIDE our doorbell path, not the game's.
         * (b) worker enter/exit + wait code — enter>exit or code==2 = stuck in
         *     graphExecute (hypothesis C); code==0 = idle, evals stopped
         *     arriving (upstream, A/B).
         * (c) gpu_busy double-sample 1s apart — a CHANGING value = GPU live
         *     (hypothesis B territory); identical nonzero = suspect latched. */
        fprintf(f, "m9-wedge-doorbells %lld tid=%d\n", (long long)g_m9_doorbells, (int)g_m9_doorbell_tid);
        {
            extern volatile int q6_wait_code;
            extern volatile unsigned long long q6_exec_enter_seq, q6_exec_exit_seq, q6_pub_consumed;
            extern volatile double q6_exec_start_ms;
            struct timespec nw; double age = -1;
            clock_gettime(CLOCK_MONOTONIC, &nw);
            if (q6_exec_start_ms > 0) age = (double)nw.tv_sec * 1e3 + (double)nw.tv_nsec / 1e6 - q6_exec_start_ms;
            fprintf(f, "m9-wedge-worker code=%d enter=%llu exit=%llu pub=%llu exec-age-ms=%.0f\n",
                    (int)q6_wait_code,
                    (unsigned long long)q6_exec_enter_seq, (unsigned long long)q6_exec_exit_seq,
                    (unsigned long long)q6_pub_consumed, age);
        }
        { char b1[64], b2[64];
          struct timespec dd = { 1, 0 };
          m9_read_gpubusy(b1); nanosleep(&dd, 0); m9_read_gpubusy(b2);
          fprintf(f, "m9-wedge-gpu-busy2 %s -> %s\n", b1, b2); }
    }
    while ((e = readdir(d)) != NULL) {
        int tid = atoi(e->d_name);
        if (tid <= 0) continue;
        char p[96], comm[64] = "?", stat[96] = "", sysc[96] = "na", wch[64] = "na";
        FILE* g;
        snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
        if ((g = fopen(p, "r"))) { if (fgets(comm, sizeof comm, g)) comm[strcspn(comm, "\n")] = 0; fclose(g); }
        snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
        if ((g = fopen(p, "r"))) { if (!fgets(stat, sizeof stat, g)) stat[0] = 0; fclose(g); }
        stat[52] = 0;   /* pid (comm) state + the first scheduler fields */
        snprintf(p, sizeof p, "/proc/self/task/%d/syscall", tid);
        if ((g = fopen(p, "r"))) { if (!fgets(sysc, sizeof sysc, g)) snprintf(sysc, sizeof sysc, "running"); else sysc[strcspn(sysc, "\n")] = 0; fclose(g); }
        snprintf(p, sizeof p, "/proc/self/task/%d/wchan", tid);
        if ((g = fopen(p, "r"))) { if (fgets(wch, sizeof wch, g)) wch[strcspn(wch, "\n")] = 0; fclose(g); }
        fprintf(f, "m9-wedge tid=%d comm=%s stat=%s syscall=%s wchan=%s\n", tid, comm, stat, sysc, wch);
        n++;
    }
    closedir(d);
    fprintf(f, "m9-wedge-forensics-end threads=%d\n", n);
    fclose(f);
}
static void* m9_wedge_monitor(void* unused)
{
    int dumps = 0;
    int hb = 0;
    prctl(PR_SET_NAME, "fsr4npu-mon", 0, 0, 0);   /* r61: name ourselves in the task dump */
    for (;;) {
        struct timespec ts = { 5, 0 };
        nanosleep(&ts, 0);
        if (g_m9_lastev_sec && (++hb % 3) == 0) m9_hb_line();   /* r61: 15s heartbeat once evals started */
        long now = (long)time(NULL);
        if (g_m9_lastev_sec && now - g_m9_lastev_sec > 30) {
            /* r60x: re-armed capture — a legit 7s pause must not consume the
             * only shot; dumps repeat (max 4, >=120s apart) while stalled and
             * stop when the eval stream recovers or the cap is hit. */
            static long last_dump;
            if (dumps < 4 && now - last_dump >= 120) {
                last_dump = now;
                dumps++;
                m9_wedge_forensics();
                stepm("m9-wedge-dump", dumps);
            }
        }
    }
    return 0;
}

/* ---- REV58b: signal-trap bracket for game-device creates ----
 * REV58a wchan law: 1874+ vkCreateImageView entries per bench, ZERO
 * ok/fail returns, thread population EXACTLY stable, no abort in logcat,
 * bench alive in Fail mode. Only surviving mechanism: the call faults
 * (SIGSEGV/SIGABRT in the wrapper), wine/box64's signal->exception
 * machinery unwinds PAST our C frame (hence no ok/fail log) and the
 * game's job thread survives. This bracket installs its own handlers,
 * catches the fault FIRST, logs signal+fault addr+code+tid, and escapes
 * via siglongjmp so the return path completes measurably.
 * Dispositions are process-wide: foreign-thread faults during the
 * bracket are delegated to the saved previous handlers. Single-flight:
 * a second concurrent caller skips (logs) rather than clobbering env. */
static sigjmp_buf g_cre_jmp;
static volatile int g_cre_armed;      /* 0 disarmed, else armed */
static volatile int g_cre_tid;
static volatile long g_cre_sig, g_cre_addr, g_cre_code;
static struct sigaction g_cre_old[3];
static const int g_cre_sigs[3] = { SIGSEGV, SIGBUS, SIGABRT };
static void g_cre_handler(int sig, siginfo_t* si, void* uc)
{
    if (!g_cre_armed || syscall(SYS_gettid) != g_cre_tid) {
        int idx = -1;
        for (int i = 0; i < 3; i++) if (g_cre_sigs[i] == sig) idx = i;
        if (idx >= 0) {
            if (g_cre_old[idx].sa_flags & SA_SIGINFO) { if (g_cre_old[idx].sa_sigaction) g_cre_old[idx].sa_sigaction(sig, si, uc); }
            else if (g_cre_old[idx].sa_handler && g_cre_old[idx].sa_handler != SIG_DFL && g_cre_old[idx].sa_handler != SIG_IGN)
                g_cre_old[idx].sa_handler(sig);
            else { signal(sig, SIG_DFL); raise(sig); }
        }
        return;
    }
    g_cre_sig  = sig;
    g_cre_addr = si ? (long)si->si_addr : 0;
    g_cre_code = si ? (long)si->si_code : 0;
    g_cre_armed = 0;
    siglongjmp(g_cre_jmp, 1);
}
/* returns the VkResult, or -999 on caught signal (see g_cre_* logs) */
static int guarded_create(const char* tag, int32_t (*fn)(void*, const void*, const void*, void**), void* dev, const void* ci, void** out)
{
    static volatile int in_flight;
    if (!__sync_bool_compare_and_swap(&in_flight, 0, 1)) { stepm("m9-cre-busy", 0); return -998; }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = g_cre_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (int i = 0; i < 3; i++) sigaction(g_cre_sigs[i], &sa, &g_cre_old[i]);
    g_cre_tid = (int)syscall(SYS_gettid);
    int32_t r = -997;
    g_cre_armed = 1;
    if (sigsetjmp(g_cre_jmp, 1) == 0) {
        r = fn(dev, ci, NULL, out);
        g_cre_armed = 0;
    } else {
        g_cre_armed = 0;  /* set by handler before longjmp; keep for clarity */
        stepm("m9-cre-SIG", g_cre_sig);
        stepm("m9-cre-addr", g_cre_addr);
        stepm("m9-cre-code", g_cre_code);
        r = -999;
    }
    for (int i = 0; i < 3; i++) sigaction(g_cre_sigs[i], &g_cre_old[i], NULL);
    stepm(tag, (long)r);
    in_flight = 0;
    return (int)r;
}
#else
static void stepm(const char* what, long v) { (void)what; (void)v; }
#endif

static void* dfn(const char* name);
static int m_is_game_process(void);   /* REV58e process gate (defined at exports) */
static int m_npu_proc(void);          /* rev3a NPU-carrier gate (game OR FSR4NPU_NPU/BENCH env) */
/* r58l: swapchain capture — images from vkGetSwapchainImagesKHR, current
 * index from vkAcquireNextImageKHR; lets us clear the PRESENTED image. */
static unsigned long long g_sw_images[8];
static unsigned int g_sw_img_count, g_sw_idx;

/* ---- REV19: the ROUTE-RACE chassis ----
 * rev18 fact set: own-ICD instance ✓ enum ✓ device ✓, then the FIRST
 * device-fn call kills the chassis thread with NO marker reached. Two
 * resolution routes died (ogdpa = igipa's vkGetDeviceProcAddr, and the
 * igipa(inst,name) trampoline). Critical tree finding: wv0_compute.c's
 * own-ICD leg NEVER actually ran — chain-first always won in the bench
 * (its ICD leg is the "legacy escape shape" whose instance create used
 * to crash pre-r26e). So NO device-fn route on an own-ICD device is
 * proven anywhere on this stack.
 * rev19 races all four candidate routes on isolated staggered threads,
 * every resolution AND every call markered, wv0's queue-first order,
 * wv0's exact struct shapes (natural layout — no phantom pads), and
 * the full chain through submit+fence+readback verify. First route to
 * VERDICT-PASS stashes the objects and arms the doorbell dispatch.
 * Routes (staggered 3s apart so a silent death can't mask the rest):
 *   A: fn = igipa(inst,"vkGetDeviceProcAddr")(dev,name)   [wv0-canonical]
 *   D: fn = gipa (inst,"vkGetDeviceProcAddr")(dev,name)   [loader-canonical]
 *   C: fn = gipa (inst,name)                              [real-GIPA direct]
 *   B: fn = igipa(inst,name)                              [rev18 shape, markers] */
static void* (*g_r19_igipa)(void*, const char*);
static void* (*g_r19_gipa)(void*, const char*);
static void* (*g_r19_ogdpa)(void*, const char*);
static void* (*g_r19_dgdpa)(void*, const char*);
static void* g_r19_inst, *g_r19_pd, *g_r19_dev;
static volatile int g_r19_winner;
static int g_m9_dev_tid;   /* r59s: tid that created the game VkDevice (the seat/NGX init thread) — route A gate */
static int m9_zc_harness_call;   /* r60y: m_comp_ensure runs FOR the ZCHARN harness — skip its route-race threads */
/* VkPhysicalDeviceMemoryProperties LP64: types@+4, heaps@+264, sizeof 520 */static struct { uint32_t cnt; struct { uint32_t props, heap; } types[32];
                uint32_t heapCnt;
                struct { unsigned long long size; uint32_t flags; uint32_t pad; } heaps[16]; } g_r19_mp;

typedef void* (*r19_res_fn)(const char* name);
static void* r19_resA(const char* n) { return g_r19_ogdpa ? g_r19_ogdpa(g_r19_dev, n) : NULL; }
static void* r19_resD(const char* n) { return g_r19_dgdpa ? g_r19_dgdpa(g_r19_dev, n) : NULL; }
static void* r19_resC(const char* n) { return g_r19_gipa  ? g_r19_gipa(g_r19_inst, n) : NULL; }
static void* r19_resB(const char* n) { return g_r19_igipa ? g_r19_igipa(g_r19_inst, n) : NULL; }

#include <stdio.h>
static void r19m(const char* t, const char* step, long v)
{
    char b[96];
    snprintf(b, sizeof b, "r19%s-%s", t, step);
    stepm(b, v);
}

/* The full wv0 chain on the own device, all fns resolved through `res`.
 * Returns 1 only on submit+fence+verified readback. */
static int r19_chain(const char* T, r19_res_fn res)
{
    /* queue FIRST — wv0 checklist (d) */
    void (*gq)(void*, uint32_t, uint32_t, void**) =
        (void (*)(void*, uint32_t, uint32_t, void**))res("vkGetDeviceQueue");
    r19m(T, "res-qfn", (long)(intptr_t)gq);
    void* queue = NULL;
    if (gq) gq(g_r19_dev, 0, 0, &queue);
    r19m(T, "queue", (long)(intptr_t)queue);
    if (!queue) { r19m(T, "no-queue", 0); return 0; }

    int32_t (*mkc)(void*, const void*, const void*, void**) =
        (int32_t (*)(void*, const void*, const void*, void**))res("vkCreateBuffer");
    void (*grq)(void*, void*, void*) =
        (void (*)(void*, void*, void*))res("vkGetBufferMemoryRequirements");
    int32_t (*alm)(void*, const void*, const void*, void**) =
        (int32_t (*)(void*, const void*, const void*, void**))res("vkAllocateMemory");
    int32_t (*bdm)(void*, void*, void*, unsigned long long) =
        (int32_t (*)(void*, void*, void*, unsigned long long))res("vkBindBufferMemory");
    int32_t (*mpm)(void*, void*, unsigned long long, unsigned long long, uint32_t, void**) =
        (int32_t (*)(void*, void*, unsigned long long, unsigned long long, uint32_t, void**))res("vkMapMemory");
    r19m(T, "res-buf", (long)(intptr_t)mkc);
    r19m(T, "res-req", (long)(intptr_t)grq);
    r19m(T, "res-mem", (long)(intptr_t)alm);
    if (!mkc || !grq || !alm || !bdm || !mpm) { r19m(T, "res-hole", 0); return 0; }

    /* two 1KB STORAGE buffers (wv0-exact: N=256 u32, usage 0x20) */
    struct { uint32_t sType; const void* pNext; uint32_t flags; unsigned long long size;
             uint32_t usage; uint32_t sharing; uint32_t qfiN; const uint32_t* qfis; } bci =
        { 12, NULL, 0, 256ull * 4ull, 0x20, 0, 0, NULL };
    void *bufA = NULL, *bufB = NULL;
    int32_t ra = mkc(g_r19_dev, &bci, NULL, &bufA);
    r19m(T, "bufA-rc", ra);
    if (ra != 0 || !bufA) { r19m(T, "bufA-fail", 1); return 0; }
    int32_t rb = mkc(g_r19_dev, &bci, NULL, &bufB);
    r19m(T, "bufB-rc", rb);
    if (rb != 0 || !bufB) { r19m(T, "bufB-fail", 1); return 0; }
    struct { unsigned long long size, alignment; uint32_t bits; } rq;
    memset(&rq, 0, sizeof rq);
    grq(g_r19_dev, bufA, &rq);
    r19m(T, "bufrq-bits", (long)rq.bits);
    r19m(T, "bufrq-size", (long)rq.size);
    /* HOST_VISIBLE|HOST_COHERENT pick (wv0/wv4 law: no non-coherent fallback) */
    int mt = -1;
    for (uint32_t t = 0; t < g_r19_mp.cnt && t < 32; t++)
        if ((rq.bits >> t) & 1 && (g_r19_mp.types[t].props & 0x6) == 0x6) { mt = (int)t; break; }
    r19m(T, "memtype", mt);
    if (mt < 0) return 0;
    struct { uint32_t sType; const void* pNext; unsigned long long size; uint32_t type; } mai =
        { 5, NULL, rq.size, (uint32_t)mt };
    void *memA = NULL, *memB = NULL;
    if (alm(g_r19_dev, &mai, NULL, &memA) != 0 || !memA) { r19m(T, "memA-fail", 1); return 0; }
    if (alm(g_r19_dev, &mai, NULL, &memB) != 0 || !memB) { r19m(T, "memB-fail", 1); return 0; }
    r19m(T, "mems-ok", 2);
    if (bdm(g_r19_dev, bufA, memA, 0) != 0) { r19m(T, "bindA-fail", 1); return 0; }
    if (bdm(g_r19_dev, bufB, memB, 0) != 0) { r19m(T, "bindB-fail", 1); return 0; }
    r19m(T, "binds-ok", 2);
    uint32_t* pA = NULL;
    if (mpm(g_r19_dev, memA, 0, rq.size, 0, (void**)&pA) != 0 || !pA) { r19m(T, "mapA-fail", 1); return 0; }
    for (uint32_t i = 0; i < 256; i++) pA[i] = i;
    /* NO unmap — platform law: leave mappings alive */

    /* shader + pipeline objects */
    int32_t (*mko)(void*, const void*, const void*, void**) =
        (int32_t (*)(void*, const void*, const void*, void**))res("vkCreateShaderModule");
    r19m(T, "res-sh", (long)(intptr_t)mko);
    struct { uint32_t sType; const void* pNext; uint32_t flags;
             unsigned long long codeSize; const uint32_t* pCode; } sci =
        { 16, NULL, 0, (unsigned long long)(WV0_SPV_WORDS * 4), wv0_spv };
    void* module = NULL;
    if (!mko || mko(g_r19_dev, &sci, NULL, &module) != 0 || !module) { r19m(T, "mod-fail", 1); return 0; }
    r19m(T, "mod-ok", 1);

    struct { uint32_t binding, type, count, stages; const void* imm; } binds[2] = {
        { 0, 7, 1, 0x20, NULL }, { 1, 7, 1, 0x20, NULL } };
    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t n; const void* b; } dslci =
        { 32, NULL, 0, 2, binds };
    void* setLayout = NULL;
    int32_t (*mkl)(void*, const void*, const void*, void**) =
        (int32_t (*)(void*, const void*, const void*, void**))res("vkCreateDescriptorSetLayout");
    r19m(T, "res-dsl", (long)(intptr_t)mkl);
    if (!mkl || mkl(g_r19_dev, &dslci, NULL, &setLayout) != 0 || !setLayout) { r19m(T, "dsl-fail", 1); return 0; }
    r19m(T, "dsl-ok", 1);

    struct { uint32_t stageFlags, offset, size; } pcr = { 0x20, 0, 8 };
    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t setN; const void* const* sets;
             uint32_t pcN; const void* pcs; } plci = { 30, NULL, 0, 1, &setLayout, 1, &pcr };
    void* pipeLayout = NULL;
    int32_t (*mkp)(void*, const void*, const void*, void**) =
        (int32_t (*)(void*, const void*, const void*, void**))res("vkCreatePipelineLayout");
    r19m(T, "res-pl", (long)(intptr_t)mkp);
    if (!mkp || mkp(g_r19_dev, &plci, NULL, &pipeLayout) != 0 || !pipeLayout) { r19m(T, "playout-fail", 1); return 0; }
    r19m(T, "playout-ok", 1);

    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t stage; const void* module;
             const char* name; const void* spec; } stg = { 18, NULL, 0, 0x20, module, "main", NULL };
    struct { uint32_t sType; const void* pNext; uint32_t flags; const void* stage; const void* layout;
             const void* base; int32_t baseIdx; } cpci = { 29, NULL, 0, &stg, pipeLayout, NULL, -1 };
    void* pipe = NULL;
    int32_t (*mkcp)(void*, void*, uint32_t, const void*, const void*, void**) =
        (int32_t (*)(void*, void*, uint32_t, const void*, const void*, void**))res("vkCreateComputePipelines");
    r19m(T, "res-cp", (long)(intptr_t)mkcp);
    int32_t rp = mkcp ? mkcp(g_r19_dev, NULL, 1, &cpci, NULL, &pipe) : -3;
    r19m(T, "pipe-rc", rp);
    if (rp != 0 || !pipe) { r19m(T, "pipe-fail", 1); return 0; }
    r19m(T, "pipe-ok", 1);

    struct { uint32_t type, count; } psz = { 7, 2 };
    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t maxSets;
             uint32_t n; const void* sizes; } poolci = { 33, NULL, 0, 1, 1, &psz };
    void* dpool = NULL;
    int32_t (*mkdp)(void*, const void*, const void*, void**) =
        (int32_t (*)(void*, const void*, const void*, void**))res("vkCreateDescriptorPool");
    if (!mkdp || mkdp(g_r19_dev, &poolci, NULL, &dpool) != 0 || !dpool) { r19m(T, "pool-fail", 1); return 0; }
    struct { uint32_t sType; const void* pNext; void* pool; uint32_t count; const void* layouts; } dsai =
        { 34, NULL, dpool, 1, &setLayout };
    void* ds = NULL;
    int32_t (*ads)(void*, const void*, void**) =
        (int32_t (*)(void*, const void*, void**))res("vkAllocateDescriptorSets");
    r19m(T, "res-ads", (long)(intptr_t)ads);
    if (!ads || ads(g_r19_dev, &dsai, &ds) != 0 || !ds) { r19m(T, "set-fail", 1); return 0; }
    r19m(T, "set-ok", 1);

    struct { void* buffer; unsigned long long off, range; } bi[2] = {
        { bufA, 0, 256ull * 4ull }, { bufB, 0, 256ull * 4ull } };
    struct { uint32_t sType; const void* pNext; void* set; uint32_t dst, elem, n, type;
             const void* ii; const void* bi_; const void* ti; } wr[2];
    memset(wr, 0, sizeof wr);
    wr[0].sType = 35; wr[0].set = ds; wr[0].dst = 0; wr[0].n = 1; wr[0].type = 7; wr[0].bi_ = &bi[0];
    wr[1].sType = 35; wr[1].set = ds; wr[1].dst = 1; wr[1].n = 1; wr[1].type = 7; wr[1].bi_ = &bi[1];
    void (*uds)(void*, uint32_t, const void*, uint32_t, const void*) =
        (void (*)(void*, uint32_t, const void*, uint32_t, const void*))res("vkUpdateDescriptorSets");
    r19m(T, "res-uds", (long)(intptr_t)uds);
    if (!uds) return 0;
    uds(g_r19_dev, 2, wr, 0, NULL);
    r19m(T, "desc-updated", 1);

    /* own CB + submit + fence + readback verify (wv0 tail) */
    int32_t (*mkcpool)(void*, const void*, const void*, void**) =
        (int32_t (*)(void*, const void*, const void*, void**))res("vkCreateCommandPool");
    int32_t (*mkfence)(void*, const void*, const void*, void**) =
        (int32_t (*)(void*, const void*, const void*, void**))res("vkCreateFence");
    r19m(T, "res-cpool", (long)(intptr_t)mkcpool);
    r19m(T, "res-fence", (long)(intptr_t)mkfence);
    if (!mkcpool || !mkfence) { r19m(T, "tail-res-hole", 0); return 0; }
    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t qfam; } cpci2 = { 39, NULL, 0, 0 };
    void* cpool = NULL;
    if (mkcpool(g_r19_dev, &cpci2, NULL, &cpool) != 0 || !cpool) { r19m(T, "cpool-fail", 1); return 0; }
    struct { uint32_t sType; const void* pNext; void* pool; uint32_t level; uint32_t count; } cbai =
        { 40, NULL, cpool, 0, 1 };
    void* cb = NULL;
    int32_t (*acb)(void*, const void*, void**) =
        (int32_t (*)(void*, const void*, void**))res("vkAllocateCommandBuffers");
    r19m(T, "res-acb", (long)(intptr_t)acb);
    if (!acb || acb(g_r19_dev, &cbai, &cb) != 0 || !cb) { r19m(T, "cb-fail", 1); return 0; }
    struct { uint32_t sType; const void* pNext; uint32_t flags; const void* inherit; } bbi = { 42, NULL, 0, NULL };
    int32_t (*bcb)(void*, const void*) = (int32_t (*)(void*, const void*))res("vkBeginCommandBuffer");
    r19m(T, "res-bcb", (long)(intptr_t)bcb);
    if (!bcb || bcb(cb, &bbi) != 0) { r19m(T, "cbbegin-fail", 1); return 0; }
    void (*bp)(void*, uint32_t, void*) = (void (*)(void*, uint32_t, void*))res("vkCmdBindPipeline");
    void (*bs)(void*, uint32_t, void*, uint32_t, uint32_t, const void*, uint32_t, const void*) =
        (void (*)(void*, uint32_t, void*, uint32_t, uint32_t, const void*, uint32_t, const void*))res("vkCmdBindDescriptorSets");
    void (*pc_)(void*, void*, uint32_t, uint32_t, uint32_t, const void*) =
        (void (*)(void*, void*, uint32_t, uint32_t, uint32_t, const void*))res("vkCmdPushConstants");
    void (*dp)(void*, uint32_t, uint32_t, uint32_t) =
        (void (*)(void*, uint32_t, uint32_t, uint32_t))res("vkCmdDispatch");
    int32_t (*ecb)(void*) = (int32_t (*)(void*))res("vkEndCommandBuffer");
    if (!bp || !bs || !pc_ || !dp || !ecb) { r19m(T, "cmdfn-hole", 0); return 0; }
    bp(cb, 1, pipe);
    bs(cb, 1, pipeLayout, 0, 1, &ds, 0, NULL);
    const uint32_t pc[2] = { 0x01010101u, 0xdeadbeefu };
    pc_(cb, pipeLayout, 0x20, 0, 8, pc);
    dp(cb, 4, 1, 1);
    if (ecb(cb) != 0) { r19m(T, "endcb-fail", 1); return 0; }
    r19m(T, "cb-recorded", 1);
    struct { uint32_t sType; const void* pNext; uint32_t flags; } fci = { 8, NULL, 0 };
    void* fence = NULL;
    if (mkfence(g_r19_dev, &fci, NULL, &fence) != 0 || !fence) { r19m(T, "fence-fail", 1); return 0; }
    int32_t (*qs)(void*, uint32_t, const void*, void*) =
        (int32_t (*)(void*, uint32_t, const void*, void*))res("vkQueueSubmit");
    int32_t (*wf)(void*, uint32_t, const void* const*, uint32_t, unsigned long long) =
        (int32_t (*)(void*, uint32_t, const void* const*, uint32_t, unsigned long long))res("vkWaitForFences");
    struct { uint32_t sType; const void* pNext; uint32_t waitN; const void* const* waits; const uint32_t* stages;
             uint32_t cmdN; const void* const* cmds; uint32_t sigN; const void* const* sigs; } si =
        { 4, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
    if (!qs || !wf) { r19m(T, "qfn-hole", 0); return 0; }
    int32_t rs = qs(queue, 1, &si, fence);
    r19m(T, "submit-rc", rs);
    if (rs != 0) return 0;
    int32_t rw = wf(g_r19_dev, 1, &fence, 1, 5000000000ull);
    r19m(T, "waitfence-rc", rw);
    if (rw != 0) return 0;

    uint32_t* pB = NULL;
    if (mpm(g_r19_dev, memB, 0, rq.size, 0, (void**)&pB) != 0 || !pB) { r19m(T, "mapB-fail", 1); return 0; }
    uint32_t bad = 0;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t exp = i * 0x01010101u + 0xdeadbeefu;
        if (pB[i] != exp) bad++;
    }
    r19m(T, "readback-bad", (long)bad);
    if (bad != 0) { r19m(T, "VERDICT-FAIL", 1); return 0; }
    /* WINNER — stash for the doorbell dispatch (bind gated on BINDTEST marker) */
    if (__sync_lock_test_and_set(&g_r19_winner, 1) == 0) {
        g_cbuf_a = bufA; g_cbuf_b = bufB;
        g_cmodule = module; g_cdsl = setLayout; g_cplayout = pipeLayout;
        g_cpipe = pipe; g_cdpool = dpool; g_cset = ds;
        g_comp_state = 1;
    }
    r19m(T, "VERDICT-PASS", 1);
    return 1;
}

static void* r19_route_thread(void* arg)
{
    long idx = (long)(intptr_t)arg;
    static const char* TAGS[4] = { "A", "D", "C", "B" };
    static r19_res_fn RES[4] = { r19_resA, r19_resD, r19_resC, r19_resB };
    prctl(PR_SET_NAME, "fsr4npu-route", 0, 0, 0);   /* r61 */
    usleep((useconds_t)(idx * 2000000));      /* stagger: A@0s D@2s C@4s B@6s */
    if (g_comp_state) { r19m(TAGS[idx], "skip-won", 1); return NULL; }
    r19m(TAGS[idx], "start", idx);
    r19_chain(TAGS[idx], RES[idx]);
    r19m(TAGS[idx], "chain-end", g_comp_state);
    return NULL;
}

static void m_comp_ensure(void)   /* rev19: setup + route race */
{
    if (g_comp_state || !g_chain_dev) { if (!g_comp_state) g_comp_state = -1; return; }
    usleep(3000000);   /* wv0-style settle (checklist c; rev18 had only 50ms) */
    /* Armada port (plan §4.2, probe_layer.c:95-98 lineage): the Android
     * wrapper/adrenotools/imagefs ICD chain is DELETED — on Armada the real
     * loader + Turnip ICD are in the pressure-vessel container (host ICDs
     * imported per plan §2.2: overrides/share/vulkan/icd.d/
     * freedreno_icd.aarch64.json). The probe-device chassis dlopens the
     * Turnip ICD library DIRECTLY (never the loader — re-entering the
     * loader from inside a layer would recurse this layer into itself),
     * bare name first (container ld path holds the imported ICD), then
     * the host /usr/lib64 location via /run/host. Every leg markered. */
    void* lv = dlopen("libvulkan_freedreno.so", 2);
    stepm("r21-dlopen-icd-bare", (long)(intptr_t)lv);
    g_r19_igipa = lv ? (void* (*)(void*, const char*))dlsym(lv, "vk_icdGetInstanceProcAddr") : NULL;
    stepm("r21-igipa-icd-bare", (long)(intptr_t)g_r19_igipa);
    if (!g_r19_igipa) {
        /* fallbacks, each markered: container/usr absolute, then the host
         * Mesa install exposed at /run/host (the MangoHud /run/host law) */
        lv = dlopen("/usr/lib64/libvulkan_freedreno.so", 2);
        stepm("r20-dlopen-icd-usr", (long)(intptr_t)lv);
        if (lv) {
            g_r19_igipa = (void* (*)(void*, const char*))dlsym(lv, "vk_icdGetInstanceProcAddr");
            stepm("r20-igipa-icd-usr", (long)(intptr_t)g_r19_igipa);
        }
        if (!g_r19_igipa) {
            lv = dlopen("/run/host/usr/lib64/libvulkan_freedreno.so", 2);
            stepm("r20-dlopen-icd-host", (long)(intptr_t)lv);
            if (lv) {
                g_r19_igipa = (void* (*)(void*, const char*))dlsym(lv, "vk_icdGetInstanceProcAddr");
                stepm("r20-igipa-icd-host", (long)(intptr_t)g_r19_igipa);
            }
        }
    }
    stepm("r19-icd-handle", (long)(intptr_t)lv);
    if (!lv) return;
    stepm("r19-igipa", (long)(intptr_t)g_r19_igipa);
    if (!g_r19_igipa) return;
    /* own instance — wv0-exact shapes (r26e-fixed) */
    struct { uint32_t sType; const void* pNext; const char* a; uint32_t av;
             const char* e; uint32_t ev; uint32_t api; } ai =
        { 0, NULL, "fsr4npu_r19", 0, "r19", 0, 0x403000 };
    struct { uint32_t sType; const void* pNext; uint32_t flags; const void* app;
             uint32_t lc; const void* pl; uint32_t ec; const void* pe; } ci =
        { 1, NULL, 0, &ai, 0, NULL, 0, NULL };
    int32_t (*ciFn)(const void*, const void*, void**) =
        (int32_t (*)(const void*, const void*, void**))g_r19_igipa(NULL, "vkCreateInstance");
    void* inst = NULL;
    int32_t ric = ciFn ? ciFn(&ci, NULL, &inst) : -3;
    stepm("r19-inst", ric);
    if (ric != 0 || !inst) return;
    g_r19_inst = inst;
    g_r19_gipa = (void* (*)(void*, const char*))g_r19_igipa(inst, "vkGetInstanceProcAddr");
    stepm("r19-gipa", (long)(intptr_t)g_r19_gipa);
    if (!g_r19_gipa) return;
    /* enumerate */
    int32_t (*en)(void*, uint32_t*, void**) =
        (int32_t (*)(void*, uint32_t*, void**))g_r19_gipa(inst, "vkEnumeratePhysicalDevices");
    uint32_t np = 0; void* pds[2] = { 0 };
    int32_t ren = en ? en(inst, &np, NULL) : -3;
    if (ren == 0 && np >= 1 && en) { uint32_t one = (np > 1 ? 1 : np); en(inst, &one, pds); }
    stepm("r19-enum", ren);
    if (ren != 0 || !pds[0]) return;
    g_r19_pd = pds[0];
    /* memory props — instance-level via gipa (shared by all routes) */
    void (*fmp)(void*, void*) =
        (void (*)(void*, void*))g_r19_gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    if (fmp) { memset(&g_r19_mp, 0, sizeof g_r19_mp); fmp(g_r19_pd, &g_r19_mp); stepm("r19-memtypes", g_r19_mp.cnt); }
    /* own device — wv0-exact QCI/DevCI. r60 feed9w: with fsr4npu-ZCHARN the
     * own-device boots with external memory ENABLED (advertised != enabled:
     * the runtime hides entry points for non-enabled extensions — the same
     * mechanism we suspect behind every layout2 nofn). Absent = zero change. */
    static const char* zc_exts[2] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf" };
    uint32_t zc_ec = access(SEAM_LOG_DIR "/fsr4npu-ZCHARN", F_OK) == 0 ? 2u : 0u;
    if (zc_ec) stepm("m9-zc-exts", zc_ec);
    static const float prio = 1.0f;
    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t fam; uint32_t cnt; const float* prio; } qci =
        { 2, NULL, 0, 0, 1, &prio };
    struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t qn; const void* qp;
             uint32_t lc; const void* pl; uint32_t ec; const void* pe; const void* fe; } dci =
        { 3, NULL, 0, 1, &qci, 0, NULL, zc_ec, zc_exts, NULL };
    int32_t (*cdFn)(void*, const void*, const void*, void**) =
        (int32_t (*)(void*, const void*, const void*, void**))g_r19_gipa(inst, "vkCreateDevice");
    void* dev = NULL;
    int32_t rdev = cdFn ? cdFn(g_r19_pd, &dci, NULL, &dev) : -3;
    stepm("r19-dev", rdev);
    if (rdev != 0 || !dev) return;
    g_r19_dev = dev;
    /* the two vkGetDeviceProcAddr variants — values logged; calls raced below */
    g_r19_ogdpa = (void* (*)(void*, const char*))g_r19_igipa(inst, "vkGetDeviceProcAddr");
    g_r19_dgdpa = (void* (*)(void*, const char*))g_r19_gipa(inst, "vkGetDeviceProcAddr");
    stepm("r19-ogdpa", (long)(intptr_t)g_r19_ogdpa);
    stepm("r19-dgdpa", (long)(intptr_t)g_r19_dgdpa);
    stepm("r19-setup-done", 1);
    if (!m9_zc_harness_call) {   /* r60y: harness invocations skip the route-race threads (timing purity) */
        for (long i = 0; i < 4; i++) {
            pthread_t th;
            if (pthread_create(&th, NULL, r19_route_thread, (void*)(intptr_t)i) == 0) pthread_detach(th);
            else stepm("r19-spawn-fail", i);
        }
    }
}
static void* chassis_thread(void* arg)
{
    (void)arg;
    stepm("r19-chassis-thread-start", 1);
    m_comp_ensure();
    stepm("r19-chassis-setup-done", g_comp_state);   /* route threads outlive this */
    return NULL;
}

// ---- creates: pure passthrough + link capture + protocol write-back ----
static void* g_layer_inst;   /* the game's VkInstance (captured at create; instance-level fn resolution for ZCC2) */
static int32_t m_vkCreateInstance(const void* ci, const void* alloc, void** out)
{
    /* REV48d: arm the WRAPPER's own logging BEFORE the chain initializes —
     * the wrapper reads WRAPPER_LOG_LEVEL/FILE when the ICD initializes,
     * which happens on the down-call below. The wrapper is the prime
     * suspect for the unattributed create-freeze (compiled-in __assert2
     * aborts on foreign handles; silent device mutation). */
    int game = m_is_game_process();
    if (game) {
        setenv("WRAPPER_LOG_LEVEL", "info", 1);
        setenv("WRAPPER_LOG_FILE", SEAM_LOG_DIR "/wrapper.log", 1);
        setenv("MESA_VK_ABORT_ON_DEVICE_LOSS", "1", 1);   /* r59l diag: a GPU fault tombstones instead of hanging silently */
        /* r59u: TU_DEBUG untouched (the container's noconform passes through;
         * flushall was boot 44's discriminator and is retired — UBWC fully on;
         * the scoped ICC chain in m_vkCreateImage stays chained by default,
         * fsr4npu-NOICC is its off switch). */
    }
    VkLayerInstanceLink__* link = NULL;
    VkLayerInstanceCreateInfo__* n;
    if (!ci) return -3;
    for (n = (VkLayerInstanceCreateInfo__*)((const void**)ci)[1]; n;
         n = (VkLayerInstanceCreateInfo__*)n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && n->function == 0 && n->pLayerInfo) { link = n->pLayerInfo; break; }
    if (!link) return -3;                     // never re-enter the loader
    /* r58j FIX: capture UNCONDITIONALLY — the r58g game-gate left g_down_gipa
     * NULL in non-game processes (Launcher/SocialClub), so vkGetInstanceProcAddr
     * returned NULL for every function -> the Launcher's Vulkan init died ->
     * RDR2.exe never started. Captures are per-process and inert without the
     * seam hooks; only the HOOK SUBSTITUTION stays game-gated (gdpa export). */
    g_down_gipa = (void* (*)(void*, const char*))link->pfnNextGetInstanceProcAddr;
    // loader protocol: advance the chain past us before forwarding
    for (n = (VkLayerInstanceCreateInfo__*)((const void**)ci)[1]; n;
         n = (VkLayerInstanceCreateInfo__*)n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && n->function == 0) { n->pLayerInfo = link->pNext; break; }
    void* (*down_gipa_here)(void*, const char*) =
        (void* (*)(void*, const char*))link->pfnNextGetInstanceProcAddr;
    int32_t (*down_create)(const void*, const void*, void**) =
        (int32_t (*)(const void*, const void*, void**))down_gipa_here(0, "vkCreateInstance");
    if (!down_create) return -3;
    int32_t rc = down_create(ci, alloc, out); // ORIGINAL create-info, unmodified
    if (rc == 0 && out && *out) g_layer_inst = *out;   /* ZCC2: instance-level fn resolution for the game-device import (§23.16) */
    /* ARMADA deploy check: ONE unconditional attach marker (M0 lineage —
     * the pid line that proved loading inside NMS). Fires in ANY process
     * whose chain the layer rode, game or not, so the CLI positive/negative
     * vulkaninfo check is decidable without launching the game. */
    stepm("min2-ci-seen", rc);
    if (game) stepm("min2-ci-rc", rc);
    if (rc == 0 && out && *out && !g_chain_inst) g_chain_inst = *out;   /* r59g: for vkGetPhysicalDeviceMemoryProperties */
    return rc;
}

static unsigned long long g3_buf, g3_mem, g3_mod, g3_dsl, g3_playout, g3_pipe, g3_pool, g3_set;
static unsigned long long g3_route_src;   /* the PROBE-device fp16 route bank */
static unsigned int g3_osw, g3_osh;
static int g3_state;                      /* 0 untouched, 1 ok, -1 failed */
static void m9_g3_ensure(void);
/* REV42: delayed chassis builder — see m_vkCreateDevice. */
static void* m9_g3_build_thread(void* arg)
{
    (void)arg;
    usleep(5000000);          /* let the game's device-create chain unwind */
    m9_g3_ensure();
    stepm("m9-g3-built", g3_state);
    return NULL;
}
static int32_t m_vkCreateDevice(void* pd, const void* ci, const void* alloc, void** out)
{
    int game = m_is_game_process();
    VkLayerDeviceLink__* link = NULL;
    VkLayerDeviceCreateInfo__* n;
    if (!ci) return -3;
    for (n = (VkLayerDeviceCreateInfo__*)((const void**)ci)[1]; n;
         n = (VkLayerDeviceCreateInfo__*)n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && n->function == 0 && n->pLayerInfo) { link = n->pLayerInfo; break; }
    if (!link) return -3;
    g_down_gdpa = (void* (*)(void*, const char*))link->pfnNextGetDeviceProcAddr;   /* r58j: unconditional (see instance hook) */
    {   /* canon: the DEVICE link carries its own gipa — vkCreateDevice is
         * resolved through IT (probe_layer.c:557-567, live-proven), never
         * through the device gdpa with a NULL device. */
        void* (*devlink_gipa)(void*, const char*) = (void* (*)(void*, const char*))link->pfnNextGetInstanceProcAddr;
        void* (*gipa_use)(void*, const char*) = devlink_gipa ? devlink_gipa : g_down_gipa;
        for (n = (VkLayerDeviceCreateInfo__*)((const void**)ci)[1]; n;
             n = (VkLayerDeviceCreateInfo__*)n->pNext)
            if (n->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && n->function == 0) { n->pLayerInfo = link->pNext; break; }
        if (!gipa_use) return -3;
        down_gipa_for_device = gipa_use;   /* r58j: unconditional */
        int32_t (*down_create)(void*, const void*, const void*, void**) =
            (int32_t (*)(void*, const void*, const void*, void**))gipa_use(0, "vkCreateDevice");
        if (!down_create) { if (game) stepm("min2-cd-nodown", 1); return -3; }
        /* web-agent round 4: once the DLSS gate opens, Streamline will REQUEST our
         * fake NV/NVX extensions at device create; Turnip would fail the device with
         * EXTENSION_NOT_PRESENT. Strip them from the enabled list for the down-call
         * (names duplicated here — the canonical SPOOF_NV_EXTS list is defined later
         * in the file, after this function). */
        const char* ci_use = (const char*)ci;
        char ci_copy[72];
        const char* extfilt[120];
        {   /* ZCC2-NMS (r60f port, 2026-10-11): the record path imports the
         * worker's rpcmem onto the GAME device — that needs
         * VK_EXT_external_memory_dma_buf + VK_KHR_external_memory_fd enabled,
         * which the game never does (the m9-era imports only ever ran on the
         * harness's OWN extension-enabled device; first game-device attempt
         * SIGILL'd at the first in-game import — runbook §23.16). Turnip
         * advertises both; append them after a physical-device support check
         * so device create can never fail with EXTENSION_NOT_PRESENT. */
            static const char* const zc_exts[2] = {
                "VK_EXT_external_memory_dma_buf", "VK_KHR_external_memory_fd",
            };
            if (game && ci) {
                uint32_t eec; memcpy(&eec, ci_use + 48, 4);
                const char* const* exts = *(const char* const* const*)(ci_use + 56);
                int have[2] = { 0, 0 };
                for (uint32_t i = 0; i < eec && i < 120 && exts && exts[i]; i++) {
                    if (!strcmp(exts[i], zc_exts[0])) have[0] = 1;
                    if (!strcmp(exts[i], zc_exts[1])) have[1] = 1;
                }
                if ((!have[0] || !have[1]) && eec <= 118) {
                    /* Turnip advertises both (vulkaninfo verified live
                     * 2026-10-10) — append unconditionally. NO enumeration
                     * here: physical-device fns can't resolve through the
                     * NULL-instance gipa path that works for vkCreateDevice
                     * (the edep attempt = instant boot death, §23.16). */
                    uint32_t keep = 0, added = 0, i;
                    for (i = 0; i < eec && i < 118 && exts && exts[i]; i++) extfilt[keep++] = exts[i];
                    if (!have[0]) { extfilt[keep++] = zc_exts[0]; added++; }
                    if (!have[1]) { extfilt[keep++] = zc_exts[1]; added++; }
                    memcpy(ci_copy, ci_use, 72);
                    memcpy(ci_copy + 48, &keep, 4);
                    { uint64_t p = (uint64_t)(uintptr_t)extfilt; memcpy(ci_copy + 56, &p, 8); }
                    ci_use = ci_copy;
                    stepm("m9-cd-zcext", (long)added);
                }
            }
        }
        {
            if (spoof_cfg(NULL) && ci) {
                uint32_t eec; memcpy(&eec, ci_use + 48, 4);
                const char* const* exts = *(const char* const* const*)(ci_use + 56);
                if (exts && eec && eec <= 120) {
                    static const char* const fakes[] = {
                        "VK_NVX_binary_import", "VK_NVX_image_view_handle",
                        "VK_NV_low_latency2", "VK_NV_low_latency",
                        "VK_EXT_buffer_device_address",
                        /* launch-5: SL adds its compute chassis unconditionally;
                         * AMD_buffer_marker exists on NV+AMD drivers, not Turnip */
                        "VK_AMD_buffer_marker",
                    };
                    uint32_t keep = 0; int dropped = 0;
                    for (uint32_t i = 0; i < eec; i++) {
                        const char* nm = exts[i];
                        int fake = 0;
                        for (unsigned j = 0; j < 6; j++)
                            if (nm && !strcmp(nm, fakes[j])) { fake = 1; break; }
                        if (fake) dropped++;
                        else extfilt[keep++] = nm;
                    }
                    if (dropped) {
                        memcpy(ci_copy, ci_use, 72);
                        memcpy(ci_copy + 48, &keep, 4);
                        { uint64_t p = (uint64_t)(uintptr_t)extfilt; memcpy(ci_copy + 56, &p, 8); }
                        stepm("m9-cd-extstrip", (long)dropped);
                        ci_use = ci_copy;
                    }
                }
            }
            /* 3x-fix regression (2026-10-10): under the REAL Adreno identity
             * the game passes queueFamilyIndex=VK_QUEUE_FAMILY_IGNORED
             * (0xFFFF) in its VkDeviceQueueCreateInfo — wine tolerates the
             * game's own create, but Streamline's wrapper COPIES this
             * createInfo and merges its compute queues into that malformed
             * entry; the merged create then fails ("vkCreateDevice failed"
             * → SL unloads). Rewrite 0xFFFF→family 0 for every queue entry
             * so SL inherits a well-formed request. 0xFFFF is never a valid
             * family index, so the rewrite is unconditionally safe. */
            {
                uint32_t qcc; memcpy(&qcc, ci_use + 20, 4);
                const char* qci = *(const char* const*)(ci_use + 24);
                {   /* unconditional probe: what does THIS block actually see? */
                    static unsigned qp = 0;
                    if (qci && qp < 3) {
                        qp++;
                        uint32_t fam0; memcpy(&fam0, qci + 20, 4);
                        m_tr("qfixProbe call=%u qcc=%u fam0=0x%x", qp, qcc, fam0);
                    }
                }
                if (qci && qcc && qcc <= 16) {
                    static char qpatch[16][40];
                    int fixed = 0;
                    for (uint32_t i = 0; i < qcc; i++) {
                        memcpy(qpatch[i], qci + (uint64_t)i * 40, 40);
                        uint32_t fam; memcpy(&fam, qpatch[i] + 20, 4);
                        if (fam == 0xFFFFFFFFu) {
                            uint32_t z = 0;
                            memcpy(qpatch[i] + 20, &z, 4);
                            fixed++;
                        }
                    }
                    if (fixed) {
                        if (ci_use != ci_copy) memcpy(ci_copy, ci_use, 72);
                        { uint64_t p = (uint64_t)(uintptr_t)qpatch; memcpy(ci_copy + 24, &p, 8); }
                        ci_use = ci_copy;
                        stepm("m9-cd-qfix", (long)fixed);
                    }
                }
            }
        }
        if (m_vktrace() && ci) {   /* VkDeviceCreateInfo: qcc@20 pQCI@24 eec@48 pExt@56 pNext@8 */
            const char* c = (const char*)ci;
            uint32_t qcc, eec;
            memcpy(&qcc, c + 20, 4); memcpy(&eec, c + 48, 4);
            m_tr("createDevice qcc=%u eec=%u", qcc, eec);
            const char* qci = *(const char* const*)(c + 24);
            for (uint32_t i = 0; i < qcc && i < 8 && qci; i++) {   /* VkDeviceQueueCreateInfo 40B: fam@20 cnt@24 */
                uint32_t fam, cnt;
                memcpy(&fam, qci + (uint64_t)i * 40 + 20, 4);
                memcpy(&cnt, qci + (uint64_t)i * 40 + 24, 4);
                m_tr("cd.q[%u] fam=%u cnt=%u", i, fam, cnt);
            }
            const char* const* exts = *(const char* const* const*)(c + 56);
            for (uint32_t i = 0; i < eec && i < 80 && exts && exts[i]; i++)
                m_tr("cd.ext %s", exts[i]);
            const char* h = *(const char* const*)(c + 8);
            for (int i = 0; h && i < 12; i++) {
                uint32_t st; memcpy(&st, h, 4);
                m_tr("cd.pnext st=%u", st);
                h = *(const char* const*)(h + 8);
            }
        }
        int32_t rc = down_create(pd, ci_use, alloc, out);
        {   /* launch-6 forensics: dump every ext name we sent down + rc,
             * first 3 game creates, always on (VKTRACE not required). */
            static unsigned ncalls = 0;
            if (game && ci && ncalls < 3) {
                ncalls++;
                uint32_t eec2; memcpy(&eec2, ci_use + 48, 4);
                const char* const* ex2 = *(const char* const* const*)(ci_use + 56);
                m_tr("cdF%u rc=%ld eec=%u", ncalls, (long)rc, eec2);
                for (uint32_t i = 0; i < eec2 && i < 80 && ex2 && ex2[i]; i++)
                    m_tr("cdF%u.ext %s", ncalls, ex2[i]);
            }
        }
        if ((game || m_npu_proc()) && rc == 0 && out && *out && !g_chain_dev) {
            g_chain_dev = *out;   // capture ONCE (chassis armed at first DOORBELL — rev20;
                                  // rev3a: also for NPU-carrier procs — xbench now, the game's
                                  // FSR4NPU_NPU deploy later — the doorbell record path needs it)
            if (!g_m9_dev_tid) g_m9_dev_tid = (int)syscall(SYS_gettid);   /* r59s route A gate */
            if (!g_chain_pd) {
                g_chain_pd = (void*)pd;
                /* r59g: fill g_r19_mp from the GAME'S device — every live path
                 * found it empty and m9_pick_mem_type fell back to type 0 (the
                 * staging reads stayed write-combined: ~230 MB/s = the record
                 * hook's dominant CPU cost). One instance-level call. */
                if (g_down_gipa && g_chain_inst && !g_r19_mp.cnt) {
                    void (*fmpg)(void*, void*) = (void (*)(void*, void*))g_down_gipa(g_chain_inst, "vkGetPhysicalDeviceMemoryProperties");
                    if (fmpg) { memset(&g_r19_mp, 0, sizeof g_r19_mp); fmpg(g_chain_pd, &g_r19_mp); stepm("r59g-memtypes", g_r19_mp.cnt); }
                }
                /* r59r probe: driver identity + device-extension census (the
                 * UBWC scoped re-enable decision: is VK_EXT_image_compression_
                 * control advertised on THIS driver?). Logging only. */
                if (g_down_gipa && g_chain_inst) {
                    void (*fpdp)(void*, void*) = (void (*)(void*, void*))g_down_gipa(g_chain_inst, "vkGetPhysicalDeviceProperties");
                    if (fpdp) {
                        static char pdbuf[1024];
                        memset(pdbuf, 0, sizeof pdbuf);
                        fpdp(g_chain_pd, pdbuf);
                        uint32_t apiver = *(const uint32_t*)(pdbuf + 0);
                        uint32_t drvver = *(const uint32_t*)(pdbuf + 4);
                        stepm("m9-probe-apiver", (long)apiver);
                        stepm("m9-probe-drvver-raw", (long)drvver);
                        /* VK_VERSION unpack of driverVersion (Mesa packs its x.y.z) */
                        stepm("m9-probe-drvver-mmpp",
                              (long)((drvver >> 22) * 1000000 + ((drvver >> 12) & 0x3ff) * 1000 + (drvver & 0xfff)));
                        stept("m9-probe-devname", pdbuf + 20);   /* deviceName at +20 (deviceType@16) */
                    }
                    uint32_t (*fenc)(void*, const char*, uint32_t*, void*) =
                        (uint32_t (*)(void*, const char*, uint32_t*, void*))g_down_gipa(g_chain_inst, "vkEnumerateDeviceExtensionProperties");
                    if (fenc) {
                        static char extbuf[256 * 260];          /* VK_MAX_EXTENSION_NAME 256 + specVersion */
                        uint32_t n = 0;
                        uint32_t erc = fenc(g_chain_pd, NULL, &n, NULL);
                        if (erc == 0 && n && n <= 256 && fenc(g_chain_pd, NULL, &n, (void*)extbuf) == 0) {
                            stepm("m9-probe-extcount", (long)n);
                            uint32_t k, icc = 0;
                            for (k = 0; k < n; k++) {
                                const char* nm = extbuf + (size_t)k * 260;
                                uint32_t spec = *(const uint32_t*)(nm + 256);
                                char ln[300];
                                snprintf(ln, sizeof ln, "%s spec=%u", nm, spec);
                                stept("m9-probe-ext", ln);
                                if (strstr(nm, "compression")) icc = 1;
                            }
                            stepm("m9-probe-icc", (long)icc);   /* 1 = route A unblocked */
                        } else {
                            stepm("m9-probe-extcount-fail", (long)((uint64_t)erc << 32 | n));
                        }
                    }
                }
            }
            /* REV42: REV41 built the chassis HERE and deadlocked the game's
             * own device-creation path (runs 3m/3n: zero device-create
             * markers — the game never got a device and fell into the
             * attract loop). Instead: a thread that waits out the create
             * chain, then builds during the game's menu-load window — long
             * before any recording starts (3g/3l: creates during recording
             * deadlock; 3m/3n: creates inside the create call deadlock; the
             * load window between is the safe zone). */
            /* REV43: NO chassis build at all. REV41 (inline) deadlocked the
             * create; REV42 (detached thread) hung inside a create and the
             * game died mid-load (runs 3m/3n/verdict/this). The law is
             * absolute: the layer NEVER creates on the game device — the
             * probe device (seat-side, at ARM) is the only sanctioned
             * surface. Stage 3 stays gated off; this is the 3f-proven
             * fill-only configuration (acks 2000/2000). */

            /* REV47 (PQ2, L4): one-time env dump — the NATIVE-side view of
             * the wrapper/driver surface (the seat dumps the wine-process
             * view at ARM; this is the container's). Any non-default value
             * keeps the C11 wrapper-mangle cluster alive. Logging only. */
            {
                static const char* const evs[] = {
                    "WRAPPER_EMULATE_BCN", "WRAPPER_LOG_LEVEL",
                    "WRAPPER_MAX_IMAGE_COUNT", "MESA_VK_WSI_PRESENT_MODE", "TU_DEBUG"
                };
                unsigned int ei;
                for (ei = 0; ei < sizeof evs / sizeof evs[0]; ei++) {
                    const char* v = getenv(evs[ei]);
                    char tag[64];
                    if (!v) { snprintf(tag, sizeof tag, "m9-env-%s-set", evs[ei]); stepm(tag, 0); continue; }
                    snprintf(tag, sizeof tag, "m9-env-%s-set", evs[ei]); stepm(tag, 1);
                    {   /* first 8 chars packed two-per-long */
                        uint32_t lo = 0, hi = 0;
                        unsigned int ci;
                        for (ci = 0; ci < 4 && v[ci]; ci++) ((unsigned char*)&lo)[ci] = (unsigned char)v[ci];
                        for (; ci < 8 && v[ci]; ci++)       ((unsigned char*)&hi)[ci - 4] = (unsigned char)v[ci];
                        snprintf(tag, sizeof tag, "m9-env-%s-lo", evs[ei]); stepm(tag, (long)lo);
                        snprintf(tag, sizeof tag, "m9-env-%s-hi", evs[ei]); stepm(tag, (long)hi);
                    }
                }
            }
        }
        if (game) stepm("min2-cd-rc", rc);
        return rc;
    }
    return -3;
}

// ---- the four seam trampolines (down resolved per-call via caller's dev) ----

/* r59r probe: VkImageCreateInfo LP64 mirror (no vulkan.h in this build) */
struct m9_ImgCI_ { uint32_t sType; void* pNext; uint32_t flags, imageType, format;
                   uint32_t w, h, d, mipLevels, arrayLayers, samples, tiling, usage,
                   sharingMode, qfic; void* qfi; uint32_t initialLayout; };
/* ================= r60 feed9w: ZERO-COPY IMPORT HARNESS (oracle rounds 8-9) =================
 * Gated by fsr4npu-ZCHARN (absent = nothing runs; the own-device also boots
 * without the external-memory extensions). Runs on the LAYER'S OWN r19
 * device — never the game device (REV43). Phase 1 at worker start: the API
 * questions + import probe + GPU->rpcmem write test. Phase 2, one-shot after
 * the first real graphExecute: DSP->GPU adjudication + negative control.
 * All results as m9-zc-* markers. */
#include <sys/ioctl.h>
struct m9_zc_sync_s_ { uint64_t flags; };
#define M9_ZC_IOCTL_SYNC _IOW('b', 0, struct m9_zc_sync_s_)
static void m9_zc_sync(int fd, uint64_t f) { struct m9_zc_sync_s_ s; s.flags = f; (void)!ioctl(fd, M9_ZC_IOCTL_SYNC, &s); }
static void* m9_zc_res(const char* n) { return g_r19_dgdpa ? g_r19_dgdpa(g_r19_dev, n) : NULL; }
static void* m9_zc_ires(const char* n) { return g_r19_gipa ? g_r19_gipa(g_r19_inst, n) : NULL; }
static int m9_zc_on = -1;    /* marker latch */
static void* g_zc_cbuf = NULL, * g_zc_fence = NULL, * g_zc_queue = NULL;
static unsigned long long g_zc_stgBuf = 0, g_zc_stgMem = 0; static unsigned int* g_zc_stgMap = NULL;
static unsigned long long g_zc_inBuf = 0, g_zc_inMem = 0, g_zc_outBuf = 0, g_zc_outMem = 0;
static int g_zc_in_fd = -1, g_zc_out_fd = -1; static void* g_zc_in_ptr; static void* g_zc_out_ptr;
static unsigned long long g_zc_in_n = 0, g_zc_out_n = 0;
/* retire-marker (fsr4npu-MARKER): the 16B game-device buffer the eval path
 * seq-stamps; the worker reads the mapped word for the lag distribution. */
static unsigned long long g_retBuf = 0, g_retMem = 0; static unsigned* g_retMap = NULL; static int g_retOn = -1;

/* one own-device submit: CB = copy(src->dst), then reset/submit/wait. 0 = retired clean. */
static int m9_zc_do_copy(unsigned long long dst, unsigned long long src, unsigned long long n)
{
    int32_t (*bcb)(void*, const void*) = (int32_t (*)(void*, const void*))m9_zc_res("vkBeginCommandBuffer");
    int32_t (*ecb)(void*) = (int32_t (*)(void*))m9_zc_res("vkEndCommandBuffer");
    void (*cb2b)(void*, unsigned long long, unsigned long long, unsigned long long, unsigned int, const void*) =
        (void (*)(void*, unsigned long long, unsigned long long, unsigned long long, unsigned int, const void*))m9_zc_res("vkCmdCopyBuffer");
    int32_t (*rf)(void*, uint32_t, const void*) = (int32_t (*)(void*, uint32_t, const void*))m9_zc_res("vkResetFences");
    int32_t (*rs)(void*, uint32_t, const void*, uint32_t, const void*, void*) =
        (int32_t (*)(void*, uint32_t, const void*, uint32_t, const void*, void*))m9_zc_res("vkQueueSubmit");
    int32_t (*wf)(void*, uint32_t, const void*, uint64_t) =
        (int32_t (*)(void*, uint32_t, const void*, uint64_t))m9_zc_res("vkWaitForFences");
    struct { uint32_t sType; const void* pNext; uint32_t flags; const void* inherit; } bbi;
    struct { uint32_t sType; const void* pNext; void* cb; uint32_t mask; uint64_t val; } sfi;
    if (!bcb || !ecb || !cb2b || !rf || !rs || !wf || !g_zc_cbuf || !g_zc_fence || !g_zc_queue) return -10;
    memset(&bbi, 0, sizeof bbi); bbi.sType = 42;
    if (bcb(g_zc_cbuf, &bbi) != 0) return -11;
    cb2b(g_zc_cbuf, dst, src, n, 1, NULL);
    if (ecb(g_zc_cbuf) != 0) return -12;
    rf(g_r19_dev, 1, (const void*)&g_zc_fence);
    memset(&sfi, 0, sizeof sfi); sfi.sType = 6; sfi.cb = g_zc_cbuf; sfi.mask = 0; sfi.val = 0;
    if (rs(g_zc_queue, 1, (const void*)&sfi, 1, (const void*)&g_zc_fence, NULL) != 0) return -13;
    return wf(g_r19_dev, 1, (const void*)&g_zc_fence, 5000000000ull);
}

/* retire-marker support (fsr4npu-MARKER): the worker reads this word */
volatile unsigned* m9_zc_retire_word(void) { return (volatile unsigned*)g_retMap; }

static int zcgpu_ok(void) { static int v = -1; if (v < 0) v = access(SEAM_LOG_DIR "/fsr4npu-ZCGPU", F_OK) == 0; return v; }
void m9_zc_harness_begin(int fd_in, void* p_in, unsigned long long n_in,
                         int fd_out, void* p_out, unsigned long long n_out)
{
    if (m9_zc_on < 0) m9_zc_on = access(SEAM_LOG_DIR "/fsr4npu-ZCHARN", F_OK) == 0;
    if (!m9_zc_on) return;
    stepm("m9-zc-begin", 1);
    g_zc_in_fd = fd_in; g_zc_in_ptr = p_in; g_zc_in_n = n_in;
    g_zc_out_fd = fd_out; g_zc_out_ptr = p_out; g_zc_out_n = n_out;
    /* (1) fdinfo of both rpcmem fds — the heap identity (exp_name) + size */
    { int fds[2] = { fd_in, fd_out }; int k;
      for (k = 0; k < 2; k++) { char p[96], lb[512]; FILE* f; int hits = 0;
        snprintf(p, sizeof p, "/proc/self/fdinfo/%d", fds[k]);
        if ((f = fopen(p, "r"))) { while (fgets(lb, sizeof lb, f)) {
            if (strstr(lb, "exp_name:") || strstr(lb, "size:") || strstr(lb, "ino:")) { hits++;
                lb[strcspn(lb, "\n")] = 0; char tag[64]; snprintf(tag, sizeof tag, "m9-zc-fd%d-info%d", k, hits); stept(tag, lb); } }
            fclose(f); }
        stepm(k ? "m9-zc-fdout-info" : "m9-zc-fdin-info", hits);
      } }
    if (!g_r19_dev || !g_r19_pd) {
        /* r60y: the r19 chassis is DEAD CODE on recent builds (nothing spawns
         * chassis_thread) — build it NOW for the harness: 3s settle inside,
         * no route-race threads (harness_call flag), own device gets the
         * external-memory extensions when ZCHARN is on. */
        if (g_chain_dev) { m9_zc_harness_call = 1; m_comp_ensure(); m9_zc_harness_call = 0; }
        if (!g_r19_dev || !g_r19_pd) { stepm("m9-zc-nodev", 0); return; }
    }
    /* (2) API questions: external buffer properties for our usage (DEDICATED_ONLY?) */
    { void (*ebp)(void*, const void*, void*) = (void (*)(void*, const void*, void*))m9_zc_ires("vkGetPhysicalDeviceExternalBufferProperties");
      stepm("m9-zc-ebp-fn", (long)(intptr_t)ebp);
      if (ebp) { struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t usage;
                          uint32_t handleType; } bi = { 1000071001, NULL, 0, 0x22u, 0x200u };
        struct { uint32_t sType; const void* pNext; uint32_t compat; uint32_t export_, import_; } bp;
        memset(&bp, 0, sizeof bp); bp.sType = 1000071002;
        ebp(g_r19_pd, &bi, &bp);
        stepm("m9-zc-ebp-compat", bp.compat);
        stepm("m9-zc-ebp-import", bp.import_);   /* bit0 = IMPORTABLE, bit3 = DEDICATED_ONLY */
      } }
    /* (3) staging buffer (own device, mapped, 64KB pattern block) */
    { int32_t (*fcb)(void*, const void*, const void*, void**) = (int32_t (*)(void*, const void*, const void*, void**))m9_zc_res("vkCreateBuffer");
      int32_t (*fam)(void*, const void*, const void*, void**) = (int32_t (*)(void*, const void*, const void*, void**))m9_zc_res("vkAllocateMemory");
      int32_t (*fbb)(void*, void*, void*, uint64_t) = (int32_t (*)(void*, void*, void*, uint64_t))m9_zc_res("vkBindBufferMemory");
      int32_t (*fmp)(void*, void*, uint64_t, uint64_t, uint32_t, void**) = (int32_t (*)(void*, void*, uint64_t, uint64_t, uint32_t, void**))m9_zc_res("vkMapMemory");
      struct { uint32_t sType; const void* pNext; uint32_t flags; unsigned long long size;
               uint32_t usage, sharing; uint32_t qfic; const void* qfi; uint32_t init; } bci;
      struct { uint32_t sType; const void* pNext; unsigned long long size; uint32_t type; } mai;
      if (!fcb || !fam || !fbb || !fmp) { stepm("m9-zc-fn", 0); return; }
      memset(&bci, 0, sizeof bci); bci.sType = 9; bci.size = 65536ull; bci.usage = 0x23u;
      if (fcb(g_r19_dev, &bci, NULL, (void**)&g_zc_stgBuf) != 0 || !g_zc_stgBuf) { stepm("m9-zc-stg-buf", 1); return; }
      memset(&mai, 0, sizeof mai); mai.sType = 5; mai.size = 65536ull;
      { uint32_t t; mai.type = 0;
        for (t = 0; t < g_r19_mp.cnt && t < 32; t++)
            if ((g_r19_mp.types[t].props & 0xFu) >= 0x3u) { mai.type = t; break; } }   /* HOST_VISIBLE|COHERENT */
      if (fam(g_r19_dev, &mai, NULL, (void**)&g_zc_stgMem) != 0 || !g_zc_stgMem) { stepm("m9-zc-stg-mem", 1); return; }
      if (fbb(g_r19_dev, (void*)g_zc_stgBuf, (void*)g_zc_stgMem, 0) != 0) { stepm("m9-zc-stg-bind", 1); return; }
      if (fmp(g_r19_dev, (void*)g_zc_stgMem, 0, ~(unsigned long long)0, 0, (void**)&g_zc_stgMap) != 0 || !g_zc_stgMap) { stepm("m9-zc-stg-map", 1); return; }
      stepm("m9-zc-stg-ok", 1); }
    /* (4) import probe: for each rpcmem fd — buffer + DMA_BUF pNext chain,
     *     fdprops memoryTypeBits, allocate (dup'd fd), bind */
    { int32_t (*fcb)(void*, const void*, const void*, void**) = (int32_t (*)(void*, const void*, const void*, void**))m9_zc_res("vkCreateBuffer");
      void (*fbr)(void*, void*, void*) = (void (*)(void*, void*, void*))m9_zc_res("vkGetBufferMemoryRequirements");
      int32_t (*fam)(void*, const void*, const void*, void**) = (int32_t (*)(void*, const void*, const void*, void**))m9_zc_res("vkAllocateMemory");
      int32_t (*fbb)(void*, void*, void*, uint64_t) = (int32_t (*)(void*, void*, void*, uint64_t))m9_zc_res("vkBindBufferMemory");
      int32_t (*gfp)(void*, uint32_t, int, void*) = (int32_t (*)(void*, uint32_t, int, void*))m9_zc_res("vkGetMemoryFdPropertiesKHR");
      stepm("m9-zc-gfp-fn", (long)(intptr_t)gfp);
      struct { uint32_t sType; const void* pNext; uint32_t handleTypes; } xb = { 1000072000, NULL, 0x200u };
      struct { uint32_t sType; const void* pNext; uint32_t flags; unsigned long long size;
               uint32_t usage, sharing; uint32_t qfic; const void* qfi; uint32_t init; } bci;
      int which;
      for (which = 0; which < 2; which++) {
        int fd = which ? g_zc_out_fd : g_zc_in_fd;
        unsigned long long n = which ? g_zc_out_n : g_zc_in_n;
        struct { uint32_t sType; const void* pNext; uint32_t memoryTypeBits; } fdp;
        memset(&fdp, 0, sizeof fdp); fdp.sType = 1000074001;
        int32_t rg = gfp ? gfp(g_r19_dev, 0x200u, fd, &fdp) : -9;
        stepm(which ? "m9-zc-out-fdprops-rc" : "m9-zc-in-fdprops-rc", rg);
        if (rg == 0) stepm(which ? "m9-zc-out-mtbits" : "m9-zc-in-mtbits", fdp.memoryTypeBits);
        memset(&bci, 0, sizeof bci); bci.sType = 9; bci.pNext = &xb; bci.size = n; bci.usage = which ? 0x22u : 0x22u;
        unsigned long long* pbuf = which ? &g_zc_outBuf : &g_zc_inBuf;
        int32_t rb = fcb(g_r19_dev, &bci, NULL, (void**)pbuf);
        stepm(which ? "m9-zc-out-buf-rc" : "m9-zc-in-buf-rc", rb);
        if (rb != 0 || !*pbuf) continue;
        struct { uint32_t sType; const void* pNext; uint32_t handleType; int fd; } imp = { 1000074000, NULL, 0x200u, dup(fd) };
        struct { uint32_t sType; const void* pNext; unsigned long long size; uint32_t type; } mai;
        struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t heapCnt;
                 uint32_t pad0; void* pad1; } unused_; (void)unused_;
        memset(&mai, 0, sizeof mai); mai.sType = 5; mai.pNext = &imp; mai.size = n;
        { uint32_t t, bits = rg == 0 ? fdp.memoryTypeBits : 0xFFFFFFFFu; mai.type = 0;
          for (t = 0; t < g_r19_mp.cnt && t < 32; t++)
            if ((bits >> t) & 1u) { mai.type = t; break; } }
        unsigned long long* pmem = which ? &g_zc_outMem : &g_zc_inMem;
        int32_t rm = fam(g_r19_dev, &mai, NULL, (void**)pmem);
        stepm(which ? "m9-zc-out-mem-rc" : "m9-zc-in-mem-rc", rm);
        if (rm != 0 || !*pmem) continue;
        int32_t rbd = fbb(g_r19_dev, (void*)*pbuf, (void*)*pmem, 0);
        stepm(which ? "m9-zc-out-bind-rc" : "m9-zc-in-bind-rc", rbd);
        stepm(which ? "m9-zc-out-imported" : "m9-zc-in-imported", rbd == 0 ? 1 : 0);
      }
    }
    /* (5) own CB + fence + queue for the GPU tests */
    { int32_t (*mkcp)(void*, const void*, const void*, void**) = (int32_t (*)(void*, const void*, const void*, void**))m9_zc_res("vkCreateCommandPool");
      int32_t (*mkfe)(void*, const void*, const void*, void**) = (int32_t (*)(void*, const void*, const void*, void**))m9_zc_res("vkCreateFence");
      void (*gdq)(void*, uint32_t, uint32_t, void**) = (void (*)(void*, uint32_t, uint32_t, void**))m9_zc_res("vkGetDeviceQueue");
      int32_t (*acb2)(void*, const void*, void**) = (int32_t (*)(void*, const void*, void**))m9_zc_res("vkAllocateCommandBuffers");
      struct { uint32_t sType; const void* pNext; uint32_t flags; uint32_t qfam; } cpci = { 39, NULL, 0, 0 };
      struct { uint32_t sType; const void* pNext; uint32_t flags; const void* inherit; } fci = { 11, NULL, 0, NULL };
      struct { uint32_t sType; const void* pNext; void* pool; uint32_t level; uint32_t count; } cbai = { 40, NULL, NULL, 0, 1 };
      if (!mkcp || !mkfe || !gdq || !acb2) { stepm("m9-zc-cmd-fn", 0); return; }
      if (mkcp(g_r19_dev, &cpci, NULL, (void**)&cbai.pool) != 0 || !cbai.pool) { stepm("m9-zc-cpool", 1); return; }
      if (mkfe(g_r19_dev, &fci, NULL, &g_zc_fence) != 0 || !g_zc_fence) { stepm("m9-zc-fence", 1); return; }
      if (acb2(g_r19_dev, &cbai, (void**)&g_zc_cbuf) != 0 || !g_zc_cbuf) { stepm("m9-zc-cb", 1); return; }
      gdq(g_r19_dev, 0, 0, &g_zc_queue);
      stepm("m9-zc-cmd-ok", g_zc_queue ? 1 : 0); }
    /* r60z: fsr4npu-ZCGPU gates the GPU-side tests — import-only boots
     * (ZCHARN alone) prove the imports are inert in-place; +ZCGPU adds the
     * first submit, isolating "imported-memory access faults" from "any
     * foreign submit mid-bench faults" (boot 57: imports OK, first do_copy
     * coincided with the wedge at eval 56). */
    static int zcgpu = -1;
    if (zcgpu < 0) { zcgpu = access(SEAM_LOG_DIR "/fsr4npu-ZCGPU", F_OK) == 0; stepm("m9-zc-gpu-gated", zcgpu); }
    /* (6) GPU->rpcmem write test: staging pattern -> imported input bank, 64 rounds */
    if (zcgpu && g_zc_inBuf && g_zc_stgMap && g_zc_queue) {
      int round, fails = 0; const int TN = 65536;
      for (round = 0; round < 64; round++) {
        unsigned int pat = 0xA5A50000u | (unsigned int)round;
        unsigned int* w = g_zc_stgMap; int i;
        for (i = 0; i < TN / 4; i++) w[i] = pat + (unsigned int)i;
        if (m9_zc_do_copy(g_zc_inBuf, g_zc_stgBuf, 65536ull) != 0) { stepm("m9-zc-w-copy-fail", round); fails = -1000; break; }
        m9_zc_sync(g_zc_in_fd, 1u /*READ*/ | 0u /*START*/);
        { unsigned int* rp = (unsigned int*)g_zc_in_ptr; int bad = 0;
          for (i = 0; i < TN / 4; i += 997u) if (rp[i] != pat + (unsigned int)i) { bad = 1; break; }
          if (bad) fails++; }
        m9_zc_sync(g_zc_in_fd, 1u | 4u /*READ|END*/);
      }
      stepm("m9-zc-gpu2dsp-fails", fails);
      stepm("m9-zc-gpu2dsp-verdict", fails == 0 ? 1 : 0);
    }
    stepm("m9-zc-phase1-done", 1);
}

void m9_zc_harness_postexec(void)
{
    if (!m9_zc_on) return;
    stepm("m9-zc-phase2", 1);
    /* DSP->GPU: the output bank holds a REAL graph output. CPU reference read
     * (post-sync) vs a GPU copy out of the imported bank. 32 rounds over live
     * evals is impossible here (one-shot) — sample the one output we have. */
    if (zcgpu_ok() && g_zc_outBuf && g_zc_stgMap && g_zc_queue && g_zc_out_ptr) {
        unsigned int* ref; unsigned long long n = g_zc_out_n < 65536ull ? g_zc_out_n : 65536ull;
        int i, bad = 0;
        ref = (unsigned int*)malloc((size_t)n);
        if (!ref) { stepm("m9-zc-p2-mem", 1); return; }
        m9_zc_sync(g_zc_out_fd, 1u /*READ*/ | 0u /*START*/);
        memcpy(ref, g_zc_out_ptr, (size_t)n);
        m9_zc_sync(g_zc_out_fd, 1u | 4u /*READ|END*/);
        if (m9_zc_do_copy(g_zc_stgBuf, g_zc_outBuf, n) != 0) { stepm("m9-zc-p2-copy-fail", 1); free(ref); return; }
        for (i = 0; i < (int)(n / 4); i += 509u) if (g_zc_stgMap[i] != ref[i]) { bad++; }
        stepm("m9-zc-dsp2gpu-mismatch", bad);
        stepm("m9-zc-dsp2gpu-verdict", bad == 0 ? 1 : 0);
        /* negative control (comparator sanity): a fresh pattern in staging
         * compared against the OLD reference MUST mismatch — a test that
         * cannot fail proves nothing. */
        { unsigned int* w = g_zc_stgMap; for (i = 0; i < (int)(n / 4); i++) w[i] = 0xDEADBEEFu;
          for (i = 0, bad = 0; i < (int)(n / 4); i += 509u) if (g_zc_stgMap[i] != ref[i]) bad++;
          stepm("m9-zc-negctl-mismatch", bad);
          stepm("m9-zc-negctl-ok", bad > 0 ? 1 : 0); }
        free(ref);
    }
    stepm("m9-zc-phase2-done", 1);
}

/* r59s route A: VK_EXT_image_compression_control raw-ABI mirrors (ext 338) */
struct m9_ICC_     { uint32_t sType; void* pNext; uint32_t flags; uint32_t planeCount; void* pFixedRateFlags; };
struct m9_ICCProps_{ uint32_t sType; void* pNext; uint32_t flags; uint32_t fixedRate; };
struct m9_ImgSub2_ { uint32_t sType; void* pNext; uint32_t aspect; uint32_t mip; uint32_t layer; };
static int g_m9_icc_n;     /* chained-image counter */
/* r60 B1 (oracle round-8 CORRECTED): the ICC chain is KEPT — chain-off is an
 * UNTESTED config (boot46: 18,555 chained creates on T26, i.e. the chain was
 * ACTIVE in every T26+UBWC measurement), so the deployed default stays
 * chain-ON. fsr4npu-NOICC (absent by default) un-chains it for a dedicated
 * future A/B boot. Per-create costs trimmed instead: the down-pointer and the
 * layout2 verifier lookup resolve ONCE (statics), and ALL per-create log
 * lines (census + icc) are gated behind fsr4npu-CENSUS, absent by default. */
static int32_t m_vkCreateImage(void* dev, const void* civ, const void* ac, void** out)
{
    /* r60: resolve ONCE — g_chain_dev is captured once at device create and
     * never overwritten, so the pointer is process-stable; same cached-static
     * pattern as cpyS/barS in the eval path. Concurrent first-call race is
     * benign (both racers store the identical pointer). */
    static int32_t (*down)(void*, const void*, const void*, void**);
    if (!down) {
        down = (int32_t (*)(void*, const void*, const void*, void**))g_down_gdpa(g_chain_dev, "vkCreateImage");
        stepm("m9-cim-down", (long)(intptr_t)(void*)down);            /* rev3a bench forensics */
        stepm("m9-cim-chaindev", (long)(intptr_t)g_chain_dev);
    }
    const struct m9_ImgCI_* ci = (const struct m9_ImgCI_*)civ;
    int chain = 0;
    /* r59u (oracle round-5 boot #2): chain DISABLED on EVERY OPTIMAL image
     * with COLOR_ATTACHMENT(0x10) | DEPTH_STENCIL(0x20) | STORAGE(0x8) usage
     * — any size, any format. BCn textures are usage 0x7 and linear staging
     * is tiling 1 (both auto-excluded; neither was UBWC-eligible anyway per
     * ubwc_possible()). This covers the classes boot 43 left compressed: the
     * 640x360 render chain and ALL depth images. If this still dies, the only
     * compressed images left are ones the layer cannot reach (the
     * wrapper-created swapchain). */
    if (ci && ci->sType == 14 && ci->tiling == 0
        && (ci->usage & (0x10u | 0x20u | 0x8u)))
        chain = 1;
    {   /* r60 B1: fsr4npu-NOICC present => skip the ICC chain this boot
         * (marker absent => deployed behavior: the chain above runs). */
        static int noicc = -1;
        if (noicc < 0) {
            FILE* mf = fopen(SEAM_LOG_DIR "/fsr4npu-NOICC", "r");
            noicc = mf ? (fclose(mf), 1) : 0;
            stepm("m9-noicc", noicc);
        }
        if (noicc) chain = 0;
    }
    /* r60: the create census is an INSTRUMENT, not function (boot46: 74,936
     * m9-img-create FUSE lines/boot, 3.88/eval on the streaming thread in the
     * bench window, 96% of the window's log). Default OFF; the fsr4npu-CENSUS
     * marker re-arms the full census + icc verify for a diagnostic boot. */
    static int census = -1;
    if (census < 0)
        census = access(SEAM_LOG_DIR "/fsr4npu-CENSUS", F_OK) == 0;
    int32_t r;
    if (chain) {
        struct m9_ICC_ icc = { 1000338001u /*COMPRESSION_CONTROL*/, ci->pNext, 4u /*DISABLED*/, 0u, (void*)0 };
        struct m9_ImgCI_ ci2 = *ci;
        ci2.pNext = &icc;
        r = down ? down(dev, &ci2, ac, out) : (int32_t)0xffffffff;
        if (census) stepm("m9-icc-chained", (long)(__sync_add_and_fetch(&g_m9_icc_n, 1)));
        if (census && r == 0 && out && *out) {
            /* verify the driver honored DISABLED: compression props via
             * vkGetImageSubresourceLayout2 (core 1.4), props side-chained.
             * r60: lookup resolved ONCE (static + tried flag — a failed
             * lookup stays failed; T26 exports neither name: 18,555
             * m9-icc-verify-nofn, zero flags reads, boot46). */
            static void (*fl2)(void*, unsigned long long, const void*, void*);
            static int fl2_tried;
            if (!fl2_tried) {
                fl2_tried = 1;
                fl2 = (void (*)(void*, unsigned long long, const void*, void*))g_down_gdpa(g_chain_dev, "vkGetImageSubresourceLayout2");
                if (!fl2) fl2 = (void (*)(void*, unsigned long long, const void*, void*))g_down_gdpa(g_chain_dev, "vkGetImageSubresourceLayout2EXT");
            }
            if (fl2) {
                struct m9_ICCProps_ props = { 1000338004u, (void*)0, 9999u, 9999u };
                struct m9_ImgSub2_ sub  = { 1000338003u, &props, 1u /*COLOR*/, 0u, 0u };
                struct { uint32_t sType; void* pNext; unsigned long long off, size, row, arr, dep; } lay;
                memset(&lay, 0, sizeof lay);
                fl2(dev, *(unsigned long long*)out, &sub, &lay);
                stepm("m9-icc-verify-flags", (long)props.flags);   /* want 4 = DISABLED */
                stepm("m9-icc-verify-fixed", (long)props.fixedRate);
                stepm("m9-icc-layout-size",  (long)lay.size);
            } else stepm("m9-icc-verify-nofn", 0);
        }
    } else {
        r = down ? down(dev, civ, ac, out) : (int32_t)0xffffffff;
    }
    if (census && ci && ci->sType == 14) {   /* VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO */
        char ln[208];
        snprintf(ln, sizeof ln,
                 "fmt=%u %ux%ux%u mips=%u arr=%u smp=%u usage=0x%x tiling=%u flags=0x%x pnext=%s icc=%d h=%p rc=%d",
                 ci->format, ci->w, ci->h, ci->d, ci->mipLevels, ci->arrayLayers,
                 ci->samples, ci->usage, ci->tiling, ci->flags,
                 ci->pNext ? "yes" : "no", chain,
                 (out && r == 0) ? (void*)*(unsigned long long*)out : (void*)0, (int)r);
        stept("m9-img-create", ln);
    }
    return r;
}

static int32_t m_vkCreateBuffer(void* dev, const void* ci, const void* ac, void** out)
{
    uint64_t size = ci ? *(const uint64_t*)((const char*)ci + 24) : 0;
    uint32_t usage = ci ? *(const uint32_t*)((const char*)ci + 32) : 0;
    if (!g_down_gdpa) return -3;
    int32_t (*down)(void*, const void*, const void*, void**) =
        (int32_t (*)(void*, const void*, const void*, void**))g_down_gdpa(dev, "vkCreateBuffer");
    if (!down) return -3;
    int32_t r = down(dev, ci, ac, out);
    if (r == 0 && out && *out) seam_on_create_buffer(r, size, usage, *out);
    return r;
}

static int32_t m_vkBindBufferMemory(void* dev, void* buf, void* mem, unsigned long long off)
{
    if (!g_down_gdpa) return -3;
    int32_t (*down)(void*, void*, void*, unsigned long long) =
        (int32_t (*)(void*, void*, void*, unsigned long long))g_down_gdpa(dev, "vkBindBufferMemory");
    if (!down) return -3;
    int32_t r = down(dev, buf, mem, off);
    if (r == 0) seam_on_bind_buffer(r, buf, mem);
    return r;
}

static int32_t m_vkMapMemory(void* dev, void* mem, unsigned long long off,
                             unsigned long long sz, uint32_t fl, void** pp)
{
    if (!g_down_gdpa) return -3;
    int32_t (*down)(void*, void*, unsigned long long, unsigned long long, uint32_t, void**) =
        (int32_t (*)(void*, void*, unsigned long long, unsigned long long, uint32_t, void**))
            g_down_gdpa(dev, "vkMapMemory");
    if (!down) return -3;
    int32_t r = down(dev, mem, off, sz, fl, pp);
    if (r == 0) seam_on_map_memory(r, mem, pp);
    return r;
}

static void m_fill_ensure(void)
{
    if (g_fill_state || !g_down_gdpa || !g_chain_dev) { if (!g_fill_state) { g_fill_state = -1; stepm("min2-fill-early-fail", 0); } return; }
    /* VkBufferCreateInfo x64: +24 size, +32 usage; TRANSFER_DST only */
    struct { uint32_t sType; const void* pNext; uint32_t flags; char pad0[4];
             unsigned long long size; uint32_t usage; char pad1[4];
             int32_t sharingMode; char pad2[4]; uint32_t qfic; char pad3[4];
             const uint32_t* pqfi; } bci;
    int32_t (*createB)(void*, const void*, const void*, void**) =
        (int32_t (*)(void*, const void*, const void*, void**))g_down_gdpa(g_chain_dev, "vkCreateBuffer");
    struct { unsigned long long size, alignment; uint32_t bits; char pad[4]; } req;
    void (*getreq)(void*, void*, void*) = (void (*)(void*, void*, void*))g_down_gdpa(g_chain_dev, "vkGetBufferMemoryRequirements");
    int32_t (*allocM)(void*, const void*, const void*, void**) =
        (int32_t (*)(void*, const void*, const void*, void**))g_down_gdpa(g_chain_dev, "vkAllocateMemory");
    int32_t (*bindM)(void*, void*, void*, unsigned long long) =
        (int32_t (*)(void*, void*, void*, unsigned long long))g_down_gdpa(g_chain_dev, "vkBindBufferMemory");
    void* mem = NULL;
    memset(&bci, 0, sizeof bci);
    bci.sType = 12;               /* VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO (vulkan_core.h:219; 11 is QUERY_POOL_CREATE_INFO — the old value worked only because turnip screens nothing) */
    bci.size = 16;
    bci.usage = 0x2;              /* VK_BUFFER_USAGE_TRANSFER_DST_BIT (vulkan_core.h:2912; the old 0x20 is STORAGE_BUFFER) */
    if (!createB || createB(g_chain_dev, &bci, NULL, &g_fill_buf) != 0 || !g_fill_buf) { g_fill_state = -1; return; }
    memset(&req, 0, sizeof req);
    if (getreq) getreq(g_chain_dev, g_fill_buf, &req);
    if (!(req.bits & 1u)) { g_fill_state = -1; return; }   /* type 0 not usable */
    {   /* VkMemoryAllocateInfo: sType,pNext,size,memTypeIndex */
        struct { uint32_t sType; const void* pNext; unsigned long long size; uint32_t type; char pad[4]; } mai;
        memset(&mai, 0, sizeof mai);
        mai.sType = 5; mai.size = req.size > 16 ? req.size : 16; mai.type = 0;
        if (!allocM || allocM(g_chain_dev, &mai, NULL, &mem) != 0 || !mem) { g_fill_state = -1; return; }
        if (!bindM || bindM(g_chain_dev, g_fill_buf, mem, 0) != 0) { g_fill_state = -1; return; }
    }
    {
        /* REV49: THE SUBMISSION TEST — map the fill buffer. The fill writes
         * 0x46495234 into this GAME-device buffer on the GAME's command
         * buffer every eval since REV15; if the CPU ever sees the magic,
         * the eval CB submits AND game-device objects execute. If it never
         * appears, every command we ever recorded was a phantom. */
        void* map = NULL;
        struct { uint32_t sType; char p0[4]; const void* pNext; unsigned long long off, size; void** pp; } mm;
        memset(&mm, 0, sizeof mm);
        mm.sType = 15;   /* MEMORY_MAP_INFO */
        mm.size = 16; mm.pp = &map;
        {
            int32_t (*mapfn)(void*, void*, unsigned long long, unsigned long long, uint32_t, void**) =
                (int32_t (*)(void*, void*, unsigned long long, unsigned long long, uint32_t, void**))dfn("vkMapMemory");
            if (mapfn && mapfn(g_chain_dev, mem, 0, 16, 0, &map) == 0 && map) {
                g_fill_map = map;
                *(volatile unsigned int*)map = 0;
                stepm("m9-fillmap-ok", 1);
            } else stepm("m9-fillmap-fail", 0);
        }
    }
    g_fill_state = 1;
    stepm("min2-fill-target-ok", (long)(intptr_t)g_fill_buf);
}


/* REV15: the full compute chassis on the chain device — proven struct
 * shapes and sTypes cribbed from wv0_compute.c (device-verified lineage). */
static void* dfn(const char* name)
{
    return g_down_gdpa ? ((void* (*)(void*, const char*))g_down_gdpa)(g_chain_dev, name) : NULL;
}

/* seam_nms.c bridge: the chain-resolved device fn table (dfn is static). */
void* msl_dfn(const char* name) { return dfn(name); }

/* seam_nms.c bridge for GAME-device calls (the ZCC2 import, §23.16): dfn()
 * resolves against g_chain_dev ONLY — a fn resolved from device A's
 * dispatch and called with device B's handle is a guaranteed SEGV on
 * Turnip (the zcc2-enter crash, pid 77131). Resolve against the ACTUAL
 * device. */
void* msl_dfn_dev(void* dev, const char* name)
{
    return g_down_gdpa ? ((void* (*)(void*, const char*))g_down_gdpa)(dev, name) : NULL;
}

/* ZCC2 follow-up: the layer-chain gdpa returned NULL for core device fns
 * against the game device (zcc2-fail rb=-9). vkCreateBuffer/AllocateMemory/
 * BindBufferMemory are ALSO instance-level fns whose trampolines accept ANY
 * device — resolve them through the device-link gipa instead. */
void* msl_ires_dev(const char* name)
{
    return (g_down_gipa && g_layer_inst) ? ((void* (*)(void*, const char*))g_down_gipa)(g_layer_inst, name) : NULL;
}


/* REV48d: THE CREATE-FREEZE ATTRIBUTION PROBE. Every prior game-device
 * create attempt from layer contexts hung or died WITHOUT a trace (3g:
 * acks froze at doorbell 1149, no g3 markers ever; 3l: doorbell 1). This
 * probe walks the object ladder ONE call at a time on a dedicated thread,
 * each step bracketed by flushed markers — a hang leaves the exact call
 * named in g3probe.log; a wrapper __assert2 abort names it in logcat.
 * The probe creates objects and STOPS: no dispatch, no uds with views, no
 * route work — the game's frame path is untouched (fill-only config). */
#include <pthread.h>
#include "route_write_spv.h"   /* include-guarded; provides route_write_spv[] for the probe */
#include "trivial_spv.h"       /* REV48e: minimal void-main compute shader (10 words-ish) for the poison test */
static void g3p_mark(const char* what, long v)
{
    FILE* f = fopen(SEAM_LOG_DIR "/g3probe.log", "a");
    if (f) { fprintf(f, "%s = %ld (tid %d)\n", what, v, (int)syscall(SYS_gettid)); fclose(f); }
}
static void* g3_probe_thread(void* arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "fsr4npu-g3", 0, 0, 0);   /* r61 */
    usleep(3000000);   /* let the game's boot settle; nothing records yet at +3s */
    g3p_mark("probe-start", 1);
    if (!g_down_gdpa || !g_chain_dev) { g3p_mark("probe-nochain", 0); return NULL; }
    {
        int32_t (*fcb)(void*, const void*, const void*, void**)  = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreateBuffer");
        int32_t (*fsm)(void*, const void*, const void*, void**)  = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreateShaderModule");
        int32_t (*fdsl)(void*, const void*, const void*, void**) = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreateDescriptorSetLayout");
        int32_t (*fpl)(void*, const void*, const void*, void**)  = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreatePipelineLayout");
        int32_t (*fcp)(void*, void*, uint32_t, const void*, const void*, void**) = (int32_t (*)(void*, void*, uint32_t, const void*, const void*, void**))dfn("vkCreateComputePipelines");
        int32_t (*fpool)(void*, const void*, const void*, void**) = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreateDescriptorPool");
        int32_t (*fads)(void*, const void*, void**)              = (int32_t (*)(void*, const void*, void**))dfn("vkAllocateDescriptorSets");
        g3p_mark("fns", (long)(fcb ? 1 : 0) + (fsm ? 2 : 0) + (fdsl ? 4 : 0) + (fpl ? 8 : 0) + (fcp ? 16 : 0) + (fpool ? 32 : 0) + (fads ? 64 : 0));
        if (!fcb || !fsm || !fdsl || !fpl || !fcp || !fpool || !fads) { g3p_mark("probe-fnhole", 0); return NULL; }
        {
            unsigned long long buf = 0, mod = 0, dsl = 0, play = 0, pipe = 0, pool = 0, set = 0;
            struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t flags; char p1[4];
                     unsigned long long size; uint32_t usage; char p2[4];
                     int sharingMode; char p3[4]; } bci;
            struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t flags; char p1[4];
                     unsigned long long codeSize; const uint32_t* pCode; } smci;   /* 40 B canonical */
            struct { uint32_t binding, type, count, stages; char p[4]; const void* immutable; } binds[1];
            struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t flags; uint32_t count; const void* p; } dslci;
            struct { uint32_t stages, offset, size; } pcr;
            unsigned long long la[1];
            struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t flags; uint32_t count; const void* const* layouts;
                     uint32_t pcrCount; char p1[4]; const void* ppcr; } plci;
            struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t flags; char p1[4];
                     struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t stage; char p1[4];
                              unsigned long long module; const char* name; const void* spec; } stage;
                     unsigned long long layout; unsigned long long base; int baseIdx; char p2[4]; } cpci;
            struct { uint32_t type, count; } psz[1];
            struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t flags; uint32_t maxSets; uint32_t n; const void* p; } poolci;
            struct { uint32_t sType; char p0[4]; const void* pNext; unsigned long long pool; uint32_t n; char p1[4]; const void* layouts; } dsai;
#include "route_write_spv.h"   /* include-guarded; provides route_write_spv[] */
            /* step 1: buffer */
            g3p_mark("pre-buffer", 1);
            memset(&bci, 0, sizeof bci);
            bci.sType = 12; bci.size = 65536; bci.usage = 0x22; /* STORAGE|TRANSFER_DST */
            g3p_mark("call-buffer", (long)(intptr_t)fcb);
            { int32_t r = fcb(g_chain_dev, &bci, NULL, (void**)&buf);
              g3p_mark("post-buffer", (long)((r << 32) | (buf & 0xffffffff))); if (r != 0 || !buf) { g3p_mark("FAIL-buffer", r); return NULL; } }
            /* step 2: shader module (use the tiny embedded route shader) */
            g3p_mark("pre-module", 1);
            memset(&smci, 0, sizeof smci);
            smci.sType = 16; smci.codeSize = 726ull * 4ull; smci.pCode = route_write_spv;
            g3p_mark("call-module", (long)(intptr_t)fsm);
            { int32_t r = fsm(g_chain_dev, &smci, NULL, (void**)&mod);
              g3p_mark("post-module", (long)((r << 32) | (mod & 0xffffffff))); if (r != 0 || !mod) { g3p_mark("FAIL-module", r); return NULL; } }
            /* step 3: DSL */
            g3p_mark("pre-dsl", 1);
            memset(binds, 0, sizeof binds);
            binds[0].binding = 5; binds[0].type = 7; binds[0].count = 1; binds[0].stages = 0x20;
            memset(&dslci, 0, sizeof dslci);
            dslci.sType = 32; dslci.count = 1; dslci.p = binds;
            g3p_mark("call-dsl", (long)(intptr_t)fdsl);
            { int32_t r = fdsl(g_chain_dev, &dslci, NULL, (void**)&dsl);
              g3p_mark("post-dsl", (long)((r << 32) | (dsl & 0xffffffff))); if (r != 0 || !dsl) { g3p_mark("FAIL-dsl", r); return NULL; } }
            /* step 4: pipeline layout */
            g3p_mark("pre-playout", 1);
            pcr.stages = 0x20; pcr.offset = 0; pcr.size = 16;
            la[0] = dsl;
            memset(&plci, 0, sizeof plci);
            plci.sType = 30; plci.count = 1; plci.layouts = (const void* const*)la; plci.pcrCount = 1; plci.ppcr = &pcr;
            g3p_mark("call-playout", (long)(intptr_t)fpl);
            { int32_t r = fpl(g_chain_dev, &plci, NULL, (void**)&play);
              g3p_mark("post-playout", (long)((r << 32) | (play & 0xffffffff))); if (r != 0 || !play) { g3p_mark("FAIL-playout", r); return NULL; } }
            /* step 5a: TRIVIAL pipeline first (REV48e poison test — REV48d
             * proved vkCreateComputePipelines on the game device HANGS with
             * route_write.spv while every other create succeeds; if the
             * trivial shader compiles, the SPV is the poison, not the path) */
            {
                unsigned long long tmod = 0, tpipe = 0;
                g3p_mark("pre-tmodule", 1);
                memset(&smci, 0, sizeof smci);
                smci.sType = 16; smci.codeSize = (unsigned long long)(TRIVIAL_SPV_WORDS * 4); smci.pCode = trivial_spv;
                { int32_t r = fsm(g_chain_dev, &smci, NULL, (void**)&tmod);
                  g3p_mark("post-tmodule", (long)((r << 32) | (tmod & 0xffffffff))); if (r != 0 || !tmod) { g3p_mark("FAIL-tmodule", r); } }
                if (tmod) {
                    g3p_mark("pre-tpipeline", 1);
                    memset(&cpci, 0, sizeof cpci);
                    cpci.sType = 29; cpci.layout = play; cpci.baseIdx = -1;
                    cpci.stage.sType = 18; cpci.stage.stage = 0x20; cpci.stage.module = tmod; cpci.stage.name = "main";
                    g3p_mark("call-tpipeline", (long)(intptr_t)fcp);
                    { int32_t r = fcp(g_chain_dev, 0, 1, &cpci, NULL, (void**)&tpipe);
                      g3p_mark("post-tpipeline", (long)((r << 32) | (tpipe & 0xffffffff))); if (r != 0 || !tpipe) { g3p_mark("FAIL-tpipeline", r); } }
                }
            }
            /* step 5b: the route_write pipeline (the historically fatal one) */
            g3p_mark("pre-pipeline", 1);
            memset(&cpci, 0, sizeof cpci);
            cpci.sType = 29; cpci.layout = play; cpci.baseIdx = -1;
            cpci.stage.sType = 18; cpci.stage.stage = 0x20; cpci.stage.module = mod; cpci.stage.name = "main";
            g3p_mark("call-pipeline", (long)(intptr_t)fcp);
            { int32_t r = fcp(g_chain_dev, 0, 1, &cpci, NULL, (void**)&pipe);
              g3p_mark("post-pipeline", (long)((r << 32) | (pipe & 0xffffffff))); if (r != 0 || !pipe) { g3p_mark("FAIL-pipeline", r); return NULL; } }
            /* step 6+7: pool + set */
            g3p_mark("pre-pool", 1);
            psz[0].type = 7; psz[0].count = 1;
            memset(&poolci, 0, sizeof poolci);
            poolci.sType = 33; poolci.maxSets = 1; poolci.n = 1; poolci.p = psz;
            g3p_mark("call-pool", (long)(intptr_t)fpool);
            { int32_t r = fpool(g_chain_dev, &poolci, NULL, (void**)&pool);
              g3p_mark("post-pool", (long)((r << 32) | (pool & 0xffffffff))); if (r != 0 || !pool) { g3p_mark("FAIL-pool", r); return NULL; } }
            g3p_mark("pre-set", 1);
            memset(&dsai, 0, sizeof dsai);
            dsai.sType = 34; dsai.pool = pool; dsai.n = 1; dsai.layouts = la;
            g3p_mark("call-set", (long)(intptr_t)fads);
            { int32_t r = fads(g_chain_dev, &dsai, (void**)&set);
              g3p_mark("post-set", (long)((r << 32) | (set & 0xffffffff))); if (r != 0 || !set) { g3p_mark("FAIL-set", r); return NULL; } }
            g3p_mark("PROBE-COMPLETE-ALL-CREATES-OK", (long)(pipe & 0xffffffff));
        }
    }
    return NULL;
}
/* REV36: the per-eval binding-8 update lives OFF the game record thread.
 * Run 3d hung the game thread inside the cross-device vkUpdateDescriptorSets
 * (probe-device set referencing the game's view - one doorbell, all stage
 * markers once, thread never returned). The t7 watcher thread performs the
 * host-side write; the record thread stashes and dispatches only after. */
static volatile unsigned long long g_r3_view, g_r3_rdev, g_r3_rset, g_r3_uds;
static volatile int g_r3_need, g_r3_done;

#include "route_write_spv.h"   /* REV37: the stage-3 shader, embedded layer-side */

/* REV37 (runs 3d/3e): the seat's probe-device stage-3 set + the game's view
 * = a CROSS-DEVICE descriptor dereference at vkUpdateDescriptorSets — hang
 * (3d, record thread) then a hard crash (3e, watcher thread; the game pid
 * died). The layer now owns stage 3 entirely ON THE CHAIN DEVICE (the
 * m_fill_ensure creation law): staging buffer + shader + pipeline + set are
 * all game-device; binding 8 receives the game's OWN view (same-device
 * deref — safe); the probe route bank is pulled in by a record-time
 * vkCmdCopyBuffer (handles-only at record — the proven cross-device class). */

static void m9_g3_ensure(void)
{
    if (g3_state) return;
    if (!g_down_gdpa || !g_chain_dev) { g3_state = -1; return; }
    {
        int32_t (*fcb)(void*, const void*, const void*, void**)  = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreateBuffer");
        void (*fbr)(void*, void*, void*)                          = (void (*)(void*, void*, void*))dfn("vkGetBufferMemoryRequirements");
        int32_t (*fam)(void*, const void*, const void*, void**)  = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkAllocateMemory");
        int32_t (*fbb)(void*, void*, void*, unsigned long long)  = (int32_t (*)(void*, void*, void*, unsigned long long))dfn("vkBindBufferMemory");
        int32_t (*fsm)(void*, const void*, const void*, void**)  = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreateShaderModule");
        int32_t (*fdsl)(void*, const void*, const void*, void**) = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreateDescriptorSetLayout");
        int32_t (*fpl)(void*, const void*, const void*, void**)  = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreatePipelineLayout");
        int32_t (*fcp)(void*, void*, uint32_t, const void*, const void*, void**) = (int32_t (*)(void*, void*, uint32_t, const void*, const void*, void**))dfn("vkCreateComputePipelines");
        int32_t (*fpool)(void*, const void*, const void*, void**) = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreateDescriptorPool");
        int32_t (*fads)(void*, const void*, void**)                = (int32_t (*)(void*, const void*, void**))dfn("vkAllocateDescriptorSets");
        void (*fuds)(void*, uint32_t, const void*, uint32_t, const void*) = (void (*)(void*, uint32_t, const void*, uint32_t, const void*))dfn("vkUpdateDescriptorSets");
        struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t flags; char p1[4];
                 unsigned long long size; uint32_t usage; char p2[4];
                 int sharingMode; char p3[4]; } bci;
        struct { uint32_t sType; char p0[4]; const void* pNext; unsigned long long size; uint32_t type; char p1[4]; } mai;
        struct { unsigned long long size, alignment; uint32_t bits; char p[4]; } req;
        struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t flags; char p1[4];
                 unsigned long long codeSize; const uint32_t* pCode; } smci;   /* 40 B canonical: codeSize@24 pCode@32 */
        struct { uint32_t binding, type, count, stages; char p[4]; const void* immutable; } binds[2];
        struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t flags; uint32_t count; const void* p; } dslci;
        struct { uint32_t stages, offset, size; } pcr;
        unsigned long long la[1];
        struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t flags; uint32_t count; const void* const* layouts;
                 uint32_t pcrCount; char p1[4]; const void* ppcr; } plci;
        struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t stage; char p1[4];
                 unsigned long long module; const char* name; const void* spec; } stci;
        struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t flags; char p1[4];
                 struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t stage; char p1[4];
                          unsigned long long module; const char* name; const void* spec; } stage;
                 unsigned long long layout; unsigned long long base; int baseIdx; char p2[4]; } cpci;
        struct { uint32_t type, count; } psz[2];
        struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t flags; uint32_t maxSets; uint32_t n; const void* p; } poolci;
        struct { uint32_t sType; char p0[4]; const void* pNext; unsigned long long pool; uint32_t n; char p1[4]; const void* layouts; } dsai;
        struct { unsigned long long buffer, offset, range; } bi5;
        struct { uint32_t sType; char p0[4]; const void* pNext; void* dstSet; uint32_t binding, elem, count, type;
                 const void* pImg; const void* pBuf; const void* pTexel; } wr;
        if (!fcb || !fbr || !fam || !fbb || !fsm || !fdsl || !fpl || !fcp || !fpool || !fads || !fuds) {
            g3_state = -1; stepm("m9-g3-fn", 0); return;
        }
        memset(&bci, 0, sizeof bci);
        bci.sType = 12; bci.size = 1920ull * 1080ull * 8ull; bci.usage = 0x22; /* STORAGE|TRANSFER_DST */
        if (fcb(g_chain_dev, &bci, NULL, (void**)&g3_buf) != 0 || !g3_buf) { g3_state = -1; stepm("m9-g3-buf", 0); return; }
        memset(&req, 0, sizeof req); fbr(g_chain_dev, (void*)g3_buf, &req);
        memset(&mai, 0, sizeof mai);
        mai.sType = 5; mai.size = req.size; mai.type = 0;
        if (fam(g_chain_dev, &mai, NULL, (void**)&g3_mem) != 0 || !g3_mem ||
            fbb(g_chain_dev, (void*)g3_buf, (void*)g3_mem, 0) != 0) { g3_state = -1; stepm("m9-g3-mem", 0); return; }
        memset(&smci, 0, sizeof smci);
        smci.sType = 16; smci.codeSize = (unsigned long long)(ROUTE_WRITE_SPV_WORDS * 4); smci.pCode = route_write_spv;
        if (fsm(g_chain_dev, &smci, NULL, (void**)&g3_mod) != 0 || !g3_mod) { g3_state = -1; stepm("m9-g3-mod", 0); return; }
        memset(binds, 0, sizeof binds);
        binds[0].binding = 5;  binds[0].type = 7; binds[0].count = 1; binds[0].stages = 0x20;
        binds[1].binding = 8;  binds[1].type = 3; binds[1].count = 1; binds[1].stages = 0x20;
        memset(&dslci, 0, sizeof dslci);
        dslci.sType = 32; dslci.count = 2; dslci.p = binds;
        if (fdsl(g_chain_dev, &dslci, NULL, (void**)&g3_dsl) != 0 || !g3_dsl) { g3_state = -1; stepm("m9-g3-dsl", 0); return; }
        pcr.stages = 0x20; pcr.offset = 0; pcr.size = 16;
        la[0] = g3_dsl;
        memset(&plci, 0, sizeof plci);
        plci.sType = 30; plci.count = 1; plci.layouts = (const void* const*)la; plci.pcrCount = 1; plci.ppcr = &pcr;
        if (fpl(g_chain_dev, &plci, NULL, (void**)&g3_playout) != 0 || !g3_playout) { g3_state = -1; stepm("m9-g3-pl", 0); return; }
        memset(&cpci, 0, sizeof cpci);
        cpci.sType = 29; cpci.layout = g3_playout; cpci.baseIdx = -1;
        cpci.stage.sType = 18; cpci.stage.stage = 0x20; cpci.stage.module = g3_mod; cpci.stage.name = "main";
        if (fcp(g_chain_dev, 0, 1, &cpci, NULL, (void**)&g3_pipe) != 0 || !g3_pipe) { g3_state = -1; stepm("m9-g3-pipe", 0); return; }
        stepm("m9-g3-pipe-ok", (long)(intptr_t)(void*)g3_pipe);
        psz[0].type = 7; psz[0].count = 1;
        psz[1].type = 3; psz[1].count = 1;
        memset(&poolci, 0, sizeof poolci);
        poolci.sType = 33; poolci.maxSets = 1; poolci.n = 2; poolci.p = psz;
        if (fpool(g_chain_dev, &poolci, NULL, (void**)&g3_pool) != 0 || !g3_pool) { g3_state = -1; stepm("m9-g3-pool", 0); return; }
        memset(&dsai, 0, sizeof dsai);
        dsai.sType = 34; dsai.pool = g3_pool; dsai.n = 1; dsai.layouts = la;
        if (fads(g_chain_dev, &dsai, (void**)&g3_set) != 0 || !g3_set) { g3_state = -1; stepm("m9-g3-set", 0); return; }
        memset(&bi5, 0, sizeof bi5);
        bi5.buffer = g3_buf; bi5.range = ~(unsigned long long)0;
        memset(&wr, 0, sizeof wr);
        wr.sType = 35; wr.dstSet = (void*)g3_set; wr.binding = 5; wr.count = 1; wr.type = 7; wr.pBuf = &bi5;
        fuds(g_chain_dev, 1, &wr, 0, NULL);
        g3_osw = 1920; g3_osh = 1080;   /* REV41: fixed grid; the probe bank
                                         * handle is read lazily at eval time */
        g3_state = 1;
        stepm("m9-g3-ok", (long)g3_osw);
    }
}


/* REV28: waits for the QNN probe, then runs the per-eval NPU worker with
 * the seat's handshake pointers (outq -> rpcmem in -> graphExecute ->
 * postA npu-in bank). The worker lives in qnn_glue.c (owns the QNN state). */

/* r59c: the real-input feed's per-eval exposure (read from the game's 1x1
 * R32F ExposureTexture) — the wv1 dispatch block patches push word 3. */
static volatile float g_feed_expo = 1.0f;
static volatile int g_feed_expo_ok = 0;

/* r59g (oracle round-3 Q2): the NPU-output ring is THREE slots with an
 * EXPLICIT publish — the worker stores g_npu_pub = (gen<<8)|slot after each
 * copy-out and never touches the two most recent slots, so the layer never
 * guesses parity from npu_done (the old (npu_done-1)&1 derivation diverged
 * whenever the worker skipped evals and could rebind the slot being written).
 * postA reads a FIXED snapshot bank (binding 4 re-bound once); each eval the
 * layer records GPU vkCmdCopyBuffer(ring[published] -> fixed) for BOTH the
 * NPU bytes and the rec fp16 pairs — no per-eval host descriptor writes
 * remain (updating a set earlier CBs still reference is UB, crash suspect c).
 * npu_done is still published for the seat. The rec pairs are produced by
 * the WORKER from cached rpcmem (layer-side pipeline creation is closed by
 * the REV43 law — boot 23 re-confirmed it kills the game). */
static unsigned long long g_ringBuf[3] = {0, 0, 0}, g_ringMem[3] = {0, 0, 0};
static unsigned long long g_recBuf[3] = {0, 0, 0}, g_recMem[3] = {0, 0, 0};
static unsigned long long g_snapBuf = 0, g_snapMem = 0, g_recFixBuf = 0, g_recFixMem = 0;
static int g_suppress_out = 0;              /* r59o: >0 while presenting the game's own frames through a pause resumption */
static void* g_ringMap[3] = {NULL, NULL, NULL};
static void* g_recMap[3] = {NULL, NULL, NULL};
static volatile unsigned long long g_npu_pub = 0;   /* (gen<<8)|slot, worker-written */
static int g_ring_ok = 0;
volatile int m9_zcc3_active = 0;   /* r60h stage 3: 1 once the first outq->importedIN copy is RECORDED (qnn_glue's worker gates its CPU input bridge off this + fsr4npu-ZCC3 — never both skipped) */
static volatile int g_feed_active = 0;
static volatile float g_jx_lag = 0.0f, g_jy_lag = 0.0f;

/* r59g (oracle round-3 Q4-5): pick the memory type from the buffer's OWN
 * req.bits instead of hard-coded type 0 — prefer HOST_VISIBLE|HOST_COHERENT|
 * HOST_CACHED, then VISIBLE|COHERENT (wv0/wv4 law: no non-coherent fallback).
 * The old type=0 happened to work but left the mapped views write-combined
 * on some drivers (the worker's 128 ns/texel uncached reads). */
static int m9_pick_mem_type(uint32_t bits)
{
    uint32_t t, vis = 0xFFFFFFFFu;
    if (!g_r19_mp.cnt) return 0;
    for (t = 0; t < g_r19_mp.cnt && t < 32; t++) {
        if (!((bits >> t) & 1)) continue;
        if ((g_r19_mp.types[t].props & 0xEu) == 0xEu) return (int)t;
        if (vis == 0xFFFFFFFFu && (g_r19_mp.types[t].props & 0x6u) == 0x6u) vis = t;
    }
    return vis == 0xFFFFFFFFu ? -1 : (int)vis;
}

/* allocate one CPU-mapped STORAGE buffer on the game device (the feed's
 * proven create pattern); returns 0 on failure */
static int m9_alloc_mapped_buf(void* dev, unsigned long long sz,
                               unsigned long long* buf, unsigned long long* mem, void** map)
{
    int32_t (*fcb)(void*, const void*, const void*, void**)  = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreateBuffer");
    void (*fbr)(void*, void*, void*)                          = (void (*)(void*, void*, void*))dfn("vkGetBufferMemoryRequirements");
    int32_t (*fam)(void*, const void*, const void*, void**)  = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkAllocateMemory");
    int32_t (*fbb)(void*, void*, void*, unsigned long long)  = (int32_t (*)(void*, void*, void*, unsigned long long))dfn("vkBindBufferMemory");
    int32_t (*fmp)(void*, void*, unsigned long long, unsigned long long, uint32_t, void**) = (int32_t (*)(void*, void*, unsigned long long, unsigned long long, uint32_t, void**))dfn("vkMapMemory");
    struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t flags; char p1[4];
             unsigned long long size; uint32_t usage; char p2[4];
             int sharingMode; char p3[4]; } bci;
    struct { uint32_t sType; char p0[4]; const void* pNext; unsigned long long size; uint32_t type; char p1[4]; } mai;
    struct { unsigned long long size, alignment; uint32_t bits; char p[4]; } req;
    int32_t r;
    if (!fcb || !fbr || !fam || !fbb || !fmp) return 0;
    memset(&bci, 0, sizeof bci);
    bci.sType = 9; bci.size = sz; bci.usage = 0x22;
    r = fcb(dev, &bci, NULL, (void**)buf);
    if (r != 0 || !*buf) return 0;
    fbr(dev, (void*)*buf, &req);
    memset(&mai, 0, sizeof mai);
    mai.sType = 5; mai.size = req.size;
    {   /* r59g: type from req.bits (was hard-coded 0) */
        int mt = m9_pick_mem_type(req.bits);
        if (mt < 0) return 0;
        mai.type = (uint32_t)mt;
        { static int logged; if (!logged) { logged = 1; stepm("m9-memtype", (long)((mai.type & 0xff) | ((g_r19_mp.types[mai.type].props & 0xf) << 8))); } }
    }
    r = fam(dev, &mai, NULL, (void**)mem);
    if (r != 0 || !*mem) return 0;
    if (fbb(dev, (void*)*buf, (void*)*mem, 0) != 0) return 0;
    if (map) {
        r = fmp(dev, (void*)*mem, 0, ~(unsigned long long)0, 0, map);
        if (r != 0 || !*map) return 0;
    }
    return 1;
}

/* r59c: half-float helpers for the MV pixel->normalized scaling. */
static float m9_f16_to_f32(unsigned short h)
{
    unsigned int sign = (unsigned int)(h >> 15) << 31;
    int exp = (int)((h >> 10) & 0x1f) - 15 + 127;
    unsigned int man = (unsigned int)(h & 0x3ff) << 13;
    if (((h >> 10) & 0x1f) == 0) return 0.0f;         /* zeros/subnormals -> 0 (velocities are tiny) */
    if (((h >> 10) & 0x1f) == 0x1f) return sign ? -1e30f : 1e30f;
    { unsigned int b = sign | ((unsigned int)exp << 23) | man; float f; memcpy(&f, &b, 4); return f; }
}
static unsigned short m9_f32_to_f16(float f)
{
    unsigned int x; memcpy(&x, &f, 4);
    { unsigned int sign = (x >> 16) & 0x8000u;
      int e = (int)((x >> 23) & 0xff) - 127 + 15;
      unsigned int m = x & 0x7fffffu;
      if (e <= 0) return (unsigned short)sign;
      if (e >= 31) return (unsigned short)(sign | 0x7c00u);
      return (unsigned short)(sign | ((unsigned int)e << 10) | (m >> 13)); }
}

static void* t7_chain_thread(void* arg)
{
    extern volatile int q6_probe_ok;
    prctl(PR_SET_NAME, "fsr4npu-t7work", 0, 0, 0);   /* r61: this thread becomes the NPU worker */

    extern void qnn_glue_worker2(volatile unsigned long long*, volatile unsigned long long*,
                                 const void*, void*, void*, unsigned long long, unsigned long long);
    SeamHeader__* h = *(SeamHeader__**)arg;
    int waited_ms = 0;
    while (!q6_probe_ok && waited_ms < 20000) { usleep(1000); waited_ms++; }
    if (!q6_probe_ok) { stepm("t7-no-probe", waited_ms); return NULL; }
    {
        const void* outq = NULL; void* npudst = NULL;
        volatile unsigned long long* eval_seq = NULL; volatile unsigned long long* npu_done = NULL;
        unsigned long long ob = 0, nb = 0;
        if (!seam_npu_handshake(h, &outq, &npudst, &eval_seq, &npu_done, &ob, &nb)) {
            stepm("t7-no-handshake", 0);
            return NULL;
        }
        stepm("t7-handshake-ok", (long)(ob >> 20));   /* outq MB */
        {   /* r59g: run the worker on a THREE-slot ring of layer-owned banks
             * (game-device STORAGE, CPU-mapped) with an explicit published
             * generation (g_npu_pub) — the worker never touches the two most
             * recent slots, and postA reads a fixed snapshot bank the layer
             * refreshes with a GPU copy from the published slot each eval. */
            if (!g_ring_ok) {
                g_ring_ok = m9_alloc_mapped_buf(g_chain_dev, 7372800ull, &g_ringBuf[0], &g_ringMem[0], &g_ringMap[0]) &&
                           m9_alloc_mapped_buf(g_chain_dev, 7372800ull, &g_ringBuf[1], &g_ringMem[1], &g_ringMap[1]) &&
                           m9_alloc_mapped_buf(g_chain_dev, 7372800ull, &g_ringBuf[2], &g_ringMem[2], &g_ringMap[2]) &&
                           m9_alloc_mapped_buf(g_chain_dev, 7372800ull, &g_recBuf[0], &g_recMem[0], &g_recMap[0]) &&
                           m9_alloc_mapped_buf(g_chain_dev, 7372800ull, &g_recBuf[1], &g_recMem[1], &g_recMap[1]) &&
                           m9_alloc_mapped_buf(g_chain_dev, 7372800ull, &g_recBuf[2], &g_recMem[2], &g_recMap[2]);
                stepm("t7-ring", g_ring_ok);
            }
            if (g_ring_ok) {
                extern void qnn_glue_worker3(volatile unsigned long long*, volatile unsigned long long*,
                                             volatile unsigned long long*, const void*,
                                             void*, void*, void*, void*, void*, void*,
                                             unsigned long long, unsigned long long);
                qnn_glue_worker3(eval_seq, npu_done, &g_npu_pub, outq,
                                 g_ringMap[0], g_ringMap[1], g_ringMap[2],
                                 g_recMap[0], g_recMap[1], g_recMap[2], ob, nb);
            } else
                qnn_glue_worker2(eval_seq, npu_done, outq, npudst, npudst, ob, nb);
        }
    }
    return NULL;
}

static void m_vkCmdUpdateBuffer(void* cb, void* dst, unsigned long long off,
                                uint32_t n, const void* data)
{
    /* rev16 PERFORMANCE: all cmd-level fns resolved ONCE and cached; the
     * per-doorbell cost is parse + copy + 4 recorded cmds + ack write. */
    static void (*bindp)(void*, uint32_t, void*);
    static void (*binds_)(void*, uint32_t, void*, uint32_t, uint32_t, const void*, uint32_t, const void*);
    static void (*pushc)(void*, void*, uint32_t, uint32_t, uint32_t, const void*);
    static void (*disp)(void*, uint32_t, uint32_t, uint32_t);
    static void (*fill)(void*, void*, unsigned long long, unsigned long long, uint32_t);
    static void (*down)(void*, void*, unsigned long long, uint32_t, const void*);
    static int fns_cached;
    static int g_nodispatch;   /* r58q: marker-gated dispatch kill for the POKE test */
    static long long t_total_ns; static long long t_max_ns; static long t_n;
    SeamHeader__* h = NULL; uint64_t seq = 0;
    int ours;
    struct timespec ts0, ts1;

    if (!fns_cached && g_down_gdpa && g_chain_dev) {
        bindp  = (void (*)(void*, uint32_t, void*))dfn("vkCmdBindPipeline");
        binds_ = (void (*)(void*, uint32_t, void*, uint32_t, uint32_t, const void*, uint32_t, const void*))dfn("vkCmdBindDescriptorSets");
        pushc  = (void (*)(void*, void*, uint32_t, uint32_t, uint32_t, const void*))dfn("vkCmdPushConstants");
        disp   = (void (*)(void*, uint32_t, uint32_t, uint32_t))dfn("vkCmdDispatch");
        fill   = (void (*)(void*, void*, unsigned long long, unsigned long long, uint32_t))dfn("vkCmdFillBuffer");
        down   = (void (*)(void*, void*, unsigned long long, uint32_t, const void*))dfn("vkCmdUpdateBuffer");
        fns_cached = 1;
        stepm("min2-cmdfns-cached", 1);
        /* r58q POKE test: fsr4npu-NODISPATCH present => record NO dispatches
         * (binds/pushes stay, harmless) so the ONLY writer into the route bank
         * is the seat's CPU poke — the bank->Output copy then carries a pure
         * CPU-written pattern to screen (transport ground truth). */
        { FILE* mf = fopen(SEAM_LOG_DIR "/fsr4npu-NODISPATCH", "r");
          g_nodispatch = mf ? (fclose(mf), 1) : 0; }
        stepm("m58q-nodispatch", g_nodispatch);
    }

    clock_gettime(CLOCK_MONOTONIC, &ts0);
    ours = seam_on_cmd_update_buffer(cb, dst, off, n, data, &h, &seq);
    if (ours && h) {
        g_m9_doorbells++;                                   /* r61: doorbell ARRIVED (vs evals PROCESSED below) */
        g_m9_doorbell_tid = (int)syscall(SYS_gettid);       /* r61: which game thread carries evals */
        /* NMS (XeSS slot, rev3a+): mailbox header version >= 6 carries the
         * XeSS geometry instead of the R59 plan/route tail. The NMS record
         * path lives in seam_nms.c; the R59 t7 worker/plans are skipped
         * (the NMS worker spawns from seam_nms_doorbell itself).
         * rev3c.6: v7 = NMS-3 (0x2D0 header + stage2 handoff) — accept the
         * family, seam_nms_doorbell re-checks magic/version exactly. */
        { int nms_hdr = (((volatile uint64_t*)h)[1] >= 6ull);
        /* M6b-2: the NPU chain — the q6 probe (QNN session init) runs for
         * BOTH header revs (its own thread); the R59 t7 worker only for the
         * R59 header (NMS spawns its own worker from seam_nms_doorbell). */
        {
            static SeamHeader__* t7_hdr;
            static int q6_armed = 0;
            if (!q6_armed) {
                extern void* q6_probe_thread_shim(void*);
                extern volatile int q6_probe_ok;
                q6_armed = 1;
                t7_hdr = h;
                stepm("q6-arm", 1);
                {
                    pthread_t th;
                    if (pthread_create(&th, NULL, q6_probe_thread_shim, NULL) == 0)
                        pthread_detach(th);
                    else
                        stepm("q6-spawn-fail", 0);
                }
                if (!nms_hdr) {
                    pthread_t th;
                    if (pthread_create(&th, NULL, t7_chain_thread, &t7_hdr) == 0)
                        pthread_detach(th);
                    else
                        stepm("t7-chain-spawn-fail", 0);
                }
            }
        }
        int work = 0;
        if (nms_hdr) {
            /* NMS-1: the XeSS-slot record path (inputs->staging+stamp, and
             * the previous NPU generation -> Output, or the blit fallback).
             * Returns the ack work bit; the R59 plan machinery is skipped. */
            extern int seam_nms_doorbell(void*, uint64_t, void*);
            work = seam_nms_doorbell(h, seq, cb);
        } else {
        /* REV22: the compute dispatch. The seat creates the chassis on the
         * GAME'S OWN DEVICE at init time (seam_comp_chassis — the sanctioned
         * client route; the layer-side own-instance chassis is RETIRED after
         * rev19/21 died at vkCreateComputePipelines) and publishes the
         * handles in the mailbox header. Here we bind them into the game's
         * own command buffer via the chain-resolved cmd fns — same-device,
         * zero UB. Gated on BINDTEST: run 1 proves seat-side creation with
         * the proven fill fallback carrying the work bit through 2000/2000;
         * run 2 flips the in-CB dispatch era. */
        static int bind_gate = -1;
        if (bind_gate < 0) {
            bind_gate = (access(SEAM_LOG_DIR "/fsr4npu-BINDTEST", F_OK) == 0) ? 1 : 0;
            stepm("r22-bind-gate", bind_gate);
        }
        if (bind_gate && bindp && binds_ && pushc && disp) {
            /* REV28: the T_fsr4 two-stage doorbell. Stage 0 (feat) records
             * EVERY eval and bumps eval_seq; stage 1 (postA) records only
             * once the NPU worker has published npu_done for a prior eval
             * (the temporal gate — postA consumes the NPU's output of an
             * earlier frame, which is how FSR history feedback works).
             * REV33 (M9): stage 2 = RCAS (final color, after postA).
             * REV34 (ROUTE-V2): stage 3 = route-write — imageStores the
             * game's Output VIEW as a STORAGE IMAGE (route_write.comp),
             * replacing the REV33 copy+blit OUTPUT ROUTE. */
            static uint64_t s_pipe[4], s_layout[4], s_set[4], s_dispatch[4];
            static uint32_t s_dgz[4], s_pb[4], s_push[4][11], s_patch[4];
            static int s_read[4], s_tried[4];
            static unsigned long long s_evals;      /* feat dispatches recorded */
            static unsigned long long s_posta;      /* postA dispatches recorded */
            int posta_this_eval = 0;
            int st;
            for (st = 0; st < 4; st++) {
                if (!s_read[st] && !s_tried[st]) {
                    s_tried[st] = 1;
                    s_pipe[st] = seam_comp_plan(st, h, &s_layout[st], &s_set[st], &s_dispatch[st],
                                                &s_dgz[st], &s_pb[st], s_push[st], &s_patch[st]);
                    if (s_pipe[st] && s_layout[st] && s_set[st] && s_pb[st] >= 4 && s_pb[st] <= 44 &&
                        (s_dispatch[st] & 0xffffffffull) && (s_dispatch[st] >> 32)) {
                        s_read[st] = 1;
                        stepm(st == 3 ? "m9-plan3-ok" : st == 2 ? "m9-plan2-ok" : st ? "t7-plan1-ok" : "t7-plan0-ok",
                              (long)(intptr_t)(void*)(uintptr_t)s_pipe[st]);
                    } else if (st == 1) {
                        stepm("t7-plan1-absent", 0);   /* feat-only boot */
                    }
                }
            }
            for (st = 0; st < 4; st++) {
                if (!s_read[st]) continue;
                if (st == 1) {   /* r59h: postA runs EVERY eval once the first
                                  * NPU generation exists — re-blending the latest
                                  * snapshot with the CURRENT color/MV is how FSR
                                  * history feedback handles an async NN (the
                                  * r59g once-per-gen gate let skipped evals
                                  * re-present the stale bank through the always-on
                                  * bank copy = the explosion "re-fire" the user
                                  * saw). Only the SNAPSHOT COPY waits for a
                                  * fresh generation. */
                    static unsigned long long s_posta_gen = 0;
                    unsigned long long pub = __atomic_load_n(&g_npu_pub, __ATOMIC_ACQUIRE);
                    unsigned long long gen = pub >> 8;
                    if (gen == 0) {
                        if ((s_evals & 0x3FF) == 1) stepm("t7-posta-skip", (long)s_evals);
                        continue;
                    }
                    {   /* the SNAPSHOT COPIES: ring[published slot] -> the FIXED
                         * NPU bank postA binding 4 reads, and rec[published slot]
                         * -> the FIXED rec bank wv1 binding 3 reads (both re-bound
                         * once). GPU-side (~0.3 ms Adreno total); kills the per-eval
                         * host descriptor writes (UB while earlier CBs reference
                         * the set). r59h: recorded ONLY on a fresh generation —
                         * between gens postA re-runs on the last snapshot. */
                        static void (*cpyS)(void*, void*, void*, uint32_t, const void*);
                        static void (*barS)(void*, unsigned int, unsigned int, unsigned int, unsigned int, const void*,
                                            unsigned int, const void*, unsigned int, const void*);
                        if (!cpyS) {
                            cpyS = (void (*)(void*, void*, void*, uint32_t, const void*))dfn("vkCmdCopyBuffer");
                            barS = (void (*)(void*, unsigned int, unsigned int, unsigned int, unsigned int, const void*,
                                             unsigned int, const void*, unsigned int, const void*))dfn("vkCmdPipelineBarrier");
                        }
                        if (gen == s_posta_gen) { /* no fresh NPU output: reuse the fixed banks as-is */ }
                        else if (cpyS && barS) {
                            /* r60e C-2 STAGE 1 (fsr4npu-ZCC2, default off): import
                             * the NPU's OUTPUT rpcmem onto the GAME device (pure
                             * pNext chains on core entry points — boot58 proved
                             * imports inert in place; boot57's death was the
                             * own-device submit, which this path NEVER issues)
                             * and snapshot-copy from the IMPORTED bank instead
                             * of the CPU-mapped ring. Stage 1 validates the
                             * imported memory as a game-CB copy source with
                             * identical content/timing; stage 2 deletes the
                             * worker memcpy + ring. */
                            static unsigned long long zcc2_buf, zcc2_mem;
                            static int zcc2_st = -1;
                            if (zcc2_st < 0) {
                                zcc2_st = 0;
                                if (access(SEAM_LOG_DIR "/fsr4npu-ZCC2", F_OK) == 0) {
                                    extern int q6_out_fd0(void);
                                    int fd = q6_out_fd0();
                                    int32_t (*fcbZ)(void*, const void*, const void*, void**) =
                                        (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreateBuffer");
                                    int32_t (*famZ)(void*, const void*, const void*, void**) =
                                        (int32_t (*)(void*, const void*, const void*, void**))dfn("vkAllocateMemory");
                                    int32_t (*fbbZ)(void*, void*, void*, uint64_t) =
                                        (int32_t (*)(void*, void*, void*, uint64_t))dfn("vkBindBufferMemory");
                                    if (fd > 0 && fcbZ && famZ && fbbZ) {
                                        struct { uint32_t sType; const void* pNext; uint32_t handleTypes; } xb =
                                            { 1000072000u, NULL, 0x200u };   /* DMA_BUF */
                                        struct { uint32_t sType; const void* pNext; uint32_t flags; unsigned long long size;
                                                 uint32_t usage, sharing; uint32_t qfic; const void* qfi; uint32_t init; } bci;
                                        struct { uint32_t sType; const void* pNext; uint32_t handleType; int fd; } imp =
                                            { 1000074000u, NULL, 0x200u, dup(fd) };
                                        struct { uint32_t sType; const void* pNext; unsigned long long size; uint32_t type; } mai;
                                        int32_t rb, rm, rbd;
                                        memset(&bci, 0, sizeof bci);
                                        bci.sType = 9; bci.pNext = &xb; bci.size = 7372800ull;
                                        bci.usage = 0xBu;   /* STORAGE|TRANSFER_DST|TRANSFER_SRC */
                                        rb = fcbZ(g_chain_dev, &bci, NULL, (void**)&zcc2_buf);
                                        stepm("m9-zcc2-buf-rc", rb);
                                        memset(&mai, 0, sizeof mai);
                                        mai.sType = 5; mai.pNext = &imp; mai.size = 7372800ull;
                                        mai.type = 0;   /* fdprops mtbits=7 (types 0,1,2 legal) */
                                        if (rb == 0 && zcc2_buf) {
                                            rm = famZ(g_chain_dev, &mai, NULL, (void**)&zcc2_mem);
                                            stepm("m9-zcc2-mem-rc", rm);
                                            if (rm == 0 && zcc2_mem) {
                                                rbd = fbbZ(g_chain_dev, (void*)zcc2_buf, (void*)zcc2_mem, 0);
                                                stepm("m9-zcc2-bind-rc", rbd);
                                                zcc2_st = (rbd == 0) ? 1 : 0;
                                            }
                                        }
                                        stepm("m9-zcc2-imported", zcc2_st);
                                    } else stepm("m9-zcc2-nofn", fd > 0 ? 0 : -1);
                                }
                            }
                            struct { uint32_t sType; char p0[4]; const void* pNext;
                                     uint32_t srcAccess, dstAccess; } mbS;
                            struct { unsigned long long so, dof, sz; } crS;
                            memset(&mbS, 0, sizeof mbS);
                            mbS.sType = 46; mbS.srcAccess = 0x20u;  mbS.dstAccess = 0x1000u;
                            barS(cb, 0x800u, 0x1000u, 0, 1, &mbS, 0, NULL, 0, NULL);   /* r59l FIX: COMPUTE -> TRANSFER (0x400 was COLOR_ATTACHMENT_OUTPUT — the stage bits were wrong all along; access masks were right) */
                            memset(&crS, 0, sizeof crS);
                            crS.sz = 7372800ull;
                            cpyS(cb, (void*)(uintptr_t)(zcc2_st == 1 ? zcc2_buf : g_ringBuf[pub & 0xff]), (void*)(uintptr_t)g_snapBuf, 1, &crS);
                            cpyS(cb, (void*)(uintptr_t)g_recBuf[pub & 0xff], (void*)(uintptr_t)g_recFixBuf, 1, &crS);
                            memset(&mbS, 0, sizeof mbS);
                            mbS.sType = 46; mbS.srcAccess = 0x1000u; mbS.dstAccess = 0x20u;
                            barS(cb, 0x1000u, 0x800u, 0, 1, &mbS, 0, NULL, 0, NULL);   /* r59l FIX: TRANSFER -> COMPUTE */
                            { static long nc; if ((++nc & 0x3FF) == 1) { stepm("m9-snap-copies", nc); if (zcc2_st == 1) stepm("m9-zcc2-copies", nc); } }
                            s_posta_gen = gen;
                        }
                    }
                }
                if (st == 2) {   /* REV33: RCAS runs only when postA recorded
                                  * this eval (it reads postA's hist write) */
                    if (!posta_this_eval) {
                        if ((s_evals & 0x3FF) == 1) stepm("m9-rcas-skip", (long)s_evals);
                        continue;
                    }
                }
                if (st == 3) {   /* REV34 (ROUTE-V2): the route-write runs only
                                  * when RCAS ran this eval (it reads RCAS's
                                  * fp16 route bank) */
                    if (!posta_this_eval) {
                        if ((s_evals & 0x3FF) == 1) stepm("m9-route-skip", (long)s_evals);
                        continue;
                    }
                }
                {
                    unsigned int push[11];
                    uint64_t sets[1];
                    uint32_t dgx = (uint32_t)(s_dispatch[st] & 0xffffffffull);
                    uint32_t dgy = (uint32_t)(s_dispatch[st] >> 32);
                    memcpy(push, s_push[st], sizeof push);
                        if (s_patch[st]) {
                            /* SeamFrame mirror: jitter_x@48 jitter_y@52 sharpness@64
                             * reset@84 (u32 -> float for the wv1 push word) */
                            struct { unsigned long long _[6]; float jx, jy, mvx, mvy, sharp;
                                     unsigned int _gap[4]; unsigned int reset; } fm;
                            const void* fp = seam_last_frame(NULL, NULL);
                            if (fp) {
                                memcpy(&fm, fp, sizeof fm);
                                /* r59e: jitter must be LAG-1 like the feeds (the color/MV
                                 * the shader sees are one frame old; pairing them with the
                                 * CURRENT jitter shimmers sub-pixel). */
                                float jxU = (g_feed_active && !g_nodispatch) ? g_jx_lag : fm.jx;
                                float jyU = (g_feed_active && !g_nodispatch) ? g_jy_lag : fm.jy;
                                if (s_patch[st] & 1u) memcpy(&push[0], &jxU, 4);
                                if (s_patch[st] & 2u) memcpy(&push[1], &jyU, 4);
                                if (s_patch[st] & 4u) memcpy(&push[9], &fm.sharp, 4);   /* wv2b PC: sharp@word9; word10 is out16 */
                                if (s_patch[st] & 8u) { float rf = fm.reset ? 1.0f : 0.0f; memcpy(&push[2], &rf, 4); }
                            }
                        }
                        if (g_feed_expo_ok) {
                            /* r59c: ONE exposure everywhere — wv1's u_expo is
                             * push word 3 (+12); wv2a/wv2b carry expo at word 4
                             * (+16). The seat published 1.0; the GLES original
                             * fed the same u_expo to all stages. */
                            if (st == 0) memcpy(&push[3], (const void*)&g_feed_expo, 4);
                            else        memcpy(&push[4], (const void*)&g_feed_expo, 4);
                        }
                    if (st == 3) {
                        /* REV41: chassis was built at game-device creation
                         * (m_vkCreateDevice). Per eval: lazy-read the probe
                         * route bank handle, write binding 8 with THIS
                         * frame's game view (same-device host update — no
                         * create lock), then record handle-only commands:
                         * memory barriers + buffer copy + bind/push/dispatch. */
                        static void (*barrier2)(void*, unsigned int, unsigned int, unsigned int, unsigned int, const void*,
                                                unsigned int, const void*, unsigned int, const void*);
                        static void (*cpyb)(void*, void*, void*, uint32_t, const void*);
                        static void (*udsi)(void*, uint32_t, const void*, uint32_t, const void*);
                        extern int seam_route_read(SeamHeader__*, uint64_t*, uint64_t*, uint32_t*, uint32_t*);
                        const unsigned char* fp = (const unsigned char*)seam_last_frame(NULL, NULL);
                        unsigned long long view;
                        unsigned int rw, rh;
                        if (g3_state != 1) continue;
                        if (!g3_route_src) {
                            uint64_t src = 0, img = 0; uint32_t w = 0, hh = 0;
                            if (seam_route_read(h, &src, &img, &w, &hh) && src && w) {
                                g3_route_src = src; g3_osw = w; g3_osh = hh;
                            }
                            if (!g3_route_src) continue;
                        }
                        if (!fp) continue;
                        view = *(const unsigned long long*)(fp + 296);
                        rw = *(const unsigned int*)(fp + 336);
                        rh = *(const unsigned int*)(fp + 340);
                        if (!view || !rw || !rh) continue;
                        if (!udsi) {
                            udsi = (void (*)(void*, uint32_t, const void*, uint32_t, const void*))dfn("vkUpdateDescriptorSets");
                            barrier2 = (void (*)(void*, unsigned int, unsigned int, unsigned int, unsigned int, const void*,
                                                unsigned int, const void*, unsigned int, const void*))dfn("vkCmdPipelineBarrier");
                            cpyb = (void (*)(void*, void*, void*, uint32_t, const void*))dfn("vkCmdCopyBuffer");
                            if (!udsi || !barrier2 || !cpyb) { stepm("m9-g3-recfn", 0); continue; }
                        }
                        {   /* binding 8 <- this eval's Output view (game device, host-side) */
                            struct { void* sampler; void* imageView; uint32_t imageLayout; char pad[4]; } ii3;
                            struct { uint32_t sType; char p0[4]; const void* pNext; void* dstSet;
                                     uint32_t dstBinding, dstArrayElement, descriptorCount, descriptorType;
                                     const void* pImageInfo; const void* pBufferInfo; const void* pTexelBufferView; } wr3;
                            memset(&ii3, 0, sizeof ii3);
                            ii3.imageView = (void*)(uintptr_t)view;
                            ii3.imageLayout = 1;        /* GENERAL */
                            memset(&wr3, 0, sizeof wr3);
                            wr3.sType = 35; wr3.dstSet = (void*)(uintptr_t)g3_set;
                            wr3.dstBinding = 8; wr3.descriptorCount = 1;
                            wr3.descriptorType = 3;     /* STORAGE_IMAGE */
                            wr3.pImageInfo = &ii3;
                            udsi(g_chain_dev, 1, &wr3, 0, NULL);
                        }
                        {
                            struct { uint32_t sType; char p[4]; const void* pNext;
                                     uint32_t srcAccess, dstAccess; } mb;
                            struct { unsigned long long srcOff, dstOff, size; } cpyr;
                            uint64_t sets2[1];
                            memset(&mb, 0, sizeof mb);
                            mb.sType = 46; mb.srcAccess = 0x40; mb.dstAccess = 0x800;
                            barrier2(cb, 0x800, 0x1000, 0, 1, &mb, 0, NULL, 0, NULL);
                            memset(&cpyr, 0, sizeof cpyr);
                            cpyr.size = (unsigned long long)g3_osw * g3_osh * 8ull;
                            cpyb(cb, (void*)(uintptr_t)g3_route_src, (void*)(uintptr_t)g3_buf, 1, &cpyr);
                            memset(&mb, 0, sizeof mb);
                            mb.sType = 46; mb.srcAccess = 0x1000; mb.dstAccess = 0x20;
                            barrier2(cb, 0x1000, 0x800, 0, 1, &mb, 0, NULL, 0, NULL);
                            push[0] = g3_osw; push[1] = g3_osh;
                            push[2] = rw;    push[3] = rh;
                            sets2[0] = g3_set;
                            bindp(cb, 1, (void*)(uintptr_t)g3_pipe);
                            binds_(cb, 1, (void*)(uintptr_t)g3_playout, 0, 1, (const void*)sets2, 0, NULL);
                            pushc(cb, (void*)(uintptr_t)g3_playout, 0x20, 0, 16, push);
                            if (!g_nodispatch) disp(cb, (rw + 7u) / 8u, (rh + 7u) / 8u, 1);
                        }
                        { static int wl; if (!wl) { wl = 1; stepm("m9-uds-watch", 1); } }
                        work = 1;
                        { static long n3; if ((++n3 & 0x3FF) == 1) stepm("m9-route-dispatches", n3); }
                        continue;
                    }
                    sets[0] = s_set[st];
                    bindp(cb, 1, (void*)(uintptr_t)s_pipe[st]);
                    {   /* r59d fix-4a (external analyst): an execution+memory
                         * barrier BEFORE each stage — the chain previously
                         * recorded wv1 -> postA -> RCAS back-to-back with NO
                         * barriers; later stages could read stale caches. */
                        static void (*barF)(void*, unsigned int, unsigned int, unsigned int, unsigned int, const void*,
                                            unsigned int, const void*, unsigned int, const void*);
                        if (!barF) barF = (void (*)(void*, unsigned int, unsigned int, unsigned int, unsigned int, const void*,
                                                    unsigned int, const void*, unsigned int, const void*))dfn("vkCmdPipelineBarrier");
                        if (barF) {
                            struct { uint32_t sType; char p0[4]; const void* pNext;
                                     uint32_t srcAccess, dstAccess; } mbF;
                            memset(&mbF, 0, sizeof mbF);
                            mbF.sType = 46;
                            mbF.srcAccess = 0x40u | 0x1000u;   /* SHADER_WRITE | TRANSFER_WRITE */
                            mbF.dstAccess = 0x20u | 0x800u;    /* SHADER_READ  | TRANSFER_READ  */
                            barF(cb, 0x800u, 0x800u, 0, 1, &mbF, 0, NULL, 0, NULL);  /* r59l FIX: COMPUTE -> COMPUTE (0x400 was COLOR_ATTACHMENT_OUTPUT) */
                        }
                    }
                    binds_(cb, 1, (void*)(uintptr_t)s_layout[st], 0, 1, (const void*)sets, 0, NULL);
                    pushc(cb, (void*)(uintptr_t)s_layout[st], 0x20, 0, s_pb[st], push);
                    if (!g_nodispatch) disp(cb, dgx, dgy, s_dgz[st] ? s_dgz[st] : 1);
                    work = 1;
                    if (st == 0) {
                        s_evals++;
                        seam_npu_bump_eval(h);
                        seam_npu_eval_wake(h);   /* r59g: futex wake — the worker sleeps instead of usleep(200) polling (5000 wakeups/s the FEX-translated game could not afford) */
                        { static long n; if ((++n & 0x3FF) == 1) stepm("t7-feat-dispatches", n); }
                        /* r60h STAGE 3 (fsr4npu-ZCC3, default off): the input
                         * bridge goes GPU-side. outq — feat's STORAGE output,
                         * dispatched two lines up — is copied into the
                         * IMPORTED input rpcmem by a recorded vkCmdCopyBuffer
                         * on the game CB; once the FIRST copy is recorded
                         * (m9_zcc3_active=1) the worker deletes its 3.69MB
                         * memcpy + sync bracket (t7-copies' 5.8ms — the last
                         * bridge). Import mirrors ZCC2 stage 1 exactly (pure
                         * pNext chains on core entry points, game device);
                         * the source handle is the seat's own outq VkBuffer,
                         * resolved by pointer match against the header's
                         * outq_ptr (seam_outq_buf_by_ptr). Semantics match
                         * the CPU bridge: both snapshot outq immediately
                         * after this feat dispatch; the worker's consumed+2
                         * pacing (~90ms) dwarfs the ~0.3ms copy. Soft-retries
                         * until the probe exposes the fd and outq resolves. */
                        {
                            static unsigned long long zcc3_buf, zcc3_mem, zcc3_src;
                            static int zcc3_st = -1;
                            static long zcc3_try;
                            if (zcc3_st < 0) {
                                if (++zcc3_try > 512) { zcc3_st = 0; stepm("m9-zcc3-giveup", zcc3_try); }
                                else if (access(SEAM_LOG_DIR "/fsr4npu-ZCC3", F_OK) == 0) {
                                    extern int q6_in_fd0(void);
                                    extern unsigned long long seam_outq_buf_by_ptr(unsigned long long);
                                    int fd = q6_in_fd0();
                                    const void* outqP = NULL;
                                    unsigned long long src = 0;
                                    if (seam_npu_handshake(h, &outqP, NULL, NULL, NULL, NULL, NULL) && outqP)
                                        src = seam_outq_buf_by_ptr((unsigned long long)(uintptr_t)outqP);
                                    zcc3_src = src;
                                    if (fd > 0 && src) {
                                        int32_t (*fcbZ)(void*, const void*, const void*, void**) =
                                            (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreateBuffer");
                                        int32_t (*famZ)(void*, const void*, const void*, void**) =
                                            (int32_t (*)(void*, const void*, const void*, void**))dfn("vkAllocateMemory");
                                        int32_t (*fbbZ)(void*, void*, void*, unsigned long long) =
                                            (int32_t (*)(void*, void*, void*, unsigned long long))dfn("vkBindBufferMemory");
                                        if (fcbZ && famZ && fbbZ) {
                                            struct { uint32_t sType; const void* pNext; uint32_t handleTypes; } xb =
                                                { 1000072000u, NULL, 0x200u };   /* DMA_BUF */
                                            struct { uint32_t sType; const void* pNext; uint32_t flags; unsigned long long size;
                                                     uint32_t usage, sharing; uint32_t qfic; const void* qfi; uint32_t init; } bci;
                                            struct { uint32_t sType; const void* pNext; uint32_t handleType; int fd; } imp =
                                                { 1000074000u, NULL, 0x200u, dup(fd) };
                                            struct { uint32_t sType; const void* pNext; unsigned long long size; uint32_t type; } mai;
                                            int32_t rb = 1, rm = 1, rbd = 1;
                                            memset(&bci, 0, sizeof bci);
                                            bci.sType = 9; bci.pNext = &xb; bci.size = 3686400ull;
                                            bci.usage = 0xBu;   /* mirror ZCC2 (proven accepted by Turnip for copy src+dst) */
                                            rb = fcbZ(g_chain_dev, &bci, NULL, (void**)&zcc3_buf);
                                            memset(&mai, 0, sizeof mai);
                                            mai.sType = 5; mai.pNext = &imp; mai.size = 3686400ull;
                                            mai.type = 0;   /* fdprops mtbits=7 (types 0,1,2 legal) */
                                            if (rb == 0 && zcc3_buf) {
                                                rm = famZ(g_chain_dev, &mai, NULL, (void**)&zcc3_mem);
                                                if (rm == 0 && zcc3_mem)
                                                    rbd = fbbZ(g_chain_dev, (void*)zcc3_buf, (void*)zcc3_mem, 0);
                                            }
                                            zcc3_st = (rb == 0 && rm == 0 && rbd == 0) ? 1 : 0;
                                            stepm("m9-zcc3-import", zcc3_st);
                                            stepm("m9-zcc3-rcs", (long)((rb << 8) | (rm << 4) | rbd));
                                            stepm("m9-zcc3-src", (long)(src & 0xffffffffull));
                                        }
                                    } else if ((zcc3_try & 0x3F) == 1) stepm("m9-zcc3-wait", fd > 0 ? 0 : -1);
                                } else zcc3_st = 0;   /* marker absent: path permanently off */
                            }
                            if (zcc3_st == 1) {
                                static void (*cpyZ)(void*, void*, void*, uint32_t, const void*);
                                static void (*barZ)(void*, unsigned int, unsigned int, unsigned int, unsigned int, const void*,
                                                    unsigned int, const void*, unsigned int, const void*);
                                if (!cpyZ) {
                                    cpyZ = (void (*)(void*, void*, void*, uint32_t, const void*))dfn("vkCmdCopyBuffer");
                                    barZ = (void (*)(void*, unsigned int, unsigned int, unsigned int, unsigned int, const void*,
                                                     unsigned int, const void*, unsigned int, const void*))dfn("vkCmdPipelineBarrier");
                                }
                                if (zcc3_src && cpyZ && barZ) {
                                    /* order feat's write -> the copy's read; the
                                     * closing barrier orders the copy against the
                                     * NEXT eval's feat write (WAR) — same shape as
                                     * the proven snapshot-copy brackets */
                                    struct { uint32_t sType; char p0[4]; const void* pNext;
                                             uint32_t srcAccess, dstAccess; } mbZ;
                                    struct { unsigned long long so, dof, sz; } crZ;
                                    memset(&mbZ, 0, sizeof mbZ);
                                    mbZ.sType = 46; mbZ.srcAccess = 0x40u;  mbZ.dstAccess = 0x800u | 0x1000u;
                                    barZ(cb, 0x800u, 0x1000u, 0, 1, &mbZ, 0, NULL, 0, NULL);   /* COMPUTE -> TRANSFER */
                                    memset(&crZ, 0, sizeof crZ);
                                    crZ.sz = 3686400ull;
                                    cpyZ(cb, (void*)(uintptr_t)zcc3_src, (void*)(uintptr_t)zcc3_buf, 1, &crZ);
                                    memset(&mbZ, 0, sizeof mbZ);
                                    mbZ.sType = 46; mbZ.srcAccess = 0x800u | 0x1000u;  mbZ.dstAccess = 0x20u | 0x40u;
                                    barZ(cb, 0x1000u, 0x800u, 0, 1, &mbZ, 0, NULL, 0, NULL);   /* TRANSFER -> COMPUTE */
                                    { static long nc; if ((++nc & 0x3FF) == 1) stepm("m9-zcc3-copies", nc); }
                                    if (!m9_zcc3_active) { m9_zcc3_active = 1; stepm("m9-zcc3-armed", 1); }
                                }
                            }
                        }
                    } else if (st == 1) {
                        s_posta++;
                        posta_this_eval = 1;
                        { static long n; if ((++n & 0x3FF) == 1) stepm("t7-posta-dispatches", n); }
                    } else {
                        { static long n; if ((++n & 0x3FF) == 1) stepm("m9-rcas-dispatches", n); }
                    }
                }
            }
        }
        if (!work) {
            m_fill_ensure();
            if (fill && g_fill_buf) { fill(cb, g_fill_buf, 0, 16, 0x46495234u); work = 1; }
        }
        /* REV49: the submission test readout — the fill's magic, CPU-side */
        if (g_fill_map && (t_n & 63) == 0) {
            unsigned int magic = *(volatile unsigned int*)g_fill_map;
            stepm("m9-fillmagic", (long)magic);
        }
        /* REV52: THE SIGHTED READBACK — vkCmdCopyImageToBuffer on the GAME'S
         * OWN CB: src = this eval's decoded Output image, dst = the probe rb
         * buffer published in the header at +0x208 (the doorbell-proven
         * game-CB + probe-dst class). The first instrument that reads the
         * Output through the game's own executor.
         * r60 SR52: default OFF — under the deployed seat (patch_r58s_640_
         * v4_noverify: seam_pq_verify entry = single RET) NOTHING reads the
         * rb52 buffer, so the every-64th-eval 7.37 MB copy is an orphan.
         * fsr4npu-SR52 re-arms it (absent = OFF = the cleanup's behavior);
         * the code stays as the future integrity-check hook. */
        {
            static int sr52on = -1;
            if (sr52on < 0) { sr52on = access(SEAM_LOG_DIR "/fsr4npu-NOSR52", F_OK) != 0; stepm("m9-sr52", sr52on); }   /* r60x: default ON — the readback PACES the GPU (boots 50/52 A/B: 17.9 vs 25.3/s); NOSR52 disables */
            if (sr52on) {
            static void (*cpy_i2b)(void*, unsigned long long, unsigned int, unsigned long long, unsigned int, const void*);
            const unsigned char* fp52 = (const unsigned char*)seam_last_frame(NULL, NULL);
            if (fp52 && h) {
                unsigned long long game_out52 = *(const unsigned long long*)(fp52 + 304);
                unsigned long long rb52 = *(const volatile unsigned long long*)((const char*)h + 0x208);
                unsigned int gw52 = *(const unsigned int*)(fp52 + 336);
                unsigned int gh52 = *(const unsigned int*)(fp52 + 340);
                if (game_out52 && rb52 && gw52 && gh52 && ((seq & 63) == 0)) {
                    if (!cpy_i2b)
                        cpy_i2b = (void (*)(void*, unsigned long long, unsigned int, unsigned long long, unsigned int, const void*))dfn("vkCmdCopyImageToBuffer");
                    if (cpy_i2b) {
                        struct { unsigned long long off; unsigned int rowLen, imgH;
                                 struct { unsigned int aspect, mip, layer, count; } sub;
                                 struct { int x, y, z; } o;
                                 struct { unsigned int w, h, d; } e; } cr52;
                        memset(&cr52, 0, sizeof cr52);
                        cr52.sub.aspect = 1; cr52.sub.count = 1;
                        cr52.e.w = gw52; cr52.e.h = gh52; cr52.e.d = 1;
                        cpy_i2b(cb, game_out52, 1 /* GENERAL */, rb52, 1, &cr52);
                        { static int sr52; if (!sr52) { sr52 = 1; stepm("m9-sr-recorded", 1); } }
                    }
                }
            }
            }   /* r60: end sr52 (REV52 integrity-hook gate) */
        }
        /* REV53: THE FINAL COPY — vkCmdCopyImage on the GAME'S OWN CB from
         * the probe scale image (img2 @ header+0x200, 1280x720 RGBA16F, held
         * white by the probe-queue route) into the game's Output. Same class
         * as the doorbell (game CB + probe src). Bare copy, NO barriers —
         * REV44's freeze came with barrier+blit. */
        {
            static void (*cpy_img)(void*, unsigned long long, unsigned int, unsigned long long, unsigned int, unsigned int, const void*);
            const unsigned char* fp53 = (const unsigned char*)seam_last_frame(NULL, NULL);
            if (fp53 && h) {
                unsigned long long game_out53 = *(const unsigned long long*)(fp53 + 304);
                unsigned long long img2_53  = *(const volatile unsigned long long*)((const char*)h + 0x200);
                unsigned int gw53 = *(const unsigned int*)(fp53 + 336);
                unsigned int gh53 = *(const unsigned int*)(fp53 + 340);
                if (game_out53 && img2_53 && ((gw53 == 1280 && gh53 == 720) || (gw53 == 1920 && gh53 == 1080))) {
                    if (!cpy_img)
                        cpy_img = (void (*)(void*, unsigned long long, unsigned int, unsigned long long, unsigned int, unsigned int, const void*))dfn("vkCmdCopyImage");
                    if (cpy_img) {
                        struct { struct { unsigned int aspect, mip, layer, count; } ss;
                                 struct { int x, y, z; } so;
                                 struct { unsigned int aspect, mip, layer, count; } ds;
                                 struct { int x, y, z; } doff;
                                 struct { unsigned int w, h, d; } e; } cr53;
                        memset(&cr53, 0, sizeof cr53);
                        cr53.ss.aspect = 1; cr53.ss.count = 1;
                        cr53.ds.aspect = 1; cr53.ds.count = 1;
                        cr53.e.w = 1280; cr53.e.h = 720; cr53.e.d = 1;
                        {   /* r60 B4: default OFF. img2 is still the WHITE probe image
                             * (boot48: 158/158 img2_hash=255B36D04F141B83) and the REV54
                             * bankcopy below rewrites game_out53 later in the SAME CB; a
                             * per-eval write into the game-owned Output is the boot 28-32
                             * hang class. fsr4npu-FINALCOPY re-arms the diagnostic. */
                            static int nofc = -1;
                            if (nofc < 0) { FILE* mf = fopen(SEAM_LOG_DIR "/fsr4npu-FINALCOPY", "r"); nofc = mf ? (fclose(mf), 0) : 1; stepm("m58s-nofinalcopy", nofc); }
                            if (!nofc) {
                                cpy_img(cb, img2_53, 1 /*GENERAL*/, game_out53, 1 /*GENERAL*/, 1, &cr53);
                                { static int c53; if (!c53) { c53 = 1; stepm("m9-finalcopy-recorded", 1); } }
                            }
                        }
                        /* REV54: ALSO copy DIRECTLY from the bank buffer (CPU-
                         * mapped, PQ-fill-proven-white) into the game Output —
                         * buffer->image, the exact reverse of the working
                         * readback. Kills the img2 middleman (a probe-queue
                         * blit product of unknown content). */
                        {
                            static void (*cpy_b2i54)(void*, unsigned long long, unsigned long long, unsigned int, unsigned int, const void*);
                            unsigned long long bank54 = 0;
                                {   /* r59p: back to the SEAT'S route bank — the pixel-proven source
                                 * (boots 25-30 showed content). An earlier r59j source — a separate
                                 * RCAS output buffer installed via a descriptor rebind — produced
                                 * BLACK screens (boots 32/38): the rebind never actually received
                                 * the shader's output; only the seat's own bank does. The source
                                 * device made no difference to the hang anyway (boot 32 hung
                                 * identically to 29/30). */
                                extern int seam_route_read(SeamHeader__*, uint64_t*, uint64_t*, uint32_t*, uint32_t*);
                                uint64_t bs = 0, bi = 0; uint32_t bw = 0, bh = 0;
                                if (seam_route_read(h, &bs, &bi, &bw, &bh) && bs) bank54 = bs;
                            }
                            if (!cpy_b2i54)
                                cpy_b2i54 = (void (*)(void*, unsigned long long, unsigned long long, unsigned int, unsigned int, const void*))dfn("vkCmdCopyBufferToImage");
                            {   /* r59o PAUSE SUPPRESSION: every pause-adjacent hang (flavor A
                                 * + boot 37 at pause-3) dies at a scene-load resumption. When the
                                 * gap since the previous eval exceeds 1.5 s we are resuming from a
                                 * bench pause — suppress the Output write for the next 8 evals and
                                 * let the game present its own frames through the load. */
                                static struct timespec s_lastev;
                                static int s_have_lastev = 0;
                                static int s_suppress = 0;
                                struct timespec s_nowev;
                                clock_gettime(CLOCK_MONOTONIC, &s_nowev);
                                if (s_have_lastev) {
                                    double gap = (double)(s_nowev.tv_sec - s_lastev.tv_sec)
                                               + (double)(s_nowev.tv_nsec - s_lastev.tv_nsec) / 1e9;
                                    if (gap > 1.5) { s_suppress = 8; stepm("m9-pause-suppress", (long)(gap * 1000.0)); }
                                }
                                s_lastev = s_nowev; s_have_lastev = 1;
                                g_m9_lastev_sec = (long)time(NULL);   /* r59u: feeds the wedge monitor */
                                g_m9_wedge_evals = (long long)seq;     /* r60x: the counter dumped at wedge */
                                {   static int s_mon;   /* r59u: one detached monitor, no Vulkan, /proc-only */
                                    if (!s_mon) { s_mon = 1; pthread_t mt; if (pthread_create(&mt, 0, m9_wedge_monitor, 0) == 0) pthread_detach(mt); }
                                }
                                {   /* r60x: BOOT IDENTITY — one attributable line per boot: build, driver,
                                     * env, marker set, memory state. First eval only. */
                                    static int s_id;
                                    if (!s_id) {
                                        s_id = 1;
                                        /* ARMADA: fixed identity (zig cc -Werror=date-time; build
                                         * reproducibility) — port tag replaces __DATE__/__TIME__ */
                                        stept("m9-identity-build", "feed9x-armada-deploy");
                                        { const char* td = getenv("TU_DEBUG");
                                          char lb[128];
                                          snprintf(lb, sizeof lb, "TU_DEBUG=%s driver=see-m9-probe", td ? td : "(unset)");
                                          stept("m9-identity-env", lb); }
                                        { const char* mks[] = { "BINDTEST", "FEED", "NOFINALCOPY", "NOPROBESUBMIT", "NORPCLEAR",
                                                               "NOPQVERIFY", "NOSR52", "CENSUS", "NOICC", "ZCHARN", "MARKER",
                                                               "AFFINITY", "SR52" };
                                          char set[192]; unsigned int k; size_t o = 0;
                                          set[0] = 0;
                                          for (k = 0; k < sizeof mks / sizeof mks[0]; k++) {
                                              char p[160];
                                              snprintf(p, sizeof p, SEAM_LOG_DIR "/fsr4npu-%s", mks[k]);
                                              if (access(p, F_OK) == 0 && o + strlen(mks[k]) + 2 < sizeof set) {
                                                  if (o) set[o++] = ',';
                                                  memcpy(set + o, mks[k], strlen(mks[k])); o += strlen(mks[k]); set[o] = 0;
                                              }
                                          }
                                          stept("m9-identity-markers", set[0] ? set : "(none)"); }
                                        { char lb[256]; long swf = -1, mf = -1; FILE* g = fopen("/proc/meminfo", "r");
                                          if (g) { while (fgets(lb, sizeof lb, g)) {
                                              if (!strncmp(lb, "SwapFree:", 9)) swf = atol(lb + 9);
                                              else if (!strncmp(lb, "MemFree:", 8)) mf = atol(lb + 8); } fclose(g); }
                                          snprintf(lb, sizeof lb, "memfree=%ldkB swapfree=%ldkB", mf, swf);
                                          stept("m9-identity-mem", lb); }
                                    }
                                }
                                g_suppress_out = s_suppress;
                                if (s_suppress > 0) s_suppress--;
                            }
                            if (cpy_b2i54 && bank54 && !g_suppress_out) {   /* r59o: suppressed through pause resumptions */
                                {   /* r59i DIAGNOSTIC: fsr4npu-NOROUTECOPY gates the
                                     * per-eval bank->Output write (our only GPU write into
                                     * a game-owned image). Hang discriminator for boots
                                     * 28-30: evals stop, all threads sleep, black screen. */
                                    static int norc = -1;
                                    if (norc < 0) { FILE* mf = fopen(SEAM_LOG_DIR "/fsr4npu-NOROUTECOPY", "r"); norc = mf ? (fclose(mf), 1) : 0; stepm("m59i-noroutecopy", norc); }
                                    if (norc) goto skip_bankcopy54;
                                }
                                struct { unsigned long long off; unsigned int rowLen, imgH;
                                         struct { unsigned int aspect, mip, layer, count; } sub;
                                         struct { int x, y, z; } o;
                                         struct { unsigned int w, h, d; } e; } cr54;
                                memset(&cr54, 0, sizeof cr54);
                                cr54.sub.aspect = 1; cr54.sub.count = 1;
                                /* r58t: extent DYNAMIC from the SeamFrame — at a
                                 * 1920x1080 Output this reads the FULL 1920-wide
                                 * bank rows => geometrically exact (no shear). */
                                cr54.e.w = gw53; cr54.e.h = gh53; cr54.e.d = 1;
                                {   /* r59k: TRANSITION GUARD — when the game's Output image
                                     * handle or dims change between evals (the bench recreates
                                     * the upscaler at scene transitions ~eval 1100/3500), skip
                                     * this eval's copy: a b2i write into an image the game is
                                     * tearing down wedged the queue host-side (boots 28/29/30/32
                                     * hung AT the transition; boot 31 with all copies off ran
                                     * 17,333 clean). One or two native frames at a transition
                                     * beat a dead bench. */
                                    static unsigned long long last_out = 0;
                                    static unsigned int last_w = 0, last_h = 0;
                                    if (game_out53 != last_out || gw53 != last_w || gh53 != last_h) {
                                        int was_set = (last_out != 0);
                                        last_out = game_out53; last_w = gw53; last_h = gh53;
                                        if (was_set) { stepm("m9-outch", (long)((gw53 << 16) | gh53)); goto skip_bankcopy54; }
                                    }
                                }
                                { static int ext_log; if (!ext_log && gw53 != 1280) { ext_log = 1; stepm("m58t-extent", (long)((gw53 << 16) | gh53)); } }
                                {   /* r59d fix-4a: RCAS's SHADER_WRITE must be
                                     * visible to the bank copy's TRANSFER_READ. */
                                    static void (*bar54)(void*, unsigned int, unsigned int, unsigned int, unsigned int, const void*,
                                                         unsigned int, const void*, unsigned int, const void*);
                                    if (!bar54) bar54 = (void (*)(void*, unsigned int, unsigned int, unsigned int, unsigned int, const void*,
                                                                  unsigned int, const void*, unsigned int, const void*))dfn("vkCmdPipelineBarrier");
                                    if (bar54) {
                                        struct { uint32_t sType; char p0[4]; const void* pNext;
                                                 uint32_t srcAccess, dstAccess; } mb54;
                                        memset(&mb54, 0, sizeof mb54);
                                        mb54.sType = 46;
                                        /* r59l FIX (oracle round-4): the old stage pair 0x400/0x800 was
                                         * COLOR_ATTACHMENT_OUTPUT/COMPUTE_SHADER — never ordered the
                                         * transfer. Now: everything earlier -> our transfer, and a NEW
                                         * post-copy barrier makes the write visible to the game's next
                                         * use of its Output image (the missing visibility edge the
                                         * hang analysis flagged). */
                                        mb54.srcAccess = 0x10000u;            /* MEMORY_WRITE */
                                        mb54.dstAccess = 0x800u | 0x1000u;    /* TRANSFER_READ | TRANSFER_WRITE */
                                        bar54(cb, 0x10000u, 0x1000u, 0, 1, &mb54, 0, NULL, 0, NULL);  /* ALL_COMMANDS -> TRANSFER */
                                    }
                                }
                                cpy_b2i54(cb, bank54, game_out53, 1 /*GENERAL*/, 1, &cr54);
                                {   /* r59l FIX: our TRANSFER_WRITE -> the game's next use */
                                    static void (*bar54b)(void*, unsigned int, unsigned int, unsigned int, unsigned int, const void*,
                                                          unsigned int, const void*, unsigned int, const void*);
                                    if (!bar54b) bar54b = (void (*)(void*, unsigned int, unsigned int, unsigned int, unsigned int, const void*,
                                                                    unsigned int, const void*, unsigned int, const void*))dfn("vkCmdPipelineBarrier");
                                    if (bar54b) {
                                        struct { uint32_t sType; char p0[4]; const void* pNext;
                                                 uint32_t srcAccess, dstAccess; } mbP;
                                        memset(&mbP, 0, sizeof mbP);
                                        mbP.sType = 46;
                                        mbP.srcAccess = 0x1000u;                 /* TRANSFER_WRITE */
                                        mbP.dstAccess = 0x8000u | 0x10000u;      /* MEMORY_READ | MEMORY_WRITE */
                                        bar54b(cb, 0x1000u, 0x10000u, 0, 1, &mbP, 0, NULL, 0, NULL);  /* TRANSFER -> ALL_COMMANDS */
                                    }
                                }
                                { static int c54; if (!c54) { c54 = 1; stepm("m9-bankcopy-recorded", 1); } }
                                {   /* r59s: the Output image handle the bankcopy writes — logged
                                     * once per distinct handle, to identify which game Output
                                     * image receives the bank copy */
                                    static unsigned long long last_out53;
                                    if (game_out53 && game_out53 != last_out53) {
                                        last_out53 = game_out53;
                                        stepm("m9-out53-handle", (long)game_out53);
                                    }
                                }
                                {   /* r59u (oracle Q4): verify the LIVE Output's compression flags at
                                     * copy time — create-time verification can be fooled by handle
                                     * recycling; this reads the image the copy is actually writing.
                                     * r60: icc logging — CENSUS-gated (absent by default); the
                                     * layout2 lookup resolves once (static + tried flag). */
                                    static unsigned s_icc_ck;
                                    static int cen53 = -1;
                                    if (cen53 < 0)
                                        cen53 = access(SEAM_LOG_DIR "/fsr4npu-CENSUS", F_OK) == 0;
                                    if (cen53 && ((++s_icc_ck) & 1023u) == 1u) {
                                        static void (*fl2x)(void*, unsigned long long, const void*, void*);
                                        static int fl2x_tried;
                                        if (!fl2x_tried) {
                                            fl2x_tried = 1;
                                            fl2x = (void (*)(void*, unsigned long long, const void*, void*))g_down_gdpa(g_chain_dev, "vkGetImageSubresourceLayout2");
                                            if (!fl2x) fl2x = (void (*)(void*, unsigned long long, const void*, void*))g_down_gdpa(g_chain_dev, "vkGetImageSubresourceLayout2EXT");
                                        }
                                        if (fl2x && game_out53) {
                                            struct m9_ICCProps_ props = { 1000338004u, (void*)0, 9999u, 9999u };
                                            struct m9_ImgSub2_ sub  = { 1000338003u, &props, 1u, 0u, 0u };
                                            struct { uint32_t sType; void* pNext; unsigned long long off, size, row, arr, dep; } lay;
                                            memset(&lay, 0, sizeof lay);
                                            fl2x(g_chain_dev, game_out53, &sub, &lay);
                                            stepm("m9-out53-live-iccflags", (long)props.flags);   /* 4 = DISABLED honored on the live image */
                                        } else stepm("m9-out53-live-nofn", 0);
                                    }
                                }
                                skip_bankcopy54: ;
                                {   /* r60 feed9w: retire marker (oracle round-9 Q3), gated
                                     * fsr4npu-MARKER. GPU-side completion signal: the
                                     * UpdateBuffer lands only after the preceding
                                     * dispatches+copy retire; the worker reads the word at
                                     * wake and logs the lag distribution. */
                                    if (g_retOn < 0) { g_retOn = access(SEAM_LOG_DIR "/fsr4npu-MARKER", F_OK) == 0; stepm("m9-retire-on", g_retOn); }
                                    if (g_retOn && g_retBuf) {
                                        static void (*rb1)(void*, unsigned int, unsigned int, unsigned int, unsigned int, const void*, unsigned int, const void*, unsigned int, const void*);
                                        static void (*rub)(void*, unsigned long long, unsigned long long, uint32_t, const void*);
                                        if (!rb1) { rb1 = dfn("vkCmdPipelineBarrier"); rub = dfn("vkCmdUpdateBuffer"); }
                                        if (rb1 && rub) {
                                            struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t srcAccess, dstAccess; } mb;
                                            memset(&mb, 0, sizeof mb); mb.sType = 46;
                                            mb.srcAccess = 0x40u; mb.dstAccess = 0x2000u;      /* SHADER_WRITE -> HOST_READ */
                                            rb1(cb, 0x800u, 0x4000u, 0, 1, &mb, 0, NULL, 0, NULL);   /* COMPUTE -> HOST */
                                            { uint32_t sv = (uint32_t)(seq & 0xffffffffull); rub(cb, g_retBuf, 0, 4, &sv); }
                                            memset(&mb, 0, sizeof mb); mb.sType = 46;
                                            mb.srcAccess = 0x1000u; mb.dstAccess = 0x2000u;     /* TRANSFER_WRITE -> HOST_READ */
                                            rb1(cb, 0x1000u, 0x4000u, 0, 1, &mb, 0, NULL, 0, NULL); /* TRANSFER -> HOST */
                                        }
                                    }
                                }
                            }
                        }
                        /* r59 INPUT FEED (fsr4npu-FEED) — proof-of-flow: the
                         * game's Color round-trips through the NPU without a
                         * seat rebuild. Per eval on the game CB: i2b Color ->
                         * stageA (host-visible, layer-created on the chain
                         * device); CPU: stageA -> outq (WRONG FORMAT — RGBA16F
                         * bytes consumed as int8 channels; correctness needs
                         * the rawlr/R11G11B10F path, this proves the LEG);
                         * the t7 worker executes the NPU; CPU: npudst ->
                         * stageB; b2b stageB -> route_buf; the bank copy above
                         * presents the model's response to the game's frames.
                         * NODISPATCH must be ON (wv1 would overwrite outq).
                         * r59b CONVERT MODE (FEED + NODISPATCH ABSENT): the
                         * layer converts stageA RGBA16F -> R11G11B10F into a
                         * layer-owned mirror buffer and REBINDS the seat's
                         * plan[0] (wv1) binding 5 to it — a SAME-DEVICE host
                         * vkUpdateDescriptorSets (the binding-8 precedent;
                         * the seat chassis sets live on the game device).
                         * With dispatches on, wv1 consumes the REAL game
                         * color; the wrong-format memcpys are skipped. */
                        {
                            static int feed_on = -1, feed_state = 0, feed_n = 0;
                            static void *mapA = NULL, *mapB = NULL, *mapM = NULL, *mapMVs = NULL, *mapEX = NULL, *mapMVm = NULL;
                            static unsigned long long bufA = 0, bufB = 0, memA = 0, memB = 0;
                            static unsigned long long bufM = 0, memM = 0, wv1_set = 0;
                            static unsigned long long bufMVs = 0, memMVs = 0, bufMVm = 0, memMVm = 0;
                            static unsigned long long bufEX = 0, memEX = 0;
                            static unsigned int g_stage_props = 0xfu;   /* r59g: HOST_VISIBLE|COHERENT|CACHED bits of the staging type (default: assume coherent) */
                            static unsigned long long g_snapBufL = 0, g_snapMemL = 0, g_recFixBufL = 0, g_recFixMemL = 0;
                            static int rebind_done = 0;
                            if (feed_on < 0) {
                                FILE* mf = fopen(SEAM_LOG_DIR "/fsr4npu-FEED", "r");
                                feed_on = mf ? (fclose(mf), 1) : 0;
                                stepm("m9-feed-marker", feed_on);
                            }
                            if (feed_on && feed_state == 0) {
                                int32_t (*fcb)(void*, const void*, const void*, void**)  = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreateBuffer");
                                void (*fbr)(void*, void*, void*)                          = (void (*)(void*, void*, void*))dfn("vkGetBufferMemoryRequirements");
                                int32_t (*fam)(void*, const void*, const void*, void**)  = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkAllocateMemory");
                                int32_t (*fbb)(void*, void*, void*, unsigned long long)  = (int32_t (*)(void*, void*, void*, unsigned long long))dfn("vkBindBufferMemory");
                                int32_t (*fmp)(void*, void*, unsigned long long, unsigned long long, uint32_t, void**) = (int32_t (*)(void*, void*, unsigned long long, unsigned long long, uint32_t, void**))dfn("vkMapMemory");
                                struct { uint32_t sType; char p0[4]; const void* pNext; uint32_t flags; char p1[4];
                                         unsigned long long size; uint32_t usage; char p2[4];
                                         int sharingMode; char p3[4]; } bci;
                                struct { uint32_t sType; char p0[4]; const void* pNext; unsigned long long size; uint32_t type; char p1[4]; } mai;
                                struct { unsigned long long size, alignment; uint32_t bits; char p[4]; } req;
                                int ok = 1;
                                if (!fcb || !fbr || !fam || !fbb || !fmp) { stepm("m9-feed-fn", 0); feed_state = -1; ok = 0; }
                                while (ok) {   /* 0 stageA 1 stageB 2 rawlr-mirror 3 MV-stage 4 MV-mirror 5 expo 6 npu-snap 7 rec-fix (r59g: the FIXED banks, alloc'd record-side so the one-shot rebind never races the t7 thread) */
                                    int which = !bufA ? 0 : !bufB ? 1 : !bufM ? 2 : !bufMVs ? 3 : !bufMVm ? 4 : !bufEX ? 5 : !g_snapBufL ? 6 : !g_recFixBufL ? 7 : !g_retBuf ? 8 : 9;
                                    unsigned long long sz = which == 0 ? 1843200ull : which == 1 ? 7372800ull :
                                                            which <= 4 ? 921600ull : (which == 5 || which == 8) ? 16ull : 7372800ull;

                                    unsigned long long *pbuf = which == 0 ? &bufA : which == 1 ? &bufB : which == 2 ? &bufM :
                                                              which == 3 ? &bufMVs : which == 4 ? &bufMVm : which == 5 ? &bufEX :
                                                              which == 6 ? &g_snapBufL : which == 7 ? &g_recFixBufL : &g_retBuf;
                                    unsigned long long *pmem = which == 0 ? &memA : which == 1 ? &memB : which == 2 ? &memM :
                                                              which == 3 ? &memMVs : which == 4 ? &memMVm : which == 5 ? &memEX :
                                                              which == 6 ? &g_snapMemL : which == 7 ? &g_recFixMemL : &g_retMem;
                                    void **pmap = which == 0 ? &mapA : which == 1 ? &mapB : which == 2 ? &mapM :
                                                  which == 3 ? &mapMVs : which == 4 ? &mapMVm : which == 5 ? &mapEX :
                                                  which == 8 ? (void**)&g_retMap : NULL;
                                    int32_t r;
                                    if (which == 2 && g_nodispatch) { feed_state = 1; stepm("m9-feed-armed", (long)((bufA & 0xffff) | ((bufB & 0xffff) << 16))); break; }
                                    if (which == 9) { g_snapBuf = g_snapBufL; g_recFixBuf = g_recFixBufL; feed_state = 1; stepm("m9-feed-armed7", (long)((bufA & 0xffff) | ((bufEX & 0xffff) << 16))); break; }
                                    if (which == 0 || which == 3) g_stage_props = (g_r19_mp.cnt > mai.type) ? (g_r19_mp.types[mai.type].props & 0xfu) : 0xfu;   /* r59g: coherence decision for the staging reads */
                                    memset(&bci, 0, sizeof bci);
                                    bci.sType = 9; bci.size = sz; bci.usage = 0x22;  /* STORAGE|TRANSFER_DST (B also SRC) */
                                    if (which == 1) bci.usage = 0x23;                /* +TRANSFER_SRC for the b2b */
                                    r = fcb(g_chain_dev, &bci, NULL, (void**)pbuf);
                                    if (r != 0 || !*pbuf) { stepm("m9-feed-buf", (long)((r << 24) | which)); ok = 0; break; }
                                    fbr(g_chain_dev, (void*)*pbuf, &req);
                                    memset(&mai, 0, sizeof mai);
                                    mai.sType = 5; mai.size = req.size;
                                    {   /* r59g: type from req.bits (was hard-coded 0) */
                                        int mtf = m9_pick_mem_type(req.bits);
                                        if (mtf < 0) { stepm("m9-feed-mt", which); ok = 0; break; }
                                        mai.type = (uint32_t)mtf;
                                    }
                                    r = fam(g_chain_dev, &mai, NULL, (void**)pmem);
                                    if (r != 0 || !*pmem) { stepm("m9-feed-mem", (long)((r << 24) | which)); ok = 0; break; }
                                    r = fbb(g_chain_dev, (void*)*pbuf, (void*)*pmem, 0);
                                    if (r != 0) { stepm("m9-feed-bind", (long)((r << 24) | which)); ok = 0; break; }
                                    if (pmap) {
                                        r = fmp(g_chain_dev, (void*)*pmem, 0, ~(unsigned long long)0, 0, pmap);
                                        if (r != 0 || !*pmap) { stepm("m9-feed-map", (long)((r << 24) | which)); ok = 0; break; }
                                    }
                                }
                            }
                            if (feed_on && feed_state == 1) {
                                static void (*cpy_i2bF)(void*, unsigned long long, unsigned int, unsigned long long, unsigned int, const void*);
                                static void (*cpy_b2bF)(void*, unsigned long long, unsigned long long, unsigned int, const void*);
                                const unsigned char* fpF = (const unsigned char*)seam_last_frame(NULL, NULL);
                                const void* outq = NULL; void* npudst = NULL;
                                uint64_t ob = 0, nb = 0;
                                if (fpF && h) {
                                    unsigned long long color_img = *(const unsigned long long*)(fpF + 112);   /* res[0].image */
                                    unsigned int cw = *(const unsigned int*)(fpF + 144), chh = *(const unsigned int*)(fpF + 148);
                                    unsigned long long rbuf = 0, rimg = 0; uint32_t rw = 0, rh = 0;
                                    if (!cpy_i2bF) cpy_i2bF = (void (*)(void*, unsigned long long, unsigned int, unsigned long long, unsigned int, const void*))dfn("vkCmdCopyImageToBuffer");
                                    if (!cpy_b2bF) cpy_b2bF = (void (*)(void*, unsigned long long, unsigned long long, unsigned int, const void*))dfn("vkCmdCopyBuffer");
                                    if (cpy_i2bF && color_img && cw == 640 && chh == 360) {
                                        struct { unsigned long long off; unsigned int rowLen, imgH;
                                                 struct { unsigned int aspect, mip, layer, count; } sub;
                                                 struct { int x, y, z; } o;
                                                 struct { unsigned int w, h, d; } e; } cf;
                                        memset(&cf, 0, sizeof cf);
                                        cf.sub.aspect = 1; cf.sub.count = 1;
                                        cf.e.w = cw; cf.e.h = chh; cf.e.d = 1;
                                        cpy_i2bF(cb, color_img, 1 /*GENERAL*/, bufA, 1, &cf);
                                    }
                                    if (!g_nodispatch && cpy_i2bF) {
                                        /* REAL INPUT SET: MV via staging (CPU scales
                                         * pixel-units -> normalized: wv1 multiplies vel
                                         * by IN_W/IN_H, and NGX MVs at MV.Scale=1 are
                                         * output-pixel units) and the PREVIOUS Output
                                         * verbatim (recp's exact layout). */
                                        unsigned long long mv_img = *(const unsigned long long*)(fpF + 240);  /* res[2].image */
                                        unsigned int mvw = *(const unsigned int*)(fpF + 272), mvh = *(const unsigned int*)(fpF + 276);
                                        unsigned long long out_img = *(const unsigned long long*)(fpF + 304); /* res[3].image */
                                        unsigned long long exp_img = *(const unsigned long long*)(fpF + 368);  /* res[4].image */
                                        unsigned int owF = *(const unsigned int*)(fpF + 336), ohF = *(const unsigned int*)(fpF + 340);
                                        struct { unsigned long long off; unsigned int rowLen, imgH;
                                                 struct { unsigned int aspect, mip, layer, count; } sub;
                                                 struct { int x, y, z; } o;
                                                 struct { unsigned int w, h, d; } e; } mf;
                                        memset(&mf, 0, sizeof mf);
                                        mf.sub.aspect = 1; mf.sub.count = 1;
                                        if (mv_img && mvw == 640 && mvh == 360) {
                                            mf.e.w = mvw; mf.e.h = mvh; mf.e.d = 1;
                                            cpy_i2bF(cb, mv_img, 1 /*GENERAL*/, bufMVs, 1, &mf);
                                        }
                                        if (exp_img) {
                                            mf.e.w = 1; mf.e.h = 1; mf.e.d = 1;
                                            cpy_i2bF(cb, exp_img, 1 /*GENERAL*/, bufEX, 1, &mf);
                                        }
                                        {   /* r59e: lag-1 jitter — the feeds are one
                                             * frame behind, the push must match. */
                                            float jx = *(const volatile float*)(fpF + 48);
                                            float jy = *(const volatile float*)(fpF + 52);
                                            g_jx_lag = jx; g_jy_lag = jy;
                                        }
                                    }
                                    if (!g_nodispatch && mapM) {
                                        if (!g_feed_active) g_feed_active = 1;
                                        /* r59g PERF: the convert segment is timed start->close (m9-convert-*)
                                         * — the memtype fix's target. Plus the coherence invalidate when the
                                         * staging type is CACHED but non-COHERENT (the GPU wrote last eval's
                                         * i2b into it). */
                                        static struct timespec s_ct0;
                                        clock_gettime(CLOCK_MONOTONIC, &s_ct0);
                                        if ((g_stage_props & 0x4u) == 0u) {
                                            static void (*finv)(void*, uint32_t, const void*);
                                            if (!finv) finv = (void (*)(void*, uint32_t, const void*))dfn("vkInvalidateMappedMemoryRanges");
                                            if (finv && memA && memMVs) {
                                                struct { uint32_t sType; char p0[4]; const void* pNext;
                                                         unsigned long long mem; unsigned long long off, sz; } rg[2];
                                                memset(rg, 0, sizeof rg);
                                                rg[0].sType = 6; rg[0].mem = memA;   rg[0].sz = 1843200ull;
                                                rg[1].sType = 6; rg[1].mem = memMVs; rg[1].sz = 921600ull;
                                                finv(g_chain_dev, 2, rg);
                                            }
                                        }
                                        /* CONVERT MODE: stageA -> R11G11B10F mirror;
                                         * MV staging -> scaled fp16 pairs -> mvec mirror;
                                         * expo staging (lag-1) -> the wv1 push patch;
                                         * rebind bindings 5/12/3 (one-time, same-device). */
                                        const unsigned short* c16 = (const unsigned short*)mapA;
                                        unsigned int* r11 = (unsigned int*)mapM;
                                        unsigned long long i, np = (unsigned long long)cw * chh;
                                        if (cw == 640 && chh == 360) {
#if defined(__ARM_NEON)
                                            /* r59f PERF: 8 px/iter NEON — the scalar loop was
                                             * ~1 ms/eval on the game's record thread (the layer
                                             * must cost essentially nothing; edge-device law). */
                                            unsigned long long n8 = np >> 3;
                                            const float32x4_t rz = vdupq_n_f32(0.0f);
                                            for (i = 0; i < n8; i++) {
                                                uint16x8x4_t px = vld4q_u16(c16 + i * 32);   /* RGBA planes x8 */
                                                uint32x4_t r0 = vmovl_u16(vget_low_u16(px.val[0]));
                                                uint32x4_t r1 = vmovl_u16(vget_high_u16(px.val[0]));
                                                uint32x4_t g0 = vmovl_u16(vget_low_u16(px.val[1]));
                                                uint32x4_t g1 = vmovl_u16(vget_high_u16(px.val[1]));
                                                uint32x4_t b0 = vmovl_u16(vget_low_u16(px.val[2]));
                                                uint32x4_t b1 = vmovl_u16(vget_high_u16(px.val[2]));
                                                /* pack: (r>>4) | (g>>4)<<11 | (b>>5)<<22 */
                                                uint32x4_t o0 = vorrq_u32(vorrq_u32(vshrq_n_u32(r0, 4), vshlq_n_u32(vshrq_n_u32(g0, 4), 11)), vshlq_n_u32(vshrq_n_u32(b0, 5), 22));
                                                uint32x4_t o1 = vorrq_u32(vorrq_u32(vshrq_n_u32(r1, 4), vshlq_n_u32(vshrq_n_u32(g1, 4), 11)), vshlq_n_u32(vshrq_n_u32(b1, 5), 22));
                                                vst1q_u32(r11 + i * 8, o0);
                                                vst1q_u32(r11 + i * 8 + 4, o1);
                                                (void)rz;
                                            }
#else
                                            for (i = 0; i < np; i++) {
                                                unsigned short r = c16[i * 4 + 0], g = c16[i * 4 + 1], b = c16[i * 4 + 2];
                                                r11[i] = ((unsigned int)r >> 4) | (((unsigned int)g >> 4) << 11) | (((unsigned int)b >> 5) << 22);
                                            }
#endif
                                            feed_n++;
                                            if (feed_n == 1) stepm("m9-convert-first", (long)r11[0]);
                                        }
                                        if (mapMVs && mapMVm) {
                                            /* r59d fix-3b (external analyst): scale STAGING ->
                                             * MIRROR (never in place). vel = mv_px / out_dims.
                                             * r59f PERF: NEON, 8 px/iter (f16 -> f32 -> scale
                                             * -> f16, interleaved store). */
                                            const unsigned short* s16 = (const unsigned short*)mapMVs;
                                            unsigned short* d16 = (unsigned short*)mapMVm;
#if defined(__ARM_NEON)
                                            const float32x4_t sx = vdupq_n_f32(1.0f / 1280.0f);
                                            const float32x4_t sy = vdupq_n_f32(1.0f / 720.0f);
                                            for (i = 0; i < 230400ull >> 3; i++) {
                                                uint16x8x2_t mv = vld2q_u16(s16 + i * 16);   /* xy planes x8 */
                                                float32x4_t x0 = vmulq_f32(vcvt_f32_f16(vget_low_u16(mv.val[0])), sx);
                                                float32x4_t x1 = vmulq_f32(vcvt_f32_f16(vget_high_u16(mv.val[0])), sx);
                                                float32x4_t y0 = vmulq_f32(vcvt_f32_f16(vget_low_u16(mv.val[1])), sy);
                                                float32x4_t y1 = vmulq_f32(vcvt_f32_f16(vget_high_u16(mv.val[1])), sy);
                                                uint16x8_t vx = vcombine_u16(vcvt_f16_f32(x0), vcvt_f16_f32(x1));
                                                uint16x8_t vy = vcombine_u16(vcvt_f16_f32(y0), vcvt_f16_f32(y1));
                                                {   uint16x8x2_t o; o.val[0] = vx; o.val[1] = vy;
                                                    vst2q_u16(d16 + i * 16, o); }
                                            }
#else
                                            for (i = 0; i < 230400ull; i++) {
                                                float vx = m9_f16_to_f32(s16[i * 2 + 0]) * (1.0f / 1280.0f);
                                                float vy = m9_f16_to_f32(s16[i * 2 + 1]) * (1.0f / 720.0f);
                                                d16[i * 2 + 0] = m9_f32_to_f16(vx);
                                                d16[i * 2 + 1] = m9_f32_to_f16(vy);
                                            }
#endif
                                        }
                                        if (mapEX) {
                                            float e = *(volatile float*)mapEX;
                                            if (e > 1e-4f && e < 1e4f) { g_feed_expo = e; if (!g_feed_expo_ok) { g_feed_expo_ok = 1; stepm("m9-expo-first", (long)(*(unsigned int*)&e)); } }
                                        }
                                        {   /* the convert-segment timer close */
                                            static double s_ct_tot; static long s_ct_n; static double s_ct_max;
                                            struct timespec s_ct1;
                                            double el;
                                            clock_gettime(CLOCK_MONOTONIC, &s_ct1);
                                            el = (double)(s_ct1.tv_sec - s_ct0.tv_sec) * 1e3 + (double)(s_ct1.tv_nsec - s_ct0.tv_nsec) / 1e6;
                                            s_ct_tot += el; s_ct_n++;
                                            if (el > s_ct_max) s_ct_max = el;
                                            if ((s_ct_n & 0x3FF) == 1) {
                                                stepm("m9-convert-mean-us", (long)(s_ct_tot / (double)s_ct_n * 1000.0));
                                                stepm("m9-convert-max-us", (long)(s_ct_max * 1000.0));
                                            }
                                        }
                                        if (g_ring_ok && h) {
                                            /* r59g: nothing per-eval here anymore — the
                                             * snapshot copy (dispatch loop, stage-1 gate)
                                             * feeds the fixed bank; bindings 3/4/1 are
                                             * re-bound ONCE in the rebind block below. */
                                            (void)0;
                                        }
                                        if (!rebind_done && h) {
                                            uint64_t lay = 0, set = 0, dis = 0; uint32_t dgz = 0, pb = 0; uint32_t push[11]; uint32_t pat = 0;
                                            void (*fuds)(void*, uint32_t, const void*, uint32_t, const void*) = (void (*)(void*, uint32_t, const void*, uint32_t, const void*))dfn("vkUpdateDescriptorSets");
                                            /* r59g: one-shot, same-device host writes —
                                             * b3 <- the FIXED rec bank (worker LUT output,
                                             * snap-copied per gen), postA's b4 <- the FIXED
                                             * NPU snapshot bank. postA's b1 (recout) keeps
                                             * its seat target — nothing reads it now. */
                                            { int g4ok = (g_recFixBuf && g_snapBuf); int nw = g4ok ? 3 : 2;
                                            if (fuds && seam_comp_plan(0, h, &lay, &set, &dis, &dgz, &pb, push, &pat) && set) {
                                                struct { unsigned long long buffer, offset, range; } biF[3];
                                                struct { uint32_t sType; char p0[4]; const void* pNext; void* dstSet;
                                                         uint32_t dstBinding, dstArrayElement, descriptorCount, descriptorType;
                                                         const void* pImg; const void* pBuf; const void* pTexel; } wrF[3];
                                                memset(biF, 0, sizeof biF);
                                                biF[0].buffer = bufM;  biF[0].range = 921600ull;    /* b5  rawlr */
                                                biF[1].buffer = bufMVm; biF[1].range = 921600ull;   /* b12 mvec (the scaled mirror) */
                                                biF[2].buffer = g_recFixBuf; biF[2].range = 7372800ull;   /* b3 recp <- the fixed rec bank (r59g) */
                                                memset(wrF, 0, sizeof wrF);
                                                wrF[0].dstBinding = 5;  wrF[1].dstBinding = 12; wrF[2].dstBinding = 3;
                                                { int k; for (k = 0; k < nw; k++) {
                                                    wrF[k].sType = 35; wrF[k].dstSet = (void*)(uintptr_t)set;
                                                    wrF[k].descriptorCount = 1; wrF[k].descriptorType = 7;
                                                    wrF[k].pBuf = &biF[k];
                                                } }
                                                fuds(g_chain_dev, nw, wrF, 0, NULL);
                                                wv1_set = set; rebind_done = 1;
                                                stepm("m9-rebind3-ok", (long)((set & 0xffffff) | ((unsigned long)g4ok << 24)));
                                                {   /* r59d fix-1 (external analyst): postA (plan 1)
                                                     * builds its LDS current-color tile from ITS OWN
                                                     * binding 5 — which still pointed at the seat's
                                                     * GRADIENT-filled rawlr bank. Rebind it to the
                                                     * live mirror too, or `up` is forever the test
                                                     * pattern and real content only arrives via the
                                                     * history blend.
                                                     * r59g: ALSO point postA's b4 at the FIXED
                                                     * snapshot bank and its b1 (recout) at the
                                                     * expansion's u8 source — both one-time. */
                                                    uint64_t lay1 = 0, set1 = 0, dis1 = 0;
                                                    uint32_t dgz1 = 0, pb1 = 0, push1[11], pat1 = 0;
                                                    if (seam_comp_plan(1, h, &lay1, &set1, &dis1, &dgz1, &pb1, push1, &pat1) && set1) {
                                                        struct { unsigned long long buffer, offset, range; } biP[2];
                                                        struct { uint32_t sType; char p0[4]; const void* pNext; void* dstSet;
                                                                 uint32_t dstBinding, dstArrayElement, descriptorCount, descriptorType;
                                                                 const void* pImg; const void* pBuf; const void* pTexel; } wrP[3];
                                                        int n1 = 1;
                                                        memset(biP, 0, sizeof biP);
                                                        wrF[0].dstSet = (void*)(uintptr_t)set1;
                                                        fuds(g_chain_dev, 1, wrF, 0, NULL);   /* b5 -> live color mirror */
                                                        if (g4ok) {
                                                            biP[0].buffer = g_snapBuf; biP[0].range = 7372800ull;   /* b4 npu-in <- fixed snapshot */
                                                            memset(wrP, 0, sizeof wrP);
                                                            wrP[0].dstBinding = 4; n1 = 1;
                                                            { int k; for (k = 0; k < n1; k++) {
                                                                wrP[k].sType = 35; wrP[k].dstSet = (void*)(uintptr_t)set1;
                                                                wrP[k].descriptorCount = 1; wrP[k].descriptorType = 7;
                                                                wrP[k].pBuf = &biP[k];
                                                            } }
                                                            fuds(g_chain_dev, n1, wrP, 0, NULL);
                                                        }
                                                        stepm("m9-rebind-p1-ok", (long)((set1 & 0xffffff) | ((unsigned long)g4ok << 24)));
                                                    }
                                                }
                                            } else {
                                                stepm("m9-rebind-noplain", 0);
                                                rebind_done = -1;
                                            }
                                            }   /* g4ok scope close */
                                        }
                                    } else if (seam_npu_handshake(h, &outq, &npudst, NULL, NULL, &ob, &nb) && outq && npudst) {
                                        unsigned long long nA = ob < 1843200ull ? ob : 1843200ull;
                                        unsigned long long nB = nb < 7372800ull ? nb : 7372800ull;
                                        memcpy((void*)outq, mapA, (size_t)nA);     /* game color -> NPU input (lag-1) */
                                        memcpy(mapB, npudst, (size_t)nB);          /* NPU output -> stage (recent) */
                                    }
                                    if (g_nodispatch) { extern int seam_route_read(SeamHeader__*, uint64_t*, uint64_t*, uint32_t*, uint32_t*);
                                      if (seam_route_read(h, &rbuf, &rimg, &rw, &rh) && rbuf && cpy_b2bF) {
                                        struct { unsigned long long so, dof, sz; } rf;
                                        memset(&rf, 0, sizeof rf);
                                        rf.sz = 7372800ull;
                                        cpy_b2bF(cb, bufB, rbuf, 1, &rf);
                                        feed_n++;
                                        if (feed_n == 1 || (feed_n & 255) == 0) {
                                            unsigned long long ha = 1469598103934665603ull, hb = 1469598103934665603ull, i;
                                            const unsigned int* a = (const unsigned int*)mapA; const unsigned int* b = (const unsigned int*)mapB;
                                            for (i = 0; i < 65536ull; i++) { ha = (ha ^ (unsigned long long)a[i]) * 1099511628211ull; hb = (hb ^ (unsigned long long)b[i]) * 1099511628211ull; }
                                            stepm("m9-feed-live", (long)feed_n);
                                            stepm("m9-feed-hashA", (long)(ha & 0xffffffff));
                                            stepm("m9-feed-hashB", (long)(hb & 0xffffffff));
                                        }
                                    } }
                                    if (!g_nodispatch && (feed_n == 1 || (feed_n & 255) == 0)) {
                                        unsigned long long ha = 1469598103934665603ull, i;
                                        const unsigned int* a = (const unsigned int*)mapA;
                                        for (i = 0; i < 65536ull; i++) ha = (ha ^ (unsigned long long)a[i]) * 1099511628211ull;
                                        stepm("m9-convert-live", (long)feed_n);
                                        stepm("m9-convert-hashA", (long)(ha & 0xffffffff));
                                    }
                                }
                            }
                        }
                        /* REV55: THE ATTACHMENT-PATH TEST — the ONE image-write
                         * class the game itself uses. Create (once) a
                         * RenderPass + ImageView of the game's Output image +
                         * Framebuffer — all create classes UNTESTED (only
                         * vkCreateComputePipelines is proven lethal) — then
                         * per eval: BeginRenderPass(loadOp=CLEAR white) +
                         * EndRenderPass on the GAME'S CB. If the sighted hash
                         * flips to white: the image handle is RIGHT, transfer
                         * paths were the poison, and M9's route is open. */
                        {
                            static unsigned long long s_rp, s_iv, s_fb;
                            static int s_rp_state;   /* 0 untouched, 1 ok, -1 failed */
                            static void (*brp)(void*, const void*, void*, unsigned int, unsigned int, unsigned int, unsigned int);
                            static void (*erp)(void*);
                            if (!s_rp_state) {
                                /* one-time creates, each bracketed */
                                int32_t (*fiv)(void*, const void*, const void*, void**) = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreateImageView");
                                int32_t (*frp)(void*, const void*, const void*, void**) = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreateRenderPass");
                                int32_t (*ffb)(void*, const void*, const void*, void**) = (int32_t (*)(void*, const void*, const void*, void**))dfn("vkCreateFramebuffer");
                                brp = (void (*)(void*, const void*, void*, unsigned int, unsigned int, unsigned int, unsigned int))dfn("vkCmdBeginRenderPass");
                                erp = (void (*)(void*))dfn("vkCmdEndRenderPass");
                                if (!fiv || !frp || !ffb || !brp || !erp) { s_rp_state = -1; stepm("m9-rp-fn", 0); }
                                else {
                                    /* VkImageViewCI — REV58d ABI FIX (wrapper-bypass
                                     * research, instruction-proven): real layout is
                                     * sType@0,pNext@8,flags@0x10,image@0x18,viewType@0x20,
                                     * format@0x24,components@0x28(16B),
                                     * subresource@0x38{aspect,baseMip,levelCount,baseLayer,
                                     * layerCount} size 0x50. The REV55 layout put image@0x10
                                     * (pre-flags shape) so the driver read image=viewType=2
                                     * and SIGSEGV'd in tu_image_view_init — the "hang". */
                                    struct { uint32_t sType; const void* pNext; uint32_t flags;
                                             unsigned long long image; uint32_t viewType, format;
                                             struct { int r, g, b, a; } comp;
                                             struct { uint32_t aspect, mip, levelCount, baseLayer, layerCount; } sub; } ivci;
                                    /* VkAttachmentDescription: flags,format,samples,loadOp,storeOp,stencilLoadOp,stencilStoreOp,initialLayout,finalLayout (9x u32 = 36B) */
                                    struct { uint32_t flags, format, samples, loadOp, storeOp, sLoad, sStore, init, fin; } att;
                                    /* VkAttachmentReference: attachment,layout */
                                    struct { uint32_t attachment, layout; } cref;
                                    /* VkSubpassDescription — r58k ABI FIX (natural
                                     * layout, NO manual pads between u32 pairs):
                                     * flags@0,bindPoint@4,inCount@8,pIn@0x10,
                                     * colCount@0x18,pCol@0x20,resCount@0x28,
                                     * pRes@0x30,pDS@0x38,preCount@0x40,pPre@0x48 */
                                    struct { uint32_t flags, bindPoint;
                                             uint32_t inCount; char p2[4]; const void* pIn;
                                             uint32_t colCount; char p3[4]; const void* pCol;
                                             uint32_t resCount; char p4[4]; const void* pRes;
                                             const void* pDS;
                                             uint32_t preCount; char p5[4]; const void* pPre; } spd;
                                    /* VkRenderPassCreateInfo — r58k ABI FIX (natural):
                                     * sType@0,pNext@8,flags@0x10,attCount@0x14,
                                     * pAtt@0x18,subCount@0x20,pSub@0x28,
                                     * depCount@0x30,pDep@0x38, size 0x40 */
                                    struct { uint32_t sType; const void* pNext; uint32_t flags;
                                             uint32_t attCount; const void* pAtt;
                                             uint32_t subCount; const void* pSub;
                                             uint32_t depCount; const void* pDep; } rpci;
                                    /* VkFramebufferCreateInfo — r58k ABI FIX:
                                     * sType@0,pNext@8,flags@0x10,rp@0x18,
                                     * attCount@0x20,pAtt@0x28,w@0x30,h@0x34,
                                     * layers@0x38, size 0x40 */
                                    struct { uint32_t sType; const void* pNext; uint32_t flags; char p0[4];
                                             unsigned long long rp; uint32_t attCount; char p1[4];
                                             const void* pAtt; uint32_t w, h, layers; } fbci;
                                    /* VkRenderPassBeginInfo 64B: sType,pNext,renderPass,fb,renderArea(offset x,y + extent w,h),clearCount,pClear */
                                    struct { uint32_t sType; char p0[4]; const void* pNext;
                                             unsigned long long rp, fb;
                                             struct { int x, y; uint32_t w, h; } area;
                                             uint32_t clrCount; char p1[4];
                                             const void* pClr; } rpbi;
                                    /* VkClearValue = VkClearColorValue (4 floats) */
                                    float clr[4];
                                    float white[4];
                                    memset(&ivci, 0, sizeof ivci);
                                    ivci.sType = 15; ivci.flags = 0; ivci.image = game_out53; ivci.viewType = 2; ivci.format = 97; /* 2D, RGBA16F */
                                    ivci.comp.r = 0; ivci.comp.g = 0; ivci.comp.b = 0; ivci.comp.a = 0; /* IDENTITY swizzle */
                                    ivci.sub.aspect = 1; ivci.sub.mip = 0; ivci.sub.levelCount = 1; ivci.sub.baseLayer = 0; ivci.sub.layerCount = 1;
                                    stepm("m9-rp-pre-iv", 1);
                                    if (guarded_create("m9-rp-iv-rc", fiv, (void*)g_chain_dev, &ivci, (void**)&s_iv) != 0 || !s_iv) { s_rp_state = -1; stepm("m9-rp-iv-fail", 0); }
                                    else {
                                        stepm("m9-rp-iv-ok", (long)(s_iv & 0xffffffff));
                                        memset(&att, 0, sizeof att);
                                        att.format = 97; att.samples = 1; att.loadOp = 2 /*CLEAR*/; att.storeOp = 1 /*STORE*/; att.init = 1; att.fin = 1;
                                        cref.attachment = 0; cref.layout = 1; /* GENERAL */
                                        memset(&spd, 0, sizeof spd);
                                        spd.bindPoint = 1; /* GRAPHICS */
                                        spd.colCount = 1; spd.pCol = &cref;
                                        memset(&rpci, 0, sizeof rpci);
                                        rpci.sType = 17; /* RENDER_PASS_CREATE_INFO */
                                        rpci.attCount = 1; rpci.pAtt = &att;
                                        rpci.subCount = 1; rpci.pSub = &spd;
                                        stepm("m9-rp-pre-rp", 1);
                                        if (guarded_create("m9-rp-rp-rc", frp, (void*)g_chain_dev, &rpci, (void**)&s_rp) != 0 || !s_rp) { s_rp_state = -1; stepm("m9-rp-rp-fail", 0); }
                                        else {
                                            stepm("m9-rp-rp-ok", (long)(s_rp & 0xffffffff));
                                            memset(&fbci, 0, sizeof fbci);
                                            fbci.sType = 24; /* FRAMEBUFFER_CREATE_INFO */
                                            fbci.rp = s_rp; fbci.attCount = 1; fbci.pAtt = (const void*)&s_iv;
                                            fbci.w = gw53; fbci.h = gh53; fbci.layers = 1;
                                            stepm("m9-rp-pre-fb", 1);
                                            if (guarded_create("m9-rp-fb-rc", ffb, (void*)g_chain_dev, &fbci, (void**)&s_fb) != 0 || !s_fb) { s_rp_state = -1; stepm("m9-rp-fb-fail", 0); }
                                            else { s_rp_state = 1; stepm("m9-rp-all-ok", (long)(s_fb & 0xffffffff)); }
                                        }
                                    }
                                }
                            }
                            if (s_rp_state == 1 && brp && erp) {
                                struct { uint32_t sType; char p0[4]; const void* pNext;
                                         unsigned long long rp, fb;
                                         struct { int x, y; uint32_t w, h; } area;
                                         uint32_t clrCount; char p1[4];
                                         const void* pClr; } rpbi;
                                float clr[4];
                                clr[0] = 60000.0f; clr[1] = 60000.0f; clr[2] = 60000.0f; clr[3] = 1.0f;
                                memset(&rpbi, 0, sizeof rpbi);
                                rpbi.sType = 48; /* RENDER_PASS_BEGIN_INFO */
                                rpbi.rp = s_rp; rpbi.fb = s_fb;
                                rpbi.area.w = gw53; rpbi.area.h = gh53;
                                rpbi.clrCount = 1; rpbi.pClr = clr;
                                {   /* r58s single-writer: fsr4npu-NORPCLEAR gates this off */
                                    static int norp = -1;
                                    if (norp < 0) { FILE* mf = fopen(SEAM_LOG_DIR "/fsr4npu-NORPCLEAR", "r"); norp = mf ? (fclose(mf), 1) : 0; stepm("m58s-norpclear", norp); }
                                    if (!norp) {
                                        brp(cb, &rpbi, NULL, 0, 0, 0, 0);
                                        erp(cb);
                                        { static int c55; if (!c55) { c55 = 1; stepm("m9-rpclear-recorded", 1); } }
                                    }
                                }
                            }
                        }
                        /* r58l: THE SCREEN TEST — vkCmdClearColorImage needs
                         * no renderpass and turnip validates no layout (law
                         * 11). RED on the CURRENT swapchain image = proof
                         * our game-CB records execute + image writes are
                         * visible. BLUE on the NGX Output = same path, the
                         * M9 target. Any red on screen during the bench
                         * disambiguates "records don't execute" from
                         * "Output-specific invisibility". */
                        {
                            static void (*cci)(void*, unsigned long long, unsigned int, const float*, unsigned int, const void*);
                            static struct { uint32_t aspect, mip, levelCount, baseLayer, layerCount; } subr;
                            static float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
                            static float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
                            if (!cci) cci = (void (*)(void*, unsigned long long, unsigned int, const float*, unsigned int, const void*))dfn("vkCmdClearColorImage");
                            if (!subr.aspect) { subr.aspect = 1; subr.levelCount = 1; subr.layerCount = 1; }
                            if (g_sw_img_count && g_sw_idx < g_sw_img_count && cci) {
                                cci(cb, g_sw_images[g_sw_idx], 1 /*GENERAL*/, red, 1, &subr);
                                { static int c58l; if (!c58l) { c58l = 1; stepm("m58l-swclear-recorded", (long)g_sw_images[g_sw_idx]); } }
                            }
                            if (game_out53 && cci) {
                                /* r58p: blue clear now MARKER-GATED (fsr4npu-BLUECLEAR).
                                 * r58o PROVED it lands + the game presents it (solid
                                 * blue whole bench, 999 evals, game alive). Default OFF
                                 * so the bank-copy (bank54->Output, records BEFORE
                                 * this) becomes the visible writer = the NPU
                                 * pipeline's real output on screen. */
                                static int blue_gate = -1;
                                if (blue_gate < 0) {
                                    FILE* mf = fopen(SEAM_LOG_DIR "/fsr4npu-BLUECLEAR", "r");
                                    blue_gate = mf ? (fclose(mf), 1) : 0;
                                    stepm("m58p-bluegate", blue_gate);
                                }
                                if (blue_gate) {
                                    cci(cb, game_out53, 1 /*GENERAL*/, blue, 1, &subr);
                                    { static int c58l2; if (!c58l2) { c58l2 = 1; stepm("m58l-outclear-recorded", (long)game_out53); } }
                                }
                            }
                        }
                    }
                }
            }
        }
        }   /* !nms_hdr: the R59 plan path */
        seam_write_ack(h, seq, work);
        }   /* nms_hdr scope */
        /* REV35 ack-debug (m9-ackdbg-*) was REPLACED by the REV47 heartbeat
         * below: it fired on (ack_count % 256)==0, which can NEVER re-fire
         * while the count is frozen at 1 — exactly the state it existed to
         * observe. */
    }
    if (down) down(cb, dst, off, n, data);

    if (ours && h) {
        {   /* REV48d: spawn the create-freeze attribution probe ONCE */
            static int probe_spawned = 1;   /* REV49: probe DISABLED — the create-lethality is proven; this run tests CB submission */
            if (!probe_spawned) {
                pthread_t g3pt;
                probe_spawned = 1;
                if (pthread_create(&g3pt, NULL, g3_probe_thread, NULL) == 0)
                    stepm("m9-g3probe-spawned", 1);
            }
        }
        long long d;
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        d = (ts1.tv_sec - ts0.tv_sec) * 1000000000ll + (ts1.tv_nsec - ts0.tv_nsec);
        t_total_ns += d; if (d > t_max_ns) t_max_ns = d; t_n++;
        if ((t_n & 0x3FF) == 1)
            stepm("min2-doorbell-us", (long)(t_total_ns / (t_n ? t_n : 1) / 1000));
        if ((t_n & 0x3FF) == 1)
            stepm("min2-doorbell-max-us", (long)(t_max_ns / 1000));
        if ((t_n & 0x3F) == 1) {
            /* REV47 (PQ2, L2) HEARTBEAT: rides the DOORBELL counter (every
             * 64 doorbells), not the ack counter — a frozen-ack run still
             * shows doorbells climbing vs acks frozen. The C8 three-way
             * split: doorbells frozen + tracked-dst miss lines in
             * fsr4npu-SEAM.log = recognition bug; doorbells climbing +
             * acks frozen = parse ok / ack store lost; both frozen while
             * the seat's pre/post-doorbell lines continue = the call never
             * reaches the hook. */
            extern uint64_t seam_ack_count(SeamHeader__*);
            extern uint64_t seam_dbg_badmagic(void);
            stepm("m9-hb-doorbells", (long)((unsigned long long)t_n & 0xffffffff));
            stepm("m9-hb-acks", (long)(seam_ack_count(h) & 0xffffffff));
            stepm("m9-hb-lastseq", (long)(seq & 0xffffffff));
            stepm("m9-hb-badmagic", (long)(seam_dbg_badmagic() & 0xffffffff));
        }
    }
}

// ---- exports: no fallbacks, caller's handles forwarded ----
/* r58l swapchain hooks: capture the presented images + acquire index.
 * r58m FIX: down-resolve via down_gipa_for_device (the device-link gipa —
 * the canon resolver for device-level fns). Calling g_down_gipa(NULL, ...)
 * for device functions is the legacy escape shape that wedged two boots. */
static int32_t m_vkGetSwapchainImagesKHR(void* dev, unsigned long long sw, unsigned int* pCount, unsigned long long* pImages)
{
    int32_t (*down)(void*, unsigned long long, unsigned int*, unsigned long long*) =
        (int32_t (*)(void*, unsigned long long, unsigned int*, unsigned long long*))
        (down_gipa_for_device ? down_gipa_for_device : g_down_gipa)((void*)0, "vkGetSwapchainImagesKHR");
    if (!down) return -3;
    int32_t rc = down(dev, sw, pCount, pImages);
    if (rc == 0 && pCount && pImages && *pCount) {
        unsigned int n = *pCount > 8 ? 8 : *pCount, i;
        for (i = 0; i < n; i++) g_sw_images[i] = pImages[i];
        g_sw_img_count = n;
        stepm("m58l-swimg", (long)g_sw_images[0]);
        stepm("m58l-swcount", (long)n);
    }
    return rc;
}
static int32_t m_vkAcquireNextImageKHR(void* dev, unsigned long long sw, unsigned long long timeout, unsigned long long sem, unsigned long long fence, unsigned int* pIndex)
{
    int32_t (*down)(void*, unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned int*) =
        (int32_t (*)(void*, unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned int*))
        (down_gipa_for_device ? down_gipa_for_device : g_down_gipa)((void*)0, "vkAcquireNextImageKHR");
    if (!down) return -3;
    int32_t rc = down(dev, sw, timeout, sem, fence, pIndex);
    if (rc == 0 && pIndex) g_sw_idx = *pIndex;
    return rc;
}
/* REV58e: PROCESS GATE. The manifest now lives in the desktop loader's
 * global implicit dir (imagefs/usr/share/vulkan/implicit_layer.d) —
 * required since the container's runtime switched to the desktop loader
 * chain (VK_ICD_FILENAMES=wrapper_icd). An ungated GLOBAL layer there
 * injects into EVERY container process; r58d's 14:28 boot proved that
 * breaks GameNative's startup ("game failed to start" — the wine system
 * processes explorer/services/winedevice/Launcher all create instances).
 * Only the game host matches: cmdline is ".../wine RDR2.exe" (verified
 * live 14:37). Non-game processes get a byte-clean pass-through; if the
 * cmdline is unreadable we FAIL OPEN (act as game) — a broken probe is
 * recoverable, a silent unlayered game is not.
 *
 * ARMADA PORT (2026-10-09): the target is NMS — the game host cmdline is
 * ".../No Man's Sky/Binaries/NMS.exe" under Proton/FEX (plan §1). The
 * match token is "NMS.exe" (7 bytes, case as shipped); same fail-open
 * law. RDR2 is also kept as a match — this layer stays pair-compatible
 * with the RDR2 seat lineage for any future re-run of that campaign. */
static int m_is_game_process(void)
{
    static int cached = -1;
    if (cached >= 0) return cached;
    char buf[1024];
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) { cached = 1; return cached; }   /* fail open */
    int n = (int)read(fd, buf, sizeof buf - 1);
    close(fd);
    int hit = 1;                                 /* fail open */
    if (n > 0) {
        buf[n] = 0;
        hit = 0;
        for (int i = 0; i + 7 <= n; i++)
            if (!strncmp(buf + i, "NMS.exe", 7)) { hit = 1; break; }
        if (!hit)
            for (int i = 0; i + 4 <= n; i++)
                if (!strncmp(buf + i, "RDR2", 4)) { hit = 1; break; }
    }
    cached = hit;
    return cached;
}

/* rev3a: the NPU carrier (buffer-ops interposition) also arms for any
 * process that sets FSR4NPU_NPU=1 — the xbench standalone driver today,
 * the game itself at the rev3 in-game deploy (game-tweaks env) — or the
 * bench-manifest env FSR4NPU_BENCH=1 (the xbench CONTROL run: doorbells
 * parsed + acked with NO work bit, nothing recorded — the R59 carrier-
 * build shape). Probes (vulkaninfo, steam, wine system procs) carry no
 * such env and stay byte-clean pass-through, preserving the REV58e law. */
static int m_npu_proc(void)
{
    static int cached = -1;
    if (cached >= 0) return cached;
    const char* a = getenv("FSR4NPU_NPU");
    const char* b = getenv("FSR4NPU_BENCH");
    cached = (a && !strcmp(a, "1")) || (b && !strcmp(b, "1")) ? 1 : 0;
    return cached;
}
/* Spoof gate: comm-based, FAIL-CLOSED. The game-gate's cmdline check cannot
 * be used here — proton's launcher python carries "NMS.exe" in its argv (it
 * is the exe it launches), so a cmdline gate spoofs the launcher and its
 * utilities.py GPU check dies (ValueError: deviceType, live-caught
 * 2026-10-09). The wine game process reports comm "NMS.exe" (M0 log
 * evidence: 'comm NMS.exe'); launchers report python3/proton. */
static int m_spoof_target(void)
{
    {
        const char* f = getenv("FSR4NPU_SPOOF_FORCE");
        if (f && f[0] == '1') return 1;   /* debug lever: verify via any process */
    }
    static int cached = -1;
    if (cached >= 0) return cached;
    char comm[64];
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd < 0) { cached = 0; return cached; }   /* fail CLOSED */
    int n = (int)read(fd, comm, sizeof comm - 1);
    close(fd);
    if (n <= 0) { cached = 0; return cached; }
    comm[n] = 0;
    cached = (strncmp(comm, "NMS.exe", 7) == 0) ? 1 : 0;
    return cached;
}

/* ---- vendor spoof (OptiScaler EnableVulkanSpoofing port, nms.exe quirk) ----
 * Live-proven 2026-10-09: NMS's Streamline chain gates DLSS on the LIVE Vulkan
 * adapter identity — on Adreno (0x5143) sl.interposer maps but no nvngx* DLL
 * is ever loaded (no seat log across a full session). OptiScaler's nms.exe
 * quirk fixes this by spoofing vkGetPhysicalDeviceProperties{,2,2KHR}; this
 * layer sits BELOW winevulkan where the real queries flow. Values are
 * OptiScaler's defaults (Config.h:494-498): 0x10de / 0x2684 / RTX 4090;
 * driver-properties pNext patched per Vulkan_Spoofing.cpp:156-157. */
#define SPOOF_QCOM_VENDOR 0x5143u
#define SPOOF_NV_VENDOR   0x10deu
#define SPOOF_NV_DEVICE   0x2684u
/* VkPhysicalDeviceProperties layout (vulkan_core.h:3589 — header-verified
 * 2026-10-09 after the deviceType-smash bug): apiVersion@0 driverVersion@4
 * vendorID@8 deviceID@12 deviceType@16 deviceName@20[256]. deviceType is NEVER
 * written — the first implementation wrote the name at +16, feeding consumers
 * deviceType=0x4449564E ('NVID' LE, the proton python ValueError value) and a
 * name missing its first 4 chars ("IA GeForce RTX 4090").
 * Properties2 embeds it @16 → vendorID@24 deviceID@28 deviceName@36.
 * Driver fields live in BOTH VkPhysicalDeviceDriverProperties (sType
 * 1000196000) and VkPhysicalDeviceVulkan12Properties (sType 52) at the SAME
 * offsets: driverID u32@16, driverName[256]@20. VK_DRIVER_ID_NVIDIA_PROPRIETARY=4
 * (vulkan_core.h:6194-6198 — the enum is 1-based: MESA_RADV=3, NVIDIA_PROPRIETARY=4;
 * the first port wrote 3 = RADV, the never-explained vulkaninfo residual). */
#define SPOOF_STYPE_DRIVER_PROPS 1000196000u
#define SPOOF_STYPE_VK12_PROPS   52u
/* armed+fields resolved ONCE, shared by every spoof surface. The 2026-10-09
 * afternoon control run (FSR4NPU_SPOOF=0) SIGSEGV'd identically to the spoofed
 * run — pdp2_pnext (and the ext-appending wrapper) had NO kill-switch gate, so
 * every "control" since the offset-fix build still carried the driver-props
 * rewrite and the phantom NV extensions. The mask bisect that concluded
 * "vendorID alone crashes" is INVALID for the same reason. All surfaces gate
 * here now; FIELDS defaults to 15 (all). */
static int spoof_cfg(int* fields_out)
{
    static int armed = -1, fields = -1;
    static unsigned long long revert_ms = 0, t0_ms = 0;
    if (armed < 0) {
        const char* a = getenv("FSR4NPU_SPOOF");
        armed = (a && !strcmp(a, "1")) ? 1 : 0;   /* default OFF (the validated ship config); FSR4NPU_SPOOF=1 arms ALL */
        const char* k = getenv("FSR4NPU_SPOOF_FIELDS");
        /* bit0 vendor, bit1 device, bit2 name, bit3 driver-props, bit4 driverVersion,
         * bit5 deviceType (unbundled from bit0 for the 3x-cost bisect: DISCRETE on a
         * UMA SoC may switch the game to staging-upload paths — web-agent round 4) */
        fields = k ? atoi(k) : 63;
        const char* r = getenv("FSR4NPU_SPOOF_REVERT_MS");
        revert_ms = r ? strtoull(r, NULL, 0) : 0;   /* 0 = spoof forever (default) */
    }
    if (fields_out) *fields_out = fields;
    return armed;
}
/* 3x-law fix 2026-10-10: the game reads GPU identity ONCE during boot
 * (pdpRead census: 33 reads, all within the first ~9s, zero after) and
 * picks a ~2.3x-heavier per-frame render path when the vendor says NVIDIA.
 * SL's gate consumes the NVIDIA answer during its early getSystemCaps
 * window (~0.5-0.75s). REVERT_MS hands SL its NVIDIA, then serves the REAL
 * Adreno identity for all later identity reads — the game's renderer burst
 * (2.2s+) sees Qualcomm and takes the light path. Scoped to the PROPERTIES
 * surfaces ONLY: the extension sandwich (edep fakes + createDevice strip)
 * must stay spoofed forever — SL re-checks extensions at its chassis
 * device-create (~6s) and unloads all plugins if they vanish. */
static int spoof_identity_on(void)
{
    static unsigned long long revert_ms = 0, t0_ms = 0;
    static int inited = 0;
    if (!spoof_cfg(NULL)) return 0;
    if (!inited) {
        inited = 1;
        const char* r = getenv("FSR4NPU_SPOOF_REVERT_MS");
        revert_ms = r ? strtoull(r, NULL, 0) : 0;   /* 0 = identity spoof forever */
    }
    if (!revert_ms) return 1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long long now = (unsigned long long)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
    if (!t0_ms) t0_ms = now;   /* anchor at first identity consult */
    return (now - t0_ms) <= revert_ms;
}
static uint32_t spoof_vendor_value(void)
{
    static uint32_t sv_vendor = SPOOF_NV_VENDOR;
    static int done = 0;
    if (!done) {
        done = 1;
        const char* vv = getenv("FSR4NPU_SPOOF_VENDOR");   /* sweep: 0x1002 AMD / 0x8086 Intel */
        if (vv) sv_vendor = (uint32_t)strtoul(vv, NULL, 0);
    }
    return sv_vendor;
}
static uint32_t spoof_device_value(void)
{
    static uint32_t sv_device = SPOOF_NV_DEVICE;
    static int done = 0;
    if (!done) {
        done = 1;
        const char* dv = getenv("FSR4NPU_SPOOF_DEVICE");
        if (dv) sv_device = (uint32_t)strtoul(dv, NULL, 0);
    }
    return sv_device;
}
static void spoof_pdp(volatile char* props)
{
    int fields;
    if (!spoof_cfg(&fields)) return;
    uint32_t vendor;
    memcpy((void*)&vendor, (const void*)(props + 8), 4);
    if (vendor != SPOOF_QCOM_VENDOR) return;   /* llvmpipe/NVIDIA untouched */
    {   /* 3x-bisect 2026-10-10: when does the game (re)read GPU identity?
         * Timestamped census of every Adreno properties read, one line each,
         * capped. The tier-decision timing question rides on this. */
        static unsigned long long t0 = 0;
        static unsigned rn = 0;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        unsigned long long now = (unsigned long long)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
        if (!t0) t0 = now;
        if (rn < 200) {
            rn++;
            m_tr("pdpRead n=%u t=%llums spoofed=%d", rn, now - t0, spoof_identity_on());
        }
    }
    if (!spoof_identity_on()) return;   /* time-box expired: serve real identity */
    uint32_t v = spoof_vendor_value(), d = spoof_device_value();
    if (fields & 1) memcpy((void*)(props + 8), &v, 4);     /* vendorID */
    if (fields & 32) {   /* bit5 (unbundled): DISCRETE — a discrete type on UMA may
                          * switch the game to staging-upload paths (3x-cost bisect) */
        uint32_t dt = 2;
        memcpy((void*)(props + 16), &dt, 4);
    }
    if (fields & 2) memcpy((void*)(props + 12), &d, 4);
    if (fields & 16) {   /* driverVersion — NVIDIA packing (NOT Vulkan-standard
                          * minor<<12): major<<22 | minor<<14 | secondary<<6 | tertiary
                          * (web-agent round 4; the leftover Mesa 26.2.4 fired the
                          * "driver out of date" warning on an NVIDIA-branded card) */
        uint32_t dv = (580u << 22) | (65u << 14);
        memcpy((void*)(props + 4), &dv, 4);
    }
    if (fields & 4) {
        memset((void*)(props + 20), 0, 256);
        {
            const char* s = "NVIDIA GeForce RTX 4090";
            for (int i = 0; s[i]; i++) props[20 + i] = s[i];
        }
    }
    {
        static unsigned n = 0;
        if (n++ < 3) stepm("m9-spoof-pdp", (long)vendor);
    }
}
static void spoof_pdp2_pnext(volatile char* out2)
{
    int fields;
    if (!spoof_cfg(&fields) || !(fields & 8)) return;   /* bit3 = driver-props walk */
    if (!spoof_identity_on()) return;   /* identity time-box: real driver fields after revert */
    for (volatile char* h = *(volatile char**)(out2 + 8); h; h = *(volatile char**)(h + 8)) {
        uint32_t st;
        memcpy((void*)&st, (const void*)h, 4);
        if (st == SPOOF_STYPE_DRIVER_PROPS || st == SPOOF_STYPE_VK12_PROPS) {
            uint32_t id = 4;   /* VK_DRIVER_ID_NVIDIA_PROPRIETARY (1-based enum!) */
            memcpy((void*)(h + 16), &id, 4);
            memset((void*)(h + 20), 0, 256);           /* driverName[256]@20 */
            {
                const char* s = "NVIDIA";
                for (int i = 0; s[i]; i++) h[20 + i] = s[i];
            }
            memset((void*)(h + 276), 0, 256);          /* driverInfo[256]@276 */
            {
                const char* s2 = "580.65";             /* web-agent round 4 */
                for (int i = 0; s2[i]; i++) h[276 + i] = s2[i];
            }
        }
    }
}
static void (*g_down_pdp)(void*, void*);
static void (*g_down_pdp2)(void*, void*);
/* ---- NVX/NV extension-presence spoof ----
 * Crash forensics 2026-10-09: with vendorID=NVIDIA (alone — field-bisect
 * proven), the game SIGSEGVs at a near-NULL member deref (si_addr 0x3a8) in
 * translated game code BEFORE any NvAPI/NGX DLL loads. NVIDIA's driver-level
 * NGX surface on Vulkan = VK_NVX_binary_import / VK_NVX_image_view_handle /
 * VK_NV_low_latency2 — none exist on Turnip. The interposer's NVIDIA branch
 * presumes them; the null it derefs is that missing surface. Fake PRESENCE
 * (names only, appended post-down via the count-first protocol so the app's
 * buffer is sized for them) + no-op stubs for the marker functions. */
static const char* const SPOOF_NV_EXTS[] = {
    "VK_NVX_binary_import", "VK_NVX_image_view_handle",
    "VK_NV_low_latency2", "VK_NV_low_latency",
    /* 2026-10-09 launch-4 verdict: SL requires the EXT alias name; Turnip
     * exposes only the promoted VK_KHR_buffer_device_address. Same feature —
     * fake the name for the check, strip it at createDevice. */
    "VK_EXT_buffer_device_address",
    /* launch-6/7 verdict: winevulkan's PE vkCreateDevice export (SL calls it
     * via GetProcAddress, wrapper.cpp:932) gates EVERY requested extension
     * against the pd snapshot built from this enumeration — AMD_buffer_marker
     * (SL chassis ext, NV+AMD drivers have it, Turnip doesn't) died there
     * before the call ever reached the unix chain. Fake presence + strip. */
    "VK_AMD_buffer_marker",
};
#define SPOOF_NV_EXT_N (sizeof(SPOOF_NV_EXTS) / sizeof(SPOOF_NV_EXTS[0]))
static uint32_t (*g_down_edep)(void*, const char*, uint32_t*, void*);
static VkResult m_vkEnumerateDeviceExtensionProperties(void* pd, const char* layerName, uint32_t* pCount, void* props)
{
    if (!spoof_cfg(NULL))   /* kill-switch: no phantom NV extensions either */
        return g_down_edep ? g_down_edep(pd, layerName, pCount, props) : (uint32_t)(0x80000000u | 7u);
    if (!g_down_edep && g_down_gipa && g_chain_inst)
        g_down_edep = (uint32_t (*)(void*, const char*, uint32_t*, void*))g_down_gipa(g_chain_inst, "vkEnumerateDeviceExtensionProperties");
    if (!g_down_edep || !pCount) return 0x80000000 | 7;   /* VK_ERROR_EXTENSION_NOT_PRESENT-ish guard */
    if (layerName) {   /* layer-property queries pass through untouched */
        return g_down_edep(pd, layerName, pCount, props);
    }
    if (!props) {      /* count pass: inflate by our fake set */
        uint32_t rc = g_down_edep(pd, layerName, pCount, props);
        uint32_t raw = *pCount;
        *pCount += (uint32_t)SPOOF_NV_EXT_N;
        {
            static unsigned nc = 0;
            if (nc++ < 12) m_tr("edepC raw=%lu inflated=%lu", (unsigned long)raw, (unsigned long)*pCount);
        }
        return rc;
    }
    /* array pass: IDEMPOTENT append, EMPTY-SLOT-FIRST clobber, protected reals.
     * Empirics 2026-10-09 (launches 1-3): the caller's count does not always
     * traverse us and the down-call can already report an inflated count
     * (down=190 with empty tail slots — winevulkan PE loader re-enumerates,
     * filters through its per-device snapshot and appends wine-only win32
     * extensions; see proton-wine dlls/winevulkan/loader.c:546). Rules:
     *  - append missing fakes into slack when there is room;
     *  - else write into EMPTY slots (name[0]==0 — observed in the tails);
     *  - else overwrite tail slots whose names are NOT in the protected set
     *    (SL-required real extensions + wine win32 bridge extensions);
     *  - log a wine-ext loop-detector: VK_KHR_external_memory_win32 present
     *    in the DOWN output proves our down pointer resolves into
     *    winevulkan's filtered PE output rather than the raw ICD. */
    uint32_t n = *pCount;
    uint32_t rc = g_down_edep(pd, layerName, pCount, props);
    uint32_t filled = *pCount;
    if (filled > n) filled = n;   /* down over-filled? clamp defensively */
    volatile char* base = (volatile char*)props;
    static const char* const PROTECTED[] = {
        "VK_EXT_buffer_device_address", "VK_KHR_push_descriptor",
        "VK_KHR_external_memory_win32", "VK_KHR_external_fence_win32",
        "VK_KHR_external_semaphore_win32", "VK_KHR_win32_keyed_mutex",
        "VK_KHR_timeline_semaphore", "VK_EXT_memory_priority",
    };
    unsigned present = 0, appended = 0, empties = 0, clob = 0, loopwine = 0, bda = 0;
    char clob_names[4][64];
    for (uint32_t i = 0; i < (uint32_t)SPOOF_NV_EXT_N; i++) {
        const char* s = SPOOF_NV_EXTS[i];
        int have = 0;
        for (uint32_t j = 0; j < filled && !have; j++) {
            const volatile char* nm = base + (uint64_t)j * 260;
            have = 1;
            for (int k = 0; s[k] || nm[k]; k++) {
                if (s[k] != nm[k]) { have = 0; break; }
            }
        }
        if (have) { present++; continue; }
        /* pick a target slot: slack, then empty, then unprotected tail */
        uint32_t slot = 0xffffffffu;
        if (filled < n) {
            slot = filled++;
            appended++;
        } else {
            for (uint32_t j = n; j > 0; j--) {
                const volatile char* nm = base + (uint64_t)(j - 1) * 260;
                if (!nm[0]) { slot = j - 1; empties++; break; }
            }
            if (slot == 0xffffffffu) {
                for (uint32_t j = n; j > 0; j--) {
                    const volatile char* nm = base + (uint64_t)(j - 1) * 260;
                    int prot = 0;
                    for (unsigned p = 0; p < sizeof(PROTECTED) / sizeof(PROTECTED[0]); p++) {
                        const char* ps = PROTECTED[p];
                        prot = 1;
                        for (int k = 0; ps[k] || nm[k]; k++)
                            if (ps[k] != nm[k]) { prot = 0; break; }
                        if (prot) break;
                    }
                    if (!prot) {
                        slot = j - 1;
                        if (clob < 4) {
                            unsigned k;
                            for (k = 0; k + 1 < sizeof(clob_names[0]) && nm[k]; k++)
                                clob_names[clob][k] = nm[k];
                            clob_names[clob][k] = 0;
                        }
                        clob++;
                        break;
                    }
                }
            }
            if (slot == 0xffffffffu) break;   /* everything protected: give up on this fake */
        }
        volatile char* dst = base + (uint64_t)slot * 260;   /* VkExtensionProperties: name[256] + specVersion@256 */
        memset((void*)dst, 0, 260);
        for (int j = 0; s[j]; j++) dst[j] = s[j];
        uint32_t sv = 1;
        memcpy((void*)(dst + 256), &sv, 4);
    }
    if (filled > n) filled = n;
    *pCount = filled;
    {   /* forensics: wine-bridge marker + SL-required real ext in down output */
        for (uint32_t j = 0; j < filled; j++) {
            const volatile char* nm = base + (uint64_t)j * 260;
            if (!strncmp((const char*)nm, "VK_KHR_external_memory_win32", 29)) loopwine = 1;
            if (!strncmp((const char*)nm, "VK_EXT_buffer_device_address", 28)) bda = 1;
        }
        static unsigned ne = 0;
        if (ne++ < 12) m_tr("edep3 pd=%p cap=%lu down=%lu present=%lu app=%lu empty=%lu clob=%lu[%s %s] wine=%lu bda=%lu rc=%ld",
                             (void*)pd, (unsigned long)n, (unsigned long)filled,
                             (unsigned long)present, (unsigned long)appended,
                             (unsigned long)empties, (unsigned long)clob,
                             clob > 0 ? clob_names[0] : "-",
                             clob > 1 ? clob_names[1] : "-",
                             (unsigned long)loopwine, (unsigned long)bda, (long)rc);
    }
    return rc;
}
static VkResult m_nv_noop(void) { return 0; }   /* VK_SUCCESS; void fns tolerate a bogus return */
static int spoof_is_nv_fn(const char* n)
{
    static const char* const fns[] = {
        "vkSetLatencySleepModeNV", "vkLatencySleepNV", "vkSetLatencyMarkerNV",
        "vkGetLatencyTimingsNV", "vkQueueNotifyOutOfBandNV",
        "vkImportObjectNVX", "vkGetObjectHandleNVX",
        "vkCmdSetCheckpointNV", "vkGetQueueCheckpointNV",
    };
    for (unsigned i = 0; i < sizeof(fns) / sizeof(fns[0]); i++)
        if (!strcmp(n, fns[i])) return 1;
    return 0;
}
static void m_vkGetPhysicalDeviceProperties(void* pd, void* out)
{
    if (!g_down_pdp && g_down_gipa && g_chain_inst)
        g_down_pdp = (void (*)(void*, void*))g_down_gipa(g_chain_inst, "vkGetPhysicalDeviceProperties");
    if (g_down_pdp) g_down_pdp(pd, out);
    if (out) spoof_pdp((volatile char*)out);
    if (m_vktrace() && out) {
        volatile char* o = (volatile char*)out;
        uint32_t av, dvv, vid, did, dt;
        memcpy(&av, (const void*)(o + 0), 4); memcpy(&dvv, (const void*)(o + 4), 4);
        memcpy(&vid, (const void*)(o + 8), 4); memcpy(&did, (const void*)(o + 12), 4);
        memcpy(&dt, (const void*)(o + 16), 4);
        m_tr("pdp api=%x drv=%x vendor=%x device=%x type=%u name=%.40s",
             av, dvv, vid, did, dt, (const char*)(o + 20));
    }
}
static void m_vkGetPhysicalDeviceProperties2(void* pd, void* out)
{
    if (!g_down_pdp2 && g_down_gipa && g_chain_inst)
        g_down_pdp2 = (void (*)(void*, void*))g_down_gipa(g_chain_inst, "vkGetPhysicalDeviceProperties2");
    if (g_down_pdp2) g_down_pdp2(pd, out);
    if (out) { spoof_pdp((volatile char*)out + 16); spoof_pdp2_pnext((volatile char*)out); }
    if (m_vktrace() && out) {
        volatile char* h = *(volatile char**)((volatile char*)out + 8);
        unsigned ns = 0;
        for (int i = 0; h && i < 24; i++) {
            uint32_t st; memcpy(&st, (const void*)h, 4);
            m_tr("pdp2.pnext st=%u", st);
            ns++;
            h = *(volatile char**)(h + 8);
        }
        m_tr("pdp2 pnext-chain-len=%u", ns);
    }
}

/* ---- vk interrogation tracer (web-agent round 2 instrument) ----
 * First-diverging-call diff, spoof-on vs spoof-off: queue families, memory
 * types/heaps, features2 pNext chain (an NV-branch query would surface as an
 * sType only present in the spoofed run), createDevice queue/ext picks, and
 * allocateMemory type indices. Pass-through wrappers — safe in any process;
 * every line carries the pid. Gated FSR4NPU_VKTRACE=1. */
static void (*g_down_qfp)(void*, uint32_t*, void*);
static void (*g_down_qfp2)(void*, uint32_t*, void*);
static void (*g_down_mp)(void*, void*);
static void (*g_down_mp2)(void*, void*);
static void (*g_down_f2)(void*, void*);
static void m_vkGetPhysicalDeviceQueueFamilyProperties(void* pd, uint32_t* pCount, void* props)
{
    if (!g_down_qfp && g_down_gipa && g_chain_inst)
        g_down_qfp = (void (*)(void*, uint32_t*, void*))g_down_gipa(g_chain_inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (g_down_qfp) g_down_qfp(pd, pCount, props);
    if (m_vktrace() && pCount) {
        m_tr("qfp n=%u (props=%d)", *pCount, props ? 1 : 0);
        if (props)
            for (uint32_t i = 0; i < *pCount && i < 8; i++) {
                volatile char* q = (volatile char*)props + (uint64_t)i * 24;   /* flags@0 count@4 tsb@8 gran@12 */
                uint32_t fl, cc; memcpy(&fl, (const void*)q, 4); memcpy(&cc, (const void*)(q + 4), 4);
                m_tr("qfp[%u] flags=%x count=%u", i, fl, cc);
            }
    }
}
static void m_vkGetPhysicalDeviceQueueFamilyProperties2(void* pd, uint32_t* pCount, void* props)
{
    if (!g_down_qfp2 && g_down_gipa && g_chain_inst)
        g_down_qfp2 = (void (*)(void*, uint32_t*, void*))g_down_gipa(g_chain_inst, "vkGetPhysicalDeviceQueueFamilyProperties2");
    if (g_down_qfp2) g_down_qfp2(pd, pCount, props);
    if (m_vktrace() && pCount && props) {
        m_tr("qfp2 n=%u", *pCount);
        for (uint32_t i = 0; i < *pCount && i < 8; i++) {
            volatile char* q = (volatile char*)props + (uint64_t)i * 40;      /* sType/pNext + props@16 */
            uint32_t fl, cc; memcpy(&fl, (const void*)(q + 16), 4); memcpy(&cc, (const void*)(q + 16 + 4), 4);
            m_tr("qfp2[%u] flags=%x count=%u", i, fl, cc);
        }
    }
}
static void m_trace_memprops(volatile char* o)
{
    /* VkPhysicalDeviceMemoryProperties: typeCount@0 types[32]@4 (8B each:
     * flags@0 heap@4) heapCount@260 heaps[16]@264 (16B: size@0 flags@8) */
    uint32_t tc, hc;
    memcpy(&tc, (const void*)o, 4); memcpy(&hc, (const void*)(o + 260), 4);
    m_tr("memprops types=%u heaps=%u", tc, hc);
    for (uint32_t i = 0; i < tc && i < 8; i++) {
        uint32_t pf, hi;
        memcpy(&pf, (const void*)(o + 4 + (uint64_t)i * 8), 4);
        memcpy(&hi, (const void*)(o + 4 + (uint64_t)i * 8 + 4), 4);
        m_tr("memtype[%u] flags=%x heap=%u", i, pf, hi);
    }
    for (uint32_t i = 0; i < hc && i < 4; i++) {
        uint64_t sz; memcpy(&sz, (const void*)(o + 264 + (uint64_t)i * 16), 8);
        m_tr("heap[%u] size=%llu", i, (unsigned long long)sz);
    }
}
static void m_vkGetPhysicalDeviceMemoryProperties(void* pd, void* out)
{
    if (!g_down_mp && g_down_gipa && g_chain_inst)
        g_down_mp = (void (*)(void*, void*))g_down_gipa(g_chain_inst, "vkGetPhysicalDeviceMemoryProperties");
    if (g_down_mp) g_down_mp(pd, out);
    if (m_vktrace() && out) m_trace_memprops((volatile char*)out);
}
static void m_vkGetPhysicalDeviceMemoryProperties2(void* pd, void* out)
{
    if (!g_down_mp2 && g_down_gipa && g_chain_inst)
        g_down_mp2 = (void (*)(void*, void*))g_down_gipa(g_chain_inst, "vkGetPhysicalDeviceMemoryProperties2");
    if (g_down_mp2) g_down_mp2(pd, out);
    if (m_vktrace() && out) m_trace_memprops((volatile char*)out + 16);   /* sType/pNext header */
}
static void m_vkGetPhysicalDeviceFeatures2(void* pd, void* out)
{
    if (!g_down_f2 && g_down_gipa && g_chain_inst)
        g_down_f2 = (void (*)(void*, void*))g_down_gipa(g_chain_inst, "vkGetPhysicalDeviceFeatures2");
    if (g_down_f2) g_down_f2(pd, out);
    if (m_vktrace() && out) {
        volatile char* h = *(volatile char**)((volatile char*)out + 8);
        unsigned ns = 0;
        for (int i = 0; h && i < 24; i++) {
            uint32_t st; memcpy(&st, (const void*)h, 4);
            m_tr("feat2.pnext st=%u", st);
            ns++;
            h = *(volatile char**)(h + 8);
        }
        m_tr("feat2 pnext-chain-len=%u", ns);
    }
}
static int32_t (*g_down_allocmem)(void*, const void*, const void*, void**);
static int32_t m_vkAllocateMemoryTrace(void* dev, const void* pInfo, const void* alloc, void** out)
{
    if (!g_down_allocmem && g_down_gdpa)
        g_down_allocmem = (int32_t (*)(void*, const void*, const void*, void**))g_down_gdpa(dev, "vkAllocateMemory");
    if (!g_down_allocmem) return 0x80000000 | 5;   /* OUT_OF_HOST_MEMORY-ish guard */
    int32_t rc = g_down_allocmem(dev, pInfo, alloc, out);
    if (m_vktrace() && pInfo) {   /* VkMemoryAllocateInfo: size@16 typeIndex@24 */
        uint64_t sz; uint32_t ti;
        memcpy(&sz, (const void*)((const char*)pInfo + 16), 8);
        memcpy(&ti, (const void*)((const char*)pInfo + 24), 4);
        static unsigned na = 0;
        if (na++ < 48 || (na & 0xff) == 0) m_tr("allocmem sz=%llu type=%u rc=%d", (unsigned long long)sz, ti, (int)rc);
    }
    return rc;
}

__attribute__((visibility("default"))) void* vkGetInstanceProcAddr(VkInstance inst, const char* pName){
    if (!pName) return NULL;
    if (!strcmp(pName, "vkCreateInstance")) return (void*)&m_vkCreateInstance;
    if (!strcmp(pName, "vkCreateDevice"))   return (void*)&m_vkCreateDevice;
    /* vendor spoof: OptiScaler nms.exe EnableVulkanSpoofing surface. KHR alias
     * resolves to the same 1.1 entry point on every loader in the chain.
     * GAME-PROCESS ONLY — see the PDPA note (proton launcher crash law).
     * armed-gated: FSR4NPU_SPOOF=0 must leave even stub registration off. */
    if (m_spoof_target() && spoof_cfg(NULL)) {
        if (!strcmp(pName, "vkGetPhysicalDeviceProperties"))    return (void*)&m_vkGetPhysicalDeviceProperties;
        if (!strcmp(pName, "vkGetPhysicalDeviceProperties2"))   return (void*)&m_vkGetPhysicalDeviceProperties2;
        if (!strcmp(pName, "vkGetPhysicalDeviceProperties2KHR")) return (void*)&m_vkGetPhysicalDeviceProperties2;
        if (!strcmp(pName, "vkEnumerateDeviceExtensionProperties")) return (void*)&m_vkEnumerateDeviceExtensionProperties;
        if (spoof_is_nv_fn(pName)) {
            static unsigned ns = 0;
            if (ns++ < 8) stepm("m9-spoof-nvfn", 0);
            return (void*)&m_nv_noop;
        }
    }
    /* interrogation tracer: registered whenever FSR4NPU_VKTRACE=1 (pass-through
     * wrappers, no struct writes — safe for the proton launcher too) */
    if (m_vktrace()) {
        if (!strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties"))    return (void*)&m_vkGetPhysicalDeviceQueueFamilyProperties;
        if (!strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties2"))   return (void*)&m_vkGetPhysicalDeviceQueueFamilyProperties2;
        if (!strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties2KHR")) return (void*)&m_vkGetPhysicalDeviceQueueFamilyProperties2;
        if (!strcmp(pName, "vkGetPhysicalDeviceMemoryProperties"))         return (void*)&m_vkGetPhysicalDeviceMemoryProperties;
        if (!strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2"))        return (void*)&m_vkGetPhysicalDeviceMemoryProperties2;
        if (!strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2KHR"))     return (void*)&m_vkGetPhysicalDeviceMemoryProperties2;
        if (!strcmp(pName, "vkGetPhysicalDeviceFeatures2"))                return (void*)&m_vkGetPhysicalDeviceFeatures2;
        if (!strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR"))             return (void*)&m_vkGetPhysicalDeviceFeatures2;
    }
    /* r58n: swapchain hook registration REMOVED — wedged 3 boots pre-RDR2
     * (swapchain fns are loader/WSI-implemented; layer-wrapping them from
     * this chain deadlocks). The hook bodies stay but are unreachable. */
    if (g_down_gipa) {
        void* r = g_down_gipa(inst, pName);
        /* crash forensics (2026-10-09): the game SIGSEGVs natively with the
         * spoof on — log every vk* name resolving NULL in the game proc; an
         * unchecked NV-only proc call is a candidate kill mechanism. */
        if (!r && m_spoof_target() && pName[0] == 'v' && pName[1] == 'k') {
            FILE* nf = fopen(SEAM_LOG_DIR "/fsr4npu-SEAM.log", "a");
            if (nf) { fprintf(nf, "m9-gipa-null %s (pid %d)\n", pName, (int)getpid()); fclose(nf); }
        }
        return r;
    }
    return NULL;   // CALLER's instance
    return NULL;
}

__attribute__((visibility("default"))) void* vkGetDeviceProcAddr(void* dev, const char* pName)
{
    if (!pName) return NULL;
    if (!(m_is_game_process() || m_npu_proc())) {  /* probes: not ours */
        if (g_down_gdpa) return g_down_gdpa(dev, pName);
        return NULL;
    }
    if (!strcmp(pName, "vkCreateBuffer"))      return (void*)&m_vkCreateBuffer;
    if (!strcmp(pName, "vkCreateImage"))       return (void*)&m_vkCreateImage;   /* r59r census */
    if (!strcmp(pName, "vkBindBufferMemory"))  return (void*)&m_vkBindBufferMemory;
    if (!strcmp(pName, "vkMapMemory"))         return (void*)&m_vkMapMemory;
    if (!strcmp(pName, "vkCmdUpdateBuffer"))   return (void*)&m_vkCmdUpdateBuffer;
    if (m_vktrace() && !strcmp(pName, "vkAllocateMemory")) return (void*)&m_vkAllocateMemoryTrace;
    /* r58n: swapchain names NOT hooked at gdpa either (see gipa note). */
    if (g_down_gdpa) return g_down_gdpa(dev, pName);     // CALLER's device
    return NULL;
}

__attribute__((visibility("default"))) void* vkGetPhysicalDeviceProcAddr(VkInstance inst, const char* pName)
{
    if (!pName) return NULL;
    /* vendor spoof on BOTH dispatch paths — this loader resolves
     * vkGetPhysicalDeviceProperties* via PDPA, not GIPA (live-found 2026-10-09:
     * GIPA-only registration left vulkaninfo seeing the real Adreno).
     * GAME-PROCESS ONLY: proton-cachyos's launcher python enumerates GPUs
     * through the same env (utilities.py get_vulkan_gpus) and DIED on the
     * spoofed struct (ValueError: deviceType garbage, live-caught 2026-10-09)
     * — non-game processes must see the real adapter untouched. */
    if (m_spoof_target() && spoof_cfg(NULL)) {
        if (!strcmp(pName, "vkGetPhysicalDeviceProperties"))    return (void*)&m_vkGetPhysicalDeviceProperties;
        if (!strcmp(pName, "vkGetPhysicalDeviceProperties2"))   return (void*)&m_vkGetPhysicalDeviceProperties2;
        if (!strcmp(pName, "vkGetPhysicalDeviceProperties2KHR")) return (void*)&m_vkGetPhysicalDeviceProperties2;
        if (!strcmp(pName, "vkEnumerateDeviceExtensionProperties")) return (void*)&m_vkEnumerateDeviceExtensionProperties;
        if (spoof_is_nv_fn(pName)) return (void*)&m_nv_noop;
    }
    if (m_vktrace()) {
        if (!strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties"))    return (void*)&m_vkGetPhysicalDeviceQueueFamilyProperties;
        if (!strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties2"))   return (void*)&m_vkGetPhysicalDeviceQueueFamilyProperties2;
        if (!strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties2KHR")) return (void*)&m_vkGetPhysicalDeviceQueueFamilyProperties2;
        if (!strcmp(pName, "vkGetPhysicalDeviceMemoryProperties"))         return (void*)&m_vkGetPhysicalDeviceMemoryProperties;
        if (!strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2"))        return (void*)&m_vkGetPhysicalDeviceMemoryProperties2;
        if (!strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2KHR"))     return (void*)&m_vkGetPhysicalDeviceMemoryProperties2;
        if (!strcmp(pName, "vkGetPhysicalDeviceFeatures2"))                return (void*)&m_vkGetPhysicalDeviceFeatures2;
        if (!strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR"))             return (void*)&m_vkGetPhysicalDeviceFeatures2;
    }
    if (g_down_gipa) return g_down_gipa(inst, pName);
    return NULL;
}

typedef struct { uint32_t sType; void* pNext; uint32_t loaderLayerInterfaceVersion;
                 void* pfnGetInstanceProcAddr; void* pfnGetDeviceProcAddr;
                 void* pfnGetPhysicalDeviceProcAddr; } VkNegotiateLayerInterface_;
__attribute__((visibility("default"))) VkResult vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface_* ni)
{
    if (!ni) return 0x80000000 | 7;
    if (ni->sType == 1 && ni->loaderLayerInterfaceVersion > 1)
        ni->loaderLayerInterfaceVersion = 1;
    ni->pfnGetInstanceProcAddr = (void*)&vkGetInstanceProcAddr;
    ni->pfnGetDeviceProcAddr = (void*)&vkGetDeviceProcAddr;
    ni->pfnGetPhysicalDeviceProcAddr = (void*)&vkGetPhysicalDeviceProcAddr;
    return 0;
}
