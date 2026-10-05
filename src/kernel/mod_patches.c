/*
 * mod_patches.c -- tuning changes a mod makes to the title's own tables.
 *
 * See mod_patches.h for the file format and what it can and cannot do.
 *
 * The parser is a small JSON reader of its own, like the input bindings'
 * (src/input/input_bindings.c): the runtime has no JSON library, and a
 * patch file is a handful of flat objects. It accepts standard JSON and
 * nothing else, so a file the tools write reads here unchanged.
 */
#include "mod_patches.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ JSON tree */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jnode {
    jtype         type;
    double        num;      /* J_NUM, J_BOOL */
    char         *str;      /* J_STR value; member key when in an object */
    char         *key;
    struct jnode *child;    /* J_ARR, J_OBJ: first element */
    struct jnode *next;     /* next sibling */
} jnode;

typedef struct {
    const char *p;
    const char *err;
    int         depth;
} jparser;

static void jfree(jnode *n)
{
    while (n) {
        jnode *next = n->next;
        jfree(n->child);
        free(n->str);
        free(n->key);
        free(n);
        n = next;
    }
}

static void jws(jparser *jp)
{
    while (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r')
        jp->p++;
}

static char *jstring_raw(jparser *jp)
{
    const char *s = jp->p + 1;   /* past the opening quote */
    size_t cap = 16, len = 0;
    char *out = malloc(cap);

    if (!out) { jp->err = "out of memory"; return NULL; }
    while (*s && *s != '"') {
        char c = *s++;
        if ((unsigned char)c < 0x20) { jp->err = "control character in string"; break; }
        if (c == '\\') {
            c = *s++;
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                /* Patch files are ASCII in practice; keep \u00XX, refuse more. */
                unsigned v = 0;
                int i;
                for (i = 0; i < 4; i++) {
                    if (!isxdigit((unsigned char)s[i])) { jp->err = "bad \\u escape"; break; }
                    v = v * 16 + (unsigned)(isdigit((unsigned char)s[i]) ? s[i] - '0'
                                            : (tolower((unsigned char)s[i]) - 'a' + 10));
                }
                if (jp->err) break;
                if (v > 0x7F) { jp->err = "non-ASCII \\u escape"; break; }
                s += 4;
                c = (char)v;
                break;
            }
            default:
                jp->err = "bad escape";
            }
            if (jp->err) break;
        }
        if (len + 2 > cap) {
            char *grown = realloc(out, cap *= 2);
            if (!grown) { jp->err = "out of memory"; break; }
            out = grown;
        }
        out[len++] = c;
    }
    if (!jp->err && *s != '"')
        jp->err = "unterminated string";
    if (jp->err) { free(out); return NULL; }
    out[len] = '\0';
    jp->p = s + 1;
    return out;
}

static jnode *jvalue(jparser *jp);

static jnode *jnew(jtype t)
{
    jnode *n = calloc(1, sizeof *n);
    if (n) n->type = t;
    return n;
}

static jnode *jcontainer(jparser *jp, jtype t)
{
    char close = t == J_OBJ ? '}' : ']';
    jnode *n = jnew(t), **tail;

    if (!n) { jp->err = "out of memory"; return NULL; }
    if (++jp->depth > 32) { jp->err = "nested too deeply"; jfree(n); return NULL; }
    tail = &n->child;
    jp->p++;
    jws(jp);
    if (*jp->p == close) { jp->p++; jp->depth--; return n; }
    for (;;) {
        char *key = NULL;
        jnode *v;
        jws(jp);
        if (t == J_OBJ) {
            if (*jp->p != '"') { jp->err = "expected a member name"; break; }
            key = jstring_raw(jp);
            if (!key) break;
            jws(jp);
            if (*jp->p != ':') { jp->err = "expected ':'"; free(key); break; }
            jp->p++;
        }
        v = jvalue(jp);
        if (!v) { free(key); break; }
        v->key = key;
        *tail = v;
        tail = &v->next;
        jws(jp);
        if (*jp->p == ',') { jp->p++; continue; }
        if (*jp->p == close) { jp->p++; jp->depth--; return n; }
        jp->err = t == J_OBJ ? "expected ',' or '}'" : "expected ',' or ']'";
        break;
    }
    jfree(n);
    return NULL;
}

