MODULE_NAME := areg
# ML build: the streaming XGBoost predictor replaces model.o (the LMS
# estimator). models/model_lms.{c,h} stay in the tree for reference but are not linked.
#   models/model-xgb.o  integer front end + the only kernel_fpu_begin()/end() calls
#   models/model-xgb-fpu.o  ALL floating-point code (and the generated models/xgb/*.h trees)
# models/model-xgb-fpu.o is the only object that may be compiled with SSE -- see below.
$(MODULE_NAME)-objs := ar.o ar_debugfs.o ar_perfs.o models/model-xgb.o models/model-xgb-fpu.o master.o utils.o
ccflags-y += -I$(src)
obj-m += $(MODULE_NAME).o

# `make probe` builds ml_fpu_probe.ko instead: only the FPU path (see
# probe_main.c), to validate floating-point handling on this kernel before
# loading the regulator. Same flags, same models/model-xgb-fpu.o / models/model-xgb.o.
ifeq ($(PROBE),1)
obj-m := ml_fpu_probe.o
ml_fpu_probe-objs := probe_main.o models/model-xgb.o models/model-xgb-fpu.o
endif

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

# ---------------------------------------------------------------------------
# FLOATING POINT IN THE KERNEL -- READ BEFORE TOUCHING THESE FLAGS
#
# The kernel builds everything with -mno-sse -mno-sse2 -mno-mmx -mno-80387 on
# purpose: it does not save/restore user FPU/SSE state on kernel entry, so any
# SSE instruction run outside kernel_fpu_begin()/kernel_fpu_end() silently
# corrupts a user process's registers (and the wrong context can hang the
# machine). The ORIGINAL LMS-AR Makefile turned SSE on for EVERY object
# (ccflags-y += -mhard-float -msse); that lets the compiler use SSE anywhere
# -- struct copies, memset, loops -- outside any FPU section. Do not do that.
#
# Instead SSE is enabled for ONE object, models/model-xgb-fpu.o, which holds all the float
# code, exactly as the kernel's own amdgpu driver does for its FP files
# (drivers/gpu/drm/amd/display/dc/dml/Makefile). Every other object keeps the
# kernel's default flags, so it cannot contain an FP instruction.
# host/fpu_containment_check.py fails if this is ever violated.
#
# -msse2 after the kernel's -mno-sse2 re-enables it for this object; AVX stays
# off. GCC >= 7.1 is required to compile double code with the kernel's 8-byte
# stack alignment (same condition as amdgpu).
# ---------------------------------------------------------------------------
HOST_ARCH := $(shell uname -m)
ifeq ($(CROSS_COMPILE),)
ifeq ($(HOST_ARCH),x86_64)
CFLAGS_models/model-xgb-fpu.o += -mhard-float -msse -msse2
# Remove the kernel's two opposing flags explicitly so the result does not
# depend on flag order (-mno-mmx -mno-3dnow -mno-avx stay: SSE2 only).
CFLAGS_REMOVE_models/model-xgb-fpu.o += -mno-sse -mno-sse2
endif
endif

# arm64's Kbuild compiles everything with -mgeneral-regs-only, which forbids
# FP/NEON codegen outright: keep it for every object except models/model-xgb-fpu.o.
CFLAGS_REMOVE_models/model-xgb-fpu.o += -mgeneral-regs-only

ifdef CONFIG_CC_IS_GCC
ifdef CONFIG_GCC_VERSION
ifneq ($(shell test $(CONFIG_GCC_VERSION) -lt 70100 && echo old),)
$(error GCC older than 7.1 cannot safely compile the SSE/double code in models/model-xgb-fpu.o with the kernel's 8-byte stack alignment)
endif
endif
endif

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
	$(MAKE) -C $(KDIR) $(KBUILD_ARCH_FLAGS) M=$(CURDIR) modules

# FPU-path-only test module (see probe_main.c). Load THIS first.
probe:
	$(MAKE) -C $(KDIR) $(KBUILD_ARCH_FLAGS) M=$(CURDIR) PROBE=1 modules

# After `make` / `make probe`: prove on the real compiled objects that only
# models/model-xgb-fpu.o contains SSE/x87 instructions. Do not insmod if this fails.
check-fpu:
	sh ./check_built_objects.sh

clean:
	$(MAKE) -C $(KDIR) $(KBUILD_ARCH_FLAGS) M=$(CURDIR) clean

.PHONY: all probe check-fpu clean
