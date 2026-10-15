#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# qsim: boot a production openfpgaOS app ELF under an emulated os25 Pocket.
#   make            -> ./qsim  (CPU core + os25 environment + GPU raster model)
#   make cpu-test   -> CPU core self-tests (Makefile.cpu; needs the guest ELFs)
#   make gputest    -> GPU model tooling (Makefile.gpu)
# Then: ./run.sh <elf> <outdir> [workload]   and   ./gate.sh <elfA> <elfB> <outdir>
CC      ?= gcc
OPT     ?= -O2
CFLAGS  := $(OPT) -g -march=native -std=gnu11 -Wall -Wextra -Wno-unused-label \
           -fno-strict-aliasing -ffp-contract=off -fno-fast-math -fno-math-errno
BUILD   := build/qsim
SRCS    := cpu.c fpu.c cache.c prof.c qelf.c lint.c gpu.c app.c env.c main.c
HDRS    := cpu.h cpu_ops.h cpu_exec.h fpu.h fpu_fast.h cache.h prof.h qelf.h lint.h gpu.h app.h env.h
OBJS    := $(SRCS:%.c=$(BUILD)/%.o)

all: qsim

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: %.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

qsim: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lm

cpu-test:
	$(MAKE) -f Makefile.cpu test

gputest:
	$(MAKE) -f Makefile.gpu gputest

clean:
	rm -rf $(BUILD) qsim

.PHONY: all clean cpu-test gputest
