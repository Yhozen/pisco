# cowsay (2026-11-27)

The third Linux app: [cowsay](https://github.com/cowsay-org/cowsay) 3.8.4 (GPL-3.0 or Artistic), a Perl script, on
perl 5.44.0, both built from their sources by `tools/build-cowsay.sh` into `out/cowsay/root`; nothing of them is in
the repository. It was ported on a boat.dev sandbox, as the first task to measure that way of working
([dev-machines.md](dev-machines.md)).

## The files

| | |
|---|---|
| Sources | `perl-5.44.0.tar.gz` sha256 `3b855066…96c3`, cowsay `v3.8.4.tar.gz` `c15bc107…9add` (in full in `tools/build-cowsay.sh`) |
| perl | static, with musl; its XS modules built in, but not the large ones cowsay does not use (Encode, Unicode::Collate, compression, digests, threads, sockets, Storable, `B`; Time::HiRes, whose probes do not link statically); its library in `/usr/lib/perl5` without the documentation |
| cowsay | `make install prefix=/usr`: `/usr/bin/cowsay`, `cowthink` (a copy: it reads its name), 51 cows in `/usr/share/cowsay/cows` |
| `/usr/bin/env` | busybox (from `tools/build-neofetch.sh`), which cowsay's `#!/usr/bin/env perl` names |
| Image | perl's module is large: 34 MB for 1.49 million lifted instructions (`Perl_yyparse` alone has hundreds of cases); the root 46 MB, most of it perl's Unicode tables |

## Build it and run it

```sh
tools/build-cowsay.sh                          # out/cowsay/root
target/release/pisco build recipes/cowsay      # out/images/cowsay
target/release/pisco run out/images/cowsay                         # "Hello from PISCO", the recipe's argument
target/release/pisco run out/images/cowsay -- -f tux "lifted"      # arguments given replace the image's
SW_EXE=/usr/bin/cowthink target/release/pisco run out/images/cowsay -- "hmm"
SCREENSHOT=out/cowsay.png node tools/run-chrome.mjs out/images/cowsay -f dragon moo
```

`tests/run.sh cowsay` runs six of them (the default cow, `-f tux`, other eyes and tongue with wrapping at 20 columns,
`-f dragon -b`, cowthink, `-l`) in strict mode; each must print what the root's own perl prints when run natively on
the machine that builds it, and the first the same in headless Chrome.

## What it took

Nothing in the runtime: perl runs as bash and busybox do. The lifter needed `notrack` jumps through a table taken as
switches (gcc 13 marks them so), else perl's parser loop became a recursion deep enough to overflow a browser
worker's stack; perl's build needed to be told where musl's C library is on Ubuntu.

## Limits

- perl's module is 34 MB: some code is decoded into more than one lifted function. Making functions share it is
  the next thing for the size of large programs.
- No `Time::HiRes`, no `Encode`; a script that needs them fails at its `use`.
