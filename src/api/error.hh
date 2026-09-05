#ifndef ASTRAL_ERROR_HH
#define ASTRAL_ERROR_HH

#include "astral/astral.h"

#include <string>

namespace astral_internal {

// Records a message for astral_last_error on the calling thread and returns the
// status unchanged, so call sites can `return fail(ASTRAL_ERR_IO, "...")`.
astral_status fail(astral_status status, const std::string &message);

// Same, for functions whose failure value is a null pointer.
void set_error(const std::string &message);
void clear_error();
const char *last_error();

} // namespace astral_internal

#endif
