# The Linux platform: `linux-x64`

Stable. `base/linux-x64` runs statically linked x86-64 Linux programs, at their system calls.

## `[run]`

| Key | Type | Required | Means |
|---|---|---|---|
| `exe` | string | yes | the program the app's first process runs: `/usr/bin/x`. It may be a script, run through its `#!` |
| `args` | list of strings | no | its arguments after `argv[0]` |

There is no `display`: a Linux app has no screen of its own; what it shows is what it prints.

## The drive

- The drive is the root, `/`. Paths in a recipe are written `/...`.
- Every ELF among the app's files is a program the app may `execve`. A builder may compile each ahead of time; the
  image says which module holds which program ([image.md](../image.md#the-config)).
- See [image.md](../image.md#files) for case, modes and links.
- What the app writes is kept by the host, apart from the image.

## Text

`[files]` are written with LF line ends.
