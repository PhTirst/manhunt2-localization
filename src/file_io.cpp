#include "file_io.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace mh2
{
    std::vector<uint8_t> ReadBytes(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            throw std::runtime_error("cannot open input: " + path.u8string());
        const auto size = file.tellg();
        if (size < 0 || static_cast<uintmax_t>(size) > std::numeric_limits<size_t>::max())
            throw std::runtime_error("cannot determine input size: " + path.u8string());
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        file.seekg(0);
        if (!bytes.empty())
            file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file)
            throw std::runtime_error("cannot read input: " + path.u8string());
        return bytes;
    }

    std::string ReadText(const fs::path &path)
    {
        const auto bytes = ReadBytes(path);
        return std::string(bytes.begin(), bytes.end());
    }

    void WriteBytes(const fs::path &path, const std::vector<uint8_t> &bytes)
    {
        if (!path.parent_path().empty())
            fs::create_directories(path.parent_path());
        static std::atomic<unsigned long long> sequence{0};
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        fs::path temporary = path;
        temporary += ".tmp-" + std::to_string(stamp) + "-" + std::to_string(sequence++);
        if (fs::exists(temporary))
            throw std::runtime_error("temporary output already exists: " + temporary.u8string());
        try
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file)
                throw std::runtime_error("cannot open output: " + path.u8string());
            if (!bytes.empty())
                file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            file.close();
            if (!file)
                throw std::runtime_error("cannot write output: " + path.u8string());
            // Replace only after the complete file is closed; never truncate the previous output.
#ifdef _WIN32
            if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("cannot replace output: " + path.u8string() +
                    " (Windows error " + std::to_string(GetLastError()) + ")");
#else
            fs::rename(temporary, path);
#endif
        }
        catch (...)
        {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            throw;
        }
    }

    void WriteText(const fs::path &path, const std::string &text)
    {
        WriteBytes(path, std::vector<uint8_t>(text.begin(), text.end()));
    }
}
