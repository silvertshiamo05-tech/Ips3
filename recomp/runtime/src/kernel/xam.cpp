#include <stdafx.h>
#include "xam.h"
#include "xdm.h"
#include <hid/hid.h>
#include <cpu/guest_thread.h>
#include <ranges>
#include <unordered_set>
#include "xxHashMap.h"
#include <user/paths.h>

struct XamListener : KernelObject
{
    uint32_t id{};
    uint64_t areas{};
    std::vector<std::tuple<uint32_t, uint32_t>> notifications;

    XamListener(const XamListener&) = delete;
    XamListener& operator=(const XamListener&) = delete;

    XamListener();
    ~XamListener();
};

struct XamEnumeratorBase : KernelObject
{
    virtual uint32_t Next(void* buffer)
    {
        return -1;
    }
};

template<typename TIterator = std::vector<XHOSTCONTENT_DATA>::iterator>
struct XamEnumerator : XamEnumeratorBase
{
    uint32_t fetch;
    size_t size;
    TIterator position;
    TIterator begin;
    TIterator end;

    XamEnumerator() = default;
    XamEnumerator(uint32_t fetch, size_t size, TIterator begin, TIterator end) : fetch(fetch), size(size), position(begin), begin(begin), end(end)
    {

    }

    uint32_t Next(void* buffer) override
    {
        if (position == end)
        {
            return -1;
        }

        if (buffer == nullptr)
        {
            for (size_t i = 0; i < fetch; i++)
            {
                if (position == end)
                {
                    return i == 0 ? -1 : i;
                }

                ++position;
            }
        }

        for (size_t i = 0; i < fetch; i++)
        {
            if (position == end)
            {
                return i == 0 ? -1 : i;
            }

            memcpy(buffer, &*position, size);

            ++position;
            buffer = (void*)((size_t)buffer + size);
        }

        return fetch;
    }
};

std::array<xxHashMap<XHOSTCONTENT_DATA>, 3> gContentRegistry{};
std::unordered_set<XamListener*> gListeners{};
xxHashMap<std::string> gRootMap;

std::string_view XamGetRootPath(const std::string_view& root)
{
    const auto result = gRootMap.find(StringHash(root));

    if (result == gRootMap.end())
        return "";

    return result->second;
}

void XamRootCreate(const std::string_view& root, const std::string_view& path)
{
    gRootMap.emplace(StringHash(root), path);
}

XamListener::XamListener()
{
    gListeners.insert(this);
}

XamListener::~XamListener()
{
    gListeners.erase(this);
}

XCONTENT_DATA XamMakeContent(uint32_t type, const std::string_view& name)
{
    XCONTENT_DATA data{ 1, type };

    strncpy(data.szFileName, name.data(), sizeof(data.szFileName));

    return data;
}

void XamRegisterContent(const XCONTENT_DATA& data, const std::string_view& root)
{
    const auto idx = data.dwContentType - 1;

    gContentRegistry[idx].emplace(StringHash(data.szFileName), XHOSTCONTENT_DATA{ data }).first->second.szRoot = root;
}

void XamRegisterContent(uint32_t type, const std::string_view name, const std::string_view& root)
{
    XCONTENT_DATA data{ 1, type, {}, "" };

    strncpy(data.szFileName, name.data(), sizeof(data.szFileName));

    XamRegisterContent(data, root);
}

uint32_t XamNotifyCreateListener(uint64_t qwAreas)
{
    auto* listener = CreateKernelObject<XamListener>();

    listener->areas = qwAreas;

    // Ips3: as on the console (and in Xenia), the first listener for system notifications gets
    // the start-up sequence; titles wait for the sign-in change before leaving their boot flow.
    static bool notifiedStartup = false;
    if (!notifiedStartup && (qwAreas & 1) != 0)
    {
        notifiedStartup = true;
        listener->notifications.emplace_back(9, 1);  // XN_SYS_UI: shown
        listener->notifications.emplace_back(9, 0);  // XN_SYS_UI: hidden
        listener->notifications.emplace_back(10, 1); // XN_SYS_SIGNINCHANGED: user 0
        listener->notifications.emplace_back(10, 1);
    }

    return GetKernelHandle(listener);
}

