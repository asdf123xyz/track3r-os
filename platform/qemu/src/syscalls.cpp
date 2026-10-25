// SPDX-License-Identifier: GPL-3.0-or-later
//
// The newlib system calls and C++ runtime hooks the target tests reach (newlib-nano plus libstdc++ for doctest).
// They are written here instead of linking libnosys, whose stubs print a link warning each. The heap is `_sbrk`
// growing up through the board's PSRAM (mps2_an386.ld); its high-water mark is kept for runtime.h. Only stdout and
// stderr exist (the semihosting console); everything else fails with an error code.
#include <o3os/platform/qemu/clock.h>
#include <o3os/platform/qemu/runtime.h>
#include <o3os/platform/qemu/semihost.h>

#include <sys/stat.h>
#include <sys/time.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>

extern "C" {
extern char __heap_start[];
extern char __heap_end[];  // the end of the PSRAM
}

namespace o3os::platform::qemu {
namespace {
char* g_brk = nullptr;
char* g_brk_peak = nullptr;
}  // namespace

// For runtime.cpp.
std::size_t heap_peak_bytes() noexcept { return g_brk_peak == nullptr ? 0 : static_cast<std::size_t>(g_brk_peak - __heap_start); }
std::size_t heap_limit_bytes() noexcept { return static_cast<std::size_t>(__heap_end - __heap_start); }

}  // namespace o3os::platform::qemu

extern "C" {

// ---- Memory -----------------------------------------------------------------------------------------------------

// malloc's only source of memory. Single context, so no locking.
void* _sbrk(std::ptrdiff_t increment) {
  using namespace o3os::platform::qemu;
  if (g_brk == nullptr) {
    g_brk = __heap_start;
    g_brk_peak = g_brk;
  }
  // The break stays inside [__heap_start, __heap_end]: not past the PSRAM, and a negative increment may not take it
  // below the start of the heap either.
  if (increment > __heap_end - g_brk || increment < __heap_start - g_brk) {
    errno = ENOMEM;
    return reinterpret_cast<void*>(-1);
  }
  char* const previous = g_brk;
  g_brk += increment;
  if (g_brk > g_brk_peak) {
    g_brk_peak = g_brk;
  }
  return previous;
}

// ---- Console and process ----------------------------------------------------------------------------------------

// stdout and stderr go to the semihosting console.
int _write(int fd, const char* data, int size) {
  if ((fd != 1 && fd != 2) || size < 0) {
    errno = EBADF;
    return -1;
  }
  o3os::platform::qemu::console_write(data, static_cast<std::size_t>(size));
  return size;
}

// exit() ends here, ending the QEMU process with the status.
[[noreturn]] void _exit(int code) { o3os::platform::qemu::exit_qemu(code); }

// abort() (std::terminate, a failed allocation) raises SIGABRT, which arrives here: report it and end the run with
// status 4 rather than idling until the ctest timeout.
int _kill(int, int signal) {
  char line[] = "o3os-qemu: abort (signal 00)\n";
  line[25] = static_cast<char>('0' + (signal / 10) % 10);
  line[26] = static_cast<char>('0' + signal % 10);
  o3os::platform::qemu::console_write(line, sizeof line - 1);
  o3os::platform::qemu::exit_qemu(4);
}
int _getpid() { return 1; }

// Standard streams are character devices (stdio line-buffers them); nothing else can be opened, read or seeked.
int _fstat(int, struct stat* st) {
  st->st_mode = S_IFCHR;
  return 0;
}
int _isatty(int fd) { return fd >= 0 && fd <= 2; }
int _close(int) {
  errno = EBADF;
  return -1;
}
int _lseek(int, int, int) {
  errno = ESPIPE;
  return -1;
}
int _read(int, char*, int) {
  errno = EBADF;
  return -1;
}
int _open(const char*, int, int) {
  errno = ENOENT;
  return -1;
}

// Time since start() of the SysTick clock, at millisecond resolution (doctest's per-test timer reads it).
int _gettimeofday(struct timeval* tv, void*) {
  const std::uint64_t ms = o3os::platform::qemu::system_clock().uptime_ms();
  tv->tv_sec = static_cast<time_t>(ms / 1000);
  tv->tv_usec = static_cast<suseconds_t>((ms % 1000) * 1000);
  return 0;
}

// ---- C++ runtime ------------------------------------------------------------------------------------------------

// What crtbegin.o would define: the handle static destructors are registered under (__cxa_atexit).
[[gnu::visibility("hidden")]] void* __dso_handle = &__dso_handle;

// newlib-nano has no wide-character stdio, but libstdc++ builds wcout / wcin around these. A narrow-only program
// never reaches them; they exist so the link resolves, and fail if called.
wint_t putwc(wchar_t, FILE*) { return WEOF; }
wint_t getwc(FILE*) { return WEOF; }
wint_t ungetwc(wint_t, FILE*) { return WEOF; }
int swprintf(wchar_t*, std::size_t, const wchar_t*, ...) { return -1; }

}  // extern "C"
