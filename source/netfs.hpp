// Network places: servers the user adds once (FTP now; the design leaves room for SMB), browsed
// like a drive and played from directly.
//
// Paths for network files are URLs without credentials ("ftp://192.168.1.5:21/Films/A%20b.mkv"),
// so history, favourites and the last folder never hold a password. The password is added only
// when a file is handed to the player (withCredentials) or a folder is listed.
#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

typedef struct mpv_handle mpv_handle;

namespace netfs {

struct Server {
    std::string type = "ftp";  // "ftp"
    std::string name;          // shown on the Storage page
    std::string host;
    int port = 0;              // 0: the protocol's usual one
    std::string user, pass;    // empty user: anonymous
    std::string path = "/";    // folder to start in
};

struct Item {
    std::string name;  // as it really is (UTF-8)
    bool dir = false;
    uint64_t size = 0;
    time_t mtime = 0;
};

// Saved servers (in <dataDir>servers.tsv).
std::vector<Server>& servers();
void load(const std::string& dataDir);
bool save();

bool isNetPath(const std::string& path);
int defaultPort(const std::string& type);
// "ftp://host:port/start/folder/", the folder a server opens in (no credentials).
std::string rootUrl(const Server& s);
// The saved server a URL belongs to, or -1.
int serverFor(const std::string& url);
// The URL with the server's user name and password in it, for the player.
std::string withCredentials(const std::string& url);
// The reverse, for paths read back from the player (only when a saved server has that login).
std::string withoutCredentials(const std::string& url);
// "Name/Films/Series" for headers and details.
std::string displayPath(const std::string& url);
// One path component, percent-encoded for a URL / decoded back.
std::string encode(const std::string& s);
std::string decode(const std::string& s);

// Playing: registers the "smxftp" stream with mpv; streamUrl() turns a saved server's file into
// it, fromStreamUrl() turns what mpv reports back into the file's own address.
void registerStreams(mpv_handle* mpv);
std::string streamUrl(const std::string& url);
std::string fromStreamUrl(const std::string& url);

// Lists a folder. Blocks (with time limits of a few seconds): call it off the UI thread.
bool list(const std::string& dirUrl, std::vector<Item>& out, std::string& error);

}  // namespace netfs
