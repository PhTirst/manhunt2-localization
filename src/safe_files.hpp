#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>

namespace mh2
{
    std::filesystem::path AbsolutePath(const std::filesystem::path &path);
    bool Within(const std::filesystem::path &child, const std::filesystem::path &parent);
    void ValidateRelative(const std::filesystem::path &path);
    void RejectLinks(const std::filesystem::path &path);
    void ValidateLanguage(const std::string &language);
    std::vector<std::filesystem::path> ResourceFiles(const std::filesystem::path &root, const std::string &extension = {});
    std::string Sha256(const std::vector<uint8_t> &bytes);
    std::string FileHash(const std::filesystem::path &path);
    struct FileChange
    {
        std::filesystem::path path;
        std::optional<std::vector<uint8_t>> bytes; // nullopt deletes an owned file.
    };
    void CommitFiles(const std::vector<FileChange> &changes);
    void CommitDirectory(const std::filesystem::path &stage, const std::filesystem::path &output);
    std::filesystem::path UniqueSibling(const std::filesystem::path &path, const std::string &label);
    class OperationLock
    {
    public:
        explicit OperationLock(const std::filesystem::path &directory);
        ~OperationLock();
        OperationLock(const OperationLock &) = delete;
        OperationLock &operator=(const OperationLock &) = delete;
    private:
        void *handle_ = nullptr;
    };
}
