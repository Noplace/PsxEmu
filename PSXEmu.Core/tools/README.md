# Headless test harnesses

Console builds of the emulation core, so it can be run and diffed from a shell
instead of by driving a GUI by hand. They take no window, no input and no audio
device.

Build them with `PSXEmu.Core\tools\build_tools.bat` (needs the MSVC command line
tools; edit the `vcvars64.bat` path at the top if your Visual Studio install
differs). Binaries land in `Temp\tools\`.

The batch file compiles the core sources directly rather than going through
MSBuild, so the harnesses stay independent of the solution configuration. It is
also the fastest way to get a compile error out of the core.

**Check counts, what each group covers, and the baselines live in
[Docs/Test-Suite.md](../../Docs/Test-Suite.md)**, not here, so there is one
place to keep current.

## What is here

| Harness | What it is |
|---|---|
| `boot_runner` | Boots a BIOS or a disc for N frames and reports everything - the checksum baselines, tracing, `--recompiler`, save states |
| `cpu_test` `gte_test` `gpu_test` `mdec_test` `timer_test` `sio_test` `spu_test` `media_test` | The eight emulation harnesses. Each takes an optional group name; `media_test` takes a work directory |
| `rec_test` | The recompiler, differentially against a reference interpreter |
| `rec_bench` | Recompiled against interpreted; a benchmark, asserts nothing |
| `host_test` | The threads and channels in `host/` |
| `frame_limiter_test` `speed_resampler_test` `letterbox_test` | The host-side headers the front end leans on |
| `wav_pitch` | The note in a WAV `boot_runner --wav` wrote |
| `make_test_disc` | Writes a synthetic disc image |

Every test harness exits 0 when everything passed.

## boot_runner

    boot_runner <bios.bin> [options]

The four numbers worth reading first:

- **non-black pixels** - separates "drew the wrong thing" from "never drew"
- **unimplemented paths hit** - how many `BREAKPOINT` markers the run reached
- **GPU tallies** - whether the GPU received anything at all
- **hardware registers touched** - how far into the boot the machine got

That last one is usually enough on its own. If the CD-ROM registers have no
accesses against them, the boot has not reached the disc check, and no amount
of staring at the framebuffer will tell you that.

When something is wrong, in rough order of usefulness:

    --hot 10                    where is it spending its time
    --dis <addr>:<n>            what is the code there
    --trace 40 --trace-skip N   what are the registers doing
    --vram out.ppm              is it drawing somewhere unexpected

`--dis` reads both RAM and the BIOS, so BIOS-resident code disassembles too.

## cpu_test

Four bugs turned up on this suite's first run, before it had been aimed at
anything: `0x80000000 / -1` was killing the host process, division by zero
returned the wrong values, bus errors were being decided on the virtual address
so KSEG1 register access failed, and `break` did nothing at all.
