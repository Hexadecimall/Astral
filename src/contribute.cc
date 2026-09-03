// Sending what you have taught Astral back to the project.
//
// The exchange is deliberately a conversation with the repository rather than a
// hard-coded upload. Astral asks what the repository accepts, the repository
// answers with a published policy, and only then is anything sent. That means
// the rules can change without every installed copy needing to.
//
// What travels is what a name means: a fingerprint and the word someone chose
// for it. What does not travel is anything about the person or the machine, and
// that filtering happens here, before the network is touched at all.
#include "contribute.hh"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace astral_internal {
namespace {

std::string trim(const std::string &text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool split_first(const std::string &line, std::string &head, std::string &rest)
{
    const size_t at = line.find_first_of(" \t");
    if (at == std::string::npos) {
        head = line;
        rest.clear();
        return false;
    }
    head = line.substr(0, at);
    rest = trim(line.substr(at));
    return true;
}

// Runs a command and returns what it printed. Used for curl and for asking a
// logged-in command-line tool for its token.
std::string capture(const std::string &command)
{
    std::string out;
    FILE *pipe = popen(command.c_str(), "r");
    if (pipe == nullptr)
        return out;
    char buffer[8192];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
        out += buffer;
    pclose(pipe);
    return out;
}

std::string shell_quote(const std::string &text)
{
    std::string out = "'";
    for (char c : text) {
        if (c == '\'')
            out += "'\\''";
        else
            out.push_back(c);
    }
    return out + "'";
}

// Escapes a string for a JSON body.
std::string json_string(const std::string &text)
{
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            } else {
                out.push_back(c);
            }
        }
    }
    return out + "\"";
}

std::string json_field(const std::string &body, const std::string &name)
{
    const std::string key = "\"" + name + "\"";
    size_t at = body.find(key);
    if (at == std::string::npos)
        return std::string();
    at = body.find(':', at + key.size());
    if (at == std::string::npos)
        return std::string();
    at = body.find('"', at);
    if (at == std::string::npos)
        return std::string();
    std::string value;
    for (size_t i = at + 1; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size()) {
            ++i;
            value.push_back(body[i]);
            continue;
        }
        if (body[i] == '"')
            break;
        value.push_back(body[i]);
    }
    return value;
}

} // namespace

std::string github_token()
{
    for (const char *name : {"ASTRAL_GITHUB_TOKEN", "GITHUB_TOKEN", "GH_TOKEN"}) {
        const char *value = std::getenv(name);
        if (value != nullptr && *value != '\0')
            return value;
    }
    // A logged-in command-line tool already holds one; borrowing it saves the
    // user from creating a second.
    const std::string borrowed = trim(capture("ghx auth token 2>/dev/null"));
    if (!borrowed.empty() && borrowed.find(' ') == std::string::npos)
        return borrowed;
    return std::string();
}

ContributionPolicy fetch_policy(const std::string &repo, const std::string &branch)
{
    ContributionPolicy policy;

    // The contents endpoint answers as soon as a commit lands and works for a
    // private repository too, where the raw host would not. The raw host is
    // kept as a fallback for anyone without a token or behind a proxy.
    std::string authorization;
    const std::string token = github_token();
    if (!token.empty())
        authorization = " -H " + shell_quote("Authorization: Bearer " + token);

    std::string text = capture("curl -sS --fail --max-time 30 -H " +
                               shell_quote("Accept: application/vnd.github.raw") + authorization +
                               " " +
                               shell_quote("https://api.github.com/repos/" + repo +
                                           "/contents/contrib/policy.astral?ref=" + branch) +
                               " 2>/dev/null");
    if (trim(text).empty()) {
        const std::string url = "https://raw.githubusercontent.com/" + repo + "/" + branch +
                                "/contrib/policy.astral";
        text = capture("curl -sS --fail --max-time 30 " + shell_quote(url) + " 2>/dev/null");
    }
    if (trim(text).empty()) {
        policy.error = "could not read the contribution policy from " + repo +
                       " (no network, or the repository does not publish one)";
        return policy;
    }

    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        std::string kind, rest;
        split_first(line, kind, rest);
        if (kind == "accept")
            policy.accepted = rest == "yes" || rest == "true";
        else if (kind == "method")
            policy.method = rest;
        else if (kind == "endpoint")
            policy.endpoint = rest;
        else if (kind == "path")
            policy.path = rest;
        else if (kind == "allow")
            policy.allowed_kinds.push_back(rest);
        else if (kind == "deny")
            policy.denied_text.push_back(rest);
        else if (kind == "limit")
            policy.record_limit = static_cast<size_t>(std::strtoul(rest.c_str(), nullptr, 10));
        else if (kind == "message")
            policy.message += (policy.message.empty() ? "" : " ") + rest;
    }
    if (policy.allowed_kinds.empty())
        policy.error = "the policy names no record kinds that may be sent";
    return policy;
}

