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
#include <mutex>
#include <thread>
#include <atomic>
#include <set>
#include <tuple>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

extern "C" {
#include "platform.h"
#include "system.h"
}

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_metal.h"
#include <vector>
/* i64 (the module's n64_game_frame raises the request, the callback is n64_host.offer_title_art) */
extern "C" {
volatile int gPortFrameCaptureRequest = 0;
void (*gPortFrameCaptureCallback)(const uint8_t* rgba, uint32_t w, uint32_t h) = nullptr;
}

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
    id<MTLTexture> color; // fb > 0 only; fb 0 = the drawable + sDepthTex
    id<MTLTexture> depth;
};

// N64 texture-coordinate modes (gbi.h): G_TX_MIRROR = 1, G_TX_CLAMP = 2.
constexpr uint32_t kTxMirror = 1;
constexpr uint32_t kTxClamp = 2;
// select_texture_fb binds texture id kFbTextureBase + fb (opaque black if that fb has no texture).
constexpr uint32_t kNoFramebufferTexture = 0xFFFFFFFFu;
constexpr uint32_t kFbTextureBase = 0xFFFF0000u;

// GL semantics: sampler state belongs to the texture object; upload/sampler calls act on the
// texture bound to the active tile.
struct TextureMtl {
    id<MTLTexture> tex;
    id<MTLSamplerState> sampler;
};

id<MTLDevice> sDevice;
id<MTLCommandQueue> sQueue;
CAMetalLayer* sLayer;

std::map<std::pair<uint64_t, uint32_t>, ShaderProgram> sShaders;
ShaderProgram* sCurrentShader;
uint32_t sNextTextureId = 1;
std::unordered_map<uint32_t, TextureMtl> sTextures;
std::unordered_map<uint64_t, id<MTLSamplerState>> sSamplerCache;
uint32_t sBoundTexture[2];
bool sTileLinearFilter[2];
int sActiveTile;
std::vector<FramebufferMtl> sFramebuffers; // index 0 = the screen
FilteringMode sTextureFilter = FILTER_THREE_POINT;
MipmapFilteringMode sMipmapFilter = MIPMAP_DISABLED;
int sShadersCompiled;

// Per-frame drawing state (M4b). Until M6 everything is drawn straight to the drawable
// (video.c turns framebuffer effects off for Metal).
struct FragUniforms {
    int32_t frame_count;
    float noise_scale;
    int32_t three_point_filter0;
    int32_t three_point_filter1;
};
id<CAMetalDrawable> sDrawable;
id<MTLCommandBuffer> sCmd;
id<MTLRenderCommandEncoder> sEnc;
id<MTLTexture> sDepthTex;
id<MTLTexture> sFrontTex;
// Vertex ring: one shared buffer per frame in flight; the semaphore keeps the CPU from
// overwriting a buffer the GPU is still reading.
constexpr int kFramesInFlight = 3;
constexpr size_t kVertexRingSize = 8 * 1024 * 1024;
id<MTLBuffer> sVertexRing[kFramesInFlight];
size_t sVertexRingOffset;
int sVertexRingIndex;
dispatch_semaphore_t sFrameSemaphore; // copy of the last presented frame (GL "front buffer" for immediate copies)
id<MTLTexture> sWhiteTex;
id<MTLSamplerState> sDefaultSampler;
std::unordered_map<uint64_t, id<MTLRenderPipelineState>> sPipelines;
std::unordered_map<uint32_t, id<MTLDepthStencilState>> sDepthStates;
uint32_t sFrameCount;
uint32_t sStatDraws, sStatTris, sStatSkipped, sStatCopies, sStatCopyFail, sStatTargets, sStatFbSamples; // PD_METAL_STATS=1 logs them every 60 frames
float sNoiseScale = 1.0f;
bool sUseAlpha, sModulate;
bool sDepthTest, sDepthUpdate, sDepthDecal;
MTLCompareFunction sDepthFunc = MTLCompareFunctionLess;
float sZNear = 0.0f, sZFar = 1.0f;
int sTargetFb; // framebuffer the current pass draws to (0 = screen)
id<MTLRenderPipelineState> sCopyPipeline; // scaled framebuffer copies
id<MTLSamplerState> sLinearClampSampler;

// Shader warm-up (no compile stutter): every combiner shader and pipeline variant the game has
// used is listed in $S/pd.shaders (plus a built-in list from long test runs, pd_shaders_default.inc).
// A background thread compiles them all as soon as the renderer starts; the game then takes
// ready-made shaders/pipelines from the warm cache instead of compiling mid-frame. Anything never
// seen before still compiles on the spot (and is added to the list for next time).
typedef std::tuple<uint64_t, uint32_t, bool> WarmShaderKey;            // id0, id1, three-point
typedef std::tuple<uint64_t, uint32_t, bool, uint32_t> WarmPipeKey;   // + variant bits
struct WarmShader {
    id<MTLFunction> vfn;
    id<MTLFunction> ffn;
    size_t num_floats;
};
std::mutex sWarmMutex;
std::map<WarmShaderKey, WarmShader> sWarmShaders;
std::map<WarmPipeKey, id<MTLRenderPipelineState>> sWarmPipes;
std::set<WarmShaderKey> sListShaders;   // what the list file holds (written once each)
std::set<WarmPipeKey> sListPipes;
std::string sListPath;
std::thread sWarmThread;
std::atomic<bool> sWarmStop{ false };
std::atomic<int> sWarmShadersDone{ 0 }, sWarmPipesDone{ 0 };
int sVp[4], sSc[4];
int sShadersFailed;
int sAnisotropy = 1;

