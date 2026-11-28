// Online play in a browser (stream.h), Windows: an HTTP + WebSocket server on 127.0.0.1, published through a
// Cloudflare quick tunnel (cloudflared, https://<random>.trycloudflare.com: the random address is the room's secret),
// the game's picture as VP8 (libvpx) and its sound as Opus, both decoded by the browser's WebCodecs, and the browser's
// gamepad, keyboard or touch controls as player 2 (input_remote_pad).
//
// Adapted from MTF Recomp's runtime/stream_host.c (https://github.com/rafael-fj/mtf-recompiled @ b5ff778, MIT,
// Copyright (c) 2026 Kayo Chiaradia and the MTF Recomp contributors): the WebSocket handshake (SHA-1, Base64), the
// frame format, the browser page and its input packet, the VP8 and Opus settings and the cloudflared start-up. Kept
// here: Cloudflare only (the owner's choice, 27/11/2026: no Local Host, no WebRTC), VP8 only (no hardware encoder),
// one browser (the game has 2 players), and the conversion and encoding on a thread of their own instead of the
// present thread (AGENTS.md: nothing serial that can run elsewhere).
//
// cloudflared is not shipped: the first start downloads Cloudflare's release pinned below (Apache-2.0) next to the
// executable and checks its SHA-256; one already there is used as it is.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opus/opus.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"
#include "stream/stream.h"
#include "ui/menu/menu.h"
#include "ui_text.h"

// cloudflared 2026.11.2 (GitHub release of 23/11/2026), its sha256 from the release's asset digest
#define CF_URL "https://github.com/cloudflare/cloudflared/releases/download/2026.11.2/cloudflared-windows-amd64.exe"
#define CF_SHA256 "555b46c85582461d189cac30d14ad05480d612613801be9f110b8ce08c5b2113"

#define VIDEO_W 1280
#define VIDEO_H 720
#define VIDEO_KBPS 4500           // MTF's "Balanced" preset
#define VIDEO_MIN_NS 30000000ull  // 30 frames a second at most: the game's own rate
#define OPUS_FRAME 960            // 20 ms at 48 kHz
#define AUDIO_RING (48000 * 2)    // one second of stereo samples

static PlatMutex g_lock = PLAT_MUTEX_INIT;       // state, link, client
static PlatMutex g_send_lock = PLAT_MUTEX_INIT;  // writes to the client's socket
static volatile int g_state = WEBPLAY_OFF;
static char g_link[256], g_reason[200];
static int g_announce;   // copy the link and say so once it is known

static SOCKET g_listen = INVALID_SOCKET;
static int g_port;
static volatile int g_running;
static volatile int g_gen;   // the start a tunnel's output belongs to: an old reader that ends says nothing

static struct {
    SOCKET sock;
    int active, player, need_kf;
    uint64_t last_input_ns;
} C = {INVALID_SOCKET};

static HANDLE g_cf_proc, g_cf_job, g_cf_pipe;

// encoder input, handed over by the present and audio threads
static PlatEvent g_wake;
static int g_wake_init;
static PlatMutex g_in_lock = PLAT_MUTEX_INIT;
static uint8_t *g_vin, *g_vwork;
static size_t g_vcap;
static int g_vin_ready, g_vin_w, g_vin_h, g_vin_stride, g_vin_rgba;
static uint64_t g_last_vid_ns;
static float g_aring[AUDIO_RING];
static uint64_t g_awr, g_ard;

static void set_state(int s, const char *why) {
    plat_mutex_lock(&g_lock);
    g_state = s;
    snprintf(g_reason, sizeof g_reason, "%s", why ? why : "");
    plat_mutex_unlock(&g_lock);
}

static void notice_text(UiTextId id, const char *arg, int secs) {
    char text[512];
    if (arg) snprintf(text, sizeof text, menu_text(id), arg);
    else snprintf(text, sizeof text, "%s", menu_text(id));
    menu_notice(text, secs);
}

static void fail(const char *why) {
    rt_log("stream: %s", why);
    set_state(WEBPLAY_FAILED, why);
    notice_text(UI_NOTICE_STREAM_FAILED, why, 12);
}

// ---- SHA-1 and Base64 for the WebSocket handshake (RFC 6455), from MTF Recomp

