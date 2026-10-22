// Ips3: the game window (iOS, or desktops with IPS3_WINDOW=1).
//
// The game runs on its own thread; this thread owns the window. A helper thread reads each new
// guest frame back from Xenia's presenter, and the window draws the latest one letterboxed with
// SDL's renderer (Metal on Apple), then the touch controls on top. Touch controls show until a
// game controller connects.
#include <stdafx.h>
#include "host.h"
#include "host_internal.h"

#include <ips3_gpu.h>

#include <SDL.h>

#include "mobile.h"
#include "settings.h"

#include <array>
#include <atomic>
#include <cctype>
#include <deque>
#include <fstream>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
	SDL_Window* g_window = nullptr;
	SDL_Renderer* g_renderer = nullptr;

	// ---- Guest frames ------------------------------------------------------------------------

	std::mutex g_frameMutex;
	std::vector<uint8_t> g_frame;
	uint32_t g_frameWidth = 0, g_frameHeight = 0;
	bool g_frameNew = false;
	std::atomic<bool> g_grabbing{ false };

	// The app is in the background, where iOS lets no app use the GPU.
	std::atomic<bool> g_background{ false };

	void FrameThread()
	{
		std::vector<uint8_t> pixels;
		uint64_t lastSwap = 0;
		while (g_grabbing)
		{
			// After the first few swaps (the GPU is still starting before them), at most ~60 a
			// second: each read-back waits for the GPU.
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
			const uint64_t swap = ips3_gpu_swap_count();
			uint32_t width, height;
			static uint64_t grabs = 0, failures = 0;
			static auto reported = std::chrono::steady_clock::now();
			if (std::chrono::steady_clock::now() - reported > std::chrono::seconds(5))
			{
				reported = std::chrono::steady_clock::now();
				fmt::println(stderr, "[host] swap {}: {} frames shown, {} read-backs not ready", swap, grabs, failures);
			}
			if (swap < 4 || swap == lastSwap || g_background)
				continue;
			if (!ips3_gpu_grab_frame(pixels, width, height))
			{
				failures++; // the GPU has not finished a copy yet: try again shortly
				continue;
			}
			grabs++;
			lastSwap = swap;
			static uint64_t grabbed = 0;
			if (grabbed == 0)
				fmt::println(stderr, "[host] first frame {}x{} at swap {}", width, height, swap);
			if (grabbed++ % 200 == 0 && getenv("IPS3_TRACE_FRAMES"))
			{
				uint64_t sum = 0;
				for (size_t i = 0; i < pixels.size(); i += 4 * 97)
					sum += pixels[i] + pixels[i + 1] + pixels[i + 2];
				fmt::println(stderr, "[host] frame {} {}x{} (swap {}) brightness {}", grabbed, width, height, swap,
					sum / std::max<size_t>(1, pixels.size() / (4 * 97)));
			}

			std::lock_guard lock(g_frameMutex);
			g_frame.swap(pixels);
			g_frameWidth = width;
			g_frameHeight = height;
			g_frameNew = true;
		}
	}

	// ---- Drawing helpers ---------------------------------------------------------------------

	SDL_Texture* g_circle = nullptr;

	SDL_Texture* MakeCircle()
	{
		constexpr int SIZE = 128;
		SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, SIZE, SIZE, 32, SDL_PIXELFORMAT_RGBA32);
		auto* pixels = static_cast<uint32_t*>(surface->pixels);
		for (int y = 0; y < SIZE; y++)
		{
			for (int x = 0; x < SIZE; x++)
			{
				const float dx = x + 0.5f - SIZE / 2.0f, dy = y + 0.5f - SIZE / 2.0f;
				const float edge = SIZE / 2.0f - std::sqrt(dx * dx + dy * dy);
				const uint8_t alpha = uint8_t(std::clamp(edge, 0.0f, 1.0f) * 255.0f);
				pixels[y * (surface->pitch / 4) + x] = SDL_MapRGBA(surface->format, 255, 255, 255, alpha);
			}
		}
		SDL_Texture* texture = SDL_CreateTextureFromSurface(g_renderer, surface);
		SDL_FreeSurface(surface);
		SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
		return texture;
	}

	void FillCircle(float cx, float cy, float r, SDL_Color color)
	{
		SDL_SetTextureColorMod(g_circle, color.r, color.g, color.b);
		SDL_SetTextureAlphaMod(g_circle, color.a);
		const SDL_FRect rect{ cx - r, cy - r, r * 2, r * 2 };
		SDL_RenderCopyF(g_renderer, g_circle, nullptr, &rect);
	}

	void FillRect(float x, float y, float w, float h, SDL_Color color)
	{
		SDL_SetRenderDrawColor(g_renderer, color.r, color.g, color.b, color.a);
		const SDL_FRect rect{ x, y, w, h };
		SDL_RenderFillRectF(g_renderer, &rect);
	}

	// A 5x7 pixel font: letters, digits and the punctuation the menu uses.
	const std::map<char, std::array<const char*, 7>> FONT =
	{
		{ 'A', { ".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#" } },
		{ 'B', { "####.", "#...#", "#...#", "####.", "#...#", "#...#", "####." } },
		{ 'C', { ".####", "#....", "#....", "#....", "#....", "#....", ".####" } },
		{ 'D', { "####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####." } },
		{ 'E', { "#####", "#....", "#....", "####.", "#....", "#....", "#####" } },
		{ 'F', { "#####", "#....", "#....", "####.", "#....", "#....", "#...." } },
		{ 'G', { ".####", "#....", "#....", "#.###", "#...#", "#...#", ".###." } },
		{ 'H', { "#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#" } },
		{ 'I', { "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####" } },
		{ 'J', { "..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.." } },
		{ 'K', { "#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#" } },
		{ 'L', { "#....", "#....", "#....", "#....", "#....", "#....", "#####" } },
		{ 'M', { "#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#" } },
		{ 'N', { "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#" } },
		{ 'O', { ".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###." } },
		{ 'P', { "####.", "#...#", "#...#", "####.", "#....", "#....", "#...." } },
		{ 'Q', { ".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#" } },
		{ 'R', { "####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#" } },
		{ 'S', { ".####", "#....", "#....", ".###.", "....#", "....#", "####." } },
		{ 'T', { "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.." } },
		{ 'U', { "#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###." } },
		{ 'V', { "#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.." } },
		{ 'W', { "#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#" } },
		{ 'X', { "#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#" } },
		{ 'Y', { "#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.." } },
		{ 'Z', { "#####", "....#", "...#.", "..#..", ".#...", "#....", "#####" } },
		{ '0', { ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###." } },
		{ '1', { "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###." } },
		{ '2', { ".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####" } },
		{ '3', { "####.", "....#", "....#", ".###.", "....#", "....#", "####." } },
		{ '4', { "...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#." } },
		{ '5', { "#####", "#....", "####.", "....#", "....#", "#...#", ".###." } },
		{ '6', { ".###.", "#....", "#....", "####.", "#...#", "#...#", ".###." } },
		{ '7', { "#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..." } },
		{ '8', { ".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###." } },
		{ '9', { ".###.", "#...#", "#...#", ".####", "....#", "....#", ".###." } },
		{ ':', { ".....", "..#..", "..#..", ".....", "..#..", "..#..", "....." } },
		{ '.', { ".....", ".....", ".....", ".....", ".....", ".##..", ".##.." } },
		{ '-', { ".....", ".....", ".....", "#####", ".....", ".....", "....." } },
		{ '/', { "....#", "....#", "...#.", "..#..", ".#...", "#....", "#...." } },
		{ '%', { "##..#", "##..#", "...#.", "..#..", ".#...", "#..##", "#..##" } },
		{ '<', { "...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#." } },
		{ '>', { ".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..." } },
		{ '_', { ".....", ".....", ".....", ".....", ".....", ".....", "#####" } },
		{ '(', { "...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#." } },
		{ ')', { ".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..." } },
	};

	// Draws `text` `height` pixels tall, centred on cy and placed on x by `align`: 0 starts at x,
	// 0.5 is centred on it, 1 ends at it.
	void DrawText(const char* text, float x, float cy, float height, SDL_Color color, float align = 0.5f)
	{
		const float px = height / 7.0f;
		const size_t length = strlen(text);
		x -= (length * 6 - 1) * px * align;
		const float y = cy - height / 2.0f;
		SDL_SetRenderDrawColor(g_renderer, color.r, color.g, color.b, color.a);
		for (size_t i = 0; i < length; i++, x += 6 * px)
		{
			const auto glyph = FONT.find(char(toupper((unsigned char)text[i])));
			if (glyph == FONT.end())
				continue;
			for (int row = 0; row < 7; row++)
				for (int col = 0; col < 5; col++)
					if (glyph->second[row][col] == '#')
					{
						const SDL_FRect pixel{ x + col * px, y + row * px, px, px };
						SDL_RenderFillRectF(g_renderer, &pixel);
					}
		}
	}

	// ---- Touch controls ----------------------------------------------------------------------
	// Positions are fractions of the window: x of its width, y and sizes of its height.

	struct TouchButton
	{
		const char* label;
		float x, y, radius;
		uint16_t button;  // XAMINPUT_GAMEPAD_*, or 0 for a trigger
		uint8_t trigger;  // 1 left, 2 right
		SDL_Color color;
		bool round;
	};

	const TouchButton BUTTONS[] =
	{
		{ "A", 0.885f, 0.80f, 0.065f, XAMINPUT_GAMEPAD_A, 0, { 70, 180, 70, 150 }, true },
		{ "B", 0.955f, 0.66f, 0.065f, XAMINPUT_GAMEPAD_B, 0, { 200, 60, 60, 150 }, true },
		{ "X", 0.815f, 0.66f, 0.065f, XAMINPUT_GAMEPAD_X, 0, { 60, 110, 210, 150 }, true },
		{ "Y", 0.885f, 0.52f, 0.065f, XAMINPUT_GAMEPAD_Y, 0, { 220, 190, 50, 150 }, true },
		{ "LT", 0.05f, 0.08f, 0.055f, 0, 1, { 140, 140, 140, 130 }, false },
		{ "LB", 0.16f, 0.08f, 0.055f, XAMINPUT_GAMEPAD_LEFT_SHOULDER, 0, { 140, 140, 140, 130 }, false },
		{ "RB", 0.84f, 0.08f, 0.055f, XAMINPUT_GAMEPAD_RIGHT_SHOULDER, 0, { 140, 140, 140, 130 }, false },
		{ "RT", 0.95f, 0.08f, 0.055f, 0, 2, { 140, 140, 140, 130 }, false },
		{ "BACK", 0.42f, 0.07f, 0.04f, XAMINPUT_GAMEPAD_BACK, 0, { 110, 110, 110, 130 }, false },
		{ "START", 0.58f, 0.07f, 0.04f, XAMINPUT_GAMEPAD_START, 0, { 110, 110, 110, 130 }, false },
		// D-pad (no labels).
		{ "", 0.08f, 0.34f, 0.045f, XAMINPUT_GAMEPAD_DPAD_UP, 0, { 120, 120, 120, 120 }, false },
		{ "", 0.08f, 0.52f, 0.045f, XAMINPUT_GAMEPAD_DPAD_DOWN, 0, { 120, 120, 120, 120 }, false },
		{ "", 0.035f, 0.43f, 0.045f, XAMINPUT_GAMEPAD_DPAD_LEFT, 0, { 120, 120, 120, 120 }, false },
		{ "", 0.125f, 0.43f, 0.045f, XAMINPUT_GAMEPAD_DPAD_RIGHT, 0, { 120, 120, 120, 120 }, false },
	};

	struct TouchStick
	{
		float x, y, radius;
		bool left;
	};
	const TouchStick STICKS[] =
	{
		{ 0.20f, 0.76f, 0.15f, true },
		{ 0.66f, 0.80f, 0.12f, false },
	};

	struct Finger
	{
		float x, y;           // pixels
		int stick = -1;       // index into STICKS when the finger started on one
	};
	std::map<SDL_FingerID, Finger> g_fingers;
	bool g_touchUsed = false;

	void WindowSize(float& w, float& h)
	{
		int iw, ih;
		SDL_GetRendererOutputSize(g_renderer, &iw, &ih);
		w = float(iw);
		h = float(ih);
	}

	void OnFinger(const SDL_TouchFingerEvent& event)
	{
		float w, h;
		WindowSize(w, h);
		const float x = event.x * w, y = event.y * h;

		if (event.type == SDL_FINGERUP)
		{
			g_fingers.erase(event.fingerId);
			return;
		}

		g_touchUsed = true;
		Finger& finger = g_fingers[event.fingerId];
		finger.x = x;
		finger.y = y;
		if (event.type == SDL_FINGERDOWN)
		{
			finger.stick = -1;
			for (int i = 0; i < int(std::size(STICKS)); i++)
			{
				const float dx = x - STICKS[i].x * w, dy = y - STICKS[i].y * h;
				if (std::sqrt(dx * dx + dy * dy) < STICKS[i].radius * h * 1.4f)
					finger.stick = i;
			}
		}
	}

	bool Pressed(const TouchButton& button, float w, float h)
	{
		for (const auto& [id, finger] : g_fingers)
		{
			if (finger.stick >= 0)
				continue;
			const float dx = finger.x - button.x * w, dy = finger.y - button.y * h;
			const float r = button.radius * h * 1.25f;
			if (button.round ? (dx * dx + dy * dy < r * r) : (std::abs(dx) < r * 1.3f && std::abs(dy) < r))
				return true;
		}
		return false;
	}

	void StickValue(int index, float w, float h, float& vx, float& vy)
	{
		vx = vy = 0;
		for (const auto& [id, finger] : g_fingers)
		{
			if (finger.stick != index)
				continue;
			const float r = STICKS[index].radius * h;
			vx = std::clamp((finger.x - STICKS[index].x * w) / r, -1.0f, 1.0f);
			vy = std::clamp(-(finger.y - STICKS[index].y * h) / r, -1.0f, 1.0f);
		}
	}

	bool ShowTouchControls()
	{
		const int mode = host::CurrentSettings().touchControls;
		if (mode != 0)
			return mode == 1;
#if IPS3_MOBILE
		return !host::ControllerConnected();
#else
		static const bool forced = getenv("IPS3_TOUCH") != nullptr; // show them from the start
		return (forced || g_touchUsed) && !host::ControllerConnected();
#endif
	}

	void ReadTouch(uint16_t& buttons, uint8_t& triggers, host::TouchSticks& sticks)
	{
		buttons = 0;
		triggers = 0;
		sticks = {};
		if (!ShowTouchControls())
			return;

		float w, h;
		WindowSize(w, h);
		for (const auto& button : BUTTONS)
		{
			if (Pressed(button, w, h))
			{
				buttons |= button.button;
				triggers |= button.trigger;
			}
		}
		StickValue(0, w, h, sticks.leftX, sticks.leftY);
		StickValue(1, w, h, sticks.rightX, sticks.rightY);
	}

	void DrawTouchControls()
	{
		if (!ShowTouchControls())
			return;

		float w, h;
		WindowSize(w, h);
		SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
		const int opacity = host::CurrentSettings().touchOpacity;
		auto fade = [opacity](SDL_Color color)
		{
			color.a = uint8_t(color.a * opacity / 100);
			return color;
		};

		for (const auto& button : BUTTONS)
		{
			SDL_Color color = button.color;
			if (Pressed(button, w, h))
				color.a = 230;
			const float cx = button.x * w, cy = button.y * h, r = button.radius * h;
			if (button.round)
				FillCircle(cx, cy, r, fade(color));
			else
				FillRect(cx - r * 1.3f, cy - r, r * 2.6f, r * 2, fade(color));
			DrawText(button.label, cx, cy, r * (button.round ? 0.8f : 0.6f), fade({ 255, 255, 255, 220 }));
		}

		for (int i = 0; i < int(std::size(STICKS)); i++)
		{
			const auto& stick = STICKS[i];
			float vx, vy;
			StickValue(i, w, h, vx, vy);
			const float cx = stick.x * w, cy = stick.y * h, r = stick.radius * h;
			FillCircle(cx, cy, r, fade({ 90, 90, 90, 110 }));
			FillCircle(cx + vx * r * 0.6f, cy - vy * r * 0.6f, r * 0.45f, fade({ 200, 200, 200, 170 }));
		}
	}

	// ---- Launcher menu (the menu app) ---------------------------------------------------------
	// Settings to change before the game starts. Tap a line, or use the D-pad and A (left and
	// right step a setting back and forth).

	enum class MenuAction { None, Start, ChooseDisc };

	struct MenuItem
	{
		const char* label;
		const char* help;
		std::string (*value)();       // nullptr for the actions
		void (*change)(int step);
		MenuAction action;
	};

	std::string OnOff(bool on) { return on ? "ON" : "OFF"; }
	host::Settings& S() { return host::CurrentSettings(); }

	const MenuItem MENU[] =
	{
		{ "START GAME", "START BAYONETTA WITH THESE SETTINGS", nullptr, nullptr, MenuAction::Start },
		{ "RESOLUTION", "2X IS SHARPER BUT MUCH SLOWER",
			[] { return std::string(S().resolutionScale == 2 ? "2X" : "1X"); },
			[](int) { S().resolutionScale = S().resolutionScale == 1 ? 2 : 1; }, MenuAction::None },
		{ "SHOW FPS", "THE FRAME RATE AT THE TOP OF THE SCREEN",
			[] { return OnOff(S().showFps); }, [](int) { S().showFps = !S().showFps; }, MenuAction::None },
		{ "TOUCH CONTROLS", "AUTO HIDES THEM WHILE A CONTROLLER IS CONNECTED",
			[] { return std::string(S().touchControls == 0 ? "AUTO" : S().touchControls == 1 ? "ALWAYS" : "HIDDEN"); },
			[](int step) { S().touchControls = (S().touchControls + step + 3) % 3; }, MenuAction::None },
		{ "TOUCH OPACITY", "HOW SOLID THE TOUCH BUTTONS LOOK",
			[] { return fmt::format("{}%", S().touchOpacity); },
			[](int step)
			{
				const int next = S().touchOpacity + step * 25;
				S().touchOpacity = next > 100 ? 25 : next < 25 ? 100 : next;
			}, MenuAction::None },
		{ "SOUND", "TURNING IT OFF SAVES A LITTLE CPU TIME",
			[] { return OnOff(S().sound); }, [](int) { S().sound = !S().sound; }, MenuAction::None },
		{ "FAST CORES", "ASK IOS TO RUN THE GAME AND GPU ON THE FAST CPU CORES",
			[] { return OnOff(S().fastCores); }, [](int) { S().fastCores = !S().fastCores; }, MenuAction::None },
		{ "ASYNC GPU SUBMIT", "SEND GPU WORK WITHOUT WAITING. TURN OFF IF THE PICTURE GLITCHES",
			[] { return OnOff(S().asyncGpuSubmit); }, [](int) { S().asyncGpuSubmit = !S().asyncGpuSubmit; }, MenuAction::None },
		{ "RENDER PATH", "INTERLOCK DOES XBOX-STYLE BLENDING IN SHADERS: NO RENDER TARGET COPIES",
			[] { return std::string(S().renderPath == 1 ? "INTERLOCK" : "FRAMEBUFFERS"); },
			[](int) { S().renderPath = 1 - S().renderPath; }, MenuAction::None },
		{ "SMALL RENDER AREAS", "GPU PASSES COVER ONLY THE ROWS DRAWN. TURN OFF IF PARTS OF THE PICTURE GO MISSING",
			[] { return OnOff(S().smallRenderAreas); },
			[](int) { S().smallRenderAreas = !S().smallRenderAreas; }, MenuAction::None },
		{ "METAL ARGUMENT BUFFERS", "HOW TEXTURES AND BUFFERS ARE HANDED TO THE GPU. TRY BOTH",
			[] { return OnOff(S().metalArgumentBuffers); },
			[](int) { S().metalArgumentBuffers = !S().metalArgumentBuffers; }, MenuAction::None },
		{ "STATUS LOG", "DIAGNOSTIC LINES IN BAYONETTA-LOG.TXT EVERY 5 SECONDS",
			[] { return OnOff(S().statusLog); }, [](int) { S().statusLog = !S().statusLog; }, MenuAction::None },
#if IPS3_MOBILE
		{ "CHOOSE DISC IMAGE", "PICK ANOTHER .ISO FROM FILES", nullptr, nullptr, MenuAction::ChooseDisc },
#endif
	};
	constexpr int MENU_SIZE = int(std::size(MENU));

	int g_menuSelected = 0;
	int g_menuPressed = -1; // the line a finger went down on

	MenuAction ActivateMenu(int index, int step)
	{
		g_menuSelected = index;
		const MenuItem& item = MENU[index];
		if (item.action != MenuAction::None)
			return item.action;
		item.change(step);
		host::SaveSettings();
		return MenuAction::None;
	}

	struct MenuLayout
	{
		float top, rowHeight, left, right;
	};
	MenuLayout GetMenuLayout(float w, float h)
	{
		return { h * 0.19f, h * 0.70f / MENU_SIZE, w * 0.12f, w * 0.88f };
	}

	int MenuRowAt(float x, float y)
	{
		float w, h;
		WindowSize(w, h);
		const MenuLayout layout = GetMenuLayout(w, h);
		x *= w;
		y *= h;
		if (x < layout.left || x > layout.right || y < layout.top)
			return -1;
		const int row = int((y - layout.top) / layout.rowHeight);
		return row < MENU_SIZE ? row : -1;
	}

	// Handles one event while the menu shows; returns what to do.
	MenuAction MenuEvent(const SDL_Event& event)
	{
		switch (event.type)
		{
		case SDL_FINGERDOWN:
			g_menuPressed = MenuRowAt(event.tfinger.x, event.tfinger.y);
			if (g_menuPressed >= 0)
				g_menuSelected = g_menuPressed;
			break;
		case SDL_FINGERUP:
		{
			const int row = MenuRowAt(event.tfinger.x, event.tfinger.y);
			const int pressed = g_menuPressed;
			g_menuPressed = -1;
			if (row >= 0 && row == pressed)
				return ActivateMenu(row, 1);
			break;
		}
		case SDL_CONTROLLERBUTTONDOWN:
			switch (event.cbutton.button)
			{
			case SDL_CONTROLLER_BUTTON_DPAD_UP: g_menuSelected = (g_menuSelected + MENU_SIZE - 1) % MENU_SIZE; break;
			case SDL_CONTROLLER_BUTTON_DPAD_DOWN: g_menuSelected = (g_menuSelected + 1) % MENU_SIZE; break;
			case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
				if (MENU[g_menuSelected].change)
					return ActivateMenu(g_menuSelected, -1);
				break;
			case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
				if (MENU[g_menuSelected].change)
					return ActivateMenu(g_menuSelected, 1);
				break;
			case SDL_CONTROLLER_BUTTON_A: return ActivateMenu(g_menuSelected, 1);
			case SDL_CONTROLLER_BUTTON_START: return MenuAction::Start;
			}
			break;
		case SDL_KEYDOWN: // desktops
			switch (event.key.keysym.sym)
			{
			case SDLK_UP: g_menuSelected = (g_menuSelected + MENU_SIZE - 1) % MENU_SIZE; break;
			case SDLK_DOWN: g_menuSelected = (g_menuSelected + 1) % MENU_SIZE; break;
			case SDLK_LEFT:
				if (MENU[g_menuSelected].change)
					return ActivateMenu(g_menuSelected, -1);
				break;
			case SDLK_RIGHT:
				if (MENU[g_menuSelected].change)
					return ActivateMenu(g_menuSelected, 1);
				break;
			case SDLK_RETURN: return ActivateMenu(g_menuSelected, 1);
			}
			break;
		}
		return MenuAction::None;
	}

	void DrawMenu(const std::string& disc)
	{
		float w, h;
		WindowSize(w, h);
		SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
		const MenuLayout layout = GetMenuLayout(w, h);
		const float text = std::min(layout.rowHeight * 0.42f, h * 0.04f);

		DrawText("BAYONETTA", w / 2, h * 0.075f, h * 0.07f, { 235, 200, 120, 255 });
		DrawText(disc.c_str(), w / 2, h * 0.145f, text * 0.7f, { 170, 170, 170, 255 });

		for (int i = 0; i < MENU_SIZE; i++)
		{
			const MenuItem& item = MENU[i];
			const float y = layout.top + i * layout.rowHeight;
			const bool selected = i == g_menuSelected;
			FillRect(layout.left, y + 2, layout.right - layout.left, layout.rowHeight - 4,
				selected ? SDL_Color{ 120, 70, 140, 220 } : SDL_Color{ 40, 40, 48, 200 });
			const float cy = y + layout.rowHeight / 2;
			const SDL_Color white{ 255, 255, 255, 255 };
			if (item.value)
			{
				DrawText(item.label, layout.left + w * 0.03f, cy, text, white, 0.0f);
				DrawText(item.value().c_str(), layout.right - w * 0.03f, cy, text, { 250, 220, 140, 255 }, 1.0f);
			}
			else
			{
				DrawText(item.label, w / 2, cy, text, item.action == MenuAction::Start ? SDL_Color{ 140, 240, 140, 255 } : white);
			}
		}

		DrawText(MENU[g_menuSelected].help, w / 2, h * 0.945f, text * 0.7f, { 200, 200, 200, 255 });
	}

	// ---- Frame rate overlay ------------------------------------------------------------------

	void DrawFps()
	{
		static auto since = std::chrono::steady_clock::now();
		static uint64_t swaps = ips3_gpu_swap_count();
		static double fps = 0;
		const auto now = std::chrono::steady_clock::now();
		const double seconds = std::chrono::duration<double>(now - since).count();
		if (seconds >= 1.0)
		{
			const uint64_t current = ips3_gpu_swap_count();
			fps = double(current - swaps) / seconds;
			swaps = current;
			since = now;
		}
		float w, h;
		WindowSize(w, h);
		const std::string text = fmt::format("{:.0f} FPS", fps);
		const float height = h * 0.03f;
		FillRect(w / 2 - height * 3.5f, h * 0.006f, height * 7.0f, height * 1.6f, { 0, 0, 0, 150 });
		DrawText(text.c_str(), w / 2, h * 0.006f + height * 0.8f, height, { 255, 255, 120, 255 });
	}

	// ---- Leaving the screen ------------------------------------------------------------------
	// iOS ends an app that uses the GPU in the background: the GPU's command thread parks before
	// the app leaves the screen and resumes when it returns.

	int SDLCALL OnAppEvent(void*, SDL_Event* event)
	{
		if (event->type == SDL_APP_WILLENTERBACKGROUND)
		{
			fmt::println(stderr, "[host] leaving the screen: pausing the GPU");
			g_background = true;
			ips3_gpu_set_paused(true);
		}
		else if (event->type == SDL_APP_DIDENTERFOREGROUND)
		{
			fmt::println(stderr, "[host] back on screen: resuming the GPU");
			ips3_gpu_set_paused(false);
			g_background = false;
		}
		return 1;
	}

	// ---- Frame display -----------------------------------------------------------------------

	SDL_Texture* g_frameTexture = nullptr;
	int g_textureWidth = 0, g_textureHeight = 0;

	void DrawFrame()
	{
		{
			std::lock_guard lock(g_frameMutex);
			if (g_frameNew)
			{
				g_frameNew = false;
				if (!g_frameTexture || g_textureWidth != int(g_frameWidth) || g_textureHeight != int(g_frameHeight))
				{
					if (g_frameTexture)
						SDL_DestroyTexture(g_frameTexture);
					// R8G8B8X8 in memory order.
					g_frameTexture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBX32, SDL_TEXTUREACCESS_STREAMING,
						int(g_frameWidth), int(g_frameHeight));
					g_textureWidth = int(g_frameWidth);
					g_textureHeight = int(g_frameHeight);
				}
				SDL_UpdateTexture(g_frameTexture, nullptr, g_frame.data(), int(g_frameWidth * 4));
			}
		}

		if (!g_frameTexture)
			return;

		float w, h;
		WindowSize(w, h);
		const float scale = std::min(w / g_textureWidth, h / g_textureHeight);
		const SDL_FRect rect{ (w - g_textureWidth * scale) / 2, (h - g_textureHeight * scale) / 2,
			g_textureWidth * scale, g_textureHeight * scale };
		SDL_RenderCopyF(g_renderer, g_frameTexture, nullptr, &rect);
	}
}