const char* gfx_metal_get_name(void) {
    return "Metal";
}

int gfx_metal_get_max_texture_size(void) {
    return 16384;
}

GfxClipParameters gfx_metal_get_clip_parameters(void) {
    // Metal clip space: z in [0, 1]; y up like GL, so no flip while drawing to the screen.
    // M6: offscreen framebuffers may need invert_y.
    return { true, false };
}

void gfx_metal_unload_shader(ShaderProgram* old_prg) {
    if (sCurrentShader == old_prg) {
        sCurrentShader = nullptr;
    }
}

void gfx_metal_load_shader(ShaderProgram* new_prg) {
    sCurrentShader = new_prg;
}

extern "C" const char* fsFullPath(const char* relPath);   // port/src/fs.c
/* shaders + pipelines built mid-frame (not from the warm cache); i64_module.c logs them with slow frames */
extern "C" { int g_MetalSpotCompiles = 0; }

static const char kWarmHeader[] = "pd-shaders v1";
static const char* const kDefaultShaderList =
#include "pd_shaders_default.inc"
    ;

static bool compile_shader_fns(const CCFeatures& cc, bool three_point, WarmShader* out, std::string* err_out) {
    std::string src = gfx_metal_generate_msl(cc, three_point, &out->num_floats);
    if (const char* salt = getenv("PD_SHADER_SALT")) {
        // test aid: new source text = a cold start (no system shader cache), as on a new device
        src += std::string("\n// salt ") + salt + "\n";
    }
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
        out->vfn = lib ? [lib newFunctionWithName:@"vertexMain"] : nil;
        out->ffn = lib ? [lib newFunctionWithName:@"fragmentMain"] : nil;
        if ((!out->vfn || !out->ffn) && err_out) {
            *err_out = std::string(err ? err.localizedDescription.UTF8String : "(no error)") + "\n--- source ---\n" + src;
        }
    }
    return out->vfn && out->ffn;
}

static uint32_t pipe_variant(NSUInteger samples, bool has_depth, bool alpha, bool modulate) {
    return ((uint32_t)samples << 3) | ((uint32_t)has_depth << 2) | ((uint32_t)alpha << 1) | (uint32_t)modulate;
}

static id<MTLRenderPipelineState> build_pipeline(id<MTLFunction> vfn, id<MTLFunction> ffn, uint32_t variant,
                                                 NSError** err) {
    const NSUInteger samples = variant >> 3;
    const bool has_depth = (variant >> 2) & 1, alpha = (variant >> 1) & 1, modulate = variant & 1;
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vfn;
    d.fragmentFunction = ffn;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm; // the layer's and every framebuffer's format
    d.depthAttachmentPixelFormat = has_depth ? MTLPixelFormatDepth32Float : MTLPixelFormatInvalid;
    d.rasterSampleCount = samples ? samples : 1;
    if (alpha) {
        MTLRenderPipelineColorAttachmentDescriptor* c = d.colorAttachments[0];
        c.blendingEnabled = YES;
        if (modulate) { // GL_DST_COLOR, GL_ZERO
            c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = MTLBlendFactorDestinationColor;
            c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor = MTLBlendFactorZero;
        } else { // GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA
            c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
            c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
    }
    return [sDevice newRenderPipelineStateWithDescriptor:d error:err];
}

// list lines: "S id0 id1 tp" (a shader) and "P id0 id1 tp variant" (a pipeline of it)
static void warm_parse_line(const char* line) {
    unsigned long long id0;
    unsigned int id1, tp, var;
    if (sscanf(line, "P %llx %x %u %x", &id0, &id1, &tp, &var) == 4) {
        sListPipes.insert(WarmPipeKey((uint64_t)id0, (uint32_t)id1, tp != 0, (uint32_t)var));
        sListShaders.insert(WarmShaderKey((uint64_t)id0, (uint32_t)id1, tp != 0));
    } else if (sscanf(line, "S %llx %x %u", &id0, &id1, &tp) == 3) {
        sListShaders.insert(WarmShaderKey((uint64_t)id0, (uint32_t)id1, tp != 0));
    }
}

static void warm_load_lists(void) {
    sListShaders.clear();
    sListPipes.clear();
    for (const char* p = kDefaultShaderList; *p;) {
        const char* e = strchr(p, '\n');
        std::string line(p, e ? (size_t)(e - p) : strlen(p));
        warm_parse_line(line.c_str());
        p = e ? e + 1 : p + line.size();
    }
    const char* path = fsFullPath("$S/pd.shaders");
    sListPath = path ? path : "";
    bool header_ok = false;
    if (FILE* f = sListPath.empty() ? nullptr : fopen(sListPath.c_str(), "r")) {
        char line[128];
        if (fgets(line, sizeof line, f) && !strncmp(line, kWarmHeader, strlen(kWarmHeader))) {
            header_ok = true;
            while (fgets(line, sizeof line, f)) {
                warm_parse_line(line);
            }
        }
        fclose(f);
    }
    if (!header_ok && !sListPath.empty()) {
        if (FILE* f = fopen(sListPath.c_str(), "w")) {
            fprintf(f, "%s\n", kWarmHeader);
            fclose(f);
        }
    }
}

static void warm_record(const char* line) {
    if (sListPath.empty()) {
        return;
    }
    if (FILE* f = fopen(sListPath.c_str(), "a")) {
        fputs(line, f);
        fclose(f);
    }
}

static void warm_record_shader(const WarmShaderKey& k) {
    std::lock_guard<std::mutex> lock(sWarmMutex);
    if (!sListShaders.insert(k).second) {
        return;
    }
    char line[96];
    snprintf(line, sizeof line, "S %016llx %08x %d\n", (unsigned long long)std::get<0>(k), std::get<1>(k),
             (int)std::get<2>(k));
    warm_record(line);
}

static void warm_record_pipe(const WarmPipeKey& k) {
    std::lock_guard<std::mutex> lock(sWarmMutex);
    if (!sListPipes.insert(k).second) {
        return;
    }
    char line[96];
    snprintf(line, sizeof line, "P %016llx %08x %d %x\n", (unsigned long long)std::get<0>(k), std::get<1>(k),
             (int)std::get<2>(k), std::get<3>(k));
    warm_record(line);
}

static void warm_thread_main(std::vector<WarmShaderKey> shaders, std::vector<WarmPipeKey> pipes) {
    // all cores: the compiles are independent (MTLDevice is thread-safe)
    const uint64_t t0 = sysGetMicroseconds();
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    dispatch_apply(shaders.size(), q, ^(size_t i) {
        const WarmShaderKey& k = shaders[i];
        if (sWarmStop) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(sWarmMutex);
            if (sWarmShaders.count(k)) {
                return;
            }
        }
        CCFeatures cc;
        gfx_cc_get_features(std::get<0>(k), std::get<1>(k), &cc);
        WarmShader w{};
        if (compile_shader_fns(cc, std::get<2>(k), &w, nullptr)) {
            std::lock_guard<std::mutex> lock(sWarmMutex);
            if (sWarmShaders.emplace(k, w).second) {
                sWarmShadersDone++;
            }
        }
    });
    dispatch_apply(pipes.size(), q, ^(size_t i) {
        const WarmPipeKey& k = pipes[i];
        if (sWarmStop) {
            return;
        }
        WarmShader w{};
        {
            std::lock_guard<std::mutex> lock(sWarmMutex);
            if (sWarmPipes.count(k)) {
                return;
            }
            auto it = sWarmShaders.find(WarmShaderKey(std::get<0>(k), std::get<1>(k), std::get<2>(k)));
            if (it == sWarmShaders.end()) {
                return;
            }
            w = it->second;
        }
        @autoreleasepool {
            NSError* err = nil;
            id<MTLRenderPipelineState> ps = build_pipeline(w.vfn, w.ffn, std::get<3>(k), &err);
            if (ps) {
                std::lock_guard<std::mutex> lock(sWarmMutex);
                if (sWarmPipes.emplace(k, ps).second) {
                    sWarmPipesDone++;
                }
            }
        }
    });
    sysLogPrintf(LOG_NOTE, "Metal: warm-up done: %d shaders, %d pipelines in %.0f ms", (int)sWarmShadersDone,
                 (int)sWarmPipesDone, (double)(sysGetMicroseconds() - t0) / 1000.0);
}

