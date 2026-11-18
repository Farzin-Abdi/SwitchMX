#include "lang_tables.hpp"

#include <cstring>

// Each lang_xx.cpp defines these two.
#define LANG(x)                                     \
    namespace lang {                                \
    extern const char* const k_##x[][2];            \
    extern const size_t k_##x##_count;              \
    }
LANG(fa) LANG(ar) LANG(es) LANG(fr) LANG(de) LANG(it) LANG(pt) LANG(ru) LANG(tr) LANG(zh) LANG(ja) LANG(ko)
#undef LANG

namespace lang {
const char* const (*table(const char* code, size_t& n))[2] {
    struct Entry {
        const char* code;
        const char* const (*rows)[2];
        const size_t* count;
    };
    static const Entry all[] = {{"fa", k_fa, &k_fa_count}, {"ar", k_ar, &k_ar_count}, {"es", k_es, &k_es_count},
                                {"fr", k_fr, &k_fr_count}, {"de", k_de, &k_de_count}, {"it", k_it, &k_it_count},
                                {"pt", k_pt, &k_pt_count}, {"ru", k_ru, &k_ru_count}, {"tr", k_tr, &k_tr_count},
                                {"zh", k_zh, &k_zh_count}, {"ja", k_ja, &k_ja_count}, {"ko", k_ko, &k_ko_count}};
    for (auto& e : all)
        if (!strcmp(e.code, code)) {
            n = *e.count;
            return e.rows;
        }
    n = 0;
    return nullptr;
}
}  // namespace lang
