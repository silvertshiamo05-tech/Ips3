#include <stdafx.h>
#include "guest_thread.h"
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <kernel/function.h>
#include "ppc_context.h"

constexpr size_t PCR_SIZE = 0xAB0;
constexpr size_t TLS_SIZE = 0x100;
constexpr size_t TEB_SIZE = 0x2E0;
constexpr size_t STACK_SIZE = 0x40000;
constexpr size_t TOTAL_SIZE = PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE;

constexpr size_t TEB_OFFSET = PCR_SIZE + TLS_SIZE;

GuestThreadContext::GuestThreadContext(uint32_t cpuNumber)
{
    assert(thread == nullptr);

    thread = (uint8_t*)g_userHeap.Alloc(TOTAL_SIZE);
    memset(thread, 0, TOTAL_SIZE);

    *(uint32_t*)thread = ByteSwap(g_memory.MapVirtual(thread + PCR_SIZE)); // tls pointer
    *(uint32_t*)(thread + 0x100) = ByteSwap(g_memory.MapVirtual(thread + PCR_SIZE + TLS_SIZE)); // teb pointer
    *(thread + 0x10C) = cpuNumber;

    *(uint32_t*)(thread + PCR_SIZE + 0x10) = 0xFFFFFFFF; // that one TLS entry that felt quirky
    *(uint32_t*)(thread + PCR_SIZE + TLS_SIZE + 0x14C) = ByteSwap(GuestThread::GetCurrentThreadId()); // thread id

    ppcContext.r1.u64 = g_memory.MapVirtual(thread + PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE); // stack pointer
    ppcContext.r13.u64 = g_memory.MapVirtual(thread);
    ppcContext.fpscr.loadFromHost();

    assert(GetPPCContext() == nullptr);
    SetPPCContext(ppcContext);
}

GuestThreadContext::~GuestThreadContext()
{
    g_userHeap.Free(thread);
}

// The handle of the thread being started on this host thread, so Start can publish its PCR.
static thread_local GuestThreadHandle* g_startingThread = nullptr;

#ifdef USE_PTHREAD
static size_t GetStackSize()
{
    // Ips3: recompiled code nests as deeply as the guest's call chains, so every guest thread
    // gets 8 MiB, or more if the main thread's limit is higher. (RLIMIT_STACK describes the main
    // thread only; on iOS it is 1 MiB, far less than secondary threads may have.)
    static const size_t stackSize = []
    {
        constexpr size_t defaultSize = 8 * 1024 * 1024;
        struct rlimit lim;
        if (getrlimit(RLIMIT_STACK, &lim) == 0 && lim.rlim_cur != RLIM_INFINITY && lim.rlim_cur > defaultSize)
            return size_t(lim.rlim_cur);
        return defaultSize;
    }();
    return stackSize;
}

static void* GuestThreadFunc(void* arg)
{
    GuestThreadHandle* hThread = (GuestThreadHandle*)arg;
#else
static void GuestThreadFunc(GuestThreadHandle* hThread)
{
#endif
    hThread->suspended.wait(true);
    g_startingThread = hThread;
    GuestThread::Start(hThread->params);
    hThread->exited = true;
    NotifyKernelWaiters();
#ifdef USE_PTHREAD
    return nullptr;
#endif
}

GuestThreadHandle::GuestThreadHandle(const GuestThreadParams& params)
    : params(params), suspended((params.flags & 0x1) != 0)
#ifdef USE_PTHREAD
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, GetStackSize());
    const auto ret = pthread_create(&thread, &attr, GuestThreadFunc, this);
    if (ret != 0) {
        fprintf(stderr, "pthread_create failed with error code 0x%X.\n", ret);
        return;
    }
}
#else
      , thread(GuestThreadFunc, this)
{
}
#endif

GuestThreadHandle::~GuestThreadHandle()
{
#ifdef USE_PTHREAD
    pthread_join(thread, nullptr);
#else
    if (thread.joinable())
        thread.join();
#endif
}

template <typename ThreadType>
static uint32_t CalcThreadId(const ThreadType& id)
{
    if constexpr (sizeof(id) == 4)
        return *reinterpret_cast<const uint32_t*>(&id);
    else
        return XXH32(&id, sizeof(id), 0);
}

uint32_t GuestThreadHandle::GetThreadId() const
{
#ifdef USE_PTHREAD
    return CalcThreadId(thread);
#else
    return CalcThreadId(thread.get_id());
#endif
}