static void warm_start(void) {
    if (getenv("PD_NOSHADERWARM")) {
        return;
    }
    warm_load_lists();
    std::vector<WarmShaderKey> shaders(sListShaders.begin(), sListShaders.end());
    std::vector<WarmPipeKey> pipes(sListPipes.begin(), sListPipes.end());
    sWarmStop = false;
    sWarmShadersDone = 0;
    sWarmPipesDone = 0;
    sysLogPrintf(LOG_NOTE, "Metal: warming %d shaders, %d pipelines in the background", (int)shaders.size(),
                 (int)pipes.size());
    sWarmThread = std::thread(warm_thread_main, std::move(shaders), std::move(pipes));
}

static void warm_stop(void) {
    sWarmStop = true;
    if (sWarmThread.joinable()) {
        sWarmThread.join();
    }
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
    const WarmShaderKey wk(shader_id0, shader_id1, prg.three_point);
    bool warm = false;
    {
        std::lock_guard<std::mutex> lock(sWarmMutex);
        auto it = sWarmShaders.find(wk);
        if (it != sWarmShaders.end()) {
            prg.vertex_fn = it->second.vfn;
            prg.fragment_fn = it->second.ffn;
            prg.num_floats = it->second.num_floats;
            warm = true;
        }
    }
    if (!warm) {
        WarmShader w{};
        std::string err;
        if (compile_shader_fns(cc, prg.three_point, &w, &err)) {
            prg.vertex_fn = w.vfn;
            prg.fragment_fn = w.ffn;
            prg.num_floats = w.num_floats;
            {
                std::lock_guard<std::mutex> lock(sWarmMutex);
                sWarmShaders.emplace(wk, w);
            }
            warm_record_shader(wk);
            g_MetalSpotCompiles++;
            if (++sShadersCompiled % 25 == 1) {
                sysLogPrintf(LOG_NOTE, "Metal: %d shaders compiled on the spot (not warmed), %d failed",
                             sShadersCompiled, sShadersFailed);
            }
        } else {
            prg.vertex_fn = nil;
            prg.fragment_fn = nil;
            prg.num_floats = w.num_floats;
            if (sShadersFailed++ < 3) {
                sysLogPrintf(LOG_ERROR, "Metal: shader %016llx/%08x failed to compile:\n%s",
                             (unsigned long long)shader_id0, shader_id1, err.c_str());
            }
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
    sPipelines.clear(); // keyed by ShaderProgram address: a new program could reuse a freed address
}

uint32_t gfx_metal_new_texture(void) {
    return sNextTextureId++;
}

void gfx_metal_select_texture(int tile, uint32_t texture_id, bool linear_filter) {
    sActiveTile = tile;
    sBoundTexture[tile] = texture_id;
    sTileLinearFilter[tile] = linear_filter;
}

void gfx_metal_upload_texture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height, bool gen_mipmaps) {
    TextureMtl& t = sTextures[sBoundTexture[sActiveTile]];
    const bool mips = gen_mipmaps || sTextureFilter == FILTER_THREE_POINT; // same rule as the GL backend
    if (!t.tex || t.tex.width != width || t.tex.height != height || (t.tex.mipmapLevelCount > 1) != mips) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:width
                                                                                    height:height
                                                                                 mipmapped:mips];
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        t.tex = [sDevice newTextureWithDescriptor:d];
    }
    [t.tex replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:rgba32_buf bytesPerRow:width * 4];
    if (mips && t.tex.mipmapLevelCount > 1) {
        id<MTLCommandBuffer> cmd = [sQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        [blit generateMipmapsForTexture:t.tex];
        [blit endEncoding];
        [cmd commit];
    }
}

