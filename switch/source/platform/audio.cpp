// audren output. The emu thread push()es into a lock-free SPSC ring; a dedicated thread
// drains it into a small set of wave buffers.
//
// Thread placement: UI on core 0, emulation on core 1, audio on core 2 alongside
// melonDS's 3D render thread (see sys::set_current_thread_core). Core 3 belongs to the
// system. Priority 0x2B is one notch above the default (0x2C) of every other thread, so
// the render thread sharing core 2 can't starve audio submission.

#include "platform/audio.h"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace audio {

namespace {

constexpr int kVoice = 0;
constexpr int kChannels = 2;
constexpr int kWaveBufs = 4;
constexpr size_t kWaveFrames = 1024;                 // ~21 ms each at 48 kHz
constexpr size_t kRingFrames = 8192;                 // ~170 ms; power of two
constexpr size_t kRingMask = kRingFrames - 1;
constexpr int kThreadCore = 2;
constexpr int kThreadPrio = 0x2B;
constexpr size_t kThreadStack = 0x10000;

const AudioRendererConfig kConfig = {
    AudioRendererOutputRate_48kHz,  // output_rate
    4,                              // num_voices
    0,                              // num_effects
    1,                              // num_sinks
    1,                              // num_mix_objs
    2,                              // num_mix_buffers
};

// One stereo frame packed as u32 so a frame is written/read atomically per slot.
uint32_t g_ring[kRingFrames];
std::atomic<size_t> g_read{0};
std::atomic<size_t> g_write{0};

AudioDriver g_drv;
AudioDriverWaveBuf g_wavebufs[kWaveBufs];
void* g_pool = nullptr;
size_t g_pool_size = 0;
int g_pool_id = -1;

Thread g_thread;
std::atomic<bool> g_running{false};
std::atomic<bool> g_muted{false};
std::atomic<unsigned> g_underruns{0};
std::atomic<unsigned> g_overflows{0};
unsigned g_rate = 48000;
bool g_ready = false;
bool g_audren_up = false;
bool g_drv_up = false;
bool g_thread_up = false;

// Consumer side: copy up to `frames` frames into out; returns frames copied.
size_t ring_pop(uint32_t* out, size_t frames) {
    size_t r = g_read.load(std::memory_order_acquire);
    size_t w = g_write.load(std::memory_order_acquire);
    size_t n = std::min(frames, w - r);
    for (size_t i = 0; i < n; ++i)
        out[i] = g_ring[(r + i) & kRingMask];
    // If the producer dropped data under us (overflow), read already moved past r; keep it.
    g_read.compare_exchange_strong(r, r + n, std::memory_order_release, std::memory_order_relaxed);
    return n;
}

void fill(AudioDriverWaveBuf& wb) {
    uint32_t* out = static_cast<uint32_t*>(const_cast<void*>(wb.data_raw));
    size_t n = ring_pop(out, kWaveFrames);
    if (g_muted.load(std::memory_order_relaxed))
        n = 0;  // still drained above so stale audio isn't played on unmute
    if (n < kWaveFrames) {
        std::memset(out + n, 0, (kWaveFrames - n) * sizeof(uint32_t));  // underrun -> silence
        if (n > 0)  // a partially filled buffer is a real gap; empty means nothing playing (paused)
            g_underruns.fetch_add(1, std::memory_order_relaxed);
    }
    armDCacheFlush(out, kWaveFrames * sizeof(uint32_t));
}

void thread_main(void*) {
    while (g_running.load(std::memory_order_acquire)) {
        for (auto& wb : g_wavebufs) {
            if (wb.state == AudioDriverWaveBufState_Free || wb.state == AudioDriverWaveBufState_Done) {
                fill(wb);
                audrvVoiceAddWaveBuf(&g_drv, kVoice, &wb);
            }
        }
        if (!audrvVoiceIsPlaying(&g_drv, kVoice))
            audrvVoiceStart(&g_drv, kVoice);
        audrvUpdate(&g_drv);
        audrenWaitFrame();
    }
}

}  // namespace

