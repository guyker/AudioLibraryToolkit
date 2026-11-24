#include "FlacMetadata.h"
#include "PreservationAudit.h"
#include "AppSettings.h"
#include "MediaConversionTask.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <algorithm>
#include <chrono>
#include <functional>
#include <vector>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        require(argc >= 3 && argc <= 5,
            "Expected source FLAC, scratch directory, optional FFmpeg and optional live-progress FLAC");
        const std::filesystem::path source = argv[1];
        const std::filesystem::path scratch = argv[2];
        if (argc >= 4) {
            const std::filesystem::path progressSource = argc == 5 ? argv[4] : argv[1];
            auto settings = AppSettingsJson::GetDefaultSettings();
            settings.FLACSettings.ffmpeg_exe_name = std::filesystem::path(argv[3]).wstring();
            const auto config = scratch / "conversion-progress-config.json";
            settings.saveToFile(config);
            std::string error;
            require(AppSettingsJson::LoadForCommandLine(config, error) != nullptr,
                "Could not load conversion-progress fixture configuration");
            for (const auto target : { AudioTarget::Mp3, AudioTarget::Flac }) {
                const auto extension = target == AudioTarget::Mp3 ? ".mp3" : ".flac";
                const auto destination = scratch / (std::string("progress-output") + extension);
                const auto temporary = scratch / (std::string("progress-temporary") + extension);
                ConversionOptions options{};
                options.target = target;
                options.sidecarFormat = MetadataSidecarFormat::Binary;
                MediaConversionTask task(progressSource, destination, temporary, false, options);
                std::vector<double> fractions;
                std::vector<std::chrono::steady_clock::time_point> measurements;
                std::vector<std::string> stages;
                task.SetProgressCallback([&](std::optional<double> fraction, std::string_view stage) {
                    require(!std::filesystem::exists(destination),
                        "A track was installed before encoding and verification completed");
                    if (stage == "Starting encoder") {
                        require(!fraction, "Encoder startup claimed a measured percentage");
                    }
                    if (fraction) {
                        fractions.push_back(*fraction);
                        measurements.push_back(std::chrono::steady_clock::now());
                    }
                    if (stages.empty() || stages.back() != stage) stages.emplace_back(stage);
                });
                require(task.Run() == 0, "Conversion with progress failed");
                require(fractions.size() >= 2 && fractions.back() == 1 &&
                    std::is_sorted(fractions.begin(), fractions.end()),
                    "FFmpeg progress was missing, unbounded, or moved backwards");
                require(std::all_of(fractions.begin(), fractions.end(),
                    [](double value) { return value >= 0 && value <= 1; }),
                    "Encoding progress exceeded its percentage bounds");
                if (argc == 5) {
                    require(std::any_of(fractions.begin(), fractions.end(),
                        [](double value) { return value > 0 && value < 1; }),
                        "No intermediate encoding percentage arrived for the artwork-bearing track");
                    require(measurements.back() - measurements.front() >= std::chrono::milliseconds(250),
                        "Encoding timestamps were buffered until the end instead of arriving live");
                }
                require(stages == std::vector<std::string>{
                    "Preparing", "Starting encoder", "Encoding", "Verifying audio", "Verifying metadata" },
                    "Track progress did not distinguish encoding from verification");
                require(task.PostRun() == 0 && std::filesystem::is_regular_file(destination),
                    "Verified conversion with progress was not installed");
            }
        }
        const auto sidecar = scratch / "archive.flac-metadata";
        bool prepared = false;
        const auto binary = MetadataSidecarFormat::Binary;
        require(FlacMetadata::Save(source, sidecar, binary, "", scratch / "clean.flac", prepared), "Save failed");
        require(FlacMetadata::Verify(source, sidecar, binary), "Valid archive rejected");
        const auto trailingSource = scratch / "trailing-tags.flac";
        std::filesystem::copy_file(source, trailingSource);
        std::string id3v1(128, '\0');
        id3v1.replace(0, 3, "TAG");
        {
            std::ofstream output(trailingSource, std::ios::binary | std::ios::app);
            output.write(id3v1.data(), static_cast<std::streamsize>(id3v1.size()));
            output.close();
            require(static_cast<bool>(output), "Cannot create trailing-tag fixture");
        }
        const auto archiveOnly = scratch / "archive-only.flac-metadata";
        std::error_code cleanupError;
        std::filesystem::remove(scratch / "clean.flac", cleanupError);
        prepared = false;
        require(FlacMetadata::Save(trailingSource, archiveOnly, binary, "", {}, prepared),
            "Archive-only save failed for appended ID3v1");
        require(!prepared, "Archive-only save unexpectedly prepared an audio copy");
        require(!std::filesystem::exists(scratch / "clean.flac"),
            "Archive-only save wrote a cleaned audio copy");
        require(FlacMetadata::Verify(trailingSource, archiveOnly, binary),
            "Archive-only save did not preserve appended ID3v1");
        std::ifstream input(sidecar, std::ios::binary);
        std::string original((std::istreambuf_iterator<char>(input)), {});
        input.close();
        const auto write = [&](const std::string& data) {
            std::ofstream output(sidecar, std::ios::binary | std::ios::trunc);
            output.write(data.data(), static_cast<std::streamsize>(data.size()));
            output.close();
            require(static_cast<bool>(output), "Cannot write fixture");
        };
        auto changed = original;
        changed.back() ^= 1;
        write(changed);
        require(!FlacMetadata::Verify(source, sidecar, binary), "Changed archive accepted");
        write(original.substr(0, original.size() - 1));
        require(!FlacMetadata::Verify(source, sidecar, binary), "Truncated archive accepted");
        write(original + "extra");
        require(!FlacMetadata::Verify(source, sidecar, binary), "Extra archive bytes accepted");
        write(original);
        require(FlacMetadata::Verify(source, sidecar, binary), "Restored archive rejected");
        require(!FlacMetadata::Verify(source, sidecar, MetadataSidecarFormat::Json), "Binary read as JSON");

        const auto json = MetadataSidecarFormat::Json;
        const auto jsonSidecar = scratch / "archive.flac-metadata.json";
        require(FlacMetadata::SidecarPath(scratch / "a.flac", json) == scratch / "a.flac.flac-metadata.json",
            "Unexpected JSON sidecar path");
        require(FlacMetadata::Save(source, jsonSidecar, json, "{\"title\":\"x\"}", scratch / "clean.flac", prepared),
            "JSON save failed");
        require(FlacMetadata::Verify(source, jsonSidecar, json), "Valid JSON archive rejected");
        std::ifstream jsonInput(jsonSidecar, std::ios::binary);
        const std::string jsonText((std::istreambuf_iterator<char>(jsonInput)), {});
        jsonInput.close();
        require(jsonText.find("\"title\"") != std::string::npos, "Readable metadata missing");
        const auto writeJson = [&](const std::string& data) {
            std::ofstream output(jsonSidecar, std::ios::binary | std::ios::trunc);
            output.write(data.data(), static_cast<std::streamsize>(data.size()));
            output.close();
            require(static_cast<bool>(output), "Cannot write JSON fixture");
        };
        const auto data = jsonText.find("\"data\"");
        require(data != std::string::npos, "JSON archive data missing");
        auto tampered = jsonText;
        const auto value = tampered.find('"', tampered.find(':', data)) + 1;
        tampered[value] = tampered[value] == 'A' ? 'B' : 'A';
        writeJson(tampered);
        require(!FlacMetadata::Verify(source, jsonSidecar, json), "Changed JSON archive accepted");
        writeJson(jsonText.substr(0, jsonText.size() / 2));
        require(!FlacMetadata::Verify(source, jsonSidecar, json), "Truncated JSON accepted");
        writeJson(jsonText);
        require(FlacMetadata::Verify(source, jsonSidecar, json), "Restored JSON archive rejected");

        require(FlacMetadata::SidecarPath(scratch / "a.flac", MetadataSidecarFormat::None).empty(),
            "None format must have no sidecar path");
        require(FlacMetadata::Save(source, {}, MetadataSidecarFormat::None, "", scratch / "clean.flac", prepared),
            "None-format preparation failed");
        require(PreservationAudit::VerifyCopy(sidecar, sidecar), "Identical copy rejected");
        require(!PreservationAudit::VerifyCopy(source, sidecar), "Changed copy accepted");
        require(PreservationAudit::Compare(source, source, true).ok, "Identical resources rejected");
        require(!PreservationAudit::Compare(source, scratch / "missing.flac", false).ok,
            "Uninspectable output accepted");
        std::ifstream audio(source, std::ios::binary);
        const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(audio)), {});
        require(bytes.size() >= 4 && std::string(bytes.begin(), bytes.begin() + 4) == "fLaC",
            "Native regression fixture must have no leading ID3");
        const auto transform = [&](const char* name, int omitted,
            const std::function<void(int, std::vector<unsigned char>&)>& modify) {
            std::vector<unsigned char> output(bytes.begin(), bytes.begin() + 4);
            std::size_t offset = 4, lastHeader = 4;
            bool last = false;
            while (!last) {
                require(offset + 4 <= bytes.size(), "Fixture header truncated");
                last = (bytes[offset] & 128) != 0;
                const int kind = bytes[offset] & 127;
                const std::size_t size = (bytes[offset + 1] << 16) |
                    (bytes[offset + 2] << 8) | bytes[offset + 3];
                require(size <= bytes.size() - offset - 4, "Fixture block truncated");
                std::vector<unsigned char> payload(bytes.begin() + offset + 4,
                    bytes.begin() + offset + 4 + size);
                offset += 4 + size;
                if (kind == omitted) continue;
                modify(kind, payload);
                lastHeader = output.size();
                output.push_back(static_cast<unsigned char>(kind));
                output.push_back(static_cast<unsigned char>(payload.size() >> 16));
                output.push_back(static_cast<unsigned char>(payload.size() >> 8));
                output.push_back(static_cast<unsigned char>(payload.size()));
                output.insert(output.end(), payload.begin(), payload.end());
            }
            output[lastHeader] |= 128;
            output.insert(output.end(), bytes.begin() + offset, bytes.end());
            const auto target = scratch / name;
            std::ofstream file(target, std::ios::binary);
            file.write(reinterpret_cast<const char*>(output.data()), output.size());
            file.close();
            require(static_cast<bool>(file), "Cannot write transformed fixture");
            return target;
        };
        const auto unchanged = [](int, std::vector<unsigned char>&) {};
        const auto lostTags = transform("lost-tags.flac", 4, unchanged);
        require(!PreservationAudit::Compare(source, lostTags, false).ok,
            "Unarchived tag loss accepted");
        require(PreservationAudit::Compare(source, lostTags, true).ok,
            "Archive-backed tag loss rejected");
        const auto lostPictures = transform("lost-pictures.flac", 6, unchanged);
        require(!PreservationAudit::Compare(source, lostPictures, true).ok,
            "Supported picture loss accepted despite archive");
        const auto changedPictures = transform("changed-pictures.flac", -1,
            [](int kind, std::vector<unsigned char>& payload) {
                if (kind == 6) payload.back() ^= 1;
            });
        require(!PreservationAudit::Compare(source, changedPictures, true).ok,
            "Changed picture payload accepted despite archive");
        const auto normalized = transform("normalized.flac", -1,
            [](int kind, std::vector<unsigned char>& payload) {
                if (kind != 6) return;
                const auto be = [&](std::size_t offset) {
                    require(offset + 4 <= payload.size(), "Picture header truncated");
                    return (payload[offset] << 24) | (payload[offset + 1] << 16) |
                        (payload[offset + 2] << 8) | payload[offset + 3];
                };
                const auto description = 8 + static_cast<std::size_t>(be(4));
                const auto width = description + 4 + static_cast<std::size_t>(be(description));
                require(width + 4 <= payload.size(), "Picture dimensions truncated");
                payload[width + 3] ^= 1;
            });
        const auto audit = PreservationAudit::Compare(source, normalized, true);
        require(audit.ok && std::any_of(audit.entries.begin(), audit.entries.end(),
            [](const auto& entry) { return entry.status == PreservationAudit::Status::Normalized; }),
            "Normalized picture descriptors not reported");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
