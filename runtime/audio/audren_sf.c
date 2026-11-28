// The SDK's audio service objects, made by the host. nn::audio::CreateAudioRendererManager(bool), replaced by name,
// hands the nn::audio client an IAudioRendererManager whose vtable holds host functions, so OpenAudioRenderer,
// RequestUpdate and the device calls arrive here as C calls on audren.c and audio_mixer.c, with no IPC message,
// session or service name in between (Adrià, 05/11/2026: the SDK replaced by name, no console service imitated).
//
// The sf object ABI of MKW's SDK 19.3.3, read from the lifted client: a guest object
// whose first word is its vtable; slot 0 AddReference(), 1 Release(), 2-3 never called by the client, then one slot
// per method in command order (IAudioDevice has no command 9). x0 is the object; an Out<T> of plain data is a T*, an
// In struct a pointer to it, an InBuffer / OutBuffer a pointer to {addr, size}, a NativeHandle (in or out) a pointer
// to {u32 handle, u8 managed}, an Out<SharedPointer<I>> a pointer to the SharedPointer's address; a float argument
// comes in s0 after the integer ones. Methods return nn::Result in w0. The signatures are those of the SDK's own
// server classes, whose symbols the sdk keeps (nn::audio::server::AudioRendererManagerImpl::*). A slot no client call
// of MKW reaches is a named fatal, not a guess.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "stream/stream.h"
#include "audren.h"
#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "platform/platform.h"

typedef struct { uint64_t vtable; void *host; int32_t refs, pad; } SfObject;   // in guest memory

enum { MANAGER_SLOTS = 9, RENDERER_SLOTS = 18, DEVICE_SLOTS = 24 };
static uint64_t *manager_vt, *renderer_vt, *device_vt;
static SfObject *manager, *device;
static PlatOnce once;

void *audren_guest(uint64_t addr, uint64_t size) {
    if ((!addr && size) || addr + size < addr || addr + size > RT_ADDRESS_LIMIT)
        rt_fatal("audio: client buffer %#llx+%#llx", (unsigned long long)addr, (unsigned long long)size);
    return (void *)(uintptr_t)addr;
}

static void ok(Cpu *c) { ret_x(c, 0); }
static AudioRenderer *renderer_of(const Cpu *c) { return ((SfObject *)arg_p(c, 0))->host; }

static SfObject *object_new(const uint64_t *vtable, void *host) {
    SfObject *o = mem_kernel_alloc(sizeof *o, 16);
    *o = (SfObject){(uint64_t)(uintptr_t)vtable, host, 1, 0};
    return o;
}

static void out_object(const Cpu *c, int i, const SfObject *o) {   // Out<SharedPointer<I>>
    ST64(LD64(arg_x(c, i)), (uint64_t)(uintptr_t)o);
}

static void out_handle(const Cpu *c, int i, uint32_t h) {   // Out<NativeHandle>: the client closes it (managed)
    uint64_t p = arg_x(c, i);
    ST32(p, h);
    ST8(p + 4, 1);
}

