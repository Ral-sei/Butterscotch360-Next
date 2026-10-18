#!/usr/bin/env python3
"""Butterscotch Xbox 360 port: link wrapper around OXDK's oxdklink.py.

Port-owned and self-contained; it imports and reuses no other project's scripts.

Why this exists: clang in MS compatibility mode records every
`#pragma comment(lib, ...)` in a `.deplibs` section, and ld.lld would try to
honour it by looking for a Windows-spelled library. `--remove-section=.deplibs`
clears it. The strip must happen on a *copy* of each link input, not in place:
stripping the real object would leave it newer than its source, and the build
system would then recompile the whole project on every run.

Usage (the CMake link rule and port/xbox360/Makefile generate this):

    oxdklink_wrapper.py --objcopy <llvm-objcopy> --oxdklink <oxdklink.py>
        [--stage <dir>] <oxdklink args...> <objects...> [-- <archives...>]

Everything that is not one of this wrapper's own options is forwarded to
oxdklink unchanged, except that .o/.obj/.a inputs are replaced by staged copies.
"""

import hashlib
import os
import shutil
import subprocess
import sys

WRAPPER_OPTIONS = ("objcopy", "oxdklink", "stage")
LINK_INPUT_SUFFIXES = (".o", ".obj", ".a")


def parse_wrapper_options(argv):
    """Take this wrapper's own options off the front; forward the rest."""
    opts = {"objcopy": None, "oxdklink": None, "stage": None}
    index = 0
    while index < len(argv):
        arg = argv[index]
        if arg.startswith("--") and arg[2:] in WRAPPER_OPTIONS:
            if index + 1 >= len(argv):
                sys.stderr.write("oxdklink_wrapper: %s needs a value\n" % arg)
                sys.exit(2)
            opts[arg[2:]] = argv[index + 1]
            index += 2
            continue
        break
    missing = [name for name, value in opts.items() if name != "stage" and not value]
    if missing:
        sys.stderr.write("oxdklink_wrapper: missing option(s): "
                         + ", ".join("--" + m for m in missing) + "\n")
        sys.exit(2)
    return opts, argv[index:]


def is_link_input(arg):
    return arg.endswith(LINK_INPUT_SUFFIXES)


def stage_input(path, stage, objcopy):
    """Copy one link input into the staging directory with .deplibs removed.

    The copy keeps the source's timestamp so the work is skipped until the input
    actually changes; stripping the original instead would defeat incremental
    builds.
    """
    source = os.path.abspath(path)
    try:
        source_stat = os.stat(source)
    except OSError:
        return path

    digest = hashlib.sha1(source.encode("utf-8", "replace")).hexdigest()[:10]
    staged = os.path.join(stage, digest + "-" + os.path.basename(source))

    fresh = False
    if os.path.exists(staged):
        staged_stat = os.stat(staged)
        fresh = (staged_stat.st_size == source_stat.st_size
                 and int(staged_stat.st_mtime) == int(source_stat.st_mtime))

    if not fresh:
        shutil.copy2(source, staged)
        result = subprocess.run([objcopy, "--remove-section=.deplibs", staged],
                                capture_output=True, text=True)
        if result.returncode != 0:
            # Not every input carries the section, and a copy left truncated by
            # an interrupted run would also fail. Retry once from a clean copy;
            # if it still fails, keep going unstripped rather than fail the link.
            try:
                os.remove(staged)
            except OSError:
                pass
            shutil.copy2(source, staged)
            result = subprocess.run([objcopy, "--remove-section=.deplibs", staged],
                                    capture_output=True, text=True)
            if result.returncode != 0:
                sys.stderr.write("oxdklink_wrapper: keeping %s unstripped: %s\n"
                                 % (path, result.stderr.strip()))
        if os.path.exists(staged):
            os.utime(staged, (source_stat.st_atime, source_stat.st_mtime))

    return staged


def prune(stage, keep):
    """Drop staged copies whose original is no longer on the link line."""
    for entry in os.listdir(stage):
        if entry in keep:
            continue
        full = os.path.join(stage, entry)
        if os.path.isfile(full):
            try:
                os.remove(full)
            except OSError:
                pass


def default_stage_dir(forwarded):
    output = None
    for index, arg in enumerate(forwarded):
        if arg in ("-o", "--output") and index + 1 < len(forwarded):
            output = forwarded[index + 1]
            break
    base = os.path.dirname(os.path.abspath(output)) if output else os.getcwd()
    return os.path.join(base, ".oxdklink", "inputs")


def main():
    opts, forwarded = parse_wrapper_options(sys.argv[1:])

    stage = opts["stage"] or default_stage_dir(forwarded)
    os.makedirs(stage, exist_ok=True)

    rewritten = []
    keep = set()
    for arg in forwarded:
        if is_link_input(arg):
            staged = stage_input(arg, stage, opts["objcopy"])
            if staged != arg:
                keep.add(os.path.basename(staged))
            rewritten.append(staged)
        else:
            rewritten.append(arg)
    prune(stage, keep)

    command = [sys.executable or "python3", opts["oxdklink"]] + rewritten
    return subprocess.run(command).returncode


if __name__ == "__main__":
    sys.exit(main())
