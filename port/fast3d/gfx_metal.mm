// Metal rendering backend for Fast3D (Apple platforms).
//
// Status: Metal plan step M2 (see docs/netplay-notes.md) = skeleton. It owns the device,
// queue and the SDL window's CAMetalLayer, clears the screen every frame and presents.
// Shaders, textures, draws and framebuffers are bookkeeping-only stubs that return
// valid handles so gfx_pc.cpp runs unchanged; they are filled in by steps M3-M6.

#ifdef __APPLE__

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <map>
#include <utility>
#include <vector>

extern "C" {
#include "platform.h"
#include "system.h"
}

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_metal.h"

extern "C" void* gfx_sdl_get_metal_layer(void);

struct ShaderProgram {
    uint64_t shader_id0;
    uint32_t shader_id1;
    uint8_t num_inputs;
    bool used_textures[2];
    size_t num_floats;
    bool three_point;
    id<MTLFunction> vertex_fn;   // nil if the MSL failed to compile
    id<MTLFunction> fragment_fn;
};

namespace {

struct FramebufferMtl {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t msaa_level = 1;
    bool render_target = false;
    bool has_depth = false;
};

id<MTLDevice> sDevice;
id<MTLCommandQueue> sQueue;
CAMetalLayer* sLayer;

std::map<std::pair<uint64_t, uint32_t>, ShaderProgram> sShaders;
ShaderProgram* sCurrentShader;
uint32_t sNextTextureId = 1;
std::vector<FramebufferMtl> sFramebuffers; // index 0 = the screen
FilteringMode sTextureFilter = FILTER_THREE_POINT;
MipmapFilteringMode sMipmapFilter = MIPMAP_DISABLED;
int sShadersCompiled;
int sShadersFailed;
int sAnisotropy = 1;

const char* gfx_metal_get_name(void) {
    return "Metal";
}

int gfx_metal_get_max_texture_size(void) {
    return 16384;
}

GfxClipParameters gfx_metal_get_clip_parameters(void) {
    // Metal clip space: z in [0, 1]; framebuffer textures are top-left origin.
    return { true, true };
}

void gfx_metal_unload_shader(ShaderProgram* old_prg) {
    if (sCurrentShader == old_prg) {
        sCurrentShader = nullptr;
    }
}

void gfx_metal_load_shader(ShaderProgram* new_prg) {
    sCurrentShader = new_prg;
}

ShaderProgram* gfx_metal_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1) {
    CCFeatures cc;
    gfx_cc_get_features(shader_id0, shader_id1, &cc);
    ShaderProgram& prg = sShaders[{ shader_id0, shader_id1 }];
    prg.shader_id0 = shader_id0;
    prg.shader_id1 = shader_id1;
    prg.num_inputs = cc.num_inputs;
    prg.used_textures[0] = cc.used_textures[0];
    prg.used_textures[1] = cc.used_textures[1];
    prg.three_point = (sTextureFilter == FILTER_THREE_POINT);
    const std::string src = gfx_metal_generate_msl(cc, prg.three_point, &prg.num_floats);
    @autoreleasepool {
        NSError* err = nil;
        MTLCompileOptions* opts = [MTLCompileOptions new];
        if (@available(macOS 15.0, iOS 18.0, *)) {
            opts.mathMode = MTLMathModeSafe; // like GLSL without fast-math: keep the combiner math exact
        } else {
            opts.fastMathEnabled = NO;
        }
        id<MTLLibrary> lib = [sDevice newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()]
                                                   options:opts
                                                     error:&err];
        prg.vertex_fn = lib ? [lib newFunctionWithName:@"vertexMain"] : nil;
        prg.fragment_fn = lib ? [lib newFunctionWithName:@"fragmentMain"] : nil;
        if (!prg.vertex_fn || !prg.fragment_fn) {
            if (sShadersFailed++ < 3) {
                sysLogPrintf(LOG_ERROR, "Metal: shader %016llx/%08x failed to compile:\n%s\n--- source ---\n%s",
                             (unsigned long long)shader_id0, shader_id1,
                             err ? err.localizedDescription.UTF8String : "(no error)", src.c_str());
            }
        } else if (++sShadersCompiled % 25 == 1) {
            sysLogPrintf(LOG_NOTE, "Metal: %d shaders compiled, %d failed", sShadersCompiled, sShadersFailed);
        }
    }
    sCurrentShader = &prg;
    return &prg;
}

