#pragma once
// The document reader: PDF, e-books (EPUB, MOBI, FB2), comics (CBZ, CBR, CB7), XPS, SVG and Office
// files (DOCX, XLSX, PPTX), drawn by MuPDF.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <pthread.h>
#include <string>
#include <vector>

#include "app.hpp"

struct fz_context;
struct fz_document;

// Everything MuPDF does happens on this one thread, which owns the open document: a slow page never
// holds up the screen, and MuPDF needs no locking.
class DocEngine {
public:
    struct Link {
        float x0, y0, x1, y1;  // on the page, in points
        int page;              // where it leads; -1 for a web address
    };
    struct Heading {
        std::string title;
        int page, depth;
    };
    struct Request {  // a page's size, or its pixels (all of it, or the part x,y,w,h at `scale`)
        int page = 0;
        float scale = 0;  // pixels per point; 0 = only the size
        int x = 0, y = 0, w = 0, h = 0;
        bool tile = false, night = false;
        bool operator==(const Request& o) const {
            return page == o.page && scale == o.scale && x == o.x && y == o.y && w == o.w && h == o.h &&
                   tile == o.tile && night == o.night;
        }
    };
    struct Result {
        enum Kind { OPENED, LAID_OUT, PAGE, SEARCH } kind = PAGE;
        uint64_t doc = 0;  // which opening it belongs to
        // OPENED / LAID_OUT
        bool ok = false, needsPassword = false, reflowable = false;
        int pages = 0, keepPage = 0;
        std::string error, title;
        std::vector<Heading> outline;
        // PAGE
        Request req;
        float pageW = 0, pageH = 0;
        int pw = 0, ph = 0;  // pixels
        std::vector<unsigned char> rgba;
        std::vector<Link> links;
        bool failed = false;
        // SEARCH: the first page from where it started with a match (-1: none), and the matches there
        int found = -1;
        std::vector<float> boxes;  // x0, y0, x1, y1 per match, in points
    };

    ~DocEngine() { stop(); }
    // Reflowable documents (e-books) are laid out as pages of w x h points, text em points high.
    uint64_t open(const std::string& path, const std::string& password, float w, float h, float em);
    void layout(float w, float h, float em, int keepPage);
    void search(const std::string& text, int from, int dir);
    // What should be drawn next, most wanted first. Anything else still waiting is dropped, and a
    // page being drawn that's no longer wanted is abandoned.
    void want(const std::vector<Request>& list);
    void close();
    bool poll(Result& out);
    void stop();

private:
    struct Job {
        enum Kind { OPEN, LAYOUT, SEARCH, CLOSE } kind;
        uint64_t doc = 0;
        std::string path, text;
        float w = 0, h = 0, em = 0;
        int page = 0, dir = 1;
    };
    static void* main(void* self);
    void loop();
    void doOpen(const Job& j);
    void doLayout(const Job& j);
    void doSearch(const Job& j);
    void doRender(const Request& r, uint64_t doc);
    void readOutline(Result& res);
    void closeDoc();
    bool start();

    fz_context* ctx_ = nullptr;
    fz_document* doc_ = nullptr;
    uint64_t docGen_ = 0;  // engine thread: the document open now

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<Request> renders_;
    Request current_;
    bool rendering_ = false;
    void* cookie_ = nullptr;  // fz_cookie of the page being drawn
    std::deque<Result> results_;
    std::atomic<uint64_t> gen_{0};  // the latest opening asked for
    std::atomic<int> searchGen_{0};
    bool quit_ = false, started_ = false;
    pthread_t thread_{};
};

class ReaderScreen : public Screen {
public:
    explicit ReaderScreen(App& a) : Screen(a) {}
    ~ReaderScreen() override;
    void open(const std::string& path, const std::string& password = "");
    void frame() override;
    void exit();
    void saveProgress();
    const std::string& path() const { return path_; }
    int page() const { return page_; }
    int pages() const { return pages_; }

private:
    struct Tex {
        int img = 0;
        DocEngine::Request req;
        int pw = 0, ph = 0;
        float pageW = 0, pageH = 0;
        std::vector<DocEngine::Link> links;
        float unused = 0;
    };
    enum Panel { NONE, MENU, CONTENTS };
    struct Row {
        std::string label, detail;
        Icon ic = Icon::None;
        bool checked = false;
        int depth = 0;
        std::function<void()> fn;
    };

    void reopenWithPassword(bool wrong);
    void receive();
    void requestPages();
    void drawPages();
    void drawFixed();
    void drawBook();
    void drawPageTex(const Tex* base, const Tex* tile, Rect at, float alpha);
    void drawChrome();
    void drawPanel();
    void drawMessage();
    void handleInput();
    void handleFixedInput();
    void handleBookInput();
    void handlePanelInput();
    void openPanel(Panel p);
    void buildMenu();
    void buildContents();
    void goTo(int page, bool fromEnd = false);
    void step(int dir);
    void askPage();
    void askSearch();
    void findNext(int dir);
    void setZoom(float z, float cx, float cy);
    void clampView();
    void relayout();
    void setRotation(int r);
    void freeTextures();
    void rotateInput(bool into);
    bool tapLink(float x, float y);
    void showChrome(float secs = 3);
    std::string pageLabel() const;

    // The page area in view coordinates: the screen turned by rot_ quarter turns.
    float VW = 1280, VH = 720, dpr_ = 1;
    float fitScale(int page) const;  // view units per point at zoom 1
    bool sizeKnown(int page) const { return sizes_.count(page) != 0; }
    std::pair<float, float> sizeOf(int page) const;
    int stride() const { return reflowable_ && spread_ ? 2 : 1; }  // e-books show two pages side by side

    DocEngine engine_;
    uint64_t doc_ = 0;
    std::string path_, title_, password_, error_;
    bool opening_ = false, opened_ = false, reflowable_ = false, spread_ = false, laying_ = false;
    int pages_ = 0, page_ = 0;
    std::vector<DocEngine::Heading> outline_;
    std::map<int, std::pair<float, float>> sizes_;  // points
    float layoutW_ = 0, layoutH_ = 0, layoutEm_ = 0;
    int resumePage_ = -1;
    double resumeFrac_ = -1;

    std::map<int, Tex> base_;  // whole pages, by number
    Tex tile_;                 // the part in view, sharp, when zoomed in
    std::vector<DocEngine::Request> wanted_;

    float zoom_ = 1, ox_ = 0, oy_ = 0, vx_ = 0, vy_ = 0;
    int place_ = 0;  // a page just turned to: 0 top, 1 bottom (when its size arrives)
    float settle_ = 0;  // seconds since the view last moved
    float pinchBase_ = 1;
    bool panning_ = false, swiping_ = false;
    float slide_ = 0;  // page-turn animation, -1..1
    int rot_ = 0;

    bool chrome_ = true;
    float chromeTimer_ = 3, chromeA_ = 1;
    Panel panel_ = NONE;
    float panelA_ = 0;
    std::vector<Row> rows_;
    int sel_ = 0;
    ScrollView panelScroll_;
    std::string panelTitle_;

    std::string search_;
    int searchPage_ = -1;
    std::vector<float> hits_;
    bool searching_ = false;
    float saveTimer_ = 0;
    int savedPage_ = -1;
    float time_ = 0;
    bool scrubbing_ = false;  // dragging along the page bar
    int scrubTo_ = 0;
};
