// Ips3: runs Xenia's Vulkan GPU for the runtime (the API is in ips3_gpu.h).
//
// The game's Direct3D writes GPU registers through the window at 0x7FC80000. Xenia traps those
// accesses; the runtime cannot (the recompiled code accesses guest memory directly), so the
// window is plain memory: a thread polls CP_RB_WPTR there and hands changes to Xenia, and the
// registers the CPU reads are mirrored back into it.

#include "ips3_gpu.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <thread>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/ui/presenter.h"

DECLARE_string(occlusion_query);

extern uint8_t* ips3_guest_membase;
xe::Memory* ips3_create_gpu_memory();
void ips3_install_write_watch_handler();
void ips3_watch_stats(uint64_t* arms, uint64_t* triggers);

namespace {

constexpr uint32_t kRegisterWindow = 0x7FC80000;
constexpr uint32_t kRegWptr = 0x01C5;

struct Gpu {
  xe::Memory* memory = nullptr;
  std::unique_ptr<xe::cpu::Processor> processor;
  std::unique_ptr<xe::Emulator> emulator;
  std::unique_ptr<xe::kernel::KernelState> kernel_state;
  std::unique_ptr<xe::gpu::vulkan::VulkanGraphicsSystem> graphics;
  std::atomic<bool> running{false};
  std::atomic<uint64_t> swaps{0};
};

Gpu g_gpu;

uint32_t LoadRegister(uint32_t index) {
  return xe::byte_swap(*reinterpret_cast<uint32_t*>(ips3_guest_membase + kRegisterWindow + index * 4));
}

void StoreRegister(uint32_t index, uint32_t value) {
  *reinterpret_cast<uint32_t*>(ips3_guest_membase + kRegisterWindow + index * 4) = xe::byte_swap(value);
}

// The registers the CPU reads, as Xenia's GraphicsSystem::ReadRegister answers them.
void MirrorRegisters() {
  auto* regs = g_gpu.graphics->register_file();
  StoreRegister(0x0F00, 0x08100748);  // RB_EDRAM_TIMING
  StoreRegister(0x0F01, 0x0000200E);  // RB_BC_CONTROL
  StoreRegister(0x0F2D, 0x00BBBBBB);  // RB_HSIO_INTERFACE_ALIGNER_VALUE
  StoreRegister(0x1951, 1);           // interrupt status: vblank
  StoreRegister(0x1961, 0x050002D0);  // AVIVO_D1MODE_VIEWPORT_SIZE 1280x720
  StoreRegister(0x194C, regs->values[0x194C]);  // D1MODE_V_COUNTER
  StoreRegister(0x01C4, regs->values[0x01C4]);  // CP_RB_RPTR
}

void RegisterThread() {
  uint32_t last_wptr = LoadRegister(kRegWptr);
  auto last_mirror = std::chrono::steady_clock::now();
  while (g_gpu.running) {
    const uint32_t wptr = LoadRegister(kRegWptr);
    if (wptr != last_wptr) {
      last_wptr = wptr;
      const auto& range = ips3_gpu_register_range;
      if (range.write) {
        range.write(nullptr, range.callback_context, kRegisterWindow + kRegWptr * 4, wptr);
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_mirror > std::chrono::milliseconds(1)) {
      last_mirror = now;
      MirrorRegisters();
    }
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
}

}  // namespace

bool ips3_gpu_start(uint8_t* guest_membase) {
  ips3_guest_membase = guest_membase;
  xe::InitializeLogging("ips3");

  // Occlusion query results: Xenia's real host queries only write their reports back as
  // submissions complete, which leaves Direct3D's GetData polling forever here; "fake" writes
  // a plausible sample count at once (Xenia's own fallback). IPS3_OCCLUSION_QUERY overrides.
  const char* occlusion = getenv("IPS3_OCCLUSION_QUERY");
  cvars::occlusion_query = occlusion ? occlusion : "fake";

  g_gpu.memory = ips3_create_gpu_memory();
  ips3_install_write_watch_handler();

  g_gpu.processor = std::make_unique<xe::cpu::Processor>(g_gpu.memory);
  g_gpu.emulator = std::make_unique<xe::Emulator>();
  g_gpu.emulator->memory_ = g_gpu.memory;
  g_gpu.kernel_state = std::make_unique<xe::kernel::KernelState>(
      g_gpu.emulator.get(), g_gpu.memory, g_gpu.processor.get());

  g_gpu.graphics = std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
  g_gpu.emulator->graphics_system_ = g_gpu.graphics.get();
  // No window: presentation goes to an offscreen guest output image that can be captured.
  if (g_gpu.graphics->Setup(g_gpu.processor.get(), g_gpu.kernel_state.get(), nullptr, true) !=
      0) {
    XELOGE("Ips3: Vulkan GPU setup failed");
    return false;
  }

  MirrorRegisters();
  g_gpu.running = true;
  std::thread(RegisterThread).detach();

  // IPS3_CAPTURE=<folder>: save the presented frame there every two seconds.
  if (const char* folder = getenv("IPS3_CAPTURE")) {
    std::string dir = folder;
    std::thread([dir] {
      for (int n = 0; g_gpu.running; n++) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const std::string path = dir + "/frame_" + std::to_string(n) + ".png";
        uint64_t arms, triggers;
        ips3_watch_stats(&arms, &triggers);
        if (ips3_gpu_capture_frame(path.c_str())) {
          XELOGI("Ips3: captured {} (frame {}, watches armed {}, triggered {})", path,
                 g_gpu.swaps.load(), arms, triggers);
        }
      }
    }).detach();
  }
  return true;
}

void ips3_gpu_initialize_ring_buffer(uint32_t gpu_address, uint32_t size_log2) {
  g_gpu.graphics->InitializeRingBuffer(gpu_address, size_log2);
}

void ips3_gpu_enable_read_pointer_writeback(uint32_t gpu_address, uint32_t block_size_log2) {
  g_gpu.graphics->EnableReadPointerWriteBack(gpu_address, block_size_log2);
}

void ips3_gpu_set_interrupt_callback(uint32_t callback, uint32_t user_data) {
  g_gpu.graphics->SetInterruptCallback(callback, user_data);
}

// Xenia's VdSwap: the 64 dwords the caller reserved become the front buffer's texture fetch
// constant and an XE_SWAP packet, then type 2 no-ops.
void ips3_gpu_write_swap(uint32_t* buffer, const uint32_t* fetch, uint32_t width, uint32_t height) {
  namespace xenos = xe::gpu::xenos;

  xenos::xe_gpu_texture_fetch_t gpu_fetch;
  xe::copy_and_swap_32_unaligned(&gpu_fetch, fetch, 6);
  // The fetch constant in the Direct3D texture header holds a virtual address; the GPU wants
  // the physical one.
  const uint32_t frontbuffer = (gpu_fetch.base_address << 12) & 0x1FFFFFFF;
  gpu_fetch.base_address = frontbuffer >> 12;

  uint32_t dwords[64] = {};
  uint32_t offset = 0;
  dwords[offset++] = xenos::MakePacketType0(xe::gpu::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 6);
  dwords[offset++] = gpu_fetch.dword_0;
  dwords[offset++] = gpu_fetch.dword_1;
  dwords[offset++] = gpu_fetch.dword_2;
  dwords[offset++] = gpu_fetch.dword_3;
  dwords[offset++] = gpu_fetch.dword_4;
  dwords[offset++] = gpu_fetch.dword_5;
  dwords[offset++] = xenos::MakePacketType3(xenos::PM4_XE_SWAP, 4);
  dwords[offset++] = xenos::kSwapSignature;
  dwords[offset++] = frontbuffer;
  dwords[offset++] = width;
  dwords[offset++] = height;
  while (offset < 64) {
    dwords[offset++] = xenos::MakePacketType2();
  }
  for (uint32_t i = 0; i < 64; i++) {
    buffer[i] = xe::byte_swap(dwords[i]);
  }
  g_gpu.swaps++;
}

uint64_t ips3_gpu_swap_count() { return g_gpu.swaps; }

namespace {

// Minimal PNG writer: RGBA8, stored (uncompressed) deflate blocks.
uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t n = 0; n < 256; n++) {
      uint32_t c = n;
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
      table[n] = c;
    }
    ready = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < size; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void PutBE32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}

