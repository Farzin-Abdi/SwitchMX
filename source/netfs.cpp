#include "netfs.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <new>

#include <poll.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-dcerpc.h>
#include <smb2/libsmb2-dcerpc-srvsvc.h>
#include <mpv/client.h>
#include <mpv/stream_cb.h>

#include "library.hpp"

namespace netfs {
namespace {

std::vector<Server> g_servers;
std::string g_file;
std::mutex g_mutex;  // listing threads read the list while the UI may change it

// Tabs and line breaks can't appear in a field of the file; everything else is kept as it is.
std::string clean(std::string s) {
    for (auto& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

struct Url {
    std::string scheme, host, path;  // path decoded, starts with '/'
    int port = 0;
    bool ok = false;
};

Url parse(const std::string& url) {
    Url u;
    size_t p = url.find("://");
    if (p == std::string::npos) return u;
    u.scheme = url.substr(0, p);
    std::string rest = url.substr(p + 3);
    size_t slash = rest.find('/');
    std::string auth = rest.substr(0, slash);
    u.path = slash == std::string::npos ? "/" : decode(rest.substr(slash));
    size_t at = auth.rfind('@');
    if (at != std::string::npos) auth = auth.substr(at + 1);  // credentials in a pasted URL
    size_t colon = auth.rfind(':');
    if (colon != std::string::npos) {
        u.port = atoi(auth.c_str() + colon + 1);
        auth = auth.substr(0, colon);
    }
    u.host = auth;
    if (!u.port) u.port = defaultPort(u.scheme);
    u.ok = !u.host.empty();
    return u;
}

std::string key(const std::string& scheme, const std::string& host, int port) {
    std::string h = host;
    for (auto& c : h) c = (char)tolower((unsigned char)c);
    return scheme + "://" + h + ":" + std::to_string(port);
}

// ---- A small FTP client: just enough to list folders. Every step has a time limit.

class Conn {
public:
    ~Conn() {
        if (fd_ >= 0) close(fd_);
    }
    bool open(const std::string& host, int port, std::string& err, in_addr* peer = nullptr) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
            err = "Can't find this server on the network";
            return false;
        }
        sockaddr_in addr = *(sockaddr_in*)res->ai_addr;
        freeaddrinfo(res);
        if (peer) *peer = addr.sin_addr;
        return openAddr(addr, err);
    }
    bool openAddr(const sockaddr_in& addr, std::string& err) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            err = "No network connection";
            return false;
        }
        // Connect with a time limit: a server that's off would otherwise take over a minute.
        int fl = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd_, (const sockaddr*)&addr, sizeof addr);
        if (rc != 0 && errno != EINPROGRESS) {
            err = "The server refused the connection";
            return false;
        }
        if (rc != 0) {
            fd_set w;
            FD_ZERO(&w);
            FD_SET(fd_, &w);
            timeval tv{5, 0};
            if (select(fd_ + 1, nullptr, &w, nullptr, &tv) <= 0) {
                err = "The server didn't answer (is it on, and on this network?)";
                return false;
            }
            int soerr = 0;
            socklen_t len = sizeof soerr;
            getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soerr, &len);
            if (soerr) {
                err = "The server refused the connection";
                return false;
            }
        }
        fcntl(fd_, F_SETFL, fl);
        timeval to{10, 0};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
        return true;
    }
    bool send(const std::string& line) {
        std::string s = line + "\r\n";
        const char* p = s.data();
        size_t n = s.size();
        while (n) {
            ssize_t k = ::send(fd_, p, n, 0);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) return false;
            p += k;
            n -= (size_t)k;
        }
        return true;
    }
    // One reply ("230 Logged in", or a multi-line "211-...\r\n211 End"); returns its code, 0 on failure.
    int reply(std::string* text = nullptr) {
        std::string first;
        if (!line(first) || first.size() < 3) return 0;
        int code = atoi(first.substr(0, 3).c_str());
        if (first.size() > 3 && first[3] == '-') {
            std::string l;
            do {
                if (!line(l)) return 0;
            } while (!(l.size() > 3 && l.compare(0, 3, first, 0, 3) == 0 && l[3] == ' '));
        }
        if (text) *text = first;
        return code;
    }
    int command(const std::string& c, std::string* text = nullptr) { return send(c) ? reply(text) : 0; }
    // Up to n bytes as they arrive; 0 at the end, -1 on an error.
    ssize_t some(char* dst, size_t n) {
        for (;;) {
            ssize_t k = recv(fd_, dst, n, 0);
            if (k < 0 && errno == EINTR) continue;
            return k;
        }
    }
    // Wakes a read blocked on another thread (it then fails).
    void abort() {
        if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
    }
    // Closes at once, without waiting for data still on its way (a reset, not a goodbye): the
    // server stops sending immediately instead of finishing a transfer nobody wants.
    void hardClose() {
        if (fd_ < 0) return;
        linger lg{1, 0};
        setsockopt(fd_, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
        close(fd_);
        fd_ = -1;
        buf_.clear();
    }
    bool isOpen() const { return fd_ >= 0; }
    // Everything until the server closes (a data connection).
    bool readAll(std::string& out) {
        char buf[16384];
        for (;;) {
            ssize_t k = recv(fd_, buf, sizeof buf, 0);
            if (k < 0 && errno == EINTR) continue;
            if (k < 0) return false;
            if (k == 0) return true;
            out.append(buf, (size_t)k);
            if (out.size() > (64u << 20)) return false;
        }
    }

private:
    bool line(std::string& out) {
        out.clear();
        for (;;) {
            size_t e = buf_.find('\n');
            if (e != std::string::npos) {
                out = buf_.substr(0, e);
                if (!out.empty() && out.back() == '\r') out.pop_back();
                buf_.erase(0, e + 1);
                return true;
            }
            char b[2048];
            ssize_t k = recv(fd_, b, sizeof b, 0);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) return false;
            buf_.append(b, (size_t)k);
        }
    }
    int fd_ = -1;
    std::string buf_;
};

