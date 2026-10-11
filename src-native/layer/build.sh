#!/bin/sh
# layer/build.sh — FULL seam layer build (Armada OS port, deploy stage).
#
# Sources = the R59 production lineage, ported into this repo per plan §4.2:
#   minimal_seam_layer.c  the layer itself (entry points, four intercepted
#                         buffer-ops, compute chassis, route, worker launch)
#   seam_layer.c          the seam protocol engine (mailbox/doorbell/ack/plan)
#   qnn_glue.c            the in-game NPU worker (QNN session + T_fsr4 loop)
#                         — carries the FIXED M3 mask=3 vote (DCVS_V3 0xA0/0xA0
#                         + RPC_CONTROL_LATENCY=100, NO RPC_POLLING_TIME,
#                         setPowerConfig failure non-fatal)
# Upstream lineage: fsr4-npu/vkbridge/layer (RUNBOOK:12007-12014 "THREE files,
# -ldl only" — the R59H boot-27 "we have FSR4" production build). Android
# paths stripped/replaced per plan §4.2; game gate matches NMS.exe.
#
# zig cc glibc aarch64 (no NDK, no bionic — AGENTS.md build rule). Target
# 2.27 floor; the container is steamrt4/Fedora 44 (backward compatible).
#
# SEAM_LOG + SEAM_LOG_DIR: the passive telemetry channel — the layer log at
# /var/home/armada/fsr4npu-logs/fsr4npu-SEAM.log (dir must exist on device;
# M0 already created it). NO debug env keys ride the launch env — the logs
# capture passively (deploy-stage law).
#
# git-bash/MSYS path mangling guard (live-proven 2026-10-09): without
# MSYS_NO_PATHCONV=1 the -DSEAM_LOG_DIR '/var/home/...' literal is rewritten
# to 'C:/Program Files/Git/var/home/...' inside the .so.
set -e
cd "$(dirname "$0")"
export MSYS_NO_PATHCONV=1
ZIG="${ZIG:-$LOCALAPPDATA/zig-x86_64-windows-0.16.0/zig.exe}"
QAIRT_INC="${QAIRT_INC:-C:/Qualcomm/qairt/2.50.0.260828/include/QNN}"
#   seam_nms.c            NMS-1 (XeSS slot) doorbell record path + worker
#                         glue, rev3a (design: docs/NPU-AT-XESS-SLOT-DESIGN.md;
#                         arms only on FSR4NPU_NPU=1 — otherwise inert)
"$ZIG" cc -target aarch64-linux-gnu.2.27 -shared -fPIC -O2 -Wall -std=gnu11 \
  -DSEAM_LOG -DSEAM_LOG_DIR='"/var/home/armada/fsr4npu-logs"' \
  -I"$QAIRT_INC" -Ishaders -I../../fsr4-npu/audit/vklocal \
  -o fsr4npu_seam.aarch64.so \
  minimal_seam_layer.c seam_layer.c qnn_glue.c seam_nms.c \
  -lpthread -ldl
echo "built fsr4npu_seam.aarch64.so (full seam layer + NMS-1 doorbell/worker, mask=3 vote, NMS gate)"
