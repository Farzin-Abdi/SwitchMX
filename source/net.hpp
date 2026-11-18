// HTTPS requests over FFmpeg's TLS (on the Switch, the console's own SSL service), and a small JSON
// reader. Blocking: call from a background thread.
#pragma once
#include <atomic>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace net {

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    double num = 0;
    bool b = false;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json& operator[](const char* k) const {
        static Json none;
        for (auto& kv : obj)
            if (kv.first == k) return kv.second;
        return none;
    }
};
bool parseJson(const std::string& text, Json& out);

struct Response {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string error;  // set when there was no answer at all
};

// Where a download goes instead of memory, with its progress (bytes so far / expected, 0 = unknown).
struct Sink {
    FILE* file = nullptr;
    std::atomic<long long> done{0}, total{0};
    std::atomic<bool> cancel{false};
    bool failed = false;  // the file couldn't be written
};

Response request(const std::string& method, const std::string& host, const std::string& path,
                 const std::vector<std::string>& headers, const std::string& body, Sink* sink = nullptr);
// Follows redirects; `url` is https://host/path.
Response get(std::string url, const std::vector<std::string>& headers = {}, Sink* sink = nullptr);

}  // namespace net