#define UNSERVED(iface)                                                                                             \
    static void unserved_##iface(Cpu *c) {                                                                         \
        char lr[160];                                                                                              \
        rt_fatal("audio: " #iface " method not served by the host, called from %s",                              \
                 ldr_describe(c->x[30] - 4, lr, sizeof lr));                                                       \
    }                                                                                                              \
    HOST_FUNCTION("audio " #iface " unserved", unserved_##iface)
UNSERVED(IAudioRendererManager);
UNSERVED(IAudioRenderer);
UNSERVED(IAudioDevice);

static void shared_ref(Cpu *c) { (void)c; }   // the manager and the device live as long as the process
HOST_FUNCTION("audio shared AddReference/Release", shared_ref);

// ---- IAudioRendererManager
// OpenAudioRenderer(Out<SharedPointer<IAudioRenderer>>, AudioRendererParameterInternal const&, NativeHandle
// transfer memory, NativeHandle process, u64 size, AppletResourceUserId, ...)
static void manager_open(Cpu *c) {
    uint32_t h = LD32(arg_x(c, 3));
    uint64_t size = arg_x(c, 5);
    const KernTransferMemory *tm = kern_handle_get(h, KERN_TMEM);
    if (!tm || tm->size < size) rt_fatal("audio: OpenAudioRenderer work buffer is not a transfer memory of %#llx bytes",
                                         (unsigned long long)size);
    out_object(c, 1, object_new(renderer_vt, audren_open(arg_p(c, 2), tm->addr, size)));
    ok(c);
}
HOST_FUNCTION("audio IAudioRendererManager OpenAudioRenderer", manager_open);

static void manager_work_size(Cpu *c) {   // GetWorkBufferSize(Out<long>, AudioRendererParameterInternal const&)
    ST64(arg_x(c, 1), audren_work_buffer_size(arg_p(c, 2)));
    ok(c);
}
HOST_FUNCTION("audio IAudioRendererManager GetWorkBufferSize", manager_work_size);

static void manager_device(Cpu *c) {   // GetAudioDeviceService(WithRevisionInfo)(Out<SharedPointer<IAudioDevice>>, ...)
    out_object(c, 1, device);
    ok(c);
}
HOST_FUNCTION("audio IAudioRendererManager GetAudioDeviceService", manager_device);

// ---- IAudioRenderer
static void renderer_add_ref(Cpu *c) { __atomic_fetch_add(&((SfObject *)arg_p(c, 0))->refs, 1, __ATOMIC_RELAXED); }
HOST_FUNCTION("audio IAudioRenderer AddReference", renderer_add_ref);

static void renderer_release(Cpu *c) {
    SfObject *o = arg_p(c, 0);
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL)) return;
    audren_close(o->host);
    mem_kernel_free(o, sizeof *o);
}
HOST_FUNCTION("audio IAudioRenderer Release", renderer_release);

static void renderer_sample_rate(Cpu *c) { ST32(arg_x(c, 1), (uint32_t)renderer_of(c)->param.sample_rate), ok(c); }
static void renderer_sample_count(Cpu *c) { ST32(arg_x(c, 1), (uint32_t)renderer_of(c)->param.sample_count), ok(c); }
static void renderer_mix_buffers(Cpu *c) { ST32(arg_x(c, 1), (uint32_t)renderer_of(c)->param.mix_buffer_count), ok(c); }
static void renderer_state(Cpu *c) { ST32(arg_x(c, 1), renderer_of(c)->state), ok(c); }
HOST_FUNCTION("audio IAudioRenderer GetSampleRate", renderer_sample_rate);
HOST_FUNCTION("audio IAudioRenderer GetSampleCount", renderer_sample_count);
HOST_FUNCTION("audio IAudioRenderer GetMixBufferCount", renderer_mix_buffers);
HOST_FUNCTION("audio IAudioRenderer GetState", renderer_state);

static void renderer_update(Cpu *c) {   // RequestUpdate(Auto)(OutBuffer output, OutBuffer performance, InBuffer input)
    audren_update(renderer_of(c), arg_p(c, 1), arg_p(c, 2), arg_p(c, 3));
    ok(c);
}
HOST_FUNCTION("audio IAudioRenderer RequestUpdate", renderer_update);

static void renderer_start(Cpu *c) { audren_set_running(renderer_of(c), 1), ok(c); }
static void renderer_stop(Cpu *c) { audren_set_running(renderer_of(c), 0), ok(c); }
static void renderer_event(Cpu *c) { out_handle(c, 1, kern_event_readable(renderer_of(c)->event)), ok(c); }
HOST_FUNCTION("audio IAudioRenderer Start", renderer_start);
HOST_FUNCTION("audio IAudioRenderer Stop", renderer_stop);
HOST_FUNCTION("audio IAudioRenderer QuerySystemEvent", renderer_event);

static void renderer_set_limit(Cpu *c) { renderer_of(c)->time_limit = (uint32_t)arg_x(c, 1), ok(c); }
static void renderer_get_limit(Cpu *c) {
    uint32_t v = renderer_of(c)->time_limit;
    ST32(arg_x(c, 1), v ? v : 100);
    ok(c);
}
HOST_FUNCTION("audio IAudioRenderer SetRenderingTimeLimit", renderer_set_limit);
HOST_FUNCTION("audio IAudioRenderer GetRenderingTimeLimit", renderer_get_limit);

