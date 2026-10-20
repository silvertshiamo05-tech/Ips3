// Ips3 shim for Xenia's xenia/ui/windowed_app_context.h. The runtime presents offscreen, so
// there is no UI thread; GraphicsSystem is given no app context.
#pragma once

#include <functional>

namespace xe {
namespace ui {

class WindowedAppContext {
 public:
  void CallInUIThreadSynchronous(std::function<void()> fn) { fn(); }
  bool CallInUIThread(std::function<void()> fn) {
    fn();
    return true;
  }
};

}  // namespace ui
}  // namespace xe