MTLSamplerAddressMode address_mode(uint32_t cm) {
    switch (cm & (kTxMirror | kTxClamp)) {
        case kTxClamp:
            return MTLSamplerAddressModeClampToEdge;
        case kTxMirror:
            return MTLSamplerAddressModeMirrorRepeat;
        case kTxMirror | kTxClamp:
            return MTLSamplerAddressModeMirrorClampToEdge;
        default:
            return MTLSamplerAddressModeRepeat;
    }
}

void gfx_metal_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt, bool mipmaps) {
    sActiveTile = tile;
    // Mirrors gfx_opengl_set_sampler_parameters' filter table.
    mipmaps = mipmaps && sMipmapFilter != MIPMAP_DISABLED;
    const bool three_point = sTextureFilter == FILTER_THREE_POINT;
    const bool min_linear = linear_filter && sTextureFilter == FILTER_LINEAR;
    const bool mag_linear = linear_filter && sTextureFilter == FILTER_LINEAR;
    const int mip = (!mipmaps || three_point || !linear_filter) ? 0 : (int)sMipmapFilter; // 0 none, 1 nearest, 2 linear
    const int aniso = mipmaps ? std::max(1, std::min(16, sAnisotropy)) : 1;
    const uint64_t key = (uint64_t)min_linear | ((uint64_t)mag_linear << 1) | ((uint64_t)mip << 2) |
                         ((uint64_t)address_mode(cms) << 4) | ((uint64_t)address_mode(cmt) << 8) |
                         ((uint64_t)aniso << 12);
    __strong id<MTLSamplerState>& smp = sSamplerCache[key];
    if (!smp) {
        MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
        d.minFilter = min_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.magFilter = mag_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.mipFilter = mip == 0 ? MTLSamplerMipFilterNotMipmapped
                               : (mip == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear);
        d.sAddressMode = address_mode(cms);
        d.tAddressMode = address_mode(cmt);
        d.maxAnisotropy = aniso;
        smp = [sDevice newSamplerStateWithDescriptor:d];
    }
    sTextures[sBoundTexture[tile]].sampler = smp;
}

// zmode is the raw RDP other_mode_l field (gbi.h: ZMODE_OPA 0, ZMODE_INTER 0x400, ZMODE_XLU 0x800,
// ZMODE_DEC 0xc00), as gfx_pc.cpp passes it.
void gfx_metal_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare, bool depth_source_prim,
                              uint16_t zmode) {
    sDepthTest = depth_test;
    sDepthUpdate = depth_update;
    sDepthDecal = false;
    if (!depth_compare) {
        sDepthFunc = MTLCompareFunctionAlways;
    } else if (zmode == 0x400 /* ZMODE_INTER */) {
        sDepthFunc = MTLCompareFunctionLessEqual;
    } else if (zmode == 0xc00 /* ZMODE_DEC */) {
        sDepthFunc = MTLCompareFunctionLessEqual;
        sDepthDecal = true;
    } else {
        sDepthFunc = depth_source_prim ? MTLCompareFunctionLessEqual : MTLCompareFunctionLess;
    }
}

void gfx_metal_set_depth_range(float znear, float zfar) {
    sZNear = znear;
    sZFar = zfar;
}

void gfx_metal_set_viewport(int x, int y, int width, int height) {
    sVp[0] = x; sVp[1] = y; sVp[2] = width; sVp[3] = height;
}

void gfx_metal_set_scissor(int x, int y, int width, int height) {
    sSc[0] = x; sSc[1] = y; sSc[2] = width; sSc[3] = height;
}

void gfx_metal_set_use_alpha(bool use_alpha, bool modulate) {
    sUseAlpha = use_alpha;
    sModulate = modulate;
}

id<MTLTexture> target_color(void) {
    return sTargetFb == 0 ? (sDrawable ? sDrawable.texture : nil) : sFramebuffers[sTargetFb].color;
}

id<MTLTexture> target_depth(void) {
    return sTargetFb == 0 ? sDepthTex : sFramebuffers[sTargetFb].depth;
}

