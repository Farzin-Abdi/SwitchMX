#include "i18n.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "lang_tables.hpp"

namespace i18n {
namespace {
int g_lang = 0;
std::unordered_map<std::string, std::string> g_table;  // English -> chosen language
std::unordered_map<std::string, std::string> g_cache;  // every text asked for, as shown

// "3 of 12 items" -> "{0} of {1} items", the numbers kept in order.
bool pattern(const std::string& s, std::string& key, std::vector<std::string>& nums) {
    bool any = false;
    for (size_t i = 0; i < s.size();) {
        if (isdigit((unsigned char)s[i])) {
            size_t j = i;
            while (j < s.size() && (isdigit((unsigned char)s[j]) ||
                                    ((s[j] == '.' || s[j] == ',' || s[j] == ':') && j + 1 < s.size() &&
                                     isdigit((unsigned char)s[j + 1]))))
                j++;
            key += "{" + std::to_string(nums.size()) + "}";
            nums.push_back(s.substr(i, j - i));
            i = j;
            any = true;
        } else {
            key += s[i++];
        }
    }
    return any;
}

std::string fill(std::string out, const std::vector<std::string>& args) {
    for (size_t k = 0; k < args.size(); k++) {
        std::string tag = "{" + std::to_string(k) + "}";
        for (size_t p; (p = out.find(tag)) != std::string::npos;) out.replace(p, tag.size(), args[k]);
    }
    return out;
}
}  // namespace

const std::vector<Language>& languages() {
    static const std::vector<Language> v = {
        {"en", "English", false},  {"fa", "فارسی", true},    {"ar", "العربية", true},  {"es", "Español", false},
        {"fr", "Français", false}, {"de", "Deutsch", false}, {"it", "Italiano", false}, {"pt", "Português", false},
        {"ru", "Русский", false},  {"tr", "Türkçe", false},  {"zh", "简体中文", false}, {"ja", "日本語", false},
        {"ko", "한국어", false},
    };
    return v;
}

void setLanguage(int index) {
    if (index < 0 || index >= (int)languages().size()) index = 0;
    if (index == g_lang && !(index && g_table.empty())) return;
    g_lang = index;
    g_table.clear();
    g_cache.clear();
    if (!index) return;
    size_t n = 0;
    const char* const(*rows)[2] = lang::table(languages()[index].code, n);
    g_table.reserve(n);
    for (size_t i = 0; i < n; i++) g_table.emplace(rows[i][0], rows[i][1]);
}

int language() { return g_lang; }
bool rtl() { return languages()[g_lang].rtl; }

std::string tr(const std::string& s) {
    if (!g_lang || s.empty()) return s;
    auto c = g_cache.find(s);
    if (c != g_cache.end()) return c->second;
    std::string out = s;
    auto it = g_table.find(s);
    if (it != g_table.end()) {
        out = it->second;
    } else {
        std::string key;
        std::vector<std::string> nums;
        if (pattern(s, key, nums)) {
            auto p = g_table.find(key);
            if (p != g_table.end()) {
                for (auto& n : nums) n = localDigits(n);  // Persian and Arabic digits
                out = fill(p->second, nums);
            }
        }
    }
#ifndef __SWITCH__
    // Dev aid: SWITCHMX_TRMISS=<file> lists interface text that has no translation.
    static const char* missFile = getenv("SWITCHMX_TRMISS");
    if (missFile && out == s) {
        bool letters = false;
        for (size_t i = 0; i + 1 < s.size(); i++) letters |= isalpha((unsigned char)s[i]) && isalpha((unsigned char)s[i + 1]);
        if (letters)
            if (FILE* f = fopen(missFile, "a")) {
                fprintf(f, "%s\n", s.c_str());
                fclose(f);
            }
    }
#endif
    if (g_cache.size() > 20000) g_cache.clear();
    g_cache.emplace(s, out);
    return out;
}

std::string localDigits(const std::string& s) {
    const char* code = languages()[g_lang].code;
    static const char* const fa[] = {"۰", "۱", "۲", "۳", "۴", "۵", "۶", "۷", "۸", "۹"};
    static const char* const ar[] = {"٠", "١", "٢", "٣", "٤", "٥", "٦", "٧", "٨", "٩"};
    const char* const* digits = !strcmp(code, "fa") ? fa : (!strcmp(code, "ar") ? ar : nullptr);
    if (!digits) return s;
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        char ch = s[i];
        if (isdigit((unsigned char)ch)) out += digits[ch - '0'];
        else if ((ch == '.' || ch == ',') && i > 0 && i + 1 < s.size() && isdigit((unsigned char)s[i - 1]) &&
                 isdigit((unsigned char)s[i + 1]))
            out += "٫";  // the decimal separator
        else if (ch == '%') out += "٪";
        else out += ch;
    }
    return out;
}

std::string trf(const std::string& pattern, const std::vector<std::string>& args) {
    std::string t = pattern;
    if (g_lang) {
        auto it = g_table.find(pattern);
        if (it != g_table.end()) t = it->second;
    }
    return fill(t, args);
}

}  // namespace i18n
