#include "safe_files.hpp"
#include "file_io.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cwctype>
#include <exception>
#include <fstream>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
namespace
{
    std::wstring Lower(std::wstring value)
    {
        for (auto &c : value) c = (wchar_t)std::towlower(c);
        return value;
    }
}
namespace mh2
{
    fs::path AbsolutePath(const fs::path &path) { return fs::weakly_canonical(fs::absolute(path)); }
    bool Within(const fs::path &child, const fs::path &parent)
    {
        const auto c = Lower(AbsolutePath(child).native()), p = Lower(AbsolutePath(parent).native());
        return c == p || (c.size() > p.size() && c.compare(0, p.size(), p) == 0 && (p.back() == L'\\' || c[p.size()] == L'\\'));
    }
    void ValidateRelative(const fs::path &path)
    {
        if (path.empty() || path.has_root_path()) throw std::runtime_error("Expected a package-relative path: " + path.u8string());
        for (const auto &part : path)
        {
            const auto name = part.native();
            if (part == "." || part == ".." || name.empty() || name.back() == L'.' || name.back() == L' ' ||
                name.find_first_of(L":*?\"<>|") != std::wstring::npos ||
                std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; }))
                throw std::runtime_error("Unsafe relative path: " + path.u8string());
            const auto stem = Lower(part.stem().native());
            if (stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul" ||
                (stem.size() == 4 && (stem.substr(0, 3) == L"com" || stem.substr(0, 3) == L"lpt") && stem[3] >= L'1' && stem[3] <= L'9'))
                throw std::runtime_error("Unsafe Windows device path: " + path.u8string());
        }
    }
    void RejectLinks(const fs::path &path)
    {
        auto current = fs::absolute(path).lexically_normal();
        while (!current.empty())
        {
            const DWORD attributes = GetFileAttributesW(current.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("Symbolic links/junctions are not allowed for managed resources: " + current.u8string());
            const auto parent = current.parent_path();
            if (parent == current) break;
            current = parent;
        }
    }
    void ValidateLanguage(const std::string &language)
    {
        if (language.empty() || language.size() > 64 || !std::isalpha((unsigned char)language.front()) ||
            !std::all_of(language.begin(), language.end(), [](unsigned char c) { return c < 128 && (std::isalnum(c) || c == '_' || c == '-'); }))
            throw std::runtime_error("Language must start with an ASCII letter and contain only letters, digits, '_' or '-' (1-64 characters)");
        auto lower = language;
        for (auto &c : lower) c = (char)std::tolower((unsigned char)c);
        for (const auto &reserved : {"key", "extra", "value", "text", "entries"})
            if (lower == reserved) throw std::runtime_error("Reserved language column: " + language);
    }
    std::vector<fs::path> ResourceFiles(const fs::path &root, const std::string &extension)
    {
        RejectLinks(root);
        if (!fs::is_directory(root)) throw std::runtime_error("Directory not found: " + root.u8string());
        std::vector<fs::path> files;
        for (const auto &entry : fs::recursive_directory_iterator(root))
        {
            RejectLinks(entry.path());
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().u8string();
            for (auto &c : ext) c = (char)std::tolower((unsigned char)c);
            if (!extension.empty() && ext != extension) continue;
            const auto relative = entry.path().lexically_relative(root);
            ValidateRelative(relative);
            files.push_back(relative);
        }
        std::sort(files.begin(), files.end());
        return files;
    }
    std::string Sha256(const std::vector<uint8_t> &bytes)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("Cannot initialize SHA-256");
        std::array<uint8_t, 32> digest{};
        const auto cleanup = [&] { if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); };
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) { cleanup(); throw std::runtime_error("Cannot create SHA-256 hash"); }
        size_t at = 0;
        while (at < bytes.size())
        {
            const ULONG count = (ULONG)std::min<size_t>(bytes.size() - at, 1 << 20);
            if (BCryptHashData(hash, const_cast<PUCHAR>(bytes.data() + at), count, 0) < 0) { cleanup(); throw std::runtime_error("SHA-256 update failed"); }
            at += count;
        }
        const auto status = BCryptFinishHash(hash, digest.data(), (ULONG)digest.size(), 0);
        cleanup();
        if (status < 0) throw std::runtime_error("SHA-256 finalization failed");
        const char *hex = "0123456789abcdef";
        std::string result;
        for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
        return result;
    }
    std::string FileHash(const fs::path &path) { return Sha256(ReadBytes(path)); }
    OperationLock::OperationLock(const fs::path &directory)
    {
        const auto native = Lower(AbsolutePath(directory).native());
        const auto *bytes = reinterpret_cast<const uint8_t *>(native.data());
        const auto key = Sha256({bytes, bytes + native.size() * sizeof(wchar_t)});
        const auto name = L"Local\\mh2loc-" + std::wstring(key.begin(), key.end());
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!handle_) throw std::runtime_error("Cannot create operation lock");
        const auto wait = WaitForSingleObject(handle_, 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
        {
            CloseHandle(handle_); handle_ = nullptr;
            throw std::runtime_error("Another mh2loc operation is using " + directory.u8string());
        }
    }
    OperationLock::~OperationLock() { if (handle_) { ReleaseMutex(handle_); CloseHandle(handle_); } }
    fs::path UniqueSibling(const fs::path &path, const std::string &label)
    {
        static std::atomic<unsigned> sequence{0};
        auto temporary = path;
        temporary += ".mh2loc-" + label + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++sequence);
        if (fs::exists(temporary)) throw std::runtime_error("Temporary path already exists: " + temporary.u8string());
        return temporary;
    }
    void CommitFiles(const std::vector<FileChange> &changes)
    {
        struct Pending { fs::path path, temporary, backup; bool saved = false, installed = false; bool remove = false; };
        std::vector<Pending> pending;
        std::set<std::wstring> destinations;
        try
        {
            for (const auto &change : changes)
            {
                RejectLinks(change.path);
                if (!destinations.insert(Lower(AbsolutePath(change.path).native())).second)
                    throw std::runtime_error("Duplicate transaction path: " + change.path.u8string());
                if (fs::exists(change.path) && !fs::is_regular_file(change.path))
                    throw std::runtime_error("Output is not a file: " + change.path.u8string());
                Pending item;
                item.path = change.path;
                item.backup = UniqueSibling(item.path, "old");
                item.remove = !change.bytes.has_value();
                if (!item.remove) item.temporary = UniqueSibling(item.path, "new");
                pending.push_back(item);
                if (change.bytes) WriteBytes(item.temporary, *change.bytes);
            }
            for (auto &item : pending)
            {
                if (fs::exists(item.path)) { fs::rename(item.path, item.backup); item.saved = true; }
                if (!item.remove)
                {
                    fs::rename(item.temporary, item.path);
                    item.installed = true;
                }
            }
        }
        catch (...)
        {
            const auto original = std::current_exception();
            std::string recovery;
            for (auto it = pending.rbegin(); it != pending.rend(); ++it)
            {
                std::error_code error;
                if (it->installed) fs::remove(it->path, error);
                if (it->saved)
                {
                    error.clear(); fs::rename(it->backup, it->path, error);
                    if (error) recovery += "\nRestore manually from " + it->backup.u8string();
                }
                if (!it->temporary.empty()) { error.clear(); fs::remove(it->temporary, error); }
            }
            if (!recovery.empty()) throw std::runtime_error("Rollback failed:" + recovery);
            std::rethrow_exception(original);
        }
        for (const auto &item : pending)
        {
            std::error_code ignored;
            if (item.saved) fs::remove(item.backup, ignored);
        }
    }
    void CommitDirectory(const fs::path &stage, const fs::path &output)
    {
        RejectLinks(stage); RejectLinks(output);
        const auto backup = UniqueSibling(output, "old");
        bool saved = false;
        try
        {
            if (fs::exists(output)) { fs::rename(output, backup); saved = true; }
            fs::rename(stage, output);
        }
        catch (...)
        {
            if (saved)
            {
                std::error_code error;
                fs::rename(backup, output, error);
                if (error) throw std::runtime_error("Restore the prior output directory from " + backup.u8string());
            }
            throw;
        }
        if (saved) { std::error_code ignored; fs::remove_all(backup, ignored); }
    }
}
