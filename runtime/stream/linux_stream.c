// Online play in a browser (stream.h) on Linux: not built yet (the Windows version, stream_win.c, uses Winsock,
// Windows processes and BCrypt). Online Play says so; the game still offers Local Play and Steam Remote Play Together.
#include "host.h"
#include "stream/stream.h"
#include "ui/menu/menu.h"
#include "ui_text.h"

#include <stdio.h>

static const char kWhy[] = "the browser stream is Windows only for now";

void stream_online(const char *what) {
    rt_log("stream: the game asked for online play (%s): %s", what, kWhy);
    stream_start();
}

int stream_start(void) {
    char text[384];
    snprintf(text, sizeof text, menu_text(UI_NOTICE_STREAM_FAILED), kWhy);
    menu_notice(text, 10);
    return 0;
}

void stream_stop(void) {}
int stream_state(void) { return STREAM_FAILED; }
int stream_clients(void) { return 0; }
void stream_link(char *out, size_t cap) { if (cap) out[0] = 0; }
const char *stream_reason(void) { return kWhy; }
int stream_video_wanted(int *w, int *h) { (void)w, (void)h; return 0; }
void stream_push_video(const uint8_t *px, int w, int h, int stride, int rgba) { (void)px, (void)w, (void)h, (void)stride, (void)rgba; }
int stream_audio_wanted(void) { return 0; }
void stream_push_audio(const float *pcm, uint32_t frames, int channels, int rate) { (void)pcm, (void)frames, (void)channels, (void)rate; }