// "type=dir;size=0;modify=20240101120000; Name"
bool parseMlsd(const std::string& l, Item& it) {
    size_t sp = l.find(' ');
    if (sp == std::string::npos) return false;
    it.name = l.substr(sp + 1);
    std::string facts = l.substr(0, sp);
    std::string type;
    size_t i = 0;
    while (i < facts.size()) {
        size_t e = facts.find(';', i);
        if (e == std::string::npos) e = facts.size();
        std::string f = facts.substr(i, e - i);
        size_t eq = f.find('=');
        if (eq != std::string::npos) {
            std::string k = f.substr(0, eq), v = f.substr(eq + 1);
            for (auto& c : k) c = (char)tolower((unsigned char)c);
            for (auto& c : v) c = (char)tolower((unsigned char)c);
            if (k == "type") type = v;
            else if (k == "size") it.size = strtoull(v.c_str(), nullptr, 10);
            else if (k == "modify" && v.size() >= 14) {
                struct tm t{};
                t.tm_year = atoi(v.substr(0, 4).c_str()) - 1900;
                t.tm_mon = atoi(v.substr(4, 2).c_str()) - 1;
                t.tm_mday = atoi(v.substr(6, 2).c_str());
                t.tm_hour = atoi(v.substr(8, 2).c_str());
                t.tm_min = atoi(v.substr(10, 2).c_str());
                t.tm_sec = atoi(v.substr(12, 2).c_str());
                it.mtime = mktime(&t);
            }
        }
        i = e + 1;
    }
    if (type == "cdir" || type == "pdir") return false;
    it.dir = type == "dir";
    return !it.name.empty() && (type == "dir" || type == "file" || type.empty());
}

// "drwxr-xr-x 2 user group 4096 Jan 01 12:00 Name" (Unix) or "01-01-24 12:00PM <DIR> Name" (Windows).
bool parseList(const std::string& l, Item& it) {
    if (l.empty() || l.rfind("total ", 0) == 0) return false;
    std::vector<std::string> f;
    size_t i = 0, fieldsEnd = 0;
    auto nextField = [&]() {
        while (i < l.size() && l[i] == ' ') i++;
        size_t s = i;
        while (i < l.size() && l[i] != ' ') i++;
        if (i > s) f.push_back(l.substr(s, i - s));
        fieldsEnd = i;
    };
    if (isdigit((unsigned char)l[0])) {  // Windows / IIS
        for (int k = 0; k < 3; k++) nextField();
        if (f.size() < 3) return false;
        while (fieldsEnd < l.size() && l[fieldsEnd] == ' ') fieldsEnd++;
        it.name = l.substr(fieldsEnd);
        it.dir = f[2] == "<DIR>";
        if (!it.dir) it.size = strtoull(f[2].c_str(), nullptr, 10);
    } else {
        for (int k = 0; k < 8; k++) nextField();
        if (f.size() < 8) return false;
        size_t s = fieldsEnd;
        if (s < l.size() && l[s] == ' ') s++;
        it.name = l.substr(s);
        char t = f[0][0];
        it.size = strtoull(f[4].c_str(), nullptr, 10);
        it.dir = t == 'd';
        if (t == 'l') {  // a link: shown as what its name says it is
            size_t arrow = it.name.find(" -> ");
            if (arrow != std::string::npos) it.name = it.name.substr(0, arrow);
            it.dir = fileExtension(it.name).empty();
        }
    }
    return !it.name.empty() && it.name != "." && it.name != "..";
}