id<MTLRenderPipelineState> pipeline_for(ShaderProgram* prg) {
    const bool has_depth = target_depth() != nil;
    id<MTLTexture> color = target_color();
    const NSUInteger samples = color ? color.sampleCount : 1;
    const uint64_t key = ((uint64_t)(uintptr_t)prg << 8) | ((uint64_t)samples << 3) | ((uint64_t)has_depth << 2) |
                         ((uint64_t)sUseAlpha << 1) | (uint64_t)sModulate;
    __strong id<MTLRenderPipelineState>& ps = sPipelines[key];
    if (!ps) {
        const uint32_t variant = pipe_variant(samples, has_depth, sUseAlpha, sModulate);
        const WarmPipeKey wk(prg->shader_id0, prg->shader_id1, prg->three_point, variant);
        {
            std::lock_guard<std::mutex> lock(sWarmMutex);
            auto it = sWarmPipes.find(wk);
            if (it != sWarmPipes.end()) {
                ps = it->second;
            }
        }
        if (!ps) {
            NSError* err = nil;
            ps = build_pipeline(prg->vertex_fn, prg->fragment_fn, variant, &err);
            if (!ps) {
                sysLogPrintf(LOG_ERROR, "Metal: pipeline failed: %s", err.localizedDescription.UTF8String);
            } else {
                {
                    std::lock_guard<std::mutex> lock(sWarmMutex);
                    sWarmPipes.emplace(wk, ps);
                }
                warm_record_pipe(wk);
                g_MetalSpotCompiles++;
            }
        }
    }
    return ps;
}

id<MTLDepthStencilState> depth_state(void) {
    const bool test = sDepthTest && target_depth() != nil;
    const uint32_t key = (uint32_t)test | ((uint32_t)sDepthUpdate << 1) | ((uint32_t)sDepthFunc << 2);
    __strong id<MTLDepthStencilState>& ds = sDepthStates[key];
    if (!ds) {
        MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
        // GL: with the depth test disabled nothing is tested or written.
        d.depthCompareFunction = test ? sDepthFunc : MTLCompareFunctionAlways;
        d.depthWriteEnabled = test && sDepthUpdate;
        ds = [sDevice newDepthStencilStateWithDescriptor:d];
    }
    return ds;
}

void begin_pass(bool clear_color, bool clear_depth) {
    if (sEnc) {
        [sEnc endEncoding];
        sEnc = nil;
    }
    id<MTLTexture> color = target_color();
    if (!sCmd || !color) {
        return;
    }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = clear_color ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    pass.depthAttachment.texture = target_depth();
    pass.depthAttachment.loadAction = clear_depth ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 1.0;
    sEnc = [sCmd renderCommandEncoderWithDescriptor:pass];
}

void gfx_metal_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    ShaderProgram* prg = sCurrentShader;
    if (!sEnc || !prg || !prg->vertex_fn || buf_vbo_num_tris == 0) {
        sStatSkipped++;
        return;
    }
    id<MTLRenderPipelineState> ps = pipeline_for(prg);
    if (!ps) {
        return;
    }
    id<MTLTexture> color = target_color();
    const int fbw = (int)color.width, fbh = (int)color.height;
    [sEnc setRenderPipelineState:ps];
    [sEnc setDepthStencilState:depth_state()];
    [sEnc setDepthClipMode:MTLDepthClipModeClamp]; // the GL backend enables GL_DEPTH_CLAMP
    [sEnc setDepthBias:(sDepthDecal ? -2.0f : 0.0f) slopeScale:(sDepthDecal ? -2.0f : 0.0f) clamp:0.0f];
    // Fast3D passes GL-style rectangles (origin bottom-left): flip to Metal's top-left origin.
    [sEnc setViewport:(MTLViewport){ (double)sVp[0], (double)(fbh - sVp[1] - sVp[3]), (double)sVp[2], (double)sVp[3],
                                     sZNear, sZFar }];
    int sx = std::max(0, sSc[0]), sy = std::max(0, fbh - sSc[1] - sSc[3]);
    int sw = std::min(sSc[2], fbw - sx), sh = std::min(sSc[3], fbh - sy);
    if (sw <= 0 || sh <= 0) {
        return;
    }
    [sEnc setScissorRect:(MTLScissorRect){ (NSUInteger)sx, (NSUInteger)sy, (NSUInteger)sw, (NSUInteger)sh }];

    const size_t bytes = buf_vbo_len * sizeof(float);
    if (bytes <= 4096) {
        [sEnc setVertexBytes:buf_vbo length:bytes atIndex:0];
    } else if (sVertexRingOffset + bytes <= kVertexRingSize) {
        id<MTLBuffer> ring = sVertexRing[sVertexRingIndex];
        memcpy((uint8_t*)ring.contents + sVertexRingOffset, buf_vbo, bytes);
        [sEnc setVertexBuffer:ring offset:sVertexRingOffset atIndex:0];
        sVertexRingOffset = (sVertexRingOffset + bytes + 255) & ~(size_t)255;
    } else {
        id<MTLBuffer> vb = [sDevice newBufferWithBytes:buf_vbo length:bytes options:MTLResourceStorageModeShared];
        [sEnc setVertexBuffer:vb offset:0 atIndex:0];
    }
    FragUniforms u = { (int32_t)sFrameCount, sNoiseScale, sTileLinearFilter[0], sTileLinearFilter[1] };
    [sEnc setFragmentBytes:&u length:sizeof(u) atIndex:0];
    for (int i = 0; i < 2; i++) {
        if (!prg->used_textures[i]) {
            continue;
        }
        auto it = sTextures.find(sBoundTexture[i]);
        const bool ok = it != sTextures.end() && it->second.tex;
        [sEnc setFragmentTexture:(ok ? it->second.tex : sWhiteTex) atIndex:i];
        [sEnc setFragmentSamplerState:((ok && it->second.sampler) ? it->second.sampler : sDefaultSampler) atIndex:i];
    }
    [sEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 * buf_vbo_num_tris];
    sStatDraws++;
    sStatTris += (uint32_t)buf_vbo_num_tris;
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
    sLayer.framebufferOnly = NO; // the screen is a copy_framebuffer source
    sQueue = [sDevice newCommandQueue];
    for (int i = 0; i < kFramesInFlight; i++) {
        sVertexRing[i] = [sDevice newBufferWithLength:kVertexRingSize options:MTLResourceStorageModeShared];
    }
    sFrameSemaphore = dispatch_semaphore_create(kFramesInFlight);
    sFramebuffers.assign(1, FramebufferMtl{});
    {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:1
                                                                                    height:1
                                                                                 mipmapped:NO];
        sWhiteTex = [sDevice newTextureWithDescriptor:d];
        const uint32_t white = 0xffffffffu;
        [sWhiteTex replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&white bytesPerRow:4];
        sDefaultSampler = [sDevice newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        id<MTLTexture> black = [sDevice newTextureWithDescriptor:d];
        const uint32_t opaque_black = 0xff000000u;
        [black replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&opaque_black bytesPerRow:4];
        sTextures[kNoFramebufferTexture] = { black, sDefaultSampler };
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sLinearClampSampler = [sDevice newSamplerStateWithDescriptor:sd];
    }
    {
        // Scaled framebuffer copy: one triangle covering the destination, sampling the source rectangle.
        static const char* kCopySrc =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 pos [[position]]; float2 uv; };\n"
            "vertex V copyVS(uint vid [[vertex_id]], constant float4& rect [[buffer(0)]]) {\n"
            "    float2 p = float2((vid << 1) & 2, vid & 2);\n"
            "    V o; o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
            "    o.uv = rect.xy + p * rect.zw; return o; }\n"
            "fragment float4 copyFS(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
            "    return t.sample(s, in.uv); }\n";
        NSError* err = nil;
        id<MTLLibrary> lib = [sDevice newLibraryWithSource:@(kCopySrc) options:nil error:&err];
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"copyVS"];
        d.fragmentFunction = [lib newFunctionWithName:@"copyFS"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        sCopyPipeline = [sDevice newRenderPipelineStateWithDescriptor:d error:&err];
        if (!sCopyPipeline) {
            sysLogPrintf(LOG_ERROR, "Metal: copy pipeline failed: %s", err.localizedDescription.UTF8String);
        }
    }
    sysLogPrintf(LOG_NOTE, "Metal: device %s", sDevice.name.UTF8String);
    warm_start();

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

