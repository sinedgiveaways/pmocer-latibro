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

enum { WEBPLAY_OFF, WEBPLAY_STARTING, WEBPLAY_READY, WEBPLAY_FAILED };

// The game asked for online play (ov_online_call): starts the stream if it is off, else copies its link again. One
// start per burst of such calls (the game makes several in a row). Any thread.
void webplay_online(const char *what);
// The menu's Start / copy the link (1: started or copied) and Stop. Any thread.
int webplay_start(void);
void webplay_stop(void);
int webplay_state(void);                     // STREAM_*
int webplay_clients(void);                   // browsers connected: 0 or 1 (player 2)
void webplay_link(char *out, size_t cap);    // the tunnel's https address, "" before it is known
const char *webplay_reason(void);            // why it failed (English, for the log and the menu), "" otherwise

// The present path (gfx/nvn/host/gfx_present.c): 1 while a browser is connected, with the picture size it wants;
// each captured frame is handed over with webplay_push_video (bytes in B, G, R, A order, or R, G, B, A with rgba set).
// The copy is all the caller pays: the conversion and VP8 run on the stream's own thread.
int webplay_video_wanted(int *w, int *h);
void webplay_push_video(const uint8_t *px, int w, int h, int stride, int rgba);
// The game's output stream (audio/audren_sf.c): interleaved float samples; only 48 kHz is sent (Opus, stereo).
int webplay_audio_wanted(void);
void webplay_push_audio(const float *pcm, uint32_t frames, int channels, int rate);

#ifdef __cplusplus
}
#endif
