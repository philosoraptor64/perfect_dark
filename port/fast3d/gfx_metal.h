#ifndef GFX_METAL_H
#define GFX_METAL_H

#include "gfx_rendering_api.h"

#ifdef __APPLE__
extern struct GfxRenderingAPI gfx_metal_api;
#endif

#ifdef __cplusplus
#include <string>
#include "gfx_cc.h"
// MSL source for one color combiner (gfx_metal_shader.cpp); *out_num_floats = floats per vertex.
std::string gfx_metal_generate_msl(const CCFeatures& cc, bool three_point, size_t* out_num_floats);
#endif

#endif
