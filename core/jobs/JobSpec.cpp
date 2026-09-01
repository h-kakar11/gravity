#include "core/jobs/JobSpec.h"

namespace mediatool::jobs {

nlohmann::json JobSpec::ToJson() const {
    nlohmann::json json;
    json["id"] = id;
    json["type"] = ToWireString(type);
    json["params"] = params;
    json["createdAt"] = createdAt;
    json["recoveryCount"] = recoveryCount;
    if (artifact) {
        json["artifact"] = {{"outputDirectory", artifact->outputDirectory},
                             {"filenameBase", artifact->filenameBase}};
    }
    return json;
}

JobSpec JobSpec::FromJson(const nlohmann::json& json) {
    JobSpec spec;
    spec.id = json.at("id").get<std::string>();
    spec.type = JobTypeFromWireString(json.at("type").get<std::string>());
    // Tolerated as absent rather than required: a TEST job legitimately has no params.
    if (json.contains("params") && json.at("params").is_object()) {
        spec.params = json.at("params");
    }
    spec.createdAt = json.value("createdAt", std::string());
    spec.recoveryCount = json.value("recoveryCount", 0);
    if (json.contains("artifact") && json.at("artifact").is_object()) {
        const nlohmann::json& artifact = json.at("artifact");
        JobArtifactLocation location;
        location.outputDirectory = artifact.value("outputDirectory", std::string());
        location.filenameBase = artifact.value("filenameBase", std::string());
        // A half-written artifact record cleans up nothing and could scope a delete to an
        // empty base name; treat it as absent.
        if (!location.outputDirectory.empty() && !location.filenameBase.empty()) {
            spec.artifact = location;
        }
    }
    return spec;
}

}  // namespace mediatool::jobs
