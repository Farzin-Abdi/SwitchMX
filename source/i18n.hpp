// Interface languages. Text is written in English in the code; tr() gives it in the chosen
// language. Text with numbers is matched by pattern ("3 items" is found as "{0} items"), and text
// with names in it is put together with trf().
#pragma once
#include <string>
#include <vector>

namespace i18n {

struct Language {
    const char* code;  // "fa"
    const char* name;  // in its own language: "فارسی"
    bool rtl;          // written right to left: the interface is mirrored
};
const std::vector<Language>& languages();
void setLanguage(int index);
int language();
bool rtl();

std::string tr(const std::string& english);
// Digits as written in the interface language (Persian and Arabic have their own).
std::string localDigits(const std::string& s);
// tr() of a pattern, then {0}, {1}, ... replaced by the arguments.
std::string trf(const std::string& pattern, const std::vector<std::string>& args);

}  // namespace i18n

using i18n::tr;
using i18n::trf;