ShaderProgram* gfx_metal_lookup_shader(uint64_t shader_id0, uint32_t shader_id1) {
    auto it = sShaders.find({ shader_id0, shader_id1 });
    return it == sShaders.end() ? nullptr : &it->second;
}

void gfx_metal_shader_get_info(ShaderProgram* prg, uint8_t* num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

void gfx_metal_clear_shaders(void) {
    sCurrentShader = nullptr;
    sShaders.clear();
}

uint32_t gfx_metal_new_texture(void) {
    return sNextTextureId++;
}

void gfx_metal_select_texture(int tile, uint32_t texture_id, bool linear_filter) {
}

void gfx_metal_upload_texture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height, bool gen_mipmaps) {
}

void gfx_metal_set_sampler_parameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt, bool mipmaps) {
}

void gfx_metal_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare, bool depth_source_prim,
                              uint16_t zmode) {
}

void gfx_metal_set_depth_range(float znear, float zfar) {
}

void gfx_metal_set_viewport(int x, int y, int width, int height) {
}

void gfx_metal_set_scissor(int x, int y, int width, int height) {
}

void gfx_metal_set_use_alpha(bool use_alpha, bool modulate) {
}

void gfx_metal_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
}

void gfx_metal_init(void) {
    sLayer = (__bridge CAMetalLayer*)gfx_sdl_get_metal_layer();
    if (!sLayer) {
        sysFatalError("Metal: the window has no CAMetalLayer");
    }
    sDevice = sLayer.device ? sLayer.device : MTLCreateSystemDefaultDevice();
    if (!sDevice) {
        sysFatalError("Metal: no Metal device");
    }
    sLayer.device = sDevice;
    sLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    sLayer.framebufferOnly = YES;
    sQueue = [sDevice newCommandQueue];
    sFramebuffers.assign(1, FramebufferMtl{});
    sysLogPrintf(LOG_NOTE, "Metal: device %s", sDevice.name.UTF8String);

    // Test aid: PD_METAL_SHADERTEST=N compiles N random combiner ids (both filter modes) at start-up.
    if (const char* n = getenv("PD_METAL_SHADERTEST")) {
        uint64_t x = 0x9E3779B97F4A7C15ull;
        auto next = [&x]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
        const int count = atoi(n);
        for (int i = 0; i < count; i++) {
            const uint64_t id0 = next();
            const uint32_t id1 = (uint32_t)next();
            CCFeatures cc;
            gfx_cc_get_features(id0, id1, &cc);
            bool valid = true; // random ids can encode combiner inputs that don't exist: skip those
            for (int a = 0; a < 2; a++)
                for (int b = 0; b < 2; b++)
                    for (int k = 0; k < 4; k++)
                        valid &= cc.c[a][b][k] <= SHADER_NOISE &&
                                 !(cc.c[a][b][k] >= SHADER_INPUT_5 && cc.c[a][b][k] <= SHADER_INPUT_7) &&
                                 !(cc.c[a][b][k] >= SHADER_INPUT_1 && cc.c[a][b][k] <= SHADER_INPUT_7 &&
                                   cc.c[a][b][k] - SHADER_INPUT_1 >= cc.num_inputs);
            if (!valid) {
                i--;
                continue;
            }
            sTextureFilter = (i & 1) ? FILTER_THREE_POINT : FILTER_LINEAR;
            gfx_metal_create_and_load_new_shader(id0, id1);
        }
        sysLogPrintf(LOG_NOTE, "Metal: shader test done: %d compiled, %d failed", sShadersCompiled, sShadersFailed);
        sShaders.clear();
        sCurrentShader = nullptr;
        sTextureFilter = FILTER_THREE_POINT;
    }
}

void gfx_metal_on_resize(void) {
}

void gfx_metal_start_frame(void) {
}

