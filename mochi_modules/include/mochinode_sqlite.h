#ifndef MB_MOCHINODE_SQLITE_H
#define MB_MOCHINODE_SQLITE_H
/*
 * mochinode_sqlite — MochiBerry native-first module.
 *
 * ABI contract (see mochiberry/module-loader-contract.md):
 *   - exports the single entry point mbjs_init_module(MBJSContext*, const char*)
 *   - registers C functions via NewCModule / AddModuleExportList / SetModuleExportList
 *   - C functions that throw MUST return MBJS_EXCEPTION (never MBJS_UNDEFINED)
 *
 * Runtime symbols (MBJS_*) are resolved through mbjs_runtime_resolve() from
 * <root>/native/mbjs_runtime.h at load time; do NOT link against mbjs.exe.
 *
 * Exported symbols (measured): open, exec, query, close
 */
#include <stdint.h>

typedef struct MBJSContext MBJSContext;
typedef struct MBJSModuleDef MBJSModuleDef;

MBJSModuleDef *mbjs_init_module(MBJSContext *ctx, const char *name);

#endif /* MB_MOCHINODE_SQLITE_H */
