#pragma once
// Audio output on libnx (audren). A dedicated thread drains a lock-free SPSC ring buffer
// fed by the emulation thread. push() never blocks: on overflow it drops the oldest data.

#include <cstddef>
#include <cstdint>

namespace audio {

bool init(unsigned sample_rate);  // host asks melonDS for 48000 (audren's native rate)
void shutdown();

// Interleaved stereo s16. Always consumes everything; returns `frames`.
size_t push(const int16_t* data, size_t frames);

void set_muted(bool muted);  // e.g. while a menu is open

struct Stats {
    float buffered_ms;    // audio queued in the ring, i.e. roughly the output latency
    unsigned underruns;   // wave buffers that ran dry (gaps), since init()
    unsigned overflows;   // pushes that dropped the oldest audio (skips), since init()
};
Stats stats();  // any thread

}  // namespace audio
