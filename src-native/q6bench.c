/* q6bench.c — standalone GPU-free benchmark of the NPU graph: load the
 * production context binary and execute the graph in a timed loop for
 * N seconds. Used by the plugin's bench action when no game is running;
 * no Vulkan involved.
 *
 * Initialization sequence: libcdsprpc -> unsigned-PD + skel selftest ->
 * libQnnSystem (binaryInfo) -> libQnnHtp + V73 stub pre-load ->
 * backend/device -> deviceGetInfrastructure + a DCVS_V3 plus
 * RPC_CONTROL_LATENCY power-config vote at the requested corner ->
 * contextCreateFromBinary -> graphRetrieve -> rpcmem IO registered as
 * MEMHANDLE -> 3 warm executes -> timed loop.
 *
 * Arguments: qnn_dir holds libQnnSystem.so, libQnnHtp.so,
 * libQnnHtpV73{Stub,Skel}.so, libcdsprpc.so, libatomic.so.1 and the context
 * bins; bin_file is one of those context bins; seconds is the timed-loop
 * length; corner_hex is an optional hex DCVS voltage corner (0x20..0xA0).
 * The default corner comes from the file
 * /var/home/armada/fsr4npu-logs/fsr4npu-VCORNER if present, else 0xA0.
 * Prints one JSON line on stdout.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>   /* QNN headers use bool */
#include <assert.h>     /* QNN headers use static_assert */
#include <time.h>

#include "QnnInterface.h"
#include "System/QnnSystemInterface.h"
#include "QnnMem.h"
#include "HTP/QnnHtpDevice.h"
#include "HTP/QnnHtpPerfInfrastructure.h"

typedef QNN_INTERFACE_VER_TYPE        iface_t;
typedef QNN_SYSTEM_INTERFACE_VER_TYPE sysiface_t;
typedef Qnn_ErrorHandle_t (*getproviders_fn)(const QnnInterface_t***, uint32_t*);
typedef Qnn_ErrorHandle_t (*sysgetproviders_fn)(const QnnSystemInterface_t***, uint32_t*);
typedef void* (*rpcmem_alloc_fn)(int, uint32_t, int);
typedef int   (*rpcmem_tofd_fn)(void*);
typedef int   (*remote_session_control_fn)(uint32_t, void*, uint32_t);
typedef int   (*remote_handle64_open_fn)(const char*, uint64_t*);

#define KNOB_DIR "/var/home/armada/fsr4npu-logs"

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void fail(const char* step, long v)
{
    printf("{\"ok\":0,\"step\":\"%s\",\"err\":%ld}\n", step, v);
    fflush(stdout);
    exit(1);
}

/* unhandled dtypes return 0, which makes tbytes() 0 and surfaces downstream
 * as a failed alloc/register step; the production binary is int8 */
static size_t dtype_size(Qnn_DataType_t dt)
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
static const char* tname(const Qnn_Tensor_t* t) { return t->version == QNN_TENSOR_VERSION_1 ? t->v1.name : t->v2.name; }
static uint32_t trank(const Qnn_Tensor_t* t) { return t->version == QNN_TENSOR_VERSION_1 ? t->v1.rank : t->v2.rank; }
static const uint32_t* tdims(const Qnn_Tensor_t* t) { return t->version == QNN_TENSOR_VERSION_1 ? t->v1.dimensions : t->v2.dimensions; }
static Qnn_DataType_t tdtype(const Qnn_Tensor_t* t) { return t->version == QNN_TENSOR_VERSION_1 ? t->v1.dataType : t->v2.dataType; }
static void tset_name_dims(Qnn_Tensor_t* t, char* name, uint32_t* dims)
{
    if (t->version == QNN_TENSOR_VERSION_1) { t->v1.name = name; t->v1.dimensions = dims; }
    else                                    { t->v2.name = name; t->v2.dimensions = dims; }
}
static void tset_mem_handle(Qnn_Tensor_t* t, Qnn_MemHandle_t h)
{
    if (t->version == QNN_TENSOR_VERSION_1) { t->v1.memType = QNN_TENSORMEMTYPE_MEMHANDLE; t->v1.memHandle = h; }
    else                                    { t->v2.memType = QNN_TENSORMEMTYPE_MEMHANDLE; t->v2.memHandle = h; }
}
static size_t tbytes(const Qnn_Tensor_t* t)
{
    size_t n = dtype_size(tdtype(t));
    for (uint32_t i = 0; i < trank(t); i++) n *= tdims(t)[i];
    return n;
}