// SetVoiceDropParameter / GetVoiceDropParameter (float): the host never drops voices; the value is kept.
static void renderer_set_drop(Cpu *c) { renderer_of(c)->voice_drop = arg_s(c, 0), ok(c); }
static void renderer_get_drop(Cpu *c) {
    float v = renderer_of(c)->voice_drop;
    uint32_t b;
    memcpy(&b, &v, 4);
    ST32(arg_x(c, 1), b);
    ok(c);
}
HOST_FUNCTION("audio IAudioRenderer SetVoiceDropParameter", renderer_set_drop);
HOST_FUNCTION("audio IAudioRenderer GetVoiceDropParameter", renderer_get_drop);

// ---- IAudioDevice: one stereo output device that never changes
static uint32_t change_event;   // the device change events: never signalled

static void put_name(uint64_t buffer) {   // OutBuffer of AudioDeviceName (0x100 bytes)
    const AudrenBuffer *b = (const AudrenBuffer *)(uintptr_t)buffer;
    if (!b->size) return;
    char *dst = audren_guest(b->addr, b->size);
    memset(dst, 0, b->size);
    strncpy(dst, audio_device_name, b->size - 1);
}

static void device_list(Cpu *c) {   // ListAudioDeviceName(Auto), ListAudioOutputDeviceName(OutBuffer, Out<int>)
    const AudrenBuffer *b = arg_p(c, 1);
    int32_t n = b->size >= 0x100;
    if (n) put_name(arg_x(c, 1));
    ST32(arg_x(c, 2), (uint32_t)n);
    ok(c);
}
static void device_set_volume(Cpu *c) { audio_device_set_volume(arg_s(c, 0)), ok(c); }   // (InBuffer name, float)
static void device_get_volume(Cpu *c) {   // (InBuffer name, Out<float>)
    float v = audio_device_volume();
    uint32_t b;
    memcpy(&b, &v, 4);
    ST32(arg_x(c, 2), b);
    ok(c);
}
static void device_active_name(Cpu *c) { put_name(arg_x(c, 1)), ok(c); }   // GetActiveAudio(Output)DeviceName(Auto)
static void device_event(Cpu *c) {   // Query*Event(Out<NativeHandle>)
    uint32_t ev = __atomic_load_n(&change_event, __ATOMIC_ACQUIRE);
    if (!ev) {
        uint32_t mine = kern_event_new(), expected = 0;
        ev = __atomic_compare_exchange_n(&change_event, &expected, mine, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
                 ? mine
                 : expected;   // the loser's event stays unused
    }
    out_handle(c, 1, kern_event_readable(ev));
    ok(c);
}
static void device_channels(Cpu *c) { ST32(arg_x(c, 1), audio_output_channels()), ok(c); }   // GetActiveChannelCount
// Switch 2: SetAudioDeviceOutputVolumeAutoTuneEnabled(bool) / IsAudioDeviceOutputVolumeAutoTuneEnabled(Out<bool>):
// no automatic volume tuning here; setting it changes nothing.
static void device_set_autotune(Cpu *c) { ok(c); }
static void device_get_autotune(Cpu *c) { ST8(arg_x(c, 1), 0), ok(c); }
HOST_FUNCTION("audio IAudioDevice ListAudioDeviceName", device_list);
HOST_FUNCTION("audio IAudioDevice SetAudioDeviceOutputVolume", device_set_volume);
HOST_FUNCTION("audio IAudioDevice GetAudioDeviceOutputVolume", device_get_volume);
HOST_FUNCTION("audio IAudioDevice GetActiveAudioDeviceName", device_active_name);
HOST_FUNCTION("audio IAudioDevice QueryAudioDeviceEvent", device_event);
HOST_FUNCTION("audio IAudioDevice GetActiveChannelCount", device_channels);
HOST_FUNCTION("audio IAudioDevice SetAudioDeviceOutputVolumeAutoTuneEnabled", device_set_autotune);
HOST_FUNCTION("audio IAudioDevice IsAudioDeviceOutputVolumeAutoTuneEnabled", device_get_autotune);

// ---- vtables
static uint64_t *vtable(int n, GuestFn add_ref, GuestFn release, GuestFn unserved, const GuestFn *methods, int nm) {
    uint64_t *vt = mem_kernel_alloc(8u * (uint64_t)n, 16);
    vt[0] = hostfn_address(add_ref), vt[1] = hostfn_address(release);
    for (int i = 2; i < n; i++) vt[i] = hostfn_address(i - 4 < nm && i >= 4 && methods[i - 4] ? methods[i - 4] : unserved);
    return vt;
}

static void build(void *arg) {
    (void)arg;
    // Slot 4 + command; NULL: not served (OpenAudioRendererForManualExecution, ExecuteAudioRendererRendering, the
    // device notifications 15-18, which no MKW call reaches).
    static const GuestFn mgr[] = {manager_open, manager_work_size, manager_device, NULL, manager_device};
    static const GuestFn ren[] = {renderer_sample_rate, renderer_sample_count, renderer_mix_buffers, renderer_state,
                                  renderer_update, renderer_start, renderer_stop, renderer_event, renderer_set_limit,
                                  renderer_get_limit, renderer_update, NULL, renderer_set_drop, renderer_get_drop};
    static const GuestFn dev[] = {device_list, device_set_volume, device_get_volume, device_active_name,
                                  device_event, device_channels, device_list, device_set_volume, device_get_volume,
                                  /* 10 */ device_active_name, device_event, device_event, device_active_name,
                                  device_list, NULL, NULL, NULL, NULL, device_set_autotune, device_get_autotune};
    _Static_assert(4 + sizeof mgr / sizeof *mgr == MANAGER_SLOTS, "IAudioRendererManager slots");
    _Static_assert(4 + sizeof ren / sizeof *ren == RENDERER_SLOTS, "IAudioRenderer slots");
    _Static_assert(4 + sizeof dev / sizeof *dev == DEVICE_SLOTS, "IAudioDevice slots");
    manager_vt = vtable(MANAGER_SLOTS, shared_ref, shared_ref, unserved_IAudioRendererManager, mgr, 5);
    renderer_vt = vtable(RENDERER_SLOTS, renderer_add_ref, renderer_release, unserved_IAudioRenderer, ren, 14);
    device_vt = vtable(DEVICE_SLOTS, shared_ref, shared_ref, unserved_IAudioDevice, dev, 20);
    manager = object_new(manager_vt, NULL);
    device = object_new(device_vt, NULL);
}

// nn::audio::CreateAudioRendererManager(bool dfc) -> SharedPointer<IAudioRendererManager> (in x8): the same host
// object for the IPC and the in-process (DFC) choice.
static void create_manager(Cpu *c) {
    plat_once(&once, build, NULL);
    ST64(c->x[8], (uint64_t)(uintptr_t)manager);
}
HOST_OVERRIDE("_ZN2nn5audio26CreateAudioRendererManagerEb", create_manager);

// nn::audio::detail::GetDefaultVirtualOutputDevice(AudioDeviceName *out): the name of the one output device, the same
// as IAudioDevice's (put_name). The SDK's code asks a console service over IPC (svc 0x1f at sdk+0x1a44ec, called from
// main+0x6a45b58), which the runtime does not serve: the run stopped there about 20 s in. AudioDeviceName is 0x100
// bytes, as in put_name. The return type is not in the mangled name: 0 serves a void and a successful Result alike.
static void default_virtual_device(Cpu *c) {
    char *out = arg_p(c, 0);
    if (out) {
        memset(out, 0, 0x100);
        strncpy(out, audio_device_name, 0xff);
    }
    static int logged;
    if (!logged++) rt_log("audio: GetDefaultVirtualOutputDevice -> %s", audio_device_name);
    ret_x(c, 0);
}
HOST_OVERRIDE("_ZN2nn5audio6detail29GetDefaultVirtualOutputDeviceEPNS0_15AudioDeviceNameE", default_virtual_device);

// ---- nn::audio output streams (OpenOutputStream, WriteOutputStream...): Orbitals' sound. Unreal mixes the game's audio
// itself and writes the mix to one output stream (the audio renderer it also opens gets a single update), so a silent
// stream meant a silent game. Each stream is now an SDL audio stream on the default device, which SDL mixes with the
// renderer's output; without a device (ORBITALS_AUDIO=none, no SDL audio) it is consumed in real time as before.
// The SDK's code opens a console audio service over IPC (nn::sm::Initialize, svc 0x1f at sdk+0x1a44ec, from main+0x6a45b90 in
// the game's audio output set-up), which the runtime does not serve: the run stopped there. Failing the open instead made the
// game tear its audio renderer down and draw black frames at 30 fps for 10 minutes (no hid, 27 files opened), so every
// function main imports is replaced here.
// Layout read from the SDK (tools/analisis/frame_report.py --func): OutputStreamType holds at +0 the address of its
// OutputStreamImpl, built at +8; the public functions read that pointer and call the impl. Only +0 is written (the open
// stream's impl address, as the SDK does) and the rest lives in a host table. The open's first int is the capacity in
// frames; StreamFormat is {sample format, channels, sample rate}: Orbitals opens {5, 2, 48000} with ints 2048, 1536
// (run of 17/11/2026), 5 being nn::audio's PcmFloat (1 Int8, 2 Int16, 3 Int24, 4 Int32, 5 Float). Out-of-range values fall
// back to 4096 frames, 2 channels, 48 kHz, float.
// The free frame count the game paces its writes on is the capacity minus what SDL still holds: the device's own clock,
// so the game never runs ahead of the speakers (a real-time estimate drifts from the device by its clock error).
// The SystemEvent of the event overload is created and signalled on every vsync (60 Hz), the rate the game's audio thread
// gets woken at to write more; it stays bound after a close (gfx_vsync_bind has no unbind; the game opens one stream).
// Statistics (24 bytes through x8) are zeros: what the SDK returns when the mixer has none.
#define MAX_STREAMS 8
typedef struct {
    uint64_t type;                 // the game's OutputStreamType; 0 for a free slot
    uint32_t rate, capacity, channels, frame_bytes;
    int started;
    float gain;                    // SetOutputStreamGain
    SDL_AudioStream *sdl;          // NULL: no device, consumed in real time
    uint64_t written, consumed;    // frames, since the open (real-time model only)
    uint64_t last_ns;              // when consumed was last brought up to date (game clock)
    SDL_AudioFormat fmt;           // the samples' format: only float goes to the browser stream
} OutStream;
static OutStream streams[MAX_STREAMS];
static PlatMutex stream_lock = PLAT_MUTEX_INIT;

static uint64_t stream_now_ns(void) { return rt_ticks() * RT_TICK_NS; }

static OutStream *stream_find(uint64_t type) {
    for (int i = 0; i < MAX_STREAMS; i++)
        if (type && streams[i].type == type) return &streams[i];
    return NULL;
}

// Without a device: the frames played since the last call, while started; playing stops at an empty stream (an
// underrun), as a mixer does.
static void stream_advance(OutStream *st) {
    uint64_t now = stream_now_ns();
    if (!st->sdl && st->started && now > st->last_ns) {
        uint64_t played = (now - st->last_ns) * st->rate / 1000000000ull;
        uint64_t filled = st->written - st->consumed;
        st->consumed += played < filled ? played : filled;
    }
    st->last_ns = now;
}

static uint32_t stream_free(OutStream *st) {
    uint64_t filled = st->written - st->consumed;
    if (st->sdl) {
        int q = SDL_GetAudioStreamQueued(st->sdl);
        filled = q > 0 ? (uint64_t)q / st->frame_bytes : 0;
    }
    return filled >= st->capacity ? 0 : (uint32_t)(st->capacity - filled);
}

static SDL_AudioFormat sdl_format(uint32_t f, uint32_t *bytes) {
    switch (f) {
    case 1: *bytes = 1; return SDL_AUDIO_S8;
    case 2: *bytes = 2; return SDL_AUDIO_S16LE;
    case 4: *bytes = 4; return SDL_AUDIO_S32LE;
    default: *bytes = 4; return SDL_AUDIO_F32LE;   // 5, PcmFloat; 3 (Int24) has no SDL format and is not used here
    }
}

static SDL_AudioStream *stream_device(uint32_t rate, uint32_t channels, SDL_AudioFormat fmt) {
    const char *mode = cfg_get("ORBITALS_AUDIO");
    if ((mode && !strcmp(mode, "none")) || cfg_get("ORBITALS_AUDIO_OFF")) return NULL;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        rt_log("audio: output stream: SDL audio init failed (%s): consumed without sound", SDL_GetError());
        return NULL;
    }
    SDL_AudioSpec spec = {fmt, (int)channels, (int)rate};
    SDL_AudioStream *s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!s) {
        rt_log("audio: output stream: no device (%s): consumed without sound", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    return s;   // opened paused: StartOutputStream resumes it
}

static void stream_close_device(OutStream *st) {
    if (!st->sdl) return;
    SDL_DestroyAudioStream(st->sdl);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    st->sdl = NULL;
}

static void stream_open(Cpu *c, uint64_t type, uint64_t ev, uint64_t format, int frames, int extra) {
    const uint32_t *f = (const uint32_t *)(uintptr_t)format;
    uint32_t rate = f && f[2] >= 8000 && f[2] <= 192000 ? f[2] : 48000;
    uint32_t channels = f && f[1] >= 1 && f[1] <= 8 ? f[1] : 2;
    uint32_t capacity = frames > 0 && frames <= (1 << 20) ? (uint32_t)frames : 4096;
    uint32_t bytes;
    SDL_AudioFormat fmt = sdl_format(f ? f[0] : 5, &bytes);
    SDL_AudioStream *sdl = stream_device(rate, channels, fmt);
    plat_mutex_lock(&stream_lock);
    OutStream *st = stream_find(type);
    if (st) stream_close_device(st);   // opened again without a close
    for (int i = 0; !st && i < MAX_STREAMS; i++)
        if (!streams[i].type) st = &streams[i];
    if (st) *st = (OutStream){type, rate, capacity, channels, channels * bytes, 0, 1.0f, sdl, 0, 0, stream_now_ns(), fmt};
    plat_mutex_unlock(&stream_lock);
    if (!st) rt_fatal("audio: more than %d output streams", MAX_STREAMS);
    ST64(type, type + 8);
    if (ev) {
        kern_bind_system_event(c, ev, 1);
        gfx_vsync_bind(ev);
    }
    char lr[96];
    const char *dev = sdl ? SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(sdl)) : NULL;
    rt_log("audio: OpenOutputStream %#llx: %s%s%s, %u Hz, %u channels, %u frames (format %#x %#x %#x, ints %d %d, event %#llx) "
           "from %s", (unsigned long long)type, sdl ? "on '" : "without a device", sdl ? (dev ? dev : "?") : "", sdl ? "'" : "",
           rate, channels, capacity, f ? f[0] : 0, f ? f[1] : 0, f ? f[2] : 0, frames, extra, (unsigned long long)ev,
           ldr_describe(c->x[30] - 4, lr, sizeof lr));
    ret_x(c, 0);
}
// Result OpenOutputStream(OutputStreamType *, os::SystemEvent *, void *work, size_t, const AudioDeviceName &,
//                         const StreamFormat &, int, int, BitFlagSet<32, OutputStreamFlag>)
static void open_stream_event(Cpu *c) {
    stream_open(c, arg_x(c, 0), arg_x(c, 1), arg_x(c, 5), (int)arg_x(c, 6), (int)arg_x(c, 7));
}
// Result OpenOutputStream(OutputStreamType *, void *work, size_t, const AudioDeviceName &, const StreamFormat &, int,
//                         BitFlagSet<32, OutputStreamFlag>)
static void open_stream(Cpu *c) { stream_open(c, arg_x(c, 0), 0, arg_x(c, 4), (int)arg_x(c, 5), 0); }