extern "C" bool gfx_sdl_app_inactive;

void gfx_metal_start_frame(void) {
    sFrameCount++;
    if (gfx_sdl_app_inactive) {
        sDrawable = nil; // nothing is encoded or presented while the app is in the background
        return;
    }
    sDrawable = [sLayer nextDrawable];
    if (!sDrawable) {
        return;
    }
    dispatch_semaphore_wait(sFrameSemaphore, DISPATCH_TIME_FOREVER);
    sVertexRingIndex = (sVertexRingIndex + 1) % kFramesInFlight;
    sVertexRingOffset = 0;
    const NSUInteger w = sDrawable.texture.width, h = sDrawable.texture.height;
    if (!sDepthTex || sDepthTex.width != w || sDepthTex.height != h) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                     width:w
                                                                                    height:h
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget;
        d.storageMode = MTLStorageModePrivate;
        sDepthTex = [sDevice newTextureWithDescriptor:d];
    }
    sCmd = [sQueue commandBuffer];
    sTargetFb = 0;
    begin_pass(true, true);
}

void gfx_metal_end_frame(void) {
    if (sEnc) {
        [sEnc endEncoding];
        sEnc = nil;
    }
    if (sCmd) {
        if (sDrawable) {
            id<MTLTexture> back = sDrawable.texture;
            if (!sFrontTex || sFrontTex.width != back.width || sFrontTex.height != back.height) {
                MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:back.pixelFormat
                                                                                             width:back.width
                                                                                            height:back.height
                                                                                         mipmapped:NO];
                d.usage = MTLTextureUsageShaderRead;
                d.storageMode = MTLStorageModePrivate;
                sFrontTex = [sDevice newTextureWithDescriptor:d];
            }
            id<MTLBlitCommandEncoder> blit = [sCmd blitCommandEncoder];
            [blit copyFromTexture:back toTexture:sFrontTex];
            [blit endEncoding];
            if (gPortFrameCaptureRequest && gPortFrameCaptureCallback && back.pixelFormat == MTLPixelFormatBGRA8Unorm) {
                /* i64: one picture of the screen for its home screen (finished on Metal's completion thread) */
                gPortFrameCaptureRequest = 0;
                const NSUInteger cw = back.width, ch = back.height;
                id<MTLBuffer> cap = [sDevice newBufferWithLength:cw * ch * 4 options:MTLResourceStorageModeShared];
                id<MTLBlitCommandEncoder> cb = [sCmd blitCommandEncoder];
                [cb copyFromTexture:back sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                         sourceSize:MTLSizeMake(cw, ch, 1) toBuffer:cap destinationOffset:0
                destinationBytesPerRow:cw * 4 destinationBytesPerImage:cw * ch * 4];
                [cb endEncoding];
                void (*fn)(const uint8_t*, uint32_t, uint32_t) = gPortFrameCaptureCallback;
                [sCmd addCompletedHandler:^(id<MTLCommandBuffer>) {
                    std::vector<uint8_t> rgba(cw * ch * 4);
                    const uint8_t* src = (const uint8_t*) cap.contents;
                    for (size_t k = 0; k < (size_t) cw * ch; k++) {
                        rgba[4 * k + 0] = src[4 * k + 2];
                        rgba[4 * k + 1] = src[4 * k + 1];
                        rgba[4 * k + 2] = src[4 * k + 0];
                        rgba[4 * k + 3] = 255;
                    }
                    fn(rgba.data(), (uint32_t) cw, (uint32_t) ch);
                }];
            }
            [sCmd presentDrawable:sDrawable];
            dispatch_semaphore_t sem = sFrameSemaphore;
            [sCmd addCompletedHandler:^(id<MTLCommandBuffer>) {
                dispatch_semaphore_signal(sem);
            }];
        }
        [sCmd commit];
    }
    sCmd = nil;
    sDrawable = nil;
    static const bool stats = getenv("PD_METAL_STATS") != nullptr;
    if (stats && sFrameCount % 60 == 0) {
        sysLogPrintf(LOG_NOTE,
                     "Metal stats: frame %u, last 60 frames: %u draws, %u tris, %u skipped, %zu shaders, "
                     "%u copies (%u failed), %u target switches, %u fb samples, %zu fbs",
                     sFrameCount, sStatDraws, sStatTris, sStatSkipped, sShaders.size(), sStatCopies, sStatCopyFail,
                     sStatTargets, sStatFbSamples, sFramebuffers.size());
        sStatDraws = sStatTris = sStatSkipped = sStatCopies = sStatCopyFail = sStatTargets = sStatFbSamples = 0;
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
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (fb_id != 0) {
        // MSAA targets are multisampled textures; they are never sampled directly (resolve first).
        uint32_t samples = std::max(1u, msaa_level);
        while (samples > 1 && ![sDevice supportsTextureSampleCount:samples]) {
            samples >>= 1;
        }
        if (!fb.color || fb.color.width != width || fb.color.height != height || fb.color.sampleCount != samples) {
            MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                         width:width
                                                                                        height:height
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModePrivate;
            if (samples > 1) {
                d.textureType = MTLTextureType2DMultisample;
                d.sampleCount = samples;
            }
            fb.color = [sDevice newTextureWithDescriptor:d];
            fb.depth = nil;
            if (samples > 1) {
                sTextures.erase(kFbTextureBase + fb_id);
            } else {
                sTextures[kFbTextureBase + fb_id] = { fb.color, sLinearClampSampler };
            }
        }
        if (has_depth_buffer && !fb.depth) {
            MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                         width:width
                                                                                        height:height
                                                                                     mipmapped:NO];
            if (samples > 1) {
                d.textureType = MTLTextureType2DMultisample;
                d.sampleCount = samples;
            }
            d.usage = MTLTextureUsageRenderTarget;
            d.storageMode = MTLStorageModePrivate;
            fb.depth = [sDevice newTextureWithDescriptor:d];
        } else if (!has_depth_buffer) {
            fb.depth = nil;
        }
    }
    fb.width = width;
    fb.height = height;
    fb.msaa_level = msaa_level;
    fb.render_target = render_target;
    fb.has_depth = has_depth_buffer;
}

