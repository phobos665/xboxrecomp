/*
 * launch_chain.c - following XLaunchNewImage into another recompiled image
 *
 * A title that ships two XBEs hands over between them with XLaunchNewImage:
 * XAPI fills the launch data page (type, title id, the path of the image to
 * run, and 3 KB of the title's own data) and calls HalReturnToFirmware(2),
 * a quick reboot, after which the kernel starts the named image with that
 * page intact. 007: Nightfire is built this way -- default.xbe is Eurocom's
 * shooter and its front end, Driving.xbe is the driving missions -- and
 * pressing START on its title screen launches driving.xbe with the data
 * "Default". Until this file existed that was "the title chose to quit".
 *
 * Here each XBE is its own recompiled executable, so the launch becomes a
 * process: the page is written to a file, the sibling executable is started
 * with RECOMP_LAUNCH_DATA_FILE naming it (kernel_data_init reads it back
 * into a guest page before the title runs), and this process waits for it
 * and exits with its code, so a harness watching one process still sees the
 * whole session. The child is put in a job object that dies with this
 * process, so a run killed from outside takes the child with it.
 *
 * Which executable a name maps to is configuration, not something the
 * runtime can guess: RECOMP_LAUNCH_MAP ("driving.xbe=C:\...\x.exe;default.xbe=
 * C:\...\y.exe"), else launch_map.txt beside the executable, one "name.xbe =
 * path" per line, relative paths against the executable's directory. The
 * name is compared without case. With no mapping the title exits as before,
 * and the log says what it wanted.
 */
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "kernel.h"
#include "xbox_memory_layout.h"

extern ptrdiff_t g_xbox_mem_offset;
#define CHAIN_MEM8(va)  (*(volatile uint8_t  *)((uintptr_t)(va) + g_xbox_mem_offset))
#define CHAIN_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))

/* The file name of the image this process is running, for XeImageFileName
 * and for the log. The template's main.c sets it before xbox_kernel_init;
 * a title built before that keeps the default. */
static char g_image_name[64] = "default.xbe";

void xbox_SetImageFileName(const char *name)
{
    const char *base = name;
    const char *p;

    if (!name || !*name)
        return;
    for (p = name; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ';')
            base = p + 1;
    if (*base)
        snprintf(g_image_name, sizeof g_image_name, "%s", base);
}

const char *xbox_ImageFileName(void)
{
    return g_image_name;
}