static void close_stream(Cpu *c) {   // void CloseOutputStream(OutputStreamType *)
    uint64_t type = arg_x(c, 0);
    plat_mutex_lock(&stream_lock);
    OutStream *st = stream_find(type);
    if (st) {
        stream_close_device(st);
        st->type = 0;
    }
    plat_mutex_unlock(&stream_lock);
    if (type) ST64(type, 0);
}
static void stream_set_started(uint64_t type, int on) {
    plat_mutex_lock(&stream_lock);
    OutStream *st = stream_find(type);
    if (st) {
        stream_advance(st);
        st->started = on;
        if (st->sdl) (void)(on ? SDL_ResumeAudioStreamDevice(st->sdl) : SDL_PauseAudioStreamDevice(st->sdl));
    }
    plat_mutex_unlock(&stream_lock);
}
static void start_stream(Cpu *c) { stream_set_started(arg_x(c, 0), 1), ret_x(c, 0); }   // Result StartOutputStream
static void stop_stream(Cpu *c) { stream_set_started(arg_x(c, 0), 0); }                 // void StopOutputStream
static void purge_stream(Cpu *c) {   // PurgeOutputStream: the frames not played yet are dropped
    plat_mutex_lock(&stream_lock);
    OutStream *st = stream_find(arg_x(c, 0));
    if (st) {
        st->consumed = st->written;
        if (st->sdl) SDL_ClearAudioStream(st->sdl);
    }
    plat_mutex_unlock(&stream_lock);
    ret_x(c, 0);
}
static void write_stream(Cpu *c) {   // int WriteOutputStream(OutputStreamType *, const void *, int frames): frames taken
    int frames = (int)arg_x(c, 2);
    uint32_t took = 0;
    plat_mutex_lock(&stream_lock);
    OutStream *st = stream_find(arg_x(c, 0));
    if (st && frames > 0) {
        stream_advance(st);
        uint32_t room = stream_free(st);
        took = (uint32_t)frames < room ? (uint32_t)frames : room;
        if (took && st->sdl) {
            const void *src = audren_guest(arg_x(c, 1), (uint64_t)took * st->frame_bytes);
            SDL_SetAudioStreamGain(st->sdl, st->gain * audio_device_volume() * audio_output_gain());
            if (!SDL_PutAudioStreamData(st->sdl, src, (int)(took * st->frame_bytes))) took = 0;
        }
        // the browser stream's sound (stream/stream_win.c): a copy into its ring, encoded on its own thread
        if (took && st->fmt == SDL_AUDIO_F32LE && webplay_audio_wanted())
            webplay_push_audio(audren_guest(arg_x(c, 1), (uint64_t)took * st->frame_bytes), took, (int)st->channels,
                              (int)st->rate);
        st->written += took;
    }
    plat_mutex_unlock(&stream_lock);
    ret_x(c, took);
}
static void free_frames(Cpu *c) {   // int GetOutputStreamFreeFrameCount(const OutputStreamType *)
    uint32_t n = 0;
    plat_mutex_lock(&stream_lock);
    OutStream *st = stream_find(arg_x(c, 0));
    if (st) {
        stream_advance(st);
        n = stream_free(st);
    }
    plat_mutex_unlock(&stream_lock);
    ret_x(c, n);
}
static void stream_state(Cpu *c) {   // OutputStreamState GetOutputStreamState(const OutputStreamType *): 1 stopped, 2 started
    plat_mutex_lock(&stream_lock);
    OutStream *st = stream_find(arg_x(c, 0));
    uint64_t state = st ? (st->started ? 2 : 1) : 0;
    plat_mutex_unlock(&stream_lock);
    ret_x(c, state);
}
static void stream_statistics(Cpu *c) {   // StreamStatistics GetOutputStreamStatistics(OutputStreamType *, bool): 24 bytes at x8
    uint64_t *out = (uint64_t *)(uintptr_t)c->x[8];
    if (out) out[0] = out[1] = out[2] = 0;
}
// void SetOutputStreamGain(OutputStreamType *, float gain, TimeSpan fade): the fade is not ramped (the gain applies from the
// next write, a few ms later).
static void stream_gain(Cpu *c) {
    float g = arg_s(c, 0);
    plat_mutex_lock(&stream_lock);
    OutStream *st = stream_find(arg_x(c, 0));
    if (st) st->gain = isfinite(g) && g >= 0 ? g : 1.0f;
    plat_mutex_unlock(&stream_lock);
}

