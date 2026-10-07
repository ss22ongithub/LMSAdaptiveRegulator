MODULE_NAME := areg
$(MODULE_NAME)-objs := ar.o ar_debugfs.o ar_perfs.o model.o master.o utils.o
obj-m += $(MODULE_NAME).o

# --- Build target selection ---------------------------------------------
# Native build (default): just run `make`. ARCH/CROSS_COMPILE are left
# empty so Kbuild auto-detects the host's own architecture and toolchain —
# this is what you want both for a normal x86_64 dev box build and for
# building directly ON a Raspberry Pi (aarch64) itself. KDIR defaults to
# the currently *running* kernel's build directory in both cases.
#
# Cross-compile build (e.g. x86_64 host targeting a Raspberry Pi): override
# ARCH, CROSS_COMPILE and KDIR on the command line, e.g.
#
#   make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
#        KDIR=/path/to/rpi-linux
#
# KDIR must point at a prepared kernel source/build tree for the target
# (after `make modules_prepare` there), not the host's /lib/modules tree.
ARCH ?=
CROSS_COMPILE ?=
KDIR ?= /lib/modules/$(shell uname -r)/build

# -msse/-mhard-float are x86-only flags (aarch64-linux-gnu-gcc aborts on
# -msse outright). Only apply them for a genuine native x86_64 build: no
# cross toolchain configured (CROSS_COMPILE empty), and the host actually
# is x86_64. This deliberately checks the real host (uname -m) rather than
# matching a specific spelling of $(ARCH) -- ARCH can validly be "x86_64",
# "x86", or left unset for the same native target, and the kernel's own
# Kbuild accepts all of those and still resolves to arch/x86/Makefile (and
# its own -mno-sse) regardless of spelling. Matching on one literal string
# here previously meant ARCH=x86 silently fell through this check while
# still building correctly against arch/x86 -- the result was a build that
# picked up the kernel's -mno-sse but never got our -msse to re-enable it,
# failing with "SSE register return with SSE disabled" instead of any
# obviously-wrong-target error.
HOST_ARCH := $(shell uname -m)
ifeq ($(CROSS_COMPILE),)
ifeq ($(HOST_ARCH),x86_64)
ccflags-y += -mhard-float -msse
endif
endif

# arm64's Kbuild compiles everything with -mgeneral-regs-only by default,
# which forbids FP/NEON codegen outright -- model.c/master.c/utils.c do
# plain `double` arithmetic (LMS weight updates, print_double()), so remove
# it for just those objects. No-op on x86/riscv, where the flag is never
# added in the first place.
CFLAGS_REMOVE_model.o  += -mgeneral-regs-only
CFLAGS_REMOVE_master.o += -mgeneral-regs-only
CFLAGS_REMOVE_utils.o  += -mgeneral-regs-only

# Enable debug traces (comment out to disable)
# ccflags-y += -DCONFIG_DEBUG_AR

# Only pass ARCH=/CROSS_COMPILE= through to the sub-make when they're
# actually set. A *literal* `ARCH=` (even empty) on a recursive make's
# command line overrides Kbuild's own `ARCH ?= ...` autodetection in its
# top-level Makefile, since command-line variables beat `?=` regardless of
# value -- so a native build would otherwise get ARCH resolved to the empty
# string and fail with "arch//Makefile: No such file or directory".
KBUILD_ARCH_FLAGS :=
ifneq ($(ARCH),)
KBUILD_ARCH_FLAGS += ARCH=$(ARCH)
endif
ifneq ($(CROSS_COMPILE),)
KBUILD_ARCH_FLAGS += CROSS_COMPILE=$(CROSS_COMPILE)
endif

all:
	$(MAKE) -C $(KDIR) $(KBUILD_ARCH_FLAGS) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) $(KBUILD_ARCH_FLAGS) M=$(PWD) clean

.PHONY: all clean
