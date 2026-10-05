# Mods overlay regression

Checks that a file under the mods folder replaces the disc file at the same
relative path, and only where it should: disc paths (`D:\`, `\Device\CdRom0\`)
are overlaid, `Partition1` (which shares the game directory and holds saves)
is not, a directory in the mods folder does not shadow a file lookup, and
`RECOMP_MODS_DIR=off` switches it off.

Asset-free and portable: it compiles `src/kernel/kernel_path.c` alone against
stubs, so it runs on Linux too.

```bash
cmake -S . -B build/tests && cmake --build build/tests --target kernel_path_mods
ctest --test-dir build/tests -R kernel_path_mods --output-on-failure
```

Or directly on Linux:

```bash
gcc -Isrc/kernel -Isrc/config -Isrc/platform -Iinclude -Isrc \
    tests/kernel_path_mods/test_mods.c src/kernel/kernel_path.c -o /tmp/kpm && /tmp/kpm
```