bool init(unsigned sample_rate) {
    if (g_ready)
        return true;

    g_read.store(0);
    g_write.store(0);
    g_underruns.store(0);
    g_overflows.store(0);
    g_rate = sample_rate;

    if (R_FAILED(audrenInitialize(&kConfig)))
        goto fail;
    g_audren_up = true;
    if (R_FAILED(audrvCreate(&g_drv, &kConfig, kChannels)))
        goto fail;
    g_drv_up = true;

    {
        static const u8 kSinkChannels[kChannels] = {0, 1};
        audrvDeviceSinkAdd(&g_drv, AUDREN_DEFAULT_DEVICE_NAME, kChannels, kSinkChannels);
    }
    if (R_FAILED(audrvUpdate(&g_drv)) || R_FAILED(audrenStartAudioRenderer()))
        goto fail;

    {
        size_t buf_bytes = kWaveFrames * sizeof(uint32_t);
        g_pool_size = (buf_bytes * kWaveBufs + (AUDREN_MEMPOOL_ALIGNMENT - 1)) & ~size_t(AUDREN_MEMPOOL_ALIGNMENT - 1);
        g_pool = aligned_alloc(AUDREN_MEMPOOL_ALIGNMENT, g_pool_size);
        if (!g_pool)
            goto fail;
        std::memset(g_pool, 0, g_pool_size);
        armDCacheFlush(g_pool, g_pool_size);

        g_pool_id = audrvMemPoolAdd(&g_drv, g_pool, g_pool_size);
        if (g_pool_id < 0)
            goto fail;
        audrvMemPoolAttach(&g_drv, g_pool_id);

        for (int i = 0; i < kWaveBufs; ++i) {
            std::memset(&g_wavebufs[i], 0, sizeof(AudioDriverWaveBuf));
            g_wavebufs[i].data_raw = static_cast<u8*>(g_pool) + i * buf_bytes;
            g_wavebufs[i].size = buf_bytes;
            g_wavebufs[i].start_sample_offset = 0;
            g_wavebufs[i].end_sample_offset = kWaveFrames;
        }
    }

    // At 48 kHz the voice matches the output rate; audren resamples anything else.
    if (!audrvVoiceInit(&g_drv, kVoice, kChannels, PcmFormat_Int16, int(sample_rate)))
        goto fail;
    audrvVoiceSetDestinationMix(&g_drv, kVoice, AUDREN_FINAL_MIX_ID);
    audrvVoiceSetMixFactor(&g_drv, kVoice, 1.0f, 0, 0);
    audrvVoiceSetMixFactor(&g_drv, kVoice, 0.0f, 0, 1);
    audrvVoiceSetMixFactor(&g_drv, kVoice, 0.0f, 1, 0);
    audrvVoiceSetMixFactor(&g_drv, kVoice, 1.0f, 1, 1);
    audrvUpdate(&g_drv);

    g_running.store(true, std::memory_order_release);
    if (R_FAILED(threadCreate(&g_thread, thread_main, nullptr, nullptr, kThreadStack, kThreadPrio, kThreadCore)))
        goto fail;
    if (R_FAILED(threadStart(&g_thread))) {
        threadClose(&g_thread);
        goto fail;
    }
    g_thread_up = true;
    g_ready = true;
    return true;

fail:
    g_ready = true;  // let shutdown() tear down whatever came up
    shutdown();
    return false;
}

void shutdown() {
    if (!g_ready)
        return;
    g_running.store(false, std::memory_order_release);
    if (g_thread_up) {
        threadWaitForExit(&g_thread);
        threadClose(&g_thread);
        g_thread_up = false;
    }
    if (g_drv_up) {
        audrvVoiceStop(&g_drv, kVoice);
        audrvVoiceDrop(&g_drv, kVoice);
        if (g_pool_id >= 0) {
            audrvMemPoolDetach(&g_drv, g_pool_id);
            audrvMemPoolRemove(&g_drv, g_pool_id);
            g_pool_id = -1;
        }
        audrvUpdate(&g_drv);
        audrvClose(&g_drv);
        g_drv_up = false;
    }
    if (g_audren_up) {
        audrenExit();
        g_audren_up = false;
    }
    free(g_pool);
    g_pool = nullptr;
    g_ready = false;
}

size_t push(const int16_t* data, size_t frames) {
    if (!g_ready || !data || frames == 0)
        return frames;

    const int16_t* src = data;
    size_t n = frames;
    if (n > kRingFrames) {  // keep only the newest ring's worth
        src += (n - kRingFrames) * kChannels;
        n = kRingFrames;
    }

    size_t w = g_write.load(std::memory_order_relaxed);
    // Overflow: drop the oldest frames by advancing the read index past them.
    size_t r = g_read.load(std::memory_order_acquire);
    if (w + n - r > kRingFrames)
        g_overflows.fetch_add(1, std::memory_order_relaxed);
    while (w + n - r > kRingFrames) {
        if (g_read.compare_exchange_weak(r, w + n - kRingFrames, std::memory_order_acq_rel, std::memory_order_acquire))
            break;
    }

    for (size_t i = 0; i < n; ++i) {
        uint32_t frame;
        std::memcpy(&frame, src + i * kChannels, sizeof(frame));
        g_ring[(w + i) & kRingMask] = frame;
    }
    g_write.store(w + n, std::memory_order_release);
    return frames;
}

Stats stats() {
    size_t queued = g_write.load(std::memory_order_acquire) - g_read.load(std::memory_order_acquire);
    return {queued * 1000.0f / float(g_rate), g_underruns.load(), g_overflows.load()};
}

void set_muted(bool muted) {
    g_muted.store(muted, std::memory_order_relaxed);
}

}  // namespace audio