/* deep-copy tensor names/dimensions: binaryInfo storage is freed with the
 * system context, so the metadata must own its copies */
static Qnn_Tensor_t* copy_tensors(const Qnn_Tensor_t* src, uint32_t n)
{
    /* n==0 guard: calloc(0, ...) may return NULL */
    Qnn_Tensor_t* dst = (Qnn_Tensor_t*)calloc(n ? n : 1, sizeof(Qnn_Tensor_t));
    for (uint32_t i = 0; i < n; i++) {
        const char* name = tname(&src[i]);
        uint32_t rank = trank(&src[i]);
        dst[i] = src[i];
        char* nc = (char*)malloc(strlen(name) + 1);
        uint32_t* dm = (uint32_t*)malloc(rank * sizeof(uint32_t));
        if (!nc || !dm) fail("tensor-copy", (long)i);   /* same clean fail as the calloc above */
        strcpy(nc, name);
        memcpy(dm, tdims(&src[i]), rank * sizeof(uint32_t));
        tset_name_dims(&dst[i], nc, dm);
    }
    return dst;
}

/* alloc ION buffers for one tensor set via rpcmem and register them on the
 * context as MEMHANDLE; fill pre-writes a mid-range int8 pattern (inputs) */
static void register_io(Qnn_Tensor_t* info, uint32_t n, const char* pfx,
                        iface_t ifc, Qnn_ContextHandle_t ctx,
                        rpcmem_alloc_fn alloc_fn, rpcmem_tofd_fn tofd_fn, int fill)
{
    for (uint32_t i = 0; i < n; i++) {
        char step[16];
        size_t nb = tbytes(&info[i]);
        void* p = alloc_fn(25 /*RPCMEM_HEAP_ID_SYSTEM*/, 1 /*cached*/, (int)nb);
        snprintf(step, sizeof step, "%s-alloc", pfx);
        if (!p) fail(step, (long)i);
        int fd = tofd_fn(p);
        snprintf(step, sizeof step, "%s-tofd", pfx);
        if (fd < 0) fail(step, (long)i);
        Qnn_MemDescriptor_t desc = QNN_MEM_DESCRIPTOR_INIT;
        desc.memShape.numDim = trank(&info[i]);
        desc.memShape.dimSize = (uint32_t*)tdims(&info[i]);
        desc.memShape.shapeConfig = NULL;
        desc.dataType = tdtype(&info[i]);
        desc.memType = QNN_MEM_TYPE_ION;
        desc.ionInfo.fd = fd;
        Qnn_MemHandle_t h = NULL;
        snprintf(step, sizeof step, "%s-register", pfx);
        if (ifc.memRegister(ctx, &desc, 1, &h) != 0 || !h) fail(step, (long)i);
        tset_mem_handle(&info[i], h);
        if (fill) memset(p, 0x40, nb);
    }
}

