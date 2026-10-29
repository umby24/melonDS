// deko3d port of upstream melonDS's compute-shader 3D renderer (GPU3D_Compute.cpp).
//
// The CPU side (polygon -> span setup, variant batching, texture cache) is upstream's,
// kept close to the original so upstream fixes can be carried over; the GL calls are
// replaced with deko3d commands on our own queue. Shaders are upstream's, assembled and
// compiled at build time (switch/tools/gen_compute_shaders.cpp), loaded from RomFS.
//
// Differences from upstream:
// - Native resolution only (ScaleFactor 1); the constants are fixed.
// - The final pass writes the packed RGB6A5 colour into a CPU-visible buffer, which
//   GetLine() hands to the software 2D engines (with X scroll, as SoftRenderer3D does).
// - Display captures used as textures are read from VRAM like any texture (the software
//   2D renderer writes captures to VRAM), so the capture-texture path is never taken.
// - Frames alternate between two slots (command memory, staging, output). The CPU's
//   per-frame data goes into the slot's uncached staging buffer and the GPU copies it into
//   cached buffers, so shaders never see stale copies in the GPU's L2. Frames run in
//   queue order behind a full barrier, so the GPU-side buffers are shared.
// - GetLine normally shows the frame started at the last RenderFrame (waiting for it, as
//   hardware timing implies). With `delay`, it shows the one before, which is long done:
//   the emu thread doesn't wait, and the 3D is one frame behind the 2D layers.

#include "gpu/deko_renderer3d.h"

#include <GPU.h>
#include <GPU3D.h>
#include <GPU3D_Texcache.h>

#include <borealis.hpp>
#include <borealis/platforms/switch/switch_video.hpp>
#include <deko3d.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gpu {

namespace {

using melonDS::s32;
using melonDS::u16;
using melonDS::u32;
using melonDS::u8;

// ComputeRenderer3D::SetRenderSettings at scale 1 (and the generator tool's values).
constexpr int kScreenWidth = 256;
constexpr int kScreenHeight = 192;
constexpr int kTileSize = 8;
constexpr int kCoarseTileCountX = 8;
constexpr int kCoarseTileCountY = 4;
constexpr int kCoarseTileW = kCoarseTileCountX * kTileSize;
constexpr int kCoarseTileH = kCoarseTileCountY * kTileSize;
constexpr int kClearCoarseBinMaskLocalSize = 64;
constexpr int kTilesPerLine = kScreenWidth / kTileSize;
constexpr int kTileLines = kScreenHeight / kTileSize;
constexpr int kMaxWorkTiles = kTilesPerLine * kTileLines * 16;
constexpr int kBinStride = 2048 / 32;
constexpr int kCoarseBinStride = kBinStride / 32;
constexpr int kMaxVariants = 256;
// Upstream sizes these by guesswork and only asserts (compiled out here), so a big scene
// would overrun them. Y spans: at most 2 + one per vertex (10) per polygon, so this
// can't run out; span indices (one per polygon line) can, and RenderFrame drops the
// polygons that don't fit instead.
constexpr int kMaxYSpanSetups = 2048 * 12;
constexpr int kMaxYSpanIndices = 96 * 2048;

// Image descriptors: the two clear bitmaps, then one per texture cache array.
constexpr u32 kImageClearColor = 0;
constexpr u32 kImageClearDepth = 1;
constexpr u32 kImageTexcacheStart = 2;
constexpr u32 kMaxTexArrays = 2048;
constexpr u32 kNumImageDescriptors = kImageTexcacheStart + kMaxTexArrays;
constexpr u32 kNumSamplers = 9;     // wrap S x wrap T: clamp, repeat, mirror
constexpr u32 kSamplerRepeat = 4;   // repeat x repeat (the clear bitmaps)

constexpr u32 kStagingSize = 16 << 20;    // per slot: texture uploads + polygon data
constexpr int kSlots = 2;
constexpr u32 kCmdChunkSize = 256 << 10;  // command memory, grown on demand

// Upstream's ComputeRenderer3D structs; the shaders depend on these layouts.
struct SpanSetupY {
    s32 Z0, Z1, W0, W1;
    s32 ColorR0, ColorG0, ColorB0;
    s32 ColorR1, ColorG1, ColorB1;
    s32 TexcoordU0, TexcoordV0;
    s32 TexcoordU1, TexcoordV1;
    s32 I0, I1;
    s32 Linear;
    s32 IRecip;
    s32 W0n, W0d, W1d;
    s32 Increment;
    s32 X0, X1, Y0, Y1;
    s32 XMin, XMax;
    s32 DxInitial;
    s32 XCovIncr;
    u32 IsDummy;
};
struct SpanSetupX {
    s32 X0, X1;
    s32 EdgeLenL, EdgeLenR, EdgeCovL, EdgeCovR;
    s32 XRecip;
    u32 Flags;
    s32 Z0, Z1, W0, W1;
    s32 ColorR0, ColorG0, ColorB0;
    s32 ColorR1, ColorG1, ColorB1;
    s32 TexcoordU0, TexcoordV0;
    s32 TexcoordU1, TexcoordV1;
    s32 CovLInitial, CovRInitial;
};
struct SetupIndices {
    u16 PolyIdx, SpanIdxL, SpanIdxR, Y;
};
struct RenderPolygon {
    u32 FirstXSpan;
    s32 YTop, YBot;
    s32 XMin, XMax;
    s32 XMinY, XMaxY;
    u32 Variant;
    u32 Attr;
    float TextureLayer;
};
struct BinResultHeader {
    u32 VariantWorkCount[kMaxVariants * 4];
    u32 SortedWorkOffset[kMaxVariants];
    u32 SortWorkWorkCount[4];
};
struct MetaUniform {
    u32 NumPolygons;
    u32 NumVariants;
    u32 AlphaRef;
    u32 DispCnt;
    u32 ToonTable[4 * 34];
    u32 ClearColor, ClearDepth, ClearAttr;
    u32 FogOffset, FogShift, FogColor;
    float ClearBitmapOffset[2];
};
// The generator tool's replacement for upstream's loose Rasterise uniforms (std140).
struct VariantUniform {
    u32 CurVariant;
    s32 TexIsCapture;
    float CaptureYOffset;
    u32 Pad;
    float InvTextureSize[2];
};

// Shader files in romfs:/shaders/compute3d/, as named by the generator tool.
enum ShaderId {
    kInterpSpansZ, kInterpSpansW,
    kBinCombined,
    kDepthBlendZ, kDepthBlendW,
    kRasteriseNoTextureZ, kRasteriseNoTextureW,
    kRasteriseNoTextureToonZ, kRasteriseNoTextureToonW,
    kRasteriseNoTextureHighlightZ, kRasteriseNoTextureHighlightW,
    kRasteriseUseTextureDecalZ, kRasteriseUseTextureDecalW,
    kRasteriseUseTextureModulateZ, kRasteriseUseTextureModulateW,
    kRasteriseUseTextureToonZ, kRasteriseUseTextureToonW,
    kRasteriseUseTextureHighlightZ, kRasteriseUseTextureHighlightW,
    kRasteriseShadowMaskZ, kRasteriseShadowMaskW,
    kClearCoarseBinMask,
    kClearIndirectWorkCount,
    kCalculateWorkOffsets,
    kSortWork,
    kFinalPass0,  // + bit 0 edge marking, bit 1 fog, bit 2 anti-aliasing
    kNumShaders = kFinalPass0 + 8,
};
const char* const kShaderFiles[kNumShaders] = {
    "InterpSpansZ", "InterpSpansW",
    "BinCombined",
    "DepthBlendZ", "DepthBlendW",
    "RasteriseNoTextureZ", "RasteriseNoTextureW",
    "RasteriseNoTextureToonZ", "RasteriseNoTextureToonW",
    "RasteriseNoTextureHighlightZ", "RasteriseNoTextureHighlightW",
    "RasteriseUseTextureDecalZ", "RasteriseUseTextureDecalW",
    "RasteriseUseTextureModulateZ", "RasteriseUseTextureModulateW",
    "RasteriseUseTextureToonZ", "RasteriseUseTextureToonW",
    "RasteriseUseTextureHighlightZ", "RasteriseUseTextureHighlightW",
    "RasteriseShadowMaskZ", "RasteriseShadowMaskW",
    "ClearCoarseBinMask",
    "ClearIndirectWorkCount",
    "CalculateWorkOffsets",
    "SortWork",
    "FinalPass0", "FinalPass1", "FinalPass2", "FinalPass3",
    "FinalPass4", "FinalPass5", "FinalPass6", "FinalPass7",
};
constexpr const char* kShaderDir = "romfs:/shaders/compute3d/";

struct DkshHeader {
    uint32_t magic;
    uint32_t header_sz;
    uint32_t control_sz;
    uint32_t code_sz;
    uint32_t programs_off;
    uint32_t num_programs;
};

uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

brls::SwitchVideoContext* video() {
    auto* platform = brls::Application::getPlatform();
    return platform ? static_cast<brls::SwitchVideoContext*>(platform->getVideoContext()) : nullptr;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> data;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return data;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0) {
        data.resize(size_t(size));
        if (fread(data.data(), 1, data.size(), f) != data.size())
            data.clear();
    }
    fclose(f);
    return data;
}