// Opens a passive data connection.
bool passive(Conn& ctl, const in_addr& peer, Conn& data, std::string& err) {
    std::string t;
    if (ctl.command("PASV", &t) != 227) {
        err = "The server doesn't allow passive transfers";
        return false;
    }
    size_t lp = t.find('(');
    unsigned v[6];
    if (lp == std::string::npos ||
        sscanf(t.c_str() + lp + 1, "%u,%u,%u,%u,%u,%u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        err = "The server's reply wasn't understood";
        return false;
    }
    // The address the server names is ignored (often wrong behind a router); its port is used on
    // the address already connected to, as most FTP clients do.
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr = peer;
    a.sin_port = htons((uint16_t)(v[4] * 256 + v[5]));
    return data.openAddr(a, err);
}

bool ftpList(const Server* s, const Url& u, std::vector<Item>& out, std::string& err) {
    Conn ctl;
    in_addr peer{};
    if (!ctl.open(u.host, u.port, err, &peer)) return false;
    if (ctl.reply() != 220) {
        err = "That doesn't look like an FTP server";
        return false;
    }
    std::string user = s && !s->user.empty() ? s->user : "anonymous";
    std::string pass = s && !s->user.empty() ? s->pass : "guest";
    int rc = ctl.command("USER " + user);
    if (rc == 331) rc = ctl.command("PASS " + pass);
    if (rc != 230 && rc != 202) {
        err = rc == 530 ? "Wrong user name or password" : "The server didn't accept the login";
        return false;
    }
    ctl.command("OPTS UTF8 ON");  // names in other alphabets; servers that don't know it ignore it
    if (ctl.command("CWD " + u.path) != 250) {
        err = "This folder isn't on the server any more";
        return false;
    }
    std::string text;
    bool mlsd = true;
    for (int attempt = 0; attempt < 2; attempt++) {
        Conn data;
        if (!passive(ctl, peer, data, err)) return false;
        int code = ctl.command(mlsd ? "MLSD" : "LIST");
        if (code == 125 || code == 150) {
            if (!data.readAll(text)) {
                err = "The folder listing was cut off";
                return false;
            }
            int end = ctl.reply();
            if (end != 226 && end != 250) {
                err = "The folder listing was cut off";
                return false;
            }
            break;
        }
        if (!mlsd || code < 500) {
            err = "The server wouldn't list this folder";
            return false;
        }
        mlsd = false;  // an older server: the classic listing
    }
    ctl.command("QUIT");
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(i, e - i);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        Item it;
        if (mlsd ? parseMlsd(l, it) : parseList(l, it)) out.push_back(it);
        i = e + 1;
    }
    return true;
}

// ---- Playing straight from an FTP server, for mpv (stream_cb). FFmpeg's own FTP waits for the
// server to finish the old transfer on every seek (about 7 s on the console); this drops the
// connection at once and starts a new one where the player wants to read.

class NetStream {
public:
    virtual ~NetStream() = default;
    virtual bool open(const std::string& url, std::string& err) = 0;
    virtual int64_t read(char* dst, uint64_t n) = 0;
    virtual int64_t seek(int64_t to) = 0;
    virtual int64_t size() const = 0;
    virtual void cancel() = 0;
};

