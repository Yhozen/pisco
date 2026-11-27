# The PS5 platform: `ps5` (experimental)

**Experimental: this page may change in any pull request, without a new schema.**

`base/ps5` runs native PS5 homebrew executables (ELF type 0xFE10) against an imitation of the PS5's system libraries
([docs/ps5.md](../../ps5.md)).

Its apps are not built from recipes yet: `tools/build-ps5-raymarch.sh` builds the one there is. When they are, a
recipe's `[run]` is the Linux platform's (`exe`, `args`), and this page will say how its drive is laid out.
