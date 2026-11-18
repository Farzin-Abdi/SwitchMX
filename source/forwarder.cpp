#include "forwarder.hpp"
#include "i18n.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "minicrypto.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

extern "C" {
extern const unsigned char g_fwdProgram[], g_fwdControl[], g_fwdMeta[], g_fwdProgramLegacy[];
extern const unsigned int g_fwdProgramSize, g_fwdControlSize, g_fwdMetaSize, g_fwdProgramLegacySize;
}

namespace forwarder {
namespace {
constexpr size_t kHeaderSize = 0xC00, kSector = 0x200;
constexpr size_t kKeyArea = 0x300, kFsHashes = 0x280, kSections = 0x240, kFsHeaders = 0x400;
constexpr int kMaxSections = 4;
constexpr uint8_t kCryptCtr = 3;

uint32_t get32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint64_t get64(const uint8_t* p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

void sha256(const void* data, size_t len, uint8_t out[32]) {
    mc_sha256 c;
    mc_sha256_init(&c);
    mc_sha256_update(&c, data, len);
    mc_sha256_final(&c, out);
}

// AES-128-XTS with a big-endian sector number as the tweak, as NCA headers use it.
void xtsEncrypt(const uint8_t key[32], uint8_t* data, size_t len) {
    mc_aes dataKey, tweakKey;
    mc_aes_init(&dataKey, key, 16);
    mc_aes_init(&tweakKey, key + 16, 16);
    for (size_t sector = 0; sector * kSector < len; sector++) {
        uint8_t iv[16] = {}, tweak[16];
        for (int i = 0; i < 8; i++) iv[15 - i] = (uint8_t)((uint64_t)sector >> (i * 8));
        mc_aes_encrypt_block(&tweakKey, iv, tweak);
        uint8_t* s = data + sector * kSector;
        for (size_t off = 0; off < kSector; off += 16) {
            uint8_t block[16];
            for (int i = 0; i < 16; i++) block[i] = s[off + i] ^ tweak[i];
            mc_aes_encrypt_block(&dataKey, block, block);
            for (int i = 0; i < 16; i++) s[off + i] = block[i] ^ tweak[i];
            uint8_t carry = 0;
            for (int i = 0; i < 16; i++) {
                uint8_t next = tweak[i] >> 7;
                tweak[i] = (uint8_t)(tweak[i] << 1) | carry;
                carry = next;
            }
            if (carry) tweak[0] ^= 0x87;
        }
    }
}

void ctrEncrypt(const mc_aes& aes, const uint8_t sectionCtr[8], uint8_t* data, uint64_t start, uint64_t end) {
    uint8_t ctr[16], stream[16];
    for (int i = 0; i < 8; i++) ctr[i] = sectionCtr[7 - i];
    for (uint64_t off = start; off < end; off += 16) {
        uint64_t n = off >> 4;
        for (int i = 0; i < 8; i++) ctr[15 - i] = (uint8_t)(n >> (i * 8));
        mc_aes_encrypt_block(&aes, ctr, stream);
        for (int i = 0; i < 16 && off + i < end; i++) data[off + i] ^= stream[i];
    }
}

// Turns a plain template into the final, encrypted archive.
Content finish(std::vector<uint8_t> nca, const Keys& keys, int type) {
    // Any key works for the sections; hacBrewPack's default keeps its output comparable.
    uint8_t sectionKey[16];
    memset(sectionKey, 4, sizeof sectionKey);
    uint8_t* area = nca.data() + kKeyArea;
    memset(area, 0, 0x40);
    memcpy(area + 0x20, sectionKey, 16);
    mc_aes kaek;
    mc_aes_init(&kaek, keys.keyArea, 16);
    for (int i = 0; i < 4; i++) mc_aes_encrypt_block(&kaek, area + i * 16, area + i * 16);

    mc_aes aes;
    mc_aes_init(&aes, sectionKey, 16);
    for (int i = 0; i < kMaxSections; i++) {
        const uint8_t* entry = nca.data() + kSections + i * 16;
        uint64_t start = get32(entry) * (uint64_t)kSector, end = get32(entry + 4) * (uint64_t)kSector;
        const uint8_t* fs = nca.data() + kFsHeaders + i * kSector;
        if (end <= start || end > nca.size() || fs[4] != kCryptCtr) continue;
        ctrEncrypt(aes, fs + 0x140, nca.data(), start, end);
    }
    xtsEncrypt(keys.header, nca.data(), kHeaderSize);

    Content c;
    sha256(nca.data(), nca.size(), c.hash);
    memcpy(c.id, c.hash, 16);
    c.type = type;
    c.data = std::move(nca);
    return c;
}

// The icon's version. The HOME Menu keeps each app's icon cached per version, so a new icon only
// shows when this goes up: raise it (with kForwarderBuild in app.cpp) whenever the icon or the
// launcher changes. Title versions count in steps of 0x10000.
constexpr uint32_t kIconVersion = 8u << 16;

// The meta template lists the other two archives, so it can only be completed after them.
std::vector<uint8_t> completeMeta(const Content& program, const Content& control) {
    std::vector<uint8_t> nca(g_fwdMeta, g_fwdMeta + g_fwdMetaSize);
    const uint8_t* entry = nca.data() + kSections;
    uint64_t section = get32(entry) * (uint64_t)kSector;
    uint8_t* fs = nca.data() + kFsHeaders;
    uint32_t blockSize = get32(fs + 0x28);
    uint64_t tableOffset = get64(fs + 0x30), tableSize = get64(fs + 0x38);
    uint64_t pfsOffset = get64(fs + 0x40), pfsSize = get64(fs + 0x48);
    uint8_t* pfs = nca.data() + section + pfsOffset;
    uint32_t files = get32(pfs + 4), strings = get32(pfs + 8);
    uint8_t* cnmt = pfs + 0x10 + files * 0x18 + strings;

    // The packaged meta header starts with the title id and then the version.
    for (int b = 0; b < 4; b++) cnmt[8 + b] = (uint8_t)(kIconVersion >> (b * 8));
    // Header, application header, then one record per archive.
    uint8_t* rec = cnmt + 0x20 + 0x10;
    const Content* contents[2] = {&program, &control};
    for (int i = 0; i < 2; i++, rec += 0x38) {
        memcpy(rec, contents[i]->hash, 32);
        memcpy(rec + 0x20, contents[i]->id, 16);
        uint64_t size = contents[i]->data.size();
        for (int b = 0; b < 6; b++) rec[0x30 + b] = (uint8_t)(size >> (b * 8));
    }

    uint8_t* table = nca.data() + section + tableOffset;
    for (uint64_t off = 0, i = 0; off < pfsSize; off += blockSize, i++) {
        uint64_t n = pfsSize - off < blockSize ? pfsSize - off : blockSize;
        sha256(pfs + off, n, table + i * 32);
    }
    sha256(table, tableSize, fs + 0x8);
    sha256(fs, kSector, nca.data() + kFsHashes);
    return nca;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool parseHex(const char* s, uint8_t* out, size_t bytes) {
    for (size_t i = 0; i < bytes; i++) {
        int hi = hexValue(s[i * 2]), lo = hi < 0 ? -1 : hexValue(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return hexValue(s[bytes * 2]) < 0;
}
}  // namespace

bool loadKeys(const std::string& path, Keys& out, std::string& error) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        error = trf("Can't open {0}", {path});
        return false;
    }
    bool gotHeader = false, gotArea = false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        char* name = line;
        while (isspace((unsigned char)*name)) name++;
        char* nameEnd = eq;
        while (nameEnd > name && isspace((unsigned char)nameEnd[-1])) nameEnd--;
        *nameEnd = 0;
        char* value = eq + 1;
        while (isspace((unsigned char)*value)) value++;
        for (char* c = name; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (!strcmp(name, "header_key")) gotHeader = parseHex(value, out.header, 32);
        else if (!strcmp(name, "key_area_key_application_00")) gotArea = parseHex(value, out.keyArea, 16);
    }
    fclose(f);
    if (!gotHeader || !gotArea) {
        error = "That keys file is missing header_key or key_area_key_application_00";
        return false;
    }
    return true;
}

std::vector<Content> build(const Keys& keys, bool legacy) {
    std::vector<Content> out;
    // Firmware 19.0.0 changed how a program's debug permission is written: a launcher made for one
    // layout is refused by the other (2001-0126 on 18.x), so the one matching the console goes in.
    if (legacy)
        out.push_back(finish(std::vector<uint8_t>(g_fwdProgramLegacy, g_fwdProgramLegacy + g_fwdProgramLegacySize), keys, 1));
    else
        out.push_back(finish(std::vector<uint8_t>(g_fwdProgram, g_fwdProgram + g_fwdProgramSize), keys, 1));
    out.push_back(finish(std::vector<uint8_t>(g_fwdControl, g_fwdControl + g_fwdControlSize), keys, 3));
    out.push_back(finish(completeMeta(out[0], out[1]), keys, 0));
    return out;
}

std::string findKeys() {
    // Lockpick_RCM writes switch/prod.keys; other tools and guides use these names and places too.
    static const char* paths[] = {"sdmc:/switch/prod.keys", "sdmc:/prod.keys", "sdmc:/switch/keys.txt",
                                  "sdmc:/keys.txt", "sdmc:/atmosphere/prod.keys", "sdmc:/switch/Lockpick_RCM/prod.keys"};
    struct stat st;
    for (auto p : paths)
        if (stat(p, &st) == 0 && st.st_size > 0) return p;
    // Copies kept next to a tool, e.g. switch/DBI/prod.keys
    if (DIR* d = opendir("sdmc:/switch")) {
        std::string found;
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string p = std::string("sdmc:/switch/") + e->d_name + "/prod.keys";
            if (stat(p.c_str(), &st) == 0 && st.st_size > 0) {
                found = p;
                break;
            }
        }
        closedir(d);
        if (!found.empty()) return found;
    }
    return "";
}

#ifdef __SWITCH__
namespace {
constexpr const char* kNroPath = "sdmc:/switch/SwitchMX/SwitchMX.nro";

std::string describe(const char* what, Result rc) {
    char buf[96];
    snprintf(buf, sizeof buf, "%s failed (error %04d-%04d)", what, 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
    return buf;
}

// Not in libnx: IApplicationManagerInterface command 16.
Result pushApplicationRecord(u64 appId, const NcmContentMetaKey& key, u8 storage) {
    struct {
        NcmContentMetaKey key;
        u8 storage_id;
        u8 pad[7];
    } record = {};
    record.key = key;
    record.storage_id = storage;
    struct {
        u8 last_modified_event;
        u8 pad[7];
        u64 app_id;
    } in = {3, {}, appId};
    Service srv;
    Result rc = nsGetApplicationManagerInterface(&srv);
    if (R_FAILED(rc)) return rc;
    rc = serviceDispatchIn(&srv, 16, in, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
                           .buffers = {{&record, sizeof record}});
    serviceClose(&srv);
    return rc;
}

Result writeContent(NcmContentStorage& cs, const Content& c) {
    NcmContentId id;
    memcpy(id.c, c.id, 16);
    bool present = false;
    Result rc = ncmContentStorageHas(&cs, &present, &id);
    if (R_FAILED(rc)) return rc;
    if (present) return 0;
    NcmPlaceHolderId ph;
    rc = ncmContentStorageGeneratePlaceHolderId(&cs, &ph);
    if (R_FAILED(rc)) return rc;
    rc = ncmContentStorageCreatePlaceHolder(&cs, &id, &ph, (s64)c.data.size());
    if (R_FAILED(rc)) return rc;
    constexpr size_t kChunk = 256 * 1024;
    for (size_t off = 0; off < c.data.size() && R_SUCCEEDED(rc); off += kChunk) {
        size_t n = c.data.size() - off < kChunk ? c.data.size() - off : kChunk;
        rc = ncmContentStorageWritePlaceHolder(&cs, &ph, off, c.data.data() + off, n);
    }
    if (R_SUCCEEDED(rc)) rc = ncmContentStorageRegister(&cs, &id, &ph);
    if (R_FAILED(rc)) ncmContentStorageDeletePlaceHolder(&cs, &ph);
    return rc;
}
}  // namespace

bool supported() { return true; }

bool legacyFirmware() { return hosversionBefore(19, 0, 0); }

bool runningFromIcon() {
    u64 id = 0;
    return R_SUCCEEDED(svcGetInfo(&id, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) && id == kTitleId;
}

bool installed() {
    if (R_FAILED(nsInitialize())) return false;
    bool found = false;
    NsApplicationRecord records[64];
    s32 offset = 0, count = 0;
    while (!found && R_SUCCEEDED(nsListApplicationRecord(records, 64, offset, &count)) && count > 0) {
        for (s32 i = 0; i < count; i++)
            if (records[i].application_id == kTitleId) found = true;
        offset += count;
    }
    nsExit();
    return found;
}

bool install(std::string& error) {
    struct stat st;
    if (stat(kNroPath, &st) != 0) {
        error = trf("The icon starts {0}, which doesn't exist. Move the app there first.", {kNroPath});
        return false;
    }
    std::string keyPath = findKeys();
    if (keyPath.empty()) {
        error = "No keys file on the SD card";
        return false;
    }
    Keys keys;
    if (!loadKeys(keyPath, keys, error)) return false;
    std::vector<Content> contents = build(keys, legacyFirmware());
    memset(&keys, 0, sizeof keys);

    Result rc = ncmInitialize();
    if (R_FAILED(rc)) {
        error = describe("Opening storage", rc);
        return false;
    }
    NcmContentStorage cs;
    NcmContentMetaDatabase db;
    rc = ncmOpenContentStorage(&cs, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        error = describe("Opening the SD card's content storage", rc);
        ncmExit();
        return false;
    }
    rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        error = describe("Opening the SD card's database", rc);
        ncmContentStorageClose(&cs);
        ncmExit();
        return false;
    }

    for (auto& c : contents) {
        rc = writeContent(cs, c);
        if (R_FAILED(rc)) {
            error = describe("Writing the icon's files", rc);
            break;
        }
    }
    NcmContentMetaKey key = {};
    key.id = kTitleId;
    key.version = kIconVersion;
    key.type = NcmContentMetaType_Application;
    key.install_type = NcmContentInstallType_Full;
    if (R_SUCCEEDED(rc)) {
        struct {
            NcmContentMetaHeader header;
            NcmApplicationMetaExtendedHeader ext;
            NcmContentInfo info[3];
        } meta = {};
        meta.header.extended_header_size = sizeof meta.ext;
        meta.header.content_count = 3;
        meta.ext.patch_id = kTitleId + 0x800;
        for (int i = 0; i < 3; i++) {
            memcpy(meta.info[i].content_id.c, contents[i].id, 16);
            meta.info[i].size_low = (u32)contents[i].data.size();
            meta.info[i].content_type = (u8)contents[i].type;
        }
        rc = ncmContentMetaDatabaseSet(&db, &key, &meta, sizeof meta);
        if (R_SUCCEEDED(rc)) rc = ncmContentMetaDatabaseCommit(&db);
        if (R_FAILED(rc)) error = describe("Saving the icon's details", rc);
    }
    ncmContentMetaDatabaseClose(&db);
    ncmContentStorageClose(&cs);
    ncmExit();
    if (R_FAILED(rc)) return false;

    rc = nsInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = pushApplicationRecord(kTitleId, key, NcmStorageId_SdCard);
        // The HOME Menu keeps app icons in a cache: drop SwitchMX's so a new icon shows. Not wrapped
        // by libnx: IApplicationManagerInterface 404 InvalidateApplicationControlCache(application id).
        if (R_SUCCEEDED(rc)) {
            Service* am = nsGetServiceSession_ApplicationManagerInterface();
            u64 id = kTitleId;
            if (am && R_FAILED(serviceDispatchIn(am, 404, id))) serviceDispatch(am, 401);  // all, as a fallback
        }
        nsExit();
    }
    if (R_FAILED(rc)) {
        error = describe("Adding the icon to the HOME Menu", rc);
        return false;
    }
    return true;
}

bool remove(std::string& error) {
    Result rc = nsInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = nsDeleteApplicationCompletely(kTitleId);
        nsExit();
    }
    if (R_FAILED(rc)) {
        error = describe("Removing the icon", rc);
        return false;
    }
    return true;
}
#else
bool supported() { return false; }
bool runningFromIcon() { return false; }
bool installed() { return false; }
bool legacyFirmware() { return false; }
bool install(std::string& error) {
    error = "Only on the Switch";
    return false;
}
bool remove(std::string& error) {
    error = "Only on the Switch";
    return false;
}
#endif

}  // namespace forwarder