class FtpStream : public NetStream {
public:
    bool open(const std::string& url, std::string& err) override {
        u_ = parse(url);
        if (!u_.ok) {
            err = "bad address";
            return false;
        }
        int i = serverFor(url);
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (i >= 0 && i < (int)g_servers.size()) {
                user_ = g_servers[i].user;
                pass_ = g_servers[i].pass;
            }
        }
        if (!connectControl(err)) return false;
        std::string t;
        if (ctl_->command("SIZE " + u_.path, &t) == 213 && t.size() > 4) size_ = strtoll(t.c_str() + 4, nullptr, 10);
        return true;
    }
    int64_t read(char* dst, uint64_t n) override {
        for (int attempt = 0; attempt < 2; attempt++) {
            if (cancelled_) return -1;
            if (size_ >= 0 && pos_ >= size_) return 0;
            std::string err;
            if (!data_ && !startTransfer(err)) {
                drop();
                continue;  // a fresh login once, then give up
            }
            ssize_t k = data_->some(dst, (size_t)std::min<uint64_t>(n, 1 << 20));
            if (k > 0) {
                pos_ += k;
                return k;
            }
            if (cancelled_) return -1;
            if (k == 0 && (size_ < 0 || pos_ >= size_)) return 0;  // the end of the file
            drop();  // cut off early (Wi-Fi hiccup): carry on from here on a new connection
        }
        return -1;
    }
    int64_t seek(int64_t to) override {
        if (to < 0 || (size_ >= 0 && to > size_)) return -1;
        if (to == pos_) return to;
        // A short hop forward (mpv probing the file) is read through instead of reconnecting.
        if (data_ && to > pos_ && to - pos_ <= (256 << 10)) {
            char skip[16384];
            while (pos_ < to) {
                ssize_t k = data_->some(skip, (size_t)std::min<int64_t>(sizeof skip, to - pos_));
                if (k <= 0) break;
                pos_ += k;
            }
            if (pos_ == to) return to;
        }
        drop();
        pos_ = to;
        return to;
    }
    int64_t size() const override { return size_; }
    void cancel() override {
        cancelled_ = true;
        std::lock_guard<std::mutex> lk(m_);
        if (ctl_) ctl_->abort();
        if (data_) data_->abort();
    }

private:
    bool connectControl(std::string& err) {
        auto c = std::make_unique<Conn>();
        if (!c->open(u_.host, u_.port, err, &peer_) || c->reply() != 220) return false;
        int rc = c->command("USER " + (user_.empty() ? std::string("anonymous") : user_));
        if (rc == 331) rc = c->command("PASS " + (user_.empty() ? std::string("guest") : pass_));
        if (rc != 230 && rc != 202) return false;
        c->command("OPTS UTF8 ON");
        if (c->command("TYPE I") != 200) return false;
        std::lock_guard<std::mutex> lk(m_);
        ctl_ = std::move(c);
        return true;
    }
    bool startTransfer(std::string& err) {
        if (!ctl_ && !connectControl(err)) return false;
        auto d = std::make_unique<Conn>();
        if (!passive(*ctl_, peer_, *d, err)) return false;
        if (pos_ > 0 && ctl_->command("REST " + std::to_string(pos_)) != 350) return false;
        int code = ctl_->command("RETR " + u_.path);
        if (code != 125 && code != 150) return false;
        std::lock_guard<std::mutex> lk(m_);
        data_ = std::move(d);
        return true;
    }
    void drop() {
        std::lock_guard<std::mutex> lk(m_);
        if (data_) data_->hardClose();
        if (ctl_) ctl_->hardClose();
        data_.reset();
        ctl_.reset();
    }
    Url u_;
    std::string user_, pass_;
    in_addr peer_{};
    std::unique_ptr<Conn> ctl_, data_;
    int64_t pos_ = 0, size_ = -1;
    std::atomic<bool> cancelled_{false};
    std::mutex m_;  // the connections, for cancel() from mpv's other thread
};

// ---- SMB (Windows, macOS and NAS shares) through libsmb2: NTLM sign-in, SMB 2 and 3.

// Plain words for what went wrong (libsmb2 reports NT status names).
std::string smbError(smb2_context* c, int rc) {
    std::string e = c ? smb2_get_error(c) : "";
    if (e.find("LOGON_FAILURE") != std::string::npos || e.find("WRONG_PASSWORD") != std::string::npos)
        return "Wrong user name or password";
    if (e.find("ACCESS_DENIED") != std::string::npos || e.find("ACCOUNT") != std::string::npos)
        return "The server didn't accept the login";
    if (e.find("BAD_NETWORK_NAME") != std::string::npos) return "There's no shared folder by that name on the server";
    if (e.find("OBJECT_NAME_NOT_FOUND") != std::string::npos || e.find("OBJECT_PATH_NOT_FOUND") != std::string::npos)
        return "This folder isn't on the server any more";
    if (e.find("imeout") != std::string::npos || e.find("connect") != std::string::npos || rc == -ETIMEDOUT ||
        rc == -ECONNREFUSED || rc == -EHOSTUNREACH)
        return "The server didn't answer (is it on, and on this network?)";
    return "Couldn't connect to the server";
}

// "/Share/Films/Series" -> share "Share", path "Films/Series".
void splitShare(const std::string& path, std::string& share, std::string& rest) {
    std::string p = path;
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    size_t slash = p.find('/');
    share = p.substr(0, slash);
    rest = slash == std::string::npos ? "" : p.substr(slash + 1);
    while (!rest.empty() && rest.back() == '/') rest.pop_back();
}

