// SPDX-License-Identifier: GPL-2.0-or-later
// winmm for Windows desktop apps: the multimedia timer's clock, which is the app's clock (rt.h). The machine
// has no joystick, no MIDI devices, no mixer, no CD audio and no recording device: each says so as Windows does
// for a device that is not there. (Sound goes through DirectSound.)
#include "../rt.h"

IMP(winmm, timeGetTime) { if (rt_clock_virtual()) rt_clock_polled(); RET((u32)(rt_uptime_us() / 1000)); }   // milliseconds since the system started (an app that waits by reading it moves a virtual clock on)
IMP(winmm, timeBeginPeriod) { RET(0); }   // TIMERR_NOERROR: the clock is as fine as it gets
IMP(winmm, timeEndPeriod) { RET(0); }
enum { MMSYSERR_BADDEVICEID = 2, MMSYSERR_NODRIVER = 6, JOYERR_UNPLUGGED = 167 };
IMP(winmm, joyGetNumDevs) { RET(0); }
IMP(winmm, joyGetDevCapsA) { RET(MMSYSERR_NODRIVER); }
IMP(winmm, joyGetPosEx) { RET(JOYERR_UNPLUGGED); }
IMP(winmm, midiInGetNumDevs) { RET(0); }
IMP(winmm, midiInGetDevCapsA) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, midiInOpen) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, midiInStart) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, midiInClose) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, midiOutOpen) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, midiOutClose) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, midiOutReset) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, midiOutShortMsg) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, midiOutLongMsg) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, midiOutPrepareHeader) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, midiOutUnprepareHeader) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, auxGetNumDevs) { RET(0); }
IMP(winmm, auxGetDevCapsA) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, auxGetVolume) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, auxSetVolume) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, mixerGetNumDevs) { RET(0); }
IMP(winmm, mixerOpen) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, mixerClose) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, mixerGetLineInfoA) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, mixerGetLineControlsA) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, mixerGetControlDetailsA) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, mixerSetControlDetails) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, mciSendCommandA) { RET(275); }   // MCIERR_DEVICE_NOT_INSTALLED
IMP(winmm, waveInOpen) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, waveInClose) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, waveInStart) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, waveInReset) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, waveInAddBuffer) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, waveInPrepareHeader) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, waveInUnprepareHeader) { RET(MMSYSERR_BADDEVICEID); }
// No wave output device yet (sound goes through DirectSound, when an app has it): opening one fails as on a machine without a sound card
IMP(winmm, waveOutOpen) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, waveOutGetDevCapsA) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, waveOutGetID) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, waveOutClose) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, waveOutReset) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, waveOutWrite) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, waveOutPrepareHeader) { RET(MMSYSERR_BADDEVICEID); }
IMP(winmm, waveOutUnprepareHeader) { RET(MMSYSERR_BADDEVICEID); }
