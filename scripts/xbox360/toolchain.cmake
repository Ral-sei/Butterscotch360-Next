# Butterscotch Xbox 360 port - CMake toolchain.
#
# Port-owned. It is NOT the public project's toolchain and must never be used to
# configure the public CMakeLists.txt. It pins the compiler to OXDK's patched
# clang, targets powerpc-unknown-none-elf with the Xenon ABI, and replaces the
# link rule with the port's own oxdklink wrapper (staging + .deplibs cleanup) so
# the output is a XEX2 rather than a host ELF executable.
#
# Expected environment, prepared by scripts/xbox360/env.sh:
#     OXDK_DIR, OXDK_LLVM, XDK_DIR
#
# The flag set mirrors what OXDK's own xbox360/xbox360.mk computes for the C
# path (verified by asking that makefile, not copied from another project).

foreach(_bs_x360_var OXDK_DIR OXDK_LLVM XDK_DIR)
    if(NOT DEFINED ${_bs_x360_var})
        if(DEFINED ENV{${_bs_x360_var}})
            set(${_bs_x360_var} "$ENV{${_bs_x360_var}}")
        else()
            message(FATAL_ERROR
                "${_bs_x360_var} is not set. Source scripts/xbox360/env.sh before configuring.")
        endif()
    endif()
endforeach()

if(NOT EXISTS "${XDK_DIR}/lib/xbox/xboxkrnl.lib")
    message(FATAL_ERROR "XDK_DIR (${XDK_DIR}) is not an Xbox 360 XDK.")
endif()

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR powerpc)

