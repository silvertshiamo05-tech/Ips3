// Ips3: Xenia kernel pieces the GPU uses (see include/xenia/kernel/*.h).

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

namespace {
thread_local XThread* g_current_thread = nullptr;
}

bool XThread::IsInThread(XThread* thread) { return thread != nullptr && g_current_thread == thread; }
bool XThread::IsInThread() { return g_current_thread != nullptr; }

XHostThread::XHostThread(KernelState*, uint32_t, uint32_t, std::function<int()> host_fn,
                         uint32_t)
    : host_fn_(std::move(host_fn)) {}

XHostThread::~XHostThread() {
  if (thread_.joinable()) {
    thread_.detach();
  }
}

uint32_t XHostThread::Create() {
  thread_ = std::thread([this] {
    g_current_thread = this;
#if defined(__linux__)
    pthread_setname_np(pthread_self(), name_.substr(0, 15).c_str());
#elif defined(__APPLE__)
    pthread_setname_np(name_.c_str());
#endif
    host_fn_();
  });
  return 0;
}

uint32_t XHostThread::Wait(uint32_t, uint32_t, uint32_t, void*) {
  if (thread_.joinable() && std::this_thread::get_id() != thread_.get_id()) {
    thread_.join();
  }
  return 0;
}

void KernelState::EmulateCPInterruptDPC(uint32_t interrupt_callback,
                                        uint32_t interrupt_callback_data, uint32_t source,
                                        uint32_t cpu) {
  if (interrupt_callback) {
    ips3_dispatch_gpu_interrupt(interrupt_callback, interrupt_callback_data, source, cpu);
  }
}

}  // namespace kernel
}  // namespace xe
