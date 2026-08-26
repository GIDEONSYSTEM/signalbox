# -*- coding: utf-8 -*-
"""Подставной SignalBox: панель настоящая, камеры выдуманные.

Зачем. Панель правится куда чаще, чем под рукой оказывается студия с живыми
камерами, а половина её поведения — группы, ориентация, ширина коробок,
перетаскивание, конструктор карточки — камер и не требует. Этот сервер отдаёт
НАСТОЯЩУЮ панель из build/Release/www и придуманный парк камер обеих марок,
поэтому интерфейс можно гонять руками где угодно.

Что живое:
  • кнопки REC и регуляторы меняют состояние — панель видит это следующим опросом;
  • группы и раскладка карточки сохраняются и переживают перезапуск;
  • Live Preview показывает бегущие цветные полосы с рамкой автофокуса;
  • у PYXIS тикает таймкод, при записи идёт счётчик и расходуются карта с батареей.

Чего нет: самих камер. Управлять нечем, все значения выдуманы — взяты из
образцов самой панели (SAMPLE_SONY / SAMPLE_BMD, те же, что в конструкторе
карточки), чтобы карточки выглядели как в работе.

🔴 Настоящему SignalBox не мешает: тот слушает 8787, этот — 8799.

    python3 tools/fakecams.py            # и открыть http://127.0.0.1:8799/
    python3 tools/fakecams.py --port 9000
    python3 tools/fakecams.py --reset    # забыть придуманные залы и раскладку

Слушает 0.0.0.0 — чтобы открыть с телефона и посмотреть, как панель сама
выбирает вертикальную раскладку.
"""
import argparse, http.server, json, os, struct, tempfile, threading, time, zlib, copy

HERE = os.path.dirname(os.path.abspath(__file__))
# Панель берём из рабочей копии репозитория — той самой, которую и правим.
WWW  = os.path.join(os.path.dirname(HERE), "build", "Release", "www")
# Придуманные залы и раскладка — во временную папку, а НЕ рядом со скриптом:
# в репозитории им делать нечего. Данные студии (~/Library/Application Support/
# SignalBox на macOS, рядом с exe на Windows) при этом не трогаются вовсе.
STORE = os.path.join(tempfile.gettempdir(), "signalbox-fakecams")
STORE_GROUPS = os.path.join(STORE, "groups.json")
STORE_LAYOUT = os.path.join(STORE, "cardlayout.json")

ap = argparse.ArgumentParser(description="Подставной SignalBox для примерки панели")
ap.add_argument("--port",  type=int, default=8799, help="порт (по умолчанию 8799)")
ap.add_argument("--reset", action="store_true", help="забыть придуманные залы и раскладку")
ARGS = ap.parse_args()
PORT = ARGS.port
os.makedirs(STORE, exist_ok=True)
if ARGS.reset:
    for p in (STORE_GROUPS, STORE_LAYOUT):
        try: os.remove(p)
        except FileNotFoundError: pass