static jnode *jvalue(jparser *jp)
{
    jnode *n;

    jws(jp);
    switch (*jp->p) {
    case '{': return jcontainer(jp, J_OBJ);
    case '[': return jcontainer(jp, J_ARR);
    case '"': {
        char *s = jstring_raw(jp);
        if (!s) return NULL;
        n = jnew(J_STR);
        if (!n) { free(s); jp->err = "out of memory"; return NULL; }
        n->str = s;
        return n;
    }
    case 't': case 'f': case 'n': {
        static const struct { const char *w; jtype t; double v; } words[] = {
            { "true", J_BOOL, 1 }, { "false", J_BOOL, 0 }, { "null", J_NULL, 0 },
        };
        size_t i;
        for (i = 0; i < 3; i++) {
            size_t len = strlen(words[i].w);
            if (!strncmp(jp->p, words[i].w, len)) {
                jp->p += len;
                n = jnew(words[i].t);
                if (!n) { jp->err = "out of memory"; return NULL; }
                n->num = words[i].v;
                return n;
            }
        }
        break;
    }
    default:
        if (*jp->p == '-' || isdigit((unsigned char)*jp->p)) {
            char *end;
            double v = strtod(jp->p, &end);
            if (end == jp->p) break;
            jp->p = end;
            n = jnew(J_NUM);
            if (!n) { jp->err = "out of memory"; return NULL; }
            n->num = v;
            return n;
        }
    }
    if (!jp->err) jp->err = "unexpected character";
    return NULL;
}

static jnode *jparse(const char *text, const char **err, int *line)
{
    jparser jp = { text, NULL, 0 };
    jnode *root = jvalue(&jp);

    if (root) {
        jws(&jp);
        if (*jp.p) { jp.err = "text after the end"; jfree(root); root = NULL; }
    }
    if (!root) {
        const char *c;
        *line = 1;
        for (c = text; c < jp.p && *c; c++)
            if (*c == '\n') (*line)++;
        *err = jp.err ? jp.err : "unreadable";
    }
    return root;
}

static jnode *jget(const jnode *obj, const char *key)
{
    jnode *n;
    if (!obj || obj->type != J_OBJ) return NULL;
    for (n = obj->child; n; n = n->next)
        if (n->key && !strcmp(n->key, key)) return n;
    return NULL;
}

/* ------------------------------------------------------------ patches */

typedef enum { T_U8, T_U16, T_U32, T_I8, T_I16, T_I32, T_F32, T_F64, T_BYTES } ptype;

static const struct { const char *name; ptype t; unsigned size; double lo, hi; } TYPES[] = {
    { "u8",  T_U8,  1, 0, 255 },
    { "u16", T_U16, 2, 0, 65535 },
    { "u32", T_U32, 4, 0, 4294967295.0 },
    { "i8",  T_I8,  1, -128, 127 },
    { "i16", T_I16, 2, -32768, 32767 },
    { "i32", T_I32, 4, -2147483648.0, 2147483647.0 },
    { "f32", T_F32, 4, 0, 0 },
    { "f64", T_F64, 8, 0, 0 },
    { "bytes", T_BYTES, 1, 0, 0 },
};

/* Parses "0x1234", "1234" or a number into *out. */
static int parse_address(const jnode *n, uint32_t *out)
{
    if (!n) return 0;
    if (n->type == J_NUM) {
        if (n->num < 0 || n->num > 4294967295.0 || n->num != floor(n->num)) return 0;
        *out = (uint32_t)n->num;
        return 1;
    }
    if (n->type == J_STR) {
        char *end;
        unsigned long v = strtoul(n->str, &end, 0);
        if (end == n->str || *end || v > 0xFFFFFFFFul) return 0;
        *out = (uint32_t)v;
        return 1;
    }
    return 0;
}

