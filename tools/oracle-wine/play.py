# SPDX-License-Identifier: GPL-2.0-or-later
# usage: play.py DISPLAY SESSION.json OUTDIR — plays a session script on an X display through XTEST and saves
# its screenshots as OUTDIR/<name>.png (the title's client area) and OUTDIR/<name>.full.png (the screen). When Wine's
# log (OUTDIR/wine.log) has SetWindowTextW calls (+relay with RelayInclude user32.SetWindowTextW), the window's last
# title at each shot goes to OUTDIR/<name>.title.txt: a title that shows its state there (Desire) is compared by it.
# The session format is in README.md. Needs python-xlib.
import json, os, re, struct, sys, time, zlib
from Xlib import X, XK, display
from Xlib.ext import xtest

d = display.Display(sys.argv[1]); session = json.load(open(sys.argv[2])); out = sys.argv[3]
root = d.screen().root
# No pointer acceleration: X would otherwise scale each motion event of more than 4 pixels, and the title would see
# other deltas than the session gives (our runtime's session player passes them on unchanged)
d.change_pointer_control(accel=(1, 1), threshold=1); d.sync()
BUTTON = {'left': 1, 'middle': 2, 'right': 3}
KEYSYM = {' ': 'space', '~': 'grave', '`': 'grave', 'enter': 'Return', 'esc': 'Escape', 'escape': 'Escape',
          'up': 'Up', 'down': 'Down', 'left': 'Left', 'right': 'Right', 'tab': 'Tab', 'shift': 'Shift_L', 'ctrl': 'Control_L'}

def windows(w, ox=0, oy=0):  # every viewable window with its absolute position, parents first
    try: a = w.get_attributes(); g = w.get_geometry()
    except Exception: return
    if a.map_state != X.IsViewable: return
    x, y = ox + g.x, oy + g.y
    yield w, x, y, g.width, g.height
    for c in w.query_tree().children: yield from windows(c, x, y)

def desktop():  # the largest top-level window: Wine's virtual desktop
    tops = [t for c in root.query_tree().children for t in list(windows(c))[:1]]
    return max(tops, key=lambda t: t[3] * t[4]) if tops else None

def client():  # the title's client area: the innermost window that is smaller than the desktop
    desk = desktop()
    inner = [t for t in windows(desk[0]) if t[3] * t[4] < desk[3] * desk[4] and t[3] >= 64 and t[4] >= 64] if desk else []
    return inner[-1][1:] if inner else (0, 0, d.screen().width_in_pixels, d.screen().height_in_pixels)

def png(path, x, y, w, h):
    img = root.get_image(x, y, w, h, X.ZPixmap, 0xffffffff).data  # BGRX, 4 bytes a pixel
    rows = b''.join(b'\0' + bytes(b for i in range(r * w * 4, (r + 1) * w * 4, 4) for b in (img[i + 2], img[i + 1], img[i])) for r in range(h))
    chunk = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))

def keycode(name):
    sym = XK.string_to_keysym(KEYSYM.get(name.lower(), name))
    if not sym: raise SystemExit(f'play.py: unknown key {name!r}')
    return d.keysym_to_keycode(sym)

start = time.time()
for step in session['steps']:
    ms = step.get('ms', 0) / 1000
    if 'wait' in step: time.sleep(step['wait'] / 1000)
    elif 'focus' in step:  # without a window manager nothing gives Wine the X focus, and without it Wine reads no raw motion
        w = desktop()[0]; w.set_input_focus(X.RevertToParent, X.CurrentTime); w.raise_window(); d.sync()
    elif 'move' in step:  # relative motion in small steps; an absolute warp would arrive as one huge delta
        dx, dy = step['move']; n = max(1, int(max(abs(dx), abs(dy)) / 12)); sx = sy = 0
        for i in range(1, n + 1):
            tx, ty = dx * i // n, dy * i // n
            xtest.fake_input(d, X.MotionNotify, True, x=tx - sx, y=ty - sy); d.sync(); sx, sy = tx, ty
            time.sleep((ms or 0.03 * n) / n)
    elif 'click' in step:
        b = BUTTON[step['click']]
        xtest.fake_input(d, X.ButtonPress, b); d.sync(); time.sleep(ms or 0.15); xtest.fake_input(d, X.ButtonRelease, b); d.sync()
    elif 'button' in step:
        xtest.fake_input(d, X.ButtonPress if step['down'] else X.ButtonRelease, BUTTON[step['button']]); d.sync()
    elif 'key' in step:
        k = keycode(step['key'])
        if 'down' in step: xtest.fake_input(d, X.KeyPress if step['down'] else X.KeyRelease, k); d.sync()
        else: xtest.fake_input(d, X.KeyPress, k); d.sync(); time.sleep(ms or 0.12); xtest.fake_input(d, X.KeyRelease, k); d.sync()
    elif 'shot' in step:
        s = d.screen(); png(f"{out}/{step['shot']}.full.png", 0, 0, s.width_in_pixels, s.height_in_pixels)
        x, y, w, h = client(); png(f"{out}/{step['shot']}.png", x, y, w, h)
        title = ''
        if os.path.exists(f'{out}/wine.log'):
            with open(f'{out}/wine.log', errors='replace') as f:
                for line in f:
                    m = re.search(r'Call user32\.SetWindowTextW\([0-9a-f]+,[0-9a-f]+ L"(.*)"\) ret=', line)
                    if m: title = m.group(1)
            if title: open(f"{out}/{step['shot']}.title.txt", 'w').write(title + '\n')
        print(f"{time.time() - start:7.1f}s shot {step['shot']} client {w}x{h} at ({x},{y})" + (f' title "{title}"' if title else ''), flush=True)
    else: raise SystemExit(f'play.py: unknown step {step}')
print(f'{time.time() - start:7.1f}s done', flush=True)
