#!/usr/bin/env python3
"""studio_server.py — PCM Studio 控制台(浏览器里看画面 + 点屏 + 拧旋钮 + 喂状态)

跑:
  cc -O2 -o /tmp/pcm_studio_run studio/main_mac.c -lm && /tmp/pcm_studio_run &
  python3 studio/tools/studio_server.py          # 然后浏览器开 http://localhost:8770

机制(零依赖, 纯 stdlib):
  画面: 系统进程每帧写 /tmp/pcm_studio/frame.ppm(原子替换) -> 这里转 PNG 推给网页
  输入: 网页点击/按键 -> POST /ev -> 追加到 /tmp/pcm_studio/events -> 系统进程读走
  状态: 网页面板 -> POST /state -> 写 /tmp/pcm_studio/state.json -> 系统进程读
"""
import http.server, socketserver, json, os, struct, zlib, io

RUN = "/tmp/pcm_studio"
PORT = 8770

# 事件类型(与 pcm_sys.h 对齐)
EV_ROTARY, EV_KEY_DOWN, EV_KEY_UP, EV_TOUCH_DOWN = 1, 2, 3, 4

def ppm_to_png():
    try:
        d = open(os.path.join(RUN, "frame.ppm"), "rb").read()
    except Exception:
        return None
    if not d.startswith(b"P6"):
        return None
    try:
        parts = d.split(b"\n", 3)
        w, h = map(int, parts[1].split())
        rgb = parts[3][:w*h*3]
        if len(rgb) < w*h*3:
            return None
    except Exception:
        return None
    raw = b"".join(b"\x00" + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 1))
            + chunk(b"IEND", b""))

def push_event(t, which=0, arg=0, x=0, y=0):
    os.makedirs(RUN, exist_ok=True)
    with open(os.path.join(RUN, "events"), "a") as f:
        f.write("%d %d %d %d %d\n" % (t, which, arg, x, y))

