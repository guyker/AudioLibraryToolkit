#include "WorkerProtocol.h"

#include <array>
#include <istream>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <string_view>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "RunLogging.h"

namespace
{
constexpr unsigned kProtocolVersion = 1;
using JsonWriter = rapidjson::Writer<rapidjson::StringBuffer>;

void writeEvent(std::ostream& output, const std::string& requestId, std::string_view type,
    const std::function<void(JsonWriter&)>& fields = {})
{
    rapidjson::StringBuffer buffer;
    JsonWriter writer(buffer);
    writer.StartObject();
    writer.Key("protocolVersion");
    writer.Uint(kProtocolVersion);
    writer.Key("requestId");
    writer.String(requestId.c_str());
    writer.Key("type");
    writer.String(type.data(), static_cast<rapidjson::SizeType>(type.size()));
    if (fields) fields(writer);
    writer.EndObject();
    output.write(buffer.GetString(), static_cast<std::streamsize>(buffer.GetSize()));
    output.put('\n');
    output.flush();
}

bool getString(const rapidjson::Value& object, const char* key, std::string& value,
    bool required, std::string& error)
{
    if (!object.IsObject() || !object.HasMember(key)) {
        if (required) error = std::string("Missing required string: ") + key;
        return !required;
    }
    const auto& member = object[key];
    if (!member.IsString()) {
        error = std::string("Expected a string for: ") + key;
        return false;
    }
    value.assign(member.GetString(), member.GetStringLength());
    if (required && value.empty()) {
        error = std::string("Value must not be empty: ") + key;
        return false;
    }
    return true;
}

bool getBool(const rapidjson::Value& object, const char* key, bool& value, std::string& error)
{
    if (!object.HasMember(key)) return true;
    if (!object[key].IsBool()) {
        error = std::string("Expected a boolean for: ") + key;
        return false;
    }
    value = object[key].GetBool();
    return true;
}

bool parseTagIssueResolutions(const rapidjson::Value& params, ConversionOptions& conversion,
    std::string& error)
{
    if (!params.HasMember("tagIssueResolutions")) return true;
    const auto& value = params["tagIssueResolutions"];
    if (!value.IsObject()) {
        error = "Expected an object for tagIssueResolutions.";
        return false;
    }

    for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member) {
        const std::string_view key(member->name.GetString(), member->name.GetStringLength());
        bool known = false;
        for (const auto kind : Mp3TagIssues::AllKinds) {
            if (key == Mp3TagIssues::Key(kind)) {
                known = true;
                break;
            }
        }
        if (!known) {
            error = "Unknown tag issue resolution: " + std::string(key);
            return false;
        }
    }

    for (const auto kind : Mp3TagIssues::AllKinds) {
        const auto* key = Mp3TagIssues::Key(kind);
        if (!value.HasMember(key)) continue;
        const auto& actionValue = value[key];
        if (!actionValue.IsString()) {
            error = std::string("Expected a string for tag issue resolution: ") + key;
            return false;
        }
        const std::string_view actionName(actionValue.GetString(), actionValue.GetStringLength());
        Mp3TagIssues::Action action = Mp3TagIssues::Action::Unresolved;
        if (actionName == "apply") action = Mp3TagIssues::Action::Apply;
        else if (actionName == "alternative") action = Mp3TagIssues::Action::Alternative;
        else if (actionName == "skipFiles") action = Mp3TagIssues::Action::SkipFiles;
        else if (actionName == "stop") action = Mp3TagIssues::Action::Stop;
        else {
            error = std::string("Invalid tag issue resolution for ") + key + ".";
            return false;
        }

        bool allowed = false;
        for (const auto& choice : Mp3TagIssues::Choices(kind)) {
            if (choice.action == action) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            error = std::string("Unsupported tag issue resolution for ") + key + ".";
            return false;
        }
        conversion.tagIssues.Set(kind, action);
    }
    return true;
}

