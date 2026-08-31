#include "core/ipc/RequestValidation.h"

#include <limits>
#include <string>

#include <gtest/gtest.h>

#include "core/errors/MediaToolException.h"

namespace mediatool::ipc {
namespace {

using errors::MediaToolException;
using json = nlohmann::json;

// Asserts that `expression` throws with exactly `expectedCode`, and that the details name
// the field. The code is the part a caller can branch on, so "it threw something" is not
// the assertion worth making -- E_MISSING_PARAM and E_INVALID_PARAM_TYPE mean different
// things to whoever is fixing the call.
#define EXPECT_REJECTED_WITH(expression, expectedCode, expectedField)                     \
    do {                                                                                  \
        try {                                                                             \
            (void)(expression);                                                           \
            FAIL() << "expected " << expectedCode << " but nothing was thrown";           \
        } catch (const MediaToolException& e) {                                           \
            EXPECT_EQ(e.Info().code, expectedCode);                                       \
            EXPECT_NE(e.Info().details.find(expectedField), std::string::npos)            \
                << "details should name the offending field: " << e.Info().details;       \
            EXPECT_FALSE(e.Info().message.empty());                                       \
        }                                                                                 \
    } while (false)

TEST(RequestValidation, RequireStringAcceptsAStringAndRejectsEverythingElse) {
    const json params{{"jobId", "job-1"}, {"number", 7}, {"object", json::object()},
                      {"array", json::array()}, {"boolean", true}, {"nulled", nullptr}};

    EXPECT_EQ(RequireString(params, "jobId"), "job-1");

    EXPECT_REJECTED_WITH(RequireString(params, "absent"), "E_MISSING_PARAM", "absent");
    // Explicit null is treated as absent, not as a wrong-typed value.
    EXPECT_REJECTED_WITH(RequireString(params, "nulled"), "E_MISSING_PARAM", "nulled");
    EXPECT_REJECTED_WITH(RequireString(params, "number"), "E_INVALID_PARAM_TYPE", "number");
    EXPECT_REJECTED_WITH(RequireString(params, "object"), "E_INVALID_PARAM_TYPE", "object");
    EXPECT_REJECTED_WITH(RequireString(params, "array"), "E_INVALID_PARAM_TYPE", "array");
    EXPECT_REJECTED_WITH(RequireString(params, "boolean"), "E_INVALID_PARAM_TYPE", "boolean");
}

TEST(RequestValidation, RequireStringOnANonObjectParamsIsAMissingParamNotACrash) {
    // A request whose "params" is an array or a scalar reaches handlers as-is if the
    // envelope check is ever bypassed; every helper must survive it.
    EXPECT_REJECTED_WITH(RequireString(json::array({1, 2}), "jobId"), "E_MISSING_PARAM", "jobId");
    EXPECT_REJECTED_WITH(RequireString(json("scalar"), "jobId"), "E_MISSING_PARAM", "jobId");
    EXPECT_REJECTED_WITH(RequireString(json(nullptr), "jobId"), "E_MISSING_PARAM", "jobId");
}

TEST(RequestValidation, NonEmptyStringRejectsTheEmptyString) {
    const json params{{"name", ""}, {"other", "x"}};
    EXPECT_EQ(RequireNonEmptyString(params, "other"), "x");
    EXPECT_REJECTED_WITH(RequireNonEmptyString(params, "name"), "E_INVALID_PARAM_VALUE", "name");
}

TEST(RequestValidation, OptionalStringDistinguishesAbsentFromWrongType) {
    const json params{{"present", "value"}, {"nulled", nullptr}, {"wrong", 3}};
    EXPECT_EQ(OptionalString(params, "present"), std::optional<std::string>("value"));
    EXPECT_FALSE(OptionalString(params, "absent").has_value());
    EXPECT_FALSE(OptionalString(params, "nulled").has_value());
    EXPECT_REJECTED_WITH(OptionalString(params, "wrong"), "E_INVALID_PARAM_TYPE", "wrong");
}

TEST(RequestValidation, RequireIntEnforcesTypeAndRange) {
    const json params{{"n", 5}, {"negative", -5}, {"float", 1.5}, {"boolean", true},
                      {"string", "5"}};

    EXPECT_EQ(RequireInt(params, "n", 1, 10), 5);
    EXPECT_EQ(RequireInt(params, "negative", -10, 10), -5);

    EXPECT_REJECTED_WITH(RequireInt(params, "n", 1, 4), "E_INVALID_PARAM_VALUE", "n");
    EXPECT_REJECTED_WITH(RequireInt(params, "n", 6, 10), "E_INVALID_PARAM_VALUE", "n");
    // A float is not an integer, and neither is a boolean or a numeric-looking string --
    // none of them get coerced.
    EXPECT_REJECTED_WITH(RequireInt(params, "float", 0, 10), "E_INVALID_PARAM_TYPE", "float");
    EXPECT_REJECTED_WITH(RequireInt(params, "boolean", 0, 10), "E_INVALID_PARAM_TYPE", "boolean");
    EXPECT_REJECTED_WITH(RequireInt(params, "string", 0, 10), "E_INVALID_PARAM_TYPE", "string");
    EXPECT_REJECTED_WITH(RequireInt(params, "absent", 0, 10), "E_MISSING_PARAM", "absent");
}

TEST(RequestValidation, RequireIntRejectsValuesBeyondTheSignedRange) {
    // listJobHistory's limit used to be read with get<std::size_t>(), where a negative
    // value wrapped to something enormous. The bounds check has to happen on a signed
    // read, and a value past int64 has to be rejected rather than wrapped.
    json params;
    params["huge"] = std::numeric_limits<std::uint64_t>::max();
    params["negative"] = -1;

    EXPECT_REJECTED_WITH(RequireInt(params, "huge", 1, 10000), "E_INVALID_PARAM_VALUE", "huge");
    EXPECT_REJECTED_WITH(RequireInt(params, "negative", 1, 10000), "E_INVALID_PARAM_VALUE",
                          "negative");
}

TEST(RequestValidation, OptionalIntFallsBackToTheDefaultAndStillValidates) {
    const json params{{"priority", 250}, {"nulled", nullptr}, {"bad", "high"}};
    EXPECT_EQ(OptionalInt(params, "priority", 0, -1000, 1000), 250);
    EXPECT_EQ(OptionalInt(params, "absent", 42, -1000, 1000), 42);
    EXPECT_EQ(OptionalInt(params, "nulled", 42, -1000, 1000), 42);
    EXPECT_REJECTED_WITH(OptionalInt(params, "priority", 0, -10, 10), "E_INVALID_PARAM_VALUE",
                          "priority");
    EXPECT_REJECTED_WITH(OptionalInt(params, "bad", 0, -10, 10), "E_INVALID_PARAM_TYPE", "bad");
}

TEST(RequestValidation, OptionalBoolDoesNotCoerce) {
    const json params{{"flag", true}, {"one", 1}, {"text", "true"}};
    EXPECT_TRUE(OptionalBool(params, "flag", false));
    EXPECT_TRUE(OptionalBool(params, "absent", true));
    EXPECT_REJECTED_WITH(OptionalBool(params, "one", false), "E_INVALID_PARAM_TYPE", "one");
    EXPECT_REJECTED_WITH(OptionalBool(params, "text", false), "E_INVALID_PARAM_TYPE", "text");
}

TEST(RequestValidation, RequireEnumAcceptsOnlyTheAllowlistAndSaysWhatIsAllowed) {
    const json params{{"kind", "DOWNLOAD"}, {"bad", "SOMETHING"}};
    EXPECT_EQ(RequireEnum(params, "kind", {"DOWNLOAD", "CONVERSION"}), "DOWNLOAD");

    try {
        RequireEnum(params, "bad", {"DOWNLOAD", "CONVERSION"});
        FAIL() << "expected the value to be rejected";
    } catch (const MediaToolException& e) {
        EXPECT_EQ(e.Info().code, "E_INVALID_PARAM_VALUE");
        EXPECT_NE(e.Info().details.find("DOWNLOAD"), std::string::npos)
            << "the rejection should list the allowed values: " << e.Info().details;
        EXPECT_NE(e.Info().details.find("CONVERSION"), std::string::npos);
    }
}

TEST(RequestValidation, ObjectHelpersRejectNonObjects) {
    const json params{{"settings", {{"a", 1}}}, {"wrong", "not an object"}};
    EXPECT_EQ(RequireObject(params, "settings").at("a"), 1);
    EXPECT_REJECTED_WITH(RequireObject(params, "absent"), "E_MISSING_PARAM", "absent");
    EXPECT_REJECTED_WITH(RequireObject(params, "wrong"), "E_INVALID_PARAM_TYPE", "wrong");

    EXPECT_TRUE(OptionalObject(params, "absent").empty());
    EXPECT_TRUE(OptionalObject(params, "absent").is_object());
    EXPECT_REJECTED_WITH(OptionalObject(params, "wrong"), "E_INVALID_PARAM_TYPE", "wrong");
}

TEST(RequestValidation, OptionalStringArrayIsBoundedAndTypeChecked) {
    const json params{{"dependsOn", {"job-1", "job-2"}},
                      {"mixed", {"job-1", 2}},
                      {"empties", {"job-1", ""}},
                      {"notArray", "job-1"}};

    EXPECT_EQ(OptionalStringArray(params, "dependsOn", 8),
              (std::vector<std::string>{"job-1", "job-2"}));
    EXPECT_TRUE(OptionalStringArray(params, "absent", 8).empty());
    EXPECT_REJECTED_WITH(OptionalStringArray(params, "notArray", 8), "E_INVALID_PARAM_TYPE",
                          "notArray");
    EXPECT_REJECTED_WITH(OptionalStringArray(params, "mixed", 8), "E_INVALID_PARAM_TYPE", "mixed");
    EXPECT_REJECTED_WITH(OptionalStringArray(params, "empties", 8), "E_INVALID_PARAM_VALUE",
                          "empties");
    // The bound is the point: an untrusted caller must not be able to make this allocate
    // as much as it likes.
    EXPECT_REJECTED_WITH(OptionalStringArray(params, "dependsOn", 1), "E_INVALID_PARAM_VALUE",
                          "dependsOn");
}

TEST(RequestValidation, DeeplyNestedAndOversizedValuesAreRejectedByTypeNotBySize) {
    // Adversarial but well-formed input: a 100k-element array where a string is expected.
    // The type check must reject it immediately rather than iterating it.
    json params;
    params["url"] = json::array();
    for (int i = 0; i < 100000; ++i) params["url"].push_back(i);
    EXPECT_REJECTED_WITH(RequireString(params, "url"), "E_INVALID_PARAM_TYPE", "url");

    // A very long but genuinely-string value is not the validation tier's problem to
    // reject: it is a legal string, and the line-length limit is what bounds it.
    params["long"] = std::string(200000, 'x');
    EXPECT_EQ(RequireString(params, "long").size(), 200000u);
}

}  // namespace
}  // namespace mediatool::ipc