void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  PutBE32(out, uint32_t(data.size()));
  const size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  PutBE32(out, Crc32(out.data() + start, out.size() - start));
}

bool WritePng(const char* path, uint32_t width, uint32_t height, uint32_t stride, const uint8_t* rgba) {
  std::vector<uint8_t> raw;
  raw.reserve(size_t(height) * (width * 4 + 1));
  for (uint32_t y = 0; y < height; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba + size_t(y) * stride, rgba + size_t(y) * stride + width * 4);
  }

  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t pos = 0; pos < raw.size() || pos == 0;) {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    z.push_back(pos + n == raw.size() ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
    if (n == 0) break;
  }
  PutBE32(z, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  PutBE32(ihdr, width);
  PutBE32(ihdr, height);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", z);
  Chunk(png, "IEND", {});

  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(png.data()), png.size());
  return bool(file);
}

}  // namespace

namespace {
// Reads the guest output back on the GPU command thread: the read-back submits to the same
// Vulkan queue as the command processor, and submitting from two threads stalls some drivers.
bool CaptureOnGpuThread(xe::ui::RawImage& image) {
  xe::ui::Presenter* presenter = g_gpu.graphics ? g_gpu.graphics->presenter() : nullptr;
  xe::gpu::CommandProcessor* cp = g_gpu.graphics ? g_gpu.graphics->command_processor() : nullptr;
  if (!presenter || !cp) {
    return false;
  }
  struct Request {
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    bool ok = false;
    xe::ui::RawImage image;
  };
  auto request = std::make_shared<Request>();
  cp->CallInThread([request, presenter]() {
    const bool ok = presenter->CaptureGuestOutput(request->image);
    std::lock_guard lock(request->mutex);
    request->ok = ok;
    request->finished = true;
    request->done.notify_all();
  });
  std::unique_lock lock(request->mutex);
  if (!request->done.wait_for(lock, std::chrono::seconds(2), [&] { return request->finished; }) ||
      !request->ok) {
    return false;
  }
  image = std::move(request->image);
  return true;
}
}  // namespace

bool ips3_gpu_grab_frame(std::vector<uint8_t>& rgbx, uint32_t& width, uint32_t& height) {
  xe::ui::RawImage image;
  if (!CaptureOnGpuThread(image) || !image.width || !image.height) {
    return false;
  }
  width = image.width;
  height = image.height;
  const size_t row = size_t(width) * 4;
  rgbx.resize(row * height);
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(rgbx.data() + row * y, image.data.data() + image.stride * y, row);
  }
  return true;
}

bool ips3_gpu_capture_frame(const char* path) {
  xe::ui::RawImage image;
  if (!CaptureOnGpuThread(image)) {
    return false;
  }
  return WritePng(path, image.width, image.height, uint32_t(image.stride), image.data.data());
}
