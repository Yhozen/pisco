// SPDX-License-Identifier: GPL-2.0-or-later
// FMOD 3 (fmod.dll) for titles whose recipe says the runtime answers for it ([dll] "fmod.dll" = "builtin").
// For now this is FMOD on a machine with no sound device, which is what the oracle's machine is: the settings
// made before FSOUND_Init are accepted, FSOUND_Init fails, and what a title still calls afterwards answers as
// the real library does when it was never initialised (the return values are the ones Wine's +snoop trace of
// the shipped fmod.dll shows). A title then loads no sounds and runs silent. Real sound (the mixer on the
// host's audio output, samples and streams from the title's files) is a later milestone.
// FMOD's functions are stdcall, exported under their decorated names (_FSOUND_Init@12); the ones that return a
// truth value return it as a signed char, in AL.
#include "../rt.h"

// (window): the window DirectSound would attach to
IMP(fmod, FSOUND_SetHWND) { RET(1); }
IMP(fmod, FSOUND_SetMinHardwareChannels) { RET(1); }
// (pool, pool length, allocate, reallocate, free): FMOD would take its memory from the pool. Nothing is allocated yet.
IMP(fmod, FSOUND_SetMemorySystem) { RET(1); }
IMP(fmod, FSOUND_Init) { // (mix rate, software channels, flags)
  if (rt_trace) rt_log("FSOUND_Init(%u Hz, %u channels, flags 0x%x): no sound device yet, so it fails as on the oracle's machine", (u32)ARG(0), (u32)ARG(1), (u32)ARG(2));
  RET(0);
}
// Called by the title although FSOUND_Init failed: each fails, as in the library that was never initialised
IMP(fmod, FSOUND_Reverb_SetProperties) { RET(0); }   // (properties)
IMP(fmod, FSOUND_SetVolume) { RET(0); }              // (channel, volume)
IMP(fmod, FSOUND_3D_Listener_SetCurrent) { }         // (current, listeners): returns nothing