/* "90 90 C3" or "9090C3" into buf; returns the byte count or -1. */
static int parse_hex_bytes(const char *s, uint8_t *buf, int cap)
{
    int n = 0;
    while (*s) {
        int hi, lo;
        if (*s == ' ' || *s == ',' || *s == '\t') { s++; continue; }
        if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1])) return -1;
        hi = isdigit((unsigned char)s[0]) ? s[0] - '0' : tolower((unsigned char)s[0]) - 'a' + 10;
        lo = isdigit((unsigned char)s[1]) ? s[1] - '0' : tolower((unsigned char)s[1]) - 'a' + 10;
        if (n >= cap) return -1;
        buf[n++] = (uint8_t)(hi * 16 + lo);
        s += 2;
    }
    return n;
}

#define MAX_PATCH_BYTES 4096

/*
 * A value or expect node into little-endian bytes. Returns the byte count,
 * or -1 with *why set.
 */
static int encode(const jnode *n, int ti, uint8_t *buf, const char **why)
{
    const jnode *e;
    int count = 0, len = 0;
    unsigned size = TYPES[ti].size;

    if (!n) { *why = "missing"; return -1; }
    if (TYPES[ti].t == T_BYTES) {
        if (n->type != J_STR) { *why = "bytes must be a hex string"; return -1; }
        len = parse_hex_bytes(n->str, buf, MAX_PATCH_BYTES);
        if (len <= 0) { *why = "not a hex byte string"; return -1; }
        return len;
    }
    if (n->type == J_NUM) e = n;
    else if (n->type == J_ARR) e = n->child;
    else { *why = "must be a number or an array of numbers"; return -1; }

    for (; e; e = n->type == J_ARR ? e->next : NULL) {
        double v;
        uint8_t *dst = buf + len;
        if (e->type != J_NUM) { *why = "array holds a non-number"; return -1; }
        if (len + (int)size > MAX_PATCH_BYTES) { *why = "too long"; return -1; }
        v = e->num;
        switch (TYPES[ti].t) {
        case T_F32: {
            float f = (float)v;
            memcpy(dst, &f, 4);
            break;
        }
        case T_F64:
            memcpy(dst, &v, 8);
            break;
        default: {
            uint32_t u;
            unsigned i;
            if (v != floor(v) || v < TYPES[ti].lo || v > TYPES[ti].hi) {
                *why = "out of range for its type";
                return -1;
            }
            u = v < 0 ? (uint32_t)(int32_t)v : (uint32_t)v;
            for (i = 0; i < size; i++)
                dst[i] = (uint8_t)(u >> (8 * i));
        }
        }
        len += (int)size;
        count++;
        if (n->type == J_NUM) break;
    }
    if (!count) { *why = "empty array"; return -1; }
    return len;
}

static void hex_dump(const uint8_t *b, int n, char *out, size_t cap)
{
    size_t used = 0;
    int i;
    out[0] = '\0';
    for (i = 0; i < n && used + 4 < cap; i++)
        used += (size_t)snprintf(out + used, cap - used, "%s%02X", i ? " " : "", b[i]);
    if (i < n && used + 4 < cap)
        snprintf(out + used, cap - used, " ...");
}

