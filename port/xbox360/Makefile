# Butterscotch Xbox 360 port - OXDK-native minimal build (toolchain probe).
#
# This is the shortest path to a real XEX once the XDK and the patched compiler
# are present: it drives OXDK's own makefile directly, with no CMake in the way.
# Use it to sanity-check the toolchain before debugging the CMake path.
#
# Run inside an OXDK shell (so OXDK_DIR / XDK_DIR / OXDK_LLVM are set):
#
#     make -C port/xbox360
#
# or pass them explicitly:
#
#     make -C port/xbox360 OXDK_DIR=/path/to/OXDK XDK_DIR=/path/to/XDK
#
# Produces $(BS_BUILD)/default.xex. Port-owned; reads no public build file.

ifeq ($(OXDK_DIR),)
$(error set OXDK_DIR to your OXDK checkout, e.g. make OXDK_DIR=/path/to/OXDK)
endif

BS_PORT_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
BS_REPO     := $(abspath $(BS_PORT_DIR)/../..)
BS_BUILD    ?= $(BS_REPO)/build/xbox360-make

OXDK_TARGET := xbox360
XENON_ABI   := 1
XEX_TITLE   := butterscotch
XDK_LIBS    := xapilib,libcMT
OUTPUT_DIR  := $(BS_BUILD)

# Compile from a build-local copy: OXDK's pattern rule places the object file
# next to its source, which would litter src/xbox360/ if we pointed it there.
BS_MIN_SRC_REAL := $(BS_REPO)/src/xbox360/main_minimal.c
BS_MIN_SRC      := $(BS_BUILD)/src/main_minimal.c

SRCS := $(BS_MIN_SRC)

include $(OXDK_DIR)/oxdk.mk

# Defined after the include on purpose: oxdk.mk's first rule is "all", and the
# default goal is whatever rule comes first. Putting this copy rule above the
# include would make "make" build only the copy and stop.
$(BS_MIN_SRC): $(BS_MIN_SRC_REAL)
	@mkdir -p $(dir $@)
	cp $< $@
