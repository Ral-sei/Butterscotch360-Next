# Xbox 360 Phase 0 Build

## Baseline

- Branch: `port/xbox360`
- Core baseline: `f8de9da8982fca6550a2051da9e44c8c1644b375`
- Toolchain: Visual Studio 2010 with Xbox 360 XDK 21256
- Configuration: `Release|Xbox 360`

## Build

```powershell
& 'F:\Microsoft Visual Studio 10.0\Common7\IDE\devenv.com' `
    Butterscotch360.sln /Build 'Release|Xbox 360'
```

The Phase 0 target compiles the current Next core, image decoder, hashing and
compression dependencies, a single STB implementation unit, the noop audio
backend, and the Xbox platform skeleton. D3D9 and networking are disabled for
this compile-only milestone and will be enabled when their platform backends
are introduced.

## Required Definitions

- `_XBOX`, `XBOX`, `PLATFORM_XBOX360`
- `IS_BIG_ENDIAN`, `STB_VORBIS_BIG_ENDIAN`
- `ENABLE_WAD14`, `ENABLE_WAD16`, `ENABLE_WAD17`
- `BZ_NO_STDIO`

`NO_RVALUE_INT64` is intentionally not defined.

## Compile Error Classification

The initial build exposed four VS2010/XDK compatibility categories:

1. Missing desktop MSVC and Win32 APIs: handled by Xbox-only header shims and
   the parser's existing buffered-read fallback.
2. Missing C99 CRT and math behavior: handled by the reduced compatibility
   header without replacing Next allocation or iteration helpers.
3. C99 syntax and UDT return ABI differences: fixed with portable aggregate
   initialization, explicit casts, MSVC alignment, and a complete `RValue`
   definition before the `BuiltinFunc` typedef.
4. Vendor linkage mode: base64, MD5, and SHA1 compile as C++ with the core;
   bzip2 remains C and uses its existing C-linkage declarations.

Final result: the XDK Release build completed with one project succeeded and
zero projects failed, producing `Release/Butterscotch360-Next.xex`.
