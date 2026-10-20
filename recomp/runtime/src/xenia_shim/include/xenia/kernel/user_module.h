// Ips3 shim for Xenia's xenia/kernel/user_module.h.
#pragma once

#include <cstdint>

namespace xe {
namespace kernel {

class UserModule {
 public:
  uint32_t title_id() const { return title_id_; }
  uint32_t title_id_ = 0;
};

}  // namespace kernel
}  // namespace xe