smb2_context* smbConnect(const Url& u, const std::string& share, const std::string& user, const std::string& pass,
                         std::string& err) {
    smb2_context* c = smb2_init_context();
    if (!c) {
        err = "Not enough memory";
        return nullptr;
    }
    smb2_set_timeout(c, 8);
    smb2_set_security_mode(c, SMB2_NEGOTIATE_SIGNING_ENABLED);
    std::string who = user.empty() ? "guest" : user;
    smb2_set_user(c, who.c_str());
    smb2_set_password(c, pass.c_str());
    std::string server = u.host + (u.port && u.port != 445 ? ":" + std::to_string(u.port) : "");
    int rc = smb2_connect_share(c, server.c_str(), share.c_str(), who.c_str());
    if (rc < 0) {
        err = smbError(c, rc);
        smb2_destroy_context(c);
        return nullptr;
    }
    return c;
}

void smbFree(smb2_context* c) {
    if (!c) return;
    smb2_disconnect_share(c);
    smb2_destroy_context(c);
}

// The server's shared folders (its top level), without printers, IPC$ and hidden ones (C$...).
bool smbShares(const Url& u, const std::string& user, const std::string& pass, std::vector<Item>& out, std::string& err) {
    smb2_context* c = smbConnect(u, "IPC$", user, pass, err);
    if (!c) return false;
    struct State {
        bool done = false;
        int status = 0;
        std::vector<Item>* out;
    } st;
    st.out = &out;
    auto cb = [](smb2_context* smb2, int status, void* data, void* priv) {
        auto* s = (State*)priv;
        s->done = true;
        s->status = status;
        if (status) return;
        auto* rep = (srvsvc_NetrShareEnum_rep*)data;
        auto& l1 = rep->ses.ShareInfo.Level1;
        for (uint32_t i = 0; i < l1.EntriesRead && l1.Buffer; i++) {
            auto& sh = l1.Buffer->share_info_1[i];
            if ((sh.type & 3) != SHARE_TYPE_DISKTREE || (sh.type & SHARE_TYPE_HIDDEN) || !sh.netname.utf8) continue;
            std::string n = sh.netname.utf8;
            if (n.empty() || n.back() == '$') continue;
            Item it;
            it.name = n;
            it.dir = true;
            s->out->push_back(it);
        }
        smb2_free_data(smb2, rep);
    };
    if (smb2_share_enum_async(c, SHARE_INFO_1, cb, &st) != 0) {
        err = "The server wouldn't list its shared folders";
        smbFree(c);
        return false;
    }
    time_t t0 = time(nullptr);
    while (!st.done && time(nullptr) - t0 < 10) {
        pollfd pfd{};
        pfd.fd = smb2_get_fd(c);
        pfd.events = smb2_which_events(c);
        if (poll(&pfd, 1, 500) < 0) break;
        if (pfd.revents && smb2_service(c, pfd.revents) < 0) break;
    }
    smbFree(c);
    if (!st.done || st.status) {
        err = "The server wouldn't list its shared folders";
        return false;
    }
    return true;
}

bool smbList(const Server* s, const Url& u, std::vector<Item>& out, std::string& err) {
    std::string user = s ? s->user : "", pass = s ? s->pass : "";
    std::string share, rest;
    splitShare(u.path, share, rest);
    if (share.empty()) return smbShares(u, user, pass, out, err);
    smb2_context* c = smbConnect(u, share, user, pass, err);
    if (!c) return false;
    smb2dir* d = smb2_opendir(c, rest.c_str());
    if (!d) {
        err = smbError(c, 0);
        if (err == "Couldn't connect to the server") err = "This folder isn't on the server any more";
        smbFree(c);
        return false;
    }
    while (smb2dirent* e = smb2_readdir(c, d)) {
        if (!e->name || !strcmp(e->name, ".") || !strcmp(e->name, "..")) continue;
        Item it;
        it.name = e->name;
        it.dir = e->st.smb2_type == SMB2_TYPE_DIRECTORY;
        it.size = e->st.smb2_size;
        it.mtime = (time_t)e->st.smb2_mtime;
        out.push_back(it);
    }
    smb2_closedir(c, d);
    smbFree(c);
    return true;
}

