#include "CommandLine.h"
#include "RunLogging.h"
#include "ffmpeg.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string readFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not read session log.");
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

void checkFfmpegDiagnosticRouting()
{
    std::ostringstream output;
    const auto original = spdlog::default_logger();
    struct RestoreLogger
    {
        std::shared_ptr<spdlog::logger> original;
        ~RestoreLogger() { spdlog::set_default_logger(std::move(original)); }
    } restore{ original };
    auto sinks = original->sinks();
    auto capture = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    capture->set_pattern("%v");
    sinks.push_back(capture);
    auto logger = std::make_shared<spdlog::logger>("routing-test", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::trace);
    spdlog::set_default_logger(std::move(logger));
    FFmpeg::initialize_ffmpeg_logging();

    {
        FFmpeg::ScopedLogCapture scoped(fs::path(u8"tool-warning-\u97f3.mp3"), "chapters");
        av_log(nullptr, AV_LOG_WARNING, "scoped FFmpeg warning\n");
        av_log(nullptr, AV_LOG_ERROR, "scoped FFmpeg error\n");
        const auto records = scoped.TakeLogs();
        require(records.size() == 2 && records[0].level == "warning" && records[1].level == "error" &&
            records[0].filePath == CommonUtils::path_to_utf8(fs::path(u8"tool-warning-\u97f3.mp3")) &&
            records[0].stage == "chapters",
            "File-scoped warning/error records lost severity or Unicode context.");
    }
    {
        FFmpeg::ScopedRunLogCapture scoped("scan");
        std::thread worker([] {
            av_log(nullptr, AV_LOG_WARNING, "unscoped FFmpeg warning\n");
            av_log(nullptr, AV_LOG_ERROR, "unscoped FFmpeg error\n");
        });
        worker.join();
        const auto records = scoped.TakeLogs();
        require(records.size() == 2 && records[0].filePath.empty() && records[1].filePath.empty() &&
            records[0].stage == "scan" && records[0].level == "warning" && records[1].level == "error",
            "Worker-thread diagnostics lost severity or invented file context.");
    }
    av_log(nullptr, AV_LOG_WARNING, "uncaptured FFmpeg warning\n");
    av_log(nullptr, AV_LOG_ERROR, "uncaptured FFmpeg error\n");
    const auto console = output.str();
    for (const auto* context : { "scoped", "unscoped", "uncaptured" }) {
        require(console.find(std::string(" ") + context + " FFmpeg warning") == std::string::npos,
            "An FFmpeg warning leaked into console logging.");
        require(console.find(std::string(" ") + context + " FFmpeg error") != std::string::npos,
            "An FFmpeg error was hidden from console logging.");
    }

}

void checkMetadataBatchProgress()
{
    std::ostringstream output;
    const auto original = spdlog::default_logger();
    struct RestoreLogger
    {
        std::shared_ptr<spdlog::logger> original;
        ~RestoreLogger() { spdlog::set_default_logger(std::move(original)); }
    } restore{ original };
    auto sinks = original->sinks();
    auto capture = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    capture->set_pattern("%v");
    sinks.push_back(capture);
    auto logger = std::make_shared<spdlog::logger>("progress-test", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));

    auto progress = std::make_shared<CommonUtils::ProgressBarInfo>();
    progress->size = 52;
    progress->name = "Batch Completed.";
    for (const auto count : { 10u, 20u, 30u, 40u, 50u }) {
        progress->count = count;
        CommonUtils::show_progress_bar(progress, CommonUtils::ProgressBarType::Complete);
    }
    require(output.str().empty(), "Intermediate batches printed completed progress lines.");
    spdlog::error("progress error remains visible");
    progress->count = 52;
    CommonUtils::show_progress_bar(progress, CommonUtils::ProgressBarType::Complete);
    const auto console = output.str();
    require(console.find("progress error remains visible") != std::string::npos,
        "Progress hid an error.");
    const auto completion = console.find("[done] Scan metadata:");
    require(completion != std::string::npos && console.find("100% 52/52.") != std::string::npos &&
        console.find("[done] Scan metadata:", completion + 1) == std::string::npos,
        "Metadata completion was missing or logged more than once.");
}
}

