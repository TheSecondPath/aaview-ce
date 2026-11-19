#!/usr/bin/env python3
"""rmwork_web: a small web page on the Pi for the head-unit test rig (Python standard library only).

  /               camera view of the head-unit screen + vagent command box + job history
  /stream.mjpg    live MJPEG from the Pi camera (one rpicam-vid process, shared by all viewers)
  /snapshot.jpg   the latest frame (the camera can't be opened twice while streaming)
  POST /job       run vagent commands through vjob, one job at a time
  /jobs.json      job queue and results

Basic auth: user "veloaa", password from /etc/rmwork-web.pass (one line).
"""
import base64, html, json, subprocess, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

PORT = 8080
PASS_FILE = '/etc/rmwork-web.pass'
CAM_CMD = ['rpicam-vid', '-n', '-t', '0', '--codec', 'mjpeg', '--width', '800', '--height', '600',
           '--framerate', '8', '--shutter', '6000', '--gain', '1', '--awb', 'indoor', '--rotation', '180',
           '-q', '70', '-o', '-']
QUICK = [
    ('Start Android Auto', ['pi:aaview unquit', 'start \\MD\\aaview\\aaview.exe /auto']),
    # A clean quit lets aaview release the decoder's memory. Killing it would leak that until the head unit restarts.
    ('Stop Android Auto', ['pi:aaview quit']),
    ('Android Auto log', ['type \\MD\\aaview\\aaview_log.txt']),
    # There is no "Kill MyShell" button: \Windows\MyShell.exe is only a boot stub that exits after starting Scratchy.
    # LG's real GUI is kronos.exe, which also runs the reverse camera, audio sources and steering keys, so it must
    # not be killed.
    ('Processes', ['procs']),
    ('Memory', ['mem']),
    ('Start MyShell (only if LG shell is NOT running)', ['start \\Windows\\MyShell.exe']),
]

PI_CMDS = {'aaview quit': ['sudo', '/usr/local/lib/veloaa/aarecvd', '--quit-aaview'],
           'aaview unquit': ['sudo', '/usr/local/lib/veloaa/aarecvd', '--unquit-aaview']}

# ---------------- camera ----------------

latest = {'jpg': None, 't': 0.0}
frame_cv = threading.Condition()


def camera_loop():
    while True:
        try:
            p = subprocess.Popen(CAM_CMD, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)
            buf = b''
            while True:
                chunk = p.stdout.read(65536)
                if not chunk:
                    break
                buf += chunk
                while True:
                    s = buf.find(b'\xff\xd8')
                    e = buf.find(b'\xff\xd9', s + 2) if s >= 0 else -1
                    if s < 0 or e < 0:
                        if len(buf) > 4_000_000:
                            buf = b''
                        break
                    with frame_cv:
                        latest['jpg'] = buf[s:e + 2]
                        latest['t'] = time.time()
                        frame_cv.notify_all()
                    buf = buf[e + 2:]
            p.wait()
        except Exception as ex:  # keep trying: camera busy, cable reseated, ...
            print('camera:', ex, flush=True)
        time.sleep(3)

# ---------------- jobs (vjob, one at a time) ----------------

jobs = []                       # dicts: id, cmds, state, started, finished, output
jobs_lock = threading.Lock()
job_cv = threading.Condition(jobs_lock)
last_id = 0


def job_worker():
    while True:
        with job_cv:
            while not any(j['state'] == 'queued' for j in jobs):
                job_cv.wait()
            job = next(j for j in jobs if j['state'] == 'queued')
            job['state'] = 'running'
            job['started'] = time.strftime('%H:%M:%S')
        out, state = '', 'done'
        try:
            # "pi:" steps run here on the Pi first, and only those listed in PI_CMDS. The rest go to vagent through vjob.
            for c in job['cmds']:
                if c.startswith('pi:'):
                    argv = PI_CMDS.get(c[3:].strip())
                    if not argv:
                        out += 'not an allowed Pi command: %s\n' % c
                        state = 'failed'
                        continue
                    r = subprocess.run(argv, capture_output=True, text=True, timeout=30)
                    out += '[pi] %s: %s' % (c[3:].strip(), r.stdout or r.stderr)
            vcmds = [c for c in job['cmds'] if not c.startswith('pi:')]
            if vcmds:
                r = subprocess.run(['/usr/local/bin/vjob', '-'], input='\n'.join(vcmds) + '\n',
                                   capture_output=True, text=True, timeout=600)
                out += r.stdout + (('\n' + r.stderr) if r.stderr.strip() else '')
                state = 'done' if r.returncode == 0 else 'failed (%d)' % r.returncode
        except Exception as ex:
            out, state = out + str(ex), 'failed'
        with jobs_lock:
            job['output'] = out
            job['state'] = state
            job['finished'] = time.strftime('%H:%M:%S')


def add_job(cmds):
    cmds = [c.strip() for c in cmds if c.strip() and not c.strip().startswith('#')]
    if not cmds:
        return None
    global last_id
    with job_cv:
        last_id += 1
        j = {'id': last_id, 'cmds': cmds, 'state': 'queued', 'started': '', 'finished': '', 'output': ''}
        jobs.append(j)
        del jobs[:-30]
        job_cv.notify_all()
        return j['id']

# ---------------- web ----------------