void XamNotifyEnqueueEvent(uint32_t dwId, uint32_t dwParam)
{
    for (const auto& listener : gListeners)
    {
        if (((1 << MSG_AREA(dwId)) & listener->areas) == 0)
            continue;

        listener->notifications.emplace_back(dwId, dwParam);
    }
}

struct Event;
uint32_t NtSetEvent(Event* handle, uint32_t* previousState);

uint32_t XamCompleteOverlapped(XXOVERLAPPED* overlapped, uint32_t result, uint32_t length)
{
    if (overlapped == nullptr)
        return result;

    overlapped->Error = result;
    overlapped->Length = length;
    overlapped->dwExtendedError = result;
    if (overlapped->hEvent != 0)
        NtSetEvent(GetKernelObject<Event>(overlapped->hEvent), nullptr);

    return 997; // ERROR_IO_PENDING
}

bool XNotifyGetNext(uint32_t hNotification, uint32_t dwMsgFilter, be<uint32_t>* pdwId, be<uint32_t>* pParam)
{
    auto& listener = *GetKernelObject<XamListener>(hNotification);

    if (dwMsgFilter)
    {
        for (size_t i = 0; i < listener.notifications.size(); i++)
        {
            if (std::get<0>(listener.notifications[i]) == dwMsgFilter)
            {
                if (pdwId)
                    *pdwId = std::get<0>(listener.notifications[i]);

                if (pParam)
                    *pParam = std::get<1>(listener.notifications[i]);

                listener.notifications.erase(listener.notifications.begin() + i);

                return true;
            }
        }
    }
    else
    {
        if (listener.notifications.empty())
            return false;

        if (pdwId)
            *pdwId = std::get<0>(listener.notifications[0]);

        if (pParam)
            *pParam = std::get<1>(listener.notifications[0]);

        listener.notifications.erase(listener.notifications.begin());

        return true;
    }

    return false;
}