uint32_t GuestThreadHandle::Wait(uint32_t timeout)
{
    if (timeout != INFINITE)
    {
        if (exited)
            return STATUS_SUCCESS;
        if (timeout == 0)
            return STATUS_TIMEOUT;
        KernelObject* self = this;
        return WaitForKernelObjects(&self, 1, false, timeout);
    }

#ifdef USE_PTHREAD
    pthread_join(thread, nullptr);
#else
    if (thread.joinable())
        thread.join();
#endif

    return STATUS_WAIT_0;
}

#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>

// Ips3: Apple's chips have few fast cores and more slow ones, and the scheduler picks by quality
// of service. The game's main thread (the first, which builds the frames) gets the highest, the
// other guest threads the next; IPS3_FAST_CORES=0 leaves them all at the default.
static void SetGuestThreadQos()
{
    static std::atomic<int> started{ 0 };
    const bool first = started++ == 0;
    const char* fast = getenv("IPS3_FAST_CORES");
    if (fast && fast[0] == '0')
        return;
    pthread_set_qos_class_self_np(first ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
}
#endif

uint32_t GuestThread::Start(const GuestThreadParams& params)
{
#ifdef __APPLE__
    SetGuestThreadQos();
#endif
    const auto procMask = (uint8_t)(params.flags >> 24);
    const auto cpuNumber = procMask == 0 ? 0 : 7 - std::countl_zero(procMask);

    GuestThreadContext ctx(cpuNumber);
    if (g_startingThread != nullptr)
    {
        g_startingThread->pcr = ctx.ppcContext.r13.u32;
        g_startingThread = nullptr;
    }

    uint32_t entry = params.function;
    if (params.startup != 0)
    {
        entry = params.startup;
        ctx.ppcContext.r3.u64 = params.function;
        ctx.ppcContext.r4.u64 = params.value;
    }
    else
    {
        ctx.ppcContext.r3.u64 = params.value;
    }

    // Ips3: ExTerminateThread unwinds back here.
    try
    {
        g_memory.FindFunction(entry)(ctx.ppcContext, g_memory.base);
    }
    catch (const GuestThreadExit& exit)
    {
        return exit.code;
    }

    return ctx.ppcContext.r3.u32;
}

GuestThreadHandle* GuestThread::Start(const GuestThreadParams& params, uint32_t* threadId)
{
    auto hThread = CreateKernelObject<GuestThreadHandle>(params);

    if (threadId != nullptr)
    {
        *threadId = hThread->GetThreadId();
    }

    return hThread;
}

uint32_t GuestThread::GetCurrentThreadId()
{
#ifdef USE_PTHREAD
    return CalcThreadId(pthread_self());
#else
    return CalcThreadId(std::this_thread::get_id());
#endif
}

void GuestThread::SetLastError(uint32_t error)
{
    auto* thread = (char*)g_memory.Translate(GetPPCContext()->r13.u32);
    if (*(uint32_t*)(thread + 0x150))
    {
        // Program doesn't want errors
        return;
    }

    // TEB + 0x160 : Win32LastError
    *(uint32_t*)(thread + TEB_OFFSET + 0x160) = ByteSwap(error);
}

#ifdef _WIN32
void GuestThread::SetThreadName(uint32_t threadId, const char* name)
{
#pragma pack(push,8)
    const DWORD MS_VC_EXCEPTION = 0x406D1388;

    typedef struct tagTHREADNAME_INFO
    {
        DWORD dwType; // Must be 0x1000.
        LPCSTR szName; // Pointer to name (in user addr space).
        DWORD dwThreadID; // Thread ID (-1=caller thread).
        DWORD dwFlags; // Reserved for future use, must be zero.
    } THREADNAME_INFO;
#pragma pack(pop)

    THREADNAME_INFO info;
    info.dwType = 0x1000;
    info.szName = name;
    info.dwThreadID = threadId;
    info.dwFlags = 0;

    __try
    {
        RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}
#endif

void SetThreadNameImpl(uint32_t a1, uint32_t threadId, uint32_t* name)
{
#ifdef _WIN32
    GuestThread::SetThreadName(threadId, (const char*)g_memory.Translate(ByteSwap(*name)));
#endif
}

int GetThreadPriorityImpl(GuestThreadHandle* hThread)
{
#ifdef _WIN32
    return GetThreadPriority(hThread == GetKernelObject(CURRENT_THREAD_HANDLE) ? GetCurrentThread() : hThread->thread.native_handle());
#else 
    return 0;
#endif
}

uint32_t SetThreadIdealProcessorImpl(GuestThreadHandle* hThread, uint32_t dwIdealProcessor)
{
    return 0;
}


