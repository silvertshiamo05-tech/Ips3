// Ips3: the parts of Xenia's xe::Memory its GPU code uses, over the runtime's flat guest memory.
//
// Guest memory is one host reservation (Memory::base in the runtime). Physical memory is the
// 512 MiB at guest 0xA0000000, so physical address p is host base + 0xA0000000 + p.
//
// CPU-write detection follows Xenia's PhysicalHeap: the GPU arms ranges it keeps copies of,
// their host pages become read-only, and the first write faults. The handler calls the
// invalidation callbacks, unwatches what they say is safe (at most 4 MiB around the write)
// and makes those pages writable again, and the write is retried.

#include <atomic>
#include <csignal>
#include <cstring>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>
#include <execinfo.h>

#include "xenia/base/math.h"
#include "xenia/base/mutex.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {
class MMIOHandler {};
}  // namespace cpu
}  // namespace xe

// Set by the runtime before the GPU starts (gpu/xenia_bridge.cpp).
uint8_t* ips3_guest_membase = nullptr;
xe::cpu::MMIORange ips3_gpu_register_range{};

namespace {

constexpr uint32_t kPhysicalBase = 0xA0000000;
constexpr uint32_t kPhysicalSize = 0x20000000;

xe::Memory* g_memory_instance = nullptr;
size_t g_page_size = 0;
uint32_t g_page_shift = 0;
std::vector<uint64_t> g_watched;  // one bit per host page of physical memory

std::atomic<uint64_t> g_watch_triggers{0};
std::atomic<uint64_t> g_watch_arms{0};

struct sigaction g_previous_segv;
struct sigaction g_previous_bus;

// Physical memory is also visible at 0xC0000000, 0xE0000000 (shifted: guest 0xE0000000 is
// physical 0x1000) and below the GPU registers at 0x7F000000 (runtime kernel/memory.cpp).
// Watches protect the page in every window, and a fault in any of them counts.
bool GuestToPhysical(uint32_t guest, uint32_t* physical) {
  if (guest >= 0xE0000000) {
    *physical = guest - 0xE0000000 + 0x1000;
    return *physical < kPhysicalSize;
  }
  if (guest >= 0xA0000000) {
    *physical = guest & 0x1FFFFFFF;
    return true;
  }
  if (guest >= 0x7F000000 && guest < 0x7FC80000) {
    *physical = guest - 0x7F000000;
    return true;
  }
  return false;
}

void ProtectPhysical(uint32_t physical, size_t size, int protection) {
  uint8_t* base = ips3_guest_membase;
  mprotect(base + 0xA0000000 + physical, size, protection);
  mprotect(base + 0xC0000000 + physical, size, protection);
  if (physical >= 0x1000) {
    mprotect(base + 0xE0000000 + physical - 0x1000, size, protection);
  }
  if (physical < 0xC80000) {
    mprotect(base + 0x7F000000 + physical, std::min<size_t>(size, 0xC80000 - physical), protection);
  }
}

bool PageWatched(uint32_t page) {
  return (g_watched[page >> 6] >> (page & 63)) & 1;
}

void SetPageWatched(uint32_t page, bool watched) {
  if (watched) {
    g_watched[page >> 6] |= uint64_t(1) << (page & 63);
  } else {
    g_watched[page >> 6] &= ~(uint64_t(1) << (page & 63));
  }
}

}  // namespace