bool gfx_metal_start_draw_to_framebuffer(int fb_id, float noise_scale) {
    if (noise_scale != 0.0f) {
        sNoiseScale = 1.0f / noise_scale;
    }
    if (fb_id < 0 || fb_id >= (int)sFramebuffers.size() || (fb_id != 0 && !sFramebuffers[fb_id].color)) {
        static int warned;
        if (warned++ < 5) {
            sysLogPrintf(LOG_WARNING, "Metal: start_draw_to_framebuffer(%d) refused (%zu fbs, has color %d)", fb_id,
                         sFramebuffers.size(), fb_id > 0 && fb_id < (int)sFramebuffers.size() && sFramebuffers[fb_id].color);
        }
        return false;
    }
    if (fb_id != sTargetFb) {
        sStatTargets++;
        sTargetFb = fb_id;
        begin_pass(false, false);
    }
    return true;
}

void resolve_into(id<MTLCommandBuffer> cmd, id<MTLTexture> msaa, id<MTLTexture> dst);
id<MTLTexture> resolve_temp(NSUInteger w, NSUInteger h);
void scaled_copy(id<MTLCommandBuffer> cmd, id<MTLTexture> src, id<MTLTexture> dst);

id<MTLTexture> fb_color(int fb_id) {
    if (fb_id == 0) {
        return sDrawable ? sDrawable.texture : nil;
    }
    return (fb_id > 0 && fb_id < (int)sFramebuffers.size()) ? sFramebuffers[fb_id].color : nil;
}

