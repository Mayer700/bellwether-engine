#include "Scene.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <limits>
#include <stdexcept>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace Bellwether
{
	std::filesystem::path GetExecutableDirectory()
	{
#if defined(_WIN32)
		for (size_t capacity = 512; capacity <= 65536; capacity *= 2)
		{
			std::wstring buffer(capacity, L'\0');
			auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (!length)
				throw std::runtime_error("Cannot locate the engine executable");
			if (length < buffer.size())
			{
				buffer.resize(length);
				return std::filesystem::path(buffer).parent_path();
			}
		}
#elif defined(__APPLE__)
		uint32_t length = 0;
		_NSGetExecutablePath(nullptr, &length);
		if (!length || length > 65536)
			throw std::runtime_error("Invalid executable path size");
		std::string buffer(length, '\0');
		if (_NSGetExecutablePath(buffer.data(), &length) == 0)
			return std::filesystem::weakly_canonical(std::filesystem::path(buffer.c_str())).parent_path();
#elif defined(__linux__)
		for (size_t capacity = 512; capacity <= 65536; capacity *= 2)
		{
			std::string buffer(capacity, '\0');
			auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
			if (length < 0)
				throw std::runtime_error("Cannot locate the engine executable");
			if (static_cast<size_t>(length) < buffer.size())
			{
				buffer.resize(static_cast<size_t>(length));
				return std::filesystem::path(buffer).parent_path();
			}
		}
#endif
		throw std::runtime_error("Engine executable path is unavailable or exceeds the supported length");
	}

	Json ReadJson(const std::filesystem::path& path)
	{
		if (!std::filesystem::exists(path))
			throw std::runtime_error("JSON file does not exist: " + path.string());
		if (!std::filesystem::is_regular_file(path))
			throw std::runtime_error("JSON path is not a regular file: " + path.string());
		if (std::filesystem::file_size(path) > 64 * 1024 * 1024)
			throw std::runtime_error("JSON file exceeds the 64 MiB document limit");
		std::ifstream input(path, std::ios::binary);
		if (!input)
			throw std::runtime_error("Cannot open JSON file: " + path.string());
		return Json::parse(input);
	}

	void AtomicWrite(const std::filesystem::path& path, const std::string& contents)
	{
		if (std::filesystem::is_directory(path))
			throw std::runtime_error("Cannot overwrite directory with file: " + path.string());
		if (!path.parent_path().empty())
			std::filesystem::create_directories(path.parent_path());
		static std::atomic<uint64_t> s_Sequence{0};
		auto temporary = path;
		auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
		temporary += "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(timestamp) + "." +
					 std::to_string(s_Sequence++) + ".tmp";
		int descriptor = _wopen(temporary.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
		temporary += "." + std::to_string(getpid()) + "." + std::to_string(timestamp) + "." +
					 std::to_string(s_Sequence++) + ".tmp";
		int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
		if (descriptor < 0)
			throw std::runtime_error("Cannot exclusively create temporary file: " + temporary.string());
		try
		{
			size_t offset = 0;
			while (offset < contents.size())
			{
				auto count = std::min<size_t>(contents.size() - offset, 1024 * 1024);
#if defined(_WIN32)
				int written = _write(descriptor, contents.data() + offset, static_cast<unsigned>(count));
#else
				auto written = write(descriptor, contents.data() + offset, count);
#endif
				if (written < 0 && errno == EINTR)
					continue;
				if (written <= 0)
					throw std::runtime_error("Temporary file write failed");
				offset += static_cast<size_t>(written);
			}
#if defined(_WIN32)
			if (_commit(descriptor) != 0)
				throw std::runtime_error("Temporary file flush failed");
			int closeResult = _close(descriptor);
#else
			if (fsync(descriptor) != 0)
				throw std::runtime_error("Temporary file flush failed");
			int closeResult = close(descriptor);
#endif
			descriptor = -1;
			if (closeResult != 0)
				throw std::runtime_error("Temporary file close failed");
#if defined(_WIN32)
			bool replaced = false;
			DWORD lastError = 0;
			for (int attempt = 0; attempt < 8; attempt++)
			{
				if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
				{
					replaced = true;
					break;
				}
				lastError = GetLastError();
				if (lastError != ERROR_ACCESS_DENIED && lastError != ERROR_SHARING_VIOLATION)
					break;
				Sleep(static_cast<DWORD>(attempt + 1));
			}
			if (!replaced)
				throw std::runtime_error("Cannot replace destination file, Win32 error " + std::to_string(lastError));
#else
			std::filesystem::rename(temporary, path);
			auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
			int directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
			if (directory < 0)
				throw std::runtime_error("Cannot open destination directory for flush");
			int syncResult = fsync(directory);
			close(directory);
			if (syncResult != 0)
				throw std::runtime_error("Destination directory flush failed");
#endif
		}
		catch (...)
		{
			if (descriptor >= 0)
			{
#if defined(_WIN32)
				_close(descriptor);
#else
				close(descriptor);
#endif
			}
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			throw;
		}
	}

} // namespace Bellwether
