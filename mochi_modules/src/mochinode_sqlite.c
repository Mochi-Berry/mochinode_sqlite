/*
 * mochinode_sqlite.c — SQLite bridge for the SEVM stack.
 *
 * Loads the system SQLite DLL at module load (LoadLibraryA + GetProcAddress)
 * and exposes a thin prepared-statement API to the engine. The engine's own
 * C API surface is resolved at runtime via mbjs_runtime.h — see that header
 * for why the DLL must NOT import mbjs.exe.
 *
 * Exposed to JS (all handles are doubles):
 *   version()                 -> string   sqlite3_libversion()
 *   open(path)                -> db | -1  sqlite3_open
 *   exec(db, sql)             -> rc       sqlite3_exec
 *   prepare(db, sql)          -> stmt|-1  sqlite3_prepare_v2
 *   step(stmt)                -> rc       sqlite3_step (100=ROW, 101=DONE)
 *   columnCount(stmt)         -> int
 *   columnName(stmt, i)       -> string
 *   columnText(stmt, i)       -> string
 *   columnInt(stmt, i)        -> number
 *   columnDouble(stmt, i)     -> number
 *   finalize(stmt)            -> rc
 *   close(db)                 -> rc
 */
#include "mbjs_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static MBJSRuntime rt;

/* ------------------------------------------------------------------ */
/* sqlite3 function pointer types (subset)                             */
/* ------------------------------------------------------------------ */

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

typedef const char *(*sq_version_t)(void);
typedef int (*sq_open_t)(const char *, sqlite3 **);
typedef int (*sq_exec_t)(sqlite3 *, const char *,
                         int (*)(void *, int, char **, char **), void *,
                         char **);
typedef int (*sq_prepare_t)(sqlite3 *, const char *, int, sqlite3_stmt **,
                            const char **);
typedef int (*sq_step_t)(sqlite3_stmt *);
typedef int (*sq_col_count_t)(sqlite3_stmt *);
typedef const char *(*sq_col_name_t)(sqlite3_stmt *, int);
typedef const unsigned char *(*sq_col_text_t)(sqlite3_stmt *, int);
typedef const void *(*sq_col_blob_t)(sqlite3_stmt *, int);
typedef int (*sq_col_bytes_t)(sqlite3_stmt *, int);
typedef int (*sq_col_int_t)(sqlite3_stmt *, int);
typedef double (*sq_col_double_t)(sqlite3_stmt *, int);
typedef int (*sq_col_type_t)(sqlite3_stmt *, int);
typedef int (*sq_finalize_t)(sqlite3_stmt *);
typedef int (*sq_close_t)(sqlite3 *);
typedef const char *(*sq_errmsg_t)(sqlite3 *);
typedef void (*sq_free_t)(void *);
typedef int (*sq_init_t)(void);

static sq_version_t    sq_version;
static sq_open_t       sq_open;
static sq_exec_t       sq_exec;
static sq_prepare_t    sq_prepare;
static sq_step_t       sq_step;
static sq_col_count_t  sq_col_count;
static sq_col_name_t   sq_col_name;
static sq_col_text_t   sq_col_text;
static sq_col_blob_t   sq_col_blob;
static sq_col_bytes_t  sq_col_bytes;
static sq_col_int_t    sq_col_int;
static sq_col_double_t sq_col_double;
static sq_col_type_t   sq_col_type;
static sq_finalize_t   sq_finalize;
static sq_close_t      sq_close;
static sq_errmsg_t     sq_errmsg;
static sq_free_t       sq_free;
static sq_init_t       sq_initialize;

static int sq_load(void)
{
#ifdef _WIN32
    static const char *const cands[] = {
        "sqlite3.dll",
        "C:/Python314/DLLs/sqlite3.dll",
        "C:/Python313/DLLs/sqlite3.dll",
        "C:/Python312/DLLs/sqlite3.dll",
        "C:/Windows/System32/sqlite3.dll",
        NULL,
    };
    HMODULE h = NULL;
    int i;
    for (i = 0; cands[i]; i++) {
        h = LoadLibraryA(cands[i]);
        if (h)
            break;
    }
    if (!h)
        return -1;
#define SQ_SYM(field, name) do {                                        \
        FARPROC p__ = GetProcAddress(h, (name));                        \
        if (!p__) return -1;                                            \
        memcpy(&(field), &p__, sizeof(p__));                            \
    } while (0)
#else
    void *h = dlopen("libsqlite3.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h)
        return -1;
#define SQ_SYM(field, name) do {                                        \
        void *p__ = dlsym(h, (name));                                   \
        if (!p__) return -1;                                            \
        memcpy(&(field), &p__, sizeof(p__));                            \
    } while (0)