// Playing from a share. Seeking is only a new offset: every read says where it starts.
class SmbStream : public NetStream {
public:
    ~SmbStream() override { closeAll(); }
    bool open(const std::string& url, std::string& err) override {
        u_ = parse(url);
        if (!u_.ok) return false;
        int i = serverFor(url);
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (i >= 0 && i < (int)g_servers.size()) {
                user_ = g_servers[i].user;
                pass_ = g_servers[i].pass;
            }
        }
        return connect(err);
    }
    int64_t read(char* dst, uint64_t n) override {
        for (int attempt = 0; attempt < 2; attempt++) {
            if (cancelled_) return -1;
            if (size_ >= 0 && pos_ >= size_) return 0;
            std::string err;
            if (!fh_ && !connect(err)) continue;
            int64_t rc = pipelined(dst, n);
            if (rc > 0) {
                pos_ += rc;
                return rc;
            }
            if (rc == 0) return 0;
            if (cancelled_) return -1;
            if (c_) closeAll();  // the connection went (Wi-Fi hiccup): once more on a new one
        }
        return -1;
    }
    int64_t seek(int64_t to) override {
        if (to < 0 || (size_ >= 0 && to > size_)) return -1;
        pos_ = to;
        return to;
    }
    int64_t size() const override { return size_; }
    void cancel() override {
        cancelled_ = true;
        std::lock_guard<std::mutex> lk(m_);
        if (c_) shutdown(smb2_get_fd(c_), SHUT_RDWR);  // wakes a read waiting for the server
    }

private:
    // Several reads in flight at once: one at a time, each waits a Wi-Fi round trip, and servers
    // that only allow small reads (64 KB) then manage barely more than the film needs. Returns the
    // bytes read from pos_ in order (0 at the end of the file, -1 on an error).
    int64_t pipelined(char* dst, uint64_t n) {
        struct Req {
            bool done = false;
            int status = 0;
        };
        uint64_t chunk = std::min<uint64_t>(maxRead_, 1 << 20);
        if (size_ >= 0) n = std::min<uint64_t>(n, (uint64_t)(size_ - pos_));
        n = std::min<uint64_t>(n, chunk * 16);
        if (n == 0) return 0;
        size_t count = (size_t)((n + chunk - 1) / chunk);
        std::vector<Req> reqs(count);
        auto cb = [](smb2_context*, int status, void*, void* priv) {
            auto* r = (Req*)priv;
            r->status = status;
            r->done = true;
        };
        size_t issued = 0;
        for (; issued < count; issued++) {
            uint64_t off = issued * chunk, len = std::min<uint64_t>(chunk, n - off);
            if (smb2_pread_async(c_, fh_, (uint8_t*)dst + off, (uint32_t)len, (uint64_t)pos_ + off, cb, &reqs[issued]) != 0)
                break;
        }
        if (issued == 0) return -1;
        // Wait for every request sent: their buffers and reqs are only valid in here, so on any
        // failure the connection is abandoned before returning (that answers what's still out).
        time_t t0 = time(nullptr);
        auto allDone = [&] {
            for (size_t i = 0; i < issued; i++)
                if (!reqs[i].done) return false;
            return true;
        };
        while (!allDone()) {
            pollfd pfd{};
            pfd.fd = smb2_get_fd(c_);
            pfd.events = smb2_which_events(c_);
            bool failed = cancelled_ || time(nullptr) - t0 > 15 || poll(&pfd, 1, 500) < 0 ||
                          (pfd.revents && smb2_service(c_, pfd.revents) < 0);
            if (failed) {
                abandon();
                return -1;
            }
        }
        int64_t total = 0;
        for (size_t i = 0; i < issued; i++) {
            if (reqs[i].status < 0) return total > 0 ? total : -1;
            total += reqs[i].status;
            if ((uint64_t)reqs[i].status < std::min<uint64_t>(chunk, n - i * chunk)) break;  // the end of the file
        }
        return total;
    }

    bool connect(std::string& err) {
        std::string share, rest;
        splitShare(u_.path, share, rest);
        smb2_context* c = smbConnect(u_, share, user_, pass_, err);
        if (!c) return false;
        smb2fh* fh = smb2_open(c, rest.c_str(), O_RDONLY);
        if (!fh) {
            err = smbError(c, 0);
            smbFree(c);
            return false;
        }
        smb2_stat_64 st{};
        if (smb2_fstat(c, fh, &st) == 0) size_ = (int64_t)st.smb2_size;
        maxRead_ = std::max<uint32_t>(64 << 10, smb2_get_max_read_size(c));
        std::lock_guard<std::mutex> lk(m_);
        c_ = c;
        fh_ = fh;
        return true;
    }
    // Drops the connection at once, without the goodbyes (which would wait for the server).
    // libsmb2 answers requests still out with an error as it goes.
    void abandon() {
        std::lock_guard<std::mutex> lk(m_);
        if (c_) smb2_destroy_context(c_);
        c_ = nullptr;
        fh_ = nullptr;  // freed with its context
    }
    void closeAll() {
        std::lock_guard<std::mutex> lk(m_);
        if (c_ && fh_) smb2_close(c_, fh_);
        fh_ = nullptr;
        smbFree(c_);
        c_ = nullptr;
    }
    Url u_;
    std::string user_, pass_;
    smb2_context* c_ = nullptr;
    smb2fh* fh_ = nullptr;
    int64_t pos_ = 0, size_ = -1;
    uint32_t maxRead_ = 1 << 20;
    std::atomic<bool> cancelled_{false};
    std::mutex m_;
};

