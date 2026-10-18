// Butterscotch Xbox 360 port - single translation unit for stb_vorbis.
//
// Xenon has no Ogg Vorbis decoder in XAudio2, so the backend decodes Ogg files
// itself. xbox360_audio.c includes stb_vorbis.c with STB_VORBIS_HEADER_ONLY for
// the declarations; this file is the one place the implementation is compiled.
// Both TUs must agree on the configuration macros.

#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"