bool makeOptions(const rapidjson::Value& request, CommandLineOptions& options,
    std::string& error)
{
    std::string operation;
    if (!getString(request, "operation", operation, true, error)) return false;
    if (!request.HasMember("parameters") || !request["parameters"].IsObject()) {
        error = "Expected an object for parameters.";
        return false;
    }
    const auto& params = request["parameters"];
    std::string inputKey;
    if (operation == "convert") {
        options.action = CommandAction::Convert;
        inputKey = "inputPath";
    }
    else if (operation == "repair") {
        options.action = CommandAction::Repair;
        inputKey = "inputPath";
    }
    else if (operation == "initializeConfig") {
        options.action = CommandAction::InitializeConfig;
        inputKey = "libraryPath";
    }
    else if (operation == "scan") {
        options.action = CommandAction::Scan;
        inputKey = "inputPath";
    }
    else if (operation == "duplicates") {
        options.action = CommandAction::Duplicates;
        inputKey = "jsonDirectory";
    }
    else if (operation == "databaseImport") {
        options.action = CommandAction::DatabaseImport;
        inputKey = "jsonDirectory";
    }
    else {
        error = "Unsupported operation: " + operation;
        return false;
    }

    if (!getString(params, inputKey.c_str(), options.inputPath, true, error)) return false;
    if (params.HasMember("outputDirectory")) {
        if (!getString(params, "outputDirectory", options.outputDirectory, true, error)) return false;
        options.outputDirectorySpecified = true;
    }
    if (params.HasMember("configPath")) {
        if (!getString(params, "configPath", options.configPath, true, error)) return false;
        options.configPathSpecified = true;
    }

    if (operation == "convert") {
        std::string target;
        if (params.HasMember("target")) {
            if (!getString(params, "target", target, true, error)) return false;
            if (target != "flac" && target != "mp3") {
                error = "target must be 'flac' or 'mp3'.";
                return false;
            }
            options.conversion.target = target == "mp3" ? AudioTarget::Mp3 : AudioTarget::Flac;
        }
        if (params.HasMember("mp3Quality")) {
            if (!params["mp3Quality"].IsInt() || params["mp3Quality"].GetInt() < 0 ||
                params["mp3Quality"].GetInt() > 9) {
                error = "mp3Quality must be an integer from 0 to 9.";
                return false;
            }
            options.conversion.mp3Quality = params["mp3Quality"].GetInt();
        }
        if (!getBool(params, "downmixToStereo", options.conversion.downmixToStereo, error) ||
            !getBool(params, "overwrite", options.overwrite, error) ||
            !getBool(params, "noMetadataSidecar", options.noMetadataSidecar, error) ||
            !parseTagIssueResolutions(params, options.conversion, error)) return false;
    }
    else if (operation == "initializeConfig") {
        if (!params.HasMember("outputDirectory") || options.outputDirectory.empty()) {
            error = "initializeConfig requires a non-empty outputDirectory.";
            return false;
        }
    }
    else if (operation == "repair") {
        if (!getBool(params, "overwrite", options.overwrite, error) ||
            !getBool(params, "dryRun", options.repairDryRun, error) ||
            !getBool(params, "removeId3v1", options.repairRemoveId3v1, error)) return false;
        if (params.HasMember("jobs")) {
            if (!params["jobs"].IsInt() || params["jobs"].GetInt() < 1 ||
                params["jobs"].GetInt() > 16) {
                error = "jobs must be an integer from 1 to 16.";
                return false;
            }
            options.repairJobs = static_cast<std::size_t>(params["jobs"].GetInt());
        }
        if (params.HasMember("rules")) {
            if (!params["rules"].IsArray()) {
                error = "rules must be an array of rule ids.";
                return false;
            }
            std::string rules;
            for (const auto& rule : params["rules"].GetArray()) {
                if (!rule.IsString() || rule.GetStringLength() == 0) {
                    error = "rules must contain only non-empty strings.";
                    return false;
                }
                options.repairRules.emplace(rule.GetString(), rule.GetStringLength());
            }
        }
    }
    else if (operation == "scan") {
        if (!getBool(params, "fullRescan", options.fullRescan, error)) return false;
        if (params.HasMember("tier")) {
            std::string tier;
            if (!getString(params, "tier", tier, true, error)) return false;
            if (tier != "catalog" && tier != "integrity" && tier != "analysis") {
                error = "tier must be 'catalog', 'integrity', or 'analysis'.";
                return false;
            }
            options.scanTier = tier;
            options.scanTierSpecified = true;
        }
        if (params.HasMember("archiveFlacMetadata")) {
            if (!params["archiveFlacMetadata"].IsBool()) {
                error = "archiveFlacMetadata must be a boolean.";
                return false;
            }
            options.archiveFlacMetadata = params["archiveFlacMetadata"].GetBool();
            options.noArchiveFlacMetadata = !options.archiveFlacMetadata;
        }
    }
    else if (operation == "duplicates") {
        if (!getBool(params, "openFolders", options.openDuplicateFolders, error) ||
            !getString(params, "reportPath", options.reportPath, false, error)) return false;
        if (params.HasMember("duplicateQuarantinePaths")) {
            if (!params["duplicateQuarantinePaths"].IsArray()) {
                error = "duplicateQuarantinePaths must be an array of album paths.";
                return false;
            }
            for (const auto& albumPath : params["duplicateQuarantinePaths"].GetArray()) {
                if (!albumPath.IsString() || albumPath.GetStringLength() == 0) {
                    error = "duplicateQuarantinePaths must contain only non-empty strings.";
                    return false;
                }
                options.duplicateQuarantinePaths.emplace_back(
                    albumPath.GetString(), albumPath.GetStringLength());
            }
        }
    }
    return true;
}

