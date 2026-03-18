# SPDX-License-Identifier: GPL-2.0-or-later

add_library(riscv STATIC)

target_sources(riscv PRIVATE
	batch.h
	debug_defines.h
	debug_reg_printer.h
	encoding.h
	field_helpers.h
	gdb_regs.h
	opcodes.h
	program.h
	riscv.h
	riscv-011.h
	riscv-011_reg.h
	riscv-013.h
	riscv-013_reg.h
	riscv_reg.h
	riscv_reg_impl.h
	batch.c
	program.c
	riscv-011.c
	riscv-011_reg.c
	riscv-013.c
	riscv-013_reg.c
	riscv.c
	riscv_reg.c
	riscv_semihosting.c
	debug_defines.c
	debug_reg_printer.c
)
