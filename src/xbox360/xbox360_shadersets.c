// Sidecar loader for the offline user-shader precompile (design doc step 4.3).
//
// The container is written big-endian (see scripts/xbox360/precompile_shaders.py)
// so the Xenon reader performs no byte swapping: every integer is read with the
// explicit big-endian helpers below and the microcode blobs are handed to
// IDirect3DDevice9_Create{Vertex,Pixel}Shader verbatim.
//
// Port-owned. No public source is touched.

#include "xbox360_shadersets.h"

#include "utils.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#include <xtl.h>
#include <d3d9.h>

static const uint8_t kSidecarMagic[8] = { 'B', 'S', 'X', '3', 'S', 'H', 'D', 0 };
#define X360_SIDECAR_VERSION 1u

struct X360ShaderSet {
    X360UserShader *shaders;
    uint32_t count;
};

// ===[ Big-endian reader ]===

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
    bool error;
} X360Reader;

static uint8_t x360ReadU8(X360Reader *r) {
    if (r->error || r->pos + 1 > r->size) { r->error = true; return 0; }
    return r->data[r->pos++];
}

static uint32_t x360ReadU32(X360Reader *r) {
    if (r->error || r->pos + 4 > r->size) { r->error = true; return 0; }
    uint32_t v = ((uint32_t) r->data[r->pos] << 24)
               | ((uint32_t) r->data[r->pos + 1] << 16)
               | ((uint32_t) r->data[r->pos + 2] << 8)
               | ((uint32_t) r->data[r->pos + 3]);
    r->pos += 4;
    return v;
}

static const uint8_t *x360ReadBytes(X360Reader *r, uint32_t len) {
    if (r->error || r->pos + len > r->size) { r->error = true; return NULL; }
    const uint8_t *p = r->data + r->pos;
    r->pos += len;
    return p;
}

// Reads a length-prefixed UTF-8 string into a fresh NUL-terminated buffer.
static char *x360ReadString(X360Reader *r) {
    uint32_t len = x360ReadU32(r);
    if (r->error || len > r->size) { r->error = true; return NULL; }
    const uint8_t *bytes = x360ReadBytes(r, len);
    if (bytes == NULL) return NULL;
    char *s = (char *) safeMalloc((size_t) len + 1);
    memcpy(s, bytes, len);
    s[len] = '\0';
    return s;
}

// ===[ Loading ]===

// Create a shader from a sidecar blob. The blob sits at an arbitrary offset in
// the container (a variable-length name precedes it), so it is not guaranteed to
// be DWORD-aligned; the Xenon Create*Shader entry points read a DWORD array, so
// copy it into an aligned scratch buffer first.
static HRESULT x360CreateVertexShaderAligned(D3DDevice *device, const uint8_t *bytes,
                                             uint32_t len, D3DVertexShader **out) {
    uint8_t *copy = (uint8_t *) safeMalloc(len);
    memcpy(copy, bytes, len);
    HRESULT hr = IDirect3DDevice9_CreateVertexShader(device, (const DWORD *) copy, out);
    free(copy);
    return hr;
}

static HRESULT x360CreatePixelShaderAligned(D3DDevice *device, const uint8_t *bytes,
                                            uint32_t len, D3DPixelShader **out) {
    uint8_t *copy = (uint8_t *) safeMalloc(len);
    memcpy(copy, bytes, len);
    HRESULT hr = IDirect3DDevice9_CreatePixelShader(device, (const DWORD *) copy, out);
    free(copy);
    return hr;
}

static bool x360LoadShader(X360Reader *r, D3DDevice *device, X360UserShader *out) {
    memset(out, 0, sizeof(*out));
    out->index = (int32_t) x360ReadU32(r);
    out->name = x360ReadString(r);
    out->flags = x360ReadU32(r);

    uint32_t vsLen = x360ReadU32(r);
    const uint8_t *vsBytes = vsLen > 0 ? x360ReadBytes(r, vsLen) : NULL;
    uint32_t psLen = x360ReadU32(r);
    const uint8_t *psBytes = psLen > 0 ? x360ReadBytes(r, psLen) : NULL;
    if (r->error) return false;

    if (vsBytes != NULL) {
        if (FAILED(x360CreateVertexShaderAligned(device, vsBytes, vsLen, &out->vs))) {
            logWarn("X360Shaders: CreateVertexShader failed for \"%s\" (index %d)\n",
                    out->name != NULL ? out->name : "?", out->index);
            return false;
        }
    }
    if (psBytes != NULL) {
        if (FAILED(x360CreatePixelShaderAligned(device, psBytes, psLen, &out->ps))) {
            logWarn("X360Shaders: CreatePixelShader failed for \"%s\" (index %d)\n",
                    out->name != NULL ? out->name : "?", out->index);
            return false;
        }
    }

    uint32_t uniformCount = x360ReadU32(r);
    if (r->error) return false;
    if (uniformCount > 0) {
        out->uniforms = (X360ShaderUniform *) safeCalloc(uniformCount, sizeof(X360ShaderUniform));
        for (uint32_t i = 0; i < uniformCount; i++) {
            X360ShaderUniform *u = &out->uniforms[i];
            u->name = x360ReadString(r);
            u->stage = x360ReadU8(r);
            u->regClass = x360ReadU8(r);
            u->regIndex = x360ReadU32(r);
            u->regCount = x360ReadU32(r);
            u->type = x360ReadU32(r);
            if (r->error) return false;
        }
        out->uniformCount = uniformCount;
    }
    return true;
}