set(BS_X360_TARGET_TRIPLE powerpc-unknown-none-elf)
set(BS_X360_TOOLCHAIN_DIR "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(BS_X360_REPO "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(BS_X360_OXDK360_DIR "${OXDK_DIR}/xbox360")

# --- compiler ---------------------------------------------------------------

set(BS_X360_CLANG "${OXDK_LLVM}/bin/clang")
set(BS_X360_CLANGXX "${OXDK_LLVM}/bin/clang++")
if(NOT EXISTS "${BS_X360_CLANG}")
    message(FATAL_ERROR "no clang at ${BS_X360_CLANG}; is OXDK_LLVM correct?")
endif()

set(CMAKE_C_COMPILER   "${BS_X360_CLANG}"   CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${BS_X360_CLANGXX}" CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER "${BS_X360_CLANG}"   CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_TARGET   "${BS_X360_TARGET_TRIPLE}")
set(CMAKE_CXX_COMPILER_TARGET "${BS_X360_TARGET_TRIPLE}")
set(CMAKE_ASM_COMPILER_TARGET "${BS_X360_TARGET_TRIPLE}")

# Xenon has no hosted CRT startup objects, so CMake's default compiler probe
# (which links an executable) would fail or fall back to the host toolchain.
# Probe by building a static library instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

find_program(BS_X360_AR      NAMES llvm-ar      HINTS "${OXDK_LLVM}/bin" REQUIRED)
find_program(BS_X360_RANLIB  NAMES llvm-ranlib  HINTS "${OXDK_LLVM}/bin" REQUIRED)
find_program(BS_X360_NM      NAMES llvm-nm      HINTS "${OXDK_LLVM}/bin" REQUIRED)
find_program(BS_X360_OBJCOPY NAMES llvm-objcopy HINTS "${OXDK_LLVM}/bin" REQUIRED)
find_program(BS_X360_OBJDUMP NAMES llvm-objdump HINTS "${OXDK_LLVM}/bin" REQUIRED)
find_program(BS_X360_PYTHON  NAMES python3 python REQUIRED)

set(CMAKE_AR      "${BS_X360_AR}"      CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB  "${BS_X360_RANLIB}"  CACHE FILEPATH "" FORCE)
set(CMAKE_NM      "${BS_X360_NM}"      CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY "${BS_X360_OBJCOPY}" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJDUMP "${BS_X360_OBJDUMP}" CACHE FILEPATH "" FORCE)

# --- shared flags -----------------------------------------------------------
#
# These live in CMAKE_<LANG>_FLAGS_INIT rather than add_compile_options() so
# CMake's own compiler/ABI checks inherit them too. Without them the XDK CRT
# headers fail those checks with "__declspec not allowed" / "Only Win32 target
# supported" while real compiles would otherwise work.

set(BS_X360_XDK_INCLUDE_DIRS
    "${XDK_DIR}/include/xbox"
    "${XDK_DIR}/TechPreview/Jul12Compiler/include/xbox")

set(BS_X360_SHIM_DIR "${BS_X360_OXDK360_DIR}/oxdk360/header-shim")
set(BS_X360_PORT_SHIM_DIR "${BS_X360_REPO}/port/xbox360/shim")

set(BS_X360_COMMON_FLAGS
    # CMake does not expand CMAKE_C_COMPILER_TARGET into the compile rule, so
    # the target triple has to be spelled out here or clang compiles for the
    # host and the XDK headers fail with __stdcall/builtin conflicts.
    -target "${BS_X360_TARGET_TRIPLE}"
    -m32
    -ffreestanding
    -fno-builtin
    -fms-extensions
    -fms-compatibility
    -fms-compatibility-version=14
    -fshort-wchar
    -fno-autolink
    -fdelayed-template-parsing
    -Wno-invalid-token-paste
    # The XDK's MSVC headers trip these three on every include; they are noise.
    -Wno-ignored-attributes
    -Wno-pragma-pack
    -Wno-ignored-pragma-intrinsic
    -D_WIN32
    -D_M_PPCBE
    -D_M_PPC
    -D_PPC_
    -D_XBOX
    -D_XENON
    -DXBOX
    -D_MSC_VER=1400
    -D_MSC_FULL_VER=140050727
    -D_SIZE_T_DEFINED
    -D_W64=
    -Xclang -target-feature -Xclang +xenon-abi
    # The port's own shims must win over both the XDK tree and the host's
    # /usr/include, which this clang keeps on the bare-target search path.
    -I "${BS_X360_PORT_SHIM_DIR}"
    -I "${BS_X360_SHIM_DIR}")

foreach(_bs_x360_inc IN LISTS BS_X360_XDK_INCLUDE_DIRS)
    list(APPEND BS_X360_COMMON_FLAGS -I "${_bs_x360_inc}")
endforeach()

string(REPLACE ";" " " BS_X360_COMMON_FLAGS_STR "${BS_X360_COMMON_FLAGS}")
set(CMAKE_C_FLAGS_INIT   "${BS_X360_COMMON_FLAGS_STR}")
set(CMAKE_CXX_FLAGS_INIT "${BS_X360_COMMON_FLAGS_STR}")
set(CMAKE_ASM_FLAGS_INIT "-target ${BS_X360_TARGET_TRIPLE}")

set(BS_X360_XDK_INCLUDE_DIRS "${BS_X360_XDK_INCLUDE_DIRS}"
    CACHE INTERNAL "Xbox 360 XDK include directories")

# --- link rule: produce a XEX through the port's wrapper ---------------------

set(BS_X360_XDK_LIBS   "xapilib,libcMT"     CACHE STRING "XDK libraries to link (comma separated)")
set(BS_X360_BASE       "0x82000000"         CACHE STRING "XEX image base")
set(BS_X360_STACK      "0x100000"           CACHE STRING "XEX stack size")
set(BS_X360_TITLE_ID   "0x0"                CACHE STRING
    "XEX title id. Must be non-zero for a title that saves: the console keys XContent by it.")
set(BS_X360_XEX_NAME   "butterscotch.exe"   CACHE STRING "PE name recorded inside the XEX")
set(BS_X360_ENTRY      "mainCRTStartup"     CACHE STRING "XEX entry symbol")

set(CMAKE_EXECUTABLE_SUFFIX ".xex")

set(BS_X360_WRAPPER  "${BS_X360_TOOLCHAIN_DIR}/oxdklink_wrapper.py")
set(BS_X360_OXDKLINK "${BS_X360_OXDK360_DIR}/tools/oxdklink/oxdklink.py")
set(BS_X360_LDSCRIPT "${BS_X360_OXDK360_DIR}/oxdk360/title.ld")

foreach(_bs_x360_need "${BS_X360_WRAPPER}" "${BS_X360_OXDKLINK}" "${BS_X360_LDSCRIPT}")
    if(NOT EXISTS "${_bs_x360_need}")
        message(FATAL_ERROR "missing Xbox 360 link input: ${_bs_x360_need}")
    endif()
endforeach()

set(BS_X360_LINK_RULE
    "${BS_X360_PYTHON}"
    "${BS_X360_WRAPPER}"
    "--objcopy"   "${BS_X360_OBJCOPY}"
    "--oxdklink"  "${BS_X360_OXDKLINK}"
    "--xdk"       "${XDK_DIR}"
    "--libs"      "${BS_X360_XDK_LIBS}"
    "--base"      "${BS_X360_BASE}"
    "--stack"     "${BS_X360_STACK}"
    "--name"      "${BS_X360_XEX_NAME}"
    "--title-id"  "${BS_X360_TITLE_ID}"
    "--entry"     "${BS_X360_ENTRY}"
    "-T"          "${BS_X360_LDSCRIPT}"
    "-o"          "<TARGET>"
    "<OBJECTS>"
    "--"
    "<LINK_LIBRARIES>")

string(REPLACE ";" " " BS_X360_LINK_RULE_STR "${BS_X360_LINK_RULE}")
set(CMAKE_C_LINK_EXECUTABLE   "${BS_X360_LINK_RULE_STR}")
set(CMAKE_CXX_LINK_EXECUTABLE "${BS_X360_LINK_RULE_STR}")
