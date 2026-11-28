#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# the GUI in the container: Xorg on the framebuffer (fbdev) with evdev input, a window manager and a few X apps
for e in /sys/class/input/event*; do echo "$e: $(cat $e/device/name)"; done
# (a package cache, when one is mounted at /opt/apkcache: a second start installs from it)
[ -d /opt/apkcache ] && ln -sf /opt/apkcache /etc/apk/cache
t0=$(date +%s)
# (the window manager: JWM, which brings cairo, glib and librsvg, or WM=twm, only the X libraries)
apk add -q xorg-server xf86-video-fbdev xf86-input-evdev ${WM:-jwm} xterm font-misc-misc font-cursor-misc xeyes xclock xsetroot font-dejavu fontconfig 2>&1 | tail -3
echo "apk took $(( $(date +%s) - t0 )) s"
# (LATIN_FONTS: only the ISO 8859 bitmap fonts, the cursor font and nil2: lifted Xorg's libXfont2 rejects the fonts
# of two-byte encodings xterm opens (12x13ja, the iso10646 ones), docs/research/linux-gui.md)
if [ -n "$LATIN_FONTS" ]; then
  for f in /usr/share/fonts/misc/*.pcf.gz; do case "$f" in *ISO8859*|*/cursor.pcf.gz|*/nil2.pcf.gz) ;; *) rm -f "$f";; esac; done
  mkfontdir /usr/share/fonts/misc
fi
# no virtual console in this kernel: Xorg shares a stand-in for one (-sharevts) and never switches (-novtswitch)
[ -e /dev/tty0 ] || ln -sf /dev/console /dev/tty0; [ -e /dev/tty7 ] || ln -sf /dev/console /dev/tty7
mkdir -p /etc/X11/xorg.conf.d /tmp/.X11-unix
cat > /etc/X11/xorg.conf <<'CONF'
Section "ServerFlags"
  Option "AutoAddDevices" "false"
  Option "AutoEnableDevices" "false"
  Option "DontVTSwitch" "true"
EndSection
Section "Module"
  Disable "glx"
  Disable "glamoregl"
EndSection
Section "Device"
  Identifier "fb"
  Driver "fbdev"
  Option "fbdev" "/dev/fb0"
EndSection
Section "InputDevice"
  Identifier "kbd"
  Driver "evdev"
  Option "Device" "/dev/input/event0"
EndSection
Section "InputDevice"
  Identifier "mouse"
  Driver "evdev"
  Option "Device" "/dev/input/event1"
EndSection
Section "InputDevice"
  Identifier "tablet"
  Driver "evdev"
  Option "Device" "/dev/input/event2"
EndSection
Section "ServerLayout"
  Identifier "l"
  Screen "s"
  InputDevice "kbd" "CoreKeyboard"
  InputDevice "mouse" "CorePointer"
  InputDevice "tablet" "SendCoreEvents"
EndSection
Section "Screen"
  Identifier "s"
  Device "fb"
EndSection
CONF
t0=$(date +%s)
Xorg :0 vt7 -sharevts -novtswitch -nolisten tcp -keeptty > /tmp/xorg.out 2>&1 &
for i in $(seq 120); do [ -e /tmp/.X11-unix/X0 ] && break; sleep 0.5; done
echo "Xorg up in $(( $(date +%s) - t0 )) s"; tail -5 /tmp/xorg.out
export DISPLAY=:0
xsetroot -solid '#2e5a6b'
sleep 1; ${WM:-jwm} &   # (WM=twm: a window manager that needs only the X libraries)
xterm ${XTERM_FONT:+-fn $XTERM_FONT} ${XTERM_FA:+-fa $XTERM_FA} ${XTERM_FW:+-fw $XTERM_FW} -geometry 80x24+40+40 &
xeyes -geometry 200x150+700+60 &
xclock -geometry 200x200+700+260 &
sleep 2; echo GUI-UP
[ -n "$GUI_BENCH" ] && /opt/gui/bench.sh
sleep 100000
