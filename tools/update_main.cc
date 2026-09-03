// Rebuilds and reinstalls the library, and can pull in a newer Ghidra release
// before doing so.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char *const SOURCE_ROOT = ASTRAL_SOURCE_ROOT;
const char *const INSTALL_ROOT = ASTRAL_INSTALL_ROOT;
const char *const VERSION = ASTRAL_VERSION;
const char *const UPSTREAM = ASTRAL_GHIDRA_VERSION;

const char *const ASTRAL_REPO = "https://github.com/NationalSecurityAgency/ghidra";

int usage()
{
    std::fprintf(stderr,
                 "usage: astral-update [install] [options]\n"
                 "\n"
                 "Rebuilds Astral from %s and installs it into %s.\n"
                 "With the install subcommand it goes to /usr/local instead, asking for\n"
                 "sudo only if that location is not already writable.\n"
                 "\n"
                 "      --check             report the installed and latest Ghidra versions\n"
                 "      --ghidra <version>  vendor this Ghidra release first, e.g. 12.1.3\n"
                 "      --languages <list>  semicolon-separated processors to compile specs for,\n"
                 "                          or ALL\n"
                 "      --prefix <dir>      install somewhere other than the default\n"
                 "      --jobs <n>          parallel build jobs (default: all cores)\n"
                 "      --clean             discard the build directory first\n",
                 SOURCE_ROOT, INSTALL_ROOT);
    return 2;
}

std::string quote(const std::string &s) { return "'" + s + "'"; }

// True when the process can already create files under `path`, walking up to
// the nearest existing ancestor when the directory itself is not there yet.
bool writable(const std::string &path)
{
    std::string candidate = path;
    while (!candidate.empty()) {
        if (access(candidate.c_str(), F_OK) == 0)
            return access(candidate.c_str(), W_OK) == 0;
        const size_t slash = candidate.find_last_of('/');
        if (slash == std::string::npos || slash == 0)
            break;
        candidate = candidate.substr(0, slash);
    }
    return access("/", W_OK) == 0;
}

// Whether the install can proceed without escalating.
//
// A prefix like /usr/local is often root-owned while lib, include, bin and
// share under it are not, so each destination is checked on its own; only a
// directory that has to be created needs the prefix itself to be writable.
bool install_tree_writable(const std::string &prefix)
{
    if (access(prefix.c_str(), F_OK) != 0)
        return writable(prefix);

    static const char *const parts[] = {"/lib", "/include", "/bin", "/share"};
    bool must_create = false;
    for (const char *part : parts) {
        const std::string path = prefix + part;
        if (access(path.c_str(), F_OK) != 0)
            must_create = true;
        else if (access(path.c_str(), W_OK) != 0)
            return false;
    }
    return must_create ? access(prefix.c_str(), W_OK) == 0 : true;
}

int run(const std::string &command)
{
    std::fprintf(stderr, "==> %s\n", command.c_str());
    int rc = std::system(command.c_str());
    return rc == 0 ? 0 : 1;
}

std::string capture(const std::string &command)
{
    std::string out;
    FILE *pipe = popen(command.c_str(), "r");
    if (pipe == nullptr)
        return out;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
        out += buffer;
    pclose(pipe);
    return out;
}

// Latest release tag, as reported by the GitHub API.
std::string latest_astral_version()
{
    std::string json = capture("curl -sS --max-time 30 "
                               "https://api.github.com/repos/NationalSecurityAgency/ghidra/tags");
    const std::string marker = "\"name\": \"Ghidra_";
    size_t pos = json.find(marker);
    if (pos == std::string::npos)
        return std::string();
    pos += marker.size();
    size_t end = json.find("_build", pos);
    if (end == std::string::npos)
        return std::string();
    return json.substr(pos, end - pos);
}