static void apply_one(const jnode *p, int index, const char *name,
                      mod_patch_guest_fn guest, mod_patch_exec_fn exec, void *ctx,
                      mod_patch_result *r)
{
    static uint8_t value[MAX_PATCH_BYTES], expect[MAX_PATCH_BYTES];
    const jnode *type = jget(p, "type"), *en = jget(p, "enabled");
    const jnode *expect_node = jget(p, "expect");
    const char *why = NULL;
    uint32_t va;
    int ti = -1, vlen, elen = -1;
    size_t i;
    uint8_t *mem;
    char found[64], wanted[64];

    if (p->type != J_OBJ) {
        fprintf(stderr, "[PATCH] %s: entry %d is not an object\n", name, index);
        r->errors++;
        return;
    }
    if (en && en->type == J_BOOL && !en->num)
        return;   /* switched off, on purpose: not counted as skipped */

    if (!parse_address(jget(p, "address"), &va)) {
        fprintf(stderr, "[PATCH] %s: entry %d has no usable \"address\"\n", name, index);
        r->errors++;
        return;
    }
    if (type && type->type == J_STR)
        for (i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++)
            if (!strcmp(type->str, TYPES[i].name)) ti = (int)i;
    if (ti < 0) {
        fprintf(stderr, "[PATCH] %s: 0x%08X has no known \"type\" "
                "(u8 u16 u32 i8 i16 i32 f32 f64 bytes)\n", name, va);
        r->errors++;
        return;
    }
    vlen = encode(jget(p, "value"), ti, value, &why);
    if (vlen < 0) {
        fprintf(stderr, "[PATCH] %s: 0x%08X \"value\" %s\n", name, va, why);
        r->errors++;
        return;
    }
    if (expect_node) {
        elen = encode(expect_node, ti, expect, &why);
        if (elen < 0) {
            fprintf(stderr, "[PATCH] %s: 0x%08X \"expect\" %s\n", name, va, why);
            r->errors++;
            return;
        }
        if (elen != vlen) {
            fprintf(stderr, "[PATCH] %s: 0x%08X \"expect\" is %d bytes and \"value\" %d; "
                    "they must match\n", name, va, elen, vlen);
            r->errors++;
            return;
        }
    }
    mem = guest(va, (uint32_t)vlen, ctx);
    if (!mem) {
        fprintf(stderr, "[PATCH] %s: 0x%08X+%d is not in guest memory; skipped\n",
                name, va, vlen);
        r->skipped++;
        return;
    }
    if (elen >= 0 && memcmp(mem, expect, (size_t)elen) != 0) {
        hex_dump(mem, elen, found, sizeof found);
        hex_dump(expect, elen, wanted, sizeof wanted);
        fprintf(stderr, "[PATCH] %s: 0x%08X holds %s, not the expected %s; skipped "
                "(another release of the game?)\n", name, va, found, wanted);
        r->skipped++;
        return;
    }
    if (exec && exec(va, ctx))
        fprintf(stderr, "[PATCH] %s: 0x%08X is in an executable section; lifted code "
                "does not read its own instructions, so this changes data only "
                "(use an override for behaviour)\n", name, va);
    memcpy(mem, value, (size_t)vlen);
    hex_dump(value, vlen, wanted, sizeof wanted);
    fprintf(stderr, "[PATCH] %s: 0x%08X <- %s%s\n", name, va, wanted,
            elen < 0 ? " (no expect: unchecked)" : "");
    r->applied++;
}

void mod_patches_apply_text(const char *text, const char *name,
                            uint32_t title_id,
                            mod_patch_guest_fn guest, mod_patch_exec_fn exec,
                            void *ctx, mod_patch_result *r)
{
    const char *err = NULL;
    int line = 0, index = 0;
    jnode *root = jparse(text, &err, &line);
    const jnode *title, *list, *p;

    if (!root) {
        fprintf(stderr, "[PATCH] %s: line %d: %s; file ignored\n", name, line, err);
        r->errors++;
        return;
    }
    title = jget(root, "title");
    if (title && title->type == J_STR && title_id) {
        char *end;
        unsigned long want = strtoul(title->str, &end, 16);
        if (*end || end == title->str) {
            fprintf(stderr, "[PATCH] %s: \"title\" is not a hex title id; file ignored\n", name);
            r->errors++;
            jfree(root);
            return;
        }
        if ((uint32_t)want != title_id) {
            fprintf(stderr, "[PATCH] %s: for title %08lX, not this one (%08X); file ignored\n",
                    name, want, title_id);
            jfree(root);
            return;
        }
    }
    list = jget(root, "patches");
    if (!list || list->type != J_ARR) {
        fprintf(stderr, "[PATCH] %s: no \"patches\" array; file ignored\n", name);
        r->errors++;
        jfree(root);
        return;
    }
    for (p = list->child; p; p = p->next)
        apply_one(p, index++, name, guest, exec, ctx, r);
    jfree(root);
}