// GL's flip_y only compensates for GL's bottom-left origin when the screen is the source; the
// drawable and the offscreen textures are both top-row-first in Metal, so no flip is needed.
void gfx_metal_copy_framebuffer(int fb_dst, int fb_src, int left, int top, bool flip_y, bool use_back) {
    // Screen as source: the back buffer only while a frame is being drawn and use_back is set;
    // otherwise the last presented frame, like GL's front buffer.
    const bool in_frame = sCmd != nil;
    id<MTLTexture> src = (fb_src == 0 && (!use_back || !in_frame)) ? sFrontTex : fb_color(fb_src);
    id<MTLTexture> dst = fb_color(fb_dst);
    if (!src || !dst || src == dst) {
        sStatCopyFail++;
        return;
    }
    sStatCopies++;
    if (sEnc) {
        [sEnc endEncoding];
        sEnc = nil;
    }
    id<MTLCommandBuffer> cmd = in_frame ? sCmd : [sQueue commandBuffer];
    if (src.sampleCount > 1) {
        // GL's glBlitFramebuffer resolves implicitly; Metal needs an explicit resolve first.
        id<MTLTexture> tmp = resolve_temp(src.width, src.height);
        resolve_into(cmd, src, tmp);
        src = tmp;
    }
    const int sw = (int)src.width, sh = (int)src.height, dw = (int)dst.width, dh = (int)dst.height;
    if (left >= 0 && top >= 0) {
        // Unscaled rectangle at (left, top) in GL bottom-left coordinates -> Metal top-left.
        const int x = std::min(left, sw - 1);
        const int w = std::min(dw, sw - x);
        const int y = std::max(0, sh - (top + dh));
        const int h = std::min(dh, sh - y);
        if (w > 0 && h > 0) {
            id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
            [blit copyFromTexture:src
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(x, y, 0)
                       sourceSize:MTLSizeMake(w, h, 1)
                        toTexture:dst
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
        }
    } else {
        scaled_copy(cmd, src, dst); // scaled full copy
    }
    if (in_frame) {
        begin_pass(false, false); // resume the current target
    } else {
        [cmd commit];
    }
}

void gfx_metal_clear_framebuffer(bool clear_color, bool clear_depth) {
    if (sCmd && (clear_color || clear_depth)) {
        begin_pass(clear_color, clear_depth); // a Metal clear = a new pass with LoadActionClear
    }
}

// Resolves a multisampled texture into a single-sampled one of the same size (render pass resolve).
void resolve_into(id<MTLCommandBuffer> cmd, id<MTLTexture> msaa, id<MTLTexture> dst) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = msaa;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    pass.colorAttachments[0].resolveTexture = dst;
    [[cmd renderCommandEncoderWithDescriptor:pass] endEncoding];
}

id<MTLTexture> sResolveTemp; // single-sampled scratch for resolves/copies of a different size

id<MTLTexture> resolve_temp(NSUInteger w, NSUInteger h) {
    if (!sResolveTemp || sResolveTemp.width != w || sResolveTemp.height != h) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:w
                                                                                    height:h
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        sResolveTemp = [sDevice newTextureWithDescriptor:d];
    }
    return sResolveTemp;
}

void scaled_copy(id<MTLCommandBuffer> cmd, id<MTLTexture> src, id<MTLTexture> dst) {
    if (!sCopyPipeline) {
        return;
    }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = dst;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
    const float rect[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    [enc setRenderPipelineState:sCopyPipeline];
    [enc setVertexBytes:rect length:sizeof(rect) atIndex:0];
    [enc setFragmentTexture:src atIndex:0];
    [enc setFragmentSamplerState:sLinearClampSampler atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
}

void gfx_metal_resolve_msaa_color_buffer(int fb_id_target, int fb_id_source) {
    id<MTLTexture> src = fb_color(fb_id_source);
    id<MTLTexture> dst = fb_color(fb_id_target);
    if (!sCmd || !src || !dst || src.sampleCount <= 1 || dst.sampleCount > 1) {
        return;
    }
    if (sEnc) {
        [sEnc endEncoding];
        sEnc = nil;
    }
    if (src.width == dst.width && src.height == dst.height) {
        resolve_into(sCmd, src, dst);
    } else {
        id<MTLTexture> tmp = resolve_temp(src.width, src.height);
        resolve_into(sCmd, src, tmp);
        scaled_copy(sCmd, tmp, dst);
    }
    begin_pass(false, false);
}

void* gfx_metal_get_framebuffer_texture_id(int fb_id) {
    return (void*)(intptr_t)fb_id;
}

void gfx_metal_select_texture_fb(int fb_id) {
    sActiveTile = 0;
    sBoundTexture[0] = sTextures.count(kFbTextureBase + fb_id) ? kFbTextureBase + fb_id : kNoFramebufferTexture;
    sStatFbSamples++;
    sTileLinearFilter[0] = true;
}

void gfx_metal_delete_texture(uint32_t texID) {
    sTextures.erase(texID);
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

/* i64 module stop (re-entry): release every Metal object before the globals
 * are restored (a restore only drops the references). Waits for the frames
 * still on the GPU and puts the frame semaphore back to its starting count
 * first (libdispatch aborts if a semaphore is freed below it). */
extern "C" void gfx_metal_release_all(void) {
    warm_stop();
    {
        std::lock_guard<std::mutex> lock(sWarmMutex);
        sWarmShaders.clear();
        sWarmPipes.clear();
        sListShaders.clear();
        sListPipes.clear();
    }
    if (!sDevice) {
        return;
    }
    if (sFrameSemaphore) {
        int got = 0;
        for (int i = 0; i < kFramesInFlight; i++) {
            if (dispatch_semaphore_wait(sFrameSemaphore, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) == 0) {
                got++;
            }
        }
        for (int i = 0; i < got; i++) {
            dispatch_semaphore_signal(sFrameSemaphore);
        }
    }
    sEnc = nil;
    sCmd = nil;
    sDrawable = nil;
    sShaders.clear();
    sCurrentShader = nullptr;
    sTextures.clear();
    sSamplerCache.clear();
    sFramebuffers.clear();
    sPipelines.clear();
    sDepthStates.clear();
    sDepthTex = nil;
    sFrontTex = nil;
    sResolveTemp = nil;
    for (int i = 0; i < kFramesInFlight; i++) {
        sVertexRing[i] = nil;
    }
    sWhiteTex = nil;
    sDefaultSampler = nil;
    sLinearClampSampler = nil;
    sCopyPipeline = nil;
    sFrameSemaphore = nil;
    sLayer = nil;
    sQueue = nil;
    sDevice = nil;
}

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
