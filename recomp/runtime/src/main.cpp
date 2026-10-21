// Bayonetta native runtime entry point.
//
// Startup sequence adapted from UnleashedRecomp's main.cpp (GPL-3.0): reserve guest memory,
// load the executable into it, mount the game data as "game:", start the main guest thread at
// the XEX entry point.
//
//   bayonetta <game> [save directory] [--xex default.xex]
//
// <game> is the disc image (.iso) or a folder with the extracted disc. default.xex is read from
// it unless --xex names another copy.

#include <host/host.h>
#include <stdafx.h>

#include <cpu/guest_thread.h>
#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <kernel/module.h>
#include <kernel/xam.h>
#include <kernel/xdm.h>
#include <kernel/io/disc_image.h>
#include <ips3_gpu.h>

#include <image.h>

#include <unistd.h>

// Defined by the host program in UnleashedRecomp as well.
Memory g_memory;
Heap g_userHeap;

static std::filesystem::path g_gamePath;
static std::filesystem::path g_savePath;

const std::filesystem::path& GetGamePath()
{
    return g_gamePath;
}

std::filesystem::path GetSavePath(bool)
{
    return g_savePath;
}

// Loads the XEX through XenonRecomp's loader, which decrypts and decompresses it (the retail
// Bayonetta executable is both encrypted and LZX-compressed), and copies the image into guest
// memory at its load address. Returns the entry point.
static uint32_t LoadExecutable(const std::vector<uint8_t>& xex, const std::string& name)
{
    Image image = Image::ParseImage(xex.data(), xex.size());

    if (!image.data || image.base != PPC_IMAGE_BASE || image.size > PPC_IMAGE_SIZE)
    {
        fmt::println(stderr, "{} is not the executable this build was recompiled from", name);
        return 0;
    }

    memcpy(g_memory.Translate(image.base), image.data.get(), image.size);
    fmt::println("Loaded {} bytes at {:08X}, entry {:08X}", image.size, image.base, image.entry_point);

    Module::Init(xex.data(), xex.size(), image);

    return uint32_t(image.entry_point);
}

// default.xex from --xex, the disc image, or the game folder.
static std::vector<uint8_t> ReadExecutable(const std::filesystem::path& override)
{
    std::vector<uint8_t> data;

    if (!override.empty())
    {
        std::ifstream file(override, std::ios::binary);
        if (file)
            data.assign(std::istreambuf_iterator<char>(file), {});
        return data;
    }

    const DiscImage::Entry* entry = nullptr;
    if (auto disc = DiscImage::Lookup(g_gamePath / "default.xex", &entry); disc != nullptr)
    {
        if (entry != nullptr && !entry->directory)
        {
            data.resize(entry->size);
            if (pread(disc->fd, data.data(), data.size(), off_t(entry->offset)) != ssize_t(data.size()))
                data.clear();
        }
        return data;
    }

    for (const auto& item : std::filesystem::directory_iterator(g_gamePath))
    {
        if (strcasecmp((const char*)item.path().filename().u8string().c_str(), "default.xex") == 0)
        {
            std::ifstream file(item.path(), std::ios::binary);
            data.assign(std::istreambuf_iterator<char>(file), {});
            break;
        }
    }

    return data;
}

static void MountGame()
{

    const auto gameContent = XamMakeContent(XCONTENTTYPE_RESERVED, "Game");
    XamRegisterContent(gameContent, g_gamePath.string());

    // Xbox 360 titles reach their disc as "game:" and, in some code paths, "D:".
    XamContentCreateEx(0, "game", &gameContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);
    XamContentCreateEx(0, "D", &gameContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);

    // The hard drive ("\\Device\\Harddisk0\\Partition1") and its cache partition live under the
    // save directory.
    for (const char* root : { "hdd", "cache" })
    {
        const auto path = g_savePath / root;
        std::filesystem::create_directories(path);
        XamRootCreate(root, path.string());
    }
}

int main(int argc, char** argv)
{
    std::vector<std::string> args;
    std::filesystem::path xexPath;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--xex") == 0 && i + 1 < argc)
            xexPath = argv[++i];
        else
            args.emplace_back(argv[i]);
    }

    if (args.empty())
    {
        fmt::println(stderr, "usage: {} <game.iso | game folder> [save directory] [--xex default.xex]", argv[0]);
        return 1;
    }

    if (g_memory.base == nullptr)
    {
        fmt::println(stderr, "Could not reserve 4 GiB of guest memory");
        return 1;
    }

    g_gamePath = std::filesystem::absolute(args[0]);
    g_savePath = args.size() > 1 ? std::filesystem::path(args[1]) : g_gamePath.parent_path() / "save";
    std::filesystem::create_directories(g_savePath);

    if (std::filesystem::is_regular_file(g_gamePath))
    {
        auto disc = DiscImage::Open(g_gamePath);
        if (disc == nullptr)
        {
            fmt::println(stderr, "{} is not an Xbox 360 disc image", g_gamePath.string());
            return 1;
        }
        DiscImage::Mount(std::move(disc));
    }

    // Guest allocations (the loader's included) come from the heaps.
    g_userHeap.Init();
    host::Init();

    if (!ips3_gpu_start(g_memory.base))
    {
        fmt::println(stderr, "Could not start the Vulkan GPU");
        return 1;
    }

    const auto xex = ReadExecutable(xexPath);
    if (xex.empty())
    {
        fmt::println(stderr, "default.xex not found");
        return 1;
    }

    const uint32_t entry = LoadExecutable(xex, xexPath.empty() ? "default.xex" : xexPath.string());
    if (!entry)
    {
        return 1;
    }

    MountGame();

    fmt::println("Starting guest thread at {:08X}", entry);
    GuestThread::Start({ entry, 0, 0 });
    return 0;
}
