// Build-time host tool: writes every shader variant of upstream melonDS's compute 3D
// renderer (mds_upstream/src/GPU3D_Compute_shaders.h) as a standalone GLSL file that
// uam (deko3d's offline compiler) accepts. Usage: gen_compute_shaders <out dir>
//
// The sources are assembled exactly as ComputeRenderer3D::CompileShader does (version,
// defines, the size constants for scale 1, Common, the shader), then adapted to deko3d:
// - loose `layout(location = N) uniform`s become a uniform block (binding 1);
// - the SetupIndices texel buffer becomes a storage buffer (binding 3) of packed u16x4;
// - the final pass writes the packed RGB6A5 colour (the software renderer's 3D line
//   format) into a storage buffer (binding 1) instead of normalising it into an image,
//   so the CPU 2D engines can read it as is.
// The variant list and the file names must match gpu/deko_renderer3d.cpp.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "GPU3D_Compute_shaders.h"

using namespace melonDS::ComputeRendererShaders;

namespace {

// Scale factor 1, as ComputeRenderer3D::SetRenderSettings computes them.
constexpr int kScreenWidth = 256, kScreenHeight = 192;
constexpr int kTileSize = 8;
constexpr int kCoarseTileCountY = 4;
constexpr int kCoarseTileArea = 8 * kCoarseTileCountY;
constexpr int kClearCoarseBinMaskLocalSize = 64;
constexpr int kMaxWorkTiles = (kScreenWidth / kTileSize) * (kScreenHeight / kTileSize) * 16;

void replace_once(std::string& s, const std::string& from, const std::string& to, const char* what) {
    size_t at = s.find(from);
    if (at == std::string::npos) {
        fprintf(stderr, "gen_compute_shaders: %s: pattern not found (upstream changed?)\n", what);
        exit(1);
    }
    s.replace(at, from.size(), to);
}

std::string assemble(const std::string& body, const std::vector<const char*>& defines) {
    std::string src = "#version 460\n";
    for (const char* d : defines)
        src += std::string("#define ") + d + "\n";
    src += "#define ScreenWidth " + std::to_string(kScreenWidth);
    src += "\n#define ScreenHeight " + std::to_string(kScreenHeight);
    src += "\n#define MaxWorkTiles " + std::to_string(kMaxWorkTiles);
    src += "\n#define TileSize " + std::to_string(kTileSize);
    src += "\nconst int CoarseTileCountY = " + std::to_string(kCoarseTileCountY) + ";";
    src += "\n#define CoarseTileArea " + std::to_string(kCoarseTileArea);
    src += "\n#define ClearCoarseBinMaskLocalSize " + std::to_string(kClearCoarseBinMaskLocalSize);
    src += Common;
    src += body;
    return src;
}

std::string adapt(std::string src) {
    if (src.find("uniform uint CurVariant;") != std::string::npos) {
        replace_once(src,
            "layout (location = 0) uniform uint CurVariant;\n"
            "layout (location = 1) uniform vec2 InvTextureSize;\n"
            "layout (location = 2) uniform int TexIsCapture;\n"
            "layout (location = 3) uniform float CaptureYOffset;\n",
            // Matches DekoRenderer3D::VariantUniform (std140).
            "layout (std140, binding = 1) uniform VariantUniform\n"
            "{\n"
            "    uint CurVariant;\n"
            "    int TexIsCapture;\n"
            "    float CaptureYOffset;\n"
            "    uint VariantPad;\n"
            "    vec2 InvTextureSize;\n"
            "};\n",
            "variant uniforms");
    }
    if (src.find("uimageBuffer SetupIndices;") != std::string::npos) {
        replace_once(src,
            "layout (binding = 0, rgba16ui) uniform readonly uimageBuffer SetupIndices;",
            "layout (std430, binding = 3) readonly buffer SetupIndicesBuffer\n"
            "{\n"
            "    uvec2 SetupIndicesPacked[];\n"
            "};",
            "SetupIndices declaration");
        replace_once(src,
            "uvec4 setup = imageLoad(SetupIndices, int(gl_GlobalInvocationID.x));",
            "uvec2 setupPacked = SetupIndicesPacked[gl_GlobalInvocationID.x];\n"
            "    uvec4 setup = uvec4(setupPacked.x & 0xFFFFU, setupPacked.x >> 16,\n"
            "                        setupPacked.y & 0xFFFFU, setupPacked.y >> 16);",
            "SetupIndices load");
    }
    if (src.find("uniform image2D FinalFB;") != std::string::npos) {
        replace_once(src,
            "layout (binding = 0, rgba8) writeonly uniform image2D FinalFB;",
            "layout (std430, binding = 1) writeonly buffer FinalOutputBuffer\n"
            "{\n"
            "    uint FinalOutput[];\n"
            "};",
            "FinalFB declaration");
        replace_once(src,
            "    vec4 result = vec4(color.x & 0x3FU, bitfieldExtract(color.x, 8, 8), bitfieldExtract(color.x, 16, 8), bitfieldExtract(color.x, 24, 8));\n"
            "    result /= vec4(63.0, 63.0, 63.0, 31.0);\n"
            "    imageStore(FinalFB, ivec2(gl_GlobalInvocationID.xy), result);",
            "    FinalOutput[gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * ScreenWidth] = color.x;",
            "FinalFB store");
    }
    return src;
}

struct Variant {
    const char* file;
    const std::string* body;
    std::vector<const char*> defines;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <out dir>\n", argv[0]);
        return 2;
    }
    const std::vector<Variant> variants = {
        {"InterpSpansZ", &InterpSpans, {"InterpSpans", "ZBuffer"}},
        {"InterpSpansW", &InterpSpans, {"InterpSpans", "WBuffer"}},
        {"BinCombined", &BinCombined, {"BinCombined"}},
        {"DepthBlendZ", &DepthBlend, {"DepthBlend", "ZBuffer"}},
        {"DepthBlendW", &DepthBlend, {"DepthBlend", "WBuffer"}},
        {"RasteriseNoTextureZ", &Rasterise, {"Rasterise", "ZBuffer", "NoTexture"}},
        {"RasteriseNoTextureW", &Rasterise, {"Rasterise", "WBuffer", "NoTexture"}},
        {"RasteriseNoTextureToonZ", &Rasterise, {"Rasterise", "ZBuffer", "NoTexture", "Toon"}},
        {"RasteriseNoTextureToonW", &Rasterise, {"Rasterise", "WBuffer", "NoTexture", "Toon"}},
        {"RasteriseNoTextureHighlightZ", &Rasterise, {"Rasterise", "ZBuffer", "NoTexture", "Highlight"}},
        {"RasteriseNoTextureHighlightW", &Rasterise, {"Rasterise", "WBuffer", "NoTexture", "Highlight"}},
        {"RasteriseUseTextureDecalZ", &Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Decal"}},
        {"RasteriseUseTextureDecalW", &Rasterise, {"Rasterise", "WBuffer", "UseTexture", "Decal"}},
        {"RasteriseUseTextureModulateZ", &Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Modulate"}},
        {"RasteriseUseTextureModulateW", &Rasterise, {"Rasterise", "WBuffer", "UseTexture", "Modulate"}},
        {"RasteriseUseTextureToonZ", &Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Toon"}},
        {"RasteriseUseTextureToonW", &Rasterise, {"Rasterise", "WBuffer", "UseTexture", "Toon"}},
        {"RasteriseUseTextureHighlightZ", &Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Highlight"}},
        {"RasteriseUseTextureHighlightW", &Rasterise, {"Rasterise", "WBuffer", "UseTexture", "Highlight"}},
        {"RasteriseShadowMaskZ", &Rasterise, {"Rasterise", "ZBuffer", "ShadowMask"}},
        {"RasteriseShadowMaskW", &Rasterise, {"Rasterise", "WBuffer", "ShadowMask"}},
        {"ClearCoarseBinMask", &ClearCoarseBinMask, {"ClearCoarseBinMask"}},
        {"ClearIndirectWorkCount", &ClearIndirectWorkCount, {"ClearIndirectWorkCount"}},
        {"CalculateWorkOffsets", &CalcOffsets, {"CalculateWorkOffsets"}},
        {"SortWork", &SortWork, {"SortWork"}},
        {"FinalPass0", &FinalPass, {"FinalPass"}},
        {"FinalPass1", &FinalPass, {"FinalPass", "EdgeMarking"}},
        {"FinalPass2", &FinalPass, {"FinalPass", "Fog"}},
        {"FinalPass3", &FinalPass, {"FinalPass", "EdgeMarking", "Fog"}},
        {"FinalPass4", &FinalPass, {"FinalPass", "AntiAliasing"}},
        {"FinalPass5", &FinalPass, {"FinalPass", "AntiAliasing", "EdgeMarking"}},
        {"FinalPass6", &FinalPass, {"FinalPass", "AntiAliasing", "Fog"}},
        {"FinalPass7", &FinalPass, {"FinalPass", "AntiAliasing", "EdgeMarking", "Fog"}},
    };

    for (const Variant& v : variants) {
        std::string path = std::string(argv[1]) + "/" + v.file + ".comp.glsl";
        std::string src = adapt(assemble(*v.body, v.defines));
        FILE* f = fopen(path.c_str(), "wb");
        if (!f || fwrite(src.data(), 1, src.size(), f) != src.size()) {
            fprintf(stderr, "gen_compute_shaders: can't write %s\n", path.c_str());
            return 1;
        }
        fclose(f);
    }
    return 0;
}