/* ------------------------------------------------------------ the runtime */

#if !defined(MOD_PATCHES_NO_RUNTIME)

#include "kernel.h"
#include "xbox_memory_layout.h"
#include "recomp_config.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#endif

int xbox_path_mods_dir(char *out, size_t n);   /* kernel_path.c */

#if defined(_WIN32)
#  define SEP "\\"
#else
#  define SEP "/"
#endif

static uint8_t *runtime_guest(uint32_t va, uint32_t len, void *ctx)
{
    (void)ctx;
    if (va < XBOX_BASE_ADDRESS || len > XBOX_TOTAL_RAM
        || va > (uint32_t)XBOX_TOTAL_RAM - len)
        return NULL;
    return (uint8_t *)((uintptr_t)va + xbox_GetMemoryOffset());
}

/* The XBE header is mapped at its base address, sections and all. */
static int runtime_exec(uint32_t va, void *ctx)
{
    const uint8_t *hdr = runtime_guest(XBOX_BASE_ADDRESS, 0x180, ctx);
    uint32_t count, table, i;

    if (!hdr || memcmp(hdr, "XBEH", 4) != 0)
        return 0;
    memcpy(&count, hdr + 0x11C, 4);
    memcpy(&table, hdr + 0x120, 4);
    if (count > 256)
        return 0;
    for (i = 0; i < count; i++) {
        const uint8_t *s = runtime_guest(table + i * 56, 56, ctx);
        uint32_t flags, sva, size;
        if (!s) return 0;
        memcpy(&flags, s + 0, 4);
        memcpy(&sva, s + 4, 4);
        memcpy(&size, s + 8, 4);
        if ((flags & 0x4) && va >= sva && va - sva < size)
            return 1;
    }
    return 0;
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || size > (1 << 22)
        || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)size + 1);
    if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        buf = NULL;
    }
    if (buf) buf[size] = '\0';
    fclose(f);
    return buf;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int ends_with_json(const char *s)
{
    size_t n = strlen(s);
    return n > 5 && !strcmp(s + n - 5, ".json");
}

void xbox_mod_patches_apply(void)
{
    static int done;
    char dir[600], path[900];
    char *names[256];
    int count = 0, i;
    mod_patch_result r = { 0, 0, 0 };

    if (done) return;
    done = 1;
    if (!xbox_path_mods_dir(dir, sizeof dir))
        return;
    snprintf(path, sizeof path, "%s" SEP "patches", dir);

#if defined(_WIN32)
    {
        WIN32_FIND_DATAA fd;
        char mask[920];
        HANDLE h;
        snprintf(mask, sizeof mask, "%s" SEP "*.json", path);
        h = FindFirstFileA(mask, &fd);
        if (h == INVALID_HANDLE_VALUE)
            return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                && ends_with_json(fd.cFileName) && count < 256)
                names[count++] = _strdup(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR *d = opendir(path);
        struct dirent *e;
        if (!d)
            return;
        while ((e = readdir(d)) != NULL)
            if (ends_with_json(e->d_name) && count < 256)
                names[count++] = strdup(e->d_name);
        closedir(d);
    }
#endif
    if (!count)
        return;
    qsort(names, (size_t)count, sizeof names[0], cmp_names);

    for (i = 0; i < count; i++) {
        char file[1200];
        char *text;
        if (!names[i]) continue;
        snprintf(file, sizeof file, "%s" SEP "%s", path, names[i]);
        text = read_text(file);
        if (!text) {
            fprintf(stderr, "[PATCH] %s: cannot read; ignored\n", names[i]);
            r.errors++;
        } else {
            mod_patches_apply_text(text, names[i], recomp_config_title_id(),
                                   runtime_guest, runtime_exec, NULL, &r);
            free(text);
        }
        free(names[i]);
    }
    fprintf(stderr, "[PATCH] %d applied, %d skipped, %d unreadable, from %d file(s) in %s\n",
            r.applied, r.skipped, r.errors, count, path);
    fflush(stderr);
}

#endif /* !MOD_PATCHES_NO_RUNTIME */