#endif
    SQ_SYM(sq_version, "sqlite3_libversion");
    SQ_SYM(sq_open, "sqlite3_open");
    SQ_SYM(sq_exec, "sqlite3_exec");
    SQ_SYM(sq_prepare, "sqlite3_prepare_v2");
    SQ_SYM(sq_step, "sqlite3_step");
    SQ_SYM(sq_col_count, "sqlite3_column_count");
    SQ_SYM(sq_col_name, "sqlite3_column_name");
    SQ_SYM(sq_col_text, "sqlite3_column_text");
    SQ_SYM(sq_col_blob, "sqlite3_column_blob");
    SQ_SYM(sq_col_bytes, "sqlite3_column_bytes");
    SQ_SYM(sq_col_int, "sqlite3_column_int");
    SQ_SYM(sq_col_double, "sqlite3_column_double");
    SQ_SYM(sq_col_type, "sqlite3_column_type");
    SQ_SYM(sq_finalize, "sqlite3_finalize");
    SQ_SYM(sq_close, "sqlite3_close");
    SQ_SYM(sq_errmsg, "sqlite3_errmsg");
    SQ_SYM(sq_free, "sqlite3_free");
    SQ_SYM(sq_initialize, "sqlite3_initialize");
#undef SQ_SYM
    /* sqlite3_open crashes under lazy init on this build of sqlite3.dll;
     * initialize explicitly before any first use. */
    if (sq_initialize() != 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* handle helpers (addresses fit in a double: < 2^53)                  */
/* ------------------------------------------------------------------ */

static int js_to_ptr(MBJSContext *ctx, MBJSValueConst v, void **out)
{
    if (!rt.ready)
        return -1;
    if (rt.ToBigInt64) {
        int64_t i64 = 0;
        if (rt.ToBigInt64(ctx, &i64, v) == 0 && i64 != 0) {
            *out = (void *)(uintptr_t)i64;
            return 0;
        }
    }
    double d = 0;
    if (rt.ToFloat64(ctx, &d, v) < 0 || d <= 0)
        return -1;
    *out = (void *)(uintptr_t)d;
    return 0;
}

static MBJSValue ptr_to_js(MBJSContext *ctx, void *p)
{
    if (rt.NewBigInt64)
        return rt.NewBigInt64(ctx, (int64_t)(uintptr_t)p);
    return MBJS_NewFloat64(ctx, (double)(uintptr_t)p);
}

/* ------------------------------------------------------------------ */
/* module functions                                                    */
/* ------------------------------------------------------------------ */

static MBJSValue sq_version_fn(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return rt.NewStringLen(ctx, sq_version(), strlen(sq_version()));
}

static MBJSValue sq_open_fn(MBJSContext *ctx, MBJSValueConst this_val,
                            int argc, MBJSValueConst *argv)
{
    const char *path;
    sqlite3 *db = NULL;
    (void)this_val;
    if (argc < 1)
        return MBJS_NewInt32(ctx, -1);
    path = rt.ToCStringLen(ctx, NULL, argv[0]);
    if (!path)
        return MBJS_NewInt32(ctx, -1);
    if (sq_open(path, &db) != 0 || !db) {
        rt.FreeCString(ctx, path);
        return MBJS_NewInt32(ctx, -1);
    }
    rt.FreeCString(ctx, path);
    return ptr_to_js(ctx, db);
}

static MBJSValue sq_exec_fn(MBJSContext *ctx, MBJSValueConst this_val,
                            int argc, MBJSValueConst *argv)
{
    void *db = NULL;
    const char *sql;
    char *err = NULL;
    int rc;
    (void)this_val;
    if (argc < 2 || js_to_ptr(ctx, argv[0], &db) < 0)
        return MBJS_NewInt32(ctx, -1);
    sql = rt.ToCStringLen(ctx, NULL, argv[1]);
    if (!sql)
        return MBJS_NewInt32(ctx, -1);
    rc = sq_exec((sqlite3 *)db, sql, NULL, NULL, &err);
    if (err) {
        /* surface the error message as a JS exception */
        rt.ThrowTypeError(ctx, "sqlite: %s", err);
        sq_free(err);
    }
    rt.FreeCString(ctx, sql);
    return MBJS_NewInt32(ctx, rc);
}

static MBJSValue sq_prepare_fn(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    void *db = NULL;
    const char *sql;
    sqlite3_stmt *st = NULL;
    (void)this_val;
    if (argc < 2 || js_to_ptr(ctx, argv[0], &db) < 0)
        return MBJS_NewInt32(ctx, -1);
    sql = rt.ToCStringLen(ctx, NULL, argv[1]);
    if (!sql)
        return MBJS_NewInt32(ctx, -1);
    if (sq_prepare((sqlite3 *)db, sql, (int)strlen(sql) + 1, &st, NULL) != 0) {
        rt.FreeCString(ctx, sql);
        return MBJS_NewInt32(ctx, -1);
    }
    rt.FreeCString(ctx, sql);
    return st ? ptr_to_js(ctx, st) : MBJS_NewInt32(ctx, -1);
}

static MBJSValue sq_step_fn(MBJSContext *ctx, MBJSValueConst this_val,
                            int argc, MBJSValueConst *argv)
{
    void *st = NULL;
    (void)this_val; (void)ctx;
    if (argc < 1 || js_to_ptr(ctx, argv[0], &st) < 0)
        return MBJS_NewInt32(ctx, -1);
    return MBJS_NewInt32(ctx, sq_step((sqlite3_stmt *)st));
}

static MBJSValue sq_col_count_fn(MBJSContext *ctx, MBJSValueConst this_val,
                                 int argc, MBJSValueConst *argv)
{
    void *st = NULL;
    (void)this_val; (void)ctx;
    if (argc < 1 || js_to_ptr(ctx, argv[0], &st) < 0)
        return MBJS_NewInt32(ctx, 0);
    return MBJS_NewInt32(ctx, sq_col_count((sqlite3_stmt *)st));
}

static MBJSValue sq_col_name_fn(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    void *st = NULL;
    double idx = 0;
    const char *name;
    (void)this_val;
    if (argc < 2 || js_to_ptr(ctx, argv[0], &st) < 0 ||
        rt.ToFloat64(ctx, &idx, argv[1]) < 0)
        return MBJS_UNDEFINED;
    name = sq_col_name((sqlite3_stmt *)st, (int)idx);
    return name ? rt.NewStringLen(ctx, name, strlen(name)) : MBJS_UNDEFINED;
}

static MBJSValue sq_col_text_fn(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    void *st = NULL;
    double idx = 0;
    const unsigned char *text;
    (void)this_val;
    if (argc < 2 || js_to_ptr(ctx, argv[0], &st) < 0 ||
        rt.ToFloat64(ctx, &idx, argv[1]) < 0)
        return MBJS_UNDEFINED;
    text = sq_col_text((sqlite3_stmt *)st, (int)idx);
    return text ? rt.NewStringLen(ctx, (const char *)text,
                                  strlen((const char *)text))
                : MBJS_UNDEFINED;
}

static MBJSValue sq_col_blob_hex_fn(MBJSContext *ctx, MBJSValueConst this_val,
                                    int argc, MBJSValueConst *argv)
{
    static const char digits[] = "0123456789abcdef";
    void *st = NULL;
    double idx = 0;
    const unsigned char *blob;
    int bytes;
    int i;
    char *hex;
    MBJSValue out;
    (void)this_val;
    if (argc < 2 || js_to_ptr(ctx, argv[0], &st) < 0 ||
        rt.ToFloat64(ctx, &idx, argv[1]) < 0)
        return rt.NewStringLen(ctx, "", 0);
    blob = (const unsigned char *)sq_col_blob((sqlite3_stmt *)st, (int)idx);
    bytes = sq_col_bytes((sqlite3_stmt *)st, (int)idx);
    if (!blob || bytes <= 0)
        return rt.NewStringLen(ctx, "", 0);
    hex = (char *)malloc((size_t)bytes * 2 + 1);
    if (!hex)
        return MBJS_UNDEFINED;
    for (i = 0; i < bytes; i++) {
        hex[i * 2] = digits[(blob[i] >> 4) & 0xf];
        hex[i * 2 + 1] = digits[blob[i] & 0xf];
    }
    hex[bytes * 2] = '\0';
    out = rt.NewStringLen(ctx, hex, (size_t)bytes * 2);
    free(hex);
    return out;
}

static MBJSValue sq_col_int_fn(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    void *st = NULL;
    double idx = 0;
    (void)this_val;
    if (argc < 2 || js_to_ptr(ctx, argv[0], &st) < 0 ||
        rt.ToFloat64(ctx, &idx, argv[1]) < 0)
        return MBJS_NewInt32(ctx, 0);
    return MBJS_NewInt32(ctx, sq_col_int((sqlite3_stmt *)st, (int)idx));
}

static MBJSValue sq_col_double_fn(MBJSContext *ctx, MBJSValueConst this_val,
                                  int argc, MBJSValueConst *argv)
{
    void *st = NULL;
    double idx = 0;
    (void)this_val;
    if (argc < 2 || js_to_ptr(ctx, argv[0], &st) < 0 ||
        rt.ToFloat64(ctx, &idx, argv[1]) < 0)
        return MBJS_NewFloat64(ctx, 0.0);
    return MBJS_NewFloat64(ctx, sq_col_double((sqlite3_stmt *)st, (int)idx));
}

/* columnType(stmt, i) -> 1=INTEGER 2=FLOAT 3=TEXT 4=BLOB 5=NULL */
static MBJSValue sq_col_type_fn(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    void *st = NULL;
    double idx = 0;
    (void)this_val;
    if (argc < 2 || js_to_ptr(ctx, argv[0], &st) < 0 ||
        rt.ToFloat64(ctx, &idx, argv[1]) < 0)
        return MBJS_NewInt32(ctx, 5);
    return MBJS_NewInt32(ctx, sq_col_type((sqlite3_stmt *)st, (int)idx));
}

static MBJSValue sq_finalize_fn(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    void *st = NULL;
    (void)this_val; (void)ctx;
    if (argc < 1 || js_to_ptr(ctx, argv[0], &st) < 0)
        return MBJS_NewInt32(ctx, -1);
    return MBJS_NewInt32(ctx, sq_finalize((sqlite3_stmt *)st));
}

static MBJSValue sq_close_fn(MBJSContext *ctx, MBJSValueConst this_val,
                             int argc, MBJSValueConst *argv)
{
    void *db = NULL;
    (void)this_val; (void)ctx;
    if (argc < 1 || js_to_ptr(ctx, argv[0], &db) < 0)
        return MBJS_NewInt32(ctx, -1);
    return MBJS_NewInt32(ctx, sq_close((sqlite3 *)db));
}

static MBJSValue sq_errmsg_fn(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    void *db = NULL;
    const char *msg;
    (void)this_val;
    if (argc < 1 || js_to_ptr(ctx, argv[0], &db) < 0)
        return MBJS_UNDEFINED;
    msg = sq_errmsg((sqlite3 *)db);
    return msg ? rt.NewStringLen(ctx, msg, strlen(msg)) : MBJS_UNDEFINED;
}

/* ------------------------------------------------------------------ */
/* module plumbing                                                     */
/* ------------------------------------------------------------------ */

#define SQ_TAB_LEN 15

static const MBJSCFunctionListEntry sq_tab[SQ_TAB_LEN] = {
    MBJS_CFUNC_DEF("version", 0, sq_version_fn),
    MBJS_CFUNC_DEF("open", 1, sq_open_fn),
    MBJS_CFUNC_DEF("exec", 2, sq_exec_fn),
    MBJS_CFUNC_DEF("prepare", 2, sq_prepare_fn),
    MBJS_CFUNC_DEF("step", 1, sq_step_fn),
    MBJS_CFUNC_DEF("columnCount", 1, sq_col_count_fn),
    MBJS_CFUNC_DEF("columnName", 2, sq_col_name_fn),
    MBJS_CFUNC_DEF("columnText", 2, sq_col_text_fn),
    MBJS_CFUNC_DEF("columnBlobHex", 2, sq_col_blob_hex_fn),
    MBJS_CFUNC_DEF("columnInt", 2, sq_col_int_fn),
    MBJS_CFUNC_DEF("columnDouble", 2, sq_col_double_fn),
    MBJS_CFUNC_DEF("columnType", 2, sq_col_type_fn),
    MBJS_CFUNC_DEF("finalize", 1, sq_finalize_fn),
    MBJS_CFUNC_DEF("close", 1, sq_close_fn),
    MBJS_CFUNC_DEF("errmsg", 1, sq_errmsg_fn),
};

static int sq_mod_init(MBJSContext *ctx, MBJSModuleDef *m)
{
    return rt.SetModuleExportList(ctx, m, sq_tab, SQ_TAB_LEN);
}

MBJS_MODULE_EXTERN MBJSModuleDef *mbjs_init_module(MBJSContext *ctx,
                                                    const char *name)
{
    const char *missing = mbjs_runtime_resolve(&rt);
    MBJSModuleDef *m;

    if (missing)
        return NULL;
    if (sq_load() < 0)
        return NULL;                  /* sqlite3.dll not found */

    m = rt.NewCModule(ctx, name ? name : "mochinode_sqlite", sq_mod_init);
    if (!m)
        return NULL;
    if (rt.AddModuleExportList(ctx, m, sq_tab, SQ_TAB_LEN) != 0)
        return NULL;
    return m;
}
