#ifndef ASTRAL_KNOWLEDGE_HH
#define ASTRAL_KNOWLEDGE_HH

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace astral_internal {

// The text of data/knowledge.astral, embedded at build time so a working
// library never depends on finding a file.
extern const char *const SEED_KNOWLEDGE_TEXT;

// What a program still carries about its own intent, and what that evidence
// means. The seed ships with the library; a user database sits alongside it and
// wins where the two disagree, which is how a rename becomes permanent.
class Knowledge {
public:
    // A function name suggested by the calls a body makes.
    struct Idiom {
        std::string first;
        std::string second;
        std::string name;
    };

    // A function name suggested by the text a body contains.
    struct Literal {
        std::vector<std::string> words;
        std::string name;
    };

    // Loads the embedded seed, then the user database if it exists.
    static Knowledge &instance();

    // Replaces everything and reloads. `user_path` empty means the default,
    // ~/.astral/learned.astral.
    void reload(const std::string &user_path = std::string());

    const std::string &user_path() const { return user_path_; }

    // Naming evidence.
    std::string verb_for(const std::string &callee) const;
    const std::vector<Idiom> &idioms() const { return idioms_; }
    const std::vector<Literal> &literals() const { return literals_; }
    std::string role_name(const std::string &shape) const;
    bool is_stop_word(const std::string &word) const;
    bool is_placeholder(const std::string &name) const;

    // Library knowledge.
    std::string prototype_for(const std::string &name) const;
    std::string header_for(const std::string &name) const;
    const std::map<std::string, std::string> &prototypes() const { return protos_; }

    // A comment to attach where `text` appears in a function body.
    std::vector<std::string> notes_for(const std::string &text) const;

    // A name learned for a function body with this fingerprint.
    std::string signature_name(uint64_t hash, uint32_t length) const;

    // Every body length the database holds. A function's real extent is rarely
    // known exactly, so a match is looked for at each of these instead.
    const std::set<uint32_t> &signature_lengths() const { return lengths_; }

    // Records a name the user chose, so the same body is recognised next time.
    // Appends to the user database and takes effect immediately.
    bool learn_signature(uint64_t hash, uint32_t length, const std::string &name,
                         std::string &error);
    // Records a rename that carries no fingerprint, as a plain note of intent.
    bool learn_name(const std::string &from, const std::string &to, std::string &error);
    // Records a function's real prototype, read from the source that built it.
    bool learn_prototype(const std::string &name, const std::string &declaration,
                         std::string &error);

    // Removes every user record naming `name`. Returns how many went.
    int forget(const std::string &name, std::string &error);
    // Empties the user database, leaving the built-in knowledge alone.
    bool forget_all(std::string &error);

    // Counts, for reporting.
    size_t size() const;
    size_t learned_count() const { return learned_; }

private:
    Knowledge() = default;
    void parse(const std::string &text, bool from_user);
    bool append_user_record(const std::string &line, std::string &error);

    std::map<std::string, std::string> verbs_;
    std::vector<Idiom> idioms_;
    std::vector<Literal> literals_;
    std::map<std::string, std::string> roles_;
    std::set<std::string> stop_words_;
    std::vector<std::string> placeholders_;
    std::map<std::string, std::string> protos_;
    std::map<std::string, std::string> headers_;
    std::vector<std::pair<std::string, std::string>> notes_;
    std::map<std::pair<uint64_t, uint32_t>, std::string> signatures_;
    std::set<uint32_t> lengths_;
    std::string user_path_;
    size_t learned_ = 0;
    bool loaded_ = false;
};

// The fingerprint of a function body: its bytes with position-dependent
// immediates masked out, so the same code compiled at a different address still
// matches. Returns false when the body is too short to identify.
bool fingerprint(const uint8_t *bytes, size_t size, const std::string &processor,
                 uint64_t &hash);

// Fingerprints every prefix of `bytes` whose length the database holds, calling
// `report(length, hash)` for each. The hash is built once and extended, so this
// costs one pass regardless of how many lengths are on file.
void fingerprint_prefixes(const uint8_t *bytes, size_t size, const std::string &processor,
                          const std::set<uint32_t> &lengths,
                          const std::function<void(uint32_t, uint64_t)> &report);

} // namespace astral_internal

#endif
