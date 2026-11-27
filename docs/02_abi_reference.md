# 02 — ABI Reference (mochinode_sqlite)

- Native entry point: `mbjs_init_module()`
- ABI version: `MBC1`
- Header ABI:
- `include/mochinode_sqlite.h`
- `include/mochinode_sqlite_abi.h`

## Runtime exports (15 — from manifest.json)

- `close`
- `columnBlobHex`
- `columnCount`
- `columnDouble`
- `columnInt`
- `columnName`
- `columnText`
- `columnType`
- `errmsg`
- `exec`
- `finalize`
- `open`
- `prepare`
- `step`
- `version`

Symbols above are exposed by `mochi_modules/src/mochinode_sqlite.c` through
`MBJS_AddModuleExportList()` inside `mbjs_init_module()`; the values are
recorded by `MBJS_SetModuleExportList()` in the module init func.

## Build variables (no absolute host paths)

- `ENGINE_INC`: directory holding `mbjs.h` + `mbjs_runtime.h` (default
  `../libmochiberry`; override at build time).
- Windows link libs: (none additional).
