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
#endif

namespace Bellwether
{

	Json ReadJson(const std::filesystem::path& path)
	{
		if (std::filesystem::file_size(path) > 64 * 1024 * 1024)
			throw std::runtime_error("JSON file exceeds the 64 MiB document limit");
		std::ifstream input(path, std::ios::binary);
		if (!input)
			throw std::runtime_error("Cannot open JSON file: " + path.string());
		return Json::parse(input);
	}

	void AtomicWrite(const std::filesystem::path& path, const std::string& contents)
	{
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
			if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
				throw std::runtime_error("Cannot replace destination file, Win32 error " +
										 std::to_string(GetLastError()));
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
