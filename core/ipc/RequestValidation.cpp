#include "core/ipc/RequestValidation.h"

#include <sstream>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"

namespace mediatool::ipc {

namespace {

std::string FieldOf(std::string_view field) { return std::string(field); }

[[noreturn]] void ThrowMissing(std::string_view field) {
    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_MISSING_PARAM", errors::ErrorCategory::Unknown, FieldOf(field) + " is required.",
        "field=" + FieldOf(field)));
}

[[noreturn]] void ThrowType(std::string_view field, std::string_view expected,
                             const nlohmann::json& actual) {
    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_INVALID_PARAM_TYPE", errors::ErrorCategory::Unknown,
        FieldOf(field) + " must be " + std::string(expected) + ".",
        "field=" + FieldOf(field) + " actualType=" + actual.type_name()));
}

[[noreturn]] void ThrowValue(std::string_view field, const std::string& why,
                              const std::string& details) {
    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_INVALID_PARAM_VALUE", errors::ErrorCategory::Unknown, FieldOf(field) + " " + why,
        "field=" + FieldOf(field) + " " + details));
}

// Absent and explicit-null are the same thing here (see the header). Returns nullptr for
// both so every helper below shares one notion of "not supplied".
const nlohmann::json* Find(const nlohmann::json& params, std::string_view field) {
    if (!params.is_object()) return nullptr;
    const auto it = params.find(field);
    if (it == params.end() || it->is_null()) return nullptr;
    return &*it;
}

std::int64_t CheckedInt(const nlohmann::json& value, std::string_view field, std::int64_t min,
                         std::int64_t max) {
    // is_number_integer() is true for JSON booleans in some libraries; nlohmann keeps them
    // distinct, but check explicitly so this stays correct if that ever changes -- an
    // accidental `true` arriving as 1 is exactly the kind of coercion this tier exists to
    // prevent.
    if (value.is_boolean() || !value.is_number_integer()) {
        ThrowType(field, "an integer", value);
    }
    // Read as the widest signed type first: a value beyond int64 range (or a negative one
    // sent where the JSON parser stored it unsigned) must be reported as out of range, not
    // silently wrapped by the get<> conversion.
    if (value.is_number_unsigned() &&
        value.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) {
        ThrowValue(field, "is out of range.",
                   "value exceeds the maximum representable integer; allowed range is " +
                       std::to_string(min) + ".." + std::to_string(max));
    }
    const auto number = value.get<std::int64_t>();
    if (number < min || number > max) {
        ThrowValue(field, "is out of range.",
                   "value=" + std::to_string(number) + " allowed=" + std::to_string(min) + ".." +
                       std::to_string(max));
    }
    return number;
}

}  // namespace

bool HasParam(const nlohmann::json& params, std::string_view field) {
    return Find(params, field) != nullptr;
}

std::string RequireString(const nlohmann::json& params, std::string_view field) {
    const nlohmann::json* value = Find(params, field);
    if (value == nullptr) ThrowMissing(field);
    if (!value->is_string()) ThrowType(field, "a string", *value);
    return value->get<std::string>();
}

std::string RequireNonEmptyString(const nlohmann::json& params, std::string_view field) {
    std::string value = RequireString(params, field);
    if (value.empty()) ThrowValue(field, "must not be empty.", "value is an empty string");
    return value;
}

std::optional<std::string> OptionalString(const nlohmann::json& params, std::string_view field) {
    const nlohmann::json* value = Find(params, field);
    if (value == nullptr) return std::nullopt;
    if (!value->is_string()) ThrowType(field, "a string", *value);
    return value->get<std::string>();
}

std::int64_t RequireInt(const nlohmann::json& params, std::string_view field, std::int64_t min,
                         std::int64_t max) {
    const nlohmann::json* value = Find(params, field);
    if (value == nullptr) ThrowMissing(field);
    return CheckedInt(*value, field, min, max);
}

std::int64_t OptionalInt(const nlohmann::json& params, std::string_view field,
                          std::int64_t defaultValue, std::int64_t min, std::int64_t max) {
    const nlohmann::json* value = Find(params, field);
    if (value == nullptr) return defaultValue;
    return CheckedInt(*value, field, min, max);
}

bool OptionalBool(const nlohmann::json& params, std::string_view field, bool defaultValue) {
    const nlohmann::json* value = Find(params, field);
    if (value == nullptr) return defaultValue;
    if (!value->is_boolean()) ThrowType(field, "a boolean", *value);
    return value->get<bool>();
}

std::string RequireEnum(const nlohmann::json& params, std::string_view field,
                         std::initializer_list<std::string_view> allowed) {
    const std::string value = RequireString(params, field);
    for (const std::string_view candidate : allowed) {
        if (value == candidate) return value;
    }

    std::ostringstream allowedList;
    bool first = true;
    for (const std::string_view candidate : allowed) {
        if (!first) allowedList << ", ";
        allowedList << candidate;
        first = false;
    }
    ThrowValue(field, "is not one of the allowed values.",
               "value=" + value + " allowed=[" + allowedList.str() + "]");
}

const nlohmann::json& RequireObject(const nlohmann::json& params, std::string_view field) {
    const nlohmann::json* value = Find(params, field);
    if (value == nullptr) ThrowMissing(field);
    if (!value->is_object()) ThrowType(field, "an object", *value);
    return *value;
}

const nlohmann::json& OptionalObject(const nlohmann::json& params, std::string_view field) {
    static const nlohmann::json kEmpty = nlohmann::json::object();
    const nlohmann::json* value = Find(params, field);
    if (value == nullptr) return kEmpty;
    if (!value->is_object()) ThrowType(field, "an object", *value);
    return *value;
}

std::vector<std::string> OptionalStringArray(const nlohmann::json& params, std::string_view field,
                                              std::size_t maxItems) {
    const nlohmann::json* value = Find(params, field);
    if (value == nullptr) return {};
    if (!value->is_array()) ThrowType(field, "an array of strings", *value);
    if (value->size() > maxItems) {
        ThrowValue(field, "has too many entries.",
                   "count=" + std::to_string(value->size()) + " max=" + std::to_string(maxItems));
    }

    std::vector<std::string> result;
    result.reserve(value->size());
    for (const auto& element : *value) {
        if (!element.is_string()) ThrowType(field, "an array of strings", element);
        std::string entry = element.get<std::string>();
        if (entry.empty()) ThrowValue(field, "must not contain empty strings.", "empty array entry");
        result.push_back(std::move(entry));
    }
    return result;
}

}  // namespace mediatool::ipc