static void x360FreeShader(X360UserShader *s) {
    if (s->vs != NULL) { IDirect3DVertexShader9_Release(s->vs); s->vs = NULL; }
    if (s->ps != NULL) { IDirect3DPixelShader9_Release(s->ps); s->ps = NULL; }
    for (uint32_t i = 0; i < s->uniformCount; i++) {
        free(s->uniforms[i].name);
    }
    free(s->uniforms);
    free(s->name);
    s->uniforms = NULL;
    s->name = NULL;
}

X360ShaderSet *X360ShaderSet_load(FileSystem *fs, const char *relativePath, D3DDevice *device) {
    if (fs == NULL || fs->vtable == NULL || fs->vtable->readFileBinary == NULL
        || relativePath == NULL || device == NULL) {
        return NULL;
    }

    uint8_t *data = NULL;
    int32_t size = 0;
    if (!fs->vtable->readFileBinary(fs, relativePath, &data, &size) || data == NULL || size <= 0) {
        logInfo("X360Shaders: no sidecar at \"%s\"; game shaders stay degraded\n", relativePath);
        return NULL;
    }

    X360Reader r = { data, (size_t) size, 0, false };
    const uint8_t *magic = x360ReadBytes(&r, sizeof(kSidecarMagic));
    uint32_t version = x360ReadU32(&r);
    uint32_t count = x360ReadU32(&r);
    if (r.error || magic == NULL || memcmp(magic, kSidecarMagic, sizeof(kSidecarMagic)) != 0
        || version != X360_SIDECAR_VERSION) {
        logWarn("X360Shaders: \"%s\" is not a usable sidecar (magic/version); ignoring\n", relativePath);
        free(data);
        return NULL;
    }

    X360ShaderSet *set = (X360ShaderSet *) safeCalloc(1, sizeof(X360ShaderSet));
    set->shaders = (X360UserShader *) safeCalloc(count > 0 ? count : 1, sizeof(X360UserShader));

    uint32_t loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        X360UserShader shader;
        if (!x360LoadShader(&r, device, &shader)) {
            x360FreeShader(&shader);
            logWarn("X360Shaders: shader %u in \"%s\" could not be loaded; skipping\n", i, relativePath);
            // The reader is in an undefined position after a failure; stop.
            break;
        }
        set->shaders[loaded++] = shader;
    }
    set->count = loaded;
    free(data);

    if (loaded == 0) {
        logInfo("X360Shaders: \"%s\" has no usable shaders; game shaders stay degraded\n", relativePath);
        X360ShaderSet_destroy(set);
        return NULL;
    }

    logInfo("X360Shaders: loaded %u precompiled user shader(s) from \"%s\"\n", loaded, relativePath);
    return set;
}

void X360ShaderSet_destroy(X360ShaderSet *set) {
    if (set == NULL) return;
    for (uint32_t i = 0; i < set->count; i++) {
        x360FreeShader(&set->shaders[i]);
    }
    free(set->shaders);
    free(set);
}

uint32_t X360ShaderSet_count(X360ShaderSet *set) {
    return set != NULL ? set->count : 0;
}

X360UserShader *X360ShaderSet_byIndex(X360ShaderSet *set, int32_t shaderIndex) {
    if (set == NULL) return NULL;
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->shaders[i].index == shaderIndex) return &set->shaders[i];
    }
    return NULL;
}

int32_t X360ShaderSet_indexAt(X360ShaderSet *set, uint32_t position) {
    if (set == NULL || position >= set->count) return -1;
    return set->shaders[position].index;
}

static bool x360NameMatches(const char *a, const char *b) {
    if (a == NULL || b == NULL) return false;
    if (strcmp(a, b) == 0) return true;
    // Accept the GameMaker-generated '_' prefix in either direction.
    if (a[0] == '_' && strcmp(a + 1, b) == 0) return true;
    if (b[0] == '_' && strcmp(a, b + 1) == 0) return true;
    return false;
}

int32_t X360UserShader_findUniform(X360UserShader *shader, const char *name) {
    if (shader == NULL || name == NULL) return -1;
    for (uint32_t i = 0; i < shader->uniformCount; i++) {
        if (x360NameMatches(shader->uniforms[i].name, name)) return (int32_t) i;
    }
    return -1;
}
