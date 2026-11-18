#pragma once
// Developer mode: lets a computer on the same network test the app while it runs on the console.
// Served by the Wi-Fi transfer server under /dev/, only while it's switched on in Settings → Debug and
// only with the token shown there. Read-only for files; it can press buttons and touch the screen.
//
//   GET  /dev/status                 the app's state as JSON (screen, playback, memory, frame times…)
//   GET  /dev/log?since=N&src=…      event and player log lines (src: event, mpv or all)
//   GET  /dev/screenshot?half=1      what's on screen, as PNG
//   POST /dev/input?keys=…           e.g. "A", "R:1500", "ZL+ZR", "wait:400", "tap:640/360",
//                                     "hold:640/360:800", "swipe:200/360/1000/360:300", "rstick:0/1:500"
//   POST /dev/open?path=…            opens a file as if picked in the browser
//   GET  /dev/mpv?get=prop | POST /dev/mpv?cmd=…   reads a player property or runs a player command
//   POST /dev/mpvlog?level=v         how much the player writes to the log
//   GET  /dev/ls?path=…  GET /dev/file?path=…&from=-65536   lists folders and reads files
//   POST /dev/back                   goes back to the file browser
//   POST /dev/exit                   closes the app (to start a new build: open it again)
#include <map>
#include <string>

class App;
struct RawInput;

class DevTools {
public:
    explicit DevTools(App& a) : app(a) {}
    // Main thread.
    void update();                        // follows the setting: starts or stops serving
    void applyInput(RawInput& raw, float dt);  // before the frame's input is read
    void endFrame(int fbw, int fbh);      // after drawing, before the frame is shown
    std::string newToken();
    double lastRequest() const;           // seconds since the last request (-1 never)
    int requests() const;

    // Any thread: a line for /dev/log.
    static void log(const char* src, const std::string& line);

private:
    bool serve(const std::string& path, const std::map<std::string, std::string>& q, const std::string& body,
               int& code, std::string& type, std::string& out);
    std::string status();
    App& app;
    bool installed_ = false, announced_ = false;
    std::string token_;
};
