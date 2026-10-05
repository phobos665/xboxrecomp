/*
 * test_mods -- the mods overlay, with no game.
 *
 * Builds a disc folder and a mods folder in a temporary directory, then asks
 * the path layer for the host paths a title's opens would reach.
 */
#include "kernel.h"
#include "recomp_config.h"

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

/* ---- stubs for what kernel_path.c calls outside itself ---- */

void xbox_log(int level, const char *subsystem, const char *fmt, ...)
{
    (void)level; (void)subsystem; (void)fmt;
}
const char *xbox_LookupSymbolicLink(const char *link) { (void)link; return NULL; }
void xbox_watch_note_path(const char *path) { (void)path; }
uint32_t recomp_config_title_id(void) { return 0; }
const char *recomp_config_lookup(const char *env_name, const char *key)
{
    const char *v = env_name ? getenv(env_name) : NULL;
    (void)key;
    return (v && v[0]) ? v : NULL;
}

/* ---- helpers ---- */

static int failures;
static char root[512];

static void set_env(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

static void write_file(const char *rel)
{
    char p[1024];
    FILE *f;

    snprintf(p, sizeof p, "%s/%s", root, rel);
    f = fopen(p, "wb");
    if (!f) { printf("  cannot create %s\n", p); exit(2); }
    fputs("x", f);
    fclose(f);
}

static void mkdir_rel(const char *rel)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    make_dir(p);
}

/* The host path for an Xbox path, as UTF-8 with '/' separators. */
static const char *host(const char *xbox_path)
{
    static char out[1024];
    xbox_host_char buf[MAX_PATH];
    char *c;

    if (!xbox_translate_path(xbox_path, buf, MAX_PATH))
        return "(failed)";
#if defined(_WIN32)
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof out, NULL, NULL);
#else
    snprintf(out, sizeof out, "%s", buf);
#endif
    for (c = out; *c; c++)
        if (*c == '\\') *c = '/';
    return out;
}

static void expect(const char *xbox_path, const char *rel_want)
{
    char want[1024], root_fwd[512];
    const char *got;
    char *c;

    snprintf(root_fwd, sizeof root_fwd, "%s", root);
    for (c = root_fwd; *c; c++)
        if (*c == '\\') *c = '/';
    snprintf(want, sizeof want, "%s/%s", root_fwd, rel_want);
    got = host(xbox_path);
    if (strcmp(got, want) != 0) {
        printf("  FAIL: %s\n        got  %s\n        want %s\n", xbox_path, got, want);
        failures++;
    } else {
        printf("  ok    %s -> %s\n", xbox_path, rel_want);
    }
}

static void init_paths(void)
{
    char game[600], save[600];
    snprintf(game, sizeof game, "%s/game", root);
    snprintf(save, sizeof save, "%s/save", root);
    xbox_path_init(game, save);
}

int main(void)
{
    char mods[600];

#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        GetTempPathA(sizeof tmp, tmp);
        snprintf(root, sizeof root, "%skpm_%lu", tmp, (unsigned long)GetCurrentProcessId());
        make_dir(root);
    }
#else
    snprintf(root, sizeof root, "/tmp/kpm_XXXXXX");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
#endif

    mkdir_rel("game");
    mkdir_rel("game/data");
    mkdir_rel("save");
    mkdir_rel("mods");
    mkdir_rel("mods/data");
    mkdir_rel("mods/music");      /* a directory where the disc has a file */
    write_file("game/data/chr.pak");
    write_file("game/data/gun.pak");
    write_file("game/music");
    write_file("mods/data/chr.pak");

    snprintf(mods, sizeof mods, "%s/mods", root);
    set_env("RECOMP_MODS_DIR", mods);
    init_paths();

    printf("overlay on:\n");
    expect("D:\\data\\chr.pak", "mods/data/chr.pak");
    expect("d:\\data\\chr.pak", "mods/data/chr.pak");
    expect("\\Device\\CdRom0\\data\\chr.pak", "mods/data/chr.pak");
    expect("\\??\\D:\\data\\chr.pak", "mods/data/chr.pak");
    expect("D:\\data/chr.pak", "mods/data/chr.pak");
    expect("D:\\data\\gun.pak", "game/data/gun.pak");        /* no mod file */
    expect("D:\\music", "game/music");                       /* mod is a dir */
    expect("D:\\data", "game/data");                         /* dirs are not overlaid */
    /* Partition1 shares the game directory but holds saves: never overlaid. */
    expect("\\Device\\Harddisk0\\Partition1\\data\\chr.pak", "game/data/chr.pak");

    printf("overlay off:\n");
    set_env("RECOMP_MODS_DIR", "off");
    init_paths();
    expect("D:\\data\\chr.pak", "game/data/chr.pak");

    printf("named folder missing:\n");
    set_env("RECOMP_MODS_DIR", "/no/such/mods/folder");
    init_paths();
    expect("D:\\data\\chr.pak", "game/data/chr.pak");

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
