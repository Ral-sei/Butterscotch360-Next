# Butterscotch Xbox 360 port - environment setup and validation.
#
# Sourced by scripts/xbox360/build.sh, or by hand:
#
#     . scripts/xbox360/env.sh
#
# It resolves and validates:
#
#   OXDK_DIR     the OXDK checkout (contains oxdk.mk)
#   OXDK_LLVM    a clang carrying the Xenon ABI patches
#   XDK_DIR      an extracted Xbox 360 XDK (lib/xbox/xboxkrnl.lib)
#   OXDK_ROOT    = OXDK_DIR
#   OXDK360_DIR  = OXDK_DIR/xbox360
#
# and checks the host tools the port needs. Every path comes from the
# environment first and falls back to a documented default; nothing here points
# at another project's tree.
#
# This file is meant to be sourced: it exports variables into the caller's
# shell and reports problems by returning non-zero.

bs_x360_die() {
    printf 'xbox360/env.sh: %s\n' "$*" >&2
    return 1
}

# --- host tools -------------------------------------------------------------

for _bs_x360_tool in cmake ninja python3 make; do
    if ! command -v "$_bs_x360_tool" >/dev/null 2>&1; then
        bs_x360_die "missing required host tool: $_bs_x360_tool"
        return 1
    fi
done
unset _bs_x360_tool

# --- OXDK -------------------------------------------------------------------

if [ -z "${OXDK_DIR:-}" ]; then
    for _bs_x360_cand in "$HOME/OXDK" "$HOME/oxdk" /opt/OXDK; do
        if [ -f "$_bs_x360_cand/oxdk.mk" ]; then
            OXDK_DIR=$_bs_x360_cand
            break
        fi
    done
fi
if [ -z "${OXDK_DIR:-}" ]; then
    bs_x360_die "set OXDK_DIR to your OXDK checkout (the directory holding oxdk.mk)"
    return 1
fi
if [ ! -f "$OXDK_DIR/oxdk.mk" ]; then
    bs_x360_die "OXDK_DIR=$OXDK_DIR does not contain oxdk.mk"
    return 1
fi
OXDK_ROOT=$OXDK_DIR
OXDK360_DIR=$OXDK_DIR/xbox360
export OXDK_ROOT OXDK360_DIR

# --- patched LLVM -----------------------------------------------------------
#
# OXDK needs a clang carrying the Xenon ABI patches. A stock clang compiles but
# produces SysV frames, which corrupt every call into XDK or kernel code.

if [ -z "${OXDK_LLVM:-}" ]; then
    for _bs_x360_cand in "$HOME/oxdk-llvm/build" "$HOME/llvm-xenon/build" /root/oxdk-llvm/build /opt/oxdk-llvm; do
        if [ -x "$_bs_x360_cand/bin/clang" ]; then
            OXDK_LLVM=$_bs_x360_cand
            break
        fi
    done
fi
if [ -z "${OXDK_LLVM:-}" ]; then
    bs_x360_die "no patched LLVM found; run OXDK's build-llvm, or set OXDK_LLVM"
    return 1
fi
if [ ! -x "$OXDK_LLVM/bin/clang" ]; then
    bs_x360_die "OXDK_LLVM=$OXDK_LLVM has no bin/clang"
    return 1
fi
if ! printf 'int f(void){return 0;}\n' | "$OXDK_LLVM/bin/clang" \
        -target powerpc-unknown-none-elf \
        -Xclang -target-feature -Xclang +xenon-abi \
        -x c -c - -o /dev/null 2>/dev/null; then
    bs_x360_die "clang at $OXDK_LLVM does not accept the xenon-abi feature"
    return 1
fi
export OXDK_LLVM

# --- Xbox 360 XDK -----------------------------------------------------------

if [ -z "${XDK_DIR:-}" ]; then
    for _bs_x360_cand in \
        "$HOME/xdk360/XDK" "$HOME/xdk360" \
        "$HOME/xdk360-extract/sdk/XDK" \
        /root/xdk360 /opt/xdk360/XDK; do
        if [ -f "$_bs_x360_cand/lib/xbox/xboxkrnl.lib" ]; then
            XDK_DIR=$_bs_x360_cand
            break
        fi
    done
fi
if [ -z "${XDK_DIR:-}" ]; then
    bs_x360_die "set XDK_DIR to an extracted Xbox 360 XDK (the directory holding lib/xbox/xboxkrnl.lib)"
    return 1
fi
if [ ! -f "$XDK_DIR/lib/xbox/xboxkrnl.lib" ]; then
    bs_x360_die "XDK_DIR=$XDK_DIR is not an Xbox 360 XDK (lib/xbox/xboxkrnl.lib is missing)"
    return 1
fi
export XDK_DIR

# --- tools that live beside the patched compiler ----------------------------

case ":$PATH:" in
    *":$OXDK_LLVM/bin:"*) ;;
    *) PATH="$OXDK_LLVM/bin:$PATH" ;;
esac
export PATH

if ! command -v llvm-objcopy >/dev/null 2>&1; then
    bs_x360_die "llvm-objcopy is not on PATH (expected under $OXDK_LLVM/bin)"
    return 1
fi
if [ ! -f "$OXDK360_DIR/tools/oxdklink/oxdklink.py" ]; then
    bs_x360_die "OXDK's linker is missing: $OXDK360_DIR/tools/oxdklink/oxdklink.py"
    return 1
fi
if [ ! -f "$OXDK360_DIR/oxdk360/title.ld" ]; then
    bs_x360_die "OXDK's linker script is missing: $OXDK360_DIR/oxdk360/title.ld"
    return 1
fi

unset _bs_x360_cand