static void lower(char *s)
{
    for (; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

/* The executable mapped to `xbe` (lower case), or 0 with out[0] = 0. */
static int find_target(const char *xbe, char *out, size_t out_bytes)
{
    const char *map = getenv("RECOMP_LAUNCH_MAP");
    char exe_dir[MAX_PATH];
    char line[1024];
    FILE *f;

    out[0] = 0;
    if (map && *map) {
        const char *p = map;
        while (*p) {
            const char *eq = strchr(p, '=');
            const char *end = strchr(p, ';');
            size_t klen;
            char key[128];

            if (!end) end = p + strlen(p);
            if (!eq || eq > end) { p = *end ? end + 1 : end; continue; }
            klen = (size_t)(eq - p);
            if (klen >= sizeof key) klen = sizeof key - 1;
            memcpy(key, p, klen); key[klen] = 0;
            lower(key);
            if (strcmp(key, xbe) == 0) {
                size_t vlen = (size_t)(end - (eq + 1));
                if (vlen >= out_bytes) vlen = out_bytes - 1;
                memcpy(out, eq + 1, vlen); out[vlen] = 0;
                return 1;
            }
            p = *end ? end + 1 : end;
        }
    }

    if (!GetModuleFileNameA(NULL, exe_dir, sizeof exe_dir))
        return 0;
    {
        char *slash = strrchr(exe_dir, '\\');
        if (slash) *slash = 0;
    }
    snprintf(line, sizeof line, "%s\\launch_map.txt", exe_dir);
    f = fopen(line, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line, *eq, *v, *e;
        char key[128];
        size_t klen;

        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || !*s) continue;
        eq = strchr(s, '=');
        if (!eq) continue;
        e = eq;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        klen = (size_t)(e - s);
        if (klen >= sizeof key) klen = sizeof key - 1;
        memcpy(key, s, klen); key[klen] = 0;
        lower(key);
        if (strcmp(key, xbe) != 0) continue;
        v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        e = v + strlen(v);
        while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) e--;
        *e = 0;
        if (v[0] && v[1] == ':')          /* absolute */
            snprintf(out, out_bytes, "%s", v);
        else
            snprintf(out, out_bytes, "%s\\%s", exe_dir, v);
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

/* Follow the launch the title asked for. Returns the child's exit code, or
 * -1 when there is nothing to follow (no path, no mapping, or the child
 * could not be started), in which case the caller exits as it would have. */
int xbox_LaunchChain(uint32_t page_va)
{
    char path[520], name[128], target[MAX_PATH], data_file[MAX_PATH];
    char cmdline[MAX_PATH + 4], cwd[MAX_PATH];
    uint8_t page[4096];
    const char *base;
    uint32_t i;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE job;
    DWORD code = 1;
    FILE *f;

    if (!page_va)
        return -1;
    for (i = 0; i < sizeof(path) - 1; i++) {
        uint8_t c = CHAIN_MEM8(page_va + 8 + i);
        if (!c) break;
        path[i] = (char)c;
    }
    path[i] = 0;
    if (!path[0])
        return -1;

    /* "\Device\CdRom0;driving.xbe", "D:\driving.xbe", "driving.xbe" */
    base = path;
    for (i = 0; path[i]; i++)
        if (path[i] == '\\' || path[i] == '/' || path[i] == ';')
            base = path + i + 1;
    snprintf(name, sizeof name, "%s", base);
    lower(name);
    if (!name[0])
        return -1;

    if (!find_target(name, target, sizeof target)) {
        fprintf(stderr, "[LAUNCH] the title asked to run '%s' (%s); no executable is mapped "
                        "to it (RECOMP_LAUNCH_MAP, or launch_map.txt beside this one), so "
                        "this is where the run ends\n", name, path);
        fflush(stderr);
        return -1;
    }
    if (GetFileAttributesA(target) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "[LAUNCH] '%s' is mapped to %s, which does not exist\n", name, target);
        fflush(stderr);
        return -1;
    }

    for (i = 0; i < sizeof page; i++)
        page[i] = CHAIN_MEM8(page_va + i);
    {
        char tmp[MAX_PATH];
        DWORD n = GetTempPathA(sizeof tmp, tmp);
        if (!n || n >= sizeof tmp)
            snprintf(tmp, sizeof tmp, ".\\");
        snprintf(data_file, sizeof data_file, "%sxboxrecomp_launch_%lu.bin", tmp,
                 (unsigned long)GetCurrentProcessId());
    }
    f = fopen(data_file, "wb");
    if (!f || fwrite(page, 1, sizeof page, f) != sizeof page) {
        if (f) fclose(f);
        fprintf(stderr, "[LAUNCH] could not write the launch data to %s\n", data_file);
        fflush(stderr);
        return -1;
    }
    fclose(f);

    SetEnvironmentVariableA("RECOMP_LAUNCH_DATA_FILE", data_file);
    SetEnvironmentVariableA("RECOMP_LAUNCH_FROM", g_image_name);

    /* The child inherits the frame-dump prefix and would overwrite this
     * process's captures with its own: give it the image name as a suffix
     * ("…/chain1" -> "…/chain1-driving"). */
    {
        char dump[MAX_PATH], tagged[MAX_PATH + 64], stem[64];
        DWORD n = GetEnvironmentVariableA("RECOMP_HLE_D3D8_DUMP", dump, sizeof dump);
        if (n && n < sizeof dump) {
            char *dot;
            snprintf(stem, sizeof stem, "%s", name);
            dot = strrchr(stem, '.');
            if (dot) *dot = 0;
            snprintf(tagged, sizeof tagged, "%s-%s", dump, stem);
            SetEnvironmentVariableA("RECOMP_HLE_D3D8_DUMP", tagged);
        }
    }

    fprintf(stderr, "[LAUNCH] %s -> %s: type %u, title 0x%08X, data %02X %02X %02X %02X "
                    "%02X %02X %02X %02X...; running %s and waiting for it\n",
            g_image_name, name, CHAIN_MEM32(page_va), CHAIN_MEM32(page_va + 4),
            page[1024], page[1025], page[1026], page[1027],
            page[1028], page[1029], page[1030], page[1031], target);
    fflush(stderr);

    snprintf(cmdline, sizeof cmdline, "\"%s\"", target);
    snprintf(cwd, sizeof cwd, "%s", target);
    {
        char *slash = strrchr(cwd, '\\');
        if (slash) *slash = 0;
    }
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);

    /* The child dies with this process: a run killed from outside must not
     * leave the second half of the title running in a window of its own. */
    job = CreateJobObjectA(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
        memset(&lim, 0, sizeof lim);
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
    }
    if (!CreateProcessA(target, cmdline, NULL, NULL, TRUE, CREATE_SUSPENDED,
                        NULL, cwd, &si, &pi)) {
        fprintf(stderr, "[LAUNCH] CreateProcess failed (error %lu) for %s\n",
                (unsigned long)GetLastError(), target);
        fflush(stderr);
        DeleteFileA(data_file);
        if (job) CloseHandle(job);
        return -1;
    }
    if (job && !AssignProcessToJobObject(job, pi.hProcess))
        fprintf(stderr, "[LAUNCH] note: the child is not in a job (error %lu); "
                        "killing this process will not stop it\n",
                (unsigned long)GetLastError());
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    fprintf(stderr, "[LAUNCH] %s finished with code 0x%08lX\n", name, (unsigned long)code);
    fflush(stderr);
    return (int)code;
}

#endif /* _WIN32 */