Contribution prepare_contribution(const std::string &database_path,
                                  const ContributionPolicy &policy, std::string &error)
{
    Contribution contribution;
    std::ifstream file(database_path);
    if (!file) {
        error = "nothing has been learned yet: " + database_path + " does not exist";
        return contribution;
    }

    const std::set<std::string> allowed(policy.allowed_kinds.begin(), policy.allowed_kinds.end());
    std::ostringstream body;
    std::set<std::string> seen;
    std::string line;

    while (std::getline(file, line)) {
        const std::string record = trim(line);
        if (record.empty() || record[0] == '#')
            continue;
        std::string kind, rest;
        split_first(record, kind, rest);

        if (allowed.count(kind) == 0) {
            ++contribution.withheld_kind;
            continue;
        }
        // A record that mentions a path or an address says something about a
        // person rather than about code, and never leaves the machine.
        bool personal = false;
        for (const std::string &denied : policy.denied_text) {
            if (record.find(denied) != std::string::npos) {
                personal = true;
                break;
            }
        }
        if (personal) {
            ++contribution.withheld_private;
            continue;
        }
        if (!seen.insert(record).second)
            continue;

        body << record << "\n";
        ++contribution.records;
        if (contribution.examples.size() < 5)
            contribution.examples.push_back(record);
        if (policy.record_limit != 0 && contribution.records >= policy.record_limit)
            break;
    }

    contribution.body = body.str();
    if (contribution.records == 0)
        error = "nothing in the database may be shared under this policy";
    return contribution;
}

namespace {

// Percent-encodes a string for use in a URL query.
std::string url_encode(const std::string &text)
{
    static const char *const hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xf]);
        }
    }
    return out;
}

std::string base64(const std::string &text)
{
    static const char *const alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((text.size() + 2) / 3 * 4);
    for (size_t i = 0; i < text.size(); i += 3) {
        const unsigned char a = static_cast<unsigned char>(text[i]);
        const unsigned char b = i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0;
        const unsigned char c = i + 2 < text.size() ? static_cast<unsigned char>(text[i + 2]) : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;
        out.push_back(alphabet[(triple >> 18) & 0x3f]);
        out.push_back(alphabet[(triple >> 12) & 0x3f]);
        out.push_back(i + 1 < text.size() ? alphabet[(triple >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < text.size() ? alphabet[triple & 0x3f] : '=');
    }
    return out;
}

std::string open_command()
{
#if defined(__APPLE__)
    return "open";
#elif defined(_WIN32)
    return "start \"\"";
#else
    return "xdg-open";
#endif
}

// A name for the contributed file that identifies the content and nothing else:
// no user name, no machine name, no date.
std::string contribution_filename(const Contribution &contribution)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    for (char c : contribution.body) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ull;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buffer) + ".astral";
}

std::string summary_of(const Contribution &contribution)
{
    std::ostringstream text;
    text << "A database submitted by `astral contribute database`.\n\n"
         << "- records: " << contribution.records << "\n"
         << "- withheld as not shareable: " << contribution.withheld_kind << "\n"
         << "- withheld as personal: " << contribution.withheld_private << "\n\n"
         << "Every file in this directory is compiled into the knowledge base, so\n"
         << "merging this is all it takes for the names in it to reach everyone.\n";
    return text.str();
}

std::string file_header(const Contribution &contribution)
{
    std::ostringstream text;
    text << "# A contributed database, " << contribution.records << " records.\n"
         << "# Every file in this directory is compiled into the knowledge base.\n\n";
    return text.str();
}

std::string curl_json(const std::string &method, const std::string &url,
                      const std::string &token, const std::string &body)
{
    std::string command = "curl -sS --max-time 120 -X " + method + " " + shell_quote(url) +
                          " -H 'Accept: application/vnd.github+json'"
                          " -H " + shell_quote("Authorization: Bearer " + token);
    if (!body.empty())
        command += " -d " + shell_quote(body);
    return capture(command + " 2>/dev/null");
}