uint32_t XamShowMessageBoxUI(uint32_t dwUserIndex, be<uint16_t>* wszTitle, be<uint16_t>* wszText, uint32_t cButtons,
    xpointer<be<uint16_t>>* pwszButtons, uint32_t dwFocusButton, uint32_t dwFlags, be<uint32_t>* pResult, XXOVERLAPPED* pOverlapped)
{
    *pResult = cButtons ? cButtons - 1 : 0;

#if _DEBUG
    assert("XamShowMessageBoxUI encountered!" && false);
#elif _WIN32
    // This code is Win32-only as it'll most likely crash, misbehave or
    // cause corruption due to using a different type of memory than what
    // wchar_t is on Linux. Windows uses 2 bytes while Linux uses 4 bytes.
    std::vector<std::wstring> texts{};

    texts.emplace_back(reinterpret_cast<wchar_t*>(wszTitle));
    texts.emplace_back(reinterpret_cast<wchar_t*>(wszText));

    for (size_t i = 0; i < cButtons; i++)
        texts.emplace_back(reinterpret_cast<wchar_t*>(pwszButtons[i].get()));

    for (auto& text : texts)
    {
        for (size_t i = 0; i < text.size(); i++)
            ByteSwapInplace(text[i]);
    }

    wprintf(L"[XamShowMessageBoxUI] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    wprintf(L"[XamShowMessageBoxUI] If you are encountering this message and the game has ceased functioning,\n");
    wprintf(L"[XamShowMessageBoxUI] please create an issue at https://github.com/hedge-dev/UnleashedRecomp/issues.\n");
    wprintf(L"[XamShowMessageBoxUI] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    wprintf(L"[XamShowMessageBoxUI] %ls\n", texts[0].c_str());
    wprintf(L"[XamShowMessageBoxUI] %ls\n", texts[1].c_str());
    wprintf(L"[XamShowMessageBoxUI] ");

    for (size_t i = 0; i < cButtons; i++)
    {
        wprintf(L"%ls", texts[2 + i].c_str());

        if (i != cButtons - 1)
            wprintf(L" | ");
    }

    wprintf(L"\n");
    wprintf(L"[XamShowMessageBoxUI] Defaulted to button: %d\n", pResult->get());
    wprintf(L"[XamShowMessageBoxUI] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
#endif

    XamNotifyEnqueueEvent(9, 0);

    return XamCompleteOverlapped(pOverlapped, 0);
}

uint32_t XamContentCreateEnumerator(uint32_t dwUserIndex, uint32_t DeviceID, uint32_t dwContentType,
    uint32_t dwContentFlags, uint32_t cItem, be<uint32_t>* pcbBuffer, be<uint32_t>* phEnum)
{
    if (dwUserIndex != 0)
    {
        GuestThread::SetLastError(ERROR_NO_SUCH_USER);
        return 0xFFFFFFFF;
    }

    const auto& registry = gContentRegistry[dwContentType - 1];
    const auto& values = registry | std::views::values;
    auto* enumerator = CreateKernelObject<XamEnumerator<decltype(values.begin())>>(cItem, sizeof(_XCONTENT_DATA), values.begin(), values.end());

    if (pcbBuffer)
        *pcbBuffer = sizeof(_XCONTENT_DATA) * cItem;

    *phEnum = GetKernelHandle(enumerator);

    return 0;
}

uint32_t XamEnumerate(uint32_t hEnum, uint32_t dwFlags, void* pvBuffer, uint32_t cbBuffer, be<uint32_t>* pcItemsReturned, XXOVERLAPPED* pOverlapped)
{
    auto* enumerator = GetKernelObject<XamEnumeratorBase>(hEnum);
    const auto count = enumerator->Next(pvBuffer);

    if (count == -1)
        return XamCompleteOverlapped(pOverlapped, ERROR_NO_MORE_FILES);

    if (pcItemsReturned)
        *pcItemsReturned = count;

    return XamCompleteOverlapped(pOverlapped, ERROR_SUCCESS, count);
}

static uint32_t ContentCreate(const char* szRootName, const XCONTENT_DATA* pContentData,
    uint32_t dwContentFlags, be<uint32_t>* pdwDisposition)
{
    const auto& registry = gContentRegistry[pContentData->dwContentType - 1];
    const auto exists = registry.contains(StringHash(pContentData->szFileName));
    const auto mode = dwContentFlags & 0xF;

    if (mode == CREATE_ALWAYS)
    {
        if (pdwDisposition)
            *pdwDisposition = XCONTENT_NEW;

        if (!exists)
        {
            std::filesystem::path rootPath;

            if (pContentData->dwContentType == XCONTENTTYPE_SAVEDATA)
            {
                rootPath = GetSavePath(true);
            }
            else if (pContentData->dwContentType == XCONTENTTYPE_DLC)
            {
                rootPath = GetGamePath() / "dlc";
            }
            else
            {
                rootPath = GetGamePath();
            }

            const std::string root = (const char*)rootPath.u8string().c_str();
            XamRegisterContent(*pContentData, root);

            std::error_code ec;
            std::filesystem::create_directory(rootPath, ec);

            XamRootCreate(szRootName, root);
        }
        else
        {
            XamRootCreate(szRootName, registry.find(StringHash(pContentData->szFileName))->second.szRoot);
        }

        return ERROR_SUCCESS;
    }

    if (mode == OPEN_EXISTING)
    {
        if (exists)
        {
            if (pdwDisposition)
                *pdwDisposition = XCONTENT_EXISTING;

            XamRootCreate(szRootName, registry.find(StringHash(pContentData->szFileName))->second.szRoot);

            return ERROR_SUCCESS;
        }
        else
        {
            if (pdwDisposition)
                *pdwDisposition = XCONTENT_NEW;

            return ERROR_PATH_NOT_FOUND;
        }
    }

    return ERROR_PATH_NOT_FOUND;
}

uint32_t XamContentCreateEx(uint32_t dwUserIndex, const char* szRootName, const XCONTENT_DATA* pContentData,
    uint32_t dwContentFlags, be<uint32_t>* pdwDisposition, be<uint32_t>* pdwLicenseMask,
    uint32_t dwFileCacheSize, uint64_t uliContentSize, PXXOVERLAPPED pOverlapped)
{
    return XamCompleteOverlapped(pOverlapped, ContentCreate(szRootName, pContentData, dwContentFlags, pdwDisposition));
}

uint32_t XamContentClose(const char* szRootName, XXOVERLAPPED* pOverlapped)
{
    gRootMap.erase(StringHash(szRootName));
    return XamCompleteOverlapped(pOverlapped, 0);
}

uint32_t XamContentGetDeviceData(uint32_t DeviceID, XDEVICE_DATA* pDeviceData)
{
    pDeviceData->DeviceID = DeviceID;
    pDeviceData->DeviceType = XCONTENTDEVICETYPE_HDD;
    pDeviceData->ulDeviceBytes = 0x10000000;
    pDeviceData->ulDeviceFreeBytes = 0x10000000;
    pDeviceData->wszName[0] = 'S';
    pDeviceData->wszName[1] = 'o';
    pDeviceData->wszName[2] = 'n';
    pDeviceData->wszName[3] = 'i';
    pDeviceData->wszName[4] = 'c';
    pDeviceData->wszName[5] = '\0';

    return 0;
}

// Ips3: Bayonetta calls the three-argument form (user, flags, caps); UnleashedRecomp's had an
// extra leading argument, which read user 0 as user 1 ("not connected").
uint32_t XamInputGetCapabilities(uint32_t userIndex, uint32_t flags, XAMINPUT_CAPABILITIES* caps)
{
    uint32_t result = hid::GetCapabilities(userIndex, caps);

    if (result == ERROR_SUCCESS)
    {
        ByteSwapInplace(caps->Flags);
        ByteSwapInplace(caps->Gamepad.wButtons);
        ByteSwapInplace(caps->Gamepad.sThumbLX);
        ByteSwapInplace(caps->Gamepad.sThumbLY);
        ByteSwapInplace(caps->Gamepad.sThumbRX);
        ByteSwapInplace(caps->Gamepad.sThumbRY);
        ByteSwapInplace(caps->Vibration.wLeftMotorSpeed);
        ByteSwapInplace(caps->Vibration.wRightMotorSpeed);
    }

    return result;
}

uint32_t XamInputGetState(uint32_t userIndex, uint32_t flags, XAMINPUT_STATE* state)
{
    memset(state, 0, sizeof(*state));

    // Ips3: report unplugged pads as such, or the game sees four controllers.
    const uint32_t result = hid::GetState(userIndex, state);
    if (result != ERROR_SUCCESS)
        return result;

    if (!hid::IsInputAllowed())
        memset(&state->Gamepad, 0, sizeof(state->Gamepad));

    if (state->Gamepad.wButtons != 0 && getenv("IPS3_TRACE_INPUT"))
        fmt::println(stderr, "[input] GetState user {} buttons {:04X} packet {}", userIndex, uint16_t(state->Gamepad.wButtons), uint32_t(state->dwPacketNumber));

    state->Gamepad.wButtons &= ~hid::g_prohibitedButtons;

    if (hid::g_isLeftStickProhibited)
    {
        state->Gamepad.sThumbLX = 0;
        state->Gamepad.sThumbLY = 0;
    }

    if (hid::g_isRightStickProhibited)
    {
        state->Gamepad.sThumbRX = 0;
        state->Gamepad.sThumbRY = 0;
    }

    ByteSwapInplace(state->Gamepad.wButtons);
    ByteSwapInplace(state->Gamepad.sThumbLX);
    ByteSwapInplace(state->Gamepad.sThumbLY);
    ByteSwapInplace(state->Gamepad.sThumbRX);
    ByteSwapInplace(state->Gamepad.sThumbRY);

    return ERROR_SUCCESS;
}

uint32_t XamInputSetState(uint32_t userIndex, uint32_t flags, XAMINPUT_VIBRATION* vibration)
{
    if (!hid::IsInputDeviceController() || !Config::Vibration)
        return ERROR_SUCCESS;

    ByteSwapInplace(vibration->wLeftMotorSpeed);
    ByteSwapInplace(vibration->wRightMotorSpeed);

    return hid::SetState(userIndex, vibration);
}

#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>

// ---------------------------------------------------------------------------------------------
// Ips3: XAM imports Bayonetta uses that Sonic Unleashed does not. One offline profile in slot 0,
// no Xbox Live.
// ---------------------------------------------------------------------------------------------

namespace
{
	constexpr uint32_t ERROR_NO_SUCH_USER_ = 0x525;
	constexpr uint32_t ERROR_NOT_FOUND_ = 0x490;
	constexpr uint32_t ERROR_EMPTY_ = 0x10D2;
	constexpr uint64_t OFFLINE_XUID = XAM_OFFLINE_XUID;

	// An enumerator that reports no items: achievements and stats live on Xbox Live.
	struct EmptyEnumerator : XamEnumeratorBase
	{
		uint32_t Next(void*) override { return -1; }
	};
}

static uint32_t XamAlloc(uint32_t flags, uint32_t size, be<uint32_t>* out)
{
	void* ptr = g_userHeap.Alloc(size);
	if (!ptr)
		return 0x8007000E; // E_OUTOFMEMORY

	memset(ptr, 0, size);
	*out = g_memory.MapVirtual(ptr);
	return 0;
}

static uint32_t XamFree(uint32_t ptr)
{
	if (ptr)
		g_userHeap.Free(g_memory.Translate(ptr));
	return 0;
}

// XINPUT_KEYSTROKE: key events made from pad state changes (no chatpad or keyboard).
struct XINPUT_KEYSTROKE_
{
	be<uint16_t> VirtualKey;
	be<uint16_t> Unicode;
	be<uint16_t> Flags;
	uint8_t UserIndex;
	uint8_t HidCode;
};

static uint32_t XamInputGetKeystrokeEx(be<uint32_t>* userIndex, uint32_t flags, XINPUT_KEYSTROKE_* keystroke)
{
	const uint32_t user = userIndex ? uint32_t(*userIndex) : 0xFF;
	if (user != 0xFF && user != 0)
		return 1167; // ERROR_DEVICE_NOT_CONNECTED

	hid::Keystroke key;
	if (!hid::GetKeystroke(user, key))
	{
		memset(keystroke, 0, sizeof(*keystroke));
		return ERROR_EMPTY_;
	}

	if (getenv("IPS3_TRACE_INPUT"))
		fmt::println(stderr, "[input] keystroke {:04X} flags {}", key.virtualKey, key.flags);

	keystroke->VirtualKey = key.virtualKey;
	keystroke->Unicode = key.unicode;
	keystroke->Flags = key.flags;
	keystroke->UserIndex = key.userIndex;
	keystroke->HidCode = key.hidCode;
	if (userIndex)
		*userIndex = key.userIndex;
	return 0;
}

static uint32_t XamLoaderGetLaunchDataSize(be<uint32_t>* size)
{
	*size = 0;
	return ERROR_NOT_FOUND_;
}

static uint32_t XamLoaderGetLaunchData(void* buffer, uint32_t size)
{
	return ERROR_NOT_FOUND_;
}

static uint32_t XamLoaderSetLaunchData(void* data, uint32_t size)
{
	return 0;
}

static uint32_t XamSessionCreateHandle(be<uint32_t>* handle)
{
	LOG_UTILITY("multiplayer sessions are not supported");
	*handle = GetKernelHandle(CreateKernelObject<EmptyEnumerator>());
	return 0;
}

static uint32_t XamSessionRefObjByHandle(uint32_t handle, be<uint32_t>* object)
{
	LOG_UTILITY("multiplayer sessions are not supported");
	*object = 0;
	return 0xC0000008; // STATUS_INVALID_HANDLE
}

static uint32_t XamShowGamerCardUIForXUID(uint32_t userIndex, uint64_t xuid, XXOVERLAPPED* overlapped)
{
	return XamCompleteOverlapped(overlapped, 0);
}

static uint32_t XamUserCreateAchievementEnumerator(uint32_t titleId, uint32_t userIndex, uint64_t xuid, uint32_t flags,
	uint32_t offset, uint32_t count, be<uint32_t>* bufferSize, be<uint32_t>* handle)
{
	if (bufferSize)
		*bufferSize = 0;
	*handle = GetKernelHandle(CreateKernelObject<EmptyEnumerator>());
	return 0;
}

// The stats enumerator takes its arguments partly on the stack; none of them matter for an empty
// result, so only the two output pointers (r9, r10) are read.
PPC_FUNC(__imp__XamUserCreateStatsEnumerator)
{
	auto* bufferSize = reinterpret_cast<be<uint32_t>*>(ctx.r9.u32 ? base + ctx.r9.u32 : nullptr);
	auto* handle = reinterpret_cast<be<uint32_t>*>(base + ctx.r10.u32);

	if (bufferSize)
		*bufferSize = 0;
	if (ctx.r10.u32)
		*handle = GetKernelHandle(CreateKernelObject<EmptyEnumerator>());

	ctx.r3.u64 = 0;
}

static uint32_t XamUserGetName(uint32_t userIndex, char* buffer, uint32_t size)
{
	if (userIndex != 0)
		return ERROR_NO_SUCH_USER_;

	if (buffer && size)
	{
		strncpy(buffer, "Player", size);
		buffer[size - 1] = '\0';
	}

	return 0;
}

// Two forms exist across XDK versions: (index, &xuid) and (index, typeMask, &xuid). Tell them
// apart by whether r4 looks like a guest pointer.
PPC_FUNC(__imp__XamUserGetXUID)
{
	if (ctx.r3.u32 != 0)
	{
		ctx.r3.u64 = ERROR_NO_SUCH_USER_;
		return;
	}

	const uint32_t out = ctx.r4.u32 >= 0x10000 ? ctx.r4.u32 : ctx.r5.u32;
	if (out)
		*reinterpret_cast<be<uint64_t>*>(base + out) = OFFLINE_XUID;

	ctx.r3.u64 = 0;
}

GUEST_FUNCTION_HOOK(__imp__XamAlloc, XamAlloc);
GUEST_FUNCTION_HOOK(__imp__XamFree, XamFree);
GUEST_FUNCTION_HOOK(__imp__XamInputGetKeystrokeEx, XamInputGetKeystrokeEx);
GUEST_FUNCTION_HOOK(__imp__XamLoaderGetLaunchDataSize, XamLoaderGetLaunchDataSize);
GUEST_FUNCTION_HOOK(__imp__XamLoaderGetLaunchData, XamLoaderGetLaunchData);
GUEST_FUNCTION_HOOK(__imp__XamLoaderSetLaunchData, XamLoaderSetLaunchData);
GUEST_FUNCTION_HOOK(__imp__XamSessionCreateHandle, XamSessionCreateHandle);
GUEST_FUNCTION_HOOK(__imp__XamSessionRefObjByHandle, XamSessionRefObjByHandle);
GUEST_FUNCTION_HOOK(__imp__XamShowGamerCardUIForXUID, XamShowGamerCardUIForXUID);
GUEST_FUNCTION_HOOK(__imp__XamUserCreateAchievementEnumerator, XamUserCreateAchievementEnumerator);
GUEST_FUNCTION_HOOK(__imp__XamUserGetName, XamUserGetName);