// One GPU buffer in its own memory block.
struct Buffer {
    dk::UniqueMemBlock mem;
    uint32_t size = 0;

    bool create(dk::Device device, uint32_t bytes, uint32_t flags) {
        size = bytes;
        mem = dk::MemBlockMaker{device, align_up(bytes, DK_MEMBLOCK_ALIGNMENT)}
                  .setFlags(flags | DkMemBlockFlags_ZeroFillInit)
                  .create();
        return bool(mem);
    }
    DkGpuAddr gpu() { return mem.getGpuAddr(); }
    void* cpu() { return mem.getCpuAddr(); }
    DkBufExtents extents() { return {mem.getGpuAddr(), size}; }
};

constexpr uint32_t kCpuWritten = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
// Staging (the CPU's per-frame data, copied by the GPU into cached buffers): uncached on
// the GPU too, so the copies never read last frame's contents from its L2.
constexpr uint32_t kCpuInput = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached;
constexpr uint32_t kGpuOnly = DkMemBlockFlags_GpuCached;
// GPU writes bypass its L2, so nothing is left for a cache flush to miss; the CPU side
// is invalidated after each frame's fence (wait_slot).
constexpr uint32_t kCpuRead = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuUncached;
constexpr uint32_t kImageMem = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;

class DekoRenderer3D;

// Texcache's loader interface; handles are image descriptor indices (never 0, which
// upstream's variant code reads as "untextured").
struct TexLoader {
    DekoRenderer3D* r;
    u32 GenerateTexture(u32 width, u32 height, u32 layers);
    void UploadTexture(u32 handle, u32 width, u32 height, u32 layer, void* data);
    void DeleteTexture(u32 handle);
};
using Texcache = melonDS::Texcache<TexLoader, u32>;

struct Variant {
    u32 Texture, Sampler;
    u16 Width, Height;
    u8 BlendMode;
    int CaptureYOffset;

    bool operator==(const Variant& other) const {
        return Texture == other.Texture && Sampler == other.Sampler && BlendMode == other.BlendMode &&
               CaptureYOffset == other.CaptureYOffset;
    }
};

class DekoRenderer3D final : public melonDS::Renderer3D {
  public:
    DekoRenderer3D(melonDS::GPU3D& gpu3d, dk::Device device, bool delay)
        : Renderer3D(gpu3d), device_(device), delay_(delay), texcache_(gpu3d.GPU, TexLoader{this}) {}
    ~DekoRenderer3D() override;

    bool Init() override;
    void Reset() override;
    void RenderFrame() override;
    void RestartFrame() override {}
    u32* GetLine(int line) override;

    // Texture cache loader.
    u32 create_tex_array(u32 width, u32 height, u32 layers);
    void upload_tex_layer(u32 desc, u32 width, u32 height, u32 layer, const void* data);
    void delete_tex_array(u32 desc);

  private:
    struct TexArray {
        dk::UniqueMemBlock mem;
        dk::Image image;
    };

    // One frame's GPU work and the memory it uses until its fence.
    struct Slot {
        std::vector<dk::UniqueMemBlock> cmd_chunks;
        size_t cmd_chunk_next = 0;
        Buffer staging;
        uint32_t staging_used = 0;
        Buffer output;      // 256x192 packed RGB6A5, read by GetLine
        Buffer timestamps;  // GPU timestamps at the start (+0) and end (+16) of the frame
        dk::Fence done = {};
        bool in_flight = false;
        bool timed = false;  // the work in flight is a whole frame with timestamps
        uint64_t submit_ts = 0;
    };

    bool load_shaders();
    void wait_slot(Slot& slot);
    void timed_wait(Slot& slot);
    void begin_commands();
    void submit(bool timed);
    DkGpuAddr stage(const void* data, uint32_t size);
    void upload(Buffer& dst, const void* data, uint32_t size);
    static void add_cmd_memory(void* user, DkCmdBuf cmdbuf, size_t min_size);

    void SetupAttrs(SpanSetupY* span, melonDS::Polygon* poly, int from, int to);
    void SetupYSpan(RenderPolygon* rp, SpanSetupY* span, melonDS::Polygon* poly, int from, int to, int side,
                    s32 positions[10][2]);
    void SetupYSpanDummy(RenderPolygon* rp, SpanSetupY* span, melonDS::Polygon* poly, int vertex, int side,
                         s32 positions[10][2]);

    dk::Device device_;
    bool delay_;
    dk::UniqueQueue queue_;

    Slot slots_[kSlots];
    int cur_ = 0;       // slot being recorded
    int latest_ = -1;   // slot of the last frame submitted
    int display_ = -1;  // slot GetLine reads (-1: blank)
    dk::UniqueCmdBuf cmd_;

    dk::UniqueMemBlock code_mem_;
    dk::Shader shaders_[kNumShaders];

    Buffer polygons_, xspans_, yspans_, setup_indices_;
    Buffer tiles_[3];  // color, depth, attr
    Buffer final_tiles_, bin_result_, work_desc_;
    Buffer meta_ubo_, variant_ubo_;
    Buffer image_descs_, sampler_descs_;

