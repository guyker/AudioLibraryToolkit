#include "FFmpegRuntime.h"

#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include <spdlog/spdlog.h>
#include "CommonUtils.h"
#include "FFmpegBuild.h"
#include "PlatformUtils.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {
constexpr std::size_t maxProbeBytes = 2 * 1024 * 1024;
constexpr auto probeTimeout = std::chrono::seconds(10);
struct Library { const char* name; unsigned expected; unsigned (*version)(); };

#ifdef _WIN32
struct Handle {
    HANDLE value{ nullptr };
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
#endif

bool capture(const fs::path& executable, const std::vector<std::string>& arguments,
    std::string& output, std::string& error)
{
    output.clear();
    error.clear();
    const auto deadline = std::chrono::steady_clock::now() + probeTimeout;
    int exitCode = -1;
#ifdef _WIN32
    Handle readPipe, writePipe, nullInput, job, process, thread;
    SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    if (!CreatePipe(&readPipe.value, &writePipe.value, &security, 65536) ||
        !SetHandleInformation(readPipe.value, HANDLE_FLAG_INHERIT, 0)) {
        error = "Cannot create FFmpeg probe pipe: " + std::system_category().message(GetLastError());
        return false;
    }
    nullInput.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (nullInput.value == INVALID_HANDLE_VALUE || !job.value ||
        !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        error = "Cannot prepare FFmpeg probe: " + std::system_category().message(GetLastError());
        return false;
    }
    const auto path = PlatformUtils::WindowsLongPath(executable);
    auto command = FFmpegRuntime::QuoteWindowsArgument(path);
    for (const auto& argument : arguments) {
        command += L" " + FFmpegRuntime::QuoteWindowsArgument(CommonUtils::utf8ToWstring(argument));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput.value;
    startup.hStdOutput = writePipe.value;
    startup.hStdError = writePipe.value;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &info)) {
        error = "Cannot launch FFmpeg probe: " + std::system_category().message(GetLastError());
        return false;
    }
    process.value = info.hProcess;
    thread.value = info.hThread;
    if (!AssignProcessToJobObject(job.value, process.value) || ResumeThread(thread.value) == DWORD(-1)) {
        error = "Cannot start FFmpeg probe: " + std::system_category().message(GetLastError());
        TerminateProcess(process.value, 1);
        WaitForSingleObject(process.value, INFINITE);
        return false;
    }
    CloseHandle(writePipe.value);
    writePipe.value = nullptr;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(readPipe.value, nullptr, 0, nullptr, &available, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) {
                error = "Cannot read FFmpeg probe: " + std::system_category().message(GetLastError());
                break;
            }
        }
        if (available) {
            std::array<char, 16384> buffer{};
            DWORD count = 0;
            if (!ReadFile(readPipe.value, buffer.data(),
                    (std::min)(available, static_cast<DWORD>(buffer.size())), &count, nullptr)) {
                error = "Cannot capture FFmpeg probe: " + std::system_category().message(GetLastError());
                break;
            }
            output.append(buffer.data(), count);
        }
        else {
            const auto state = WaitForSingleObject(process.value, 1);
            if (state == WAIT_OBJECT_0) {
                // Check the pipe once more after process exit before declaring capture complete.
                DWORD remaining = 0;
                PeekNamedPipe(readPipe.value, nullptr, 0, nullptr, &remaining, nullptr);
                if (remaining) continue;
                DWORD code = 0;
                if (!GetExitCodeProcess(process.value, &code)) {
                    error = "Cannot get FFmpeg probe exit code: " + std::system_category().message(GetLastError());
                }
                else exitCode = static_cast<int>(code);
                break;
            }
            if (state == WAIT_FAILED) {
                error = "Cannot wait for FFmpeg probe: " + std::system_category().message(GetLastError());
                break;
            }
        }
        if (output.size() > maxProbeBytes || std::chrono::steady_clock::now() >= deadline) {
            error = "FFmpeg probe exceeded its output limit or 10-second timeout.";
            break;
        }
    }
    if (!error.empty()) {
        TerminateJobObject(job.value, 1);
        WaitForSingleObject(process.value, INFINITE);
        return false;
    }