HOST_OVERRIDE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_"
              "12StreamFormatEiiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE",
              open_stream_event);
HOST_OVERRIDE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiNS_"
              "4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE",
              open_stream);
HOST_OVERRIDE("_ZN2nn5audio17CloseOutputStreamEPNS0_16OutputStreamTypeE", close_stream);
HOST_OVERRIDE("_ZN2nn5audio17StartOutputStreamEPNS0_16OutputStreamTypeE", start_stream);
HOST_OVERRIDE("_ZN2nn5audio16StopOutputStreamEPNS0_16OutputStreamTypeE", stop_stream);
HOST_OVERRIDE("_ZN2nn5audio17PurgeOutputStreamEPNS0_16OutputStreamTypeE", purge_stream);
HOST_OVERRIDE("_ZN2nn5audio17WriteOutputStreamEPNS0_16OutputStreamTypeEPKvi", write_stream);
HOST_OVERRIDE("_ZN2nn5audio29GetOutputStreamFreeFrameCountEPKNS0_16OutputStreamTypeE", free_frames);
HOST_OVERRIDE("_ZN2nn5audio20GetOutputStreamStateEPKNS0_16OutputStreamTypeE", stream_state);
HOST_OVERRIDE("_ZN2nn5audio25GetOutputStreamStatisticsEPNS0_16OutputStreamTypeEb", stream_statistics);
HOST_OVERRIDE("_ZN2nn5audio19SetOutputStreamGainEPNS0_16OutputStreamTypeEfNS_8TimeSpanE", stream_gain);
