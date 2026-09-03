#ifndef ASTRAL_CONTRIBUTE_HH
#define ASTRAL_CONTRIBUTE_HH

#include <string>
#include <vector>

namespace astral_internal {

// What a repository will accept, read from its published policy.
struct ContributionPolicy {
    bool accepted = false;
    std::string method = "pull-request";
    // Where contributed databases live in the repository.
    std::string path = "contrib/databases";
    // A service that takes submissions from anyone, with no account. Empty
    // when the project is not running one.
    std::string endpoint;
    std::vector<std::string> allowed_kinds;
    std::vector<std::string> denied_text;
    size_t record_limit = 0;
    std::string message;
    std::string error; // set when the policy could not be read
};

// The result of preparing a submission: what would be sent, and what was held
// back and why.
struct Contribution {
    std::string body;             // the records, ready to send
    size_t records = 0;           // how many are in it
    size_t withheld_kind = 0;     // dropped because the kind is not accepted
    size_t withheld_private = 0;  // dropped because they said something personal
    std::vector<std::string> examples; // a few of the records, to show the user
};

// Asks the repository what it accepts. `repo` is "owner/name".
ContributionPolicy fetch_policy(const std::string &repo, const std::string &branch = "main");

// Selects the records that may be sent, dropping everything else. Reads the
// user's learned database.
Contribution prepare_contribution(const std::string &database_path,
                                  const ContributionPolicy &policy, std::string &error);

// How a submission was delivered, so the caller can say something useful.
enum class Delivery {
    Endpoint,  // posted to a service that needs no account
    Api,       // a pull request opened with a token the machine already had
    Browser,   // written out, and the browser carries it the rest of the way
};

struct Submission {
    Delivery delivery = Delivery::Browser;
    std::string url;   // what was opened, or where to go
    std::string file;  // the records, when the browser has to carry them
};

// Sends it, choosing whatever route is open on this machine. A token is never
// required: without one the records are written out and a prefilled issue is
// opened in the browser, where the person is already signed in.
bool send_contribution(const std::string &repo, const ContributionPolicy &policy,
                       const Contribution &contribution, const std::string &title,
                       Submission &out, std::string &error);

// The token that will be used, or an empty string when none can be found.
std::string github_token();

} // namespace astral_internal

#endif
