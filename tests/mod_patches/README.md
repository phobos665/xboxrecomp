# Mod patch regression

Applies patch files (`<mods>/patches/*.json`, see `src/kernel/mod_patches.h`)
to a fake guest memory buffer and checks what lands: each type's encoding,
arrays, hex bytes, the `expect` guard, the `title` filter, `enabled: false`,
addresses outside memory, the executable-section warning, and malformed files.

Asset-free and portable:

```bash
gcc -DMOD_PATCHES_NO_RUNTIME -Isrc/kernel tests/mod_patches/test_patches.c \
    src/kernel/mod_patches.c -lm -o /tmp/mp && /tmp/mp
```
