#pragma once
// Small libnx system services the frontend needs but must not call directly.

namespace sys {

// Pins the calling thread to one CPU core (0-2; core 3 belongs to the OS).
// Used for melonDS worker threads, which the frontend creates with std::thread.
void set_current_thread_core(int core);

// The Switch's local wall-clock time (system clock + the console's time zone), as
// shown in the HOME menu. False if the time service is unavailable.
struct LocalTime {
    int year, month, day;  // month 1-12, day 1-31
    int hour, minute, second;
};
bool local_time(LocalTime& out);

// CPU clock (clkrst on 8.0.0+, pcv before). The first call remembers the clock the app
// started with; restore_cpu_clock() puts it back. The OS may reset the clock when the
// app loses and regains focus, so callers re-apply periodically.
bool set_cpu_clock_mhz(unsigned mhz);
void restore_cpu_clock();

// Process memory in bytes: currently used, and the total the process may use (much
// lower in applet mode than when launched via title override).
void memory_usage(unsigned long long& used, unsigned long long& total);

}  // namespace sys
