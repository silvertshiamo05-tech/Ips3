// Ips3: the iOS frontend's view of the ARMSX3/RPCS3 core.
//
// This is the iOS counterpart of android/src/rpcsx-android.cpp, without JNI: a plain C API
// (include/ips3_core.h) that the Swift app resolves with dlsym after dlopen()ing the
// framework. It sets up RPCS3's callbacks, the frame the Vulkan backend presents through
// (a CAMetalLayer handed to MoltenVK), the virtual pads the app feeds from touch and
// GameController input, firmware/PKG installation and the game list.

#include "stdafx.h"

#include "ips3_core.h"
#include "CoreAudioBackend.h"

#include "Crypto/unedat.h"
#include "Crypto/unpkg.h"
#include "Crypto/unself.h"
#include "Emu/Audio/Null/NullAudioBackend.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/Modules/cellSaveData.h"
#include "Emu/Cell/Modules/cellSysutil.h"
#include "Emu/Cell/Modules/sceNpTrophy.h"
#include "Emu/Cell/timers.hpp"
#include "Emu/IdManager.h"
#include "Emu/Io/KeyboardHandler.h"
#include "Emu/Io/Null/NullKeyboardHandler.h"
#include "Emu/Io/Null/NullMouseHandler.h"
#include "Emu/Io/Null/null_camera_handler.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/Io/pad_config.h"
#include "Emu/Io/pad_config_types.h"
#include "Emu/RSX/GSFrameBase.h"
#include "Emu/RSX/Null/NullGSRender.h"
#include "Emu/RSX/Overlays/overlay_manager.h"
#include "Emu/RSX/Overlays/overlay_save_dialog.h"
#include "Emu/RSX/Overlays/overlay_trophy_notification.h"
#include "Emu/RSX/Overlays/overlay_utils.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/VK/VKGSRender.h"
#include "Emu/System.h"
#include "Emu/emu_callbacks.h"
#include "Emu/localized_string_id.h"
#include "Emu/system_config.h"
#include "Emu/system_utils.hpp"
#include "Emu/vfs_config.h"
#include "Input/pad_thread.h"
#include "Input/virtual_pad_handler.h"
#include "Loader/ISO.h"
#include "Loader/PSF.h"
#include "Loader/PUP.h"
#include "Loader/TAR.h"
#include "Utilities/File.h"
#include "Utilities/StrUtil.h"
#include "Utilities/Thread.h"
#include "rpcs3_version.h"
#include "util/logs.hpp"
#include "util/sysinfo.hpp"
#include "util/asm.hpp"
#include "util/video_provider.h"

#include <os/log.h>
#include <sys/resource.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

LOG_CHANNEL(ios_log, "IOS");

#define IPS3_EXPORT extern "C" __attribute__((visibility("default")))

namespace
{
	char* dup_string(std::string_view str)
	{
		char* out = static_cast<char*>(std::malloc(str.size() + 1));
		std::memcpy(out, str.data(), str.size());
		out[str.size()] = '\0';
		return out;
	}

