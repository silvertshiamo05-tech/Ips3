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

#include <atomic>
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
			if (swap < 4 || swap == lastSwap || !ips3_gpu_grab_frame(pixels, width, height))
				continue;
			lastSwap = swap;
			static uint64_t grabbed = 0;
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

	// A 5x7 pixel font with the letters the button labels use.
	const std::map<char, std::array<const char*, 7>> FONT =
	{
		{ 'A', { ".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#" } },
		{ 'B', { "####.", "#...#", "#...#", "####.", "#...#", "#...#", "####." } },
		{ 'C', { ".####", "#....", "#....", "#....", "#....", "#....", ".####" } },
		{ 'K', { "#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#" } },
		{ 'L', { "#....", "#....", "#....", "#....", "#....", "#....", "#####" } },
		{ 'R', { "####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#" } },
		{ 'S', { ".####", "#....", "#....", ".###.", "....#", "....#", "####." } },
		{ 'T', { "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.." } },
		{ 'X', { "#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#" } },
		{ 'Y', { "#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.." } },
	};

	// Draws `text` centred on (cx, cy), `height` pixels tall.
	void DrawText(const char* text, float cx, float cy, float height, SDL_Color color)
	{
		const float px = height / 7.0f;
		const size_t length = strlen(text);
		float x = cx - (length * 6 - 1) * px / 2.0f;
		const float y = cy - height / 2.0f;
		SDL_SetRenderDrawColor(g_renderer, color.r, color.g, color.b, color.a);
		for (size_t i = 0; i < length; i++, x += 6 * px)
		{
			const auto glyph = FONT.find(text[i]);
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

		for (const auto& button : BUTTONS)
		{
			SDL_Color color = button.color;
			if (Pressed(button, w, h))
				color.a = 230;
			const float cx = button.x * w, cy = button.y * h, r = button.radius * h;
			if (button.round)
				FillCircle(cx, cy, r, color);
			else
				FillRect(cx - r * 1.3f, cy - r, r * 2.6f, r * 2, color);
			DrawText(button.label, cx, cy, r * (button.round ? 0.8f : 0.6f), { 255, 255, 255, 220 });
		}

		for (int i = 0; i < int(std::size(STICKS)); i++)
		{
			const auto& stick = STICKS[i];
			float vx, vy;
			StickValue(i, w, h, vx, vy);
			const float cx = stick.x * w, cy = stick.y * h, r = stick.radius * h;
			FillCircle(cx, cy, r, { 90, 90, 90, 110 });
			FillCircle(cx + vx * r * 0.6f, cy - vy * r * 0.6f, r * 0.45f, { 200, 200, 200, 170 });
		}
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

void host::ShowError(const std::string& message)
{
	fmt::println(stderr, "[host] {}", message);
	if (WindowMode())
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

	std::filesystem::path pending = image;
	bool started = false;
	bool picking = false;

#if IPS3_MOBILE
	if (pending.empty())
		pending = mobile::FindGameImage();
#endif

	for (bool running = true; running;)
	{
		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
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

		if (!started && !pending.empty())
		{
			const auto chosen = pending;
			pending.clear();
			started = start(chosen);
			if (started)
			{
				g_grabbing = true;
				std::thread(FrameThread).detach();
			}
#if IPS3_MOBILE
			else
				mobile::ForgetGameImage(); // a wrong pick must not stick: the picker shows again
#endif
		}
#if IPS3_MOBILE
		else if (!started && !picking)
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

		uint16_t buttons;
		uint8_t triggers;
		TouchSticks sticks;
		ReadTouch(buttons, triggers, sticks);
		UpdatePad(buttons, triggers, sticks);

		SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
		SDL_RenderClear(g_renderer);
		DrawFrame();
		DrawTouchControls();

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