# ---------------------------------------------------------------- парк камер
SONY = {
  "vendor":"sony","model":"ILCE-7SM3","online":True,"rec":False,
  "battery":78,"acPower":True,"cardMinutes":96,"overheat":0,
  "iso":"800","isoEff":None,
  "isoOpts":{"cur":800,"rw":True,"opts":[[400,"400"],[500,"500"],[640,"640"],[800,"800"],
             [1000,"1000"],[1250,"1250"],[1600,"1600"],[2000,"2000"],[2500,"2500"],[3200,"3200"]]},
  "aperture":{"cur":280,"rw":True,"opts":[[180,"F1.8"],[200,"F2.0"],[220,"F2.2"],[250,"F2.5"],
              [280,"F2.8"],[320,"F3.2"],[350,"F3.5"],[400,"F4.0"],[450,"F4.5"],[560,"F5.6"]]},
  "shutter":{"cur":65586,"rw":True,"opts":[[65561,"1/25"],[65586,"1/50"],[65596,"1/60"],
             [65661,"1/125"],[65786,"1/250"],[66036,"1/500"],[66536,"1/1000"]]},
  "shutterMeasurement":"ShutterSpeed",
  "wb":{"cur":17,"rw":True,"opts":[[0,"Авто"],[17,"Дневной свет"],[18,"Тень"],[19,"Облачно"],
        [20,"Лампа накаливания"],[21,"Флуор. тёплый"]]},
  "wbKelvin":5600,"wbKelvinRw":True,"wbKelvinRange":[2500,9900,100],
  "micGain":{"cur":18,"rw":True,"opts":[[15,"15"],[16,"16"],[17,"17"],[18,"18"],[19,"19"],[20,"20"]]},
}
BMD = {
  "vendor":"bmd","model":"PYXIS 6K","name":"","online":True,"apiOff":False,"rec":False,
  "timecode":"15:02:25:03","recTime":"00:00:00:00",
  "codec":"BRaw:12_1","resolution":[6048,3408],"fps":"25","dynamicRange":"Film",
  "tally":"None","powerSource":"AC","milliVolt":11900,
  "iso":1250,"isoOpts":[100,200,400,800,1250,1600,3200,6400,12800],
  "gain":10,"gainOpts":[-12,-6,0,6,10,12,18,24,30],
  "shutter":50,"shutterOpts":[25,30,50,60,100,125,180,200,250],"shutterMeasurement":"ShutterSpeed",
  "wb":5500,"wbRange":[2500,10000],"tint":0,"tintRange":[-50,50],
  "iris":6.2,"irisControllable":True,"autoExposure":"Off",
}

def mk(base, num, key, **over):
    c = copy.deepcopy(base)
    c["id"] = "CAM %d" % num
    c["key"] = key
    c["ip"] = key if not key.startswith("bmd:") else "10.0.0.%d" % (20 + num)
    c.update(over)
    return c

# Парк подобран так, чтобы разом было видно всё: зал, где карточки переносятся
# на вторую строку; зал на две; пустой зал; камера без зала. Плюс состояния,
# которые редко поймаешь вживую: запись, севшая батарея, перегрев, Program.
CAMS = [
    mk(SONY, 1, "10.0.0.11", rec=True,  battery=64, cardMinutes=41),
    mk(SONY, 2, "10.0.0.12", battery=91, acPower=False),
    mk(SONY, 3, "10.0.0.13", model="ZV-E1", battery=12, acPower=False, cardMinutes=7),
    mk(SONY, 4, "10.0.0.14", battery=55, overheat=2),
    mk(BMD,  5, "bmd:s1",   name="ШИРОКИЙ", tally="Program", rec=True),
    mk(BMD,  6, "bmd:s2",   name="КРУПНЫЙ", tally="Preview", powerSource="Battery", milliVolt=15400),
    mk(SONY, 7, "10.0.0.17", model="ILCE-7M4", battery=83),
]
DEFAULT_GROUPS = {"groups":[
    {"id":"g1","name":"Большой зал","cams":["10.0.0.11","10.0.0.12","10.0.0.13","10.0.0.14"],"binds":[]},
    {"id":"g2","name":"Малый зал","cams":["bmd:s1","bmd:s2"],"binds":[]},
    {"id":"g3","name":"Гримёрка","cams":[],"binds":[]},
]}

def load(path, default):
    try:
        with open(path, encoding="utf-8") as f: return json.load(f)
    except Exception: return copy.deepcopy(default)

STATE = {"seq":1, "groups":load(STORE_GROUPS, DEFAULT_GROUPS), "layout":load(STORE_LAYOUT, None)}
LOCK = threading.Lock()

def bump(): STATE["seq"] += 1

def find(sel):
    """Камера по номеру из id, по ключу или по ip. 'all' — весь парк."""
    if sel == "all": return list(CAMS)
    s = str(sel)
    out = [c for c in CAMS if c["key"] == s or c["ip"] == s or c["id"].split()[-1] == s]
    return out

