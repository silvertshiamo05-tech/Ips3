// Ips3 shim for Xenia's xenia/kernel/xthread.h. The GPU runs its command processor and frame
// limiter on "host threads"; here those are plain host threads.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "xenia/base/threading.h"

namespace xe {
namespace kernel {

class KernelState;

// Xenia's intrusive reference; the GPU code only creates, dereferences and resets these.
template <typename T>
class object_ref {
 public:
  object_ref() = default;
  explicit object_ref(T* value) : value_(value) {}
  object_ref(object_ref&&) = default;
  object_ref& operator=(object_ref&&) = default;
  T* get() const { return value_.get(); }
  T* operator->() const { return value_.get(); }
  explicit operator bool() const { return value_ != nullptr; }
  void reset() { value_.reset(); }

 private:
  std::unique_ptr<T> value_;
};

class XThread {
 public:
  virtual ~XThread() = default;
  // True on the host thread this object started.
  static bool IsInThread(XThread* thread);
  static bool IsInThread();
};

class XHostThread : public XThread {
 public:
  XHostThread(KernelState* kernel_state, uint32_t stack_size, uint32_t creation_flags,
              std::function<int()> host_fn, uint32_t guest_process = 0);
  ~XHostThread() override;

  void set_name(const std::string& name) { name_ = name; }
  void set_can_debugger_suspend(bool) {}
  uint32_t Create();
  // Waits for the thread to finish (the arguments mirror Xenia's kernel wait).
  uint32_t Wait(uint32_t, uint32_t, uint32_t, void*);

  struct PriorityShim {
    template <typename T>
    void set_priority(T) {}
    void Resume() {}
    void Suspend() {}
  };
  PriorityShim* thread() { return &priority_; }

 private:
  std::function<int()> host_fn_;
  std::string name_;
  std::thread thread_;
  std::atomic<std::thread::id> id_{};
  PriorityShim priority_;
};

}  // namespace kernel
}  // namespace xe