PAGE = '''<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>velosterAA</title><style>
:root{--bg:#111;--fg:#ddd;--mut:#888;--acc:#4af;--box:#1b1b1b;--brd:#333}
body{margin:0;background:var(--bg);color:var(--fg);font:15px system-ui,sans-serif}
main{max-width:900px;margin:0 auto;padding:12px 16px}
h1{font-size:18px;margin:4px 0 10px} img{width:100%;max-width:800px;background:#000;border:1px solid var(--brd)}
.row{display:flex;flex-wrap:wrap;gap:6px;margin:10px 0}
button{background:var(--box);color:var(--fg);border:1px solid var(--brd);border-radius:6px;padding:8px 10px;font-size:14px}
button:hover{border-color:var(--acc)} textarea{width:100%;box-sizing:border-box;height:90px;background:var(--box);
color:var(--fg);border:1px solid var(--brd);font:13px ui-monospace,monospace;padding:6px}
.job{background:var(--box);border:1px solid var(--brd);border-radius:6px;margin:8px 0;padding:8px}
.job pre{white-space:pre-wrap;word-break:break-word;font:12px ui-monospace,monospace;margin:6px 0 0;max-height:360px;overflow:auto}
.mut{color:var(--mut);font-size:13px}
</style></head><body><main>
<h1>velosterAA: head unit</h1>
<img id="cam" src="/stream.mjpg" alt="camera">
<div class="mut">Live camera (about 8 fps). Each command round trip takes about 10 s (the stick has to be safely re-plugged).</div>
<div class="row">__QUICK__</div>
<textarea id="cmds" placeholder="vagent commands, one per line, e.g.&#10;dir \\MD\\aaview&#10;type \\MD\\aaview\\aaview_log.txt"></textarea>
<div class="row"><button onclick="send(document.getElementById('cmds').value.split('\\n'))">Run commands</button></div>
<div id="jobs"></div>
<div class="mut">Commands: log, run, start, kill [name], sleep, procs, collect, copy, dir, type, del, mkdir, rmdir, reg, mem.
Writes only go to the stick or \\Temp.</div>
</main><script>
const quick=__QUICKJSON__;
function send(c){fetch('/job',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({cmds:c})}).then(load)}
function esc(s){return s.replace(/[&<>]/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[m]))}
function load(){fetch('/jobs.json').then(r=>r.json()).then(js=>{document.getElementById('jobs').innerHTML=js.reverse().map(j=>
 '<div class="job"><b>#'+j.id+'</b> <span class="mut">'+esc(j.state)+(j.started?' &middot; started '+j.started:'')+(j.finished?' &middot; finished '+j.finished:'')+
 '</span><pre>'+esc(j.cmds.join('\\n'))+'</pre>'+(j.output?'<pre>'+esc(j.output)+'</pre>':'')+'</div>').join('')})}
load();setInterval(load,4000);
document.getElementById('cam').onerror=function(){setTimeout(()=>{this.src='/stream.mjpg?'+Date.now()},3000)};
</script></body></html>'''


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def authed(self):
        try:
            pw = open(PASS_FILE).read().strip()
        except OSError:
            pw = ''
        want = 'Basic ' + base64.b64encode(('veloaa:' + pw).encode()).decode()
        if pw and self.headers.get('Authorization') == want:
            return True
        self.send_response(401)
        self.send_header('WWW-Authenticate', 'Basic realm="velosterAA"')
        self.end_headers()
        return False

    def send_bytes(self, code, ctype, data):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if not self.authed():
            return
        path = self.path.split('?')[0]
        if path == '/':
            q = ''.join('<button onclick="send(quick[%d])">%s</button>' % (i, html.escape(n)) for i, (n, _) in enumerate(QUICK))
            page = PAGE.replace('__QUICK__', q).replace('__QUICKJSON__', json.dumps([c for _, c in QUICK]))
            self.send_bytes(200, 'text/html; charset=utf-8', page.encode())
        elif path == '/snapshot.jpg':
            with frame_cv:
                if not latest['jpg'] or time.time() - latest['t'] > 5:
                    frame_cv.wait(5)
                jpg = latest['jpg']
            if jpg:
                self.send_bytes(200, 'image/jpeg', jpg)
            else:
                self.send_bytes(503, 'text/plain', b'no camera frame yet')
        elif path == '/stream.mjpg':
            self.send_response(200)
            self.send_header('Content-Type', 'multipart/x-mixed-replace; boundary=frame')
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            last = 0.0
            try:
                while True:
                    with frame_cv:
                        while latest['t'] <= last:
                            frame_cv.wait(10)
                        jpg, last = latest['jpg'], latest['t']
                    self.wfile.write(b'--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n' % len(jpg))
                    self.wfile.write(jpg + b'\r\n')
            except (BrokenPipeError, ConnectionResetError):
                pass
        elif path == '/jobs.json':
            with jobs_lock:
                data = json.dumps(jobs).encode()
            self.send_bytes(200, 'application/json', data)
        else:
            self.send_bytes(404, 'text/plain', b'not found')

    def do_POST(self):
        if not self.authed():
            return
        if self.path != '/job':
            return self.send_bytes(404, 'text/plain', b'not found')
        # block posts from other sites (the browser sends the saved password with them)
        if self.headers.get('Content-Type', '').split(';')[0].strip().lower() != 'application/json':
            return self.send_bytes(400, 'text/plain', b'expected application/json')
        origin = self.headers.get('Origin')
        if origin is not None and urlsplit(origin).netloc != self.headers.get('Host', ''):
            return self.send_bytes(403, 'text/plain', b'cross-site request')
        n = int(self.headers.get('Content-Length', '0') or 0)
        try:
            body = json.loads(self.rfile.read(min(n, 65536)) or b'{}')
            jid = add_job(list(body.get('cmds', []))[:50])
        except (ValueError, TypeError):
            jid = None
        self.send_bytes(200 if jid else 400, 'application/json', json.dumps({'id': jid}).encode())


if __name__ == '__main__':
    threading.Thread(target=camera_loop, daemon=True).start()
    threading.Thread(target=job_worker, daemon=True).start()
    ThreadingHTTPServer(('0.0.0.0', PORT), Handler).serve_forever()