# ------------------------------------------------------------------ команды
def apply_cmd(c, action, value):
    v = str(value)
    if action == "rec":
        c["rec"] = (v == "start")
        if c["vendor"] == "bmd" and not c["rec"]: c["recTime"] = "00:00:00:00"
    elif action == "iso":
        c["iso"] = int(v) if c["vendor"] == "bmd" else v
        if c["vendor"] == "sony": c["isoOpts"]["cur"] = int(v)
        else: c["gain"] = max(-12, min(30, round(20 * (len(v) - 3))))   # грубая связь ISO↔gain
    elif action == "gain":   c["gain"] = int(v)
    elif action == "aperture": c["aperture"]["cur"] = int(v)
    elif action == "shutter":
        if c["vendor"] == "bmd": c["shutter"] = int(v)
        else: c["shutter"]["cur"] = int(v)
    elif action == "wb":
        if c["vendor"] == "bmd": c["wb"] = int(v)
        else: c["wb"]["cur"] = int(v)
    elif action == "wbkelvin":
        if c["vendor"] == "bmd": c["wb"] = int(v)
        else: c["wbKelvin"] = int(v)
    elif action == "tint":    c["tint"] = int(v)
    elif action == "micgain": c["micGain"]["cur"] = int(v)

def ticker():
    """Секундный ход: таймкод, счётчик записи, расход батареи и карты."""
    while True:
        time.sleep(1)
        with LOCK:
            for c in CAMS:
                if c["vendor"] == "bmd":
                    h,m,s,f = [int(x) for x in c["timecode"].split(":")]
                    s += 1; m += s//60; s %= 60; h += m//60; m %= 60; h %= 24
                    c["timecode"] = "%02d:%02d:%02d:%02d" % (h,m,s,f)
                    if c["rec"]:
                        h,m,s,f = [int(x) for x in c["recTime"].split(":")]
                        s += 1; m += s//60; s %= 60; h += m//60; m %= 60
                        c["recTime"] = "%02d:%02d:%02d:%02d" % (h,m,s,f)
                elif c["rec"] and c["cardMinutes"] > 0:
                    if int(time.time()) % 12 == 0:
                        c["cardMinutes"] -= 1
                        if not c["acPower"] and c["battery"] > 0: c["battery"] -= 1
            bump()
threading.Thread(target=ticker, daemon=True).start()

# ------------------------------------------------------- картинка Live Preview
LVW, LVH, NFRAMES = 480, 270, 40
BARS = [(235,235,235),(235,235,16),(16,235,235),(16,235,16),(235,16,235),(235,16,16),(16,16,235)]
_png_cache = {}

def _chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))

