// Single translation unit that compiles the vendored stb single-header
// implementations exactly once. Other files include the headers for their
// declarations only.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO // we decode from memory buffers, not files
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
