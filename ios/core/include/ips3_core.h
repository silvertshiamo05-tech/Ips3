// Ips3 core C API.
//
// The app never links Ips3Core.framework. It dlopen()s it after its own preflight checks
// (JIT, virtual address space, memory limit) and resolves these symbols with dlsym, so a
// device that cannot run the core gets an explanation instead of a crash at launch: RPCS3
// reserves its guest address space from static initialisers, which run inside dlopen().
//
// Every function is safe to call from any thread unless noted. Strings returned as
// `char*` are heap-allocated and must be released with ips3_free().
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPS3_API_VERSION 1

enum ips3_log_level
{
	IPS3_LOG_FATAL = 1,
	IPS3_LOG_ERROR = 2,
	IPS3_LOG_WARNING = 4,
	IPS3_LOG_NOTICE = 6,
};

// Mirrors system_state in Emu/System.h.
enum ips3_state
{
	IPS3_STATE_STOPPED = 0,
	IPS3_STATE_LOADING = 1,
	IPS3_STATE_STOPPING = 2,
	IPS3_STATE_RUNNING = 3,
	IPS3_STATE_PAUSED = 4,
	IPS3_STATE_FROZEN = 5,
	IPS3_STATE_READY = 6,
	IPS3_STATE_STARTING = 7,
};

// PS3 pad bits, CELL_PAD_CTRL_* (digital1 / digital2).
enum ips3_pad_digital1
{
	IPS3_PAD_SELECT = 0x01,
	IPS3_PAD_L3 = 0x02,
	IPS3_PAD_R3 = 0x04,
	IPS3_PAD_START = 0x08,
	IPS3_PAD_UP = 0x10,
	IPS3_PAD_RIGHT = 0x20,
	IPS3_PAD_DOWN = 0x40,
	IPS3_PAD_LEFT = 0x80,
	IPS3_PAD_PS = 0x100, // opens the home menu
};

enum ips3_pad_digital2
{
	IPS3_PAD_L2 = 0x01,
	IPS3_PAD_R2 = 0x02,
	IPS3_PAD_L1 = 0x04,
	IPS3_PAD_R1 = 0x08,
	IPS3_PAD_TRIANGLE = 0x10,
	IPS3_PAD_CIRCLE = 0x20,
	IPS3_PAD_CROSS = 0x40,
	IPS3_PAD_SQUARE = 0x80,
};

typedef void (*ips3_log_fn)(int level, const char* channel, const char* text, void* user);

// value/max of 0/0 means "indeterminate". Return false to cancel (where supported).
typedef bool (*ips3_progress_fn)(const char* message, uint64_t value, uint64_t max, void* user);

int ips3_api_version(void);

// Directories must already exist or be creatable. data_dir holds config.yml, dev_hdd0,
// dev_flash and friends (visible in Files); cache_dir holds PPU/SPU/shader caches and logs;
// resource_dir is the read-only bundle directory with fonts and icons.
//
// physical_memory_bytes / app_memory_limit_bytes drive the low-memory profile (see
// ips3_apply_memory_profile). Returns false only if the core cannot start at all.
bool ips3_initialize(const char* data_dir, const char* cache_dir, const char* resource_dir,
	uint64_t physical_memory_bytes, uint64_t app_memory_limit_bytes);

void ips3_set_log_callback(ips3_log_fn fn, void* user);

// Writes the defaults that keep a 4 GB iPad inside its memory budget. Called by
// ips3_initialize on first run; call again to reset.
void ips3_apply_memory_profile(uint64_t physical_memory_bytes, uint64_t app_memory_limit_bytes);

// "" when no firmware is installed.
char* ips3_firmware_version(void);

// Synchronous; run on a background thread.
bool ips3_install_firmware(const char* pup_path, ips3_progress_fn progress, void* user);
bool ips3_install_pkg(const char* pkg_path, ips3_progress_fn progress, void* user);

// JSON array of {"title","title_id","version","category","path","icon","kind"} for HDD
// games, disc folders and ISOs under the games directory (and any extra directory given).
char* ips3_list_games(const char* extra_dir);

// The CAMetalLayer the renderer presents to, and its size in pixels. Pass NULL when the
// view goes away; the renderer waits for the next one.
void ips3_set_surface(void* metal_layer, int width_px, int height_px);
void ips3_surface_resized(int width_px, int height_px);

// game_boot_result from Emu/System.h; 0 is success.
int ips3_boot(const char* path);
int ips3_state(void);
void ips3_pause(void);
void ips3_resume(void);
void ips3_stop(void);
void ips3_open_home_menu(void);
void ips3_set_backgrounded(bool backgrounded);

// Sticks are 0..255 with 128 centred, as the PS3 reports them.
void ips3_pad_set(int port, uint32_t digital1, uint32_t digital2, int lx, int ly, int rx, int ry);
// Motion, 0..1023 with 512 at rest (DS3 sixaxis units).
void ips3_pad_set_motion(int port, int x, int y, int z, int g);
// Rumble motor levels for the app to forward to a GCController, (large << 8) | small.
int ips3_pad_get_rumble(int port);

// Config nodes by path, e.g. "Core/PPU Decoder", "Video/Resolution Scale".
char* ips3_config_get(const char* path);
bool ips3_config_set(const char* path, const char* value);
void ips3_config_save(void);
// Whole config as YAML.
char* ips3_config_dump(void);

// Frames presented per second over the last second, and the title currently running.
double ips3_get_fps(void);
char* ips3_get_title(void);

// Path of the current log file.
char* ips3_log_path(void);

void ips3_free(void* ptr);

#ifdef __cplusplus
}
#endif