PAGE = r"""<!doctype html><meta charset=utf-8>
<title>PCM Studio</title>
<style>
:root{--bg:#0b0e14;--panel:#161c28;--hair:rgba(255,255,255,.08);--ink:#eef2f7;--ink2:#98a2b3;--amber:#e9b24a;--bt:#3aa0ff}
*{box-sizing:border-box;margin:0;padding:0}
body{background:linear-gradient(160deg,#0a0c11,#12151d);color:var(--ink);min-height:100vh;
  font:14px/1.5 -apple-system,BlinkMacSystemFont,"PingFang SC",sans-serif;padding:24px;
  display:flex;gap:24px;flex-wrap:wrap;justify-content:center}
.left{display:flex;flex-direction:column;gap:14px;align-items:center}
h1{font-size:13px;letter-spacing:.2em;text-transform:uppercase;color:var(--amber);font-weight:600}
#scr{width:800px;max-width:92vw;aspect-ratio:800/480;border-radius:14px;overflow:hidden;cursor:crosshair;
  box-shadow:0 24px 60px -24px #000,0 0 0 1px var(--hair);background:#000;display:block}
.keys{display:flex;gap:8px;flex-wrap:wrap;max-width:800px;justify-content:center}
button{background:var(--panel);color:var(--ink);border:1px solid var(--hair);border-radius:9px;
  padding:9px 14px;cursor:pointer;font:inherit;font-size:13px}
button:hover{border-color:var(--amber);color:var(--amber)}
button:active{transform:translateY(1px)}
.knob{display:flex;align-items:center;gap:10px;background:var(--panel);border:1px solid var(--hair);
  border-radius:99px;padding:7px 8px 7px 16px}
.knob b{font-size:12px;color:var(--ink2);font-weight:500}
.right{width:320px;display:flex;flex-direction:column;gap:12px}
.card{background:var(--panel);border:1px solid var(--hair);border-radius:13px;padding:16px}
.card h2{font-size:11px;letter-spacing:.16em;text-transform:uppercase;color:var(--ink2);margin-bottom:12px;font-weight:600}
label{display:flex;flex-direction:column;gap:5px;font-size:12px;color:var(--ink2);margin-bottom:11px}
input,select{background:#0d121b;border:1px solid var(--hair);border-radius:7px;padding:7px 9px;color:var(--ink);font:inherit;font-size:13px}
input[type=range]{padding:0}
.row{display:flex;gap:8px}.row>*{flex:1}
.hint{font-size:11px;color:#5c6472;line-height:1.6}
kbd{background:#0d121b;border:1px solid var(--hair);border-radius:4px;padding:1px 5px;font-size:11px}
</style>
<div class=left>
  <h1>PCM Studio · 800×480 实时</h1>
  <img id=scr>
  <div class=keys>
    <div class=knob><b>音量</b><button onclick=knob(0,-1)>−</button><button onclick=knob(0,1)>+</button></div>
    <button onclick=key(15)>⏮ 上一首</button>
    <button onclick=key(16)>⏯ 播放/暂停</button>
    <button onclick=key(14)>⏭ 下一首</button>
    <button onclick=key(1)>MEDIA</button>
    <button onclick=key(2)>RADIO</button>
    <button onclick=key(7)>SETUP</button>
    <button onclick=key(8)>← BACK</button>
  </div>
  <div class=hint>点画面 = 触摸 · 键盘 <kbd>←</kbd><kbd>→</kbd> 拧音量 · <kbd>空格</kbd> 播放/暂停 · <kbd>Esc</kbd> 返回</div>
</div>
<div class=right>
  <div class=card>
    <h2>模拟车机状态</h2>
    <label>音量 <span id=vt></span><input type=range id=vol min=0 max=40 value=18 oninput=push()></label>
    <label>歌名<input id=title value="扒下啊伪黄帝新装" oninput=push()></label>
    <label>艺术家 / 副标题<input id=artist value="蓝牙音频" oninput=push()></label>
    <label>设备名<input id=device value="Demo Phone" oninput=push()></label>
    <div class=row>
      <label>播放状态<select id=play onchange=push()>
        <option value=1>播放中</option><option value=2>暂停</option><option value=0>停止</option></select></label>
      <label>音源<select id=src onchange=push()>
        <option value=1>蓝牙</option><option value=2>FM</option><option value=3>AUX</option></select></label>
    </div>
    <div class=row>
      <label>时<input id=hh type=number value=3 min=0 max=23 oninput=push()></label>
      <label>分<input id=mm type=number value=0 min=0 max=59 oninput=push()></label>
    </div>
  </div>
  <div class=card>
    <h2>说明</h2>
    <div class=hint>这里跑的是**真正的系统代码**(studio/sys + studio/scenes),
    跟上台架编译的是同一份源码。改场景代码 → 重编 → 这里立刻能看能点。<br><br>
    画面是系统自己渲染的 RGBA5551 帧, 跟台架硬件层格式一致 ⇒ 所见即所得。</div>
  </div>
</div>
<script>
const img=document.getElementById('scr');
function tick(){ img.src='/frame.png?'+Date.now(); }
img.onload=()=>setTimeout(tick,100); img.onerror=()=>setTimeout(tick,400);
tick();
function ev(t,which,arg,x,y){ fetch('/ev',{method:'POST',body:JSON.stringify({t,which,arg,x,y})}); }
function knob(w,d){ ev(1,w,d,0,0); }
function key(k){ ev(2,0,k,0,0); }
img.onclick=e=>{ const r=img.getBoundingClientRect();
  ev(4,0,0, Math.round((e.clientX-r.left)/r.width*800), Math.round((e.clientY-r.top)/r.height*480)); };
document.onkeydown=e=>{
  if(e.key=='ArrowRight')knob(0,1); else if(e.key=='ArrowLeft')knob(0,-1);
  else if(e.key==' '){e.preventDefault();key(16);} else if(e.key=='Escape')key(8);
};
function push(){
  document.getElementById('vt').textContent=document.getElementById('vol').value;
  const s={volume:+vol.value, play_state:+play.value, source:+src.value,
    hour:+hh.value, minute:+mm.value,
    title:title.value, artist:artist.value, device:device.value};
  fetch('/state',{method:'POST',body:JSON.stringify(s)});
}
push();
</script>
"""

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        if self.path.startswith("/frame.png"):
            png = ppm_to_png()
            if png: self._send(200, "image/png", png)
            else:   self._send(503, "text/plain", b"no frame")
        else:
            self._send(200, "text/html; charset=utf-8", PAGE.encode())
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        try: d = json.loads(self.rfile.read(n) or b"{}")
        except Exception: d = {}
        if self.path == "/ev":
            push_event(d.get("t",0), d.get("which",0), d.get("arg",0), d.get("x",0), d.get("y",0))
        elif self.path == "/state":
            os.makedirs(RUN, exist_ok=True)
            with open(os.path.join(RUN,"state.json"),"w") as f: json.dump(d,f,ensure_ascii=False)
        self._send(200, "application/json", b"{}")

if __name__ == "__main__":
    os.makedirs(RUN, exist_ok=True)
    socketserver.TCPServer.allow_reuse_address = True
    print("PCM Studio 控制台: http://localhost:%d" % PORT)
    socketserver.TCPServer(("127.0.0.1", PORT), H).serve_forever()