namespace xe {

// The runtime never constructs a Memory: its heap members would need all of Xenia's heap
// code. ips3_create_gpu_memory() hands out zeroed storage of the right size (empty vectors,
// null pointers) and this fills in what the GPU code reads.
bool Memory::Initialize() {
  virtual_membase_ = ips3_guest_membase;
  physical_membase_ = ips3_guest_membase + kPhysicalBase;

  g_page_size = size_t(sysconf(_SC_PAGESIZE));
  g_page_shift = xe::log2_floor(uint32_t(g_page_size));
  g_watched.assign(((kPhysicalSize >> g_page_shift) + 63) / 64, 0);
  g_memory_instance = this;
  return true;
}

const BaseHeap* Memory::LookupHeap(uint32_t) const { return nullptr; }
BaseHeap* Memory::LookupHeapByType(bool, uint32_t) { return nullptr; }
// A placeholder: only its QueryRangeAccess/QueryRegionInfo are called (see stubs.cc), and they
// do not read the object.
VirtualHeap* Memory::GetPhysicalHeap() {
  alignas(alignof(VirtualHeap)) static uint8_t storage[sizeof(VirtualHeap)] = {};
  return reinterpret_cast<VirtualHeap*>(storage);
}

uint32_t Memory::HostToGuestVirtual(const void* host_address) const {
  return uint32_t(static_cast<const uint8_t*>(host_address) - virtual_membase_);
}

uint32_t Memory::GetPhysicalAddress(uint32_t address) const {
  uint32_t physical;
  return GuestToPhysical(address, &physical) ? physical : UINT32_MAX;
}

bool Memory::AddVirtualMappedRange(uint32_t virtual_address, uint32_t mask, uint32_t size,
                                   void* context, cpu::MMIOReadCallback read_callback,
                                   cpu::MMIOWriteCallback write_callback) {
  ips3_gpu_register_range = {virtual_address, mask,          size,
                             context,         read_callback, write_callback};
  return true;
}

void* Memory::RegisterPhysicalMemoryInvalidationCallback(
    PhysicalMemoryInvalidationCallback callback, void* callback_context) {
  auto* entry = new std::pair<PhysicalMemoryInvalidationCallback, void*>(callback,
                                                                         callback_context);
  auto lock = global_critical_region_.Acquire();
  physical_memory_invalidation_callbacks_.push_back(entry);
  return entry;
}

void Memory::UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle) {
  auto* entry =
      static_cast<std::pair<PhysicalMemoryInvalidationCallback, void*>*>(callback_handle);
  {
    auto lock = global_critical_region_.Acquire();
    auto& callbacks = physical_memory_invalidation_callbacks_;
    auto it = std::find(callbacks.begin(), callbacks.end(), entry);
    if (it != callbacks.end()) {
      callbacks.erase(it);
    }
  }
  delete entry;
}

void Memory::EnablePhysicalMemoryAccessCallbacks(uint32_t physical_address, uint32_t length,
                                                 bool enable_invalidation_notifications,
                                                 bool enable_data_providers) {
  if (!enable_invalidation_notifications || length == 0) {
    return;  // data providers are not used by the GPU's shared memory
  }
  physical_address &= 0x1FFFFFFF;
  length = std::min(length, kPhysicalSize - physical_address);

  const uint32_t first = physical_address >> g_page_shift;
  const uint32_t last = (physical_address + length - 1) >> g_page_shift;

  auto lock = global_critical_region_.Acquire();
  g_watch_arms++;
  uint32_t run_start = UINT32_MAX;
  auto flush = [&](uint32_t end) {
    if (run_start != UINT32_MAX) {
      ProtectPhysical(run_start << g_page_shift, size_t(end - run_start) << g_page_shift,
                      PROT_READ);
      run_start = UINT32_MAX;
    }
  };
  for (uint32_t page = first; page <= last; ++page) {
    if (PageWatched(page)) {
      flush(page);
      continue;
    }
    SetPageWatched(page, true);
    if (run_start == UINT32_MAX) {
      run_start = page;
    }
  }
  flush(last + 1);
}