void gfx_metal_end_frame(void) {
    @autoreleasepool {
        id<CAMetalDrawable> drawable = [sLayer nextDrawable];
        if (!drawable) {
            return;
        }
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        // Skeleton marker colour (dark red): proves the Metal path presents frames.
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.35, 0.0, 0.0, 1.0);
        id<MTLCommandBuffer> cmd = [sQueue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
        [enc endEncoding];
        [cmd presentDrawable:drawable];
        [cmd commit];
    }
}

void gfx_metal_finish_render(void) {
}

int gfx_metal_create_framebuffer(void) {
    sFramebuffers.push_back(FramebufferMtl{});
    return (int)sFramebuffers.size() - 1;
}

void gfx_metal_update_framebuffer_parameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                             bool opengl_invert_y, bool render_target, bool has_depth_buffer,
                                             bool can_extract_depth) {
    if (fb_id < 0 || fb_id >= (int)sFramebuffers.size()) {
        return;
    }
    FramebufferMtl& fb = sFramebuffers[fb_id];
    fb.width = width;
    fb.height = height;
    fb.msaa_level = msaa_level;
    fb.render_target = render_target;
    fb.has_depth = has_depth_buffer;
}

bool gfx_metal_start_draw_to_framebuffer(int fb_id, float noise_scale) {
    return true;
}

void gfx_metal_copy_framebuffer(int fb_dst, int fb_src, int left, int top, bool flip_y, bool use_back) {
}

void gfx_metal_clear_framebuffer(bool clear_color, bool clear_depth) {
}

void gfx_metal_resolve_msaa_color_buffer(int fb_id_target, int fb_id_source) {
}

void* gfx_metal_get_framebuffer_texture_id(int fb_id) {
    return (void*)(intptr_t)fb_id;
}

void gfx_metal_select_texture_fb(int fb_id) {
}

void gfx_metal_delete_texture(uint32_t texID) {
}

void gfx_metal_set_texture_filter(FilteringMode mode) {
    sTextureFilter = mode;
}

FilteringMode gfx_metal_get_texture_filter(void) {
    return sTextureFilter;
}

void gfx_metal_set_mipmap_filter(MipmapFilteringMode mode) {
    sMipmapFilter = mode;
}

void gfx_metal_set_anisotropy_level(int level) {
    sAnisotropy = level;
}

int gfx_metal_get_max_anisotropy_level(void) {
    return 16;
}

} // namespace

struct GfxRenderingAPI gfx_metal_api = {
    gfx_metal_get_name,
    gfx_metal_get_max_texture_size,
    gfx_metal_get_clip_parameters,
    gfx_metal_unload_shader,
    gfx_metal_load_shader,
    gfx_metal_create_and_load_new_shader,
    gfx_metal_lookup_shader,
    gfx_metal_shader_get_info,
    gfx_metal_clear_shaders,
    gfx_metal_new_texture,
    gfx_metal_select_texture,
    gfx_metal_upload_texture,
    gfx_metal_set_sampler_parameters,
    gfx_metal_set_depth_mode,
    gfx_metal_set_depth_range,
    gfx_metal_set_viewport,
    gfx_metal_set_scissor,
    gfx_metal_set_use_alpha,
    gfx_metal_draw_triangles,
    gfx_metal_init,
    gfx_metal_on_resize,
    gfx_metal_start_frame,
    gfx_metal_end_frame,
    gfx_metal_finish_render,
    gfx_metal_create_framebuffer,
    gfx_metal_update_framebuffer_parameters,
    gfx_metal_start_draw_to_framebuffer,
    gfx_metal_copy_framebuffer,
    gfx_metal_clear_framebuffer,
    gfx_metal_resolve_msaa_color_buffer,
    gfx_metal_get_framebuffer_texture_id,
    gfx_metal_select_texture_fb,
    gfx_metal_delete_texture,
    gfx_metal_set_texture_filter,
    gfx_metal_get_texture_filter,
    gfx_metal_set_mipmap_filter,
    gfx_metal_set_anisotropy_level,
    gfx_metal_get_max_anisotropy_level,
};

#endif
