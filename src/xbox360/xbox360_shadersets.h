#ifndef BS_XBOX360_SHADERSETS_H
#define BS_XBOX360_SHADERSETS_H

#include <stdbool.h>
#include <stdint.h>

// The XDK's d3d9.h needs the Win32 base types, which xtl.h supplies; include it
// first so this header is self-contained regardless of the includer's order.
#include <xtl.h>
#include <d3d9.h>

#include "file_system.h"

// Runtime side of the offline user-shader precompile (design doc step 4.3).
//
// scripts/xbox360/precompile_shaders.py compiles a title's HLSL9 shaders with
// the Xbox 360 fxc and writes a sidecar ("data.x360shaders"). This module loads
// that sidecar, creates the D3D9 shader objects, and exposes the uniform table
// so the renderer can implement shaderIsCompiled / gpuSetShader /
// shaderGetUniform / shader_set_uniform_* without d3dx9.
//
// If the sidecar is absent or empty (e.g. a GMS 2.3+ title that stores only a
// declarations-only hlsl9 slot), loading returns NULL and the renderer keeps
// its explicit "not precompiled, using the built-in shader" fallback.
//
// Port-owned. No public source is touched.

#define X360_SHADER_FLAG_HAS_VS              (1u << 0)
#define X360_SHADER_FLAG_HAS_PS              (1u << 1)
#define X360_SHADER_FLAG_ALPHATEST_STRIPPED  (1u << 2)
#define X360_SHADER_FLAG_MATRICES_TRANSPOSED (1u << 3)

typedef enum {
    X360_UNIFORM_REG_CONSTANT = 0,
    X360_UNIFORM_REG_SAMPLER  = 1,
} X360UniformRegClass;

typedef enum {
    X360_UNIFORM_INVALID = 0,
    X360_UNIFORM_FLOAT   = 1,
    X360_UNIFORM_FLOAT2  = 2,
    X360_UNIFORM_FLOAT3  = 3,
    X360_UNIFORM_FLOAT4  = 4,
    X360_UNIFORM_FLOAT4X4 = 5,
    X360_UNIFORM_BOOL    = 6,
    X360_UNIFORM_INT     = 7,
    X360_UNIFORM_SAMPLER2D = 8,
} X360UniformType;

typedef enum {
    X360_SHADER_STAGE_VERTEX = 0,
    X360_SHADER_STAGE_PIXEL  = 1,
} X360ShaderStage;

typedef struct {
    char *name;
    uint8_t stage;      // X360ShaderStage
    uint8_t regClass;   // X360UniformRegClass
    uint32_t regIndex;
    uint32_t regCount;  // constant registers the declaration spans
    uint32_t type;      // X360UniformType
} X360ShaderUniform;

typedef struct {
    int32_t index;      // index into dataWin->shdr.shaders
    char *name;
    uint32_t flags;     // X360_SHADER_FLAG_*
    D3DVertexShader *vs;
    D3DPixelShader *ps;
    X360ShaderUniform *uniforms;
    uint32_t uniformCount;
} X360UserShader;

typedef struct X360ShaderSet X360ShaderSet;

// Load the sidecar at relativePath through the port file system and create the
// D3D9 shaders. Returns NULL (never aborts) when the file is missing, malformed,
// or contains zero usable shaders.
X360ShaderSet *X360ShaderSet_load(FileSystem *fs, const char *relativePath, D3DDevice *device);
void X360ShaderSet_destroy(X360ShaderSet *set);
uint32_t X360ShaderSet_count(X360ShaderSet *set);
X360UserShader *X360ShaderSet_byIndex(X360ShaderSet *set, int32_t shaderIndex);

// data.win shader index stored at sidecar position `position` (for the shader
// self-test, which does not know the index the game will use).
int32_t X360ShaderSet_indexAt(X360ShaderSet *set, uint32_t position);

// Uniform lookup accepts the name as written in the shader and the
// GameMaker-generated '_'-prefixed spelling (a GML shader_get_uniform("u_X")
// must find a uniform compiled as "_u_X"). Returns the uniform slot, or -1.
int32_t X360UserShader_findUniform(X360UserShader *shader, const char *name);

#endif /* BS_XBOX360_SHADERSETS_H */