def frame_png(cam_num, idx):
    """Цветные полосы с бегущей риской. Все строки одинаковые — рисуем одну."""
    key = (cam_num, idx)
    if key in _png_cache: return _png_cache[key]
    tint = 0.55 + 0.45 * ((cam_num * 7) % 5) / 4.0        # свой оттенок у каждой камеры
    sweep = int((idx / NFRAMES) * LVW)
    row = bytearray()
    for x in range(LVW):
        r,g,b = BARS[min(x * len(BARS) // LVW, len(BARS)-1)]
        if abs(x - sweep) < 3: r = g = b = 255
        else: r,g,b = int(r*tint), int(g*tint), int(b*tint)
        row += bytes((r,g,b))
    raw = (b"\x00" + bytes(row)) * LVH
    png = (b"\x89PNG\r\n\x1a\n"
           + _chunk(b"IHDR", struct.pack(">IIBBBBB", LVW, LVH, 8, 2, 0, 0, 0))
           + _chunk(b"IDAT", zlib.compress(raw, 6))
           + _chunk(b"IEND", b""))
    _png_cache[key] = png
    return png

def af_frames(idx):
    """Рамка автофокуса, которая ездит вместе с риской; изредка «захват»."""
    cx = 0.08 + 0.84 * (idx / NFRAMES)
    return [{"k":"face","cx":round(cx,3),"cy":0.46,"w":0.20,"h":0.30,
             "ty": 2 if (idx // 10) % 2 == 0 else 1, "st":1}]

# ---------------------------------------------------------------------- HTTP
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw): super().__init__(*a, directory=WWW, **kw)
    def log_message(self, *a): pass

    def _send(self, body, ctype, extra=None, code=200):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD": self.wfile.write(body)

    def _json(self, obj, extra=None):
        self._send(json.dumps(obj, ensure_ascii=False).encode(), "application/json; charset=utf-8", extra)

    def do_GET(self):
        path, _, q = self.path.partition("?")
        args = dict(p.split("=", 1) for p in q.split("&") if "=" in p)

        if path == "/": self.path = "/cam-control-panel.html"; return super().do_GET()

        if path == "/status.json":
            with LOCK:
                seq = STATE["seq"]
                if args.get("seq") == str(seq):
                    self.send_response(304); self.send_header("X-Status-Seq", str(seq))
                    self.send_header("Content-Length", "0"); self.end_headers(); return
                body = {"server": SERVER_URL, "cameras": copy.deepcopy(CAMS)}
            return self._json(body, {"X-Status-Seq": str(seq)})

        if path == "/groups.json":
            with LOCK: return self._json(STATE["groups"])
        if path == "/cardlayout.json":
            with LOCK: lay = STATE["layout"]
            if lay is None: self.send_error(404); return
            return self._json(lay)
        if path == "/cameras.json":
            return self._json({"cameras":[{"ip":c["ip"],"model":c["model"]} for c in CAMS]})
        if path == "/update.json":
            return self._json({"version":"1.0.8","latest":"1.0.8","auto":False,"state":"idle"})

        if path.startswith("/liveview/"):
            try: num = int(path.split("/")[-1].split(".")[0])
            except ValueError: self.send_error(404); return
            idx = int(time.time() * 8) % NFRAMES
            if args.get("seq") == str(idx):
                self.send_response(304); self.send_header("Content-Length","0"); self.end_headers(); return
            return self._send(frame_png(num, idx), "image/png",
                              {"X-Cam-Seq": str(idx),
                               "X-Cam-Frames": json.dumps(af_frames(idx))})
        return super().do_GET()

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        try: body = json.loads(self.rfile.read(n) or b"{}")
        except Exception: body = {}
        path = self.path.split("?")[0]

        if path == "/cmd":
            targets = []
            with LOCK:
                if "cams" in body:
                    for k in body.get("cams") or []: targets += find(k)
                else:
                    targets = find(body.get("cam"))
                for c in targets: apply_cmd(c, body.get("action",""), body.get("value",""))
                bump()
            return self._send(b"", "text/plain", code=204)

        if path == "/groups/save":
            with LOCK:
                # Как настоящий сервер (§2): схемой владеет панель, мы пишем
                # присланный объект ЦЕЛИКОМ. Поэтому лишние ключи (ungrouped —
                # порядок карточек вне залов) сохраняются и возвращаются.
                STATE["groups"] = body
                with open(STORE_GROUPS,"w",encoding="utf-8") as f:
                    json.dump(STATE["groups"], f, ensure_ascii=False)
            return self._send(b"", "text/plain", code=204)

        if path == "/cardlayout/save":
            with LOCK:
                STATE["layout"] = body
                with open(STORE_LAYOUT,"w",encoding="utf-8") as f:
                    json.dump(body, f, ensure_ascii=False)
            return self._send(b"", "text/plain", code=204)

        if path == "/shutdown":
            self._send(b"", "text/plain", code=204)
            threading.Thread(target=lambda: (time.sleep(.3), os._exit(0)), daemon=True).start()
            return
        return self._send(b"", "text/plain", code=204)

def lan_ip():
    """Адрес для доступа с телефона. Спрашиваем у интерфейса, а не пробой
    наружу: настоящий сервер тоже так не делает — при VPN проба уходит в
    туннель и выбирается адрес туннеля (§2)."""
    try:
        return os.popen("ipconfig getifaddr en0").read().strip() or "127.0.0.1"
    except Exception: return "127.0.0.1"

if not os.path.isdir(WWW):
    raise SystemExit("не нашёл панель: %s" % WWW)
SERVER_URL = "http://%s:%d/" % (lan_ip(), PORT)
srv = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), H)
print("подставной SignalBox — камеры выдуманы, управлять нечем")
print("  панель:     http://127.0.0.1:%d/" % PORT)
print("  с телефона: %s" % SERVER_URL)
print("  залы и раскладка: %s" % STORE)
print("  Ctrl+C — остановить")
try:
    srv.serve_forever()
except KeyboardInterrupt:
    print()