namespace
{
	// Message boxes only open on the window's thread; other threads queue theirs.
	SDL_threadID g_windowThread = 0;
	std::mutex g_errorsMutex;
	std::vector<std::string> g_errors;

	void ShowQueuedErrors()
	{
		std::vector<std::string> errors;
		{
			std::lock_guard lock(g_errorsMutex);
			errors.swap(g_errors);
		}
		for (const auto& message : errors)
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Bayonetta", message.c_str(), g_window);
	}
}

void host::ShowError(const std::string& message)
{
	fmt::println(stderr, "[host] {}", message);
	if (!WindowMode())
		return;
	if (g_windowThread != 0 && SDL_ThreadID() != g_windowThread)
	{
		std::lock_guard lock(g_errorsMutex);
		g_errors.push_back(message);
		return;
	}
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Bayonetta", message.c_str(), g_window);
}

int host::RunWindow(const std::filesystem::path& image, const std::function<bool(const std::filesystem::path&)>& start)
{
	SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
	SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
	SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1"); // desktops: the mouse drives the touch controls
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
	{
		fmt::println(stderr, "[host] no video: {}", SDL_GetError());
		return 1;
	}

	Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
#if IPS3_MOBILE
	flags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS;
#endif
	g_window = SDL_CreateWindow("Bayonetta", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, flags);
	g_renderer = g_window ? SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
	if (g_window && !g_renderer)
		g_renderer = SDL_CreateRenderer(g_window, -1, 0); // e.g. a software-only video driver
	if (!g_renderer)
	{
		fmt::println(stderr, "[host] cannot create the window: {}", SDL_GetError());
		return 1;
	}
	fmt::println(stderr, "[host] window renderer: {}", [] { SDL_RendererInfo info; SDL_GetRendererInfo(g_renderer, &info); return info.name; }());
	g_circle = MakeCircle();
	SDL_AddEventWatch(OnAppEvent, nullptr);

	std::filesystem::path pending = image;
	bool started = false;
	bool starting = false;
	static std::atomic<int> startResult{ -1 }; // -1 while starting, then 1 started or 0 failed (static: the start thread may outlive the loop)
	bool picking = false;
	g_windowThread = SDL_ThreadID();
	auto startedAt = std::chrono::steady_clock::now();
	bool menuOpen = host::MenuEnabled();

#if IPS3_MOBILE
	if (pending.empty())
		pending = mobile::FindGameImage();
#endif

	for (bool running = true; running;)
	{
		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			if (menuOpen && event.type != SDL_QUIT)
			{
				switch (MenuEvent(event))
				{
				case MenuAction::Start:
					menuOpen = false;
					fmt::println(stderr, "[host] starting from the menu");
					break;
				case MenuAction::ChooseDisc:
#if IPS3_MOBILE
					if (!picking)
					{
						mobile::ForgetGameImage();
						pending.clear();
						picking = true;
						mobile::PickGameImage(g_window, [&](const std::filesystem::path& picked)
						{
							pending = picked;
							picking = false;
						});
					}
#endif
					break;
				default:
					break;
				}
				ProcessControllerEvent(event);
				continue;
			}
			switch (event.type)
			{
			case SDL_QUIT:
				running = false;
				break;
			case SDL_FINGERDOWN:
			case SDL_FINGERMOTION:
			case SDL_FINGERUP:
				OnFinger(event.tfinger);
				break;
			default:
				ProcessControllerEvent(event);
				break;
			}
		}

		// The game starts on a worker thread, so a slow or stuck start (the GPU, the disc) never
		// freezes the window.
		if (!started && !starting && !menuOpen && !pending.empty())
		{
			const auto chosen = pending;
			pending.clear();
			starting = true;
			startedAt = std::chrono::steady_clock::now();
			startResult = -1;
			std::thread([&start, chosen] { startResult = start(chosen) ? 1 : 0; }).detach();
		}
		ShowQueuedErrors();
		if (starting && startResult != -1)
		{
			starting = false;
			started = startResult == 1;
			if (started)
			{
				g_grabbing = true;
				std::thread(FrameThread).detach();
			}
			else
			{
#if IPS3_MOBILE
				mobile::ForgetGameImage(); // a wrong pick must not stick: the picker shows again
#endif
				menuOpen = host::MenuEnabled(); // back to the menu, where there is one
			}
		}
#if IPS3_MOBILE
		else if (!started && !starting && !picking && !menuOpen)
		{
			picking = true;
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Bayonetta", mobile::PickPrompt(), g_window);
			mobile::PickGameImage(g_window, [&](const std::filesystem::path& picked)
			{
				pending = picked;
				picking = false;
			});
		}
		mobile::PollPicker();
#endif

#if IPS3_MOBILE
		// No picture 20 s after starting: show the end of the log, which says what is stuck.
		static bool reported = false;
		if ((started || starting) && !reported && !g_frameTexture &&
			std::chrono::steady_clock::now() - startedAt > std::chrono::seconds(20))
		{
			reported = true;
			fmt::println(stderr, "[host] no picture after 20 s ({}, swap {})",
				starting ? "still starting" : "running", ips3_gpu_swap_count());
			std::ifstream log(mobile::DataPath() / "bayonetta-log.txt");
			std::deque<std::string> tail;
			for (std::string line; std::getline(log, line);)
			{
				// The runtime's own lines ("[status] ...", "[host] ..."), not the driver's.
				if (line.empty() || line[0] != '[' || line.starts_with("[mvk"))
					continue;
				tail.push_back(line.substr(0, 160));
				if (tail.size() > 40)
					tail.pop_front();
			}
			std::string text = "No picture yet. Screenshot this for the developer:\n\n";
			for (const auto& line : tail)
				text += line + "\n";
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Bayonetta", text.c_str(), g_window);
		}
#endif

		uint16_t buttons;
		uint8_t triggers;
		TouchSticks sticks;
		ReadTouch(buttons, triggers, sticks);
		UpdatePad(buttons, triggers, sticks);

		// Nothing may draw while the app is off the screen.
		if (g_background)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}

		SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
		SDL_RenderClear(g_renderer);
		if (menuOpen)
		{
			std::string disc = pending.empty() ? "NO DISC IMAGE YET: CHOSEN AT START" : "DISC: " + pending.filename().string();
			DrawMenu(disc);
		}
		else
		{
			DrawFrame();
			DrawTouchControls();
			if (host::CurrentSettings().showFps && started)
				DrawFps();
		}

		// IPS3_WINDOW_SHOTS=<folder>[:seconds]: saves what the window shows every few seconds
		// (default 5) as shot_<seconds since start>.bmp (testing).
		static const char* shots = getenv("IPS3_WINDOW_SHOTS");
		if (shots)
		{
			static std::string folder;
			static int every = 5;
			static auto nextShot = std::chrono::steady_clock::now();
			static const auto begin = std::chrono::steady_clock::now();
			if (folder.empty())
			{
				folder = shots;
				if (const size_t colon = folder.rfind(':'); colon != std::string::npos)
				{
					every = std::max(1, atoi(folder.c_str() + colon + 1));
					folder.resize(colon);
				}
			}
			if (std::chrono::steady_clock::now() >= nextShot)
			{
				nextShot += std::chrono::seconds(every);
				int w, h;
				SDL_GetRendererOutputSize(g_renderer, &w, &h);
				SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
				SDL_RenderReadPixels(g_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch);
				const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - begin).count();
				SDL_SaveBMP(surface, fmt::format("{}/shot_{:05}.bmp", folder, seconds).c_str());
				SDL_FreeSurface(surface);
			}
		}

		// IPS3_WINDOW_SHOT=<file.bmp>[:seconds]: saves what the window shows once (testing).
		static const char* shot = getenv("IPS3_WINDOW_SHOT");
		static const auto opened = std::chrono::steady_clock::now();
		if (shot)
		{
			std::string spec = shot;
			const size_t colon = spec.rfind(':');
			const int after = colon != std::string::npos ? atoi(spec.c_str() + colon + 1) : 30;
			if (std::chrono::steady_clock::now() - opened > std::chrono::seconds(after))
			{
				int w, h;
				SDL_GetRendererOutputSize(g_renderer, &w, &h);
				SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
				SDL_RenderReadPixels(g_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch);
				SDL_SaveBMP(surface, spec.substr(0, colon).c_str());
				SDL_FreeSurface(surface);
				shot = nullptr;
			}
		}

		SDL_RenderPresent(g_renderer); // waits for the display's refresh, where there is vsync

		// Without vsync (software renderers) don't spin: at most ~120 iterations a second.
		static auto last = std::chrono::steady_clock::now();
		const auto next = last + std::chrono::microseconds(8333);
		if (std::chrono::steady_clock::now() < next)
			std::this_thread::sleep_until(next);
		last = std::chrono::steady_clock::now();
	}

	g_grabbing = false;
	return 0;
}