#else
    std::vector<std::string> values{ executable.string() };
    values.insert(values.end(), arguments.begin(), arguments.end());
    std::vector<char*> pointers;
    for (auto& value : values) pointers.push_back(value.data());
    pointers.push_back(nullptr);
    int pipeEnds[2]{};
    if (pipe(pipeEnds) != 0) {
        error = "Cannot create FFmpeg probe pipe: " + std::system_category().message(errno);
        return false;
    }
    const auto child = fork();
    if (child == 0) {
        close(pipeEnds[0]);
        const auto input = open("/dev/null", O_RDONLY);
        if (input == -1 || dup2(input, STDIN_FILENO) == -1 ||
            dup2(pipeEnds[1], STDOUT_FILENO) == -1 || dup2(pipeEnds[1], STDERR_FILENO) == -1) _exit(126);
        close(input);
        close(pipeEnds[1]);
        execv(pointers[0], pointers.data());
        _exit(errno == ENOENT ? 127 : 126);
    }
    const auto forkError = errno;
    close(pipeEnds[1]);
    if (child == -1) {
        close(pipeEnds[0]);
        error = "Cannot launch FFmpeg probe: " + std::system_category().message(forkError);
        return false;
    }
    if (fcntl(pipeEnds[0], F_SETFL, O_NONBLOCK) == -1) {
        error = "Cannot configure FFmpeg probe pipe: " + std::system_category().message(errno);
    }
    bool exited = false;
    int status = 0;
    while (error.empty()) {
        std::array<char, 4096> buffer{};
        const auto count = read(pipeEnds[0], buffer.data(), buffer.size());
        if (count > 0) output.append(buffer.data(), static_cast<std::size_t>(count));
        else if (count < 0 && errno != EAGAIN && errno != EINTR) {
            error = "Cannot read FFmpeg probe: " + std::system_category().message(errno);
            break;
        }
        if (!exited) {
            const auto waited = waitpid(child, &status, WNOHANG);
            if (waited == child) exited = true;
            else if (waited == -1 && errno != EINTR) {
                error = "Cannot wait for FFmpeg probe: " + std::system_category().message(errno);
                break;
            }
        }
        if (exited && count == 0) {
            exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            break;
        }
        if (output.size() > maxProbeBytes || std::chrono::steady_clock::now() >= deadline) {
            error = "FFmpeg probe exceeded its output limit or 10-second timeout.";
            break;
        }
        if (count <= 0) {
            pollfd descriptor{ pipeEnds[0], POLLIN, 0 };
            if (poll(&descriptor, 1, 10) == -1 && errno != EINTR) {
                error = "Cannot poll FFmpeg probe: " + std::system_category().message(errno);
            }
        }
    }
    close(pipeEnds[0]);
    if (!error.empty()) {
        if (!exited) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) == -1 && errno == EINTR) {}
        }
        return false;
    }
#endif
    if (exitCode != 0) {
        error = "FFmpeg probe failed with exit code " + std::to_string(exitCode) + ": " + output.substr(0, 2048);
        return false;
    }
    return true;
}

fs::path libraryPath(const Library& library)
{
#ifdef _WIN32
    // An imported function address can belong to an executable thunk rather than its DLL.
    const auto name = CommonUtils::utf8ToWstring(std::string(library.name) + "-" +
        std::to_string(library.expected >> 16) + ".dll");
    const auto module = GetModuleHandleW(name.c_str());
    if (!module) return {};
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    return fs::path(std::wstring(path.data(), length));
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(library.version), &info) == 0 || !info.dli_fname) return {};
    return fs::path(info.dli_fname);
#endif
}
}

bool FFmpegRuntime::IsBundledSelection(std::wstring_view selection)
{
    return selection.empty() || selection == L"bundled" || selection == L"ffmpeg" || selection == L"ffmpeg.exe";
}

bool FFmpegRuntime::IsValidSelection(std::wstring_view selection)
{
    return selection.find(L'\0') == std::wstring_view::npos &&
        (IsBundledSelection(selection) ||
            CommonUtils::path_from_utf8(PlatformUtils::WideToUTF8(std::wstring(selection))).is_absolute());
}

