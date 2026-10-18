// Butterscotch Xbox 360 port - single translation unit for the stb libraries.
//
// The public build compiles no stb_impl for console platforms from src/*.c, so
// each platform directory carries its own. This mirrors the Switch/Vita
// arrangement.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