	std::string json_escape(std::string_view in)
	{
		std::string out;
		out.reserve(in.size() + 2);

		for (const char c : in)
		{
			switch (c)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20)
				{
					fmt::append(out, "\\u%04x", static_cast<unsigned char>(c));
				}
				else
				{
					out += c;
				}
			}
		}

		return out;
	}

	std::string with_slash(std::string dir)
	{
		if (!dir.empty() && !dir.ends_with('/'))
		{
			dir += '/';
		}

		return dir;
	}

	// ---------------------------------------------------------------------------------------
	// Logging: the file log plus a mirror of warnings and worse to os_log and the app.
	// ---------------------------------------------------------------------------------------

	std::atomic<ips3_log_fn> g_log_fn{nullptr};
	std::atomic<void*> g_log_user{nullptr};
	std::string g_log_path;

	struct ios_log_listener final : logs::listener
	{
		ios_log_listener() { logs::listener::add(this); }

		void log(u64 /*stamp*/, const logs::message& msg, std::string_view prefix, std::string_view text) override
		{
			const auto lvl = static_cast<logs::level>(msg);

			// Severity is inverted (always=0 .. trace=7): only warnings and worse leave the file.
			if (lvl > logs::level::warning)
			{
				return;
			}

			std::string line{prefix};
			line += text;

			const os_log_type_t type = lvl <= logs::level::fatal ? OS_LOG_TYPE_FAULT
				: lvl == logs::level::error ? OS_LOG_TYPE_ERROR
				: OS_LOG_TYPE_DEFAULT;
			os_log_with_type(OS_LOG_DEFAULT, type, "%{public}s", line.c_str());

			if (const auto fn = g_log_fn.load())
			{
				const int level = lvl <= logs::level::fatal ? IPS3_LOG_FATAL
					: lvl == logs::level::error ? IPS3_LOG_ERROR
					: IPS3_LOG_WARNING;
				fn(level, msg->name, line.c_str(), g_log_user.load());
			}
		}
	};

	// ---------------------------------------------------------------------------------------
	// The frame: a CAMetalLayer owned by the app's EmulationView.
	// ---------------------------------------------------------------------------------------

	std::atomic<void*> g_metal_layer{nullptr};
	std::atomic<u64> g_surface_size{0}; // width << 32 | height, in pixels
	std::atomic<u64> g_surface_epoch{0};
	std::atomic<bool> g_backgrounded{false};

	std::atomic<u64> g_fps_bits{0};

	struct ios_frame final : GSFrameBase
	{
		void* wait_for_layer() const
		{
			u32 waited_ms = 0;

			while (true)
			{
				if (void* layer = g_metal_layer.load())
				{
					return layer;
				}

				if (Emu.IsStopped())
				{
					return nullptr;
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				waited_ms += 20;

				if (waited_ms % 5000 == 0 && !g_backgrounded)
				{
					ios_log.error("Still waiting for the Metal layer after %u ms", waited_ms);
				}
			}
		}

		void close() override {}
		void reset() override {}
		bool shown() override { return true; }
		void hide() override {}
		void show() override {}
		void toggle_fullscreen() override {}

		// Vulkan only: MoltenVK builds its own surface from handle().
		void delete_context(draw_context_t) override {}
		draw_context_t make_context() override { return nullptr; }
		void set_current(draw_context_t) override {}

		void flip(draw_context_t, bool /*skip_frame*/) override
		{
			const u64 now = get_system_time();
			m_frames++;

			if (!m_window_start)
			{
				m_window_start = now;
			}
			else if (now - m_window_start >= 1'000'000)
			{
				const double fps = m_frames * 1'000'000.0 / static_cast<double>(now - m_window_start);
				g_fps_bits = std::bit_cast<u64>(fps);
				m_window_start = now;
				m_frames = 0;
			}
		}

		int client_width() override
		{
			if (const u64 packed = g_surface_size.load())
			{
				return static_cast<int>(packed >> 32);
			}

			return 1280;
		}

		int client_height() override
		{
			if (const u64 packed = g_surface_size.load())
			{
				return static_cast<int>(packed & 0xffffffffu);
			}

			return 720;
		}

		f64 client_display_rate() override { return 60.; }
		bool has_alpha() override { return false; }

		display_handle_t handle() const override { return wait_for_layer(); }
		u64 display_epoch() const override { return g_surface_epoch.load(); }

		bool can_consume_frame() const override { return false; }
		void present_frame(std::vector<u8>&&, u32, u32, u32, bool) const override {}
		void take_screenshot(std::vector<u8>&&, u32, u32, bool) override {}
		void update_title(double) override {}

	private:
		u64 m_window_start = 0;
		u32 m_frames = 0;
	};

	// ---------------------------------------------------------------------------------------
	// Dialogs. In-game dialogs are drawn by RPCS3's own overlays; this one only reports
	// progress to whoever asked for it (firmware/PKG installs).
	// ---------------------------------------------------------------------------------------

	struct silent_msg_dialog final : MsgDialogBase
	{
		void Create(const std::string& msg, const std::string& title) override
		{
			ios_log.notice("Message dialog: %s %s", title, msg);
		}
		void Close(bool) override {}
		void SetMsg(const std::string&) override {}
		void ProgressBarSetMsg(u32, const std::string&) override {}
		void ProgressBarReset(u32) override {}
		void ProgressBarInc(u32, u32) override {}
		void ProgressBarSetValue(u32, u32) override {}
		void ProgressBarSetLimit(u32, u32) override {}
	};

	struct overlay_save_dialog final : SaveDialogBase
	{
		s32 ShowSaveDataList(const std::string& base_dir, std::vector<SaveDataEntry>& save_entries, s32 focused, u32 op,
			vm::ptr<CellSaveDataListSet> listSet, bool enable_overlay) override
		{
			const bool use_end = sysutil_send_system_cmd(CELL_SYSUTIL_DRAWING_BEGIN, 0) >= 0;
			s32 result = -2;

			if (auto manager = g_fxo->try_get<rsx::overlays::display_manager>())
			{
				result = manager->create<rsx::overlays::save_dialog>()->show(base_dir, save_entries, focused, op, listSet, enable_overlay);

				if (result == rsx::overlays::user_interface::selection_code::error)
				{
					result = -2;
				}
			}

			if (use_end)
			{
				sysutil_send_system_cmd(CELL_SYSUTIL_DRAWING_END, 0);
			}

			return result;
		}
	};

	struct overlay_trophy_notification final : TrophyNotificationBase
	{
		s32 ShowTrophyNotification(const SceNpTrophyDetails& trophy, const std::vector<uchar>& trophy_icon_buffer) override
		{
			if (auto manager = g_fxo->try_get<rsx::overlays::display_manager>())
			{
				auto popup = std::make_shared<rsx::overlays::trophy_notification>();
				return manager->add(popup, false)->show(trophy, trophy_icon_buffer);
			}

			return 0;
		}
	};

	// ---------------------------------------------------------------------------------------
	// Virtual pads: one per PS3 port, fed by the app.
	// ---------------------------------------------------------------------------------------

	std::mutex g_pad_mutex;
	std::array<std::shared_ptr<Pad>, CELL_PAD_MAX_PORT_NUM> g_pads;

	std::shared_ptr<Pad> get_pad(int port)
	{
		if (port < 0 || static_cast<usz>(port) >= g_pads.size())
		{
			return nullptr;
		}

		std::lock_guard lock(g_pad_mutex);
		return g_pads[port];
	}

	bool init_virtual_pad(const std::shared_ptr<Pad>& pad)
	{
		const u32 capabilities = CELL_PAD_CAPABILITY_PS3_CONFORMITY | CELL_PAD_CAPABILITY_PRESS_MODE |
			CELL_PAD_CAPABILITY_HP_ANALOG_STICK | CELL_PAD_CAPABILITY_ACTUATOR | CELL_PAD_CAPABILITY_SENSOR_MODE;

		// Port 0 is the touch overlay and is always plugged in; the others connect when the app
		// first sends input for them (a second GameController).
		const u32 status = pad->m_player_id == 0 ? CELL_PAD_STATUS_CONNECTED : 0;

		pad->Init(status, capabilities, CELL_PAD_DEV_TYPE_STANDARD, CELL_PAD_PCLASS_TYPE_STANDARD, 0, 0, 0, 50);

		const auto add = [&](u32 offset, u32 code)
		{
			pad->m_buttons.emplace_back(offset, std::vector<std::set<u32>>{}, code);
		};

		add(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_UP);
		add(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_DOWN);
		add(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_LEFT);
		add(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_RIGHT);
		add(CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CROSS);
		add(CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_SQUARE);
		add(CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CIRCLE);
		add(CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_TRIANGLE);
		add(CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L1);
		add(CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L2);
		add(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_L3);
		add(CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R1);
		add(CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R2);
		add(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_R3);
		add(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_START);
		add(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_SELECT);
		add(CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_PS);

		pad->m_sticks[0] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X, {}, {});
		pad->m_sticks[1] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y, {}, {});
		pad->m_sticks[2] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X, {}, {});
		pad->m_sticks[3] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y, {}, {});

		pad->m_sensors[0] = AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_X, 0, 0, 0, DEFAULT_MOTION_X);
		pad->m_sensors[1] = AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_Y, 0, 0, 0, DEFAULT_MOTION_Y);
		pad->m_sensors[2] = AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_Z, 0, 0, 0, DEFAULT_MOTION_Z);
		pad->m_sensors[3] = AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_G, 0, 0, 0, DEFAULT_MOTION_G);

		pad->m_vibrate_motors[0] = VibrateMotor(true);
		pad->m_vibrate_motors[1] = VibrateMotor(false);

		if (pad->m_player_id < g_pads.size())
		{
			std::lock_guard lock(g_pad_mutex);
			g_pads[pad->m_player_id] = pad;
		}

		return true;
	}

	void open_home_menu_async()
	{
		if (!g_metal_layer.load())
		{
			return;
		}

		std::thread([]
		{
			if (auto pad_thread = pad::get_pad_thread(true))
			{
				pad_thread->open_home_menu();
			}
		}).detach();
	}

	// ---------------------------------------------------------------------------------------
	// Callbacks
	// ---------------------------------------------------------------------------------------

	// Work posted to "the main thread". RPCS3's main thread is just a serial queue for us.
	struct main_queue
	{
		std::mutex mutex;
		std::condition_variable cv;
		std::deque<std::function<void()>> items;
		std::thread worker;

		main_queue()
		{
			worker = std::thread([this]
			{
				pthread_setname_np("RPCS3 main");
				thread_ctrl::set_native_priority(1);

				while (true)
				{
					std::function<void()> item;
					{
						std::unique_lock lock(mutex);
						cv.wait(lock, [this] { return !items.empty(); });
						item = std::move(items.front());
						items.pop_front();
					}

					item();
				}
			});
			worker.detach();
		}

		void post(std::function<void()> fn)
		{
			{
				std::lock_guard lock(mutex);
				items.push_back(std::move(fn));
			}

			cv.notify_one();
		}
	};

	main_queue& get_main_queue()
	{
		static main_queue s_queue;
		return s_queue;
	}

	std::string localized(localized_string_id id)
	{
		// The overlays fall back to their own text when this is empty, which is fine for now.
		static_cast<void>(id);
		return {};
	}

	void setup_callbacks()
	{
		emu_callbacks callbacks{};

		callbacks.call_from_main_thread = [](std::function<void()> cb, atomic_t<u32>* wake_up)
		{
			if (wake_up)
			{
				cb();
				*wake_up = true;
				wake_up->notify_one();
				return;
			}

			get_main_queue().post(std::move(cb));
		};

		callbacks.on_run = [](auto...) {};
		callbacks.on_pause = [](auto...) {};
		callbacks.on_resume = [](auto...) {};
		callbacks.on_stop = [](auto...) {};
		callbacks.on_ready = [](auto...) {};
		callbacks.on_missing_fw = [](auto...) { ios_log.error("Firmware is not installed. Install a PS3UPDAT.PUP first."); };
		callbacks.on_emulation_stop_no_response = [](std::shared_ptr<atomic_t<bool>> closed, int seconds)
		{
			if (!closed || !*closed)
			{
				ios_log.error("Emulation has not stopped after %d seconds.", seconds);
			}
		};
		callbacks.on_save_state_progress = [](auto...) {};
		callbacks.enable_disc_eject = [](auto...) {};
		callbacks.enable_disc_insert = [](auto...) {};
		callbacks.handle_taskbar_progress = [](auto...) {};
		callbacks.init_kb_handler = [](auto...)
		{
			ensure(g_fxo->init<KeyboardHandlerBase, NullKeyboardHandler>(Emu.DeserialManager()));
		};
		callbacks.init_mouse_handler = [](auto...)
		{
			ensure(g_fxo->init<MouseHandlerBase, NullMouseHandler>(Emu.DeserialManager()));
		};
		callbacks.init_pad_handler = [](auto...)
		{
			ensure(g_fxo->init<named_thread<pad_thread>>(nullptr, nullptr, ""));
		};
		callbacks.update_emu_settings = [](auto...) {};
		callbacks.save_emu_settings = [](auto...)
		{
			Emulator::SaveSettings(g_cfg.to_string(), Emu.GetTitleID());
		};
		callbacks.enable_gamemode = [](bool) {};
		callbacks.get_database_config = [](const std::string&) { return std::string{}; };
		callbacks.get_photo_path = [](std::string_view) { return std::string{}; };
		callbacks.try_to_quit = [](bool, std::function<void()> on_exit)
		{
			if (on_exit)
			{
				on_exit();
			}

			return true;
		};
		callbacks.make_video_source = []() -> std::unique_ptr<video_source> { return nullptr; };
		callbacks.close_gs_frame = [](auto...) {};
		callbacks.get_gs_frame = [] { return std::make_unique<ios_frame>(); };
		callbacks.get_camera_handler = [](auto...) { return std::make_shared<null_camera_handler>(); };
		callbacks.get_music_handler = [](auto...) { return std::make_shared<null_music_handler>(); };
		callbacks.init_gs_render = [](utils::serial* ar)
		{
			switch (g_cfg.video.renderer.get())
			{
			case video_renderer::null:
				g_fxo->init<rsx::thread, named_thread<NullGSRender>>(ar);
				break;
			default:
				g_fxo->init<rsx::thread, named_thread<VKGSRender>>(ar);
				break;
			}
		};
		callbacks.get_audio = [](auto...) -> std::shared_ptr<AudioBackend>
		{
			std::shared_ptr<AudioBackend> result;

			if (g_cfg.audio.renderer.get() == audio_renderer::null)
			{
				result = std::make_shared<NullAudioBackend>();
			}
			else
			{
				result = std::make_shared<CoreAudioBackend>();
			}

			if (!result->Initialized())
			{
				ios_log.error("Audio renderer %s could not be initialized, using Null instead.", result->GetName());
				result = std::make_shared<NullAudioBackend>();
			}

			return result;
		};
		callbacks.get_audio_enumerator = [](auto...) { return nullptr; };
		callbacks.get_msg_dialog = [] { return std::make_shared<silent_msg_dialog>(); };
		callbacks.get_osk_dialog = [](auto...) { return nullptr; };
		callbacks.get_save_dialog = [](auto...) { return std::make_unique<overlay_save_dialog>(); };
		callbacks.get_sendmessage_dialog = [](auto...) { return nullptr; };
		callbacks.get_recvmessage_dialog = [](auto...) { return nullptr; };
		callbacks.get_trophy_notification_dialog = [](auto...) { return std::make_unique<overlay_trophy_notification>(); };
		callbacks.get_localized_string = [](localized_string_id id, const char*) -> std::string { return localized(id); };
		callbacks.get_localized_u32string = [](localized_string_id id, const char*) -> std::u32string { return utf8_to_u32string(localized(id)); };
		callbacks.get_localized_setting = [](auto...) { return ""; };
		callbacks.play_sound = [](const std::string&, std::optional<f32>) {};
		callbacks.get_image_info = [](auto...) { return false; };
		callbacks.get_scaled_image = [](auto...) { return false; };
		callbacks.resolve_path = [](std::string_view arg)
		{
			std::error_code ec;
			auto result = std::filesystem::weakly_canonical(std::filesystem::path(fmt::replace_all(arg, "\\", "/")), ec).string();
			return ec ? std::string(arg) : result;
		};
		callbacks.get_font_dirs = [](auto...) { return std::vector<std::string>(); };
		callbacks.on_install_pkgs = [](const std::vector<std::string>& pkgs, bool from_optical_drive)
		{
			for (const std::string& pkg : pkgs)
			{
				if (!rpcs3::utils::install_pkg(pkg, from_optical_drive))
				{
					return false;
				}
			}

			return true;
		};
		callbacks.add_breakpoint = [](auto...) {};
		callbacks.display_sleep_control_supported = [](auto...) { return false; };
		callbacks.enable_display_sleep = [](auto...) {};
		callbacks.check_microphone_permissions = [](auto...) {};

		g_emu_callbacks = std::move(callbacks);
	}

	// ---------------------------------------------------------------------------------------
	// Config by path
	// ---------------------------------------------------------------------------------------

	cfg::_base* find_cfg_node(cfg::_base* root, std::string_view path)
	{
		for (const std::string& elem : fmt::split(path, {"/"}))
		{
			if (elem.empty())
			{
				continue;
			}

			if (root->get_type() != cfg::type::node)
			{
				return nullptr;
			}

			cfg::_base* child = nullptr;

			for (cfg::_base* node : static_cast<cfg::node*>(root)->get_nodes())
			{
				if (node->get_name() == elem)
				{
					child = node;
					break;
				}
			}

			if (!child)
			{
				return nullptr;
			}

			root = child;
		}

		return root;
	}

	bool g_initialized = false;
	std::mutex g_lifecycle_mutex;
	std::unique_ptr<logs::listener> g_file_log;

	struct progress_sink
	{
		ips3_progress_fn fn;
		void* user;

		bool operator()(std::string_view msg, u64 value, u64 max) const
		{
			if (!fn)
			{
				return true;
			}

			const std::string text{msg};
			return fn(text.c_str(), value, max, user);
		}
	};

	std::string game_json(const std::string& sfo_path, std::string_view boot_path, std::string_view icon_path, std::string_view kind)
	{
		const psf::registry psf = psf::load_object(sfo_path);

		if (psf.empty())
		{
			return {};
		}

		return fmt::format(R"({"title":"%s","title_id":"%s","version":"%s","category":"%s","path":"%s","icon":"%s","kind":"%s"})",
			json_escape(psf::get_string(psf, "TITLE")),
			json_escape(psf::get_string(psf, "TITLE_ID")),
			json_escape(psf::get_string(psf, "APP_VER", psf::get_string(psf, "VERSION"))),
			json_escape(psf::get_string(psf, "CATEGORY")),
			json_escape(boot_path),
			json_escape(icon_path),
			kind);
	}

	void scan_disc_dirs(const std::string& root, std::vector<std::string>& out)
	{
		for (const auto& entry : fs::dir(root))
		{
			if (entry.name == "." || entry.name == "..")
			{
				continue;
			}

			const std::string path = root + entry.name;

			if (entry.is_directory)
			{
				const std::string sfo = path + "/PS3_GAME/PARAM.SFO";

				if (fs::is_file(sfo))
				{
					if (std::string json = game_json(sfo, path, path + "/PS3_GAME/ICON0.PNG", "disc"); !json.empty())
					{
						out.push_back(std::move(json));
					}
				}
				else if (fs::is_file(path + "/PARAM.SFO") && fs::is_file(path + "/USRDIR/EBOOT.BIN"))
				{
					if (std::string json = game_json(path + "/PARAM.SFO", path, path + "/ICON0.PNG", "folder"); !json.empty())
					{
						out.push_back(std::move(json));
					}
				}
			}
			else if (fmt::to_lower(entry.name).ends_with(".iso"))
			{
				std::string stem = entry.name.substr(0, entry.name.size() - 4);
				out.push_back(fmt::format(R"({"title":"%s","title_id":"","version":"","category":"DG","path":"%s","icon":"","kind":"iso"})",
					json_escape(stem), json_escape(path)));
			}
		}
	}
} // namespace

