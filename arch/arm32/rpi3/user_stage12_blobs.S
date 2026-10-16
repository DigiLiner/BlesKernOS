.section .rodata.stage12_apps,"a",%progbits
.align 4
.macro APP_BLOB symbol, file
.global rpi3_stage12_\symbol\()_start
.global rpi3_stage12_\symbol\()_end
rpi3_stage12_\symbol\()_start:
.incbin "build/rpi3/arm-programs/\file"
rpi3_stage12_\symbol\()_end:
.align 4
.endm
APP_BLOB about, about.BEX
APP_BLOB help, help.BEX
APP_BLOB dir, dir.BEX
APP_BLOB ls, ls.BEX
APP_BLOB cat, cat.BEX
APP_BLOB type, type.BEX
APP_BLOB echo, echo.BEX
APP_BLOB calc, calc.BEX
APP_BLOB uptime, uptime.BEX
APP_BLOB uname, uname.BEX
APP_BLOB tree, tree.BEX
APP_BLOB hexdump, hexdump.BEX
APP_BLOB checksum, checksum.BEX
APP_BLOB find, find.BEX
APP_BLOB sleep, sleep.BEX
APP_BLOB attrib, attrib.BEX
APP_BLOB chmod, chmod.BEX
APP_BLOB copy, copy.BEX
APP_BLOB cp, cp.BEX
APP_BLOB delete, delete.BEX
APP_BLOB rm, rm.BEX
APP_BLOB mkdir, mkdir.BEX
APP_BLOB rmdir, rmdir.BEX
APP_BLOB move, move.BEX
APP_BLOB rename, rename.BEX
APP_BLOB touch, touch.BEX
APP_BLOB more, more.BEX
APP_BLOB strings, strings.BEX
APP_BLOB diff, diff.BEX
APP_BLOB mem, mem.BEX
APP_BLOB benchmark, benchmark.BEX
APP_BLOB compress, compress.BEX
APP_BLOB extract, extract.BEX
APP_BLOB backup, backup.BEX
