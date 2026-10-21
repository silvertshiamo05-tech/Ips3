// Ips3: Android file access -- the app's data folder, the log file, and the disc image picker.
//
// The 7.8 GB disc image is read in place: Android's document picker hands out a content URI,
// which BayonettaActivity (android/src/.../BayonettaActivity.java) keeps permission for and
// opens; the runtime reads the open file through /proc/self/fd.
#include "mobile.h"

#include <SDL_system.h>
#include <android/log.h>
#include <jni.h>

#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

namespace
{
	std::function<void(const std::filesystem::path&)> g_picked;

	// Calls an int-returning method of the activity (a BayonettaActivity) with no arguments.
	int CallActivity(const char* name)
	{
		auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
		auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
		if (env == nullptr || activity == nullptr)
			return -1;
		jclass cls = env->GetObjectClass(activity);
		jmethodID method = env->GetMethodID(cls, name, "()I");
		int result = method ? env->CallIntMethod(activity, method) : -1;
		if (env->ExceptionCheck())
		{
			env->ExceptionClear();
			result = -1;
		}
		env->DeleteLocalRef(cls);
		env->DeleteLocalRef(activity);
		return result;
	}

	std::filesystem::path FdPath(int fd)
	{
		return fd >= 0 ? std::filesystem::path("/proc/self/fd/" + std::to_string(fd)) : std::filesystem::path();
	}

	// Copies stderr to logcat as well, line by line (tag "Bayonetta").
	void TeeToLogcat(int readFd, FILE* file)
	{
		std::string line;
		char buffer[4096];
		for (ssize_t n; (n = read(readFd, buffer, sizeof(buffer))) > 0;)
		{
			fwrite(buffer, 1, size_t(n), file);
			fflush(file);
			for (ssize_t i = 0; i < n; i++)
			{
				if (buffer[i] == '\n')
				{
					__android_log_write(ANDROID_LOG_INFO, "Bayonetta", line.c_str());
					line.clear();
				}
				else
					line += buffer[i];
			}
		}
	}
}

std::filesystem::path mobile::DataPath()
{
	const char* path = SDL_AndroidGetExternalStoragePath();
	return path ? std::filesystem::path(path) : std::filesystem::path(SDL_AndroidGetInternalStoragePath());
}

void mobile::RedirectLog()
{
	const auto path = DataPath() / "bayonetta-log.txt";
	FILE* file = fopen(path.c_str(), "w");
	int fds[2];
	if (file == nullptr || pipe(fds) != 0)
		return;
	setvbuf(stdout, nullptr, _IOLBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
	dup2(fds[1], STDOUT_FILENO);
	dup2(fds[1], STDERR_FILENO);
	close(fds[1]);
	std::thread(TeeToLogcat, fds[0], file).detach();
}

std::filesystem::path mobile::FindGameImage()
{
	if (const int fd = CallActivity("openSavedGame"); fd >= 0)
		return FdPath(fd);

	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(DataPath(), ec))
	{
		auto ext = entry.path().extension().string();
		for (auto& c : ext)
			c = char(tolower(c));
		if (ext == ".iso" && entry.is_regular_file(ec))
			return entry.path();
	}
	return {};
}

const char* mobile::PickPrompt()
{
	return "Choose your Bayonetta (Xbox 360) disc image (.iso). It is read in place, not copied, "
		"so pick a file stored on this device or its SD card.";
}

void mobile::PickGameImage(SDL_Window*, std::function<void(const std::filesystem::path&)> picked)
{
	g_picked = std::move(picked);
	if (CallActivity("pickGame") < 0)
	{
		auto done = std::move(g_picked);
		g_picked = nullptr;
		done({});
	}
}

void mobile::PollPicker()
{
	if (!g_picked)
		return;
	const int fd = CallActivity("pollPickedGame"); // -2 while the picker is open
	if (fd == -2)
		return;
	auto done = std::move(g_picked);
	g_picked = nullptr;
	done(FdPath(fd));
}