int main()
{
    auto testName = fs::path(u8"afc-log-\u97f3-");
    testName += std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto testDirectory = fs::temp_directory_path() / testName;
    try {
        require(fs::create_directory(testDirectory), "Could not create session-log test directory.");

        const auto oldLog = testDirectory / "afc-expired.log";
        const auto unrelatedLog = testDirectory / "unrelated.log";
        const auto activeLog = testDirectory / "afc-active.log";
        for (const auto& file : { oldLog, unrelatedLog, activeLog }) {
            std::ofstream(file) << "retain only if not expired or active";
            fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::hours(24 * 31));
        }
        auto marker = activeLog;
        marker += ".active";
#ifdef _WIN32
        std::ofstream(marker) << _getpid();
#else
        std::ofstream(marker) << getpid();
#endif
        require(RunLogging::Start("logging-test", testDirectory),
            "Could not start the session logger.");
        require(!fs::exists(oldLog), "Expired owned log was not pruned.");
        require(fs::exists(activeLog) && fs::exists(marker),
            "An active log was pruned.");
        require(fs::exists(unrelatedLog), "Retention pruned an unrelated log.");
        const auto infoLog = RunLogging::CurrentLogPath();
        require(infoLog.has_value(), "Session log path was not returned.");
        RunLogging::SetFileLevel(spdlog::level::info);
        spdlog::debug("debug detail excluded");
        spdlog::info("session info retained");
        require(RunLogging::RecordError("file-only error detail retained"),
            "File-only errors should be retained at the info threshold.");
        require(RunLogging::RecordWarning("file-only warning detail retained"),
            "File-only warnings should be retained at the info threshold.");
        checkFfmpegDiagnosticRouting();
        checkMetadataBatchProgress();
        RunLogging::ShowProgress("transient progress 1/100");
        RunLogging::ShowProgress("transient progress 100/100");
        spdlog::warn("session warning retained");
        RunLogging::ShowProgress("transient progress after warning");
        RunLogging::ClearProgress();
        RunLogging::Finish(0);
        auto activeMarker = *infoLog;
        activeMarker += ".active";
        require(!fs::exists(activeMarker),
            "Completed session retained its active marker.");
        spdlog::shutdown();

        const auto infoContent = readFile(*infoLog);
        require(infoContent.find("Request started: logging-test") != std::string::npos,
            "Session start was not recorded.");
        require(infoContent.find("session info retained") != std::string::npos &&
            infoContent.find("session warning retained") != std::string::npos,
            "Configured session details or warning were not recorded.");
        require(infoContent.find("debug detail excluded") == std::string::npos,
            "The detailed file-log threshold was not applied.");
        require(infoContent.find("file-only error detail retained") != std::string::npos,
            "File-only error details were not recorded.");
        require(infoContent.find("file-only warning detail retained") != std::string::npos,
            "File-only warning details were not recorded.");
        for (const auto* context : { "scoped", "unscoped", "uncaptured" }) {
            require(infoContent.find(std::string(" ") + context + " FFmpeg warning") != std::string::npos &&
                infoContent.find(std::string(" ") + context + " FFmpeg error") != std::string::npos,
                "The session log lost FFmpeg warning/error details.");
        }
        require(infoContent.find("transient progress") == std::string::npos &&
            infoContent.find('\x1b') == std::string::npos,
            "Transient progress or terminal control characters leaked into the session log.");

        require(RunLogging::Start("trace-test", testDirectory),
            "Could not start a second request logger.");
        const auto traceLog = RunLogging::CurrentLogPath();
        require(traceLog.has_value() && *traceLog != *infoLog,
            "Each request must receive a unique log file.");
        RunLogging::SetFileLevel(spdlog::level::trace);
        spdlog::debug("trace request detail");
        RunLogging::Finish(0);
        spdlog::shutdown();
        require(readFile(*traceLog).find("trace request detail") != std::string::npos,
            "The trace threshold did not retain detailed diagnostics.");

        require(RunLogging::Start("off-test", testDirectory),
            "Could not start a request with logging disabled.");
        const auto offLog = RunLogging::CurrentLogPath();
        RunLogging::SetFileLevel(spdlog::level::off);
        spdlog::info("disabled detail");
        require(!RunLogging::RecordError("disabled error detail"),
            "Disabled error logging should report that file-only details are unavailable.");
        require(!RunLogging::RecordWarning("disabled warning detail"),
            "Disabled warning logging should report that file-only details are unavailable.");
        RunLogging::Finish(1);
        spdlog::shutdown();
        require(offLog.has_value() && readFile(*offLog).find("Request finished: failed") != std::string::npos,
            "A failed request did not retain its finish status when detail logging was disabled.");
        require(readFile(*offLog).find("disabled detail") == std::string::npos,
            "An off threshold did not suppress ordinary details.");
        require(readFile(*offLog).find("disabled error detail") == std::string::npos,
            "An off threshold did not suppress file-only error details.");
        require(readFile(*offLog).find("disabled warning detail") == std::string::npos,
            "An off threshold did not suppress file-only warning details.");

        const char* arguments[] = {
            "AudioLibraryToolkit", "scan", "library", "--log-level", "trace"
        };
        CommandLineOptions options;
        require(ParseCommandLine(5, arguments, options) == 1 && options.logLevel == "trace",
            "The CLI did not accept the detailed log-level override.");

        require(!RunLogging::Start("unwritable", unrelatedLog),
            "A file was accepted as the session log directory.");
        require(!RunLogging::CurrentLogPath(), "A failed log start reported an available path.");
        require(!RunLogging::RecordError("unavailable error detail"),
            "A failed log start accepted file-only error details.");
        require(!RunLogging::RecordWarning("unavailable warning detail"),
            "A failed log start accepted file-only warning details.");
        RunLogging::Finish(1);
        spdlog::shutdown();
        fs::remove_all(testDirectory);
        return 0;
    }
    catch (const std::exception& error) {
        spdlog::shutdown();
        std::error_code cleanupError;
        fs::remove_all(testDirectory, cleanupError);
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
