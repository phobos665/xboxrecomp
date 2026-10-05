/*
 * test_runtime -- xbox_mod_patches_apply end to end: the mods folder from the
 * path layer, the patches folder scanned in name order, the executable check
 * read from a real-shaped XBE header, and the writes landing in guest memory.
 */
#include "kernel.h"
#include "mod_patches.h"
#include "xbox_memory_layout.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  define make_dir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define make_dir(p) mkdir((p), 0755)
#endif

/* ---- stubs ---- */
static uint8_t *g_ram;
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)(uintptr_t)g_ram; }
void xbox_log(int level, const char *subsystem, const char *fmt, ...)
{
    (void)level; (void)subsystem; (void)fmt;
}
const char *xbox_LookupSymbolicLink(const char *link) { (void)link; return NULL; }
void xbox_watch_note_path(const char *path) { (void)path; }
uint32_t recomp_config_title_id(void) { return 0x4553000Au; }
const char *recomp_config_lookup(const char *env_name, const char *key)
{
    const char *v = env_name ? getenv(env_name) : NULL;
    (void)key;
    return (v && v[0]) ? v : NULL;
}

static char root[512];
static int failures;

static void put(const char *rel, const char *text)
{
    char p[1024];
    FILE *f;
    snprintf(p, sizeof p, "%s/%s", root, rel);
    f = fopen(p, "wb");
    if (!f) { printf("cannot write %s\n", p); exit(2); }
    fputs(text, f);
    fclose(f);
}

static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static void w32(uint32_t va, uint32_t v) { memcpy(g_ram + va, &v, 4); }

int main(void)
{
    char p[700];

    /* Guest memory as wide as the runtime's check allows, with an XBE
     * header at the base: one executable section at 0x11000-0x12000. */
    g_ram = calloc(1, XBOX_TOTAL_RAM);
    if (!g_ram) return 2;
    memcpy(g_ram + XBOX_BASE_ADDRESS, "XBEH", 4);
    w32(XBOX_BASE_ADDRESS + 0x11C, 1);                 /* section count */
    w32(XBOX_BASE_ADDRESS + 0x120, 0x10200);           /* section headers */
    w32(0x10200 + 0, 0x6);                             /* preload | executable */
    w32(0x10200 + 4, 0x11000);
    w32(0x10200 + 8, 0x1000);
    g_ram[0x20000] = 5;

#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        GetTempPathA(sizeof tmp, tmp);
        snprintf(root, sizeof root, "%smpr_%lu", tmp, (unsigned long)GetCurrentProcessId());
        make_dir(root);
    }
#else
    snprintf(root, sizeof root, "/tmp/mpr_XXXXXX");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
#endif
    snprintf(p, sizeof p, "%s/game", root); make_dir(p);
    snprintf(p, sizeof p, "%s/save", root); make_dir(p);
    snprintf(p, sizeof p, "%s/mods", root); make_dir(p);
    snprintf(p, sizeof p, "%s/mods/patches", root); make_dir(p);

    /* Name order decides: b_ runs after a_ and sees a_'s write. */
    put("mods/patches/a_first.json",
        "{ \"title\": \"4553000A\", \"patches\": ["
        " { \"address\": \"0x00020000\", \"type\": \"u8\", \"expect\": 5, \"value\": 6 } ] }");
    put("mods/patches/b_second.json",
        "{ \"patches\": ["
        " { \"address\": \"0x00020000\", \"type\": \"u8\", \"expect\": 6, \"value\": 7 },"
        " { \"address\": \"0x00011010\", \"type\": \"u8\", \"value\": 1 } ] }");
    put("mods/patches/c_other_title.json",
        "{ \"title\": \"4D530004\", \"patches\": ["
        " { \"address\": \"0x00020001\", \"type\": \"u8\", \"value\": 9 } ] }");
    put("mods/patches/readme.txt", "not a patch");

    snprintf(p, sizeof p, "%s/mods", root);
#if defined(_WIN32)
    _putenv_s("RECOMP_MODS_DIR", p);
#else
    setenv("RECOMP_MODS_DIR", p, 1);
#endif
    {
        char game[700], save[700];
        snprintf(game, sizeof game, "%s/game", root);
        snprintf(save, sizeof save, "%s/save", root);
        xbox_path_init(game, save);
    }

    xbox_mod_patches_apply();
    check(g_ram[0x20000] == 7, "files applied in name order (5 -> 6 -> 7)");
    check(g_ram[0x11010] == 1, "a patch into .text writes (with a warning)");
    check(g_ram[0x20001] == 0, "another title's file did nothing");

    g_ram[0x20000] = 5;
    xbox_mod_patches_apply();
    check(g_ram[0x20000] == 5, "applies once per process");

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    free(g_ram);
    return failures ? 1 : 0;
}
