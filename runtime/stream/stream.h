// Online play in a browser (runtime/stream/stream_win.c): the game's picture and sound stream to a friend's browser
// through a Cloudflare quick tunnel, and the browser's gamepad, keyboard or touch controls drive player 2. The game's
// Online Play entry starts it (sdk/ov/ov_net.c); Steam Remote Play Together stays for Local Play (steam/steam_rpt.c).
// Windows only; elsewhere (linux_stream.c) every call says it is off.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { STREAM_OFF, STREAM_STARTING, STREAM_READY, STREAM_FAILED };

// The game asked for online play (ov_online_call): starts the stream if it is off, else copies its link again. One
// start per burst of such calls (the game makes several in a row). Any thread.
void stream_online(const char *what);
// The menu's Start / copy the link (1: started or copied) and Stop. Any thread.
int stream_start(void);
void stream_stop(void);
int stream_state(void);                     // STREAM_*
int stream_clients(void);                   // browsers connected: 0 or 1 (player 2)
void stream_link(char *out, size_t cap);    // the tunnel's https address, "" before it is known
const char *stream_reason(void);            // why it failed (English, for the log and the menu), "" otherwise

// The present path (gfx/nvn/host/gfx_present.c): 1 while a browser is connected, with the picture size it wants;
// each captured frame is handed over with stream_push_video (bytes in B, G, R, A order, or R, G, B, A with rgba set).
// The copy is all the caller pays: the conversion and VP8 run on the stream's own thread.
int stream_video_wanted(int *w, int *h);
void stream_push_video(const uint8_t *px, int w, int h, int stride, int rgba);
// The game's output stream (audio/audren_sf.c): interleaved float samples; only 48 kHz is sent (Opus, stereo).
int stream_audio_wanted(void);
void stream_push_audio(const float *pcm, uint32_t frames, int channels, int rate);

#ifdef __cplusplus
}
#endif
