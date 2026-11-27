# SPDX-License-Identifier: GPL-2.0-or-later
# usage: title-compare.py WINE_RUN OUR_LOG [--keep N]: the window's title at each checkpoint of a session, ours (the
# last "window title: ..." before "checkpoint NAME") against Wine's (WINE_RUN/NAME.title.txt, play.py writes it), for
# a title that says its state there (Desire: the scene, the enemy's state). Frame rate and memory figures (from
# " | <n> FPS" on) and what changes with time inside a state (" / Thinking", " / Walking") are left out. Wine's log
# writes code units above 0x7f as \xxxx: both sides are compared as code points. Exit status 1 if one differs.
import re, sys, os
wine_dir, log = sys.argv[1], sys.argv[2]
def norm(t):
    t = re.sub(r'\\([0-9a-f]{4})', lambda m: chr(int(m.group(1), 16)), t.strip())
    t = re.sub(r' \| \d+ FPS.*$', '', t)
    return re.sub(r' / (Thinking|Walking|Idle)\b', '', t)
title, ours = None, {}
for line in open(log, errors='replace'):
    m = re.match(r'pisco: window title: (.*)$', line)
    if m: title = m.group(1); continue
    m = re.match(r'pisco: checkpoint (\S+) frame', line)
    if m: ours[m.group(1)] = title
bad = same = 0
for name, t in ours.items():
    f = os.path.join(wine_dir, f'{name}.title.txt')
    if not os.path.exists(f): print(f'  {name}: no title of Wine\'s'); continue
    w = open(f, encoding='utf-8', errors='replace').read()
    if norm(w) == norm(t or ''): same += 1; print(f'  {name}: "{norm(w)}"')
    else: bad += 1; print(f'  {name}: Wine "{norm(w)}" | here "{norm(t or "")}"')
print(f'{same} of {same + bad} checkpoints with a title of Wine\'s have the same title')
sys.exit(1 if bad or not same else 0)
