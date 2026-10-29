#include "platform/system.h"

#include <switch.h>

#include <algorithm>

namespace sys {

void set_current_thread_core(int core) {
    if (core < 0 || core > 2)
        return;
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
}

bool local_time(LocalTime& out) {
    // libnx's time() is UTC; the time zone comes from the time service's "my rule".
    u64 now = 0;
    TimeCalendarTime cal = {};
    TimeCalendarAdditionalInfo info = {};
    if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &now)) ||
        R_FAILED(timeToCalendarTimeWithMyRule(now, &cal, &info)))
        return false;
    out = {cal.year, cal.month, cal.day, cal.hour, cal.minute, cal.second};
    return true;
}

namespace {

// One clock domain (CPU or GPU): clkrst on 8.0.0+, pcv before. clkrstInitialize and
// pcvInitialize are reference-counted, so each domain opens and closes its own.
struct Clock {
    PcvModuleId id;
    PcvModule legacy;
    bool init = false;
    bool use_clkrst = false;
    ClkrstSession session = {};
    u32 stock_hz = 0;

    bool open() {
        if (init)
            return true;
        use_clkrst = hosversionAtLeast(8, 0, 0);
        if (use_clkrst) {
            if (R_FAILED(clkrstInitialize()))
                return false;
            if (R_FAILED(clkrstOpenSession(&session, id, 3))) {
                clkrstExit();
                return false;
            }
            clkrstGetClockRate(&session, &stock_hz);
        } else {
            if (R_FAILED(pcvInitialize()))
                return false;
            pcvGetClockRate(legacy, &stock_hz);
        }
        init = true;
        return true;
    }

    bool set_hz(u32 hz) {
        return R_SUCCEEDED(use_clkrst ? clkrstSetClockRate(&session, hz) : pcvSetClockRate(legacy, hz));
    }

    void restore() {
        if (!init)
            return;
        if (stock_hz)
            set_hz(stock_hz);
        if (use_clkrst) {
            clkrstCloseSession(&session);
            clkrstExit();
        } else {
            pcvExit();
        }
        init = false;
    }
};

Clock g_cpu{PcvModuleId_CpuBus, PcvModule_CpuBus};
Clock g_gpu{PcvModuleId_GPU, PcvModule_GPU};

}  // namespace

bool set_cpu_clock_mhz(unsigned mhz) {
    return g_cpu.open() && g_cpu.set_hz(u32(mhz) * 1000000u);
}

void restore_cpu_clock() {
    g_cpu.restore();
}

bool set_gpu_clock_khz(unsigned khz) {
    if (!g_gpu.open())
        return false;
    // Never below what the app started with (docked mode already runs faster).
    u32 hz = std::max(u32(khz) * 1000u, g_gpu.stock_hz);
    return g_gpu.set_hz(hz);
}

void restore_gpu_clock() {
    g_gpu.restore();
}

void memory_usage(unsigned long long& used, unsigned long long& total) {
    u64 u = 0, t = 0;
    svcGetInfo(&u, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&t, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    used = u;
    total = t;
}

}  // namespace sys
