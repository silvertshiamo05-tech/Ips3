// Ips3 Bayonetta runtime: shared includes.
//
// Adapted from UnleashedRecomp (https://github.com/hedge-dev/UnleashedRecomp), GPL-3.0.
// This replaces its stdafx.h, which pulls in SDL, ImGui and the renderer; the runtime only
// needs the standard library, the recompiler's headers and a few small libraries.
#pragma once

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <numeric>
#include <semaphore>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/core.h>
#include <ankerl/unordered_dense.h>
#include <o1heap.h>
#include <xxhash.h>

#include <xbox.h>
#include <ppc/ppc_recomp_shared.h>

#include "os/logger.h"
#include "user/config.h"
#include "framework.h"
#include "mutex.h"
