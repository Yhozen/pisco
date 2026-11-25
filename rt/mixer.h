// SPDX-License-Identifier: GPL-2.0-or-later
// The one sound output of a title (mixer.c): 48 kHz stereo float frames in a ring in shared memory that the host
// plays (an AudioWorklet in the browser; Node has no output). Audio libraries (XAudio2, FMOD 3) register a source
// that adds what they play into a block of frames; the mixer calls every source for each block.
#pragma once
#include "rt.h"

#define MIXER_RATE 48000
// Adds `frames` stereo frames into out (interleaved left, right). Called with mixer_lock held, on the mixer's
// thread (real time) or on the title's thread (a virtual clock).
typedef void (*MixerSource)(Cpu *c, float *out, u32 frames, void *ctx);
void mixer_add_source(MixerSource fn, void *ctx);
// Opens the ring for the host. With the host's clock a thread mixes, a little ahead of the wall clock. With a
// virtual clock (SW_CLOCK) the title's own thread mixes up to the title's clock whenever it moves
// (mixer_clock_moved), so what is mixed is a function of the title's calls alone; SW_AUDIO_WAV=<file> makes the
// host write it to a WAV file as well.
void mixer_start(void);
// What sources change: the mixer reads it with this lock held
extern pthread_mutex_t mixer_lock;
// kernel32.c calls it when the virtual clock moves (a frame presented, a Sleep): mixes up to the clock
void mixer_clock_moved(void);
