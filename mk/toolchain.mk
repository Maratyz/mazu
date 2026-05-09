# mk/toolchain.mk - Cross-compiler detection and flags for Mazu

ifndef _MK_TOOLCHAIN_INCLUDED
_MK_TOOLCHAIN_INCLUDED := 1

CROSS_COMPILE ?= riscv-none-elf-
CC  := $(CROSS_COMPILE)gcc
AS  := $(CROSS_COMPILE)gcc
LD  := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy

# Architecture flags
ARCH_CFLAGS := -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -mno-relax

# Compiler flags
CFLAGS_BASE := -std=gnu99 -ffreestanding -fno-builtin -nostdinc \
               -Wall -Wextra -Wuninitialized -pedantic

# Preprocessor flags
CPPFLAGS := -MMD -MP -Iinclude/ -Iarch/$(ARCH)/include -I.

# Performance flags (controlled by RELEASE env var)
ifneq ($(RELEASE),)
PERF_FLAGS := -O2
else
PERF_FLAGS := -ggdb
endif

CFLAGS := $(PERF_FLAGS) $(ARCH_CFLAGS) $(CFLAGS_BASE)

# Stack-protector (CONFIG_STACK_PROTECTOR=y)
ifneq ($(filter y,$(CONFIG_STACK_PROTECTOR)),)
CFLAGS += -fstack-protector-strong
endif

# UBSan trap mode (CONFIG_UBSAN=y) — no runtime needed; ebreak on UB
ifneq ($(filter y,$(CONFIG_UBSAN)),)
CFLAGS += -fsanitize=undefined -fno-sanitize-recover=all -fsanitize-trap=all
endif

# Linker
LIBGCC = $(shell $(CC) $(ARCH_CFLAGS) --print-libgcc-file-name)
ARCH_LDFLAGS := -m elf64lriscv --no-relax -T arch/$(ARCH)/kernel.ld

endif # _MK_TOOLCHAIN_INCLUDED
