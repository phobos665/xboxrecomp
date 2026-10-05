/*
 * test_patches -- a mod's tuning patches, against fake guest memory.
 */
#include "mod_patches.h"

#include <stdio.h>
#include <string.h>

#define BASE 0x00100000u
#define SIZE 0x1000u
#define EXEC_LO 0x00100800u   /* pretend [0x800, 0x900) is .text */
#define EXEC_HI 0x00100900u

static uint8_t mem[SIZE];
static int failures;

static uint8_t *guest(uint32_t va, uint32_t len, void *ctx)
{
    (void)ctx;
    if (va < BASE || len > SIZE || va - BASE > SIZE - len) return NULL;
    return mem + (va - BASE);
}

static int exec_section(uint32_t va, void *ctx)
{
    (void)ctx;
    return va >= EXEC_LO && va < EXEC_HI;
}

static mod_patch_result run(const char *json, uint32_t title)
{
    mod_patch_result r = { 0, 0, 0 };
    mod_patches_apply_text(json, "test.json", title, guest, exec_section, NULL, &r);
    return r;
}

static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static void check_result(mod_patch_result r, int a, int s, int e, const char *what)
{
    char line[200];
    snprintf(line, sizeof line, "%s (applied %d skipped %d errors %d)", what,
             r.applied, r.skipped, r.errors);
    check(r.applied == a && r.skipped == s && r.errors == e, line);
}

static float f32_at(uint32_t off) { float f; memcpy(&f, mem + off, 4); return f; }
static uint16_t u16_at(uint32_t off) { return (uint16_t)(mem[off] | mem[off + 1] << 8); }

int main(void)
{
    mod_patch_result r;
    float one = 1.0f;

    memset(mem, 0, sizeof mem);
    memcpy(mem + 0x10, &one, 4);
    mem[0x20] = 10; mem[0x22] = 20; mem[0x24] = 30;
    mem[0x30] = 0x74; mem[0x31] = 0x05;

    printf("types and guards:\n");
    r = run("{ \"patches\": ["
            "  { \"address\": \"0x00100010\", \"type\": \"f32\", \"expect\": 1.0, \"value\": 1.5 },"
            "  { \"address\": \"0x00100020\", \"type\": \"u16\", \"expect\": [10,20,30], \"value\": [12,24,36] },"
            "  { \"address\": \"0x00100030\", \"type\": \"bytes\", \"expect\": \"74 05\", \"value\": \"EB05\" },"
            "  { \"address\": 1048640, \"type\": \"i32\", \"value\": -2 },"
            "  { \"address\": \"0x00100050\", \"type\": \"f64\", \"value\": 0.25 }"
            "] }", 0);
    check_result(r, 5, 0, 0, "five patches apply");
    check(f32_at(0x10) == 1.5f, "f32 written");
    check(u16_at(0x20) == 12 && u16_at(0x22) == 24 && u16_at(0x24) == 36, "u16 array written");
    check(mem[0x30] == 0xEB && mem[0x31] == 0x05, "bytes written");
    check(mem[0x40] == 0xFE && mem[0x41] == 0xFF && mem[0x42] == 0xFF && mem[0x43] == 0xFF,
          "i32 -2 is little-endian two's complement");
    {
        double d; memcpy(&d, mem + 0x50, 8);
        check(d == 0.25, "f64 written");
    }

    printf("expect mismatch leaves memory alone:\n");
    r = run("{ \"patches\": [ { \"address\": \"0x00100010\", \"type\": \"f32\","
            " \"expect\": 1.0, \"value\": 9.0 } ] }", 0);
    check_result(r, 0, 1, 0, "mismatch is skipped");
    check(f32_at(0x10) == 1.5f, "memory unchanged");

    printf("an array is all or nothing:\n");
    r = run("{ \"patches\": [ { \"address\": \"0x00100020\", \"type\": \"u16\","
            " \"expect\": [12,24,99], \"value\": [1,2,3] } ] }", 0);
    check_result(r, 0, 1, 0, "one wrong element skips the whole patch");
    check(u16_at(0x20) == 12, "first element unchanged");

    printf("title filter:\n");
    r = run("{ \"title\": \"4553000A\", \"patches\": [ { \"address\": \"0x00100060\","
            " \"type\": \"u8\", \"value\": 7 } ] }", 0x4553000Au);
    check_result(r, 1, 0, 0, "matching title applies");
    r = run("{ \"title\": \"4D530004\", \"patches\": [ { \"address\": \"0x00100061\","
            " \"type\": \"u8\", \"value\": 7 } ] }", 0x4553000Au);
    check_result(r, 0, 0, 0, "another title's file is ignored quietly");
    check(mem[0x61] == 0, "nothing written for another title");

    printf("switched off, out of range, executable:\n");
    r = run("{ \"patches\": ["
            "  { \"address\": \"0x00100070\", \"type\": \"u8\", \"value\": 1, \"enabled\": false },"
            "  { \"address\": \"0x00100FFE\", \"type\": \"u32\", \"value\": 1 },"
            "  { \"address\": \"0x00000010\", \"type\": \"u8\", \"value\": 1 },"
            "  { \"address\": \"0x00100810\", \"type\": \"u8\", \"value\": 144 }"
            "] }", 0);
    check_result(r, 1, 2, 0, "disabled not counted; two outside memory skipped");
    check(mem[0x70] == 0, "disabled patch not written");
    check(mem[0x810] == 144, "an executable-section patch still writes (and warns)");

    printf("bad entries:\n");
    r = run("{ \"patches\": ["
            "  { \"address\": \"0x00100080\", \"type\": \"u8\", \"value\": 300 },"
            "  { \"address\": \"0x00100080\", \"type\": \"u9\", \"value\": 1 },"
            "  { \"address\": \"nowhere\", \"type\": \"u8\", \"value\": 1 },"
            "  { \"address\": \"0x00100080\", \"type\": \"u8\", \"value\": 1.5 },"
            "  { \"address\": \"0x00100080\", \"type\": \"u16\", \"expect\": [1], \"value\": [1,2] },"
            "  { \"address\": \"0x00100080\", \"type\": \"bytes\", \"value\": \"9G\" },"
            "  7"
            "] }", 0);
    check_result(r, 0, 0, 7, "seven malformed entries, each reported");
    check(mem[0x80] == 0, "none of them wrote");

    printf("malformed files:\n");
    r = run("{ \"patches\": [ { \"address\": \"0x00100090\", \"type\": \"u8\", \"value\": 1 } ", 0);
    check_result(r, 0, 0, 1, "truncated file");
    r = run("{ \"patch\": [] }", 0);
    check_result(r, 0, 0, 1, "no patches array");
    r = run("{ \"patches\": [] } trailing", 0);
    check_result(r, 0, 0, 1, "text after the end");
    r = run("{ \"note\": \"tab\\tand \\u0041\", \"patches\": [] }", 0);
    check_result(r, 0, 0, 0, "escapes parse");

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
