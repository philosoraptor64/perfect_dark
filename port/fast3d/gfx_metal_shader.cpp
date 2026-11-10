// Metal Shading Language generator for Fast3D color combiners.
//
// A line-by-line port of the GLSL generator in gfx_opengl.cpp (gfx_opengl_create_and_load_new_shader),
// so both backends render the same combiner math. GLSL names are kept through aliases at the top of
// every shader (vec2/3/4 = float2/3/4, GLSL-style mod), which lets the combiner expressions below be
// identical to the OpenGL ones. Keep the two generators in sync when one changes.
//
// Vertex data: the same interleaved float layout gfx_pc.cpp builds for OpenGL, read by vertex_id from
// buffer(0): position(4), per used texture uv(2) + clampS?(1) + clampT?(1), fog(4)?, grayscale(4)?,
// then num_inputs x (opt_alpha ? 4 : 3).
// Fragment bindings: texture(0..1) + sampler(0..1) = uTex0/uTex1, buffer(0) = FragUniforms.

#include <cstdarg>
#include <cstdio>
#include <string>

#include "gfx_cc.h"
#include "gfx_metal.h"

namespace {

#define RAND_NOISE "((random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + 1.0) / 2.0)"

const char* shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha,
                               bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "vec4(1.0, 1.0, 1.0, 1.0)" : "vec3(1.0, 1.0, 1.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a"
                                           : (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                         : "vec3(texVal0.a, texVal0.a, texVal0.a)");
            case SHADER_TEXEL1A:
                return hint_single_element ? "texVal1.a"
                                           : (with_alpha ? "vec4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                         : "vec3(texVal1.a, texVal1.a, texVal1.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
            case SHADER_NOISE:
                return with_alpha ? "vec4(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")"
                                  : "vec3(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")";
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_1:
                return "1.0";
            case SHADER_INPUT_1:
                return "vInput1.a";
            case SHADER_INPUT_2:
                return "vInput2.a";
            case SHADER_INPUT_3:
                return "vInput3.a";
            case SHADER_INPUT_4:
                return "vInput4.a";
            case SHADER_TEXEL0:
            case SHADER_TEXEL0A:
                return "texVal0.a";
            case SHADER_TEXEL1:
            case SHADER_TEXEL1A:
                return "texVal1.a";
            case SHADER_COMBINED:
                return "texel.a";
            case SHADER_NOISE:
                return RAND_NOISE;
        }
    }
    return "";
}

#undef RAND_NOISE

void append_formula(std::string& o, const uint8_t c[2][4], bool do_single, bool do_multiply, bool do_mix,
                    bool with_alpha, bool only_alpha, bool opt_alpha) {
    auto item = [&](int i, bool hint) { return shader_item_to_str(c[only_alpha][i], with_alpha, only_alpha, opt_alpha, hint); };
    if (do_single) {
        o += item(3, false);
    } else if (do_multiply) {
        o += item(0, false);
        o += " * ";
        o += item(2, true);
    } else if (do_mix) {
        o += "mix(";
        o += item(1, false);
        o += ", ";
        o += item(0, false);
        o += ", ";
        o += item(2, true);
        o += ")";
    } else {
        o += "(";
        o += item(0, false);
        o += " - ";
        o += item(1, false);
        o += ") * ";
        o += item(2, true);
        o += " + ";
        o += item(3, false);
    }
}

void appendf(std::string& o, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void appendf(std::string& o, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    o += buf;
}

} // namespace

