#!/usr/bin/env python
"""bench.py <seconds> <outprefix> [t:line ...] - talk to the app's console without resetting it.

Attaches esp_idf_monitor with --no-reset under a pty (opening the port any
other way resets the board, and an app started by kodeOS returns to it), sends
each console line at its time, prints the console's answers, and turns every
`screenshot` answer into <outprefix>_N.png with kodeOS's own payload decoder
(tools/kodeos/kpayload.py of KodeOS, path in KPAYLOAD_DIR).
"""
import os, pty, re, select, signal, sys, time

secs = float(sys.argv[1]); prefix = sys.argv[2]
events = sorted((float(a.split(':', 1)[0]), a.split(':', 1)[1]) for a in sys.argv[3:])
port = os.environ.get('KODE_PORT', '/dev/cu.usbmodem101')
py = os.environ.get('IDF_PY', os.path.expanduser('~/.espressif/tools/python/v6.1/venv/bin/python'))
pid, fd = pty.fork()
if pid == 0:
    os.execvp(py, [py, '-m', 'esp_idf_monitor', '-p', port, '-b', '115200', '--no-reset', '--disable-auto-color'])
buf = b''
t0 = time.time(); i = 0
while time.time() - t0 < secs:
    if i < len(events) and time.time() - t0 >= events[i][0]:
        os.write(fd, events[i][1].encode() + b'\r\n'); i += 1
    r, _, _ = select.select([fd], [], [], 0.05)
    if r:
        try: buf += os.read(fd, 1 << 16)
        except OSError: break
try:
    os.kill(pid, signal.SIGTERM); time.sleep(0.2); os.kill(pid, signal.SIGKILL)
except Exception:
    pass
text = re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', buf.decode('utf-8', 'replace'))
lines = [l.strip('\r') for l in text.split('\n')]
sys.path.insert(0, os.environ.get('KPAYLOAD_DIR', os.path.expanduser('~/Documents/git/KodeOS/tools')))
from kpayload import decode_records, write_capture, head_fields
shot = 0; parts = None; head = None
for l in lines:
    s = l.strip()
    if s.startswith('>|d='):
        if parts is not None: parts.append(s[4:])
        continue
    if 'screenshot' in s and s.startswith('kode>'):
        parts = []; head = None; continue
    if parts is not None and s.startswith('>|'):
        head = s[2:]; continue
    if parts is not None and s.startswith('>err'):
        print('screenshot refused:', s); parts = None; continue
    if parts is not None and s.startswith('>ok'):
        if parts:
            f = head_fields(head) if head else {}
            want = f.get('len') or f.get('size')
            try:
                data = decode_records(parts, int(want) if want else None)
                out = f'{prefix}_{shot}.png'; shot += 1
                print(write_capture(out, data)[1])
            except Exception as e:
                print('screenshot not decoded:', e)
        parts = None; continue
    if s.startswith('>') or 'Guru' in s or 'back from app' in s or 'wdt' in s:
        print(s[:160])