std::string parseErrorText(const rapidjson::Document& document)
{
    return std::string("Invalid JSON request: ") + rapidjson::GetParseError_En(document.GetParseError());
}

void writeError(std::ostream& output, std::string requestId, std::string message)
{
    writeEvent(output, requestId, "error", [&message](JsonWriter& writer) {
        writer.Key("message");
        writer.String(message.c_str());
    });
}
}

int WorkerProtocol::Run(std::istream& input, std::ostream& output, const OperationRunner& runOperation)
{
    std::mutex outputMutex;
    writeEvent(output, "", "ready", [](JsonWriter& writer) {
        writer.Key("capabilities");
        writer.StartArray();
        for (const auto* capability : std::array{
                "convert", "scan", "duplicates", "databaseImport", "repair", "initializeConfig" }) {
            writer.String(capability);
        }
        writer.EndArray();
    });

    std::string line;
    while (std::getline(input, line)) {
        rapidjson::Document request;
        request.Parse(line.c_str(), line.size());
        std::string requestId;
        if (request.HasParseError()) {
            writeError(output, "", parseErrorText(request));
            continue;
        }
        std::string requestIdError;
        if (!request.IsObject() ||
            !getString(request, "requestId", requestId, true, requestIdError)) {
            writeError(output, "", "Request must be an object with a non-empty requestId.");
            continue;
        }
        if (!request.HasMember("protocolVersion") || !request["protocolVersion"].IsUint() ||
            request["protocolVersion"].GetUint() != kProtocolVersion) {
            writeError(output, requestId, "Unsupported or missing protocolVersion.");
            continue;
        }
        std::string type;
        std::string error;
        if (!getString(request, "type", type, true, error) ||
            (type != "run" && type != "preview")) {
            writeError(output, requestId, error.empty() ? "type must be 'run' or 'preview'." : std::move(error));
            continue;
        }

        CommandLineOptions options;
        if (!makeOptions(request, options, error)) {
            writeError(output, requestId, std::move(error));
            continue;
        }
        options.preview = type == "preview";
        if (options.preview && options.action != CommandAction::Convert &&
            options.action != CommandAction::Repair) {
            writeError(output, requestId, "Preview is supported only for convert and repair operations.");
            continue;
        }

        writeEvent(output, requestId, "started");
        RunLogging::SetProgressEventHandler([&output, &outputMutex, requestId](std::string message) {
            std::lock_guard lock(outputMutex);
            writeEvent(output, requestId, "progress", [&message](JsonWriter& writer) {
                writer.Key("message");
                writer.String(message.c_str(), static_cast<rapidjson::SizeType>(message.size()));
            });
        });
        std::string text;
        int exitCode = 1;
        try {
            exitCode = runOperation(options, text);
        }
        catch (const std::exception& exception) {
            text += std::string("Worker command failed: ") + exception.what() + "\n";
        }
        RunLogging::SetProgressEventHandler({});
        if (!text.empty()) {
            std::lock_guard lock(outputMutex);
            writeEvent(output, requestId, "output", [&text](JsonWriter& writer) {
                writer.Key("stream");
                writer.String("stdout");
                writer.Key("text");
                writer.String(text.c_str(), static_cast<rapidjson::SizeType>(text.size()));
            });
        }
        std::lock_guard lock(outputMutex);
        writeEvent(output, requestId, "completed", [exitCode](JsonWriter& writer) {
            writer.Key("exitCode");
            writer.Int(exitCode);
            writer.Key("status");
            writer.String(exitCode == 0 ? "succeeded" : "failed");
        });
    }
    return 0;
}
