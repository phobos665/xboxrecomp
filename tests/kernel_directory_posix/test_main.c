/*
 * kernel_directory_posix - closing a directory handle releases its search.
 *
 * The POSIX file backend keeps a directory search (an open DIR* and a slot in
 * a 64-entry table) per handle. Before this was fixed it was released only
 * when a search ran to its end, so a title that stops at the first match kept
 * the DIR* open, and host handle values are reused: the next directory opened
 * at the same value carried on the old search. 64 abandoned searches filled
 * the table for good.
 *
 * Checked here, with the file backend included so its private table can be
 * inspected: xbox_NtClose drops the search, over more open/search/close
 * cycles than the table has slots, and xbox_file_handle_closing -- what the
 * kernel bridge's NtClose (ordinal 187) calls before closing the host handle
 * itself -- does the same. No game files; a temporary directory only.
 */
#include "kernel_file.c"   /* the backend, for its private search table */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void *recomp_lookup(ULONG address) { (void)address; return NULL; }
void *recomp_lookup_manual(ULONG address) { (void)address; return NULL; }

static unsigned active_searches(void)
{
    unsigned n = 0;
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++)
        if (s_dir_contexts[i].handle)
            n++;
    return n;
}

static HANDLE open_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY);
    return fd < 0 ? INVALID_HANDLE_VALUE : w32_open_handle(fd, dir);
}

static NTSTATUS first_entry(HANDLE h)
{
    char buf[1024];
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_ANSI_STRING pattern = { 1, 2, (PCHAR)"*" };
    return xbox_NtQueryDirectoryFile(h, NULL, NULL, NULL, &ios, buf, sizeof buf,
                                     XboxFileDirectoryInformation, &pattern, TRUE);
}

int main(void)
{
    char dir[] = "/tmp/xboxrecomp-dirtest-XXXXXX";
    char path[sizeof dir + 8];
    int failures = 0;

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/f%d", dir, i);
        close(open(path, O_CREAT | O_WRONLY, 0644));
    }

    for (int round = 0; round < MAX_DIR_CONTEXTS + 8 && !failures; round++) {
        HANDLE h = open_dir(dir);
        NTSTATUS st = first_entry(h);
        if (st != STATUS_SUCCESS || active_searches() != 1) {
            printf("FAIL round %d: status %08X, %u searches open\n",
                   round, (unsigned)st, active_searches());
            failures++;
        }
        if (xbox_NtClose(h) != STATUS_SUCCESS || active_searches() != 0) {
            printf("FAIL round %d: %u searches left after NtClose\n",
                   round, active_searches());
            failures++;
        }
    }

    {
        HANDLE h = open_dir(dir);
        first_entry(h);
        xbox_file_handle_closing(h);
        if (active_searches() != 0) {
            printf("FAIL: %u searches left after xbox_file_handle_closing\n",
                   active_searches());
            failures++;
        }
        CloseHandle(h);
    }

    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/f%d", dir, i);
        unlink(path);
    }
    rmdir(dir);
    if (failures)
        return 1;
    printf("PASS: %d open/search/close cycles and the bridge's close path; "
           "no search outlives its handle\n", MAX_DIR_CONTEXTS + 8);
    return 0;
}
