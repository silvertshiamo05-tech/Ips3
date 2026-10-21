#include <stdafx.h>
#include "memory.h"
#include <ips3_gpu.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/vm_map.h>
#endif

// Ips3: physical memory and its views.
//
// The console's 512 MiB of physical memory is visible through several windows at once:
// 0xA0000000 (64 KiB pages), 0xC0000000 (16 MiB pages), 0xE0000000 (4 KiB pages, shifted: guest
// 0xE0000000 is physical 0x1000) and the GPU write-back range at 0x7F000000. Direct3D converts
// GPU addresses into any of them, so they must share the same pages (Xenia's memory map).
// 0xA0000000 is the primary view (the physical heap allocates there); the others alias it.
struct PhysicalView
{
    uint32_t guest;
    uint32_t physical;
    uint32_t size;
};

static bool MapPhysicalViews(uint8_t* base)
{
    // Built here, not as a global: guest memory is set up during static initialisation.
    const PhysicalView PHYSICAL_ALIASES[] =
    {
        { 0xC0000000, 0x00000000, 0x20000000 },
        { 0xE0000000, ips3_e_view_shift(), 0x20000000 - ips3_e_view_shift() },
        // Up to the GPU register window at 0x7FC80000, which stays separate memory.
        { 0x7F000000, 0x00000000, 0x00C80000 },
    };

    constexpr uint32_t PHYSICAL_BASE = 0xA0000000;
    constexpr size_t PHYSICAL_SIZE = 0x20000000;

#ifdef __APPLE__
    for (const auto& view : PHYSICAL_ALIASES)
    {
        // vm_remap, not mach_vm_remap: iOS's SDK has no mach_vm.h (the types are 64-bit either way).
        vm_address_t target = vm_address_t(base + view.guest);
        vm_prot_t current, maximum;
        const kern_return_t kr = vm_remap(mach_task_self(), &target, view.size, 0,
            VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
            vm_address_t(base + PHYSICAL_BASE + view.physical), FALSE, &current, &maximum, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS)
        {
            fprintf(stderr, "[memory] cannot alias physical memory at %08X (%d)\n", view.guest, kr);
            return false;
        }
    }
    return true;
#else
    const int fd = memfd_create("ips3-physical", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, PHYSICAL_SIZE) != 0)
    {
        perror("[memory] memfd_create");
        return false;
    }

    auto map = [&](uint32_t guest, uint32_t physical, size_t size)
    {
        void* host = mmap(base + guest, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, physical);
        if (host == MAP_FAILED)
        {
            fprintf(stderr, "[memory] cannot map physical memory at %08X\n", guest);
            return false;
        }
        return true;
    };

    bool ok = map(PHYSICAL_BASE, 0, PHYSICAL_SIZE);
    for (const auto& view : PHYSICAL_ALIASES)
        ok = ok && map(view.guest, view.physical, view.size);
    close(fd);
    return ok;
#endif
}

Memory::Memory()
{
#ifdef _WIN32
    base = (uint8_t*)VirtualAlloc((void*)0x100000000ull, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (base == nullptr)
        base = (uint8_t*)VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (base == nullptr)
        return;

    DWORD oldProtect;
    VirtualProtect(base, 4096, PAGE_NOACCESS, &oldProtect);
#else
    base = (uint8_t*)mmap((void*)0x100000000ull, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (base == (uint8_t*)MAP_FAILED)
        base = (uint8_t*)mmap(NULL, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (base == nullptr)
        return;

    mprotect(base, size_t(sysconf(_SC_PAGESIZE)), PROT_NONE); // null guest pointers fault
#endif

    if (!MapPhysicalViews(base))
    {
        base = nullptr;
        return;
    }

    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        if (PPCFuncMappings[i].host != nullptr)
            InsertFunction(PPCFuncMappings[i].guest, PPCFuncMappings[i].host);
    }
}

void* MmGetHostAddress(uint32_t ptr)
{
    return g_memory.Translate(ptr);
}
