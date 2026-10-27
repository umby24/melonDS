#include "platform/system.h"

#include <switch.h>

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

bool g_clock_init = false;
bool g_use_clkrst = false;
ClkrstSession g_clkrst;
u32 g_stock_hz = 0;

bool clock_init() {
    if (g_clock_init)
        return true;
    g_use_clkrst = hosversionAtLeast(8, 0, 0);
    if (g_use_clkrst) {
        if (R_FAILED(clkrstInitialize()))
            return false;
        if (R_FAILED(clkrstOpenSession(&g_clkrst, PcvModuleId_CpuBus, 3))) {
            clkrstExit();
            return false;
        }
        clkrstGetClockRate(&g_clkrst, &g_stock_hz);
    } else {
        if (R_FAILED(pcvInitialize()))
            return false;
        pcvGetClockRate(PcvModule_CpuBus, &g_stock_hz);
    }
    g_clock_init = true;
    return true;
}

bool set_hz(u32 hz) {
    return R_SUCCEEDED(g_use_clkrst ? clkrstSetClockRate(&g_clkrst, hz) : pcvSetClockRate(PcvModule_CpuBus, hz));
}

}  // namespace

bool set_cpu_clock_mhz(unsigned mhz) {
    return clock_init() && set_hz(u32(mhz) * 1000000u);
}

void restore_cpu_clock() {
    if (!g_clock_init)
        return;
    if (g_stock_hz)
        set_hz(g_stock_hz);
    if (g_use_clkrst) {
        clkrstCloseSession(&g_clkrst);
        clkrstExit();
    } else {
        pcvExit();
    }
    g_clock_init = false;
}

void memory_usage(unsigned long long& used, unsigned long long& total) {
    u64 u = 0, t = 0;
    svcGetInfo(&u, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&t, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    used = u;
    total = t;
}

}  // namespace sys
