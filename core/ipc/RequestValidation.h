#pragma once

// The validation tier every IPC request parameter passes through before a handler sees it
// (docs/ipc-contract.md, audit #21).
//
// Handlers used to index straight into the params object -- params.at("url"),
// params.at("priority").get<int>() -- which meant a caller sending the wrong shape got
// whatever nlohmann threw (json::out_of_range, json::type_error) reported back as a
// generic E_UNHANDLED_EXCEPTION carrying the library's own English exception text. That is
// three separate problems: the caller can't tell "you forgot a field" from "the core is
// broken", the error message leaks an implementation detail, and an out-of-range numeric
// value reaches the code that acts on it unchecked.
//
// These helpers live in core/ (not in main.cpp) specifically so they can be tested against
// adversarial input directly, which is the only way to have any confidence in a layer whose
// entire job is to survive input nobody would write on purpose.
//
// Every failure throws errors::MediaToolException with a code naming the failure mode
// (E_MISSING_PARAM / E_INVALID_PARAM_TYPE / E_INVALID_PARAM_VALUE), a message written for
// the person using the app, and details naming the field for the person debugging it. An
// explicit JSON `null` is treated as "absent" throughout: the Rust and TypeScript sides
// both serialize an unset optional as null, and a protocol that distinguishes the two
// would be a protocol nobody can implement correctly by accident.

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace mediatool::ipc {

// True if `field` is present and not null. Prefer the typed helpers below to branching on
// this; it exists for the handful of handlers that genuinely need to know.
bool HasParam(const nlohmann::json& params, std::string_view field);

// Required string. Throws if absent/null (E_MISSING_PARAM) or not a JSON string
// (E_INVALID_PARAM_TYPE). Empty strings are allowed -- use RequireNonEmptyString when
// they aren't.
std::string RequireString(const nlohmann::json& params, std::string_view field);

// As RequireString, plus rejects "" (E_INVALID_PARAM_VALUE).
std::string RequireNonEmptyString(const nlohmann::json& params, std::string_view field);

// Absent/null yields std::nullopt. A present non-string still throws: a caller that sends
// `{"formatId": 137}` has made a mistake worth reporting, not one worth guessing at.
std::optional<std::string> OptionalString(const nlohmann::json& params, std::string_view field);

// Required integer, bounds-checked inclusively. Rejects non-integers, including JSON
// floats and booleans (E_INVALID_PARAM_TYPE) and out-of-range values
// (E_INVALID_PARAM_VALUE). The range is mandatory rather than defaulted: every integer
// this protocol accepts turns into an allocation, a loop bound, or a thread count
// somewhere, so "no particular limit" is never the right answer.
std::int64_t RequireInt(const nlohmann::json& params, std::string_view field, std::int64_t min,
                         std::int64_t max);

// Absent/null yields `defaultValue`; anything present is validated exactly as RequireInt.
std::int64_t OptionalInt(const nlohmann::json& params, std::string_view field,
                          std::int64_t defaultValue, std::int64_t min, std::int64_t max);

// Absent/null yields `defaultValue`. A present non-boolean throws rather than being
// coerced -- 0/1/"true" are not booleans on this wire protocol.
bool OptionalBool(const nlohmann::json& params, std::string_view field, bool defaultValue);

// Required string restricted to `allowed`. The rejection message lists the allowed values,
// so a caller (or a developer reading a log) never has to go find them.
std::string RequireEnum(const nlohmann::json& params, std::string_view field,
                         std::initializer_list<std::string_view> allowed);

// Required JSON object. Returned by reference into `params`, which must outlive the use.
const nlohmann::json& RequireObject(const nlohmann::json& params, std::string_view field);

// Absent/null yields an empty object; a present non-object throws. Returns a reference to
// either the field or a shared static empty object.
const nlohmann::json& OptionalObject(const nlohmann::json& params, std::string_view field);

// Absent/null yields an empty vector. Every element must be a non-empty string, and the
// array may hold at most `maxItems` of them -- an unbounded list from an untrusted caller
// is an unbounded allocation.
std::vector<std::string> OptionalStringArray(const nlohmann::json& params, std::string_view field,
                                              std::size_t maxItems);

}  // namespace mediatool::ipc