// mpv may call cancel after close (see readahead.cpp): cookies are ids into this registry.
std::mutex g_streamsMutex;
std::map<uintptr_t, NetStream*> g_streams;
uintptr_t g_nextStream = 1;

NetStream* streamFor(void* cookie) {
    std::lock_guard<std::mutex> lk(g_streamsMutex);
    auto it = g_streams.find((uintptr_t)cookie);
    return it == g_streams.end() ? nullptr : it->second;
}

int64_t cbRead(void* c, char* buf, uint64_t n) {
    NetStream* s = streamFor(c);
    return s ? s->read(buf, n) : -1;
}
int64_t cbSeek(void* c, int64_t off) {
    NetStream* s = streamFor(c);
    if (!s) return MPV_ERROR_GENERIC;
    int64_t r = s->seek(off);
    return r < 0 ? MPV_ERROR_GENERIC : r;
}
int64_t cbSize(void* c) {
    NetStream* s = streamFor(c);
    int64_t n = s ? s->size() : -1;
    return n < 0 ? MPV_ERROR_UNSUPPORTED : n;
}
void cbClose(void* c) {
    NetStream* s = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_streamsMutex);
        auto it = g_streams.find((uintptr_t)c);
        if (it == g_streams.end()) return;
        s = it->second;
        g_streams.erase(it);
    }
    delete s;
}
void cbCancel(void* c) {
    std::lock_guard<std::mutex> lk(g_streamsMutex);
    auto it = g_streams.find((uintptr_t)c);
    if (it != g_streams.end()) it->second->cancel();
}

int cbOpen(void*, char* uri, mpv_stream_cb_info* info) {
    std::string url = fromStreamUrl(uri);
    NetStream* s = url.rfind("smb://", 0) == 0 ? (NetStream*)new (std::nothrow) SmbStream()
                                                : (NetStream*)new (std::nothrow) FtpStream();
    if (!s) return MPV_ERROR_NOMEM;
    std::string err;
    bool ok = false;
    try {
        ok = s->open(url, err);
    } catch (...) {
    }
    if (!ok) {
        delete s;
        return MPV_ERROR_LOADING_FAILED;
    }
    {
        std::lock_guard<std::mutex> lk(g_streamsMutex);
        uintptr_t id = g_nextStream++;
        g_streams[id] = s;
        info->cookie = (void*)id;
    }
    info->read_fn = cbRead;
    info->seek_fn = cbSeek;
    info->size_fn = cbSize;
    info->close_fn = cbClose;
    info->cancel_fn = cbCancel;
    return 0;
}

}  // namespace

std::vector<Server>& servers() { return g_servers; }

void load(const std::string& dataDir) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_file = dataDir + "servers.tsv";
    g_servers.clear();
    FILE* f = fopen(g_file.c_str(), "rb");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        std::vector<std::string> v;
        size_t i = 0;
        while (i <= l.size()) {
            size_t e = l.find('\t', i);
            if (e == std::string::npos) e = l.size();
            v.push_back(l.substr(i, e - i));
            i = e + 1;
        }
        if (v.size() < 7 || v[0].empty() || v[0][0] == '#') continue;
        Server s;
        s.type = v[0];
        s.name = v[1];
        s.host = v[2];
        s.port = atoi(v[3].c_str());
        s.user = v[4];
        s.pass = v[5];
        s.path = v[6].empty() ? "/" : v[6];
        g_servers.push_back(s);
    }
    fclose(f);
}

bool save() {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::string out = "# SwitchMX network places: type, name, host, port, user, password, folder\n";
    for (auto& s : g_servers)
        out += clean(s.type) + "\t" + clean(s.name) + "\t" + clean(s.host) + "\t" + std::to_string(s.port) + "\t" +
               clean(s.user) + "\t" + clean(s.pass) + "\t" + clean(s.path) + "\n";
    return !g_file.empty() && writeFileAtomic(g_file, out);
}

