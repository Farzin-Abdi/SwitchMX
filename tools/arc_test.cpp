// Command-line harness for the archive engine: arc_test x|l|c ...
#include <cstdio>
#include <cstring>

#include "../source/archiver.hpp"

static const char* name(ArcResult r) {
    switch (r) {
        case ArcResult::Ok: return "OK";
        case ArcResult::NeedPassword: return "NEEDPW";
        case ArcResult::WrongPassword: return "WRONGPW";
        case ArcResult::Cancelled: return "CANCELLED";
        default: return "ERROR";
    }
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::string err;
    ArcProgress prog;
    ArcResult r = ArcResult::Error;
    if (!strcmp(argv[1], "x") && argc >= 4) {  // x archive dest [password] [policy r|k|s]
        ConflictPolicy p = argc > 5 && argv[5][0] == 'k' ? ConflictPolicy::KeepBoth
                           : argc > 5 && argv[5][0] == 's' ? ConflictPolicy::Skip
                                                           : ConflictPolicy::Replace;
        r = extractArchive(argv[2], argv[3], argc > 4 ? argv[4] : "", p, prog, err);
    } else if (!strcmp(argv[1], "l")) {  // l archive [password]
        std::vector<ArcEntry> e;
        r = listArchive(argv[2], argc > 3 ? argv[3] : "", e, err);
        printf("%zu entries\n", e.size());
    } else if (!strcmp(argv[1], "c") && argc >= 6) {  // c out fmt password input...
        std::vector<std::string> in(argv + 5, argv + argc);
        ArcFormat f = argv[3][0] == '7' ? ArcFormat::SevenZip : argv[3][0] == 't' ? ArcFormat::TarGz : ArcFormat::Zip;
        r = createArchive(in, argv[2], f, argv[4], prog, err);
    }
    printf("%s%s%s\n", name(r), err.empty() ? "" : ": ", err.c_str());
    return 0;
}
