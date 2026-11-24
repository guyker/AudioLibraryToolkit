#include "AudioRepair.h"

#include "BoundedTaskRunner.h"
#include "CommonUtils.h"
#include "MediaConversionTask.h"
#include "PlatformUtils.h"
#include "PreservationAudit.h"
#include "RepairRules.h"
#include "RunLogging.h"

#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace
{
namespace fs = std::filesystem;

using FixList = std::vector<std::pair<std::string, std::size_t>>;
constexpr auto reportName = "repair-report.json";
constexpr auto summaryName = "repair-summary.html";
constexpr auto markerName = ".repair-in-progress";

class RepairProgress : public RunLogging::PhaseProgress
{
public:
    RepairProgress(std::string phase, std::optional<std::size_t> total = std::nullopt,
        std::string unit = "files")
        : PhaseProgress("Repair " + std::move(phase), total, std::move(unit))
    {
    }
};

std::string utf8(const fs::path& path)
{
    return CommonUtils::path_to_utf8(path);
}

FixList fixesOf(const RepairRules::Analysis& analysis)
{
    FixList fixes;
    for (const auto& finding : analysis.findings) {
        if (finding.fix) fixes.emplace_back(finding.rule, finding.count);
    }
    return fixes;
}

std::string describeFixes(const FixList& fixes)
{
    std::string result;
    for (const auto& [rule, count] : fixes) {
        if (!result.empty()) result += ", ";
        result += rule + " x" + std::to_string(count);
    }
    return result;
}

std::atomic_uint64_t temporarySequence{ 0 };

fs::path temporaryPath(const fs::path& target)
{
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const auto processId = _getpid();
#else
    const auto processId = getpid();
#endif
    return target.parent_path() / (".afc-repair-" + std::to_string(processId) + "-" +
        std::to_string(timestamp) + "-" + std::to_string(++temporarySequence) + ".tmp");
}

bool samePathComponent(const fs::path& left, const fs::path& right)
{
#ifdef _WIN32
    const auto& a = left.native();
    const auto& b = right.native();
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
        b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
#else
    return left == right;
#endif
}

bool isMissingPathError(const std::error_code& error)
{
    return error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory;
}

bool isSameOrChildPath(const fs::path& candidate, const fs::path& base)
{
    auto candidatePart = candidate.begin();
    for (auto basePart = base.begin(); basePart != base.end(); ++basePart, ++candidatePart) {
        if (candidatePart == candidate.end() || !samePathComponent(*candidatePart, *basePart)) {
            return false;
        }
    }
    return true;
}

bool validateOutputDirectoryChain(const fs::path& root, const fs::path& relative,
    const fs::path& sourceRoot, bool create)
{
    std::error_code ec;
    const auto canonicalRoot = fs::weakly_canonical(root, ec);
    if (ec) {
        spdlog::error("Cannot resolve repair output root '{}': {}", utf8(root), ec.message());
        return false;
    }
    fs::path current = root;
    fs::path prefix;
    for (const auto& component : relative) {
        if (component == "." || component.empty()) continue;
        if (component == ".." || component.has_root_path()) {
            spdlog::error("Repair destination escaped its output directory: '{}'.", utf8(relative));
            return false;
        }
        prefix /= component;
        current /= component;
        auto status = fs::symlink_status(current, ec);
        if (isMissingPathError(ec)) {
            ec.clear();
            status = fs::file_status(fs::file_type::not_found);
        }
        if (ec) {
            spdlog::error("Cannot inspect repair output directory '{}': {}", utf8(current), ec.message());
            return false;
        }
        if (status.type() != fs::file_type::not_found &&
            PlatformUtils::IsLinkOrReparsePoint(current)) {
            spdlog::error("Repair output contains a redirected or unverifiable directory component: '{}'.",
                utf8(current));
            return false;
        }
        if (status.type() == fs::file_type::not_found) {
            if (!create) continue;
            if (!fs::create_directory(current, ec) && ec) {
                spdlog::error("Cannot create repair output directory '{}': {}", utf8(current), ec.message());
                return false;
            }
            ec.clear();
            status = fs::symlink_status(current, ec);
            if (ec || PlatformUtils::IsLinkOrReparsePoint(current) || !fs::is_directory(status)) {
                spdlog::error("Repair output directory changed while it was being created: '{}'.", utf8(current));
                return false;
            }
        }
        const auto canonical = fs::weakly_canonical(current, ec);
        const auto expected = (canonicalRoot / prefix).lexically_normal();
        if (ec || isSameOrChildPath(canonical, sourceRoot) ||
            !isSameOrChildPath(canonical, expected) || !isSameOrChildPath(expected, canonical)) {
            spdlog::error("Repair output contains a redirected or unverifiable directory component: '{}'.", utf8(current));
            return false;
        }
        if (!fs::is_directory(status)) {
            spdlog::error("Repair output path component is not a directory: '{}'.", utf8(current));
            return false;
        }
    }
    return true;
}

bool samePath(const fs::path& left, const fs::path& right)
{
    return isSameOrChildPath(left, right) && isSameOrChildPath(right, left);
}

fs::path normalizedPath(fs::path path)
{
#ifdef _WIN32
    auto value = path.native();
    if (value.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        return fs::path(L"\\\\" + value.substr(8));
    }
    if (value.rfind(L"\\\\?\\", 0) == 0) {
        return fs::path(value.substr(4));
    }
#endif
    return path;
}

struct SafeOutputDirectoryHandle
{
    fs::path relative;
#ifdef _WIN32
    HANDLE handle{ INVALID_HANDLE_VALUE };
    ~SafeOutputDirectoryHandle()
    {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
#else
    int descriptor{ -1 };
    ~SafeOutputDirectoryHandle()
    {
        if (descriptor >= 0) ::close(descriptor);
    }
#endif
};

class SafeOutputTree;

class SafeOutputFile
{
public:
    SafeOutputFile(SafeOutputTree& tree, std::shared_ptr<SafeOutputDirectoryHandle> parent,
        fs::path name);
    ~SafeOutputFile();
    SafeOutputFile(const SafeOutputFile&) = delete;
    SafeOutputFile& operator=(const SafeOutputFile&) = delete;

    void Write(const char* data, std::size_t size);
    void CopyFrom(const fs::path& source);
    void Flush();
    fs::path VerificationPath() const;
    bool Install(const fs::path& targetName, bool overwrite);

private:
    void RemoveTemporary() noexcept;

    SafeOutputTree& _tree;
    std::shared_ptr<SafeOutputDirectoryHandle> _parent;
    fs::path _name;
    bool _installed{ false };
#ifdef _WIN32
    HANDLE _handle{ INVALID_HANDLE_VALUE };
    BY_HANDLE_FILE_INFORMATION _identity{};
    HANDLE _verifyHandle{ INVALID_HANDLE_VALUE };
#else
    int _descriptor{ -1 };
    dev_t _device{};
    ino_t _inode{};
#endif
};

class SafeOutputTree
{
public:
    SafeOutputTree(fs::path root, fs::path sourceRoot)
        : _root(std::move(root))
    {
        std::error_code error;
        _canonicalRoot = fs::weakly_canonical(_root, error);
        if (error) throw fs::filesystem_error("Cannot resolve repair output root", _root, error);
        _sourceRoot = fs::weakly_canonical(sourceRoot, error);
        if (error) throw fs::filesystem_error("Cannot resolve repair source root", sourceRoot, error);
        if (isSameOrChildPath(_canonicalRoot, _sourceRoot)) {
            throw std::runtime_error("Repair output root overlaps its source library.");
        }

        auto rootHandle = std::make_shared<SafeOutputDirectoryHandle>();
#ifdef _WIN32
        if (PlatformUtils::IsLinkOrReparsePoint(_root)) {
            throw std::runtime_error("Repair output root is a reparse point.");
        }
        rootHandle->handle = ::CreateFileW(PlatformUtils::WindowsLongPath(_root).c_str(),
            FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (rootHandle->handle == INVALID_HANDLE_VALUE) {
            throw fs::filesystem_error("Cannot open repair output root", _root,
                std::error_code(static_cast<int>(GetLastError()), std::system_category()));
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(rootHandle->handle, &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            throw std::runtime_error("Repair output root is not a real directory.");
        }
#else
        rootHandle->descriptor = ::open(_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (rootHandle->descriptor < 0) {
            throw fs::filesystem_error("Cannot open repair output root", _root,
                std::error_code(errno, std::generic_category()));
        }
#endif
        _directories.emplace(fs::path{}, std::move(rootHandle));
        Validate();
    }

    bool Validate() const
    {
        for (const auto& [relative, directory] : _directories) {
            const auto actual = ResolveDirectory(directory);
            std::error_code error;
            const auto canonical = fs::weakly_canonical(actual, error);
            const auto expected = (relative.empty() ? _canonicalRoot : _canonicalRoot / relative).lexically_normal();
            if (error || !samePath(normalizedPath(canonical), normalizedPath(expected)) ||
                isSameOrChildPath(canonical, _sourceRoot)) {
                throw std::runtime_error("Repair output directory changed or overlaps its source: " +
                    utf8(_root / relative));
            }
        }
        return true;
    }

    void EnsureDirectory(const fs::path& relative)
    {
        (void)Directory(relative, true);
    }

    void CreateDirectoryExclusive(const fs::path& relative)
    {
        if (relative.empty() || relative.has_parent_path() || relative.is_absolute() ||
            relative == "." || relative == "..") {
            throw std::runtime_error("Repair marker must be a single directory name.");
        }
#ifdef _WIN32
        if (!CreateDirectoryW(PlatformUtils::WindowsLongPath(_root / relative).c_str(), nullptr)) {
            throw fs::filesystem_error("Cannot reserve repair marker", _root / relative,
                std::error_code(static_cast<int>(GetLastError()), std::system_category()));
        }
#else
        const auto name = relative.native();
        const auto root = _directories.at(fs::path{});
        if (::mkdirat(root->descriptor, name.c_str(), 0777) != 0) {
            throw fs::filesystem_error("Cannot reserve repair marker", _root / relative,
                std::error_code(errno, std::generic_category()));
        }
#endif
        (void)Directory(relative, false);
        Validate();
    }

    std::unique_ptr<SafeOutputFile> CreateOutputFile(const fs::path& relative)
    {
        if (relative.empty() || relative.filename() == "." || relative.filename() == ".." ||
            relative.filename().has_parent_path() || relative.is_absolute()) {
            throw std::runtime_error("Invalid repair output filename: " + utf8(relative));
        }
        auto parent = Directory(relative.parent_path(), true);
        Validate();
        return std::make_unique<SafeOutputFile>(*this, std::move(parent), relative.filename());
    }

    void RemoveDirectory(const fs::path& relative)
    {
        auto directory = Directory(relative.parent_path(), false);
        auto marker = Directory(relative, false);
#ifdef _WIN32
        FILE_DISPOSITION_INFO disposition{ TRUE };
        if (!SetFileInformationByHandle(marker->handle, FileDispositionInfo,
                &disposition, sizeof(disposition))) {
            throw fs::filesystem_error("Cannot remove repair marker", _root / relative,
                std::error_code(static_cast<int>(GetLastError()), std::system_category()));
        }
#else
        struct stat opened{}, named{};
        if (::fstat(marker->descriptor, &opened) != 0 ||
            ::fstatat(directory->descriptor, relative.filename().c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
            opened.st_dev != named.st_dev || opened.st_ino != named.st_ino ||
            ::unlinkat(directory->descriptor, relative.filename().c_str(), AT_REMOVEDIR) != 0) {
            throw fs::filesystem_error("Cannot remove repair marker", _root / relative,
                std::error_code(errno, std::generic_category()));
        }
#endif
        _directories.erase(relative);
        Validate();
    }

private:
    friend class SafeOutputFile;

    fs::path ResolveDirectory(const std::shared_ptr<SafeOutputDirectoryHandle>& directory) const
    {
#ifdef _WIN32
        std::wstring value(512, L'\0');
        for (;;) {
            const DWORD length = GetFinalPathNameByHandleW(directory->handle, value.data(),
                static_cast<DWORD>(value.size()), FILE_NAME_NORMALIZED);
            if (length == 0) {
                throw fs::filesystem_error("Cannot resolve repair output directory",
                    _root / directory->relative,
                    std::error_code(static_cast<int>(GetLastError()), std::system_category()));
            }
            if (length < value.size()) {
                value.resize(length);
                return fs::path(value);
            }
            value.resize(static_cast<std::size_t>(length) + 1);
        }
#elif defined(__APPLE__)
        std::array<char, PATH_MAX> value{};
        if (::fcntl(directory->descriptor, F_GETPATH, value.data()) != 0) {
            throw fs::filesystem_error("Cannot resolve repair output directory",
                _root / directory->relative, std::error_code(errno, std::generic_category()));
        }
        return fs::path(value.data());
#else
        const auto descriptorPath = fs::path("/proc/self/fd") / std::to_string(directory->descriptor);
        std::array<char, PATH_MAX> value{};
        const auto length = ::readlink(descriptorPath.c_str(), value.data(), value.size() - 1);
        if (length < 0) {
            throw fs::filesystem_error("Cannot resolve repair output directory",
                _root / directory->relative, std::error_code(errno, std::generic_category()));
        }
        value[static_cast<std::size_t>(length)] = '\0';
        return fs::path(value.data());
#endif
    }

    std::shared_ptr<SafeOutputDirectoryHandle> Directory(fs::path relative, bool create)
    {
        if (relative.is_absolute()) throw std::runtime_error("Repair output path must be relative.");
        relative = relative.lexically_normal();
        if (relative == ".") relative.clear();
        fs::path prefix;
        auto parent = _directories.at(fs::path{});
        for (const auto& component : relative) {
            if (component.empty() || component == ".") continue;
            if (component == ".." || component.has_root_path()) {
                throw std::runtime_error("Repair output path escaped its root: " + utf8(relative));
            }
            prefix /= component;
            const auto found = _directories.find(prefix);
            if (found != _directories.end()) {
                parent = found->second;
                continue;
            }
            auto child = std::make_shared<SafeOutputDirectoryHandle>();
            child->relative = prefix;
#ifdef _WIN32
            const auto childPath = _root / prefix;
            DWORD attributes = GetFileAttributesW(PlatformUtils::WindowsLongPath(childPath).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const DWORD inspectError = GetLastError();
                if (!create || (inspectError != ERROR_FILE_NOT_FOUND && inspectError != ERROR_PATH_NOT_FOUND)) {
                    throw fs::filesystem_error("Cannot inspect repair output directory", childPath,
                        std::error_code(static_cast<int>(inspectError), std::system_category()));
                }
                if (!CreateDirectoryW(PlatformUtils::WindowsLongPath(childPath).c_str(), nullptr) &&
                    GetLastError() != ERROR_ALREADY_EXISTS) {
                    throw fs::filesystem_error("Cannot create repair output directory", childPath,
                        std::error_code(static_cast<int>(GetLastError()), std::system_category()));
                }
            }
            child->handle = ::CreateFileW(PlatformUtils::WindowsLongPath(childPath).c_str(),
                DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (child->handle == INVALID_HANDLE_VALUE) {
                throw fs::filesystem_error("Cannot open repair output directory", childPath,
                    std::error_code(static_cast<int>(GetLastError()), std::system_category()));
            }
            BY_HANDLE_FILE_INFORMATION information{};
            if (!GetFileInformationByHandle(child->handle, &information) ||
                (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                throw std::runtime_error(
                    "Repair output contains a redirected or unverifiable directory component: " + utf8(childPath));
            }
#else
            const auto componentName = component.native();
            if (create && ::mkdirat(parent->descriptor, componentName.c_str(), 0777) != 0 &&
                errno != EEXIST) {
                throw fs::filesystem_error("Cannot create repair output directory", _root / prefix,
                    std::error_code(errno, std::generic_category()));
            }
            child->descriptor = ::openat(parent->descriptor, componentName.c_str(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child->descriptor < 0) {
                throw fs::filesystem_error("Cannot open repair output directory", _root / prefix,
                    std::error_code(errno, std::generic_category()));
            }
#endif
            _directories.emplace(prefix, child);
            Validate();
            parent = std::move(child);
        }
        Validate();
        return parent;
    }

    fs::path _root;
    fs::path _canonicalRoot;
    fs::path _sourceRoot;
    std::map<fs::path, std::shared_ptr<SafeOutputDirectoryHandle>> _directories;
};

SafeOutputFile::SafeOutputFile(SafeOutputTree& tree,
    std::shared_ptr<SafeOutputDirectoryHandle> parent, fs::path name)
    : _tree(tree), _parent(std::move(parent)), _name(std::move(name))
{
#ifdef _WIN32
    const auto path = _tree._root / _parent->relative / _name;
    _handle = ::CreateFileW(PlatformUtils::WindowsLongPath(path).c_str(),
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (_handle == INVALID_HANDLE_VALUE) {
        throw fs::filesystem_error("Cannot create repair temporary", path,
            std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    }
    if (!GetFileInformationByHandle(_handle, &_identity) ||
        (_identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        CloseHandle(_handle);
        _handle = INVALID_HANDLE_VALUE;
        throw std::runtime_error("Repair temporary is not a regular file: " + utf8(path));
    }
#else
    const auto nameValue = _name.native();
    _descriptor = ::openat(_parent->descriptor, nameValue.c_str(),
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
    if (_descriptor < 0) {
        throw fs::filesystem_error("Cannot create repair temporary", _tree._root / _parent->relative / _name,
            std::error_code(errno, std::generic_category()));
    }
    struct stat information{};
    if (::fstat(_descriptor, &information) != 0 || !S_ISREG(information.st_mode)) {
        ::close(_descriptor);
        _descriptor = -1;
        throw std::runtime_error("Repair temporary is not a regular file.");
    }
    _device = information.st_dev;
    _inode = information.st_ino;
#endif
}

SafeOutputFile::~SafeOutputFile()
{
    RemoveTemporary();
#ifdef _WIN32
    if (_verifyHandle != INVALID_HANDLE_VALUE) CloseHandle(_verifyHandle);
    if (_handle != INVALID_HANDLE_VALUE) CloseHandle(_handle);
#else
    if (_descriptor >= 0) ::close(_descriptor);
#endif
}

void SafeOutputFile::Write(const char* data, std::size_t size)
{
    if (!_tree.Validate()) throw std::runtime_error("Repair output path changed during staging.");
    while (size != 0) {
#ifdef _WIN32
        const DWORD chunk = static_cast<DWORD>((std::min)(size,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(_handle, data, chunk, &written, nullptr) || written == 0) {
            throw fs::filesystem_error("Cannot write repair temporary",
                _tree._root / _parent->relative / _name,
                std::error_code(static_cast<int>(GetLastError()), std::system_category()));
        }
        const auto count = static_cast<std::size_t>(written);
#else
        const auto count = ::write(_descriptor, data, size);
        if (count < 0) {
            if (errno == EINTR) continue;
            throw fs::filesystem_error("Cannot write repair temporary",
                _tree._root / _parent->relative / _name,
                std::error_code(errno, std::generic_category()));
        }
        if (count == 0) throw std::runtime_error("Cannot write repair temporary.");
#endif
        data += count;
        size -= count;
    }
}

void SafeOutputFile::CopyFrom(const fs::path& source)
{
    std::ifstream input(source, std::ios::binary);
    if (!input) throw fs::filesystem_error("Cannot open repair source for copying", source,
        std::make_error_code(std::errc::io_error));
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) Write(buffer.data(), static_cast<std::size_t>(count));
    }
    if (!input.eof()) throw fs::filesystem_error("Cannot read repair source for copying", source,
        std::make_error_code(std::errc::io_error));
}

void SafeOutputFile::Flush()
{
#ifdef _WIN32
    if (!FlushFileBuffers(_handle)) {
        throw fs::filesystem_error("Cannot flush repair temporary",
            _tree._root / _parent->relative / _name,
            std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    }
    if (!GetFileInformationByHandle(_handle, &_identity)) {
        throw fs::filesystem_error("Cannot inspect flushed repair temporary",
            _tree._root / _parent->relative / _name,
            std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    }
#else
    if (::fsync(_descriptor) != 0) {
        throw fs::filesystem_error("Cannot flush repair temporary",
            _tree._root / _parent->relative / _name,
            std::error_code(errno, std::generic_category()));
    }
#endif
}

fs::path SafeOutputFile::VerificationPath() const
{
#ifdef _WIN32
    const HANDLE handle = _verifyHandle != INVALID_HANDLE_VALUE ? _verifyHandle : _handle;
    std::wstring value(512, L'\0');
    for (;;) {
        const DWORD length = GetFinalPathNameByHandleW(handle, value.data(),
            static_cast<DWORD>(value.size()), FILE_NAME_NORMALIZED);
        if (length == 0) {
            throw fs::filesystem_error("Cannot resolve repair temporary",
                _tree._root / _parent->relative / _name,
                std::error_code(static_cast<int>(GetLastError()), std::system_category()));
        }
        if (length < value.size()) {
            value.resize(length);
            return fs::path(value);
        }
        value.resize(static_cast<std::size_t>(length) + 1);
    }
#elif defined(__APPLE__)
    std::array<char, PATH_MAX> value{};
    if (::fcntl(_descriptor, F_GETPATH, value.data()) != 0) {
        throw fs::filesystem_error("Cannot resolve repair temporary",
            _tree._root / _parent->relative / _name,
            std::error_code(errno, std::generic_category()));
    }
    return fs::path(value.data());
#else
    return fs::path("/proc/self/fd") / std::to_string(_descriptor);
#endif
}

bool SafeOutputFile::Install(const fs::path& targetName, bool overwrite)
{
    if (targetName.empty() || targetName.filename() != targetName || targetName == "." || targetName == "..") {
        throw std::runtime_error("Repair install target must be a filename.");
    }
    _tree.Validate();
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION stagedIdentity{};
    if (_handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(_handle, &stagedIdentity)) {
        throw fs::filesystem_error("Cannot inspect verified repair temporary",
            _tree._root / _parent->relative / _name,
            std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    }
    CloseHandle(_handle);
    _handle = INVALID_HANDLE_VALUE;
    const auto tempPath = _tree._root / _parent->relative / _name;
    HANDLE renameHandle = CreateFileW(PlatformUtils::WindowsLongPath(tempPath).c_str(),
        DELETE | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (renameHandle == INVALID_HANDLE_VALUE) {
        throw fs::filesystem_error("Cannot lock verified repair temporary", tempPath,
            std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    }
    BY_HANDLE_FILE_INFORMATION currentIdentity{};
    const bool sameFile = GetFileInformationByHandle(renameHandle, &currentIdentity) &&
        (currentIdentity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        currentIdentity.dwVolumeSerialNumber == stagedIdentity.dwVolumeSerialNumber &&
        currentIdentity.nFileIndexHigh == stagedIdentity.nFileIndexHigh &&
        currentIdentity.nFileIndexLow == stagedIdentity.nFileIndexLow &&
        currentIdentity.nFileSizeHigh == stagedIdentity.nFileSizeHigh &&
        currentIdentity.nFileSizeLow == stagedIdentity.nFileSizeLow &&
        currentIdentity.ftLastWriteTime.dwHighDateTime == stagedIdentity.ftLastWriteTime.dwHighDateTime &&
        currentIdentity.ftLastWriteTime.dwLowDateTime == stagedIdentity.ftLastWriteTime.dwLowDateTime;
    if (!sameFile) {
        CloseHandle(renameHandle);
        throw std::runtime_error("Repair temporary changed after verification.");
    }
    CloseHandle(renameHandle);
    const auto installedPath = _tree._root / _parent->relative / targetName;
    const DWORD moveFlags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(PlatformUtils::WindowsLongPath(tempPath).c_str(),
            PlatformUtils::WindowsLongPath(installedPath).c_str(), moveFlags)) {
        const auto error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        if (!overwrite && (error == std::errc::file_exists ||
                error.value() == ERROR_ALREADY_EXISTS || error.value() == ERROR_FILE_EXISTS)) return false;
        throw fs::filesystem_error("Cannot install verified repair output", installedPath, error);
    }
    _name = targetName;
    _verifyHandle = ::CreateFileW(PlatformUtils::WindowsLongPath(installedPath).c_str(),
        GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (_verifyHandle == INVALID_HANDLE_VALUE) {
        throw fs::filesystem_error("Cannot open installed repair output for verification", installedPath,
            std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    }
    BY_HANDLE_FILE_INFORMATION installedIdentity{};
    if (!GetFileInformationByHandle(_verifyHandle, &installedIdentity) ||
        (installedIdentity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        installedIdentity.dwVolumeSerialNumber != stagedIdentity.dwVolumeSerialNumber ||
        installedIdentity.nFileIndexHigh != stagedIdentity.nFileIndexHigh ||
        installedIdentity.nFileIndexLow != stagedIdentity.nFileIndexLow ||
        installedIdentity.nFileSizeHigh != stagedIdentity.nFileSizeHigh ||
        installedIdentity.nFileSizeLow != stagedIdentity.nFileSizeLow ||
        installedIdentity.ftLastWriteTime.dwHighDateTime != stagedIdentity.ftLastWriteTime.dwHighDateTime ||
        installedIdentity.ftLastWriteTime.dwLowDateTime != stagedIdentity.ftLastWriteTime.dwLowDateTime) {
        throw std::runtime_error("Installed repair output changed during publication.");
    }
    _installed = true;
#else
    struct stat staged{};
    if (_descriptor < 0 || ::fstat(_descriptor, &staged) != 0) {
        throw fs::filesystem_error("Cannot inspect verified repair temporary",
            _tree._root / _parent->relative / _name,
            std::error_code(errno, std::generic_category()));
    }
    struct stat named{};
    if (::fstatat(_parent->descriptor, _name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(named.st_mode) || named.st_dev != staged.st_dev || named.st_ino != staged.st_ino) {
        throw std::runtime_error("Repair temporary changed after verification.");
    }
    struct stat target{};
    const auto targetValue = targetName.native();
    const bool targetExists = ::fstatat(_parent->descriptor, targetValue.c_str(),
        &target, AT_SYMLINK_NOFOLLOW) == 0;
    if (!targetExists && errno != ENOENT) {
        throw fs::filesystem_error("Cannot inspect repair destination",
            _tree._root / _parent->relative / targetName,
            std::error_code(errno, std::generic_category()));
    }
    if (targetExists && S_ISLNK(target.st_mode)) {
        throw std::runtime_error("Repair destination became a link before installation.");
    }
    if (targetExists && !overwrite) return false;
    int result = 0;
    if (overwrite) {
        result = ::renameat(_parent->descriptor, _name.c_str(),
            _parent->descriptor, targetValue.c_str());
    }
    else {
        result = ::linkat(_parent->descriptor, _name.c_str(),
            _parent->descriptor, targetValue.c_str(), 0);
        if (result == 0) {
            if (::unlinkat(_parent->descriptor, _name.c_str(), 0) != 0) {
                throw fs::filesystem_error("Cannot remove installed repair temporary",
                    _tree._root / _parent->relative / _name,
                    std::error_code(errno, std::generic_category()));
            }
        }
    }
    if (result != 0) {
        if (!overwrite && errno == EEXIST) return false;
        throw fs::filesystem_error("Cannot install verified repair output",
            _tree._root / _parent->relative / targetName,
            std::error_code(errno, std::generic_category()));
    }
    _name = targetName;
    _installed = true;
    if (::fstatat(_parent->descriptor, targetValue.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(named.st_mode) || named.st_dev != staged.st_dev || named.st_ino != staged.st_ino) {
        throw std::runtime_error("Installed repair output changed during publication.");
    }
#endif
    _tree.Validate();
    return true;
}

void SafeOutputFile::RemoveTemporary() noexcept
{
    if (_installed) return;
#ifdef _WIN32
    if (_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(_handle);
        _handle = INVALID_HANDLE_VALUE;
    }
    const auto path = _tree._root / _parent->relative / _name;
    HANDLE handle = CreateFileW(PlatformUtils::WindowsLongPath(path).c_str(),
        DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            spdlog::error("Cannot open repair temporary for cleanup '{}': {}",
                utf8(path), std::error_code(static_cast<int>(error), std::system_category()).message());
        }
        return;
    }
    BY_HANDLE_FILE_INFORMATION current{};
    const bool sameFile = GetFileInformationByHandle(handle, &current) &&
        current.dwVolumeSerialNumber == _identity.dwVolumeSerialNumber &&
        current.nFileIndexHigh == _identity.nFileIndexHigh &&
        current.nFileIndexLow == _identity.nFileIndexLow &&
        (current.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    if (sameFile) {
        FILE_DISPOSITION_INFO disposition{ TRUE };
        if (!SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition))) {
            spdlog::error("Cannot remove repair temporary '{}': {}", utf8(path),
                std::error_code(static_cast<int>(GetLastError()), std::system_category()).message());
        }
    }
    else {
        spdlog::error("Repair temporary changed before cleanup; leaving it untouched: '{}'.", utf8(path));
    }
    CloseHandle(handle);
#else
    if (_descriptor >= 0) {
        struct stat opened{}, named{};
        if (::fstat(_descriptor, &opened) != 0) {
            spdlog::error("Cannot inspect repair temporary for cleanup '{}': {}",
                utf8(_tree._root / _parent->relative / _name), std::error_code(errno, std::generic_category()).message());
            return;
        }
        if (::fstatat(_parent->descriptor, _name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT) {
                spdlog::error("Cannot locate repair temporary for cleanup '{}': {}",
                    utf8(_tree._root / _parent->relative / _name), std::error_code(errno, std::generic_category()).message());
            }
            return;
        }
        if (opened.st_dev != named.st_dev || opened.st_ino != named.st_ino) {
            spdlog::error("Repair temporary changed before cleanup; leaving it untouched: '{}'.",
                utf8(_tree._root / _parent->relative / _name));
            return;
        }
        if (::unlinkat(_parent->descriptor, _name.c_str(), 0) != 0 && errno != ENOENT) {
            spdlog::error("Cannot remove repair temporary '{}': {}",
                utf8(_tree._root / _parent->relative / _name), std::error_code(errno, std::generic_category()).message());
        }
    }
#endif
}

std::string reportJson(const AudioRepair::Plan& plan, const std::string& status, const std::string& error)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("schemaVersion"); writer.Int(2);
    writer.Key("source"); writer.String(utf8(plan.input).c_str());
    writer.Key("output"); writer.String(utf8(plan.output).c_str());
    writer.Key("status"); writer.String(status.c_str());
    writer.Key("error"); writer.String(error.c_str());
    writer.Key("rules"); writer.StartArray();
    for (const auto& rule : RepairRules::Catalog()) {
        if (plan.rules.empty() || plan.rules.contains(rule.id)) writer.String(rule.id.c_str());
    }
    writer.EndArray();
    writer.Key("inspectedFiles"); writer.Uint64(plan.inspectedFiles);
    writer.Key("repairedFiles");
    writer.Uint64(static_cast<std::uint64_t>(std::count_if(plan.files.begin(), plan.files.end(),
        [](const AudioRepair::PlannedFile& file) { return file.repair && file.status == "installed"; })));
    writer.Key("copiedFiles");
    writer.Uint64(static_cast<std::uint64_t>(std::count_if(plan.files.begin(), plan.files.end(),
        [](const AudioRepair::PlannedFile& file) { return !file.repair && file.status == "installed"; })));
    writer.Key("conflicts"); writer.StartArray();
    for (const auto& conflict : plan.conflicts) writer.String(utf8(conflict).c_str());
    writer.EndArray();
    writer.Key("findings"); writer.StartArray();
    for (const auto& finding : plan.findings) {
        writer.StartObject();
        writer.Key("source"); writer.String(utf8(finding.source.lexically_relative(plan.input)).c_str());
        writer.Key("rule"); writer.String(finding.rule.c_str());
        writer.Key("status"); writer.String(finding.status.c_str());
        writer.Key("count"); writer.Uint64(finding.count);
        writer.Key("detail"); writer.String(finding.detail.c_str());
        const auto impact = AudioRepair::DescribeFindingImpact(finding);
        writer.Key("risk"); writer.StartObject();
        writer.Key("label"); writer.String(impact.label.data(),
            static_cast<rapidjson::SizeType>(impact.label.size()));
        writer.Key("leavingUnchanged"); writer.String(impact.unchangedDetail.data(),
            static_cast<rapidjson::SizeType>(impact.unchangedDetail.size()));
        writer.Key("fixing"); writer.String(impact.fixDetail.data(),
            static_cast<rapidjson::SizeType>(impact.fixDetail.size()));
        writer.EndObject();
        writer.EndObject();
    }
    writer.EndArray();
    writer.Key("files"); writer.StartArray();
    for (const auto& file : plan.files) {
        writer.StartObject();
        writer.Key("source"); writer.String(utf8(file.source.lexically_relative(plan.input)).c_str());
        writer.Key("output"); writer.String(utf8(file.target.lexically_relative(plan.output)).c_str());
        writer.Key("action"); writer.String(file.repair ? "repair" : "copy");
        writer.Key("status"); writer.String(file.status.c_str());
        if (file.repair) {
            const bool installedRepair = file.status == "installed";
            writer.Key(installedRepair ? "appliedRules" : "plannedRules"); writer.StartArray();
            for (const auto& [rule, count] : file.fixes) {
                writer.StartObject();
                writer.Key("rule"); writer.String(rule.c_str());
                writer.Key("count"); writer.Uint64(count);
                writer.EndObject();
            }
            writer.EndArray();
            writer.Key("impact"); writer.StartObject();
            writer.Key("audio");
            writer.String(installedRepair
                ? "Installed output was verified byte-identical across the audio data range; audio was not decoded."
                : plan.dryRun
                    ? "The proposed edit preserves the audio data range byte-for-byte; it has not been installed or decoded."
                    : "No audio-preservation claim is made because this repair was not verified and installed.");
            writer.Key("metadata");
            writer.String(installedRepair
                ? "Metadata named by the installed rules changed; FLAC padding may also change."
                : plan.dryRun
                    ? "The listed rules describe proposed metadata changes; FLAC padding may also change."
                    : "The listed rules were planned, but no metadata change is claimed as installed.");
            writer.EndObject();
        }
        else {
            writer.Key("impact");
            writer.String("File copied byte-for-byte; no audio or metadata changes.");
        }
        writer.EndObject();
    }
    writer.EndArray();
    writer.EndObject();
    return std::string(buffer.GetString(), buffer.GetSize());
}

std::string escapeHtml(std::string_view text)
{
    std::string escaped;
    for (const char ch : text) {
        switch (ch) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        default: escaped += ch; break;
        }
    }
    return escaped;
}

const char* severityName(AudioRepair::FindingImpact::Risk risk)
{
    using Risk = AudioRepair::FindingImpact::Risk;
    return risk == Risk::Review ? "High" : risk == Risk::Compatibility ? "Medium" : "Low";
}

std::string severityBadge(AudioRepair::FindingImpact::Risk risk)
{
    const std::string name = severityName(risk);
    std::string css = name;
    std::transform(css.begin(), css.end(), css.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return "<span class=\"sev " + css + "\">" + name + "</span>";
}

std::string_view issueTitle(std::string_view rule)
{
    static const std::map<std::string_view, std::string_view> titles{
        { "flac.leading-id3", "FLAC: ID3v2 tag before the FLAC data" },
        { "flac.trailing-tags", "FLAC: ID3v1/APE/Lyrics3 tags after the audio" },
        { "flac.multiple-comment-blocks", "FLAC: more than one tag (Vorbis comment) block" },
        { "flac.duplicate-comments", "FLAC: duplicate tag values" },
        { "flac.empty-comments", "FLAC: empty tags" },
        { "flac.invalid-comments", "FLAC: malformed tags" },
        { "flac.picture-descriptor", "FLAC: incorrect cover-art description" },
        { "flac.invalid-seektable", "FLAC: invalid seek table" },
        { "flac.missing-md5", "FLAC: missing audio checksum (MD5)" },
        { "mp3.duplicate-id3v2", "MP3: more than one ID3v2 tag" },
        { "mp3.duplicate-frames", "MP3: duplicate tag fields" },
        { "mp3.empty-frames", "MP3: empty tag fields" },
        { "mp3.text-encoding", "MP3: text encoding not valid for ID3v2.3" },
        { "mp3.picture-mime", "MP3: incorrect cover-art type" },
        { "mp3.redundant-id3v1", "MP3: redundant ID3v1 tag" },
        { "mp3.ape-lyrics3", "MP3: APE or Lyrics3 tag" },
        { "mp3.stream-garbage", "MP3: unexpected bytes before the audio" },
        { "mp3.unsupported-id3-layout", "MP3: unsupported ID3v2 tag layout" },
        { "unreadable", "Unreadable file" },
    };
    const auto title = titles.find(rule);
    return title == titles.end() ? rule : title->second;
}

// Finding details describe completed edits ("Removed ..."); planned or unapplied edits read as proposals.
std::string proposedWording(std::string_view detail)
{
    static constexpr std::pair<std::string_view, std::string_view> verbs[]{
        { "Removed ", "Remove " }, { "Corrected ", "Correct " }, { "Replaced ", "Replace " }, { "Changed ", "Change " } };
    for (const auto& [past, present] : verbs) {
        if (detail.starts_with(past)) return std::string(present) + std::string(detail.substr(past.size()));
    }
    return std::string(detail);
}

std::string summaryHtml(const AudioRepair::Plan& plan, std::string_view status, std::string_view error,
    bool preview = false)
{
    using AudioRepair::Finding;
    using Risk = AudioRepair::FindingImpact::Risk;
    enum class State { Planned, Repaired, Failed, NotAttempted, Review, Unreadable };

    const auto folderOf = [&](const fs::path& source) {
        return source.lexically_relative(plan.input).parent_path();
    };
    const auto folderName = [](const fs::path& folder) {
        return folder.empty() ? std::string("(Library root)") : utf8(folder);
    };
    std::map<fs::path, const AudioRepair::PlannedFile*> files;
    for (const auto& file : plan.files) files.emplace(file.source, &file);
    const auto fileFailed = [&](const fs::path& source) {
        const auto file = files.find(source);
        return file != files.end() && file->second->status == "failed";
    };
    const auto stateOf = [&](const Finding& finding) {
        if (finding.status == "unreadable") return State::Unreadable;
        if (finding.status == "manual-review") return State::Review;
        if (finding.status == "repaired") return State::Repaired;
        if (preview) return State::Planned;
        return fileFailed(finding.source) ? State::Failed : State::NotAttempted;
    };
    const auto stateLabel = [](State state) {
        switch (state) {
        case State::Planned: return "Planned fix";
        case State::Repaired: return "Fixed and verified";
        case State::Failed: return "Not fixed (repair failed)";
        case State::NotAttempted: return "Not fixed (not attempted)";
        case State::Review: return "Not changed (needs manual review)";
        default: return "Not changed (file could not be read)";
        }
    };
    const auto changeText = [&](const Finding& finding) {
        const auto state = stateOf(finding);
        std::string text = state == State::Planned || state == State::Failed || state == State::NotAttempted ?
            proposedWording(finding.detail) : finding.detail;
        if (finding.count > 1) text += " (" + std::to_string(finding.count) + " items)";
        return std::string(stateLabel(state)) + ": " + escapeHtml(text);
    };
    const auto tracksIn = [&](const fs::path& folder, std::size_t affected) {
        const auto total = plan.folderTracks.find(folder);
        const auto count = total == plan.folderTracks.end() ? affected : (std::max)(total->second, affected);
        return affected >= count ? "all " + std::to_string(count) + " track(s)" :
            std::to_string(affected) + " of " + std::to_string(count) + " tracks";
    };

    // An issue is a rule plus whether it is review-only, so every finding in a group shares one severity and risk text.
    struct Issue {
        std::string key, anchor;
        AudioRepair::FindingImpact impact{};
        std::map<fs::path, std::vector<const Finding*>> albums;
        std::set<fs::path> tracks;
        std::size_t planned{}, repaired{}, failed{}, unattempted{}, review{};
    };
    std::map<std::string, Issue> issues;
    std::map<fs::path, std::vector<const Finding*>> trackFindings;
    const auto keyOf = [](const Finding& finding) {
        const bool review = finding.status == "manual-review" || finding.status == "unreadable";
        return finding.rule + (review ? "#review" : "");
    };
    for (const auto& finding : plan.findings) {
        auto& issue = issues[keyOf(finding)];
        if (issue.key.empty()) {
            issue.key = keyOf(finding);
            issue.impact = AudioRepair::DescribeFindingImpact(finding);
            issue.anchor = "issue-" + issue.key;
            std::replace_if(issue.anchor.begin(), issue.anchor.end(),
                [](char ch) { return ch == '.' || ch == '#'; }, '-');
        }
        issue.albums[folderOf(finding.source)].push_back(&finding);
        issue.tracks.insert(finding.source);
        switch (stateOf(finding)) {
        case State::Planned: ++issue.planned; break;
        case State::Repaired: ++issue.repaired; break;
        case State::Failed: ++issue.failed; break;
        case State::NotAttempted: ++issue.unattempted; break;
        default: ++issue.review; break;
        }
        trackFindings[finding.source].push_back(&finding);
    }
    std::vector<const Issue*> issueOrder;
    for (const auto& [key, issue] : issues) issueOrder.push_back(&issue);
    std::stable_sort(issueOrder.begin(), issueOrder.end(), [](const Issue* left, const Issue* right) {
        if (left->impact.risk != right->impact.risk) return left->impact.risk > right->impact.risk;
        if (left->failed != right->failed) return left->failed > right->failed;
        return left->tracks.size() > right->tracks.size();
    });
    std::map<std::string, std::size_t> issueRank;
    for (std::size_t index = 0; index < issueOrder.size(); ++index) issueRank[issueOrder[index]->key] = index;
    const auto issueKeysOf = [&](const std::vector<const Finding*>& findings) {
        std::vector<std::string> keys;
        for (const auto* finding : findings) {
            auto key = keyOf(*finding);
            if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(std::move(key));
        }
        std::sort(keys.begin(), keys.end(), [&](const auto& left, const auto& right) {
            return issueRank[left] < issueRank[right];
        });
        return keys;
    };
    const auto issueLink = [&](const std::string& key) {
        const auto& issue = issues.at(key);
        return severityBadge(issue.impact.risk) + " <a href=\"#" + issue.anchor + "\">" +
            escapeHtml(issueTitle(issue.key.substr(0, issue.key.find('#')))) +
            (issue.key.ends_with("#review") ? " (review only)" : "") + "</a>";
    };
    const auto trackRisk = [&](const std::vector<const Finding*>& findings) {
        auto risk = Risk::Low;
        for (const auto* finding : findings) risk = (std::max)(risk, AudioRepair::DescribeFindingImpact(*finding).risk);
        return risk;
    };

    struct Album {
        fs::path folder;
        std::string anchor;
        Risk risk = Risk::Low;
        std::vector<fs::path> tracks;
        std::vector<std::string> issueKeys;
        std::vector<fs::path> failedCopies;
        std::size_t multiIssueTracks{}, repaired{}, copied{}, pending{}, failed{};
    };
    std::map<fs::path, Album> albums;
    for (const auto& [track, findings] : trackFindings) {
        auto& album = albums[folderOf(track)];
        album.tracks.push_back(track);
        album.risk = (std::max)(album.risk, trackRisk(findings));
        if (issueKeysOf(findings).size() > 1) ++album.multiIssueTracks;
        for (const auto& key : issueKeysOf(findings)) {
            if (std::find(album.issueKeys.begin(), album.issueKeys.end(), key) == album.issueKeys.end()) {
                album.issueKeys.push_back(key);
            }
        }
    }
    for (const auto& file : plan.files) {
        if (file.status == "failed" && !file.repair) albums[folderOf(file.source)].failedCopies.push_back(file.source);
    }
    std::size_t totalRepaired = 0, totalCopied = 0, totalPending = 0, totalFailed = 0, multiIssueTracks = 0;
    for (const auto& file : plan.files) {
        const auto album = albums.find(folderOf(file.source));
        const auto count = [&](std::size_t& total, std::size_t Album::*field) {
            ++total;
            if (album != albums.end()) ++(album->second.*field);
        };
        if (file.status == "installed") count(file.repair ? totalRepaired : totalCopied, file.repair ? &Album::repaired : &Album::copied);
        else if (file.status == "failed") count(totalFailed, &Album::failed);
        else if (preview && !file.repair) count(totalCopied, &Album::copied);
        else if (file.repair) count(totalPending, &Album::pending);
    }
    std::vector<Album*> albumOrder;
    for (auto& [folder, album] : albums) {
        album.folder = folder;
        std::sort(album.issueKeys.begin(), album.issueKeys.end(), [&](const auto& left, const auto& right) {
            return issueRank[left] < issueRank[right];
        });
        std::stable_sort(album.tracks.begin(), album.tracks.end(), [&](const auto& left, const auto& right) {
            return trackRisk(trackFindings[left]) > trackRisk(trackFindings[right]);
        });
        multiIssueTracks += album.multiIssueTracks;
        albumOrder.push_back(&album);
    }
    // Severity is the primary key; repair status only breaks ties within a severity.
    const auto statusPriority = [preview](const Album& album) {
        if (!preview && album.failed) return 2;
        if (!preview && album.pending) return 1;
        return 0;
    };
    std::stable_sort(albumOrder.begin(), albumOrder.end(), [&](const Album* left, const Album* right) {
        if (left->risk != right->risk) return left->risk > right->risk;
        if (statusPriority(*left) != statusPriority(*right)) return statusPriority(*left) > statusPriority(*right);
        return left->tracks.size() > right->tracks.size();
    });
    for (std::size_t index = 0; index < albumOrder.size(); ++index) {
        albumOrder[index]->anchor = "album-" + std::to_string(index + 1);
    }
    std::map<fs::path, std::string> albumAnchors;
    for (const auto* album : albumOrder) albumAnchors[album->folder] = album->anchor;

    const char* title = preview ? "Metadata repair plan (preview, no files changed)" : "Metadata repair results";
    std::ostringstream html;
    html << R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>)HTML" << title << R"HTML(</title>
<style>
body {font: 1rem/1.5 system-ui, sans-serif; max-width: 72rem; margin: 2rem auto; padding: 0 1rem; overflow-wrap: anywhere}
details {border: 1px solid #888; border-radius: .4rem; margin: .65rem 0; padding: .65rem}
details details {margin-left: .5rem}
summary {cursor: pointer} input {font: inherit; width: min(28rem, 95%)} li {margin: .4rem 0}
.sev {display: inline-block; min-width: 4.2rem; text-align: center; border-radius: .3rem; padding: 0 .3rem; font-size: .85rem; font-weight: 600; border: 1px solid}
.sev.high {background: #fde2e1; color: #8a1c13} .sev.medium {background: #fff3cd; color: #6b4e00} .sev.low {background: #e3f1e6; color: #1f5130}
.risk {margin: .3rem 0 .6rem; padding: .4rem .6rem; border-left: .25rem solid #888}
.note {color: #555; font-size: .9rem}
</style>
</head><body><main><h1>)HTML" << title << R"HTML(</h1>
)HTML";
    html << "<p>Source: " << escapeHtml(utf8(plan.input)) << "<br>Run status: " << escapeHtml(status) << "</p>";
    if (!error.empty()) html << "<p><strong>Error:</strong> " << escapeHtml(error) << "</p>";
    html << "<ul>";
    if (preview) {
        html << "<li>No repairs have run. Changes below are proposed, not made.</li><li>" << plan.inspectedFiles
             << " audio files inspected; " << trackFindings.size() << " track(s) with findings in " << albums.size()
             << " album folder(s).</li><li>" << totalPending << " file(s) planned for repair; " << totalCopied
             << " other file(s) in the same folders planned to be copied unchanged.</li>";
    }
    else {
        html << "<li>" << trackFindings.size() << " track(s) with findings in " << albums.size()
             << " album folder(s).</li><li>" << totalRepaired << " repaired, " << totalCopied << " copied unchanged, "
             << totalFailed << " failed, " << totalPending << " repairs not attempted.</li>";
    }
    html << "<li>" << issues.size() << " distinct issue(s); " << multiIssueTracks
         << " track(s) have more than one issue.</li></ul>";
    if (!preview) {
        html << R"HTML(<p class="note">Installed repairs were verified against the original audio bytes. Severity describes metadata and player compatibility, not sound quality.
Unresolved findings were not fixed; another repair may have changed the same file.
See <a href="repair-report.json">the JSON report</a> for the full file inventory.</p>
)HTML";
    }
    html << "<p class=\"note\">Severity: " << severityBadge(Risk::Review)
         << " needs manual review, no automatic fix; " << severityBadge(Risk::Compatibility)
         << " safe for the audio, but some players may show tags differently; " << severityBadge(Risk::Low)
         << " minor metadata clean-up.</p>";
    html << R"HTML(
<label for="filter">Filter issues, albums, tracks, or changes</label><br>
<input type="search" id="filter" hidden>
<p id="matches" role="status"></p>
)HTML";

    html << "<h2>Issues by severity</h2>";
    if (issueOrder.empty()) html << "<p>No findings.</p>";
    for (const auto* issue : issueOrder) {
        const auto rule = issue->key.substr(0, issue->key.find('#'));
        html << "<details data-search-group=\"issue\" id=\"" << issue->anchor << "\"><summary>"
             << severityBadge(issue->impact.risk) << " <strong>" << escapeHtml(issueTitle(rule))
             << (issue->key.ends_with("#review") ? " (review only)" : "") << "</strong> <small>"
             << escapeHtml(rule) << "</small> - " << issue->tracks.size() << " track(s) in "
             << issue->albums.size() << " album(s)";
        const std::pair<std::size_t, const char*> counts[]{ { issue->planned, "planned" },
            { issue->repaired, "fixed" }, { issue->failed, "failed" }, { issue->unattempted, "not attempted" },
            { issue->review, "review only" } };
        for (const auto& [value, label] : counts) {
            if (value) html << "; " << value << ' ' << label;
        }
        html << "</summary><div class=\"risk\"><strong>If left unchanged:</strong> "
             << escapeHtml(issue->impact.unchangedDetail) << "<br><strong>If fixed:</strong> "
             << escapeHtml(issue->impact.fixDetail) << "</div>";
        for (const auto& [folder, findings] : issue->albums) {
            std::set<fs::path> albumTracks;
            for (const auto* finding : findings) albumTracks.insert(finding->source);
            html << "<details><summary><a href=\"#" << albumAnchors[folder] << "\">"
                 << escapeHtml(folderName(folder)) << "</a> - " << tracksIn(folder, albumTracks.size())
                 << "</summary><ul>";
            for (const auto* finding : findings) {
                html << "<li><strong>" << escapeHtml(utf8(finding->source.filename())) << "</strong><br>"
                     << changeText(*finding);
                std::vector<std::string> others;
                for (const auto& key : issueKeysOf(trackFindings[finding->source])) {
                    if (key != issue->key) others.push_back(issueLink(key));
                }
                if (!others.empty()) {
                    html << "<br><small>This track also has: ";
                    for (std::size_t index = 0; index < others.size(); ++index) html << (index ? ", " : "") << others[index];
                    html << "</small>";
                }
                html << "</li>";
            }
            html << "</ul></details>";
        }
        html << "</details>\n";
    }

    html << "<h2>Albums by severity</h2><p class=\"note\">Only album folders with findings are listed.</p>";
    if (albumOrder.empty()) html << "<p>No album findings.</p>";
    for (const auto* album : albumOrder) {
        const auto* state = !preview && album->failed ? "Failed" : !preview && album->pending ? "Incomplete" : nullptr;
        html << "<details data-search-group=\"album\" id=\"" << album->anchor << "\"><summary>"
             << severityBadge(album->risk) << " <strong>" << escapeHtml(folderName(album->folder)) << "</strong> - ";
        if (state) html << "<strong>" << state << "</strong> - ";
        html << tracksIn(album->folder, album->tracks.size()) << " affected; " << album->issueKeys.size()
             << " issue(s)";
        if (album->multiIssueTracks) html << "; " << album->multiIssueTracks << " track(s) with multiple issues";
        html << "</summary><p>";
        if (preview) html << album->pending << " file(s) planned for repair, " << album->copied << " planned unchanged copies";
        else html << album->repaired << " repaired, " << album->copied << " copied unchanged, " << album->failed
                  << " failed, " << album->pending << " not attempted";
        html << ".</p><p><strong>Issues in this album</strong></p><ul>";
        for (const auto& key : album->issueKeys) {
            std::size_t affected = 0;
            for (const auto& track : album->tracks) {
                const auto& findings = trackFindings[track];
                affected += std::any_of(findings.begin(), findings.end(),
                    [&](const Finding* finding) { return keyOf(*finding) == key; });
            }
            html << "<li>" << issueLink(key) << " - " << tracksIn(album->folder, affected) << "</li>";
        }
        html << "</ul><p><strong>Affected tracks</strong></p><ul>";
        for (const auto& track : album->tracks) {
            const auto& findings = trackFindings[track];
            const auto keys = issueKeysOf(findings);
            html << "<li>" << severityBadge(trackRisk(findings)) << " <strong>" << escapeHtml(utf8(track.filename()))
                 << "</strong>";
            if (keys.size() > 1) html << " <small>(" << keys.size() << " issues)</small>";
            html << "<ul>";
            for (const auto& key : keys) {
                html << "<li>" << issueLink(key);
                for (const auto* finding : findings) {
                    if (keyOf(*finding) == key) html << "<br>" << changeText(*finding);
                }
                html << "</li>";
            }
            html << "</ul></li>";
        }
        html << "</ul>";
        if (!album->failedCopies.empty()) {
            html << "<p><strong>Files that failed to copy:</strong> ";
            for (std::size_t index = 0; index < album->failedCopies.size(); ++index) {
                html << (index ? ", " : "") << escapeHtml(utf8(album->failedCopies[index].filename()));
            }
            html << "</p>";
        }
        if (album->failed) {
            html << "<p>" << album->failed << " file(s) failed. See the run error and JSON file status.</p>";
        }
        html << "</details>\n";
    }
    html << R"HTML(</main>
<script>
const filter = document.getElementById('filter');
const matches = document.getElementById('matches');
const groups = Array.from(document.querySelectorAll('details[data-search-group]'));
const nested = Array.from(document.querySelectorAll('details:not([data-search-group])'));
filter.hidden = false;
filter.addEventListener('input', () => {
    const query = filter.value.trim().toLowerCase();
    let visible = 0;
    for (const group of groups) {
        group.hidden = !group.textContent.toLowerCase().includes(query);
        if (!group.hidden) ++visible;
        if (query) group.open = !group.hidden;
    }
    for (const item of nested) {
        item.hidden = query !== '' && !item.textContent.toLowerCase().includes(query);
        if (query) item.open = !item.hidden;
    }
    matches.textContent = `${visible} of ${groups.length} issues and albums shown`;
});
const reveal = () => {
    const target = location.hash && document.getElementById(location.hash.slice(1));
    if (!target) return;
    for (let element = target; element; element = element.parentElement) {
        if (element.tagName === 'DETAILS') { element.hidden = false; element.open = true; }
    }
    target.scrollIntoView();
};
window.addEventListener('hashchange', reveal);
reveal();
</script></body></html>
)HTML";
    return html.str();
}

bool writeReport(const fs::path& reportPath, std::string_view content, bool overwrite)
{
    const auto temporary = temporaryPath(reportPath);
    std::error_code ec;
    if (fs::exists(temporary, ec) || ec) return false;
    const bool verified = [&] {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.close();
        if (!output) return false;
        std::ifstream verify(temporary, std::ios::binary);
        const std::string actual{ std::istreambuf_iterator<char>(verify), std::istreambuf_iterator<char>() };
        return verify && actual == content;
    }();
    if (!verified || !MediaConversionTask::InstallOutputFile(temporary, reportPath, overwrite)) {
        fs::remove(temporary, ec);
        if (ec) spdlog::warn("Cannot remove repair report temporary '{}': {}", utf8(temporary), ec.message());
        return false;
    }
    return true;
}

bool writeSafeReport(SafeOutputTree& output, const fs::path& name,
    std::string_view content, bool overwrite)
{
    try {
        auto temporary = temporaryPath(name).filename();
        auto file = output.CreateOutputFile(temporary);
        file->Write(content.data(), content.size());
        file->Flush();
        const auto readBack = [&] {
            std::ifstream verify(file->VerificationPath(), std::ios::binary);
            const std::string actual{ std::istreambuf_iterator<char>(verify), std::istreambuf_iterator<char>() };
            return verify && actual == content;
        };
        if (!readBack() || !file->Install(name, overwrite)) return false;
        return readBack() && output.Validate();
    }
    catch (const std::exception& error) {
        spdlog::error("Cannot securely write repair report '{}': {}", utf8(name), error.what());
        return false;
    }
}
}

namespace AudioRepair
{
FindingImpact DescribeFindingImpact(const Finding& finding)
{
    using Risk = FindingImpact::Risk;
    if (finding.status == "manual-review" || finding.status == "unreadable") {
        return { Risk::Review, "Needs review",
            "If left unchanged, the reported defect or uncertainty remains; its effect depends on the affected reader.",
            "No automatic fix is proposed because a safe, standards-preserving edit could not be established." };
    }
    const auto& rule = finding.rule;
    if (rule == "mp3.redundant-id3v1") {
        return { Risk::Compatibility, "Compatibility caution",
            "Readers may display conflicting legacy values or select the wrong tag.",
            "Older players that read only ID3v1 may stop showing its fields." };
    }
    if (rule == "flac.leading-id3") {
        return { Risk::Compatibility, "Compatibility caution",
            "Some FLAC readers may ignore or reject the non-native leading tag.",
            "Readers that rely on the leading ID3 tag may no longer show its fields." };
    }
    if (rule == "flac.trailing-tags" || rule == "mp3.ape-lyrics3") {
        return { Risk::Compatibility, "Compatibility caution",
            "Readers may select a conflicting trailing or APE tag instead of the primary metadata.",
            "Readers that rely on the removed tag format may no longer show its fields." };
    }
    if (rule == "flac.multiple-comment-blocks") {
        return { Risk::Compatibility, "Compatibility caution",
            "FLAC requires at most one Vorbis comment block; readers may ignore extra blocks.",
            "An extra block is removed only when its fields and vendor string repeat the first block." };
    }
    if (rule == "flac.duplicate-comments") {
        return { Risk::Compatibility, "Compatibility caution",
            "Readers may show repeated values or choose one unpredictably.",
            "Removing an identical occurrence may alter the multiplicity of a multivalued field." };
    }
    if (rule == "flac.invalid-seektable") {
        return { Risk::Compatibility, "Compatibility caution",
            "The invalid table may cause poor seeking or be ignored by readers.",
            "Dropping seek points can make seeking slower; audio frames are unchanged." };
    }
    if (rule == "flac.picture-descriptor" || rule == "mp3.picture-mime") {
        return { Risk::Low, "Low metadata risk",
            "Incorrect picture descriptors may prevent some readers from displaying artwork.",
            "The descriptor is inferred from recognized image bytes; unusual or malformed images are left for review." };
    }
    if (rule == "mp3.duplicate-frames") {
        return { Risk::Low, "Low metadata risk",
            "Readers may show duplicate standard text fields.",
            "One byte-identical standard text frame is removed; other frame types and flagged frames are retained." };
    }
    if (rule == "mp3.duplicate-id3v2") {
        return { Risk::Compatibility, "Compatibility caution",
            "Readers may disagree about which leading tag to use.",
            "Older readers or offset-based chapter references may depend on the removed tag; chapter-bearing files are retained for review." };
    }
    if (rule == "flac.empty-comments" || rule == "mp3.empty-frames") {
        return { Risk::Low, "Low metadata risk",
            "Readers may display an empty field or treat it as missing.",
            "An empty field becomes absent; an MP3 tag's only frame is retained to avoid an empty tag." };
    }
    if (rule == "mp3.text-encoding") {
        return { Risk::Compatibility, "Compatibility caution",
            "A reader that does not support the stored encoding may display incorrect text.",
            "Text encoding changes while decoded text and the original tag size are preserved; tags that would need to grow are left unchanged." };
    }
    if (rule == "mp3.unsupported-id3-layout") {
        return { Risk::Review, "Needs review",
            "The tag could not be fully interpreted, so defects inside it may remain undetected.",
            "No change is made because unsupported headers, flags or frame layouts cannot be safely rewritten." };
    }
    if (rule == "mp3.stream-garbage") {
        return { Risk::Review, "Needs review",
            "Unexpected bytes may affect playback or indicate an incomplete or damaged stream.",
            "No bytes are removed automatically because they may be recoverable audio or unsupported metadata." };
    }
    return { Risk::Compatibility, "Compatibility caution",
        "The finding may affect metadata interpretation in some readers.",
        "Check the proposed edit with the players and tools you use." };
}

int Prepare(const fs::path& input, const fs::path& output, bool overwrite,
    const std::set<std::string>& rules, Plan& plan, std::size_t workers, bool dryRun)
{
    spdlog::info("Repair source folder: '{}'.", utf8(input));
    plan = {};
    plan.rules = rules;
    plan.workers = workers;
    plan.dryRun = dryRun;
    if (workers == 0 || workers > 16) {
        spdlog::error("Repair workers must be between 1 and 16.");
        return -1;
    }
    std::error_code ec;
    try {
        if (PlatformUtils::IsLinkOrReparsePoint(input)) {
            spdlog::error("Repair source is a link or reparse point: '{}'.", utf8(input));
            return -1;
        }
    }
    catch (const fs::filesystem_error& error) {
        spdlog::error("Cannot inspect repair source '{}': {}", utf8(input), error.what());
        return -1;
    }
    plan.input = fs::absolute(input, ec).lexically_normal();
    if (ec || !fs::is_directory(plan.input, ec) || ec) {
        spdlog::error("Repair input is not a readable directory: '{}'.", utf8(input));
        return -1;
    }
    plan.output = fs::absolute(output, ec).lexically_normal();
    if (ec || plan.output.empty()) {
        spdlog::error("Cannot resolve repair output directory: {}", ec.message());
        return -1;
    }
    plan.overwrite = overwrite;
    const auto canonicalInput = fs::weakly_canonical(plan.input, ec);
    if (ec) {
        spdlog::error("Cannot resolve repair input: {}", ec.message());
        return -1;
    }
    const auto canonicalOutput = fs::weakly_canonical(plan.output, ec);
    if (ec || isSameOrChildPath(canonicalOutput, canonicalInput) ||
        isSameOrChildPath(canonicalInput, canonicalOutput)) {
        spdlog::error("Repair output overlaps its source library.");
        return -1;
    }

    std::set<fs::path> affected;
    std::map<fs::path, FixList> repairSources;
    std::vector<fs::path> candidates;
    RepairProgress discovery("discovery", std::nullopt, "entries");
    try {
        for (const auto& entry : fs::recursive_directory_iterator(plan.input)) {
            discovery.Advance();
            if (PlatformUtils::IsLinkOrReparsePoint(entry.path())) {
                spdlog::error("Repair input contains a link or reparse point: '{}'.", utf8(entry.path()));
                return -1;
            }
            const auto status = entry.symlink_status();
            if (!fs::is_regular_file(status) || !RepairRules::IsSupportedFile(entry.path())) continue;
            candidates.push_back(entry.path());
        }
    }
    catch (const fs::filesystem_error& exception) {
        spdlog::error("Cannot inspect repair input: {}", exception.what());
        return -1;
    }
    std::sort(candidates.begin(), candidates.end());
    discovery.Finish();
    struct Inspection {
        std::vector<Finding> findings;
        FixList fixes;
        bool repair{ false };
    };
    std::vector<Inspection> inspections(candidates.size());
    RepairProgress inspectionProgress("inspection", candidates.size());
    try {
        BoundedTaskRunner::ForEach(candidates.size(), workers, [&](std::size_t index) {
            const auto& path = candidates[index];
            auto& inspection = inspections[index];
            try {
                const auto analysis = RepairRules::Analyze(path, plan.rules);
                for (const auto& finding : analysis.findings) {
                    inspection.findings.push_back({ path, finding.rule,
                        finding.fix ? "repairable" : "manual-review", finding.count, finding.detail });
                }
                if (analysis.edit) {
                    inspection.repair = true;
                    inspection.fixes = fixesOf(analysis);
                }
            }
            catch (const std::exception& exception) {
                inspection.findings.push_back({ path, "unreadable", "unreadable", 1, exception.what() });
            }
            inspectionProgress.Advance();
        });
    }
    catch (const std::exception& exception) {
        spdlog::error("Cannot run repair inspections: {}", exception.what());
        return -1;
    }
    inspectionProgress.Finish();
    RepairProgress planning("planning", std::nullopt, "entries");
    plan.inspectedFiles = candidates.size();
    for (const auto& candidate : candidates) {
        ++plan.folderTracks[candidate.lexically_relative(plan.input).parent_path()];
    }
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        auto& inspection = inspections[index];
        plan.findings.insert(plan.findings.end(), std::make_move_iterator(inspection.findings.begin()),
            std::make_move_iterator(inspection.findings.end()));
        if (inspection.repair) {
            auto folder = candidates[index].parent_path().lexically_relative(plan.input);
            if (folder == ".") folder.clear();
            affected.insert(std::move(folder));
            repairSources.emplace(candidates[index], std::move(inspection.fixes));
        }
    }

    const bool rootAffected = affected.contains(fs::path{});
    for (auto it = affected.begin(); it != affected.end();) {
        bool covered = rootAffected && !it->empty();
        for (auto parent = it->parent_path(); !covered && !parent.empty(); parent = parent.parent_path()) {
            if (affected.contains(parent)) {
                covered = true;
                break;
            }
        }
        if (covered) it = affected.erase(it);
        else ++it;
    }
    plan.repairedFolders.assign(affected.begin(), affected.end());

    std::set<fs::path> directories;
    try {
        for (const auto& folder : plan.repairedFolders) {
            const auto root = folder.empty() ? plan.input : plan.input / folder;
            directories.insert(folder);
            for (const auto& entry : fs::recursive_directory_iterator(root)) {
                planning.Advance();
                if (PlatformUtils::IsLinkOrReparsePoint(entry.path())) {
                    spdlog::error("Affected repair folder contains a link or reparse point: '{}'.",
                        utf8(entry.path()));
                    return -1;
                }
                const auto status = entry.symlink_status();
                const auto relative = entry.path().lexically_relative(plan.input);
                if (fs::is_directory(status)) {
                    directories.insert(relative);
                    continue;
                }
                if (!fs::is_regular_file(status)) {
                    spdlog::error("Affected repair folder contains a non-regular file: '{}'.", utf8(entry.path()));
                    return -1;
                }
                const auto size = entry.file_size(ec);
                if (ec) {
                    spdlog::error("Cannot inspect repair source '{}': {}", utf8(entry.path()), ec.message());
                    return -1;
                }
                const auto modified = entry.last_write_time(ec);
                if (ec) {
                    spdlog::error("Cannot inspect repair source '{}': {}", utf8(entry.path()), ec.message());
                    return -1;
                }
                const auto target = plan.output / relative;
                if (!validateOutputDirectoryChain(plan.output, relative.parent_path(), canonicalInput, false)) {
                    return -1;
                }
                const auto canonicalTarget = fs::weakly_canonical(target, ec);
                if (ec || isSameOrChildPath(canonicalTarget, canonicalInput)) {
                    spdlog::error("Repair destination overlaps its source: '{}'.", utf8(target));
                    return -1;
                }
                const bool parentExists = fs::exists(target.parent_path(), ec);
                if (ec) {
                    spdlog::error("Cannot inspect repair destination directory '{}': {}",
                        utf8(target.parent_path()), ec.message());
                    return -1;
                }
                auto targetStatus = fs::file_status(fs::file_type::not_found);
                if (parentExists) {
                    targetStatus = fs::symlink_status(target, ec);
                    if (isMissingPathError(ec)) {
                        ec.clear();
                        targetStatus = fs::file_status(fs::file_type::not_found);
                    }
                    if (ec) {
                        spdlog::error("Cannot inspect repair destination '{}': {}", utf8(target), ec.message());
                        return -1;
                    }
                }
                if (fs::exists(targetStatus) && PlatformUtils::IsLinkOrReparsePoint(target)) {
                    spdlog::error("Repair destination is a link: '{}'.", utf8(target));
                    return -1;
                }
                const bool targetExists = fs::exists(targetStatus);
                if (targetExists) {
                    if (!overwrite) {
                        plan.conflicts.push_back(target);
                    }
                    else if (!fs::is_regular_file(targetStatus)) {
                        spdlog::error("Repair destination is not a regular file: '{}'.", utf8(target));
                        return -1;
                    }
                }
                const auto fixes = repairSources.find(entry.path());
                const bool repair = fixes != repairSources.end();
                plan.files.push_back({ entry.path(), target, {}, size, modified, repair,
                    repair ? fixes->second : FixList{}, "not-attempted" });
                plan.files.back().temporary = temporaryPath(target);
            }
        }
    }
    catch (const fs::filesystem_error& exception) {
        spdlog::error("Cannot prepare affected repair folders: {}", exception.what());
        return -1;
    }
    std::sort(plan.files.begin(), plan.files.end(), [](const PlannedFile& left, const PlannedFile& right) {
        return left.source < right.source;
    });

    for (const auto& directory : directories) {
        const auto target = plan.output / directory;
        if (!validateOutputDirectoryChain(plan.output, directory, canonicalInput, false)) return -1;
        if (fs::exists(target, ec) && (!overwrite || !fs::is_directory(target, ec) || ec)) {
            plan.conflicts.push_back(target);
        }
        if (ec) {
            spdlog::error("Cannot inspect repair directory destination: {}", ec.message());
            return -1;
        }
    }
    for (const auto* name : { reportName, summaryName }) {
        const auto report = plan.output / name;
        const auto matchesReport = [&](const fs::path& target) {
            return samePathComponent(target.lexically_relative(plan.output), fs::path(name));
        };
        if (std::any_of(plan.files.begin(), plan.files.end(),
                [&](const PlannedFile& file) { return matchesReport(file.target); }) ||
            std::any_of(directories.begin(), directories.end(),
                [&](const fs::path& directory) { return matchesReport(plan.output / directory); })) {
            spdlog::error("Repair output would copy a file or folder over the reserved report '{}'.", name);
            return -1;
        }
        const auto destination = fs::symlink_status(report, ec);
        if (destination.type() == fs::file_type::not_found) ec.clear();
        if (ec) {
            spdlog::error("Cannot inspect repair report destination '{}': {}", utf8(report), ec.message());
            return -1;
        }
        if (fs::exists(destination) && (!overwrite || !fs::is_regular_file(destination))) {
            plan.conflicts.push_back(report);
        }
    }
    const auto marker = plan.output / markerName;
    if (!dryRun && (fs::exists(marker, ec) || ec)) {
        spdlog::error("Repair output has an existing in-progress marker or cannot be inspected.");
        return -1;
    }
    planning.Finish();
    return 0;
}

int PrintDryRunReport(const Plan& plan)
{
    std::cout << reportJson(plan, "dry-run", {}) << '\n' << std::flush;
    if (!std::cout) {
        spdlog::error("Cannot write the dry-run report.");
        return 1;
    }
    const bool attention = std::any_of(plan.findings.begin(), plan.findings.end(),
        [](const Finding& finding) { return finding.status == "manual-review" || finding.status == "unreadable"; });
    return attention ? 1 : 0;
}

bool WritePreviewReports(const std::vector<Plan>& plans)
{
    if (plans.empty()) return true;
    std::error_code ec;
    auto root = fs::temp_directory_path(ec);
    if (!ec) root = fs::absolute(root, ec);
    if (ec) {
        spdlog::error("Cannot locate a directory for repair previews: {}", ec.message());
        return false;
    }
    if (root.has_relative_path() && root.filename().empty()) root = root.parent_path();

    PlatformUtils::ScopedCurrentDirectory directory;
    // A system temporary directory may contain a library, but the new preview must be outside it.
    if (!directory.EnterValidatedDirectory(root, plans.front().input, false, nullptr,
            /*allowSourceAncestor=*/true)) return false;
    const auto canonicalRoot = fs::canonical(root, ec);
    if (ec || !directory.IsAnchored(root)) {
        spdlog::error("Cannot validate the repair preview directory.");
        return false;
    }
    for (const auto& plan : plans) {
        const auto source = fs::canonical(plan.input, ec);
        if (ec || isSameOrChildPath(canonicalRoot, source)) {
            spdlog::error("Cannot save repair previews: temporary directory overlaps a source library or its path cannot be verified.");
            return false;
        }
    }

    for (const auto& plan : plans) {
        auto name = temporaryPath("repair-preview.html").filename();
#ifdef _WIN32
        name.replace_extension(".preview.html");
#else
        name.replace_extension(".preview");
        if (::mkdir(directory.ResolvePath(name).c_str(), S_IRWXU) != 0) {
            spdlog::error("Cannot reserve private repair preview directory '{}'.", utf8(root / name));
            return false;
        }
        name /= "repair-preview.html";
#endif
        if (!writeReport(directory.ResolvePath(name), summaryHtml(plan, "preview", {}, true), false)) {
#ifndef _WIN32
            fs::remove(directory.ResolvePath(name.parent_path()), ec);
#endif
            spdlog::error("Cannot save repair preview '{}'. No repairs were started.", utf8(root / name));
            return false;
        }
        spdlog::info("Repair preview: '{}'.", utf8(root / name));
    }
    return directory.Restore();
}

bool ReportDestinationConflicts(const Plan& plan)
{
    if (plan.conflicts.empty()) return false;
    spdlog::error("Repair output folder '{}' has {} existing destination conflict(s). "
        "Choose a different OutDirectory/--output-dir, or explicitly enable overwrite if replacement is intended.",
        utf8(plan.output), plan.conflicts.size());
    std::string details = "Repair destination conflicts under '" + utf8(plan.output) + "':";
    for (const auto& conflict : plan.conflicts) {
        details += "\n    Existing: " + utf8(conflict);
    }
    if (RunLogging::RecordError(details)) {
        spdlog::error("Full conflict paths are in the session log: {}",
            utf8(*RunLogging::CurrentLogPath()));
    } else {
        spdlog::error("Detailed conflict paths are unavailable: enable file logging (LogLevel 0-4) "
            "and ensure the session log can be created.");
    }
    return true;
}

int Execute(Plan& plan, std::size_t* repaired, std::size_t* copied)
{
    if (plan.dryRun) {
        spdlog::error("A dry-run plan cannot be executed.");
        return -1;
    }
    if (ReportDestinationConflicts(plan)) {
        spdlog::error("No repair was started; source files and existing outputs are unchanged.");
        return -1;
    }
    if (repaired) *repaired = 0;
    if (copied) *copied = 0;
    bool owned = false;
    PlatformUtils::ScopedCurrentDirectory outputDirectory;
    if (!outputDirectory.EnterValidatedDirectory(plan.output, plan.input, true, &owned)) {
        return -1;
    }
    (void)owned;
    std::error_code ec;
    const auto canonicalSource = fs::weakly_canonical(plan.input, ec);
    if (ec) {
        spdlog::error("Cannot validate repair source before execution: {}", ec.message());
        return -1;
    }
    std::unique_ptr<SafeOutputTree> safeOutput;
    try {
        safeOutput = std::make_unique<SafeOutputTree>(plan.output, canonicalSource);
        safeOutput->CreateDirectoryExclusive(markerName);
        for (const auto& file : plan.files) {
            safeOutput->EnsureDirectory(file.target.lexically_relative(plan.output).parent_path());
        }
    }
    catch (const std::exception& exception) {
        spdlog::error("Cannot safely prepare repair output: {}", exception.what());
        return -1;
    }

    spdlog::info("Repair source folder: '{}'.", utf8(plan.input));
    std::string error;
    struct StagedFile {
        std::optional<RepairRules::Edit> edit;
        std::string error;
        std::unique_ptr<SafeOutputFile> output;
    };
    fs::path currentAlbum;
    RepairProgress execution("installation", plan.files.size(), "verified files installed");
    for (std::size_t first = 0; first < plan.files.size() && error.empty(); first += plan.workers) {
        const auto count = (std::min)(plan.workers, plan.files.size() - first);
        std::vector<StagedFile> staged(count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto& file = plan.files[first + index];
            if (file.repair && file.source.parent_path() != currentAlbum) {
                currentAlbum = file.source.parent_path();
                spdlog::info("Repairing album folder: {}", utf8(currentAlbum));
            }
        }
        try {
            BoundedTaskRunner::ForEach(count, plan.workers, [&](std::size_t index) {
                auto& file = plan.files[first + index];
                auto& result = staged[index];
                file.status = "processing";
                const auto relative = file.target.lexically_relative(plan.output);
                const auto temporary = file.temporary.lexically_relative(plan.output);
                if (relative.empty() || temporary.empty() || *relative.begin() == ".." || *temporary.begin() == "..") {
                    result.error = "repair destination escaped its output directory";
                    return;
                }
                std::error_code fileError;
                if (PlatformUtils::IsLinkOrReparsePoint(file.source)) {
                    result.error = "repair source became a link or reparse point: " + utf8(file.source);
                    return;
                }
                const auto sourceSize = fs::file_size(file.source, fileError);
                if (fileError) {
                    result.error = "cannot inspect repair source: " + utf8(file.source) + ": " + fileError.message();
                    return;
                }
                const auto sourceModified = fs::last_write_time(file.source, fileError);
                if (fileError || sourceSize != file.size || sourceModified != file.modified) {
                    result.error = "source changed after repair preview: " + utf8(file.source);
                    return;
                }
                try {
                    if (file.repair) {
                        spdlog::info("Checking repair candidate: {}", utf8(file.source.lexically_relative(plan.input)));
                        auto analysis = RepairRules::Analyze(file.source, plan.rules);
                        if (!analysis.edit || fixesOf(analysis) != file.fixes) {
                            result.error = "repair finding changed after preview: " + utf8(file.source);
                            return;
                        }
                        result.edit = std::move(analysis.edit);
                        result.output = safeOutput->CreateOutputFile(temporary);
                        RepairRules::Write(file.source, *result.edit, [&](const char* data, std::size_t size) {
                            result.output->Write(data, size);
                        });
                        result.output->Flush();
                        std::string verificationError;
                        if (!RepairRules::Verify(file.source, result.output->VerificationPath(),
                                *result.edit, plan.rules, verificationError)) {
                            result.error = "repair verification failed for '" + utf8(file.source) + "': " + verificationError;
                        }
                    }
                    else {
                        result.output = safeOutput->CreateOutputFile(temporary);
                        result.output->CopyFrom(file.source);
                        result.output->Flush();
                        if (!PreservationAudit::VerifyCopy(file.source, result.output->VerificationPath())) {
                            result.error = "unchanged file copy verification failed: " + utf8(file.source);
                        }
                    }
                }
                catch (const std::exception& exception) {
                    result.error = "cannot stage '" + utf8(file.source) + "': " + exception.what();
                }
            });
        }
        catch (const std::exception& exception) {
            error = "cannot run repair workers: " + std::string(exception.what());
        }
        for (std::size_t index = 0; index < count && error.empty(); ++index) {
            auto& file = plan.files[first + index];
            auto& result = staged[index];
            if (!result.error.empty()) {
                file.status = "failed";
                error = result.error;
                break;
            }
            bool outputValid = false;
            try {
                outputValid = safeOutput->Validate();
            }
            catch (const std::exception& exception) {
                file.status = "failed";
                error = "cannot validate repair output directory before installation: " +
                    utf8(file.target.parent_path()) + ": " + exception.what();
                break;
            }
            if (!outputValid || !result.output) {
                file.status = "failed";
                error = "repair output directory changed before installation: " + utf8(file.target.parent_path());
                break;
            }
            try {
                if (!result.output->Install(file.target.filename(), plan.overwrite)) {
                    file.status = "failed";
                    error = "cannot install verified repair output: " + utf8(file.target);
                    break;
                }
            }
            catch (const fs::filesystem_error& exception) {
                file.status = "failed";
                error = "cannot install verified repair output '" + utf8(file.target) + "': " +
                    exception.code().message();
                break;
            }
            catch (const std::exception& exception) {
                file.status = "failed";
                error = "cannot install verified repair output '" + utf8(file.target) + "': " + exception.what();
                break;
            }
            const auto installed = result.output->VerificationPath();
            if (file.repair) {
                std::string verificationError;
                if (!RepairRules::Verify(file.source, installed, *result.edit, plan.rules, verificationError)) {
                    file.status = "failed";
                    error = "installed repair verification failed: " + verificationError;
                    break;
                }
                for (auto& finding : plan.findings) {
                    if (finding.source == file.source && finding.status == "repairable") finding.status = "repaired";
                }
                if (repaired) ++*repaired;
                spdlog::info("Repaired '{}' ({}); audio bytes are unchanged (no decode/re-encode).",
                    utf8(file.source.lexically_relative(plan.input)), describeFixes(file.fixes));
            }
            else {
                if (!PreservationAudit::VerifyCopy(file.source, installed)) {
                    file.status = "failed";
                    error = "installed unchanged file verification failed: " + utf8(file.source);
                    break;
                }
                if (copied) ++*copied;
            }
            file.status = "installed";
            execution.Advance();
        }
        for (std::size_t index = 0; index < count; ++index) {
            auto& file = plan.files[first + index];
            staged[index].output.reset();
            if (file.status == "processing") file.status = "not-attempted";
        }
    }
    execution.Finish(error.empty());

    const bool attention = std::any_of(plan.findings.begin(), plan.findings.end(),
        [](const Finding& finding) { return finding.status == "manual-review" || finding.status == "unreadable"; });
    for (const auto& finding : plan.findings) {
        if (finding.status == "manual-review" || finding.status == "unreadable") {
            spdlog::warn("Repair finding [{}] {} '{}': {}", finding.status, finding.rule,
                utf8(finding.source.lexically_relative(plan.input)), finding.detail);
        }
    }
    const auto reportStatus = !error.empty() ? "failed" :
        attention ? "completed-with-findings" : plan.files.empty() ? "no-repairs-needed" : "completed";
    bool outputAnchored = false;
    try {
        outputAnchored = outputDirectory.IsAnchored(plan.output) && safeOutput->Validate();
    }
    catch (const std::exception& exception) {
        if (error.empty()) error = exception.what();
    }
    if (!outputAnchored) {
        if (error.empty()) error = "repair output directory changed during execution";
    }
    else if (!writeSafeReport(*safeOutput, reportName, reportJson(plan, reportStatus, error), plan.overwrite)) {
        spdlog::error("Cannot write or verify repair report '{}'.", utf8(plan.output / reportName));
        if (error.empty()) error = "cannot write or verify repair report";
    }
    else if (!writeSafeReport(*safeOutput, summaryName, summaryHtml(plan, reportStatus, error), plan.overwrite)) {
        spdlog::error("Cannot write or verify repair summary '{}'; JSON report: '{}'.",
            utf8(plan.output / summaryName), utf8(plan.output / reportName));
        if (error.empty()) error = "cannot write or verify repair summary";
    }
    else {
        spdlog::info("Repair summary: '{}'.", utf8(plan.output / summaryName));
    }
    bool markerRemoved = false;
    if (error.empty() && outputDirectory.IsAnchored(plan.output)) {
        try {
            safeOutput->Validate();
            safeOutput->RemoveDirectory(markerName);
            markerRemoved = true;
        }
        catch (const std::exception& exception) {
            error = exception.what();
        }
    }
    if (error.empty() && outputDirectory.IsAnchored(plan.output) && markerRemoved) {
        if (attention) {
            spdlog::warn("Repair completed with findings requiring review; report '{}'.", utf8(plan.output / reportName));
            return 1;
        }
        spdlog::info("Repair completed: {} repaired, {} copied; report '{}'.",
            repaired ? *repaired : 0, copied ? *copied : 0, utf8(plan.output / reportName));
        return 0;
    }
    if (error.empty()) {
        error = ec ? ec.message() : "repair output directory changed during execution";
    }
    spdlog::error("Repair failed: {}; output and in-progress marker retained at '{}'.",
        error, utf8(plan.output));
    return -1;
}
}
