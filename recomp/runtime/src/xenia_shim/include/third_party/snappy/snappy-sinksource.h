// Ips3 shim for snappy's sink/source interface, used only by Xenia's GPU trace writer (GPU
// traces are not recorded in the Ips3 runtime).
#pragma once

#include <cstddef>

namespace snappy {

class Source {
 public:
  virtual ~Source() = default;
  virtual size_t Available() const = 0;
  virtual const char* Peek(size_t* len) = 0;
  virtual void Skip(size_t n) = 0;
};

class Sink {
 public:
  virtual ~Sink() = default;
  virtual void Append(const char* bytes, size_t n) = 0;
  virtual char* GetAppendBuffer(size_t, char* scratch) { return scratch; }
};

class ByteArraySource : public Source {
 public:
  ByteArraySource(const char* p, size_t n) : ptr_(p), left_(n) {}
  size_t Available() const override { return left_; }
  const char* Peek(size_t* len) override {
    *len = left_;
    return ptr_;
  }
  void Skip(size_t n) override {
    ptr_ += n;
    left_ -= n;
  }

 private:
  const char* ptr_;
  size_t left_;
};

}  // namespace snappy
