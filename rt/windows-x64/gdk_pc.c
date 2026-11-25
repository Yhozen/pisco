// SPDX-License-Identifier: GPL-2.0-or-later
// The PC GDK's XGameRuntime objects (QueryApiImpl): how a Windows desktop title reaches XGameRuntime.
#include "../gdk/gdk.h"
// the console-style functions (rt/gdk/xgameruntime.c) its methods forward to
IMP(xgameruntime, XAsyncGetStatus); IMP(xgameruntime, XAsyncRun); IMP(xgameruntime, XGameGetXboxTitleId); IMP(xgameruntime, XSystemGetAnalyticsInfo); IMP(xgameruntime, XSystemGetXboxLiveSandboxId); IMP(xgameruntime, XThreadAssertNotTimeSensitive);

// ---- the PC GDK's XGameRuntime: xgameruntime.lib (linked into a desktop title) loads xgameruntime.dll, checks its
// version through InitializeApiImpl*, then reaches every API as a method of a COM object it asks QueryApiImpl for
// (a class id and an interface id per API family). Objects we model are in pc_apis; others are refused. ----
static void guid_str(u64 g, char *out) {
  snprintf(out, 40, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", M32(g), M16(g + 4), M16(g + 6), M8(g + 8), M8(g + 9), M8(g + 10), M8(g + 11), M8(g + 12), M8(g + 13), M8(g + 14), M8(g + 15));
}
IMP(xgameruntime, InitializeApiImpl) { RET(S_OK); }
IMP(xgameruntime, InitializeApiImplEx) { RET(S_OK); }
IMP(xgameruntime, InitializeApiImplEx2) { rt_log("XGameRuntime (PC): initialised (game version 0x%llx, SDK 0x%llx)", ARG(0), ARG(1)); RET(S_OK); }
IMP(xgameruntime, UninitializeApiImpl) { RET(S_OK); }
IMP(xgameruntime, XErrorReport) { rt_log("XGameRuntime: XErrorReport 0x%08x %s", (u32)ARG(0), ARG(1) ? GSTR(ARG(1)) : ""); }
// the API families the PC library asks for, as its thunks use them (facts read from a desktop title's code:
// class id, interface id, method slots); slots 0-2 are IUnknown's
typedef struct { const char *clsid, *iid; const char *methods[32]; } PcApi;
static const PcApi pc_apis[] = {
  {"073b7dcb-1fcf-4030-94be-e3c9eb623428", "073b7dcb-1fcf-4030-94be-e3c9eb623428", {[3] = "PcAsync_GetStatus", [6] = "PcAsync_Run", [29] = "PcThread_AssertNotTimeSensitive"}},
  {"973a344e-24bf-4d0f-8457-56c534892b29", "2549f142-6419-4a06-97b5-931aab7c2f34", {[3] = "PcGame_GetXboxTitleId"}},
  {"b884675d-b738-4a9c-815d-9a9a1e0c6c9b", "b884675d-b738-4a9c-815d-9a9a1e0c6c9b", {[3] = "PcSystem_GetAnalyticsInfo"}},
  {"e349bd1a-fc20-4e40-b99c-4178cc6b409f", "1861cf2e-e18b-4834-a9f5-b4a4e6efb4cf", {[4] = "PcSystem_GetXboxLiveSandboxId"}},
};
IMP(xgameruntime, QueryApiImpl) { // (class id, interface id, &object)
  char a[40], b[40]; guid_str(ARG(0), a); guid_str(ARG(1), b);
  static u64 objs[sizeof pc_apis / sizeof *pc_apis];
  for (unsigned k = 0; k < sizeof pc_apis / sizeof *pc_apis; k++) {
    if (strcmp(a, pc_apis[k].clsid) || strcmp(b, pc_apis[k].iid)) continue;
    if (!objs[k]) {
      const char *names[32]; const char *prefix = "";
      for (int i = 0; i < 32; i++) names[i] = i == 0 ? "Pc_QueryInterface" : i == 1 ? "Pc_AddRef" : i == 2 ? "Pc_Release" : pc_apis[k].methods[i];
      u64 o = (uptr)calloc(1, 16); M64(o) = rt_com_vtable("xgameruntime", prefix, names, 32); objs[k] = o;
    }
    if (ARG(2)) M64(ARG(2)) = objs[k];
    RET(S_OK); return;
  }
  rt_log("XGameRuntime (PC): QueryApiImpl %s / %s: not modelled", a, b);
  if (ARG(2)) M64(ARG(2)) = 0;
  RET(0x80040154u); // REGDB_E_CLASSNOTREG
}
// a method call on such an object: the console function with the object pointer taken out of the arguments
static void drop_this(Cpu *c) {
  R[RCX] = R[RDX]; R[RDX] = R[8]; R[8] = R[9]; R[9] = M64(R[RSP] + 0x28);
  for (int k = 4; k < 10; k++) M64(R[RSP] + 8 + 8 * k) = M64(R[RSP] + 16 + 8 * k);
}
IMP(xgameruntime, Pc_QueryInterface) { if (ARG(2)) M64(ARG(2)) = ARG(0); RET(S_OK); }
IMP(xgameruntime, Pc_AddRef) { RET(2); }
IMP(xgameruntime, Pc_Release) { RET(1); } // one object per family, kept
IMP(xgameruntime, PcAsync_GetStatus) { drop_this(c); imp_xgameruntime_XAsyncGetStatus(c); }
IMP(xgameruntime, PcAsync_Run) { drop_this(c); imp_xgameruntime_XAsyncRun(c); }
IMP(xgameruntime, PcThread_AssertNotTimeSensitive) { drop_this(c); imp_xgameruntime_XThreadAssertNotTimeSensitive(c); }
IMP(xgameruntime, PcGame_GetXboxTitleId) { drop_this(c); imp_xgameruntime_XGameGetXboxTitleId(c); }
IMP(xgameruntime, PcSystem_GetXboxLiveSandboxId) { drop_this(c); imp_xgameruntime_XSystemGetXboxLiveSandboxId(c); }
// the PC method fills the caller's structure; the console function returns it through a hidden pointer: the same call
IMP(xgameruntime, PcSystem_GetAnalyticsInfo) { drop_this(c); imp_xgameruntime_XSystemGetAnalyticsInfo(c); }