bool FFmpegRuntime::ValidateLibraries()
{
    const std::array libraries{
        Library{ "avformat", LIBAVFORMAT_VERSION_INT, avformat_version },
        Library{ "avcodec", LIBAVCODEC_VERSION_INT, avcodec_version },
        Library{ "avfilter", LIBAVFILTER_VERSION_INT, avfilter_version },
        Library{ "avutil", LIBAVUTIL_VERSION_INT, avutil_version },
        Library{ "swresample", LIBSWRESAMPLE_VERSION_INT, swresample_version }
    };
    auto directory = PlatformUtils::ExecutableDirectory();
    if (directory.empty()) {
        spdlog::error("Cannot locate the application directory for bundled FFmpeg.");
        return false;
    }
#ifndef _WIN32
    directory /= "../lib/AudioLibraryToolkit";
#endif
    for (const auto& library : libraries) {
        const auto path = libraryPath(library);
        std::error_code ec;
        const bool local = !path.empty() && fs::equivalent(path.parent_path(), directory, ec);
        const auto actual = library.version();
        if (!local || ec || actual != library.expected) {
            spdlog::error("FFmpeg {} is missing, external or incompatible: '{}'; restore the complete application package.",
                library.name, CommonUtils::path_to_utf8(path));
            return false;
        }
        spdlog::debug("FFmpeg {} {}.{}.{}: '{}'.", library.name,
            actual >> 16, (actual >> 8) & 255, actual & 255, CommonUtils::path_to_utf8(path));
    }
    if (!std::string_view(av_version_info()).starts_with(AFC_FFMPEG_BUILD)) {
        spdlog::error("FFmpeg library build '{}' differs from the bundled build '{}'.",
            av_version_info(), AFC_FFMPEG_BUILD);
        return false;
    }
    spdlog::info("Bundled FFmpeg libraries: {}.", av_version_info());
    return true;
}

bool FFmpegRuntime::ResolveEncoder(std::wstring_view selection, AudioTarget target, fs::path& executable)
{
    if (!IsValidSelection(selection)) {
        spdlog::error("FLACSettings.ffmpeg_exe_name must be bundled or an absolute executable path; PATH lookup is disabled.");
        return false;
    }
    const bool bundled = IsBundledSelection(selection);
    if (bundled) {
        executable = PlatformUtils::ExecutableDirectory();
        if (executable.empty()) {
            spdlog::error("Cannot locate the application directory for bundled FFmpeg.");
            return false;
        }
#ifdef _WIN32
        executable /= "ffmpeg.exe";
#else
        executable /= "ffmpeg";
#endif
    }
    else executable = CommonUtils::path_from_utf8(PlatformUtils::WideToUTF8(std::wstring(selection)));
    std::error_code ec;
    if (!fs::is_regular_file(executable, ec) || ec) {
        spdlog::error("Selected FFmpeg executable is missing: '{}'. No fallback will be used.",
            CommonUtils::path_to_utf8(executable));
        return false;
    }
    if (!bundled) {
        spdlog::warn("External FFmpeg override: '{}'; bundled encoder-version consistency does not apply.",
            CommonUtils::path_to_utf8(executable));
    }
    std::string output, error;
    if (!capture(executable, { "-version" }, output, error) || !output.starts_with("ffmpeg version ")) {
        spdlog::error("Cannot identify FFmpeg '{}': {}", CommonUtils::path_to_utf8(executable),
            error.empty() ? "not an FFmpeg encoder" : error);
        return false;
    }
    const auto version = output.substr(0, output.find_first_of("\r\n"));
    if (bundled && !output.starts_with("ffmpeg version " AFC_FFMPEG_BUILD)) {
        spdlog::error("Bundled encoder mismatch: {}; expected {}.", version, AFC_FFMPEG_BUILD);
        return false;
    }
    if (!capture(executable, { "-hide_banner", "-h", "full" }, output, error) ||
        output.find("-stats_enc_post") == std::string::npos ||
        output.find("-stats_enc_post_fmt") == std::string::npos) {
        spdlog::error("Selected FFmpeg lacks required encoder-statistics support: {}", error);
        return false;
    }
    const std::string codec = target == AudioTarget::Mp3 ? "libmp3lame" : "flac";
    if (!capture(executable, { "-hide_banner", "-h", "encoder=" + codec }, output, error) ||
        !output.starts_with("Encoder " + codec + " ")) {
        spdlog::error("Selected FFmpeg cannot use encoder '{}': {}", codec, error);
        return false;
    }
    spdlog::info("FFmpeg encoder ({}): '{}'; {}.", bundled ? "bundled" : "external",
        CommonUtils::path_to_utf8(executable), version);
    return true;
}

#ifdef _WIN32
std::wstring FFmpegRuntime::QuoteWindowsArgument(const std::wstring& argument)
{
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        quoted.append(character == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        quoted += character;
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}
#endif