static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_digest(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    size_t padded = ((len + 8) / 64 + 1) * 64;
    uint8_t *buf = calloc(1, padded);
    if (!buf) return;
    memcpy(buf, data, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[padded - 1 - i] = (uint8_t)(bits >> (i * 8));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)buf[off + i * 4] << 24) | ((uint32_t)buf[off + i * 4 + 1] << 16) |
                   ((uint32_t)buf[off + i * 4 + 2] << 8) | buf[off + i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999u;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1u;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
            else f = b ^ c ^ d, k = 0xCA62C1D6u;
            uint32_t t = rol32(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol32(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    free(buf);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

static void base64_encode(const uint8_t *in, size_t len, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
        out[j++] = tbl[(v >> 18) & 63];
        out[j++] = tbl[(v >> 12) & 63];
        out[j++] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out[j++] = i + 2 < len ? tbl[v & 63] : '=';
    }
    out[j] = 0;
}

// ---- WebSocket frames

static int send_all(SOCKET s, const uint8_t *buf, size_t len) {
    for (size_t sent = 0; sent < len;) {
        int r = send(s, (const char *)buf + sent, (int)(len - sent), 0);
        if (r <= 0) return -1;
        sent += (size_t)r;
    }
    return 0;
}

static size_t ws_header(uint8_t hdr[10], uint8_t opcode, size_t len) {
    hdr[0] = (uint8_t)(0x80 | (opcode & 0x0f));
    if (len < 126) { hdr[1] = (uint8_t)len; return 2; }
    if (len < 65536) { hdr[1] = 126, hdr[2] = (uint8_t)(len >> 8), hdr[3] = (uint8_t)len; return 4; }
    hdr[1] = 127;
    for (int i = 0; i < 8; i++) hdr[2 + i] = (uint8_t)((uint64_t)len >> ((7 - i) * 8));
    return 10;
}

// A frame to the browser. A video delta frame is dropped when the socket cannot take it at once (a slow link): the
// next frame is then a key frame, so the picture catches up instead of falling behind.
static void ws_send(uint8_t opcode, const uint8_t *payload, size_t len, int droppable) {
    uint8_t hdr[10];
    size_t hlen = ws_header(hdr, opcode, len);
    plat_mutex_lock(&g_send_lock);
    SOCKET s = C.sock;
    if (C.active && s != INVALID_SOCKET) {
        fd_set w;
        struct timeval tv = {0, 0};
        FD_ZERO(&w);
        FD_SET(s, &w);
        if (droppable && select(0, NULL, &w, NULL, &tv) <= 0) C.need_kf = 1;
        else if (send_all(s, hdr, hlen) < 0 || (len && send_all(s, payload, len) < 0)) C.active = 0;
    }
    plat_mutex_unlock(&g_send_lock);
}

// ---- the page the friend opens (MTF Recomp's, VP8 and the WebSocket only)
static const char k_page[] =
"<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
"<title>Orbitals Together</title><style>"
"*{box-sizing:border-box;margin:0;padding:0;font-family:system-ui,sans-serif;-webkit-tap-highlight-color:transparent;user-select:none}"
"body{background:#0b0e14;color:#e6edf3;display:flex;flex-direction:column;height:100vh;height:100dvh;overflow:hidden;touch-action:none}"
"#bar{display:flex;flex-wrap:wrap;align-items:center;gap:8px;padding:6px 12px;background:#131924;border-bottom:1px solid #263042;font-size:13px}"
"#bar button{background:#2563eb;color:#fff;border:0;border-radius:6px;padding:6px 10px;cursor:pointer;font-weight:600;font-size:12px}"
"#bar button.sec{background:#334155}#stat{margin-left:auto;color:#94a3b8;font-size:12px}"
"#stage{flex:1;min-height:0;display:flex;align-items:center;justify-content:center;background:#000;position:relative;overflow:hidden}"
"canvas{width:100%;height:100%;object-fit:contain;background:#000}"
"#hint{position:absolute;bottom:10px;left:12px;background:rgba(0,0,0,.65);padding:6px 10px;border-radius:6px;font-size:12px;color:#cbd5e1;pointer-events:none}"
"#touchUI{position:absolute;inset:0;pointer-events:none;display:none}"
"#tStick{position:absolute;left:22px;bottom:22px;width:136px;height:136px;border-radius:50%;background:rgba(255,255,255,.10);border:2px solid rgba(255,255,255,.28);pointer-events:auto;touch-action:none}"
"#tKnob{position:absolute;left:50%;top:50%;width:54px;height:54px;margin:-27px 0 0 -27px;border-radius:50%;background:rgba(255,255,255,.45);border:2px solid rgba(255,255,255,.8);pointer-events:none}"
"#tBtns{position:absolute;right:20px;bottom:20px;width:152px;height:152px;pointer-events:none}"
".tb{position:absolute;width:50px;height:50px;border-radius:50%;background:rgba(30,41,59,.68);border:2px solid rgba(255,255,255,.42);color:#fff;font-weight:700;font-size:16px;display:flex;align-items:center;justify-content:center;pointer-events:auto;touch-action:none}"
".tb.on,.tsys.on{background:rgba(59,130,246,.85);border-color:#fff}"
"#tbY{left:51px;top:0}#tbX{left:0;top:51px}#tbB{right:0;top:51px}#tbA{left:51px;bottom:0}"
".tsys{position:absolute;top:14px;height:34px;min-width:48px;padding:0 12px;border-radius:17px;background:rgba(30,41,59,.65);border:1.5px solid rgba(255,255,255,.35);color:#fff;font-weight:700;font-size:14px;display:flex;align-items:center;justify-content:center;pointer-events:auto;touch-action:none}"
"#tbL{left:22px}#tbR{right:22px}#tbMin{left:50%;transform:translateX(-58px)}#tbPlus{left:50%;transform:translateX(10px)}"
"</style></head><body>"
"<div id='bar'><strong>Orbitals Together</strong>"
"<button id='btnConn'>Reconnect</button><button id='btnAudio' class='sec'>Sound: off</button>"
"<button id='btnTouch' class='sec'>Touch controls</button><button id='btnFs' class='sec'>Full screen</button>"
"<span id='stat'>Connecting...</span></div>"
"<div id='stage'><canvas id='cv' width='1280' height='720'></canvas>"
"<div id='hint'>Gamepad (automatic) or keyboard: WASD stick, arrows camera, J/K/U/I = A/B/Y/X, Q/E = L/R, Enter = +, Backspace = -</div>"
"<div id='touchUI'><div id='tStick'><div id='tKnob'></div></div><div id='tBtns'>"
"<div id='tbX' class='tb' data-bit='2'>X</div><div id='tbY' class='tb' data-bit='3'>Y</div>"
"<div id='tbA' class='tb' data-bit='0'>A</div><div id='tbB' class='tb' data-bit='1'>B</div></div>"
"<div id='tbL' class='tsys' data-bit='6'>L</div><div id='tbR' class='tsys' data-bit='7'>R</div>"
"<div id='tbMin' class='tsys' data-bit='11'>-</div><div id='tbPlus' class='tsys' data-bit='10'>+</div></div></div>"
"<script>"
"const statEl=document.getElementById('stat'),cv=document.getElementById('cv'),ctx=cv.getContext('2d',{alpha:false,desynchronized:true});"
"let ws=null,vdec=null,adec=null,actx=null,audioOn=false,nextPlay=0,needKf=true,hostLayout=0;const keys={};"
"const XB=[0,1,2,3,10,11,4,5,6,7,9,8,14,12,15,13],NIN=[1,0,3,2,10,11,4,5,6,7,9,8,14,12,15,13];"
"function padMap(gp){return hostLayout===1||(hostLayout===0&&/057e|nintendo|pro controller|joy-con/i.test(gp.id))?NIN:XB;}"
"addEventListener('keydown',e=>{keys[e.code]=1;if(['Space','ArrowUp','ArrowDown','ArrowLeft','ArrowRight'].includes(e.code))e.preventDefault();});"
"addEventListener('keyup',e=>{keys[e.code]=0;});"
"document.getElementById('btnFs').onclick=()=>{const st=document.getElementById('stage');"
"if(!document.fullscreenElement)(st.requestFullscreen||st.webkitRequestFullscreen).call(st);else document.exitFullscreen();};"
"document.getElementById('btnAudio').onclick=async()=>{if(!actx)actx=new AudioContext({sampleRate:48000,latencyHint:'interactive'});"
"if(actx.state==='suspended')await actx.resume();audioOn=!audioOn;"
"document.getElementById('btnAudio').textContent='Sound: '+(audioOn?'on':'off');nextPlay=0;};"
"function initDecoders(){if(vdec)try{vdec.close()}catch(_){}needKf=true;"
"vdec=new VideoDecoder({output:f=>{const w=f.displayWidth,h=f.displayHeight;if(cv.width!==w||cv.height!==h){cv.width=w;cv.height=h;}"
"ctx.drawImage(f,0,0,w,h);f.close();},error:e=>{console.error(e);setTimeout(()=>{initDecoders();if(ws&&ws.readyState===1)ws.send('{\"type\":\"kf\"}');},50);}});"
"vdec.configure({codec:'vp8',optimizeForLatency:true});"
"if(adec)try{adec.close()}catch(_){}"
"if('AudioDecoder'in window){adec=new AudioDecoder({output:ad=>{if(!audioOn||!actx){ad.close();return;}"
"const ch=ad.numberOfChannels,n=ad.numberOfFrames,buf=actx.createBuffer(ch,n,ad.sampleRate);"
"for(let c=0;c<ch;c++){const t=new Float32Array(n);ad.copyTo(t,{planeIndex:c,format:'f32-planar'});buf.copyToChannel(t,c);}"
"ad.close();const src=actx.createBufferSource();src.buffer=buf;src.connect(actx.destination);const now=actx.currentTime;"
"if(nextPlay<now||nextPlay>now+0.12)nextPlay=now+0.015;src.start(nextPlay);nextPlay+=buf.duration;},error:e=>console.error(e)});"
"adec.configure({codec:'opus',sampleRate:48000,numberOfChannels:2});}}"
"function onMedia(ab){const u=new Uint8Array(ab),dv=new DataView(ab),t=u[0];"
"if(t===1){const kf=(u[1]&1)===1,ts=dv.getUint32(6,true)*1000;if(!vdec||vdec.state!=='configured')return;"
"if(needKf&&!kf)return;needKf=false;vdec.decode(new EncodedVideoChunk({type:kf?'key':'delta',timestamp:ts,data:u.subarray(10)}));}"
"else if(t===2&&adec&&adec.state==='configured'&&audioOn){adec.decode(new EncodedAudioChunk({type:'key',timestamp:dv.getUint32(4,true)*1000,data:u.subarray(8)}));}"
"else if(t===3){hostLayout=u[2];statEl.textContent='Connected as player '+(u[1]+1);}"
"else if(t===4){statEl.textContent='The game has no free player: the host must unplug their second controller';}}"
"function connect(){if(ws){ws.close();ws=null;}"
"if(!('VideoDecoder'in window)){statEl.textContent='This browser cannot decode the stream: use Chrome, Edge or Firefox';return;}"
"initDecoders();const cur=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/ws');"
"ws=cur;ws.binaryType='arraybuffer';ws.onopen=()=>{statEl.textContent='Connected, waiting for the picture...';};"
"ws.onclose=e=>{if(ws!==cur)return;statEl.textContent='Disconnected, reconnecting...';setTimeout(()=>{if(ws===cur)connect();},1500);};"
"ws.onmessage=ev=>{if(typeof ev.data!=='string')onMedia(ev.data);};}"
"document.getElementById('btnConn').onclick=connect;"
"let tBits=0,tTap=0,tTicks=0,tLx=0,tLy=0,stickPid=null;"
"const touchEl=document.getElementById('touchUI'),hintEl=document.getElementById('hint'),knob=document.getElementById('tKnob'),stickEl=document.getElementById('tStick'),btnT=document.getElementById('btnTouch');"
"const coarse=matchMedia('(pointer:coarse)').matches;let showTouch=coarse;"
"function syncTouch(){touchEl.style.display=showTouch?'block':'none';hintEl.style.display=coarse?'none':'block';btnT.textContent='Touch controls: '+(showTouch?'on':'off');}"
"btnT.onclick=()=>{showTouch=!showTouch;syncTouch();};syncTouch();addEventListener('contextmenu',e=>e.preventDefault());"
"addEventListener('gamepadconnected',()=>{if(showTouch){showTouch=false;syncTouch();}});"
"function updStick(cx,cy){const r=stickEl.getBoundingClientRect(),mx=r.left+r.width/2,my=r.top+r.height/2,rad=r.width*0.42;"
"let dx=cx-mx,dy=cy-my,d=Math.hypot(dx,dy);if(d>rad){dx=dx*rad/d;dy=dy*rad/d;}knob.style.transform='translate('+dx+'px,'+dy+'px)';"
"tLx=(dx/rad*32767)|0;tLy=(-dy/rad*32767)|0;}"
"stickEl.addEventListener('pointerdown',e=>{e.preventDefault();stickPid=e.pointerId;stickEl.setPointerCapture(e.pointerId);updStick(e.clientX,e.clientY);});"
"stickEl.addEventListener('pointermove',e=>{if(e.pointerId===stickPid)updStick(e.clientX,e.clientY);});"
"const endStick=e=>{if(e.pointerId===stickPid){stickPid=null;tLx=tLy=0;knob.style.transform='';}};"
"stickEl.addEventListener('pointerup',endStick);stickEl.addEventListener('pointercancel',endStick);"
"document.querySelectorAll('#touchUI [data-bit]').forEach(el=>{const bit=1<<(+el.dataset.bit),ids=new Set();"
"el.addEventListener('pointerdown',e=>{e.preventDefault();ids.add(e.pointerId);el.setPointerCapture(e.pointerId);tBits|=bit;tTap|=bit;tTicks=3;el.classList.add('on');});"
"const up=e=>{if(ids.delete(e.pointerId)&&!ids.size){tBits&=~bit;el.classList.remove('on');}};"
"el.addEventListener('pointerup',up);el.addEventListener('pointercancel',up);});"
"function readInput(){let b=tBits|tTap,lx=tLx,ly=tLy,rx=0,ry=0;if(tTicks>0&&--tTicks===0)tTap=0;"
"if(keys.KeyJ)b|=1;if(keys.KeyK)b|=2;if(keys.KeyI)b|=4;if(keys.KeyU)b|=8;if(keys.KeyQ)b|=64;if(keys.KeyE)b|=128;"
"if(keys.Digit1)b|=256;if(keys.Digit3)b|=512;if(keys.Enter)b|=1024;if(keys.Backspace)b|=2048;"
"if(keys.KeyA)lx=-32767;if(keys.KeyD)lx=32767;if(keys.KeyW)ly=32767;if(keys.KeyS)ly=-32767;"
"if(keys.ArrowLeft)rx=-32767;if(keys.ArrowRight)rx=32767;if(keys.ArrowUp)ry=32767;if(keys.ArrowDown)ry=-32767;"
"for(const gp of(navigator.getGamepads?navigator.getGamepads():[])){if(!gp)continue;"
"const map=padMap(gp);"
"for(let i=0;i<map.length;i++)if(gp.buttons[map[i]]&&gp.buttons[map[i]].pressed)b|=1<<i;"
"if(gp.axes.length>=2){const x=gp.axes[0],y=-gp.axes[1];if(Math.abs(x)>0.08)lx=(x*32767)|0;if(Math.abs(y)>0.08)ly=(y*32767)|0;}"
"if(gp.axes.length>=4){const x=gp.axes[2],y=-gp.axes[3];if(Math.abs(x)>0.08)rx=(x*32767)|0;if(Math.abs(y)>0.08)ry=(y*32767)|0;}break;}"
"return[b,lx,ly,rx,ry];}"
"setInterval(()=>{if(!ws||ws.readyState!==1)return;const[b,lx,ly,rx,ry]=readInput(),buf=new ArrayBuffer(13),dv=new DataView(buf);"
"dv.setUint8(0,0x11);dv.setUint32(1,b>>>0,true);dv.setInt16(5,lx,true);dv.setInt16(7,ly,true);dv.setInt16(9,rx,true);dv.setInt16(11,ry,true);ws.send(buf);},8);"
"setInterval(()=>{if(ws&&ws.readyState===1)ws.send('{\"type\":\"ping\"}');},25000);connect();"
"</script></body></html>";

// ---- the browser's connection

static int recv_exact(SOCKET s, uint8_t *buf, size_t len) {
    for (size_t got = 0; got < len;) {
        int r = recv(s, (char *)buf + got, (int)(len - got), 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

// The browser's 13-byte packet: 0x11, the buttons in nn::hid's order (input_map.h B_A..B_DOWN), then the two sticks.
static void apply_input(const uint8_t *buf, size_t len) {
    if (len < 13 || buf[0] != 0x11) return;
    uint32_t b;
    int16_t v[4];
    memcpy(&b, buf + 1, 4);
    memcpy(v, buf + 5, 8);
    plat_mutex_lock(&g_lock);
    int p = C.player;
    C.last_input_ns = plat_now_ns();
    plat_mutex_unlock(&g_lock);
    input_remote_pad(p, b & 0xffffu, v[0], v[1], v[2], v[3]);
}

static void client_main(void *arg) {
    (void)arg;
    SOCKET s = C.sock;
    // the host's layout (F1 > Controls > Button layout): 1 Nintendo, 2 Xbox, 0 automatic (the page reads the pad's
    // name: Nintendo's controllers by position, every other one by label, as input.c does for local pads)
    const char *lay = cfg_value("ORBITALS_PAD_LAYOUT");
    uint8_t hello[3] = {3, (uint8_t)C.player, (uint8_t)(!strcmp(lay, "nintendo") ? 1 : !strcmp(lay, "xbox") ? 2 : 0)};
    ws_send(0x2, hello, sizeof hello, 0);
    uint8_t *buf = malloc(8192);
    while (buf && g_running && C.active) {
        uint8_t h[2], mask[4] = {0};
        if (recv_exact(s, h, 2) < 0) break;
        uint8_t op = h[0] & 0x0f;
        uint64_t len = h[1] & 0x7f;
        if (len == 126) {
            uint8_t e[2];
            if (recv_exact(s, e, 2) < 0) break;
            len = (uint64_t)e[0] << 8 | e[1];
        } else if (len == 127) {
            uint8_t e[8];
            if (recv_exact(s, e, 8) < 0) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = len << 8 | e[i];
        }
        if ((h[1] & 0x80) && recv_exact(s, mask, 4) < 0) break;
        if (len >= 8192 || (len && recv_exact(s, buf, (size_t)len) < 0)) break;
        for (uint64_t i = 0; i < len; i++) buf[i] ^= mask[i & 3];
        buf[len] = 0;
        if (op == 0x8) break;
        if (op == 0x9) { ws_send(0xA, buf, (size_t)len, 0); continue; }
        if (op == 0x1) {
            if (strstr((const char *)buf, "\"kf\"")) C.need_kf = 1;
            continue;
        }
        if (op == 0x2 || op == 0x0) apply_input(buf, (size_t)len);
    }
    free(buf);
    plat_mutex_lock(&g_send_lock);
    closesocket(C.sock);
    C.sock = INVALID_SOCKET;
    C.active = 0;
    plat_mutex_unlock(&g_send_lock);
    input_remote_detach(C.player);
    rt_log("stream: the browser of player %d left", C.player + 1);
    notice_text(UI_NOTICE_STREAM_LEFT, NULL, 5);
}

static const char *find_ci(const char *hay, const char *needle) {
    for (size_t n = strlen(needle); *hay; hay++)
        if (!_strnicmp(hay, needle, n)) return hay;
    return NULL;
}

static void reply(SOCKET s, const char *status, const char *type, const char *body, size_t len) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n", status, type, len);
    send_all(s, (const uint8_t *)hdr, (size_t)n);
    if (len) send_all(s, (const uint8_t *)body, len);
    closesocket(s);
}

static void handle_conn(SOCKET s) {
    DWORD to = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
    char req[4096];
    int total = 0;
    while (total < (int)sizeof req - 1) {
        int n = recv(s, req + total, (int)sizeof req - 1 - total, 0);
        if (n <= 0) break;
        req[total += n] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (total <= 0) { closesocket(s); return; }
    if (!find_ci(req, "Upgrade: websocket")) {
        reply(s, "200 OK", "text/html; charset=utf-8", k_page, sizeof k_page - 1);
        return;
    }
    const char *k = find_ci(req, "Sec-WebSocket-Key:");
    if (!k) { closesocket(s); return; }
    k += 18;
    while (*k == ' ' || *k == '\t') k++;
    char key[128];
    size_t ki = 0;
    while (k[ki] && k[ki] != '\r' && k[ki] != '\n' && ki < 64) key[ki] = k[ki], ki++;
    snprintf(key + ki, sizeof key - ki, "%s", "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    uint8_t digest[20];
    char accept[64];
    sha1_digest((const uint8_t *)key, strlen(key), digest);
    base64_encode(digest, 20, accept);
    to = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
    to = 1500;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    if (C.active) { reply(s, "503 Busy", "text/plain", "A friend is already connected", 29); return; }
    char resp[256];
    int rl = snprintf(resp, sizeof resp, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    send_all(s, (const uint8_t *)resp, (size_t)rl);
    int p = input_remote_attach();   // player 2, unless a second controller already is
    if (p < 0) {
        uint8_t full[2] = {4, 0};
        uint8_t hdr[10];
        size_t hl = ws_header(hdr, 0x2, sizeof full);
        send_all(s, hdr, hl);
        send_all(s, full, sizeof full);
        closesocket(s);
        rt_log("stream: a browser connected, but the game has no free player (a second controller is plugged in)");
        notice_text(UI_NOTICE_STREAM_FULL, NULL, 8);
        return;
    }
    plat_mutex_lock(&g_send_lock);
    C.sock = s, C.active = 1, C.player = p, C.need_kf = 1, C.last_input_ns = plat_now_ns();
    plat_mutex_unlock(&g_send_lock);
    rt_log("stream: a browser joined as player %d", p + 1);
    notice_text(UI_NOTICE_STREAM_JOINED, NULL, 6);
    if (!plat_thread_spawn(client_main, NULL, "stream client", 0)) {
        closesocket(s);
        C.active = 0;
        input_remote_detach(p);
    }
}

static void server_main(void *arg) {
    (void)arg;
    while (g_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_listen, &fds);
        struct timeval tv = {0, 100000};
        if (select(0, &fds, NULL, NULL, &tv) > 0) {
            SOCKET c = accept(g_listen, NULL, NULL);
            if (c != INVALID_SOCKET) handle_conn(c);
        }
    }
}

// ---- the encoder: the newest picture as VP8, the sound as 20 ms Opus frames

static void to_i420(const uint8_t *px, int stride, int w, int h, int rgba, vpx_image_t *img) {
    int ri = rgba ? 0 : 2, bi = rgba ? 2 : 0;
    uint8_t *Y = img->planes[VPX_PLANE_Y], *U = img->planes[VPX_PLANE_U], *V = img->planes[VPX_PLANE_V];
    for (int y = 0; y < h; y++) {
        const uint8_t *row = px + (size_t)y * stride;
        uint8_t *yr = Y + (size_t)y * img->stride[VPX_PLANE_Y];
        for (int x = 0; x < w; x++) {   // BT.601 full range, as MTF Recomp's
            int r = row[x * 4 + ri], g = row[x * 4 + 1], b = row[x * 4 + bi];
            yr[x] = (uint8_t)((77 * r + 150 * g + 29 * b + 128) >> 8);
        }
        if ((y & 1) || y + 1 >= h) continue;
        const uint8_t *r2 = row + stride;
        uint8_t *ur = U + (size_t)(y >> 1) * img->stride[VPX_PLANE_U], *vr = V + (size_t)(y >> 1) * img->stride[VPX_PLANE_V];
        for (int x = 0; x + 1 < w; x += 2) {
            int r = (row[x * 4 + ri] + row[x * 4 + 4 + ri] + r2[x * 4 + ri] + r2[x * 4 + 4 + ri]) >> 2;
            int g = (row[x * 4 + 1] + row[x * 4 + 5] + r2[x * 4 + 1] + r2[x * 4 + 5]) >> 2;
            int b = (row[x * 4 + bi] + row[x * 4 + 4 + bi] + r2[x * 4 + bi] + r2[x * 4 + 4 + bi]) >> 2;
            int u = ((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128, v = ((128 * r - 107 * g - 21 * b + 128) >> 8) + 128;
            ur[x >> 1] = (uint8_t)(u < 0 ? 0 : u > 255 ? 255 : u);
            vr[x >> 1] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
}

static void send_video(int kf, int w, int h, const uint8_t *data, size_t sz) {
    uint8_t *msg = malloc(10 + sz);
    if (!msg) return;
    uint16_t ww = (uint16_t)w, hh = (uint16_t)h;
    uint32_t ts = (uint32_t)(plat_now_ns() / 1000000ull);
    msg[0] = 1, msg[1] = (uint8_t)(kf ? 1 : 0);   // codec 0: VP8
    memcpy(msg + 2, &ww, 2), memcpy(msg + 4, &hh, 2), memcpy(msg + 6, &ts, 4), memcpy(msg + 10, data, sz);
    ws_send(0x2, msg, 10 + sz, !kf);
    free(msg);
}

static void encoder_main(void *arg) {
    (void)arg;
    vpx_codec_ctx_t vpx;
    vpx_image_t img;
    int vpx_ok = 0, vw = 0, vh = 0;
    uint64_t pts = 0;
    int err = 0;
    OpusEncoder *opus = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (err != OPUS_OK) opus = NULL, rt_log("stream: no Opus encoder (%d): the stream has no sound", err);
    float frame[OPUS_FRAME * 2];
    while (g_running) {
        plat_event_wait_ns(&g_wake, 10000000ull);
        // sound: every whole 20 ms the ring holds
        for (;;) {
            plat_mutex_lock(&g_in_lock);
            int have = g_awr - g_ard >= OPUS_FRAME * 2;
            if (have)
                for (int i = 0; i < OPUS_FRAME * 2; i++) frame[i] = g_aring[(g_ard + i) % AUDIO_RING];
            if (have) g_ard += OPUS_FRAME * 2;
            plat_mutex_unlock(&g_in_lock);
            if (!have) break;
            if (!opus || !C.active) continue;
            uint8_t out[1500];
            int n = opus_encode_float(opus, frame, OPUS_FRAME, out + 8, (opus_int32)sizeof out - 8);
            if (n <= 0) continue;
            uint16_t r100 = 480;
            uint32_t ts = (uint32_t)(plat_now_ns() / 1000000ull);
            out[0] = 2, out[3] = 2;
            memcpy(out + 1, &r100, 2), memcpy(out + 4, &ts, 4);
            ws_send(0x2, out, (size_t)n + 8, 0);
        }
        // picture: the newest one handed over
        plat_mutex_lock(&g_in_lock);
        int ready = g_vin_ready, w = g_vin_w, h = g_vin_h, stride = g_vin_stride, rgba = g_vin_rgba;
        if (ready) {
            uint8_t *t = g_vin;
            g_vin = g_vwork, g_vwork = t, g_vin_ready = 0;
        }
        plat_mutex_unlock(&g_in_lock);
        if (!ready || !C.active) continue;
        if (!vpx_ok || vw != w || vh != h) {
            if (vpx_ok) vpx_codec_destroy(&vpx), vpx_img_free(&img), vpx_ok = 0;
            vpx_codec_enc_cfg_t cfg;
            if (vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &cfg, 0) == VPX_CODEC_OK) {
                cfg.g_w = (unsigned)w, cfg.g_h = (unsigned)h;
                cfg.g_timebase.num = 1, cfg.g_timebase.den = 30;
                cfg.rc_target_bitrate = VIDEO_KBPS;
                cfg.rc_min_quantizer = 4, cfg.rc_max_quantizer = 48;
                cfg.rc_end_usage = VPX_CBR;
                cfg.rc_buf_sz = 120, cfg.rc_buf_initial_sz = 60, cfg.rc_buf_optimal_sz = 60;
                cfg.g_lag_in_frames = 0;
                cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
                cfg.kf_mode = VPX_KF_AUTO, cfg.kf_max_dist = 60;
                cfg.g_threads = 4;
                if (vpx_codec_enc_init(&vpx, vpx_codec_vp8_cx(), &cfg, 0) == VPX_CODEC_OK) {
                    if (vpx_img_alloc(&img, VPX_IMG_FMT_I420, (unsigned)w, (unsigned)h, 1)) {
                        vpx_codec_control(&vpx, VP8E_SET_CPUUSED, -8);
                        vpx_ok = 1, vw = w, vh = h, pts = 0;
                        rt_log("stream: VP8 %dx%d at %d kb/s", w, h, VIDEO_KBPS);
                    } else vpx_codec_destroy(&vpx);
                }
            }
            if (!vpx_ok) continue;
        }
        to_i420(g_vwork, stride, w, h, rgba, &img);
        int flags = 0;
        if (C.need_kf) flags |= VPX_EFLAG_FORCE_KF, C.need_kf = 0;
        if (vpx_codec_encode(&vpx, &img, (vpx_codec_pts_t)pts++, 1, flags, VPX_DL_REALTIME) != VPX_CODEC_OK) continue;
        vpx_codec_iter_t it = NULL;
        const vpx_codec_cx_pkt_t *pkt;
        while ((pkt = vpx_codec_get_cx_data(&vpx, &it)))
            if (pkt->kind == VPX_CODEC_CX_FRAME_PKT)
                send_video(!!(pkt->data.frame.flags & VPX_FRAME_IS_KEY), w, h, pkt->data.frame.buf, pkt->data.frame.sz);
    }
    if (vpx_ok) vpx_codec_destroy(&vpx), vpx_img_free(&img);
    if (opus) opus_encoder_destroy(opus);
}

int webplay_video_wanted(int *w, int *h) {
    if (!g_running || !C.active) return 0;
    *w = VIDEO_W, *h = VIDEO_H;
    return 1;
}

void webplay_push_video(const uint8_t *px, int w, int h, int stride, int rgba) {
    if (!g_running || !C.active || !px || w <= 0 || h <= 0) return;
    uint64_t now = plat_now_ns();
    if (now - g_last_vid_ns < VIDEO_MIN_NS) return;
    g_last_vid_ns = now;
    size_t need = (size_t)stride * h;
    plat_mutex_lock(&g_in_lock);
    if (g_vcap < need) {
        uint8_t *a = realloc(g_vin, need), *b = a ? realloc(g_vwork, need) : NULL;
        if (a) g_vin = a;
        if (b) g_vwork = b, g_vcap = need;
    }
    if (g_vcap >= need) {
        memcpy(g_vin, px, need);
        g_vin_ready = 1, g_vin_w = w, g_vin_h = h, g_vin_stride = stride, g_vin_rgba = rgba;
    }
    plat_mutex_unlock(&g_in_lock);
    plat_event_set(&g_wake);
}

int webplay_audio_wanted(void) { return g_running && C.active; }

void webplay_push_audio(const float *pcm, uint32_t frames, int channels, int rate) {
    if (!g_running || !C.active || !pcm || channels < 1 || rate != 48000) return;
    plat_mutex_lock(&g_in_lock);
    if (g_awr - g_ard + (uint64_t)frames * 2 > AUDIO_RING) g_ard = g_awr;   // the encoder fell behind: start over
    for (uint32_t i = 0; i < frames; i++) {
        float l = pcm[(size_t)i * channels], r = channels > 1 ? pcm[(size_t)i * channels + 1] : l;
        g_aring[g_awr++ % AUDIO_RING] = l;
        g_aring[g_awr++ % AUDIO_RING] = r;
    }
    plat_mutex_unlock(&g_in_lock);
}

// ---- cloudflared: found or downloaded next to the executable, started, its https address read from its output

static int to_wide(const char *s, wchar_t *w, int cap) { return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap) > 0; }

static int exe_dir(char *out, size_t cap) {
    if (!plat_exe_path(out, cap)) return 0;
    char *cut = plat_last_sep(out);
    if (cut) *cut = 0;
    return 1;
}

static int sha256_file(const wchar_t *path, char hex[65]) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hh = NULL;
    int ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
             BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0) == 0;
    static uint8_t buf[1 << 16];
    DWORD n;
    while (ok && ReadFile(f, buf, sizeof buf, &n, NULL) && n) ok = BCryptHashData(hh, buf, n, 0) == 0;
    uint8_t d[32];
    ok = ok && BCryptFinishHash(hh, d, 32, 0) == 0;
    if (hh) BCryptDestroyHash(hh);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    for (int i = 0; ok && i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
    return ok;
}

// curl.exe ships with Windows 10 (1803) and 11; a hidden child with no console window.
static int run_hidden(wchar_t *cmd, DWORD wait_ms, DWORD *code) {
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESHOWWINDOW, si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return 0;
    WaitForSingleObject(pi.hProcess, wait_ms);
    GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return 1;
}

static int ensure_cloudflared(wchar_t *exe, size_t cap) {
    char dir[1024];
    wchar_t wdir[1024], tmp[1100], cmd[2600];
    if (!exe_dir(dir, sizeof dir) || !to_wide(dir, wdir, 1024)) { fail("the program's folder is unknown"); return 0; }
    _snwprintf(exe, cap, L"%ls\\cloudflared.exe", wdir);
    exe[cap - 1] = 0;
    if (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES) return 1;
    rt_log("stream: downloading cloudflared (%s)", CF_URL);
    _snwprintf(tmp, 1100, L"%ls\\cloudflared.exe.part", wdir);
    tmp[1099] = 0;
    DeleteFileW(tmp);
    _snwprintf(cmd, 2600, L"curl.exe -fsSL --retry 2 -o \"%ls\" \"" CF_URL "\"", tmp);
    cmd[2599] = 0;
    DWORD code = 1;
    if (!run_hidden(cmd, 300000, &code) || code) {
        DeleteFileW(tmp);
        fail(code == 1 ? "curl.exe is missing" : "cloudflared could not be downloaded (no internet?)");
        return 0;
    }
    char hex[65] = "";
    if (!sha256_file(tmp, hex) || strcmp(hex, CF_SHA256)) {
        DeleteFileW(tmp);
        rt_log("stream: cloudflared came with sha256 %s, not %s: refused", hex, CF_SHA256);
        fail("cloudflared came damaged: try again");
        return 0;
    }
    if (!MoveFileExW(tmp, exe, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp);
        fail("cloudflared could not be saved next to the program");
        return 0;
    }
    rt_log("stream: cloudflared 2026.11.2 saved next to the program (sha256 checked)");
    return 1;
}

static void tunnel_reader(void *arg) {
    int gen = (int)(intptr_t)arg;
    char buf[512], line[1024];
    size_t li = 0;
    DWORD n;
    while (g_running && ReadFile(g_cf_pipe, buf, sizeof buf, &n, NULL) && n) {
        for (DWORD i = 0; i < n; i++) {
            if (buf[i] != '\n' && buf[i] != '\r' && li < sizeof line - 1) { line[li++] = buf[i]; continue; }
            line[li] = 0, li = 0;
            const char *u = strstr(line, "https://"), *e = u ? strstr(u, ".trycloudflare.com") : NULL;
            if (!e || e - u > 200) continue;
            plat_mutex_lock(&g_lock);
            int first = !g_link[0];
            snprintf(g_link, sizeof g_link, "%.*s", (int)(e + 18 - u), u);
            g_state = WEBPLAY_READY;
            g_reason[0] = 0;
            int announce = first && g_announce;
            char link[256];
            snprintf(link, sizeof link, "%s", g_link);
            plat_mutex_unlock(&g_lock);
            if (!first) continue;
            rt_log("stream: link ready: %s", link);
            if (announce) {
                plat_clipboard_set(link);
                notice_text(UI_NOTICE_STREAM_LINK, link, 20);
            }
        }
    }
    if (g_running && gen == g_gen) fail("the Cloudflare tunnel stopped");
}

static int start_tunnel(const wchar_t *cf) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE r = NULL, w = NULL;
    if (!CreatePipe(&r, &w, &sa, 0)) return 0;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    // a job that kills cloudflared with the game, however the game ends
    g_cf_job = CreateJobObjectW(NULL, NULL);
    if (g_cf_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION j = {0};
        j.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_cf_job, JobObjectExtendedLimitInformation, &j, sizeof j);
    }
    wchar_t cmd[1400];
    _snwprintf(cmd, 1400, L"\"%ls\" tunnel --no-autoupdate --url http://127.0.0.1:%d", cf, g_port);
    cmd[1399] = 0;
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW, si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = w;
    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(w);
    if (!ok) {
        CloseHandle(r);
        return 0;
    }
    if (g_cf_job) AssignProcessToJobObject(g_cf_job, pi.hProcess);
    CloseHandle(pi.hThread);
    g_cf_proc = pi.hProcess, g_cf_pipe = r;
    return plat_thread_spawn(tunnel_reader, (void *)(intptr_t)g_gen, "stream tunnel", 0);
}

static int open_server(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 0;
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return 0;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // only cloudflared reaches it
    a.sin_port = 0;                               // any free port: cloudflared is told which
    int len = sizeof a;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || getsockname(ls, (struct sockaddr *)&a, &len) || listen(ls, 8)) {
        closesocket(ls);
        return 0;
    }
    g_port = ntohs(a.sin_port);
    g_listen = ls;
    return 1;
}

static void start_main(void *arg) {
    (void)arg;
    wchar_t cf[1100];
    if (!ensure_cloudflared(cf, 1100)) return;
    if (!open_server()) { fail("no local port for the stream"); return; }
    g_running = 1;
    if (!plat_thread_spawn(server_main, NULL, "stream server", 0) ||
        !plat_thread_spawn(encoder_main, NULL, "stream encoder", 0) || !start_tunnel(cf)) {
        g_running = 0;
        fail("cloudflared could not be started");
        return;
    }
    rt_log("stream: server on 127.0.0.1:%d, waiting for the Cloudflare link", g_port);
}

int webplay_start(void) {
    plat_mutex_lock(&g_lock);
    int s = g_state;
    char link[256];
    snprintf(link, sizeof link, "%s", g_link);
    if (s == WEBPLAY_STARTING) { plat_mutex_unlock(&g_lock); return 1; }
    if (s == WEBPLAY_READY) {
        plat_mutex_unlock(&g_lock);
        plat_clipboard_set(link);
        notice_text(UI_NOTICE_STREAM_LINK, link, 20);
        return 1;
    }
    g_state = WEBPLAY_STARTING, g_reason[0] = 0, g_link[0] = 0, g_announce = 1, g_gen++;
    if (!g_wake_init) plat_event_init(&g_wake, 0, 0), g_wake_init = 1;
    plat_mutex_unlock(&g_lock);
    notice_text(UI_NOTICE_STREAM_STARTING, NULL, 8);
    if (!plat_thread_spawn(start_main, NULL, "stream start", 0)) { fail("no thread for the stream"); return 0; }
    return 1;
}

void webplay_stop(void) {
    if (g_state == WEBPLAY_OFF) return;
    g_running = 0;
    if (g_listen != INVALID_SOCKET) closesocket(g_listen), g_listen = INVALID_SOCKET;
    plat_mutex_lock(&g_send_lock);
    if (C.sock != INVALID_SOCKET) closesocket(C.sock), C.sock = INVALID_SOCKET;   // client_main ends and frees player 2
    plat_mutex_unlock(&g_send_lock);
    if (g_cf_proc) TerminateProcess(g_cf_proc, 0), CloseHandle(g_cf_proc), g_cf_proc = NULL;
    if (g_cf_job) CloseHandle(g_cf_job), g_cf_job = NULL;
    if (g_cf_pipe) CloseHandle(g_cf_pipe), g_cf_pipe = NULL;
    if (g_wake_init) plat_event_set(&g_wake);
    set_state(WEBPLAY_OFF, NULL);
    plat_mutex_lock(&g_lock);
    g_link[0] = 0;
    plat_mutex_unlock(&g_lock);
    rt_log("stream: stopped");
}

// One start, or one copy of the link, per burst of online calls: the game makes several in a row.
void webplay_online(const char *what) {
    static volatile int32_t last_s;
    int32_t now_s = (int32_t)(plat_now_ns() / 1000000000ull), was = plat_cas32(&last_s, 0, 0);
    if (was && now_s - was < 10) return;
    plat_xchg32(&last_s, now_s ? now_s : 1);
    rt_log("stream: the game asked for online play (%s): the browser stream", what);
    webplay_start();
}

int webplay_state(void) { return g_state; }
int webplay_clients(void) { return g_running && C.active ? 1 : 0; }

void webplay_link(char *out, size_t cap) {
    plat_mutex_lock(&g_lock);
    snprintf(out, cap, "%s", g_link);
    plat_mutex_unlock(&g_lock);
}

const char *webplay_reason(void) { return g_reason; }
