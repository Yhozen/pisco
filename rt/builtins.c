// The compiler-rt helpers lifted code needs, for wasm32 and wasm64 alike.
// The empty asm statements stop LLVM from turning these back into the 128-bit operations they implement.
typedef unsigned long long u64; typedef unsigned int u32;
typedef unsigned __int128 u128; typedef __int128 s128;
#define OPAQUE(x) __asm__("" : "+r"(x))
static u64 mulhi(u64 a, u64 b) {
  u64 al = (u32)a, ah = a >> 32, bl = (u32)b, bh = b >> 32;
  OPAQUE(al); OPAQUE(ah); OPAQUE(bl); OPAQUE(bh);
  u64 t = al * bl, m1 = ah * bl + (t >> 32), m2 = al * bh + (u32)m1;
  return ah * bh + (m1 >> 32) + (m2 >> 32);
}
s128 __multi3(s128 a, s128 b) {
  u64 al = (u64)a, ah = (u64)((u128)a >> 64), bl = (u64)b, bh = (u64)((u128)b >> 64);
  u64 lo = al * bl, hi = mulhi(al, bl) + al * bh + ah * bl;
  return (s128)(((u128)hi << 64) | lo);
}
