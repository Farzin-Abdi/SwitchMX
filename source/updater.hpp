// Updates from GitHub releases: whether a newer SwitchMX is out, and downloading it over the app
// itself. Each step runs on its own thread; the About page checks back every frame.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

class Updater {
public:
    enum State { Idle, Checking, UpToDate, Available, NoRelease, Failed, Downloading, Installed, DownloadFailed };
    static constexpr const char* kRepo = "Farzin-Abdi/SwitchMX";
    static constexpr const char* kReleasesPage = "https://github.com/Farzin-Abdi/SwitchMX/releases";

    void check();
    // Downloads the release's .nro and puts it in place of `nroPath` (the old one is kept until the
    // new one is complete and checked).
    void install(const std::string& nroPath);
    void cancel();
    State state() const;
    std::string latest() const;   // "1.2.0"
    std::string notes() const;    // what's new, from the release
    std::string message() const;  // why it failed
    float progress() const;       // 0..1 while downloading, -1 if unknown
    static int running();         // threads still working (the app waits for them before closing)
    // "1.10.0" > "1.9.2"; a leading "v" is ignored.
    static int compareVersions(const std::string& a, const std::string& b);

private:
    struct Shared;
    std::shared_ptr<Shared> s_;
};