// Opens the pull request, forking first when the person cannot write to the
// repository directly. Everything here needs a token; without one the browser
// route below does the same job by hand.
bool open_pull_request(const std::string &repo, const ContributionPolicy &policy,
                       const Contribution &contribution, const std::string &title,
                       const std::string &token, Submission &out, std::string &error)
{
    const std::string me = json_field(curl_json("GET", "https://api.github.com/user", token, ""),
                                      "login");
    if (me.empty()) {
        error = "the token was not accepted by GitHub";
        return false;
    }

    // Writing straight to the repository is simpler, so try that first.
    const std::string about = curl_json("GET", "https://api.github.com/repos/" + repo, token, "");
    const bool can_write = about.find("\"push\": true") != std::string::npos ||
                           about.find("\"push\":true") != std::string::npos;
    std::string working = repo;
    if (!can_write) {
        curl_json("POST", "https://api.github.com/repos/" + repo + "/forks", token, "");
        working = me + "/" + repo.substr(repo.find('/') + 1);
        // A fork is created asynchronously; give it a moment to exist.
        for (int attempt = 0; attempt < 10; ++attempt) {
            const std::string check =
                curl_json("GET", "https://api.github.com/repos/" + working, token, "");
            if (json_field(check, "full_name") == working)
                break;
            capture("sleep 2");
        }
    }

    const std::string base_branch =
        json_field(curl_json("GET", "https://api.github.com/repos/" + repo, token, ""),
                   "default_branch");
    const std::string base = base_branch.empty() ? "main" : base_branch;
    const std::string head_sha = json_field(
        curl_json("GET", "https://api.github.com/repos/" + working + "/git/ref/heads/" + base,
                  token, ""),
        "sha");
    if (head_sha.empty()) {
        error = "could not find the branch to build on";
        return false;
    }

    const std::string filename = contribution_filename(contribution);
    const std::string branch = "database-" + filename.substr(0, 8);
    std::ostringstream ref;
    ref << "{\"ref\":\"refs/heads/" << branch << "\",\"sha\":" << json_string(head_sha) << "}";
    curl_json("POST", "https://api.github.com/repos/" + working + "/git/refs", token, ref.str());

    const std::string content = base64(file_header(contribution) + contribution.body);
    std::ostringstream put;
    put << "{\"message\":" << json_string(title) << ",\"content\":" << json_string(content)
        << ",\"branch\":" << json_string(branch) << "}";
    const std::string written =
        curl_json("PUT",
                  "https://api.github.com/repos/" + working + "/contents/" + policy.path + "/" +
                      filename,
                  token, put.str());
    if (json_field(written, "sha").empty() && written.find("\"content\"") == std::string::npos) {
        const std::string message = json_field(written, "message");
        error = "could not add the file" + (message.empty() ? "" : ": " + message);
        return false;
    }

    std::ostringstream pull;
    pull << "{\"title\":" << json_string(title) << ",\"head\":"
         << json_string(working == repo ? branch : me + ":" + branch) << ",\"base\":"
         << json_string(base) << ",\"body\":" << json_string(summary_of(contribution)) << "}";
    const std::string response =
        curl_json("POST", "https://api.github.com/repos/" + repo + "/pulls", token, pull.str());
    const std::string url = json_field(response, "html_url");
    if (url.empty()) {
        const std::string message = json_field(response, "message");
        error = "could not open the pull request" + (message.empty() ? "" : ": " + message);
        return false;
    }
    out.delivery = Delivery::Api;
    out.url = url;
    return true;
}

} // namespace

bool send_contribution(const std::string &repo, const ContributionPolicy &policy,
                       const Contribution &contribution, const std::string &title,
                       Submission &out, std::string &error)
{
    // A project running a submission service takes anything from anyone, which
    // is the only route needing neither an account nor a browser.
    if (!policy.endpoint.empty()) {
        capture("curl -sS --fail --max-time 120 -X POST " + shell_quote(policy.endpoint) +
                " -H 'Content-Type: text/plain' -H " +
                shell_quote("X-Astral-Records: " + std::to_string(contribution.records)) +
                " --data-binary @- <<'ASTRAL_SUBMISSION_END'\n" + contribution.body +
                "ASTRAL_SUBMISSION_END\n 2>/dev/null");
        out.delivery = Delivery::Endpoint;
        out.url = policy.endpoint;
        return true;
    }

    const std::string token = github_token();
    if (!token.empty()) {
        std::string pull_error;
        if (open_pull_request(repo, policy, contribution, title, token, out, pull_error))
            return true;
        // A token that cannot open a pull request is common enough; the browser
        // can always finish the job, so that is tried rather than failing.
    }

    // No account needed here either. The whole database is written out, and the
    // browser is sent to the page that uploads a file to the repository. GitHub
    // forks and opens the pull request itself for anyone without write access.
    const char *home = std::getenv("HOME");
    out.file = (home != nullptr ? std::string(home) : std::string(".")) + "/" +
               contribution_filename(contribution);
    std::ofstream file(out.file);
    if (!file) {
        error = "cannot write " + out.file;
        return false;
    }
    file << file_header(contribution) << contribution.body;
    file.close();

    out.url = "https://github.com/" + repo + "/upload/main/" + url_encode(policy.path);
    out.delivery = Delivery::Browser;
    capture(open_command() + " " + shell_quote(out.url) + " >/dev/null 2>&1");
    return true;
}

} // namespace astral_internal
