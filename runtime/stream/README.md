# runtime/stream

Online play in a browser: the game's Online Play entry (and F1 > Game > Online play in a browser) streams the picture
and the sound to a friend's browser through a Cloudflare quick tunnel, and the friend's gamepad, keyboard or touch
controls drive player 2. The friend needs only a recent browser (Chrome, Edge or Firefox: WebCodecs); the host needs
nothing set up: the first start downloads `cloudflared` next to the executable. Steam Remote Play Together
([../steam/](../steam/README.md)) stays for Local Play. The owner's choice of 27/11/2026; how to use it:
[docs/remote-play.md](../../docs/remote-play.md#online-play-in-a-browser).

| File | What it is |
|---|---|
| `stream.h` | The interface: start, stop, state and link for the menu; the hand-over of frames (gfx_present.c) and samples (audren_sf.c) |
| `stream_win.c` | Windows: HTTP and WebSocket server on 127.0.0.1, `cloudflared` (pinned 2026.11.2, sha256 checked) and its `trycloudflare.com` address, VP8 (libvpx) and Opus on a thread of their own, the browser page and its input as player 2 (`input_remote_pad`). Adapted from MTF Recomp (MIT) |
| `linux_stream.c` | Linux: Online Play says the browser stream is Windows only |

How it flows:

- Online Play calls `nifm::HandleNetworkRequestErrorResult` (sdk/ov/ov_net.c): `stream_online` starts the stream
  once per burst. When `cloudflared` prints its address, the link goes to the clipboard and a notice shows it.
- The friend opens the link: the page connects back over a WebSocket; `input_remote_attach` gives them player 2,
  or the notice says a second controller already has it.
- While the browser is connected, each present blits the swapchain image to 1280 x 720 and reads it back
  (`gfx_present.c`); `stream_push_video` copies the newest one for the encoder thread (30 per second at most). The
  game's output stream hands its float samples over (`stream_push_audio`). VP8 at 4.5 Mb/s, Opus 20 ms frames.
- The address is the room's secret: anyone with it can join as player 2 while the stream runs. Stop ends it.
