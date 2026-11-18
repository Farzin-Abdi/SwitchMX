// Wi-Fi file transfer: a small web server that serves a file manager page to any browser on the
// same network. It reaches only the storage roots it is given (SD card, USB drives) and asks for a
// PIN shown on the console before it lists, reads or changes anything.
#pragma once
#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

struct WebRequest;

class WebServer {
public:
    struct Root {
        std::string path, name;
    };
    ~WebServer() { stop(); }
    bool start(int port = 8080);
    void stop();
    bool running() const { return running_; }
    int port() const { return port_; }
    const std::string& pin() const { return pin_; }
    std::string error() const;
    // The places browsers may reach; updated by the app as drives come and go.
    void setRoots(const std::vector<Root>& roots);
    // True once after a browser changed files (so lists on screen can be refreshed).
    bool takeChanged() { return changed_.exchange(false); }
    int activeTransfers() const { return transfers_; }
    // Files a browser renamed, moved ({from, to}) or deleted ({path, ""}) since the last call, so
    // playlists, likes and resume points can follow them. Taken on the app's own thread.
    std::vector<std::pair<std::string, std::string>> takeMoves();
    int aliveThreads() const { return alive_; }
    // Where the server reports problems worth keeping (the app's event log). Set before start().
    std::function<void(const std::string&)> onLog;
    // Developer mode (devtools.hpp): requests under /dev/ with this token go to the handler; an
    // empty token turns /dev/ off. The handler runs on a server thread.
    using DevHandler = std::function<bool(const std::string& path, const std::map<std::string, std::string>& query,
                                          const std::string& body, int& code, std::string& type, std::string& out)>;
    void setDev(const std::string& token, DevHandler handler) {
        std::lock_guard<std::mutex> lk(m_);
        devToken_ = token;
        dev_ = std::move(handler);
    }

private:
    // A few threads made once at start: the Switch allows a process only so many threads, and a
    // thread per browser connection could fail to start.
    static constexpr int kWorkers = 6;
    struct ThreadArg;
    static void* acceptMain(void* arg);
    static void* workerMain(void* arg);
    void acceptLoop(int gen, int listenFd);
    void workerLoop(int gen);
    std::atomic<int> gen_{0}, alive_{0};
    void handle(int fd);
    void handleDev(int fd, struct WebRequest& r);
    void log(const std::string& line) {
        if (onLog) onLog(line);
    }
    bool allowed(const std::string& path);
    std::vector<Root> roots();

    std::atomic<bool> running_{false}, quit_{false}, changed_{false};
    std::atomic<int> transfers_{0}, badPins_{0};
    std::atomic<long long> lockedUntil_{0};
    int listenFd_ = -1, port_ = 0;
    std::string pin_;
    std::mutex qm_;
    std::condition_variable qcv_;
    std::deque<int> queue_;   // accepted connections waiting for a worker
    std::set<int> active_;    // connections being served (closed at once on stop)
    mutable std::mutex m_;
    std::vector<Root> roots_;
    std::string error_;
    std::string devToken_;
    DevHandler dev_;
    std::vector<std::pair<std::string, std::string>> moves_;
    void noteMove(const std::string& from, const std::string& to) {
        std::lock_guard<std::mutex> lk(m_);
        if (moves_.size() < 10000) moves_.push_back({from, to});
    }
};
