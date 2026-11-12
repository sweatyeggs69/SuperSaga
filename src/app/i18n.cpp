#include "app/i18n.h"

#include <cctype>
#include <memory>
#include <unordered_map>

// The .po files from assets/lang, compiled in (see CMakeLists.txt).
extern const unsigned char supersaga_lang_fr[];
extern const unsigned supersaga_lang_fr_size;
extern const unsigned char supersaga_lang_de[];
extern const unsigned supersaga_lang_de_size;
extern const unsigned char supersaga_lang_es[];
extern const unsigned supersaga_lang_es_size;
extern const unsigned char supersaga_lang_it[];
extern const unsigned supersaga_lang_it_size;
extern const unsigned char supersaga_lang_pt_br[];
extern const unsigned supersaga_lang_pt_br_size;

namespace supersaga::i18n {

namespace {

struct Entry {
    Language lang;
    const unsigned char* data;
    const unsigned* size;
};

const std::vector<Entry>& entries() {
    static const std::vector<Entry> e = {
        {{"en", "English"}, nullptr, nullptr},
        {{"fr", "Français"}, supersaga_lang_fr, &supersaga_lang_fr_size},
        {{"de", "Deutsch"}, supersaga_lang_de, &supersaga_lang_de_size},
        {{"es", "Español"}, supersaga_lang_es, &supersaga_lang_es_size},
        {{"it", "Italiano"}, supersaga_lang_it, &supersaga_lang_it_size},
        {{"pt-BR", "Português (Brasil)"}, supersaga_lang_pt_br, &supersaga_lang_pt_br_size},
    };
    return e;
}

using Table = std::unordered_map<std::string, std::string>;

// Every table stays loaded once used, so pointers handed out by tr() never dangle, even after
// the language changes.
std::unordered_map<std::string, std::unique_ptr<Table>>& tables() {
    static std::unordered_map<std::string, std::unique_ptr<Table>> t;
    return t;
}

std::string g_code = "en";
const Table* g_table = nullptr;
const char* g_name = "English";

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& ch : r) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return r;
}

// Reads one quoted .po string starting at s[i] == '"'; handles \n, \t, \", \\.
bool read_quoted(std::string_view s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    while (i < s.size() && s[i] != '"') {
        char ch = s[i++];
        if (ch == '\\' && i < s.size()) {
            char e = s[i++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                default: out += e; break;
            }
        } else {
            out += ch;
        }
    }
    if (i >= s.size()) return false;
    ++i;  // closing quote
    return true;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> parse_po(std::string_view text) {
    std::vector<std::pair<std::string, std::string>> out;
    enum class Field { None, Id, Str, Other } field = Field::None;
    std::string id, str;
    bool have_id = false;
    auto flush = [&] {
        if (have_id && !id.empty() && !str.empty()) out.emplace_back(id, str);
        id.clear();
        str.clear();
        have_id = false;
    };
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string_view::npos || line[b] == '#') {
            if (b == std::string_view::npos && field == Field::Str) {
                flush();
                field = Field::None;
            }
            continue;
        }
        line.remove_prefix(b);
        std::string* target = nullptr;
        if (line.rfind("msgid ", 0) == 0) {
            if (have_id) flush();
            field = Field::Id;
            have_id = true;
            line.remove_prefix(6);
            target = &id;
        } else if (line.rfind("msgstr ", 0) == 0) {
            field = Field::Str;
            line.remove_prefix(7);
            target = &str;
        } else if (line[0] == '"') {
            target = field == Field::Id ? &id : field == Field::Str ? &str : nullptr;
        } else {
            field = Field::Other;  // msgctxt, msgid_plural, ...: not used
            continue;
        }
        if (!target) continue;
        size_t i = line.find('"');
        if (i != std::string_view::npos) read_quoted(line, i, *target);
    }
    flush();
    return out;
}

const std::vector<Language>& languages() {
    static const std::vector<Language> l = [] {
        std::vector<Language> v;
        for (const auto& e : entries()) v.push_back(e.lang);
        return v;
    }();
    return l;
}

bool set_language(std::string_view code) {
    for (const auto& e : entries()) {
        if (lower(code) != lower(e.lang.code)) continue;
        g_code = e.lang.code;
        g_name = e.lang.name;
        if (!e.data) {
            g_table = nullptr;
            return true;
        }
        auto& slot = tables()[g_code];
        if (!slot) {
            slot = std::make_unique<Table>();
            for (auto& [k, v] : parse_po({reinterpret_cast<const char*>(e.data), *e.size}))
                slot->emplace(std::move(k), std::move(v));
        }
        g_table = slot.get();
        return true;
    }
    g_code = "en";
    g_name = "English";
    g_table = nullptr;
    return false;
}

const std::string& language() { return g_code; }
const char* language_name() { return g_name; }

std::string match_locale(const std::vector<std::string>& preferred) {
    for (const std::string& raw : preferred) {
        std::string loc = lower(raw);
        for (char& ch : loc)
            if (ch == '_') ch = '-';
        if (size_t dot = loc.find_first_of(".@"); dot != std::string::npos) loc.resize(dot);
        const std::string base = loc.substr(0, loc.find('-'));
        if (base.empty()) continue;
        // An exact match first ("pt-br"), then the language alone ("fr-ca" -> "fr", "pt" -> "pt-BR").
        for (const auto& e : entries())
            if (lower(e.lang.code) == loc) return e.lang.code;
        for (const auto& e : entries()) {
            const std::string code = lower(e.lang.code);
            if (code.substr(0, code.find('-')) == base) return e.lang.code;
        }
    }
    return "en";
}

const char* tr(const char* english) {
    if (!g_table || !english || !*english) return english;
    auto it = g_table->find(english);
    return it == g_table->end() ? english : it->second.c_str();
}

std::string format(std::string_view text, const std::vector<std::string>& args) {
    std::string out;
    out.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '{' && i + 1 < text.size() && text[i + 1] == '{') {
            out += '{';
            ++i;
        } else if (ch == '}' && i + 1 < text.size() && text[i + 1] == '}') {
            out += '}';
            ++i;
        } else if (ch == '{') {
            size_t close = text.find('}', i);
            std::string_view num = close == std::string_view::npos ? std::string_view() : text.substr(i + 1, close - i - 1);
            bool digits = !num.empty() && num.size() < 3;
            for (char d : num) digits = digits && std::isdigit(static_cast<unsigned char>(d));
            if (digits) {
                size_t n = size_t(std::stoi(std::string(num)));
                if (n < args.size()) out += args[n];
                i = close;
            } else {
                out += ch;
            }
        } else {
            out += ch;
        }
    }
    return out;
}

}  // namespace supersaga::i18n