    Buffer clear_mem_[2];
    dk::Image clear_images_[2];
    u32 clear_bitmap_[256 * 256];
    u8 clear_bitmap_dirty_ = 0x3;

    TexArray tex_arrays_[kMaxTexArrays];
    std::vector<u32> free_tex_arrays_;
    bool tex_overflow_ = false;

    Texcache texcache_;

    std::vector<SetupIndices> yspan_indices_;
    SpanSetupY yspan_setups_[kMaxYSpanSetups];
    RenderPolygon render_polygons_[2048];

    u32 scrolled_line_[256];
    u32 blank_line_[256] = {};

    // Timing, logged every kStatsFrames frames (see log_stats).
    struct Stats {
        uint64_t setup_ns = 0, wait_ns = 0, max_setup_ns = 0, max_wait_ns = 0;
        uint64_t queued_ns = 0, busy_ns = 0, max_busy_ns = 0;  // from GPU timestamps
        unsigned timed = 0;
        unsigned frames = 0, rendered = 0, polygons = 0, dropped = 0;
    } stats_;
    void log_stats();
};

u32 TexLoader::GenerateTexture(u32 width, u32 height, u32 layers) {
    return r->create_tex_array(width, height, layers);
}
void TexLoader::UploadTexture(u32 handle, u32 width, u32 height, u32 layer, void* data) {
    r->upload_tex_layer(handle, width, height, layer, data);
}
void TexLoader::DeleteTexture(u32 handle) {
    r->delete_tex_array(handle);
}

DekoRenderer3D::~DekoRenderer3D() {
    if (queue_)
        queue_.waitIdle();
    texcache_.Reset();
}

bool DekoRenderer3D::load_shaders() {
    std::vector<uint8_t> files[kNumShaders];
    uint32_t code_size = 0;
    for (int i = 0; i < kNumShaders; ++i) {
        files[i] = read_file(std::string(kShaderDir) + kShaderFiles[i] + ".dksh");
        if (files[i].size() < sizeof(DkshHeader)) {
            fprintf(stderr, "[gpu] missing shader %s%s.dksh\n", kShaderDir, kShaderFiles[i]);
            return false;
        }
        const auto* h = reinterpret_cast<const DkshHeader*>(files[i].data());
        if (h->control_sz + h->code_sz > files[i].size()) {
            fprintf(stderr, "[gpu] bad shader file %s.dksh\n", kShaderFiles[i]);
            return false;
        }
        code_size = align_up(code_size, DK_SHADER_CODE_ALIGNMENT) + h->code_sz;
    }
    code_mem_ = dk::MemBlockMaker{device_, align_up(code_size + DK_SHADER_CODE_UNUSABLE_SIZE, DK_MEMBLOCK_ALIGNMENT)}
                    .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code)
                    .create();
    if (!code_mem_)
        return false;
    uint32_t offset = 0;
    for (int i = 0; i < kNumShaders; ++i) {
        const auto* h = reinterpret_cast<const DkshHeader*>(files[i].data());
        offset = align_up(offset, DK_SHADER_CODE_ALIGNMENT);
        memcpy(static_cast<uint8_t*>(code_mem_.getCpuAddr()) + offset, files[i].data() + h->control_sz, h->code_sz);
        dk::ShaderMaker{code_mem_, offset}.setControl(files[i].data()).setProgramId(0).initialize(shaders_[i]);
        offset += h->code_sz;
    }
    return true;
}

bool DekoRenderer3D::Init() {
    queue_ = dk::QueueMaker{device_}
                 .setFlags(DkQueueFlags_HighPrio | DkQueueFlags_Graphics | DkQueueFlags_Compute |
                           DkQueueFlags_DisableZcull)
                 .create();
    cmd_ = dk::CmdBufMaker{device_}.setUserData(this).setCbAddMem(add_cmd_memory).create();
    if (!queue_ || !cmd_ || !load_shaders())
        return false;

    const int bin_result_size = sizeof(BinResultHeader) +
                                kTilesPerLine * kTileLines * kCoarseBinStride * 4 +  // BinnedMaskCoarse
                                kTilesPerLine * kTileLines * kBinStride * 4 +        // BinnedMask
                                kTilesPerLine * kTileLines * kBinStride * 4;         // WorkOffsets
    bool ok = polygons_.create(device_, sizeof(RenderPolygon) * 2048, kGpuOnly) &&
              yspans_.create(device_, sizeof(SpanSetupY) * kMaxYSpanSetups, kGpuOnly) &&
              setup_indices_.create(device_, sizeof(SetupIndices) * kMaxYSpanIndices, kGpuOnly) &&
              xspans_.create(device_, sizeof(SpanSetupX) * kMaxYSpanIndices, kGpuOnly) &&
              final_tiles_.create(device_, 4 * 3 * 2 * kScreenWidth * kScreenHeight, kGpuOnly) &&
              bin_result_.create(device_, bin_result_size, kGpuOnly) &&
              work_desc_.create(device_, kMaxWorkTiles * 2 * 4 * 2, kGpuOnly) &&
              meta_ubo_.create(device_, align_up(sizeof(MetaUniform), DK_UNIFORM_BUF_ALIGNMENT), kCpuWritten) &&
              variant_ubo_.create(device_, align_up(sizeof(VariantUniform), DK_UNIFORM_BUF_ALIGNMENT), kCpuWritten) &&
              image_descs_.create(device_, sizeof(DkImageDescriptor) * kNumImageDescriptors, kCpuWritten) &&
              sampler_descs_.create(device_, sizeof(DkSamplerDescriptor) * kNumSamplers, kCpuWritten);
    for (Slot& slot : slots_)
        ok = ok && slot.staging.create(device_, kStagingSize, kCpuInput) &&
             slot.output.create(device_, 4 * kScreenWidth * kScreenHeight, kCpuRead) &&
             slot.timestamps.create(device_, 64, kCpuRead);
    for (auto& t : tiles_)
        ok = ok && t.create(device_, 4 * kTileSize * kTileSize * kMaxWorkTiles, kGpuOnly);
    if (!ok) {
        fprintf(stderr, "[gpu] out of GPU memory for the 3D renderer\n");
        return false;
    }

    auto* images = static_cast<dk::ImageDescriptor*>(image_descs_.cpu());
    for (int i = 0; i < 2; ++i) {
        dk::ImageLayout layout;
        dk::ImageLayoutMaker{device_}.setFormat(DkImageFormat_R32_Uint).setDimensions(256, 256).initialize(layout);
        if (!clear_mem_[i].create(device_, layout.getSize(), kImageMem))
            return false;
        clear_images_[i].initialize(layout, clear_mem_[i].mem, 0);
        images[kImageClearColor + i].initialize(dk::ImageView{clear_images_[i]});
    }

    auto* samplers = static_cast<dk::SamplerDescriptor*>(sampler_descs_.cpu());
    const DkWrapMode wrap[3] = {DkWrapMode_ClampToEdge, DkWrapMode_Repeat, DkWrapMode_MirroredRepeat};
    for (u32 j = 0; j < 3; ++j)
        for (u32 i = 0; i < 3; ++i)
            samplers[i + j * 3].initialize(
                dk::Sampler{}.setFilter(DkFilter_Nearest, DkFilter_Nearest).setWrapMode(wrap[i], wrap[j]));

    free_tex_arrays_.clear();
    for (u32 i = kMaxTexArrays; i-- > 0;)
        free_tex_arrays_.push_back(kImageTexcacheStart + i);

    yspan_indices_.resize(kMaxYSpanIndices);
    fprintf(stderr, "[gpu] deko3d 3D renderer ready\n");
    return true;
}

