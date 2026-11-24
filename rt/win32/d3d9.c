// SPDX-License-Identifier: GPL-2.0-or-later
// Direct3D 9 for Windows desktop titles. Not there yet: creating it is reported and fails, which is how far the
// Win32 process milestone goes. The device (on the GPU ring, with shaders generated from fixed-function state)
// is the next milestones' work.
#include "../rt.h"

IMP(d3d9, Direct3DCreate9) { rt_log("Direct3DCreate9(SDK version %u): Direct3D 9 is not implemented yet", (u32)ARG(0)); RET(0); }