// Globals the core expects its frontend to define (rpcs3.cpp on desktop, the JNI bridge and
// remote_play.cpp on Android).
std::string g_input_config_override;
cfg_input_configurations g_cfg_input_configs;

// Remote play is Android-only; with max size 0 the presenter never captures.
atomic_t<u32> g_remote_play_max_size{0};
atomic_t<u32> g_remote_play_frames{0};
atomic_t<u32> g_remote_play_skipped{0};
atomic_t<bool> g_remote_play_hardware{false};

// Upstream's desktop main() provides these two; the core calls them.
[[noreturn]] void report_fatal_error(std::string_view text, bool /*is_html*/ = false, bool /*include_help_text*/ = true)
{
	std::string buf{text};
	fmt::append(buf, "\nBuild: \"%s\"", rpcs3::get_verbose_version());

	os_log_fault(OS_LOG_DEFAULT, "RPCS3 fatal: %{public}s", buf.c_str());
	std::fprintf(stderr, "RPCS3 fatal: %s\n", buf.c_str());

	if (!g_log_path.empty())
	{
		if (fs::file f{g_log_path, fs::write + fs::append + fs::create})
		{
			f.write("\nFATAL: " + buf + "\n");
		}
	}

	logs::listener::sync_all();
	utils::trap();
	std::abort();
}