// Handles a write to watched physical memory at [physical_address, +length). Returns false
// if nothing there was watched.
bool Memory::TriggerPhysicalMemoryCallbacks(global_unique_lock_type global_lock_locked_once,
                                            uint32_t virtual_address, uint32_t length,
                                            bool is_write, bool unwatch_exact_range,
                                            bool unprotect) {
  uint32_t physical;
  if (length == 0 || !GuestToPhysical(virtual_address, &physical)) {
    return false;
  }
  length = std::min(length, kPhysicalSize - physical);

  uint32_t first = physical >> g_page_shift;
  uint32_t last = (physical + length - 1) >> g_page_shift;

  bool any = false;
  for (uint32_t page = first; page <= last && !any; ++page) {
    any = PageWatched(page);
  }
  if (!any) {
    return false;
  }

  g_watch_triggers++;
  const uint32_t start = first << g_page_shift;
  const uint32_t span = ((last + 1) << g_page_shift) - start;

  // Each callback says how much around the write it no longer needs watched.
  uint32_t unwatch_first = 0;
  uint32_t unwatch_last = UINT32_MAX;
  for (auto* callback : physical_memory_invalidation_callbacks_) {
    auto range = callback->first(callback->second, start, span, unwatch_exact_range);
    if (!unwatch_exact_range) {
      unwatch_first = std::max(unwatch_first, range.first);
      unwatch_last =
          std::min(unwatch_last, xe::sat_add(range.first, std::max(range.second, 1u) - 1));
    }
  }
  if (unwatch_exact_range) {
    unwatch_first = start;
    unwatch_last = start + span - 1;
  } else {
    constexpr uint32_t kMaxUnwatchExcess = 4 * 1024 * 1024;
    unwatch_first = std::min(unwatch_first, start);
    unwatch_last = std::max(unwatch_last, start + span - 1);
    unwatch_first = std::max(unwatch_first, start & ~(kMaxUnwatchExcess - 1));
    unwatch_last = std::min(unwatch_last, (start + span - 1) | (kMaxUnwatchExcess - 1));
    unwatch_last = std::min(unwatch_last, kPhysicalSize - 1);
  }

  first = unwatch_first >> g_page_shift;
  last = unwatch_last >> g_page_shift;
  uint32_t run_start = UINT32_MAX;
  auto flush = [&](uint32_t end) {
    if (run_start != UINT32_MAX) {
      if (unprotect) {
        ProtectPhysical(run_start << g_page_shift, size_t(end - run_start) << g_page_shift,
                        PROT_READ | PROT_WRITE);
      }
      run_start = UINT32_MAX;
    }
  };
  for (uint32_t page = first; page <= last; ++page) {
    if (!PageWatched(page)) {
      flush(page);
      continue;
    }
    SetPageWatched(page, false);
    if (run_start == UINT32_MAX) {
      run_start = page;
    }
  }
  flush(last + 1);
  return true;
}

}  // namespace xe

xe::Memory* ips3_create_gpu_memory() {
  alignas(alignof(xe::Memory)) static uint8_t storage[sizeof(xe::Memory)] = {};
  auto* memory = reinterpret_cast<xe::Memory*>(storage);
  memory->Initialize();
  return memory;
}

// Called before the host writes guest memory in a way a fault cannot catch (a read()
// system call into guest memory fails with EFAULT on a read-only page instead).
void ips3_memory_will_write(uint32_t guest_address, uint32_t length) {
  if (!g_memory_instance) {
    return;
  }
  auto lock = xe::global_critical_region::AcquireDirect();
  g_memory_instance->TriggerPhysicalMemoryCallbacks(std::move(lock), guest_address, length,
                                                    true, false, true);
}

namespace {

void FaultHandler(int signal, siginfo_t* info, void* context) {
  auto* address = static_cast<uint8_t*>(info->si_addr);
  if (g_memory_instance && address >= ips3_guest_membase &&
      address < ips3_guest_membase + 0x100000000ull) {
    const uint32_t guest = uint32_t(address - ips3_guest_membase);
    auto lock = xe::global_critical_region::AcquireDirect();
    if (g_memory_instance->TriggerPhysicalMemoryCallbacks(std::move(lock), guest, 1, true,
                                                          false, true)) {
      return;  // retry the write
    }
  }

  // Not ours: hand it to whoever was installed before (or crash as usual).
  const struct sigaction& previous = signal == SIGBUS ? g_previous_bus : g_previous_segv;
  if (previous.sa_flags & SA_SIGINFO) {
    previous.sa_sigaction(signal, info, context);
    return;
  }
  if (previous.sa_handler == SIG_IGN) {
    return;
  }
  if (previous.sa_handler != SIG_DFL) {
    previous.sa_handler(signal);
    return;
  }

  // A real crash: say where before the default action ends the process.
  char header[96];
  const int length = snprintf(header, sizeof(header), "Ips3: fatal signal %d at address %p\n",
                              signal, info->si_addr);
  write(STDERR_FILENO, header, length);
  void* frames[48];
  backtrace_symbols_fd(frames, backtrace(frames, 48), STDERR_FILENO);
  ::signal(signal, SIG_DFL);
}

}  // namespace

void ips3_install_write_watch_handler() {
  struct sigaction action {};
  action.sa_sigaction = FaultHandler;
  action.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&action.sa_mask);
  sigaction(SIGSEGV, &action, &g_previous_segv);
  sigaction(SIGBUS, &action, &g_previous_bus);
}

void ips3_watch_stats(uint64_t* arms, uint64_t* triggers) {
  *arms = g_watch_arms;
  *triggers = g_watch_triggers;
}
