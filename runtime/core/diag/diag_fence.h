// ORBITALS_DIAG_FENCE=1: a record of the waits and signals that can hold a frame back (diag_fence.c).
#pragma once
#include <stddef.h>
#include <stdint.h>

// Event kinds. *_WAIT, ACQUIRE and FINISH open a blocking wait that the matching *_END closes.
enum {
    DF_SUBMIT,           // nvnQueueSubmitCommands done: obj queue, a fence value, b handles | held << 32
    DF_CATCHUP_TIMEOUT,  // the submit's 500 ms wait for its own fence gave up: obj queue, a fence value
    DF_FENCE_WAIT,       // the back end blocks a thread for a fence: a value, b timeout ns (all ones: none)
    DF_FENCE_WAIT_END,   // a value, b 1 reached or 0 not, obj ns waited
    DF_SYNC_WAIT,        // nvnSyncWait blocks (not a poll): obj sync address, a fence value, b timeout ns
    DF_SYNC_WAIT_END,    // obj sync address, a fence value, b 1 satisfied or 0 timed out
    DF_ACQUIRE,          // nvnWindowAcquireTexture enters: obj window, a image
    DF_ACQUIRE_END,      // obj window, a image
    DF_ACQUIRE_TIMEOUT,  // the acquire's 1 s wait for the GPU gave up: obj window, a image, b fence value
    DF_PRESENT,          // nvnQueuePresentTexture: obj window, a image, b presents so far
    DF_FINISH,           // nvnQueueFinish enters
    DF_FINISH_END,
    DF_FENCE_DONE,       // the completion thread moved "done": a was, b reached
    DF_EVENT_SIGNAL,     // nvnEventSignal by the CPU: obj value address, a mode, b value
    DF_EVENT_WRITE,      // a GPU-side event signal became visible at its fence: obj value address, a mode, b value
    DF_N
};

// -1: not read yet; 0: off; 1: on. The macro costs one load while it is off.
extern volatile int diag_fence_state;
void diag_fence_note(unsigned kind, uint64_t obj, uint64_t a, uint64_t b);
#define DIAG_FENCE(kind, obj, a, b) \
    do { if (diag_fence_state) diag_fence_note((kind), (uint64_t)(obj), (uint64_t)(a), (uint64_t)(b)); } while (0)

// Guest-side waits and signals, by what they are on: the idle threads of an abort sit in these, not in the NVN layer.
// DK_CV: the key of WaitProcessWideKeyAtomic / SignalProcessWideKey (svc 0x1C, 0x1D: nn::os events and condition
// variables). DK_ADDR: the address of WaitForAddress / SignalToAddress (0x34, 0x35). DK_EVENT: the kernel event
// (host pointer of the Event: its readable and writable handles differ) of WaitSynchronization / SignalEvent
// (0x18, 0x11) and of the host's own signals (vsync, decoders). Per key: waits, signals, when and by whom last.
enum { DK_CV = 1, DK_ADDR, DK_EVENT };
void diag_fence_key(unsigned kind, uint64_t id, int signal);
#define DIAG_KEY(kind, id, signal) \
    do { if (diag_fence_state) diag_fence_key((kind), (uint64_t)(id), (signal)); } while (0)
// One line for a waiting thread (kernel_diag.c's thread dump): how often its key was waited on and signalled. 0: none.
// flag 0: a wait with a timeout began on the key; 1: such a wait ended by timing out.
void diag_fence_key_flag(unsigned kind, uint64_t id, int flag);
int diag_fence_key_text(unsigned kind, uint64_t id, char *out, size_t cap);

// A line of the counters and of the open waits, once every 5 s of the vsync thread's ticks (gfx_present.c).
void diag_fence_heartbeat(uint64_t tick);
// The counters, the open waits and the last events, after the guest threads' dump at a fatal abort (svc.c).
void diag_fence_dump(void);
