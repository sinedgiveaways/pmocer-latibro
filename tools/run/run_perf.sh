#!/usr/bin/env bash
# Run build/orbitals-together under perf record (user space, flat) for up to $2 s (default 120) with the GPU memory cap; then the
# 40 hottest symbols of the boot. Usage: run_perf.sh NAME [SECONDS]; WORLD_SECS (default 30): the game's own --seconds
# limit, world clock. ORBITALS_RUN_BIN: another executable (a player folder's mkw, to compare its orbitalgame.so).
set -euo pipefail
cd "$(dirname "$0")/../.."
# GPU memory cap 4096 MiB: with 3072 both heaps fill and the race's 1280x720 targets are not created, so the scene is
# black (sp-v0, sp-h0, 01/11/2026); every good race run of that day (rt-*, vis*, m30) had 4096 passed by hand.
n=$1; secs=${2:-120}; log=build/run/$n.log
# The last line of every run, whatever ended it: a wait looks for it instead of guessing from the output (07/11/2026:
# waits for «exit» in a run's output never ended, the perf report has no such word, and two twins sat idle).
trap 'echo "run_perf.sh: $n ended (rc $?)"' EXIT
. tools/run/why.sh "$n"
# MIN_AVAIL only raises the floor: below 8500 the system OOM killed runs and the desktop (on an integrated GPU;
# 07/11/2026: four runs launched at 8000 to get past it).
[ "${MIN_AVAIL:-8500}" -ge 8500 ] || { echo "MIN_AVAIL=${MIN_AVAIL} refused: the floor is 8500 MB; wait for memory instead"; exit 1; }
tools/run/gpu_ok.sh || exit 1   # no run on a GPU left where the next fault can freeze the desktop
# The pre-submit checks stop the run before a fault reaches the GPU (vk_guard.c); ORBITALS_GPU_GUARD=0 to measure without.
export ORBITALS_GPU_GUARD=${ORBITALS_GPU_GUARD:-1}
avail=$(tools/run/avail.sh --wait "${MIN_AVAIL:-8500}")   # WAIT_MEM=<min>: wait for it instead of refusing
[ "$avail" -ge "${MIN_AVAIL:-8500}" ] || { echo "only $avail MB available (with the GPU page pool): not starting"; exit 1; }
# Our runs open their window behind whatever is in use, without the focus; the game is told it has it (it pauses
# otherwise). ORBITALS_BACKGROUND_WINDOW=0 brings the window to the front.
# The control FIFO (runtime/core/base/ctl.c), so tools/run/ctl.sh shot reaches these runs
# too (mz4-fp, 08/11/2026: the HUD shots of a race run went nowhere without it).
mkdir -p build/run; rm -f "build/run/$n.ctl"; mkfifo "build/run/$n.ctl"
export ORBITALS_CTL=${ORBITALS_CTL:-$PWD/build/run/$n.ctl}
export ORBITALS_BACKGROUND_WINDOW=${ORBITALS_BACKGROUND_WINDOW:-1} ORBITALS_ASSUME_FOCUS=${ORBITALS_ASSUME_FOCUS:-1}
tools/run/kwin-rule.sh --ensure >/dev/null   # the rule that keeps that window minimized and without focus
ORBITALS_FLIGHT_DIR=${ORBITALS_FLIGHT_DIR:-$PWD/scratchpad/flight${n#run}} ORBITALS_GPU_MEM_LIMIT=${ORBITALS_GPU_MEM_LIMIT:-4096} ORBITALS_SPIRV_DUMP=$PWD/scratchpad/missing-$n ORBITALS_TRACE=${ORBITALS_TRACE:-gfx,present} JOB_MEM=${JOB_MEM:-7G} \
  tools/run/job.sh run "$n" perf record ${PERF_ARGS:-} -F 499 -o "scratchpad/$n.perf" -- timeout -s KILL "$secs" "${ORBITALS_RUN_BIN:-build/orbitals-together}" --seconds "${WORLD_SECS:-30}" \
  > "$log" 2>&1 || true
echo "nvn functions $(grep -c 'first call' "$log"), presents $(grep -c 'QueuePresentTexture' "$log"), last frame $(grep -o 'vk_exec: frame [0-9]*' "$log" | tail -1)"
grep -n 'FATAL\|fatal signal' "$log" | head -3 || true
# With call graphs (-g) the report took minutes and memory on a box already short of it (rt-z7, 01/11/2026): only the
# file then, for perf script or perf report by hand.
case " ${PERF_ARGS:-} " in
  *" -g "*|*" --call-graph"*) echo "perf data with call graphs: scratchpad/$n.perf (no report here)" ;;
  # head closes the pipe after 40 lines: the SIGPIPE upstream is not the run's result (rc 141, bc1-c 08/11/2026)
  *) perf report -i "scratchpad/$n.perf" --stdio --no-children --sort symbol 2>/dev/null | grep -v '^#' | grep -v '^$' | head -40 || true ;;
esac