// Replaces the vendored decompiler sources and processor specs with those of the
// requested Ghidra release.
int vendor_ghidra(const std::string &version)
{
    const std::string root = SOURCE_ROOT;
    const std::string work = root + "/build/vendor";
    const std::string tag = "Ghidra_" + version + "_build";
    const std::string tarball = work + "/ghidra-src.tar.gz";
    const std::string prefix = "ghidra-" + tag;

    if (run("mkdir -p " + quote(work)) != 0)
        return 1;
    if (run("curl -sSL --fail --max-time 1800 -o " + quote(tarball) + " " + quote(std::string(ASTRAL_REPO) + "/archive/refs/tags/" + tag + ".tar.gz")) != 0) {
        std::fprintf(stderr, "astral-update: could not download Ghidra %s\n", version.c_str());
        return 1;
    }
    if (run("rm -rf " + quote(work + "/x") + " && mkdir -p " + quote(work + "/x")) != 0)
        return 1;
    const std::string extract = "tar xzf " + quote(tarball) + " -C " + quote(work + "/x") +
                                " --strip-components=1 " +
                                quote(prefix + "/Ghidra/Features/Decompiler/src/decompile/cpp") +
                                " " + quote(prefix + "/Ghidra/Processors");
    if (run(extract) != 0)
        return 1;

    const std::string src = work + "/x/Ghidra";
    const std::string dst = root + "/third_party/ghidra";
    if (run("rm -rf " + quote(dst + "/decompile") + " " + quote(dst + "/processors")) != 0)
        return 1;
    if (run("mkdir -p " + quote(dst + "/decompile") + " " + quote(dst + "/processors")) != 0)
        return 1;
    if (run("cp -R " + quote(src + "/Features/Decompiler/src/decompile/cpp/.") + " " +
            quote(dst + "/decompile/")) != 0)
        return 1;
    // Only the language data is needed; the Java side of each processor is not.
    const std::string copy_specs =
        "for p in " + quote(src + "/Processors") + "/*/; do n=$(basename \"$p\"); "
        "if [ -d \"$p/data/languages\" ]; then mkdir -p " + quote(dst + "/processors") +
        "/\"$n\"/data && cp -R \"$p/data/languages\" " + quote(dst + "/processors") +
        "/\"$n\"/data/; fi; done";
    if (run(copy_specs) != 0)
        return 1;

    std::ofstream version_file(dst + "/VERSION");
    if (!version_file) {
        std::fprintf(stderr, "astral-update: cannot write %s/VERSION\n", dst.c_str());
        return 1;
    }
    version_file << version << "\n";
    version_file.close();

    run("rm -rf " + quote(work));
    std::fprintf(stderr, "vendored Ghidra %s\n", version.c_str());
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    std::string astral_version;
    std::string languages;
    std::string prefix = INSTALL_ROOT;
    std::string jobs;
    bool check = false;
    bool clean = false;

    int first = 1;
    if (argc > 1 && std::string(argv[1]) == "install") {
        prefix = "/usr/local";
        first = 2;
    }

    for (int i = first; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char *what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "astral-update: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--check")
            check = true;
        else if (arg == "--ghidra")
            astral_version = next("--ghidra");
        else if (arg == "--languages")
            languages = next("--languages");
        else if (arg == "--prefix")
            prefix = next("--prefix");
        else if (arg == "--jobs")
            jobs = next("--jobs");
        else if (arg == "--clean")
            clean = true;
        else
            return usage();
    }

    if (check) {
        std::printf("Astral %s\n", VERSION);
        std::printf("vendored Ghidra     %s\n", UPSTREAM);
        std::string latest = latest_astral_version();
        if (latest.empty())
            std::printf("latest Ghidra       (could not reach github.com)\n");
        else {
            std::printf("latest Ghidra       %s\n", latest.c_str());
            if (latest != UPSTREAM)
                std::printf("\nrun: astral-update --ghidra %s\n", latest.c_str());
        }
        return 0;
    }

    if (!astral_version.empty() && vendor_ghidra(astral_version) != 0)
        return 1;

    const std::string build_dir = std::string(SOURCE_ROOT) + "/build";
    if (clean && run("rm -rf " + quote(build_dir)) != 0)
        return 1;

    std::string configure = "cmake -S " + quote(SOURCE_ROOT) + " -B " + quote(build_dir) +
                            " -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=" + quote(prefix);
    if (!languages.empty())
        configure += " -DASTRAL_LANGUAGES=" + quote(languages);
    if (run(configure) != 0)
        return 1;

    std::string build = "cmake --build " + quote(build_dir);
    if (!jobs.empty())
        build += " --parallel " + jobs;
    else
        build += " --parallel";
    if (run(build) != 0)
        return 1;

    std::string install = "cmake --install " + quote(build_dir);
    if (!install_tree_writable(prefix)) {
        std::fprintf(stderr, "\n%s is not writable by this user; installing with sudo\n",
                     prefix.c_str());
        install = "sudo " + install;
    }
    if (run(install) != 0)
        return 1;

    std::printf("\ninstalled to %s\n", prefix.c_str());
    if (prefix != "/usr/local")
        std::printf("compile against it with -I%s/include -L%s/lib -lAstral\n", prefix.c_str(),
                    prefix.c_str());
    else
        std::printf("compile against it with -lAstral\n");
    return 0;
}
