#define NOMINMAX
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace publication_test
{
enum class Mutation { None, TimestampOnWriterClose, ReplaceAfterVerification, ContentAfterVerification };
Mutation mutation = Mutation::None;
HANDLE writer = INVALID_HANDLE_VALUE;
HANDLE reader = INVALID_HANDLE_VALUE;
std::filesystem::path temporary;
bool injected = false;

HANDLE WINAPI createFile(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
    DWORD disposition, DWORD flags, HANDLE templateFile)
{
    const auto handle = ::CreateFileW(path, access, share, security, disposition, flags, templateFile);
    if (handle != INVALID_HANDLE_VALUE && disposition == CREATE_NEW && (access & GENERIC_WRITE)) {
        writer = handle;
        temporary = path;
    }
    else if (handle != INVALID_HANDLE_VALUE && disposition == OPEN_EXISTING &&
        access == GENERIC_READ && std::filesystem::path(path) == temporary) {
        reader = handle;
    }
    return handle;
}

BOOL WINAPI closeHandle(HANDLE handle)
{
    const bool isWriter = handle == writer;
    const bool isReader = handle == reader;
    FILETIME timestamp{};
    if (isWriter && mutation == Mutation::TimestampOnWriterClose) {
        BY_HANDLE_FILE_INFORMATION information{};
        if (!::GetFileInformationByHandle(handle, &information)) {
            throw std::runtime_error("Cannot read timestamp injection baseline.");
        }
        timestamp = information.ftLastWriteTime;
        ++timestamp.dwHighDateTime;
        if (!::SetFileTime(handle, nullptr, nullptr, &timestamp)) {
            throw std::runtime_error("Cannot simulate close-time timestamp finalization.");
        }
        injected = true;
    }
    if (isReader && mutation == Mutation::ContentAfterVerification) {
        BY_HANDLE_FILE_INFORMATION information{};
        if (!::GetFileInformationByHandle(handle, &information)) {
            throw std::runtime_error("Cannot read content injection baseline.");
        }
        timestamp = information.ftLastWriteTime;
    }
    const auto result = ::CloseHandle(handle);
    if (!result) return result;
    if (isWriter) writer = INVALID_HANDLE_VALUE;
    if (isReader) reader = INVALID_HANDLE_VALUE;
    if (isReader && !injected && mutation == Mutation::ReplaceAfterVerification) {
        std::filesystem::rename(temporary, temporary.parent_path() / "displaced.tmp");
        std::ofstream replacement(temporary, std::ios::binary);
        replacement << "altered";
        replacement.close();
        injected = true;
    }
    else if (isReader && !injected && mutation == Mutation::ContentAfterVerification) {
        const auto writable = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (writable == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot inject content change.");
        DWORD written = 0;
        const bool changed = ::WriteFile(writable, "altered", 7, &written, nullptr) &&
            written == 7 && ::SetFileTime(writable, nullptr, nullptr, &timestamp);
        ::CloseHandle(writable);
        if (!changed) throw std::runtime_error("Cannot simulate same-size, same-timestamp content change.");
        injected = true;
    }
    return result;
}
}

// Exercise the private publication lifecycle with Win32 close-boundary faults, not a production test hook.
#define CreateFileW publication_test::createFile
#define CloseHandle publication_test::closeHandle
#include "../AudioLibraryToolkit/src/repair/AudioRepair.cpp"
#undef CloseHandle
#undef CreateFileW

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

void runCase(const fs::path& root, publication_test::Mutation mutation, bool success)
{
    fs::create_directory(root);
    publication_test::mutation = mutation;
    publication_test::injected = false;
    SafeOutputTree output(root, root.parent_path() / "source");
    require(writeSafeReport(output, "report.json", "content", false) == success,
        "Unexpected publication result.");
    require(publication_test::injected, "Close-boundary fault was not exercised.");
    if (success) {
        require(read(root / "report.json") == "content", "Published content differs.");
        require(!writeSafeReport(output, "report.json", "content", false),
            "Existing destination was overwritten without permission.");
        require(read(root / "report.json") == "content", "Collision changed existing report.");
        require(writeSafeReport(output, "report.json", "updated", true), "Explicit overwrite failed.");
        require(read(root / "report.json") == "updated", "Overwrite content differs.");
    }
    else if (mutation == publication_test::Mutation::ReplaceAfterVerification) {
        require(!fs::exists(root / "report.json"), "Replaced temporary was published.");
        require(read(publication_test::temporary) == "altered", "Unowned replacement was deleted.");
    }
}
}

int main()
{
    const auto root = fs::temp_directory_path() /
        ("repair-publication-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        require(fs::create_directory(root), "Cannot reserve test directory.");
        fs::create_directory(root / "source");
        std::ofstream(root / "source" / "untouched.txt") << "source";
        fs::create_directory(root / "locked");
        {
            SafeOutputTree output(root / "locked", root / "source");
            auto staged = output.CreateOutputFile("cancelled.tmp");
            staged->Write("content", 7);
            staged->Flush();
            require(read(staged->VerificationPath()) == "content", "Staged content differs.");
            const auto writable = ::CreateFileW((root / "locked" / "cancelled.tmp").c_str(),
                GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            const auto openError = GetLastError();
            if (writable != INVALID_HANDLE_VALUE) ::CloseHandle(writable);
            require(writable == INVALID_HANDLE_VALUE && openError == ERROR_SHARING_VIOLATION,
                "Verified staging did not exclude external writers.");
        }
        require(fs::is_empty(root / "locked"), "Cancelled verified staging was not cleaned up.");
        runCase(root / "timestamp", publication_test::Mutation::TimestampOnWriterClose, true);
        runCase(root / "replacement", publication_test::Mutation::ReplaceAfterVerification, false);
        runCase(root / "content", publication_test::Mutation::ContentAfterVerification, false);
        require(read(root / "source" / "untouched.txt") == "source", "Source content changed.");
        require(std::distance(fs::directory_iterator(root / "source"), fs::directory_iterator{}) == 1,
            "Source directory changed.");
        publication_test::mutation = publication_test::Mutation::None;
        fs::remove_all(root);
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        publication_test::mutation = publication_test::Mutation::None;
        std::error_code cleanupError;
        fs::remove_all(root, cleanupError);
        if (cleanupError) std::cerr << "Test cleanup failed: " << cleanupError.message() << '\n';
        return 1;
    }
}
