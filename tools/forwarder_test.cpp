// Dev tool: checks that forwarder::build() reproduces hacBrewPack's archives exactly, given the same
// throwaway keys. Usage: forwarder_test keysfile ncadir
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "forwarder.hpp"

int main(int argc, char** argv) {
    if (argc != 3) return 1;
    forwarder::Keys keys;
    std::string err;
    if (!forwarder::loadKeys(argv[1], keys, err)) {
        puts(err.c_str());
        return 1;
    }
    int failures = 0;
    for (auto& c : forwarder::build(keys)) {
        char hex[33] = {};
        for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", c.id[i]);
        std::string path = std::string(argv[2]) + "/" + hex + (c.type == 0 ? ".cnmt.nca" : ".nca");
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) {
            printf("FAIL type %d: no reference archive named %s\n", c.type, hex);
            failures++;
            continue;
        }
        std::vector<uint8_t> ref(c.data.size() + 1);
        size_t n = fread(ref.data(), 1, ref.size(), f);
        fclose(f);
        bool same = n == c.data.size() && !memcmp(ref.data(), c.data.data(), n);
        printf("%s type %d: %s (%zu bytes)\n", same ? "ok  " : "FAIL", c.type, hex, c.data.size());
        if (!same) failures++;
    }
    return failures;
}