void qt_events_aware_op(int /*repeat_duration_ms*/, std::function<bool()> wrapped_op)
{
	while (!wrapped_op())
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

IPS3_EXPORT int ips3_api_version(void)
{
	return IPS3_API_VERSION;
}

IPS3_EXPORT void ips3_set_log_callback(ips3_log_fn fn, void* user)
{
	g_log_user = user;
	g_log_fn = fn;
}

IPS3_EXPORT void ips3_apply_memory_profile(uint64_t physical_memory_bytes, uint64_t app_memory_limit_bytes)
{
	// The budget that matters is the jetsam limit, not the RAM size: a 4 GB iPad gives an app
	// roughly 2.0 GB, or about 3.0 GB with the increased-memory-limit entitlement, and that has
	// to hold 512 MB of guest RAM/VRAM, MoltenVK's allocations (GPU memory is app memory on
	// Apple silicon), the texture cache and LLVM while it compiles.
	const u64 budget = app_memory_limit_bytes ? app_memory_limit_bytes : physical_memory_bytes / 2;
	const bool tight = budget < 3200ull * 1024 * 1024;

	ios_log.notice("Memory profile: physical %u MiB, app limit %u MiB (%s)", physical_memory_bytes >> 20,
		app_memory_limit_bytes >> 20, tight ? "tight" : "normal");

	// CPU: LLVM recompilers are the only arm64 option. One compile thread on a tight budget:
	// each LLVM instance can take several hundred MB on a large PPU module.
	g_cfg.core.ppu_decoder.set(ppu_decoder_type::llvm);
	g_cfg.core.spu_decoder.set(spu_decoder_type::llvm);
	g_cfg.core.ppu_threads.set(2);
	g_cfg.core.llvm_threads.set(tight ? 1 : 2);
	// Precompiling every PRX of a game up front is what blows the budget on first boot.
	g_cfg.core.llvm_precompilation.set(false);
	g_cfg.core.spu_cache.set(true);
	// A14: 2 performance + 4 efficiency cores.
	g_cfg.core.preferred_spu_threads.set(0);
	g_cfg.core.thread_scheduler.set(thread_scheduler_mode::alt);
	g_cfg.core.llvm_cpu.from_string("");

	// GPU: native 720p, nothing that keeps extra copies of render targets around.
	g_cfg.video.renderer.set(video_renderer::vulkan);
	g_cfg.video.resolution_scale_percent.set(100);
	g_cfg.video.antialiasing_level.set(msaa_level::none);
	g_cfg.video.write_color_buffers.set(false);
	g_cfg.video.read_color_buffers.set(false);
	g_cfg.video.read_depth_buffer.set(false);
	g_cfg.video.write_depth_buffer.set(false);
	g_cfg.video.strict_rendering_mode.set(false);
	g_cfg.video.multithreaded_rsx.set(false);
	g_cfg.video.shader_compiler_threads_count.set(tight ? 1 : 2);
	// The shader interpreter keeps large uber-shaders resident; plain async compilation
	// shows missing geometry for a moment instead.
	g_cfg.video.shadermode.set(shader_mode::async_recompiler);
	g_cfg.video.vk.asynchronous_texture_streaming.set(false);
	g_cfg.video.disable_msl_fast_math.set(false);

	// "Cubeb" is the only non-null renderer in the iOS enum; the bridge maps it to RemoteIO.
	g_cfg.audio.renderer.set(audio_renderer::cubeb);
	g_cfg.audio.enable_buffering.set(true);
	g_cfg.audio.desired_buffer_duration.set(64);

	Emulator::SaveSettings(g_cfg.to_string(), "");
}

IPS3_EXPORT bool ips3_initialize(const char* data_dir, const char* cache_dir, const char* resource_dir,
	uint64_t physical_memory_bytes, uint64_t app_memory_limit_bytes)
{
	std::lock_guard lock(g_lifecycle_mutex);

	if (g_initialized)
	{
		return true;
	}

	// The core reads these through fs::get_config_dir()/get_cache_dir()/get_executable_dir().
	// The app normally exports them before dlopen; doing it again here covers a direct call.
	setenv("IPS3_DATA_DIR", with_slash(data_dir).c_str(), 0);
	setenv("IPS3_CACHE_DIR", with_slash(cache_dir).c_str(), 0);
	setenv("IPS3_RESOURCE_DIR", with_slash(resource_dir).c_str(), 0);

	fs::create_path(fs::get_config_dir());
	fs::create_path(fs::get_cache_dir());

	// Initialise the thread pool finalizer.
	static_cast<void>(named_thread("", [](int) {}));

	static ios_log_listener s_log_mirror;

	{
		const std::string dir = fs::get_log_dir();
		const std::string current = dir + "Ips3.log";
		std::error_code ec;

		if (std::filesystem::exists(current, ec))
		{
			std::filesystem::rename(current, dir + "Ips3.old.log", ec);
		}

		fs::device_stat stats{};
		const u64 limit = fs::statfs(dir, stats) ? stats.avail_free / 4 : 64ull << 20;

		g_log_path = current;
		g_file_log = logs::make_file_listener(current, limit);
	}

	{
		logs::stored_message ver{ios_log.always()};
		ver.text = fmt::format("Ips3 (ARMSX3 %s) for iOS", rpcs3::get_version().to_string());

		logs::stored_message sys{ios_log.always()};
		sys.text = utils::get_system_info();

		logs::stored_message os{ios_log.always()};
		os.text = utils::get_OS_version_string();

		logs::stored_message mem{ios_log.always()};
		mem.text = fmt::format("Physical memory: %u MiB, app memory limit: %u MiB", physical_memory_bytes >> 20, app_memory_limit_bytes >> 20);

		logs::set_init({std::move(ver), std::move(sys), std::move(os), std::move(mem)});
	}

	// Deep recursion in LLVM's optimiser on the compile threads.
	if (rlimit rl{}; getrlimit(RLIMIT_STACK, &rl) == 0)
	{
		rl.rlim_cur = std::min<rlim_t>(rl.rlim_max, 64ull << 20);
		setrlimit(RLIMIT_STACK, &rl);
	}

	virtual_pad_handler::set_on_connect_cb(init_virtual_pad);
	setup_callbacks();

	Emu.SetHasGui(false);
	Emu.SetUsr("00000001");
	Emu.SetSupportedRenderers({video_renderer::null, video_renderer::vulkan});
	Emu.SetDefaultRenderer(video_renderer::vulkan);
	Emu.SetDefaultGraphicsAdapter("Default");
	Emu.Init();

	for (usz i = 0; i < g_cfg_input.player.size(); i++)
	{
		g_cfg_input.player[i]->handler.set(pad_handler::virtual_pad);
		g_cfg_input.player[i]->device.from_string("Virtual");
	}

	g_cfg_input.save("", g_cfg_input_configs.default_config);

	// First run (or a config written by another build): apply the memory profile.
	const std::string marker = fs::get_config_dir() + ".ips3_profile_v1";

	if (!fs::is_file(marker))
	{
		ips3_apply_memory_profile(physical_memory_bytes, app_memory_limit_bytes);
		fs::write_file(marker, fs::rewrite, std::string("1"));
	}
	else
	{
		// Things that are wrong on iOS whatever the user picked.
		g_cfg.core.ppu_threads.set(2);
		g_cfg.core.llvm_cpu.from_string("");
		Emulator::SaveSettings(g_cfg.to_string(), "");
	}

	g_initialized = true;
	ios_log.success("Ips3 core initialised. Data: %s", fs::get_config_dir());
	return true;
}

IPS3_EXPORT char* ips3_firmware_version(void)
{
	return dup_string(utils::get_firmware_version());
}

IPS3_EXPORT bool ips3_install_firmware(const char* pup_path, ips3_progress_fn progress_fn, void* user)
{
	const progress_sink progress{progress_fn, user};

	fs::file pup_f(pup_path);

	if (!pup_f)
	{
		ios_log.error("Firmware: cannot open %s (%s)", pup_path, fs::g_tls_error);
		return false;
	}

	pup_object pup(std::move(pup_f));

	if (static_cast<pup_error>(pup) != pup_error::ok)
	{
		ios_log.error("Firmware: %s is not a valid PS3UPDAT.PUP", pup_path);
		return false;
	}

	fs::file update_files_f = pup.get_file(0x300);

	if (!update_files_f || !update_files_f.size())
	{
		ios_log.error("Firmware: the update file is broken");
		return false;
	}

	tar_object update_files(update_files_f);

	auto update_filenames = update_files.get_filenames();
	std::erase_if(update_filenames, [](const std::string& s) { return !s.starts_with("dev_flash_"); });

	if (update_filenames.empty())
	{
		ios_log.error("Firmware: the update file contains no dev_flash packages");
		return false;
	}

	u64 processed = 0;

	for (const auto& update_filename : update_filenames)
	{
		if (!progress(fmt::format("Installing %s", update_filename), processed, update_filenames.size()))
		{
			return false;
		}

		auto update_file_stream = update_files.get_file(update_filename);

		if (update_file_stream->m_file_handler)
		{
			update_file_stream->m_file_handler->handle_file_op(*update_file_stream, 0, update_file_stream->get_size(umax), nullptr);
		}

		fs::file update_file = fs::make_stream(std::move(update_file_stream->data));

		SCEDecrypter self_dec(update_file);
		self_dec.LoadHeaders();
		self_dec.LoadMetadata(SCEPKG_ERK, SCEPKG_RIV);
		self_dec.DecryptData();

		auto dev_flash_tar_f = self_dec.MakeFile();

		if (dev_flash_tar_f.size() < 3)
		{
			ios_log.error("Firmware: %s could not be decrypted", update_filename);
			return false;
		}

		tar_object dev_flash_tar(dev_flash_tar_f[2]);

		if (!dev_flash_tar.extract())
		{
			ios_log.error("Firmware: could not write %s into %s (%s)", update_filename, g_cfg_vfs.get_dev_flash(), fs::g_tls_error);
			return false;
		}

		processed++;
	}

	progress("Firmware installed", processed, processed);
	ios_log.success("Firmware %s installed", utils::get_firmware_version());
	return true;
}

IPS3_EXPORT bool ips3_install_pkg(const char* pkg_path, ips3_progress_fn progress_fn, void* user)
{
	const progress_sink progress{progress_fn, user};
	progress("Installing package", 0, 0);

	const bool ok = rpcs3::utils::install_pkg(pkg_path, false);

	progress(ok ? "Package installed" : "Package installation failed", 1, 1);
	return ok;
}

IPS3_EXPORT char* ips3_list_games(const char* extra_dir)
{
	std::vector<std::string> games;

	// Installed (HDD) titles.
	const std::string hdd_game = rpcs3::utils::get_hdd0_game_dir();

	for (const auto& entry : fs::dir(hdd_game))
	{
		if (!entry.is_directory || entry.name == "." || entry.name == "..")
		{
			continue;
		}

		const std::string dir = hdd_game + entry.name;
		const std::string sfo = dir + "/PARAM.SFO";

		if (!fs::is_file(sfo) || !fs::is_file(dir + "/USRDIR/EBOOT.BIN"))
		{
			continue;
		}

		const psf::registry psf = psf::load_object(sfo);
		const std::string_view category = psf::get_string(psf, "CATEGORY");

		// Game data and patches live here too and are not bootable on their own.
		if (category != "HG" && category != "AP" && category != "AM" && category != "AV" && category != "CB")
		{
			continue;
		}

		if (std::string json = game_json(sfo, dir, dir + "/ICON0.PNG", "hdd"); !json.empty())
		{
			games.push_back(std::move(json));
		}
	}

	// Disc folders and ISOs.
	scan_disc_dirs(rpcs3::utils::get_games_dir(), games);

	if (extra_dir && *extra_dir)
	{
		scan_disc_dirs(with_slash(extra_dir), games);
	}

	std::string out = "[";

	for (usz i = 0; i < games.size(); i++)
	{
		if (i)
		{
			out += ',';
		}

		out += games[i];
	}

	out += ']';
	return dup_string(out);
}

IPS3_EXPORT void ips3_set_surface(void* metal_layer, int width_px, int height_px)
{
	if (width_px > 0 && height_px > 0)
	{
		g_surface_size = (u64{static_cast<u32>(width_px)} << 32) | static_cast<u32>(height_px);
	}

	if (g_metal_layer.exchange(metal_layer) != metal_layer)
	{
		g_surface_epoch++;
	}
}

IPS3_EXPORT void ips3_surface_resized(int width_px, int height_px)
{
	if (width_px > 0 && height_px > 0)
	{
		g_surface_size = (u64{static_cast<u32>(width_px)} << 32) | static_cast<u32>(height_px);
	}
}

IPS3_EXPORT int ips3_boot(const char* path_c)
{
	std::lock_guard lock(g_lifecycle_mutex);

	if (!Emu.IsStopped(true))
	{
		Emu.SetContinuousMode(false);
		Emu.Kill();

		for (int waited = 0; !Emu.IsStopped(true) && waited < 10000; waited += 20)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}

	if (Emu.IsStopped(true))
	{
		unload_iso();
	}

	std::string path = path_c ? path_c : "";

	while (path.ends_with('/'))
	{
		path.pop_back();
	}

	Emu.SetForceBoot(true);
	Emu.SetGameDir("PS3_GAME");

	if (fs::is_dir(path))
	{
		// HDD title or extracted folder: boot its EBOOT.BIN. Disc folders boot as a directory.
		if (const std::string eboot = path + "/USRDIR/EBOOT.BIN"; fs::is_file(eboot))
		{
			path = eboot;
		}
	}

	ios_log.notice("Booting '%s'", path);
	return static_cast<int>(Emu.BootGame(path, "", false, cfg_mode::custom));
}

IPS3_EXPORT int ips3_state(void)
{
	return static_cast<int>(Emu.GetStatus(false));
}

IPS3_EXPORT void ips3_pause(void)
{
	Emu.Pause();
}

IPS3_EXPORT void ips3_resume(void)
{
	Emu.Resume();
}

IPS3_EXPORT void ips3_stop(void)
{
	std::lock_guard lock(g_lifecycle_mutex);
	Emu.SetContinuousMode(false);
	Emu.Kill();
}

IPS3_EXPORT void ips3_open_home_menu(void)
{
	open_home_menu_async();
}

IPS3_EXPORT void ips3_set_backgrounded(bool backgrounded)
{
	g_backgrounded = backgrounded;
}

IPS3_EXPORT void ips3_pad_set(int port, uint32_t digital1, uint32_t digital2, int lx, int ly, int rx, int ry)
{
	const auto pad = get_pad(port);

	if (!pad)
	{
		return;
	}

	if (!(pad->m_port_status & CELL_PAD_STATUS_CONNECTED))
	{
		pad->m_port_status |= CELL_PAD_STATUS_CONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES;
	}

	for (auto& btn : pad->m_buttons)
	{
		const bool was_pressed = btn.m_pressed;

		if (btn.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL1)
		{
			btn.m_pressed = (digital1 & btn.m_outKeyCode) != 0;

			if (btn.m_outKeyCode == CELL_PAD_CTRL_PS && btn.m_pressed && !was_pressed)
			{
				open_home_menu_async();
			}
		}
		else if (btn.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL2)
		{
			btn.m_pressed = (digital2 & btn.m_outKeyCode) != 0;
		}

		btn.m_value = btn.m_pressed ? 255 : 0;
	}

	pad->m_sticks[0].m_value = static_cast<u16>(std::clamp(lx, 0, 255));
	pad->m_sticks[1].m_value = static_cast<u16>(std::clamp(ly, 0, 255));
	pad->m_sticks[2].m_value = static_cast<u16>(std::clamp(rx, 0, 255));
	pad->m_sticks[3].m_value = static_cast<u16>(std::clamp(ry, 0, 255));
}

IPS3_EXPORT void ips3_pad_set_motion(int port, int x, int y, int z, int g)
{
	if (const auto pad = get_pad(port))
	{
		pad->m_sensors[0].m_value = static_cast<u16>(std::clamp(x, 0, 1023));
		pad->m_sensors[1].m_value = static_cast<u16>(std::clamp(y, 0, 1023));
		pad->m_sensors[2].m_value = static_cast<u16>(std::clamp(z, 0, 1023));
		pad->m_sensors[3].m_value = static_cast<u16>(std::clamp(g, 0, 1023));
	}
}

IPS3_EXPORT int ips3_pad_get_rumble(int port)
{
	if (const auto pad = get_pad(port))
	{
		return (int{pad->m_vibrate_motors[0].value} << 8) | pad->m_vibrate_motors[1].value;
	}

	return 0;
}

IPS3_EXPORT char* ips3_config_get(const char* path)
{
	if (cfg::_base* node = find_cfg_node(&g_cfg, path ? path : ""))
	{
		return dup_string(node->to_string());
	}

	return nullptr;
}

IPS3_EXPORT bool ips3_config_set(const char* path, const char* value)
{
	cfg::_base* node = find_cfg_node(&g_cfg, path ? path : "");

	if (!node || node->get_type() == cfg::type::node)
	{
		ios_log.error("config: no setting at '%s'", path);
		return false;
	}

	if (!node->from_string(value ? value : "", !Emu.IsStopped()))
	{
		ios_log.error("config: '%s' rejected value '%s'", path, value);
		return false;
	}

	return true;
}

IPS3_EXPORT void ips3_config_save(void)
{
	Emulator::SaveSettings(g_cfg.to_string(), "");
}

IPS3_EXPORT char* ips3_config_dump(void)
{
	return dup_string(g_cfg.to_string());
}

IPS3_EXPORT double ips3_get_fps(void)
{
	return std::bit_cast<double>(g_fps_bits.load());
}

IPS3_EXPORT char* ips3_get_title(void)
{
	return dup_string(Emu.GetTitleAndTitleID());
}

IPS3_EXPORT char* ips3_log_path(void)
{
	return dup_string(g_log_path);
}

IPS3_EXPORT void ips3_free(void* ptr)
{
	std::free(ptr);
}
