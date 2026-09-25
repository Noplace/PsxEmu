# CHD support: the vendored libraries

`psx/disc.cpp` reads CHD disc images through these. They are other projects'
code, copied in unchanged and built as C with warnings off - by
`PSXEmu.Core.vcxproj` for the emulator, and into `Temp\tools\thirdparty.lib`
by `tools\build_tools.bat` for the harnesses.

| Directory | What | Licence | Taken from |
|---|---|---|---|
| `libchdr/` | libchdr: the CHD container, its map and its codecs, with `dr_flac` for FLAC | BSD 3-clause (`libchdr/LICENSE.txt`); dr_flac public domain / MIT-0 | DuckStation's `dep/libchdr` |
| `lzma/` | The LZMA SDK 24.08, just the decoder and the encoder libchdr takes its decoder settings from (`LzmaDec`, `LzmaEnc`, `LzFind`, `LzFindOpt`, `CpuArch`) | Public domain (`lzma/LICENSE`) | DuckStation's `dep/lzma` |
| `zlib/` | zlib 1.3.1, inflate for libchdr and deflate for `tools\chd_writer.h` | zlib licence (`zlib/LICENSE`) | vcpkg's clean source tree |
| `zstd_stub/` | Not zstd: see below | This project's | - |

Built with `Z7_ST`, so the LZMA encoder has no threading and needs nothing of
the SDK's threads code.

## zstd is not included

libchdr reads CHDs compressed with zstd - the `zstd` and `cdzs` codecs - and
includes `<zstd.h>` for them. chdman only uses those when asked; its default
for CDs is `cdlz`, `cdzl` and `cdfl` (LZMA, zlib and FLAC), which are all here.
Rather than vendor zstd for the rare image that needs it, `zstd_stub/` declares
the few functions libchdr calls and makes every one fail. A zstd CHD fails to
open, and `Disc::open_error()` says why. libchdr also prints `NO DSTREAM
CREATED!` to stdout when that happens; that is its own message.

To support zstd, replace `zstd_stub/` with zstd's `lib/` directory and build
its `decompress/` and `common/` sources in its place.
