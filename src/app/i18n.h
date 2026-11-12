#pragma once
// Translations of the launcher and menu text, shared by every GBA port (SuperSaga, MetroFuse,
// SuperSaga, ...). The English text in the code is the key: tr("Settings") returns the text for
// the chosen language, or the English text when a language has no entry for it. Each language is
// a gettext .po file in assets/lang, compiled into the program. No SDL or ImGui dependency.
//
// Text with values in it uses numbered placeholders, so a language can put them in its own
// order: trf("Saved to slot {0}", slot). tools/i18n.py lists the texts the code uses and checks
// every .po file against them.

#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Marks text for translation where it is defined (tables, arrays) and is passed through tr()
// where it is shown.
#define N_(text) text

namespace supersaga::i18n {

struct Language {
    const char* code;  // "en", "fr", "pt-BR", ...
    const char* name;  // the language's own name: "English", "Français", ...
};
// English first, then the translations in the order the language menu shows them.
const std::vector<Language>& languages();

// Switches the text language. Unknown codes fall back to English (and return false).
bool set_language(std::string_view code);
const std::string& language();
const char* language_name();

// The language to use when the player has not picked one: the first of the system's preferred
// locales ("fr_CA", "pt-BR", "de", ...) that has a translation, else English.
std::string match_locale(const std::vector<std::string>& preferred);

// The text in the current language. The returned pointer stays valid for the whole run.
const char* tr(const char* english);

// tr() with {0}, {1}, ... replaced by the arguments. "{{" writes a literal brace.
std::string format(std::string_view text, const std::vector<std::string>& args);
inline std::string arg(const std::string& s) { return s; }
inline std::string arg(std::string_view s) { return std::string(s); }
inline std::string arg(const char* s) { return s ? s : ""; }
template <class T>
std::string arg(T v) requires std::is_arithmetic_v<T> { return std::to_string(v); }
template <class... A>
std::string trf(const char* english, const A&... args) {
    return format(tr(english), {arg(args)...});
}

// Parses .po text into (msgid, msgstr) pairs; entries with an empty msgstr are left out.
// Exposed for tests and tools.
std::vector<std::pair<std::string, std::string>> parse_po(std::string_view text);

}  // namespace supersaga::i18n
