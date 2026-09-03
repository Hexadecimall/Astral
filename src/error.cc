#include "error.hh"

namespace astral_internal {

namespace {
thread_local std::string g_last_error;
}

void set_error(const std::string &message) { g_last_error = message; }

void clear_error() { g_last_error.clear(); }

const char *last_error() { return g_last_error.c_str(); }

astral_status fail(astral_status status, const std::string &message)
{
    g_last_error = message;
    return status;
}

} // namespace astral_internal
