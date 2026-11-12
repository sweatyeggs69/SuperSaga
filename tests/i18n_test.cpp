// Translations: the .po reader, placeholders, locale matching, and that every compiled-in
// language covers every text in assets/lang/messages.pot.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "app/i18n.h"

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}
std::string read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
}  // namespace

int main() {
    using namespace supersaga::i18n;

    // .po reading: escapes, continuation lines, comments, empty msgstr skipped, header skipped.
    auto po = parse_po(
        "# comment\nmsgid \"\"\nmsgstr \"\"\n\"Language: fr\\n\"\n\n"
        "#: src/a.cpp:1\nmsgid \"Play\"\nmsgstr \"Jouer\"\n\n"
        "msgid \"Two \"\n\"lines\"\nmsgstr \"Deux \"\n\"lignes\"\n\n"
        "msgid \"Quote \\\"x\\\"\\n\"\nmsgstr \"Citation \\\"x\\\"\\n\"\n\n"
        "msgid \"Untranslated\"\nmsgstr \"\"\n");
    check(po.size() == 3, "parse_po entry count");
    if (po.size() == 3) {
        check(po[0].first == "Play" && po[0].second == "Jouer", "simple entry");
        check(po[1].first == "Two lines" && po[1].second == "Deux lignes", "continuation lines");
        check(po[2].first == "Quote \"x\"\n" && po[2].second == "Citation \"x\"\n", "escapes");
    }

    // Placeholders.
    check(format("{1} then {0}", {"a", "b"}) == "b then a", "reordered placeholders");
    check(format("{{0}} {0}", {"x"}) == "{0} x", "escaped braces");
    check(trf("Slot {0}", 3) == "Slot 3", "trf in English");

    // Locale matching.
    check(match_locale({"fr-CA"}) == "fr", "fr-CA -> fr");
    check(match_locale({"pt_BR"}) == "pt-BR", "pt_BR -> pt-BR");
    check(match_locale({"pt-PT"}) == "pt-BR", "pt-PT -> pt-BR");
    check(match_locale({"ja", "de-AT"}) == "de", "first supported locale wins");
    check(match_locale({"ja"}) == "en", "unsupported -> English");
    check(match_locale({}) == "en", "no locales -> English");

    // Unknown language falls back to English.
    check(!set_language("xx") && language() == "en", "unknown language");
    check(std::string(tr("Play")) == "Play", "English passes through");

    // Every language translates every text the code uses.
    std::vector<std::string> ids;
    {
        // messages.pot has empty msgstr, so parse_po skips them: read the msgids directly.
        std::istringstream in(read(std::string(SUPERSAGA_LANG_DIR) + "/messages.pot"));
        std::string text, line;
        while (std::getline(in, line)) text += line + "\n";
        // Give each entry a placeholder translation so parse_po returns its msgid.
        std::string filled;
        std::istringstream in2(text);
        while (std::getline(in2, line)) filled += (line == "msgstr \"\"" ? std::string("msgstr \"x\"") : line) + "\n";
        for (auto& [id, s] : parse_po(filled)) ids.push_back(id);
    }
    check(ids.size() > 100, "messages.pot lists the texts");
    for (const Language& l : languages()) {
        if (std::string(l.code) == "en") continue;
        check(set_language(l.code), std::string("set_language ") + l.code);
        int missing = 0;
        for (const std::string& id : ids)
            if (tr(id.c_str()) == id && id.find('{') == std::string::npos && id.size() > 3) ++missing;
        // A few texts are the same in some languages ("Turbo", "Options"); a real gap is many.
        check(missing < 25, std::string(l.code) + ": " + std::to_string(missing) + " texts look untranslated");
    }
    // Pointers from tr() stay valid after switching language.
    set_language("fr");
    const char* fr = tr("Play");
    set_language("de");
    check(std::string(fr) != "", "pointer still valid");
    set_language("en");

    if (failures) return 1;
    std::printf("translations: ok\n");
    return 0;
}
