// Finding things at run time.
#include "paths.hh"

#include <cstdlib>
#include <sys/stat.h>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if !defined(_WIN32)
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace astral_internal {
namespace {

bool is_directory(const std::string &path)
{
    struct stat info;
    return !path.empty() && stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string parent_of(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos || slash == 0 ? std::string() : path.substr(0, slash);
}

// Walks up from a file until a directory holding share/astral is found. That
// directory is the install this copy belongs to, wherever it was put.
std::string prefix_above(const std::string &file)
{
    std::string directory = parent_of(file);
    for (int step = 0; step < 6 && !directory.empty(); ++step) {
        if (is_directory(directory + "/share/astral"))
            return directory;
        directory = parent_of(directory);
    }
    return std::string();
}

} // namespace

std::string executable_path()
{
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return std::string();
    return std::string(buffer.data());
#elif defined(_WIN32)
    return std::string();
#else
    std::vector<char> buffer(4096, '\0');
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    return length > 0 ? std::string(buffer.data(), static_cast<size_t>(length)) : std::string();
#endif
}

std::string library_path()
{
#if defined(_WIN32)
    return std::string();
#else
    // Asking the loader where this very function lives finds the library even
    // when the program that loaded it lives somewhere else entirely.
    Dl_info info;
    if (dladdr(reinterpret_cast<const void *>(&library_path), &info) != 0 &&
        info.dli_fname != nullptr)
        return std::string(info.dli_fname);
    return std::string();
#endif
}

std::string install_prefix()
{
    const std::string from_library = prefix_above(library_path());
    if (!from_library.empty())
        return from_library;
    return prefix_above(executable_path());
}

std::string default_spec_root()
{
    if (const char *set = std::getenv("ASTRAL_SPECS"))
        if (*set != '\0')
            return set;

    const std::string prefix = install_prefix();
    if (!prefix.empty() && is_directory(prefix + "/share/astral/specs"))
        return prefix + "/share/astral/specs";

    // A build tree keeps them beside the binary rather than in an install.
    for (const std::string &near : {parent_of(executable_path()), parent_of(library_path())})
        if (!near.empty() && is_directory(near + "/specs"))
            return near + "/specs";

    for (const char *system : {"/usr/local/share/astral/specs", "/usr/share/astral/specs"})
        if (is_directory(system))
            return system;

    return std::string();
}

} // namespace astral_internal