void DekoRenderer3D::Reset() {
    for (Slot& slot : slots_)
        wait_slot(slot);
    if (queue_)
        queue_.waitIdle();
    texcache_.Reset();
    tex_overflow_ = false;
    clear_bitmap_dirty_ = 0x3;
    latest_ = display_ = -1;
}

void DekoRenderer3D::add_cmd_memory(void* user, DkCmdBuf cmdbuf, size_t min_size) {
    auto* self = static_cast<DekoRenderer3D*>(user);
    Slot& slot = self->slots_[self->cur_];
    if (slot.cmd_chunk_next == slot.cmd_chunks.size() || slot.cmd_chunks[slot.cmd_chunk_next].getSize() < min_size) {
        uint32_t size = align_up(std::max<uint32_t>(kCmdChunkSize, uint32_t(min_size)), DK_MEMBLOCK_ALIGNMENT);
        slot.cmd_chunks.insert(slot.cmd_chunks.begin() + slot.cmd_chunk_next,
                               dk::MemBlockMaker{self->device_, size}.setFlags(kCpuWritten).create());
    }
    dk::MemBlock chunk = slot.cmd_chunks[slot.cmd_chunk_next++];
    dkCmdBufAddMemory(cmdbuf, chunk, 0, chunk.getSize());
}

uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

constexpr unsigned kStatsFrames = 600;

void DekoRenderer3D::log_stats() {
    Stats& st = stats_;
    if (++st.frames < kStatsFrames)
        return;
    fprintf(stderr,
            "[gpu] 3D over %u frames (%u rendered): CPU setup avg %.2f ms max %.2f, "
            "waiting for the GPU avg %.2f ms max %.2f; per rendered frame the GPU queued it "
            "%.2f ms then worked %.2f ms (max %.2f); avg %u polygons%s\n",
            st.frames, st.rendered, st.rendered ? st.setup_ns / 1e6 / st.rendered : 0.0, st.max_setup_ns / 1e6,
            st.wait_ns / 1e6 / st.frames, st.max_wait_ns / 1e6, st.timed ? st.queued_ns / 1e6 / st.timed : 0.0,
            st.timed ? st.busy_ns / 1e6 / st.timed : 0.0, st.max_busy_ns / 1e6,
            st.rendered ? st.polygons / st.rendered : 0,
            st.dropped ? ", some polygons dropped (span limit)" : "");
    st = {};
}

void DekoRenderer3D::wait_slot(Slot& slot) {
    if (slot.in_flight) {
        slot.done.wait();
        slot.in_flight = false;
        slot.output.mem.flushCpuCache(0, slot.output.size);
        if (slot.timed) {
            slot.timed = false;
            slot.timestamps.mem.flushCpuCache(0, slot.timestamps.size);
            // A counter report is {value, timestamp}; the timestamp is the second word.
            const auto* ts = static_cast<const uint64_t*>(slot.timestamps.cpu());
            uint64_t start = ts[1], end = ts[3];
            if (end >= start && start >= slot.submit_ts) {
                uint64_t busy = dkTimestampToNs(end - start);
                stats_.queued_ns += dkTimestampToNs(start - slot.submit_ts);
                stats_.busy_ns += busy;
                stats_.max_busy_ns = std::max(stats_.max_busy_ns, busy);
                stats_.timed++;
            }
        }
    }
}

// wait_slot, counted as time the emu thread spent waiting for the GPU.
void DekoRenderer3D::timed_wait(Slot& slot) {
    if (!slot.in_flight)
        return;
    const uint64_t start = now_ns();
    wait_slot(slot);
    const uint64_t waited = now_ns() - start;
    stats_.wait_ns += waited;
    stats_.max_wait_ns = std::max(stats_.max_wait_ns, waited);
}

// Starts recording into slots_[cur_], which must not be in flight. The full barrier
// orders this frame after everything queued before it (the GPU-side buffers are shared).
void DekoRenderer3D::begin_commands() {
    Slot& slot = slots_[cur_];
    cmd_.clear();
    slot.cmd_chunk_next = 0;
    slot.staging_used = 0;
    cmd_.barrier(DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Shader |
                                     DkInvalidateFlags_Descriptors | DkInvalidateFlags_L2Cache);
    cmd_.reportCounter(DkCounter_Timestamp, slot.timestamps.gpu());
}

void DekoRenderer3D::submit(bool timed) {
    Slot& slot = slots_[cur_];
    cmd_.signalFence(slot.done, true);  // flush: GetLine reads the output from the CPU
    queue_.submitCommands(cmd_.finishList());
    slot.submit_ts = dkDeviceGetCurrentTimestamp(device_);
    queue_.flush();
    slot.in_flight = true;
    slot.timed = timed;
}