std::string gfx_metal_generate_msl(const CCFeatures& cc, bool three_point, size_t* out_num_floats) {
    const bool tp = three_point;
    const int in_comps = cc.opt_alpha ? 4 : 3;
    std::string o;

    o += "#include <metal_stdlib>\nusing namespace metal;\n";
    o += "typedef float2 vec2; typedef float3 vec3; typedef float4 vec4;\n";
    // GLSL mod (floored), not fmod (truncated): WRAP depends on it for negative values.
    o += "template <typename T> inline T mod(T x, float y) { return x - y * floor(x / y); }\n";
    o += "#define WRAP(x, low, high) mod((x)-(low), (high)-(low)) + (low)\n";
    o += "struct FragUniforms { int frame_count; float noise_scale; int three_point_filter0; int three_point_filter1; };\n";

    // Varyings
    o += "struct Varyings {\n    float4 position [[position]];\n";
    for (int i = 0; i < 2; i++) {
        if (cc.used_textures[i]) {
            appendf(o, "    float2 vTexCoord%d;\n", i);
            for (int j = 0; j < 2; j++) {
                if (cc.clamp[i][j]) {
                    appendf(o, "    float vTexClamp%s%d;\n", j == 0 ? "S" : "T", i);
                }
            }
        }
    }
    if (cc.opt_fog) {
        o += "    float4 vFog;\n";
    }
    if (cc.opt_grayscale) {
        o += "    float4 vGrayscaleColor;\n";
    }
    for (int i = 0; i < cc.num_inputs; i++) {
        appendf(o, "    float%d vInput%d;\n", in_comps, i + 1);
    }
    o += "};\n";

    // Vertex shader: offsets follow the OpenGL attribute order.
    std::string vs_body;
    size_t off = 4;
    for (int i = 0; i < 2; i++) {
        if (cc.used_textures[i]) {
            appendf(vs_body, "    out.vTexCoord%d = float2(v[%zu], v[%zu]);\n", i, off, off + 1);
            off += 2;
            for (int j = 0; j < 2; j++) {
                if (cc.clamp[i][j]) {
                    appendf(vs_body, "    out.vTexClamp%s%d = v[%zu];\n", j == 0 ? "S" : "T", i, off);
                    off += 1;
                }
            }
        }
    }
    if (cc.opt_fog) {
        appendf(vs_body, "    out.vFog = float4(v[%zu], v[%zu], v[%zu], v[%zu]);\n", off, off + 1, off + 2, off + 3);
        off += 4;
    }
    if (cc.opt_grayscale) {
        appendf(vs_body, "    out.vGrayscaleColor = float4(v[%zu], v[%zu], v[%zu], v[%zu]);\n", off, off + 1, off + 2,
                off + 3);
        off += 4;
    }
    for (int i = 0; i < cc.num_inputs; i++) {
        if (in_comps == 4) {
            appendf(vs_body, "    out.vInput%d = float4(v[%zu], v[%zu], v[%zu], v[%zu]);\n", i + 1, off, off + 1,
                    off + 2, off + 3);
        } else {
            appendf(vs_body, "    out.vInput%d = float3(v[%zu], v[%zu], v[%zu]);\n", i + 1, off, off + 1, off + 2);
        }
        off += in_comps;
    }
    const size_t num_floats = off;
    if (out_num_floats) {
        *out_num_floats = num_floats;
    }

    appendf(o, "vertex Varyings vertexMain(uint vid [[vertex_id]], device const float* vbo [[buffer(0)]]) {\n"
               "    device const float* v = vbo + vid * %zu;\n"
               "    Varyings out;\n"
               "    out.position = float4(v[0], v[1], v[2], v[3]);\n",
            num_floats);
    o += vs_body;
    o += "    return out;\n}\n";

    // Fragment helpers
    o += "float random(vec3 value) {\n"
         "    float random = dot(sin(value), vec3(12.9898, 78.233, 37.719));\n"
         "    return fract(sin(random) * 143758.5453);\n"
         "}\n";
    o += "inline vec4 texOffset(texture2d<float> tex, sampler smp, vec2 uv, vec2 texSize, vec2 off) {\n"
         "    return tex.sample(smp, uv - off / texSize);\n"
         "}\n";
    if (tp) {
        o += "vec4 filter3point(texture2d<float> tex, sampler smp, vec2 texCoord, vec2 texSize) {\n"
             "    vec2 offset = fract(texCoord * texSize - vec2(0.5));\n"
             "    offset -= step(1.0, offset.x + offset.y);\n"
             "    vec4 c0 = texOffset(tex, smp, texCoord, texSize, offset);\n"
             "    vec4 c1 = texOffset(tex, smp, texCoord, texSize, vec2(offset.x - sign(offset.x), offset.y));\n"
             "    vec4 c2 = texOffset(tex, smp, texCoord, texSize, vec2(offset.x, offset.y - sign(offset.y)));\n"
             "    return c0 + abs(offset.x) * (c1 - c0) + abs(offset.y) * (c2 - c0);\n"
             "}\n";
    }
    const char* sample_expr = tp ? "mix(t.sample(smp, %s), filter3point(t, smp, %s, texSize), float(three_point_filter))"
                                 : "t.sample(smp, %s)";
    o += tp ? "vec4 hookTexture2D(texture2d<float> t, sampler smp, vec2 uv, vec2 texSize, int three_point_filter) {\n"
            : "vec4 hookTexture2D(texture2d<float> t, sampler smp, vec2 uv, vec2 texSize) {\n";
    if (cc.opt_blur) {
        // blur filter, used for menu backgrounds (same weights as the GLSL version)
        o += "    vec4 cw = vec4(0.0);\n"
             "    for (int i = 0; i < 16; ++i) {\n"
             "        vec2 xy = vec2(float(i & 3), float(i >> 2));\n"
             "        float w = 0.009947 - length(xy) * 0.001;\n"
             "        vec2 scaled_uv = uv + (vec2(-1.5) + xy) / texSize;\n";
        o += "        vec4 tex = ";
        appendf(o, sample_expr, "scaled_uv", "scaled_uv");
        o += ";\n"
             "        cw += vec4(tex.rgb * w, w);\n"
             "    }\n"
             "    return vec4(cw.rgb / cw.a, 1.0);\n";
    } else {
        o += "    return ";
        appendf(o, sample_expr, "uv", "uv");
        o += ";\n";
    }
    o += "}\n";

    // Fragment shader
    o += "fragment float4 fragmentMain(Varyings in [[stage_in]], constant FragUniforms& u [[buffer(0)]]";
    for (int i = 0; i < 2; i++) {
        if (cc.used_textures[i]) {
            appendf(o, ", texture2d<float> uTex%d [[texture(%d)]], sampler uSmp%d [[sampler(%d)]]", i, i, i, i);
        }
    }
    o += ") {\n";
    o += "    float4 gl_FragCoord = in.position;\n"
         "    int frame_count = u.frame_count;\n"
         "    float noise_scale = u.noise_scale;\n"
         "    (void)gl_FragCoord; (void)frame_count; (void)noise_scale;\n";
    for (int i = 0; i < cc.num_inputs; i++) {
        appendf(o, "    vec%d vInput%d = in.vInput%d;\n", in_comps, i + 1, i + 1);
    }
    if (cc.opt_fog) {
        o += "    vec4 vFog = in.vFog;\n";
    }
    if (cc.opt_grayscale) {
        o += "    vec4 vGrayscaleColor = in.vGrayscaleColor;\n";
    }

    for (int i = 0; i < 2; i++) {
        if (!cc.used_textures[i]) {
            continue;
        }
        const bool s = cc.clamp[i][0], t = cc.clamp[i][1];
        appendf(o, "    vec2 texSize%d = vec2(float(uTex%d.get_width()), float(uTex%d.get_height()));\n", i, i, i);
        appendf(o, "    vec2 vTexCoord%d = in.vTexCoord%d;\n", i, i);
        if (!s && !t) {
            appendf(o, "    vec2 vTexCoordAdj%d = vTexCoord%d;\n", i, i);
        } else if (s && t) {
            appendf(o, "    vec2 vTexCoordAdj%d = clamp(vTexCoord%d, 0.5 / texSize%d, vec2(in.vTexClampS%d, in.vTexClampT%d));\n",
                    i, i, i, i, i);
        } else if (s) {
            appendf(o, "    vec2 vTexCoordAdj%d = vec2(clamp(vTexCoord%d.x, 0.5 / texSize%d.x, in.vTexClampS%d), vTexCoord%d.y);\n",
                    i, i, i, i, i);
        } else {
            appendf(o, "    vec2 vTexCoordAdj%d = vec2(vTexCoord%d.x, clamp(vTexCoord%d.y, 0.5 / texSize%d.y, in.vTexClampT%d));\n",
                    i, i, i, i, i);
        }
        if (tp) {
            appendf(o, "    vec4 texVal%d = hookTexture2D(uTex%d, uSmp%d, vTexCoordAdj%d, texSize%d, u.three_point_filter%d);\n",
                    i, i, i, i, i, i);
        } else {
            appendf(o, "    vec4 texVal%d = hookTexture2D(uTex%d, uSmp%d, vTexCoordAdj%d, texSize%d);\n", i, i, i, i, i);
        }
    }

    o += cc.opt_alpha ? "    vec4 texel;\n" : "    vec3 texel;\n";
    for (int c = 0; c < (cc.opt_2cyc ? 2 : 1); c++) {
        o += "    texel = ";
        if (!cc.color_alpha_same[c] && cc.opt_alpha) {
            o += "vec4(";
            append_formula(o, cc.c[c], cc.do_single[c][0], cc.do_multiply[c][0], cc.do_mix[c][0], false, false, true);
            o += ", ";
            append_formula(o, cc.c[c], cc.do_single[c][1], cc.do_multiply[c][1], cc.do_mix[c][1], true, true, true);
            o += ")";
        } else {
            append_formula(o, cc.c[c], cc.do_single[c][0], cc.do_multiply[c][0], cc.do_mix[c][0], cc.opt_alpha, false,
                           cc.opt_alpha);
        }
        o += ";\n";
        if (c == 0) {
            o += "    texel = WRAP(texel, -1.01, 1.01);\n";
        }
    }
    o += "    texel = WRAP(texel, -0.51, 1.51);\n";
    o += "    texel = clamp(texel, 0.0, 1.0);\n";
    if (cc.opt_fog) {
        o += cc.opt_alpha ? "    texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);\n"
                          : "    texel = mix(texel, vFog.rgb, vFog.a);\n";
    }
    if (cc.opt_texture_edge && cc.opt_alpha) {
        o += "    if (texel.a > 0.19) texel.a = 1.0; else discard_fragment();\n";
    }
    if (cc.opt_alpha && cc.opt_noise) {
        o += "    texel.a *= floor(clamp(random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + "
             "texel.a, 0.0, 1.0));\n";
    }
    if (cc.opt_grayscale) {
        o += "    float intensity = (texel.r + texel.g + texel.b) / 3.0;\n"
             "    vec3 new_texel = vGrayscaleColor.rgb * intensity;\n"
             "    texel.rgb = mix(texel.rgb, new_texel, vGrayscaleColor.a);\n";
    }
    if (cc.opt_alpha) {
        if (cc.opt_alpha_threshold) {
            o += "    if (texel.a < 8.0 / 256.0) discard_fragment();\n";
        }
        if (cc.opt_invisible) {
            o += "    texel.a = 0.0;\n";
        }
        o += "    return texel;\n";
    } else {
        o += "    return vec4(texel, 1.0);\n";
    }
    o += "}\n";
    return o;
}