bool isNetPath(const std::string& p) { return p.rfind("ftp://", 0) == 0 || p.rfind("smb://", 0) == 0; }

void registerStreams(mpv_handle* mpv) {
    mpv_stream_cb_add_ro(mpv, "smxftp", nullptr, cbOpen);
    mpv_stream_cb_add_ro(mpv, "smxsmb", nullptr, cbOpen);
}

std::string streamUrl(const std::string& url) {
    // SMB always (FFmpeg has none); FTP for saved servers (an ftp:// address from a playlist keeps
    // FFmpeg's own FTP and the login written in it).
    if (url.rfind("smb://", 0) == 0) return "smxsmb://" + url.substr(6);
    if (url.rfind("ftp://", 0) != 0 || serverFor(url) < 0) return url;
    return "smxftp://" + url.substr(6);
}

std::string fromStreamUrl(const std::string& url) {
    if (url.rfind("smxftp://", 0) == 0) return "ftp://" + url.substr(9);
    if (url.rfind("smxsmb://", 0) == 0) return "smb://" + url.substr(9);
    return url;
}

int defaultPort(const std::string& type) { return type == "ftp" ? 21 : type == "smb" ? 445 : 0; }

std::string encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        // '@' and ':' are encoded too: file names like "@Channel_Film.mkv" are common.
        if (isalnum(c) || (c && strchr("-_.~/!$&'()*+,;=", c))) out += (char)c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string rootUrl(const Server& s) {
    std::string p = s.path.empty() ? "/" : s.path;
    if (p[0] != '/') p = "/" + p;
    if (p.back() != '/') p += '/';
    int port = s.port ? s.port : defaultPort(s.type);
    return s.type + "://" + s.host + ":" + std::to_string(port) + encode(p);
}

int serverFor(const std::string& url) {
    Url u = parse(url);
    if (!u.ok) return -1;
    std::string k = key(u.scheme, u.host, u.port);
    std::lock_guard<std::mutex> lk(g_mutex);
    for (size_t i = 0; i < g_servers.size(); i++) {
        const Server& s = g_servers[i];
        if (key(s.type, s.host, s.port ? s.port : defaultPort(s.type)) == k) return (int)i;
    }
    return -1;
}

std::string withCredentials(const std::string& url) {
    if (!isNetPath(url)) return url;
    size_t authEnd = url.find('/', url.find("://") + 3);
    if (url.substr(0, authEnd).find('@') != std::string::npos) return url;  // has its own login
    int i = serverFor(url);
    if (i < 0) return url;
    Server s;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        s = g_servers[i];
    }
    if (s.user.empty()) return url;
    // In the user and password, '/', ':' and '@' must be encoded too.
    auto part = [](const std::string& v) {
        static const char* hex = "0123456789ABCDEF";
        std::string o;
        for (unsigned char c : v) {
            if (isalnum(c) || (c && strchr("-_.~", c))) o += (char)c;
            else {
                o += '%';
                o += hex[c >> 4];
                o += hex[c & 15];
            }
        }
        return o;
    };
    size_t p = url.find("://") + 3;
    return url.substr(0, p) + part(s.user) + ":" + part(s.pass) + "@" + url.substr(p);
}

std::string withoutCredentials(const std::string& url) {
    if (!isNetPath(url)) return url;
    size_t p = url.find("://") + 3, authEnd = url.find('/', p);
    size_t at = url.substr(0, authEnd).rfind('@');
    if (at == std::string::npos || at < p) return url;
    std::string bare = url.substr(0, p) + url.substr(at + 1);
    return serverFor(bare) >= 0 ? bare : url;
}

std::string displayPath(const std::string& url) {
    Url u = parse(url);
    if (!u.ok) return url;
    int i = serverFor(url);
    std::string name;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        name = i >= 0 ? g_servers[i].name : u.host;
    }
    return name + u.path;
}

bool list(const std::string& dirUrl, std::vector<Item>& out, std::string& error) {
    out.clear();
    Url u = parse(dirUrl);
    if (!u.ok) {
        error = "That isn't a network address";
        return false;
    }
    int i = serverFor(dirUrl);
    Server s;
    bool known = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (i >= 0 && i < (int)g_servers.size()) {
            s = g_servers[i];
            known = true;
        }
    }
    if (u.scheme == "ftp") return ftpList(known ? &s : nullptr, u, out, error);
    if (u.scheme == "smb") return smbList(known ? &s : nullptr, u, out, error);
    error = "This kind of server isn't supported";
    return false;
}

}  // namespace netfs