static uint32_t read_vcorner_arg(const char* arg)
{
    if (arg && *arg) {
        unsigned long v = strtoul(arg, NULL, 0);
        if (v >= 0x20 && v <= 0xA0) return (uint32_t)v;
    }
    uint32_t vc = 0xA0;
    FILE* f = fopen(KNOB_DIR "/fsr4npu-VCORNER", "r");
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

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <qnn_dir> <bin_file> <seconds> [corner_hex]\n", argv[0]);
        fail("args", argc);
    }
    const char* dir = argv[1];
    const char* bin = argv[2];
    int seconds = atoi(argv[3]);
    if (seconds < 1) seconds = 1;
    if (seconds > 60) seconds = 60;
    uint32_t vc = read_vcorner_arg(argc > 4 ? argv[4] : NULL);
    char path[600];
    void* rpc_lib;
    void* sys_lib;
    void* be_lib;
    rpcmem_alloc_fn rpc_alloc;
    rpcmem_tofd_fn rpc_tofd;
    iface_t iface;
    Qnn_BackendHandle_t backend;
    Qnn_DeviceHandle_t device;
    Qnn_ContextHandle_t context;
    Qnn_GraphHandle_t graph;

    /* DSP-side loader path: ';' separator, normalize any ':' away */
    {
        const char* cur = getenv("ADSP_LIBRARY_PATH");
        char nv[2048];
        size_t o = (size_t)snprintf(nv, sizeof nv, "%s;", dir);
        if (cur && *cur) {
            for (const char* p = cur; *p && o + 1 < sizeof nv; p++)
                nv[o++] = (*p == ':') ? ';' : *p;
            /* reachable when the snprintf above truncated: it returns the
             * would-be length, so o can already exceed the buffer and the
             * append loop never ran */
            if (o >= sizeof nv) o = sizeof nv - 1;
            nv[o] = 0;
        }
        (void)setenv("ADSP_LIBRARY_PATH", nv, 1);
        (void)setenv("LD_LIBRARY_PATH", dir, 1);
    }

    snprintf(path, sizeof path, "%s/libcdsprpc.so", dir);
    rpc_lib = dlopen(path, RTLD_NOW);
    if (!rpc_lib) fail("rpc-lib", 0);
    rpc_alloc = (rpcmem_alloc_fn)dlsym(rpc_lib, "rpcmem_alloc");
    rpc_tofd  = (rpcmem_tofd_fn)dlsym(rpc_lib, "rpcmem_to_fd");
    if (!rpc_alloc || !rpc_tofd) fail("rpc-fns", 0);
    {
        remote_session_control_fn rsc = (remote_session_control_fn)(void*)dlsym(rpc_lib, "remote_session_control");
        remote_handle64_open_fn  rho = (remote_handle64_open_fn)(void*)dlsym(rpc_lib, "remote_handle64_open");
        if (rsc) {
            struct { int domain; int enable; } um = { 3, 1 };   /* CDSP unsigned PD */
            (void)rsc(2 /*DSPRPC_CONTROL_UNSIGNED_MODULE*/, &um, sizeof um);
        }
        if (rho) {
            uint64_t rh = 0;
            (void)rho("file:///libQnnHtpV73Skel.so?qnn_2_50_0_skel_handle_invoke&_modver=1.0&_dom=cdsp", &rh);
        }
    }

    {   /* libQnnSystem needs libatomic pre-loaded */
        void* lah = dlopen("libatomic.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!lah) { snprintf(path, sizeof path, "%s/libatomic.so.1", dir); lah = dlopen(path, RTLD_NOW | RTLD_GLOBAL); }
    }

    /* binaryInfo for graph name + tensor metadata */
    char gname[128] = "";
    Qnn_Tensor_t* in_info;
    Qnn_Tensor_t* out_info;
    uint32_t n_in;
    uint32_t n_out;
    snprintf(path, sizeof path, "%s/libQnnSystem.so", dir);
    sys_lib = dlopen(path, RTLD_NOW);
    if (!sys_lib) fail("sys-lib", 0);
    void* ctx_buf = NULL; long ctx_size = 0;
    {
        sysgetproviders_fn sgp = (sysgetproviders_fn)dlsym(sys_lib, "QnnSystemInterface_getProviders");
        if (!sgp) fail("sys-getproviders-fn", 0);
        QnnSystemInterface_t** sp = NULL; uint32_t n = 0;
        if (sgp((const QnnSystemInterface_t***)&sp, &n) != 0 || !n) fail("sys-getproviders", -1);
        sysiface_t si = sp[0]->QNN_SYSTEM_INTERFACE_VER_NAME;
        QnnSystemContext_Handle_t sysCtx = NULL;
        if (si.systemContextCreate(&sysCtx) != 0) fail("sys-ctx-create", -1);
        snprintf(path, sizeof path, "%s/%s", dir, bin);
        FILE* f = fopen(path, "rb");
        if (!f) fail("ctx-file", 0);
        fseek(f, 0, SEEK_END); ctx_size = ftell(f); fseek(f, 0, SEEK_SET);
        ctx_buf = malloc((size_t)ctx_size);
        if (!ctx_buf || fread(ctx_buf, 1, (size_t)ctx_size, f) != (size_t)ctx_size) fail("ctx-read", ctx_size);
        fclose(f);
        const QnnSystemContext_BinaryInfo_t* bi = NULL;
        Qnn_ContextBinarySize_t bis = 0;
        if (si.systemContextGetBinaryInfo(sysCtx, ctx_buf, (uint32_t)ctx_size, &bi, &bis) != 0 || !bi) fail("bininfo", -1);
        const QnnSystemContext_GraphInfo_t* graphs = NULL; uint32_t ng = 0;
        /* every version binds the same graphs/numGraphs fields, so this
         * switch is only a version gate; graphs[0] is then read as
         * graphInfoV1 below without checking graphs[0].version */
        switch (bi->version) {
            case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1: graphs = bi->contextBinaryInfoV1.graphs; ng = bi->contextBinaryInfoV1.numGraphs; break;
            case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2: graphs = bi->contextBinaryInfoV2.graphs; ng = bi->contextBinaryInfoV2.numGraphs; break;
            case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3: graphs = bi->contextBinaryInfoV3.graphs; ng = bi->contextBinaryInfoV3.numGraphs; break;
            default: fail("bininfo-version", -1);
        }
        if (!graphs || ng < 1) fail("bininfo-graphs", 0);
        snprintf(gname, sizeof gname, "%s", graphs[0].graphInfoV1.graphName);
        n_in  = graphs[0].graphInfoV1.numGraphInputs;
        n_out = graphs[0].graphInfoV1.numGraphOutputs;
        in_info  = copy_tensors(graphs[0].graphInfoV1.graphInputs, n_in);
        out_info = copy_tensors(graphs[0].graphInfoV1.graphOutputs, n_out);
        (void)si.systemContextFree(sysCtx);
    }

    snprintf(path, sizeof path, "%s/libQnnHtp.so", dir);
    be_lib = dlopen(path, RTLD_NOW);
    if (!be_lib) fail("be-lib", 0);
    {   /* HTP dlopens the V73 stub by bare name at session time */
        snprintf(path, sizeof path, "%s/libQnnHtpV73Stub.so", dir);
        (void)dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    }
    {
        getproviders_fn gp = (getproviders_fn)dlsym(be_lib, "QnnInterface_getProviders");
        if (!gp) fail("be-getproviders-fn", 0);
        QnnInterface_t** prov = NULL; uint32_t n = 0;
        if (gp((const QnnInterface_t***)&prov, &n) != 0 || !n) fail("be-getproviders", -1);
        /* libQnnHtp registers a single provider: prov[0] is the fallback when
         * the version predicate matches nothing, the last match wins if
         * several ever do */
        QnnInterface_t* matched = prov[0];
        for (uint32_t i = 1; i < n; i++)
            if (prov[i]->apiVersion.coreApiVersion.major == QNN_API_VERSION_MAJOR &&
                prov[i]->apiVersion.coreApiVersion.minor >= QNN_API_VERSION_MINOR)
                matched = prov[i];
        iface = matched->QNN_INTERFACE_VER_NAME;
    }
    {
        Qnn_LogHandle_t log = NULL;
        if (iface.logCreate(NULL, QNN_LOG_LEVEL_ERROR, &log) != 0) fail("log-create", -1);
        if (iface.backendCreate(log, NULL, &backend) != 0) fail("backend-create", -1);
        if (iface.deviceCreate(log, NULL, &device) != 0) fail("device-create", -1);
    }

    /* infrastructure + the DCVS_V3/RPC_CONTROL_LATENCY power-config vote at
     * the requested corner; the power/vote configuration is best-effort — a
     * setPowerConfig failure does not abort the benchmark */
    {
        QnnHtpDevice_PerfInfrastructure_t* perf;
        uint32_t power_id;
        QnnDevice_Infrastructure_t dev_infra = NULL;
        if (iface.deviceGetInfrastructure(&dev_infra) != 0 || !dev_infra) fail("dev-infra", -1);
        QnnHtpDevice_Infrastructure_t* htp_infra = (QnnHtpDevice_Infrastructure_t*)dev_infra;
        if (htp_infra->infraType != QNN_HTP_DEVICE_INFRASTRUCTURE_TYPE_PERF) fail("htp-infra-type", (long)htp_infra->infraType);
        perf = &htp_infra->perfInfra;
        if (perf->createPowerConfigId(0, 0, &power_id) != 0) fail("power-id", -1);
        QnnHtpPerfInfrastructure_PowerConfig_t dcvs, rpc_lat;
        const QnnHtpPerfInfrastructure_PowerConfig_t* cfgs[3];
        memset(&dcvs, 0, sizeof dcvs);
        memset(&rpc_lat, 0, sizeof rpc_lat);
        dcvs.option = QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_DCVS_V3;
        dcvs.dcvsV3Config.contextId = power_id;
        dcvs.dcvsV3Config.setDcvsEnable = 1;
        dcvs.dcvsV3Config.dcvsEnable = 0;
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
        (void)perf->setPowerConfig(power_id, cfgs);
    }

    if (iface.contextCreateFromBinary(backend, device, NULL, ctx_buf,
                                      (uint32_t)ctx_size, &context, NULL) != 0 || !context)
        fail("context-create", -1);
    if (iface.graphRetrieve(context, gname, &graph) != 0 || !graph) fail("graph-retrieve", -1);

    /* graphExecute below is passed these same clamped counts, so a graph with
     * more than 8 inputs or outputs fails at execute-warm rather than run */
    if (n_in > 8) n_in = 8;
    if (n_out > 8) n_out = 8;
    register_io(in_info, n_in, "in", iface, context, rpc_alloc, rpc_tofd, 1);
    register_io(out_info, n_out, "out", iface, context, rpc_alloc, rpc_tofd, 0);

    for (int w = 0; w < 3; w++)
        if (iface.graphExecute(graph, in_info, n_in, out_info, n_out, NULL, NULL) != 0)
            fail("execute-warm", -w - 1);

    double total = 0, best = 1e9, worst = 0;
    long reps = 0;
    double t_end = now_ms() + (double)seconds * 1e3;
    while (now_ms() < t_end) {
        double t0 = now_ms();
        Qnn_ErrorHandle_t e = iface.graphExecute(graph, in_info, n_in, out_info, n_out, NULL, NULL);
        double dt = now_ms() - t0;
        if (e != 0) fail("execute-timed", reps); /* err = iteration count; consumers key on step only */
        total += dt; reps++;
        if (dt < best) best = dt;
        if (dt > worst) worst = dt;
    }
    if (!reps) fail("no-reps", 0);

    printf("{\"ok\":1,\"bin\":\"%s\",\"corner\":\"0x%02X\",\"seconds\":%d,\"reps\":%ld,"
           "\"exec_us\":%.0f,\"min_us\":%.0f,\"max_us\":%.0f,\"fps_cap\":%.0f}\n",
           bin, vc, seconds, reps,
           total / reps * 1e3, best * 1e3, worst * 1e3,
           1e3 / (total / reps));
    fflush(stdout);
    return 0;
}
