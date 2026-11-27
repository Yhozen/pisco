# The Windows platforms: `windows-x86`, `windows-x64`, `xbox-series`

Stable. Three bases, one way of naming files and starting an app: `base/windows-x86` (32-bit Windows desktop of the
2000s, as Windows XP SP3), `base/windows-x64` (64-bit Windows desktop, as Windows 10) and `base/xbox-series` (the
GDK's console build, whose kernel is a Windows one).

## `[run]`

| Key | Type | Required | Means |
|---|---|---|---|
| `exe` | string | yes | the program the app starts with: `C:\...\x.exe` |
| `display` | string | no | the screen's size, `<width>x<height>` (a full-screen app's mode, a window's client area) |
| `args` | list of strings | no | its command line after the program's name |

## The drive

- One drive, `C:`. Paths in a recipe are written `C:\...`; on the drive they are relative to `C:\`.
- Names are matched without regard to case (ASCII), as Windows does; a name keeps the case it was placed with.
- Files have no modes and no links: every layer's file is a plain file.
- The base's layer holds `C:\WINDOWS` (with a placeholder in `system32` for every DLL the base answers for) and the
  user's folders; its own page in `bases/` lists them ([base.md](../base.md)).
- The app's folder (`{app}`) is the folder of `run.exe` unless the first extract step says otherwise. Its working
  folder at start is that folder.
- What the app writes is kept by the host, apart from the image, keyed by the app's `name`.

## Text

`[files]` are written with CR LF line ends. The registry's values are the base's, then the recipe's (which win),
matched without regard to case.
