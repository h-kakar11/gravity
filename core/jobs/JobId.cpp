#include "core/jobs/JobTypes.h"

#include "core/common/Uuid.h"

namespace mediatool::jobs {

JobId GenerateJobId() { return "job-" + common::GenerateUuidV4(); }

}  // namespace mediatool::jobs