DkGpuAddr DekoRenderer3D::stage(const void* data, uint32_t size) {
    Slot& slot = slots_[cur_];
    uint32_t offset = align_up(slot.staging_used, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
    if (offset + size > slot.staging.size) {
        // A frame with more new data than fits: run what we have and start over.
        submit(false);
        wait_slot(slot);
        begin_commands();
        offset = 0;
    }
    memcpy(static_cast<uint8_t*>(slot.staging.cpu()) + offset, data, size);
    slot.staging_used = offset + size;
    return slot.staging.gpu() + offset;
}

// Copies CPU data into a GPU-cached buffer, in command order.
void DekoRenderer3D::upload(Buffer& dst, const void* data, uint32_t size) {
    if (size)
        cmd_.copyBuffer(stage(data, size), dst.gpu(), size);
}

u32 DekoRenderer3D::create_tex_array(u32 width, u32 height, u32 layers) {
    if (free_tex_arrays_.empty()) {
        // Textures of this frame render untextured; the cache is emptied next frame.
        tex_overflow_ = true;
        return 0;
    }
    u32 desc = free_tex_arrays_.back();
    TexArray& t = tex_arrays_[desc - kImageTexcacheStart];
    dk::ImageLayout layout;
    dk::ImageLayoutMaker{device_}
        .setType(DkImageType_2DArray)
        .setFormat(DkImageFormat_RGBA8_Uint)
        .setDimensions(width, height, layers)
        .initialize(layout);
    t.mem = dk::MemBlockMaker{device_, align_up(uint32_t(layout.getSize()), DK_MEMBLOCK_ALIGNMENT)}
                .setFlags(kImageMem)
                .create();
    if (!t.mem) {
        tex_overflow_ = true;
        return 0;
    }
    free_tex_arrays_.pop_back();
    t.image.initialize(layout, t.mem, 0);
    static_cast<dk::ImageDescriptor*>(image_descs_.cpu())[desc].initialize(dk::ImageView{t.image});
    return desc;
}

void DekoRenderer3D::upload_tex_layer(u32 desc, u32 width, u32 height, u32 layer, const void* data) {
    if (desc < kImageTexcacheStart)
        return;
    DkGpuAddr src = stage(data, width * height * 4);
    cmd_.copyBufferToImage({src, 0, 0}, dk::ImageView{tex_arrays_[desc - kImageTexcacheStart].image},
                           {0, 0, layer, width, height, 1});
}

// Called with the GPU idle (Reset, or the destructor).
void DekoRenderer3D::delete_tex_array(u32 desc) {
    if (desc < kImageTexcacheStart)
        return;
    tex_arrays_[desc - kImageTexcacheStart].mem = {};
    free_tex_arrays_.push_back(desc);
}

// --- Upstream's span setup (GPU3D_Compute.cpp), unchanged apart from names. ---

void DekoRenderer3D::SetupAttrs(SpanSetupY* span, melonDS::Polygon* poly, int from, int to) {
    span->Z0 = poly->FinalZ[from];
    span->W0 = poly->FinalW[from];
    span->Z1 = poly->FinalZ[to];
    span->W1 = poly->FinalW[to];
    span->ColorR0 = poly->Vertices[from]->FinalColor[0];
    span->ColorG0 = poly->Vertices[from]->FinalColor[1];
    span->ColorB0 = poly->Vertices[from]->FinalColor[2];
    span->ColorR1 = poly->Vertices[to]->FinalColor[0];
    span->ColorG1 = poly->Vertices[to]->FinalColor[1];
    span->ColorB1 = poly->Vertices[to]->FinalColor[2];
    span->TexcoordU0 = poly->Vertices[from]->TexCoords[0];
    span->TexcoordV0 = poly->Vertices[from]->TexCoords[1];
    span->TexcoordU1 = poly->Vertices[to]->TexCoords[0];
    span->TexcoordV1 = poly->Vertices[to]->TexCoords[1];
}

void DekoRenderer3D::SetupYSpanDummy(RenderPolygon* rp, SpanSetupY* span, melonDS::Polygon* poly, int vertex,
                                     int side, s32 positions[10][2]) {
    s32 x0 = positions[vertex][0];
    if (side) {
        span->DxInitial = -0x40000;
        x0--;
    } else {
        span->DxInitial = 0;
    }

    span->X0 = span->X1 = x0;
    span->XMin = x0;
    span->XMax = x0;
    span->Y0 = span->Y1 = positions[vertex][1];

    if (span->XMin < rp->XMin) {
        rp->XMin = span->XMin;
        rp->XMinY = span->Y0;
    }
    if (span->XMax > rp->XMax) {
        rp->XMax = span->XMax;
        rp->XMaxY = span->Y0;
    }

    span->Increment = 0;
    span->I0 = span->I1 = span->IRecip = 0;
    span->Linear = true;
    span->XCovIncr = 0;
    span->IsDummy = true;

    SetupAttrs(span, poly, vertex, vertex);
}

void DekoRenderer3D::SetupYSpan(RenderPolygon* rp, SpanSetupY* span, melonDS::Polygon* poly, int from, int to,
                                int side, s32 positions[10][2]) {
    span->X0 = positions[from][0];
    span->X1 = positions[to][0];
    span->Y0 = positions[from][1];
    span->Y1 = positions[to][1];

    SetupAttrs(span, poly, from, to);

    s32 minXY, maxXY;
    bool negative = false;
    if (span->X1 > span->X0) {
        span->XMin = span->X0;
        span->XMax = span->X1 - 1;
        minXY = span->Y0;
        maxXY = span->Y1;
    } else if (span->X1 < span->X0) {
        span->XMin = span->X1;
        span->XMax = span->X0 - 1;
        negative = true;
        minXY = span->Y1;
        maxXY = span->Y0;
    } else {
        span->XMin = span->X0;
        if (side)
            span->XMin--;
        span->XMax = span->XMin;
        // doesn't matter for completely vertical slope
        minXY = span->Y0;
        maxXY = span->Y0;
    }

    if (span->XMin < rp->XMin) {
        rp->XMin = span->XMin;
        rp->XMinY = minXY;
    }
    if (span->XMax > rp->XMax) {
        rp->XMax = span->XMax;
        rp->XMaxY = maxXY;
    }

    span->IsDummy = false;

    s32 xlen = span->XMax + 1 - span->XMin;
    s32 ylen = span->Y1 - span->Y0;

    // slope increment has a 18-bit fractional part
    // note: for some reason, x/y isn't calculated directly,
    // instead, 1/y is calculated and then multiplied by x
    if (ylen == 0) {
        span->Increment = 0;
    } else if (ylen == xlen) {
        span->Increment = 0x40000;
    } else {
        s32 yrecip = (1 << 18) / ylen;
        span->Increment = (span->X1 - span->X0) * yrecip;
        if (span->Increment < 0)
            span->Increment = -span->Increment;
    }

    bool xMajor = (span->Increment > 0x40000);

    if (side) {
        // right
        if (xMajor)
            span->DxInitial = negative ? (0x20000 + 0x40000) : (span->Increment - 0x20000);
        else if (span->Increment != 0)
            span->DxInitial = negative ? 0x40000 : 0;
        else
            span->DxInitial = -0x40000;
    } else {
        // left
        if (xMajor)
            span->DxInitial = negative ? ((span->Increment - 0x20000) + 0x40000) : 0x20000;
        else if (span->Increment != 0)
            span->DxInitial = negative ? 0x40000 : 0;
        else
            span->DxInitial = 0;
    }

    if (xMajor) {
        if (side) {
            span->I0 = span->X0 - 1;
            span->I1 = span->X1 - 1;
        } else {
            span->I0 = span->X0;
            span->I1 = span->X1;
        }
        // used for calculating AA coverage
        span->XCovIncr = (ylen << 10) / xlen;
    } else {
        span->I0 = span->Y0;
        span->I1 = span->Y1;
    }

    if (span->I0 != span->I1)
        span->IRecip = (1 << 30) / (span->I1 - span->I0);
    else
        span->IRecip = 0;

    span->Linear = (span->W0 == span->W1) && !(span->W0 & 0x7E) && !(span->W1 & 0x7E);

    if ((span->W0 & 0x1) && !(span->W1 & 0x1)) {
        span->W0n = (span->W0 - 1) >> 1;
        span->W0d = (span->W0 + 1) >> 1;
        span->W1d = span->W1 >> 1;
    } else {
        span->W0n = span->W0 >> 1;
        span->W0d = span->W0 >> 1;
        span->W1d = span->W1 >> 1;
    }
}

// DS BGR555 -> the renderer's 6-bit components (r | g << 8 | b << 16), as upstream does.
u32 to_rgb6(u32 color) {
    u32 r = (color << 1) & 0x3E; if (r) r++;
    u32 g = (color >> 4) & 0x3E; if (g) g++;
    u32 b = (color >> 9) & 0x3E; if (b) b++;
    return r | (g << 8) | (b << 16);
}

void DekoRenderer3D::RenderFrame() {
    log_stats();
    if (delay_)
        display_ = latest_;  // what was submitted last time is shown from now on
    cur_ = display_ == 0 ? 1 : 0;  // never the slot on display
    timed_wait(slots_[cur_]);
    const uint64_t setup_start = now_ns();
    if (tex_overflow_) {
        queue_.waitIdle();
        texcache_.Reset();
        tex_overflow_ = false;
    }
    begin_commands();

    u8 clrBitmapDirty;
    if (!texcache_.Update(clrBitmapDirty) && GPU3D.RenderFrameIdentical)
        return;  // the previous output stands; nothing recorded is submitted

    // if we're using a clear bitmap, set that up
    clear_bitmap_dirty_ |= clrBitmapDirty;
    if (GPU3D.RenderDispCnt & (1 << 14)) {
        for (int k = 0; k < 2; ++k) {
            if (!(clear_bitmap_dirty_ & (1 << k)))
                continue;
            const u16* vram = reinterpret_cast<const u16*>(&GPU.VRAMFlat_Texture[k ? 0x60000 : 0x40000]);
            for (int i = 0; i < 256 * 256; i++) {
                u16 val = vram[i];
                if (k == 0) {
                    u32 a = (val & 0x8000) ? 31 : 0;
                    clear_bitmap_[i] = to_rgb6(val) | (a << 24);
                } else {
                    u32 depth = ((val & 0x7FFF) * 0x200) + 0x1FF;
                    u32 fog = (val & 0x8000) << 9;
                    clear_bitmap_[i] = depth | fog;
                }
            }
            DkGpuAddr src = stage(clear_bitmap_, sizeof(clear_bitmap_));
            cmd_.copyBufferToImage({src, 0, 0}, dk::ImageView{clear_images_[k]}, {0, 0, 0, 256, 256, 1});
        }
        clear_bitmap_dirty_ = 0;
    }

    int numYSpans = 0;
    int numSetupIndices = 0;

    // Upstream batches polygons into variants (same texture, sampler and blend mode);
    // array textures of the same size keep the variant count down.
    u32 numVariants = 0, prevVariant = 0, prevTexLayer = 0;
    Variant variants[kMaxVariants];

    bool enableTextureMaps = GPU3D.RenderDispCnt & (1 << 0);

    u32 numPolygons = GPU3D.RenderNumPolygons;
    for (u32 i = 0; i < numPolygons; i++) {
        melonDS::Polygon* polygon = GPU3D.RenderPolygonRAM[i];

        // Not in upstream: stop at the first polygon that might not fit (one span index
        // per line, a new variant).
        if (numSetupIndices + kScreenHeight + 1 > kMaxYSpanIndices || numVariants >= u32(kMaxVariants)) {
            if (!stats_.dropped)
                fprintf(stderr, "[gpu] 3D: drawing %u of %u polygons (renderer limits)\n", i, numPolygons);
            stats_.dropped += numPolygons - i;
            numPolygons = i;
            break;
        }

        u32 nverts = polygon->NumVertices;
        u32 vtop = polygon->VTop, vbot = polygon->VBottom;

        u32 curVL = vtop, curVR = vtop;
        u32 nextVL, nextVR;

        render_polygons_[i].FirstXSpan = numSetupIndices;
        render_polygons_[i].Attr = polygon->Attr;

        bool foundVariant = false;
        if (i > 0) {
            // if the whole texture attribute matches
            // the texture layer will also match
            melonDS::Polygon* prevPolygon = GPU3D.RenderPolygonRAM[i - 1];
            foundVariant = prevPolygon->TexParam == polygon->TexParam &&
                           prevPolygon->TexPalette == polygon->TexPalette &&
                           (prevPolygon->Attr & 0x30) == (polygon->Attr & 0x30) &&
                           prevPolygon->IsShadowMask == polygon->IsShadowMask;
        }

        if (!foundVariant) {
            Variant variant;
            variant.BlendMode = polygon->IsShadowMask ? 4 : ((polygon->Attr >> 4) & 0x3);
            variant.Texture = 0;
            variant.Sampler = 0;
            variant.CaptureYOffset = -1;
            u32* textureLastVariant = nullptr;
            // we always need to look up the texture to get the layer of the array texture
            u32 textype = (polygon->TexParam >> 26) & 0x7;
            if (enableTextureMaps && textype) {
                texcache_.GetTexture(polygon->TexParam, polygon->TexPalette, variant.Texture, prevTexLayer,
                                     textureLastVariant);

                bool wrapS = (polygon->TexParam >> 16) & 1;
                bool wrapT = (polygon->TexParam >> 17) & 1;
                bool mirrorS = (polygon->TexParam >> 18) & 1;
                bool mirrorT = (polygon->TexParam >> 19) & 1;
                variant.Sampler = (wrapS ? (mirrorS ? 2 : 1) : 0) + (wrapT ? (mirrorT ? 2 : 1) : 0) * 3;

                if (*textureLastVariant < numVariants && variants[*textureLastVariant] == variant) {
                    foundVariant = true;
                    prevVariant = *textureLastVariant;
                }
            }

            if (!foundVariant) {
                for (int j = int(numVariants) - 1; j >= 0; j--) {
                    if (variants[j] == variant) {
                        foundVariant = true;
                        prevVariant = j;
                        break;
                    }
                }
                if (!foundVariant) {
                    prevVariant = numVariants;
                    variants[numVariants] = variant;
                    variants[numVariants].Width = melonDS::TextureWidth(polygon->TexParam);
                    variants[numVariants].Height = melonDS::TextureHeight(polygon->TexParam);
                    numVariants++;
                    assert(numVariants <= kMaxVariants);
                }

                if (textureLastVariant)
                    *textureLastVariant = prevVariant;
            }
        }
        render_polygons_[i].Variant = prevVariant;
        render_polygons_[i].TextureLayer = (float)prevTexLayer;

        if (polygon->FacingView) {
            nextVL = curVL + 1;
            if (nextVL >= nverts) nextVL = 0;
            nextVR = curVR - 1;
            if ((s32)nextVR < 0) nextVR = nverts - 1;
        } else {
            nextVL = curVL - 1;
            if ((s32)nextVL < 0) nextVL = nverts - 1;
            nextVR = curVR + 1;
            if (nextVR >= nverts) nextVR = 0;
        }

        s32 scaledPositions[10][2];
        s32 ytop = kScreenHeight, ybot = 0;
        for (u32 v = 0; v < polygon->NumVertices; v++) {
            scaledPositions[v][0] = polygon->Vertices[v]->FinalPosition[0];
            scaledPositions[v][1] = polygon->Vertices[v]->FinalPosition[1];
            ytop = std::min(scaledPositions[v][1], ytop);
            ybot = std::max(scaledPositions[v][1], ybot);
        }
        render_polygons_[i].YTop = ytop;
        render_polygons_[i].YBot = ybot;
        render_polygons_[i].XMin = kScreenWidth;
        render_polygons_[i].XMax = 0;

        if (ybot == ytop) {
            vtop = 0;
            vbot = 0;

            render_polygons_[i].YBot++;

            int j = 1;
            if (scaledPositions[j][0] < scaledPositions[vtop][0]) vtop = j;
            if (scaledPositions[j][0] > scaledPositions[vbot][0]) vbot = j;

            j = nverts - 1;
            if (scaledPositions[j][0] < scaledPositions[vtop][0]) vtop = j;
            if (scaledPositions[j][0] > scaledPositions[vbot][0]) vbot = j;

            assert(numYSpans < kMaxYSpanSetups);
            u32 curSpanL = numYSpans;
            SetupYSpanDummy(&render_polygons_[i], &yspan_setups_[numYSpans++], polygon, vtop, 0, scaledPositions);
            assert(numYSpans < kMaxYSpanSetups);
            u32 curSpanR = numYSpans;
            SetupYSpanDummy(&render_polygons_[i], &yspan_setups_[numYSpans++], polygon, vbot, 1, scaledPositions);

            yspan_indices_[numSetupIndices].PolyIdx = i;
            yspan_indices_[numSetupIndices].SpanIdxL = curSpanL;
            yspan_indices_[numSetupIndices].SpanIdxR = curSpanR;
            yspan_indices_[numSetupIndices].Y = ytop;
            numSetupIndices++;
        } else {
            u32 curSpanL = numYSpans;
            assert(numYSpans < kMaxYSpanSetups);
            SetupYSpan(&render_polygons_[i], &yspan_setups_[numYSpans++], polygon, curVL, nextVL, 0, scaledPositions);
            u32 curSpanR = numYSpans;
            assert(numYSpans < kMaxYSpanSetups);
            SetupYSpan(&render_polygons_[i], &yspan_setups_[numYSpans++], polygon, curVR, nextVR, 1, scaledPositions);

            for (u32 y = ytop; y < u32(ybot); y++) {
                if (y >= u32(scaledPositions[nextVL][1]) && curVL != polygon->VBottom) {
                    while (y >= u32(scaledPositions[nextVL][1]) && curVL != polygon->VBottom) {
                        curVL = nextVL;
                        if (polygon->FacingView) {
                            nextVL = curVL + 1;
                            if (nextVL >= nverts)
                                nextVL = 0;
                        } else {
                            nextVL = curVL - 1;
                            if ((s32)nextVL < 0)
                                nextVL = nverts - 1;
                        }
                    }

                    assert(numYSpans < kMaxYSpanSetups);
                    curSpanL = numYSpans;
                    SetupYSpan(&render_polygons_[i], &yspan_setups_[numYSpans++], polygon, curVL, nextVL, 0,
                               scaledPositions);
                }
                if (y >= u32(scaledPositions[nextVR][1]) && curVR != polygon->VBottom) {
                    while (y >= u32(scaledPositions[nextVR][1]) && curVR != polygon->VBottom) {
                        curVR = nextVR;
                        if (polygon->FacingView) {
                            nextVR = curVR - 1;
                            if ((s32)nextVR < 0)
                                nextVR = nverts - 1;
                        } else {
                            nextVR = curVR + 1;
                            if (nextVR >= nverts)
                                nextVR = 0;
                        }
                    }

                    assert(numYSpans < kMaxYSpanSetups);
                    curSpanR = numYSpans;
                    SetupYSpan(&render_polygons_[i], &yspan_setups_[numYSpans++], polygon, curVR, nextVR, 1,
                               scaledPositions);
                }

                yspan_indices_[numSetupIndices].PolyIdx = i;
                yspan_indices_[numSetupIndices].SpanIdxL = curSpanL;
                yspan_indices_[numSetupIndices].SpanIdxR = curSpanR;
                yspan_indices_[numSetupIndices].Y = y;
                numSetupIndices++;
            }
        }
    }

    stats_.rendered++;
    stats_.polygons += numPolygons;

    if (numYSpans > 0) {
        upload(yspans_, yspan_setups_, sizeof(SpanSetupY) * numYSpans);
        upload(setup_indices_, yspan_indices_.data(), sizeof(SetupIndices) * numSetupIndices);
        upload(polygons_, render_polygons_, sizeof(RenderPolygon) * numPolygons);
    }

    MetaUniform meta;
    meta.DispCnt = GPU3D.RenderDispCnt;
    meta.NumPolygons = numPolygons;
    meta.NumVariants = numVariants;
    meta.AlphaRef = GPU3D.RenderAlphaRef;
    {
        u32 a = (GPU3D.RenderClearAttr1 >> 16) & 0x1F;
        meta.ClearColor = to_rgb6(GPU3D.RenderClearAttr1) | (a << 24);
        meta.ClearDepth = ((GPU3D.RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;
        meta.ClearAttr = GPU3D.RenderClearAttr1 & 0x3F008000;

        u8 xoff = (GPU3D.RenderClearAttr2 >> 16) & 0xFF;
        u8 yoff = (GPU3D.RenderClearAttr2 >> 24) & 0xFF;
        meta.ClearBitmapOffset[0] = (float)xoff / 256.0f;
        meta.ClearBitmapOffset[1] = (float)yoff / 256.0f;
    }
    memset(meta.ToonTable, 0, sizeof(meta.ToonTable));
    for (u32 i = 0; i < 32; i++)
        meta.ToonTable[i * 4 + 0] = to_rgb6(GPU3D.RenderToonTable[i]);
    for (u32 i = 0; i < 34; i++)
        meta.ToonTable[i * 4 + 1] = GPU3D.RenderFogDensityTable[i];
    for (u32 i = 0; i < 8; i++)
        meta.ToonTable[i * 4 + 2] = to_rgb6(GPU3D.RenderEdgeTable[i]);
    meta.FogOffset = GPU3D.RenderFogOffset;
    meta.FogShift = GPU3D.RenderFogShift;
    {
        u32 fogA = (GPU3D.RenderFogColor >> 16) & 0x1F;
        meta.FogColor = to_rgb6(GPU3D.RenderFogColor) | (fogA << 24);
    }

    // Uploads above must land before anything reads them.
    cmd_.barrier(DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);

    // bind everything
    cmd_.bindImageDescriptorSet(image_descs_.gpu(), kNumImageDescriptors);
    cmd_.bindSamplerDescriptorSet(sampler_descs_.gpu(), kNumSamplers);
    cmd_.bindStorageBuffers(DkStage_Compute, 0,
                            {polygons_.extents(), xspans_.extents(), yspans_.extents(), setup_indices_.extents()});
    cmd_.bindStorageBuffers(DkStage_Compute, 5,
                            {final_tiles_.extents(), bin_result_.extents(), work_desc_.extents()});
    cmd_.bindUniformBuffer(DkStage_Compute, 0, meta_ubo_.gpu(), meta_ubo_.size);
    cmd_.bindUniformBuffer(DkStage_Compute, 1, variant_ubo_.gpu(), variant_ubo_.size);
    cmd_.pushConstants(meta_ubo_.gpu(), meta_ubo_.size, 0, sizeof(MetaUniform), &meta);

    cmd_.bindShaders(DkStageFlag_Compute, {&shaders_[kClearCoarseBinMask]});
    cmd_.dispatchCompute(kTilesPerLine * kTileLines / kClearCoarseBinMaskLocalSize, 1, 1);

    bool wbuffer = false;
    if (numYSpans > 0) {
        wbuffer = GPU3D.RenderPolygonRAM[0]->WBuffer;

        cmd_.bindShaders(DkStageFlag_Compute, {&shaders_[kClearIndirectWorkCount]});
        cmd_.dispatchCompute((numVariants + 31) / 32, 1, 1);

        // calculate x-spans
        cmd_.bindShaders(DkStageFlag_Compute, {&shaders_[wbuffer ? kInterpSpansW : kInterpSpansZ]});
        cmd_.dispatchCompute((numSetupIndices + 31) / 32, 1, 1);
        cmd_.barrier(DkBarrier_Primitives, 0);

        // bin polygons
        cmd_.bindShaders(DkStageFlag_Compute, {&shaders_[kBinCombined]});
        cmd_.dispatchCompute((numPolygons + 31) / 32, kScreenWidth / kCoarseTileW,
                             kScreenHeight / kCoarseTileH);
        cmd_.barrier(DkBarrier_Primitives, 0);

        // calculate list offsets
        cmd_.bindShaders(DkStageFlag_Compute, {&shaders_[kCalculateWorkOffsets]});
        cmd_.dispatchCompute((numVariants + 31) / 32, 1, 1);
        cmd_.barrier(DkBarrier_Primitives, 0);

        // sort shader work
        cmd_.bindShaders(DkStageFlag_Compute, {&shaders_[kSortWork]});
        cmd_.dispatchComputeIndirect(bin_result_.gpu() + offsetof(BinResultHeader, SortWorkWorkCount));
        cmd_.barrier(DkBarrier_Primitives, 0);

        // the tile buffers take bindings 2-4 from here on
        cmd_.bindStorageBuffers(DkStage_Compute, 2, {tiles_[0].extents(), tiles_[1].extents(), tiles_[2].extents()});

        // rasterise
        bool highLightMode = GPU3D.RenderDispCnt & (1 << 1);
        const int w = wbuffer ? 1 : 0;
        const int shadersNoTexture[] = {
            kRasteriseNoTextureZ + w,
            kRasteriseNoTextureZ + w,
            (highLightMode ? kRasteriseNoTextureHighlightZ : kRasteriseNoTextureToonZ) + w,
            kRasteriseNoTextureZ + w,
            kRasteriseShadowMaskZ + w,
        };
        const int shadersUseTexture[] = {
            kRasteriseUseTextureModulateZ + w,
            kRasteriseUseTextureDecalZ + w,
            (highLightMode ? kRasteriseUseTextureHighlightZ : kRasteriseUseTextureToonZ) + w,
            kRasteriseUseTextureDecalZ + w,
            kRasteriseShadowMaskZ + w,
        };

        int prevShader = -1;
        u32 prevTexture = 0, prevSampler = ~0u;
        for (u32 i = 0; i < numVariants; i++) {
            int shader;
            if (variants[i].Texture == 0) {
                shader = shadersNoTexture[variants[i].BlendMode];
            } else {
                shader = shadersUseTexture[variants[i].BlendMode];
                if (variants[i].Texture != prevTexture || variants[i].Sampler != prevSampler) {
                    cmd_.bindTextures(DkStage_Compute, 0, {dkMakeTextureHandle(variants[i].Texture, variants[i].Sampler)});
                    prevTexture = variants[i].Texture;
                    prevSampler = variants[i].Sampler;
                }
            }
            if (shader != prevShader) {
                cmd_.bindShaders(DkStageFlag_Compute, {&shaders_[shader]});
                prevShader = shader;
            }

            VariantUniform vu = {};
            vu.CurVariant = i;
            vu.TexIsCapture = 0;
            vu.InvTextureSize[0] = 1.f / variants[i].Width;
            vu.InvTextureSize[1] = 1.f / variants[i].Height;
            cmd_.pushConstants(variant_ubo_.gpu(), variant_ubo_.size, 0, sizeof(vu), &vu);
            cmd_.dispatchComputeIndirect(bin_result_.gpu() + offsetof(BinResultHeader, VariantWorkCount) + i * 4 * 4);
        }
    }
    cmd_.barrier(DkBarrier_Primitives, 0);

    // compose final image
    cmd_.bindStorageBuffers(DkStage_Compute, 2, {tiles_[0].extents(), tiles_[1].extents(), tiles_[2].extents()});
    cmd_.bindTextures(DkStage_Compute, 0,
                      {dkMakeTextureHandle(kImageClearColor, kSamplerRepeat),
                       dkMakeTextureHandle(kImageClearDepth, kSamplerRepeat)});
    cmd_.bindShaders(DkStageFlag_Compute, {&shaders_[wbuffer ? kDepthBlendW : kDepthBlendZ]});
    cmd_.dispatchCompute(kScreenWidth / kTileSize, kScreenHeight / kTileSize, 1);
    cmd_.barrier(DkBarrier_Primitives, 0);

    // the final pass writes the output buffer at binding 1
    cmd_.bindStorageBuffers(DkStage_Compute, 1, {slots_[cur_].output.extents()});
    u32 finalPassShader = 0;
    if (GPU3D.RenderDispCnt & (1 << 4))
        finalPassShader |= 0x4;
    if (GPU3D.RenderDispCnt & (1 << 7))
        finalPassShader |= 0x2;
    if (GPU3D.RenderDispCnt & (1 << 5))
        finalPassShader |= 0x1;
    cmd_.bindShaders(DkStageFlag_Compute, {&shaders_[kFinalPass0 + finalPassShader]});
    cmd_.dispatchCompute(kScreenWidth / 32, kScreenHeight, 1);
    cmd_.barrier(DkBarrier_Full, 0);
    cmd_.reportCounter(DkCounter_Timestamp, slots_[cur_].timestamps.gpu() + 16);

    submit(true);
    latest_ = cur_;
    if (!delay_)
        display_ = cur_;
    const uint64_t setup = now_ns() - setup_start;
    stats_.setup_ns += setup;
    stats_.max_setup_ns = std::max(stats_.max_setup_ns, setup);
}

u32* DekoRenderer3D::GetLine(int line) {
    if (display_ < 0)
        return blank_line_;
    Slot& shown = slots_[display_];
    timed_wait(shown);

    if (GPU3D.AbortFrame) {
        memset(scrolled_line_, 0, sizeof(scrolled_line_));
        return scrolled_line_;
    }

    u32* rawline = static_cast<u32*>(shown.output.cpu()) + line * kScreenWidth;
    u16 xpos = GPU3D.RenderXPos;
    if (xpos == 0)
        return rawline;

    // apply X scroll, as SoftRenderer3D::GetLine does
    if (xpos & 0x100) {
        int i = 0, j = xpos;
        for (; j < 512; i++, j++)
            scrolled_line_[i] = 0;
        for (j = 0; i < 256; i++, j++)
            scrolled_line_[i] = rawline[j];
    } else {
        int i = 0, j = xpos;
        for (; j < 256; i++, j++)
            scrolled_line_[i] = rawline[j];
        for (; i < 256; i++)
            scrolled_line_[i] = 0;
    }
    return scrolled_line_;
}

}  // namespace

std::unique_ptr<melonDS::Renderer3D> make_deko_renderer3d(melonDS::GPU3D& gpu3d, bool delay) {
    auto* v = video();
    if (!v)
        return nullptr;
    std::string probe = std::string(kShaderDir) + kShaderFiles[0] + ".dksh";
    if (FILE* f = fopen(probe.c_str(), "rb"))
        fclose(f);
    else
        return nullptr;
    return std::make_unique<DekoRenderer3D>(gpu3d, v->getDeko3dDevice(), delay);
}

}  // namespace gpu
