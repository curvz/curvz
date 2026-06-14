#include "Hyphenator.hpp"
#include "CurvzLog.hpp"

#ifdef CURVZ_HAVE_HYPHEN
#include <hyphen.h>   // libhyphen (Hunspell hyphenation). Header dir is added by
                      // CMake (HYPHEN_INCLUDE_DIR); it may live at hyphen.h or
                      // hyphen/hyphen.h depending on the distro.
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>

namespace fs = std::filesystem;
#endif

namespace Curvz {

#ifdef CURVZ_HAVE_HYPHEN
namespace {

// Optional override directory (set_hyphen_dict_dir). Consulted first.
std::string g_dict_dir;

// language -> loaded dict, owned for process lifetime. A separate failure set
// remembers languages whose dict couldn't be found/loaded, so a missing dict
// isn't re-probed on every word.
struct DictDeleter {
    void operator()(HyphenDict* d) const { if (d) hnj_hyphen_free(d); }
};
std::map<std::string, std::unique_ptr<HyphenDict, DictDeleter>> g_dicts;
std::map<std::string, bool> g_load_failed;

// Candidate paths for hyph_<lang>.dic, priority order: explicit override,
// then Curvz-bundled data dirs (so a shipped dict wins over a system one),
// then the standard system locations and the Flatpak sandbox share.
std::vector<std::string> dict_candidates(const std::string& lang) {
    std::vector<std::string> out;
    const std::string file = "hyph_" + lang + ".dic";
    if (!g_dict_dir.empty())
        out.push_back(g_dict_dir + "/" + file);
    if (const char* xdg = std::getenv("XDG_DATA_HOME"))
        out.push_back(std::string(xdg) + "/curvz/hyphen/" + file);
    if (const char* home = std::getenv("HOME"))
        out.push_back(std::string(home) + "/.local/share/curvz/hyphen/" + file);
    out.push_back("/usr/share/curvz/hyphen/" + file);
    out.push_back("/usr/share/hyphen/" + file);    // Fedora hyphen-en, Debian
    out.push_back("/usr/share/myspell/" + file);   // older LibreOffice layout
    out.push_back("/app/share/hyphen/" + file);     // Flatpak sandbox
    return out;
}

// Resolve + load (once) the dict for `lang`. Returns nullptr if unavailable;
// the nullptr case is cached in g_load_failed so we don't re-probe the disk.
HyphenDict* get_dict(const std::string& lang) {
    auto it = g_dicts.find(lang);
    if (it != g_dicts.end())
        return it->second.get();
    if (g_load_failed.count(lang))
        return nullptr;

    for (const auto& path : dict_candidates(lang)) {
        std::error_code ec;
        if (!fs::exists(path, ec))
            continue;
        HyphenDict* d = hnj_hyphen_load(path.c_str());
        if (d) {
            LOG_INFO("Hyphenator: loaded '{}' from {}", lang, path);
            g_dicts.emplace(lang,
                            std::unique_ptr<HyphenDict, DictDeleter>(d));
            return d;
        }
        LOG_WARN("Hyphenator: hnj_hyphen_load failed for {}", path);
    }
    LOG_INFO("Hyphenator: no dictionary for '{}' -- automatic hyphenation off "
             "for this language (plain wrapping used)", lang);
    g_load_failed[lang] = true;
    return nullptr;
}

// Trim leading/trailing non-letters so "(modish),\u201d" hyphenates on its core
// "modish". Fills [lo, hi) byte window of the core; false when no letters.
bool letter_core(const std::string& w, size_t& lo, size_t& hi) {
    lo = 0;
    hi = w.size();
    while (lo < hi && !std::isalpha((unsigned char)w[lo]))
        ++lo;
    while (hi > lo && !std::isalpha((unsigned char)w[hi - 1]))
        --hi;
    return hi > lo;
}

}  // namespace
#endif  // CURVZ_HAVE_HYPHEN

void set_hyphen_dict_dir(const std::string& dir) {
#ifdef CURVZ_HAVE_HYPHEN
    g_dict_dir = dir;
#else
    (void)dir;
#endif
}

std::vector<size_t> hyphenate(const std::string& word, const std::string& lang) {
    std::vector<size_t> breaks;
#ifdef CURVZ_HAVE_HYPHEN
    if (word.size() < 4)
        return breaks;  // nothing useful to break below ~4 chars

    HyphenDict* dict = get_dict(lang);
    if (!dict)
        return breaks;

    // Hyphenate the letter core only; punctuation around it never breaks.
    size_t lo = 0, hi = 0;
    if (!letter_core(word, lo, hi))
        return breaks;
    const std::string core = word.substr(lo, hi - lo);
    const int n = (int)core.size();
    if (n < 4)
        return breaks;

    // libhyphen wants a lowercased word in the dict's encoding (en_US is
    // ASCII/UTF-8). Lowercasing ASCII preserves byte count, so break positions
    // map straight back to the original (possibly mixed-case) core by offset.
    std::string lower = core;
    for (auto& c : lower)
        c = (char)std::tolower((unsigned char)c);

    // Buffers per the libhyphen contract: `hyphens` is word_size+5; `hyphword`
    // (the '='-marked result, which we don't use) is generously word_size*2+1.
    std::vector<char> hyphens(n + 5, 0);
    std::vector<char> hyphword(n * 2 + 1, 0);
    char** rep = nullptr;
    int*   pos = nullptr;
    int*   cut = nullptr;

    // Non-zero return == error. rep/pos/cut carry non-standard hyphenation
    // (e.g. German "ck"->"k-k"); en_US has none, but we free defensively.
    int rc = hnj_hyphen_hyphenate2(dict, lower.c_str(), n,
                                   hyphens.data(), hyphword.data(),
                                   &rep, &pos, &cut);

    if (rc == 0) {
        // hyphens[i] holds a digit char for the gap after core char i; an ODD
        // value means a break is legal there. Skip i == n-1 (no break after the
        // final char). For an ASCII core, char index == byte index, so the
        // original-word byte offset of the break is lo + i + 1.
        //
        // NOTE (multibyte): for non-ASCII dictionaries libhyphen indexes by
        // character, not byte; en_US is ASCII so this is exact. A char->byte
        // remap is the follow-up when a non-English dict is added.
        for (int i = 0; i < n - 1; ++i) {
            if (((hyphens[i] - '0') % 2) == 1)
                breaks.push_back(lo + (size_t)i + 1);
        }
    }

    // Free anything hyphenate2 allocated for non-standard reps.
    if (rep || pos || cut) {
        for (int i = 0; i < n; ++i)
            if (rep && rep[i])
                free(rep[i]);
        free(rep);
        free(pos);
        free(cut);
    }
#else
    (void)word;
    (void)lang;
#endif  // CURVZ_HAVE_HYPHEN
    return breaks;
}

}  // namespace Curvz
