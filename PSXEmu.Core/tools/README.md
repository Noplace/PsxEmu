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

## boot_runner

    boot_runner <bios.bin> [options]

Boots a BIOS dump and runs for a given number of frames, then reports what
happened. See [Docs/Test-Suite.md](../../Docs/Test-Suite.md) for the full option
list and the current baselines.

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

## media_test

    media_test [work-directory]

Protocol-level tests for the disc layer and the CD-ROM controller, with no
BIOS, no window, and no disc of its own - it writes the images it needs into
the work directory and removes them afterwards. Exit code 0 if all checks
passed.

Currently 56 checks. See [Docs/Test-Suite.md](../../Docs/Test-Suite.md) for
what each group covers and why it is worth testing this way.

## Still to write

- **`gte_test`** - amidog's GTE suite, needed from the day GTE work starts.
- **`cpu_test`** - amidog's CPU suite, as the regression gate.
- **Memory card round trips** in `media_test`, once cards exist - write, wipe,
  read back. The wipe is the point.
