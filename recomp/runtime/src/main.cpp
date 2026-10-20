// Bayonetta native runtime entry point.
//
// Startup sequence adapted from UnleashedRecomp's main.cpp (GPL-3.0): reserve guest memory,
// load the executable into it, mount the game data as "game:", start the main guest thread at
// the XEX entry point.
//
//   bayonetta <path/to/default.xex> <game data directory>

#include <stdafx.h>

#include <cpu/guest_thread.h>
#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <kernel/xam.h>
#include <kernel/xdm.h>

#include <image.h>

static std::filesystem::path g_gamePath;

const std::filesystem::path& GetGamePath()
{
    return g_gamePath;
}

// Loads the XEX through XenonRecomp's loader, which decrypts and decompresses it (the retail
// Bayonetta executable is both encrypted and LZX-compressed), and copies the image into guest
// memory at its load address. Returns the entry point.
static uint32_t LoadExecutable(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        fmt::println(stderr, "Cannot open {}", path.string());
        return 0;
    }

    std::vector<uint8_t> xex((std::istreambuf_iterator<char>(file)), {});
    Image image = Image::ParseImage(xex.data(), xex.size());

    if (!image.data || image.base != PPC_IMAGE_BASE || image.size > PPC_IMAGE_SIZE)
    {
        fmt::println(stderr, "{} is not the executable this build was recompiled from", path.string());
        return 0;
    }

    memcpy(g_memory.Translate(image.base), image.data.get(), image.size);
    fmt::println("Loaded {} bytes at {:08X}, entry {:08X}", image.size, image.base, image.entry_point);

    return uint32_t(image.entry_point);
}

static void MountGame()
{
    g_userHeap.Init();

    const auto gameContent = XamMakeContent(XCONTENTTYPE_RESERVED, "Game");
    XamRegisterContent(gameContent, g_gamePath.string());

    // Xbox 360 titles reach their disc as "game:" and, in some code paths, "D:".
    XamContentCreateEx(0, "game", &gameContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);
    XamContentCreateEx(0, "D", &gameContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fmt::println(stderr, "usage: {} <default.xex> <game data directory>", argv[0]);
        return 1;
    }

    if (g_memory.base == nullptr)
    {
        fmt::println(stderr, "Could not reserve 4 GiB of guest memory");
        return 1;
    }

    g_gamePath = argv[2];

    const uint32_t entry = LoadExecutable(argv[1]);
    if (!entry)
    {
        return 1;
    }

    MountGame();

    fmt::println("Starting guest thread at {:08X}", entry);
    GuestThread::Start({ entry, 0, 0 });
    return 0;
}
