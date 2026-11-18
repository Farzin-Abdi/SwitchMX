// The document reader (see reader.hpp). MuPDF draws pages on the DocEngine thread; this screen shows
// them as textures, with zoom, panning, contents, search, night mode and turning for upright use.

#include "reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#include "platform.hpp"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/ucdn.h>
}

#ifdef __SWITCH__
// newlib has no timegm; MuPDF reads PDF dates with it.
extern "C" time_t timegm(struct tm* tm) {
    long long y = tm->tm_year + 1900LL + tm->tm_mon / 12;
    int mon = tm->tm_mon % 12;
    if (mon < 0) {
        mon += 12;
        y--;
    }
    y -= mon <= 1;  // the year counted from March
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * ((mon + 10) % 12) + 2) / 5 + tm->tm_mday - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return (time_t)(days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec);
}

// newlib's getentropy needs this; MuPDF uses it for random IDs. libnx's generator supplies it.
extern "C" void randomGet(void* buf, size_t len);
struct _reent;
extern "C" int _getentropy_r(struct _reent*, void* buf, size_t len) {
    randomGet(buf, len);
    return 0;
}
#endif

namespace {

constexpr float kPt = 2.0f;           // view units per point for e-book pages
constexpr int kMaxPixels = 6 << 20;   // a whole page's pixels at most
constexpr int kMaxSide = 4096;

// Chinese, Japanese and Korean text that a document doesn't bring its own font for uses the
// console's (MuPDF's own CJK fonts are left out to keep the app small).
fz_font* cjkFont(fz_context* ctx, const char* name, int ordering, int serif) {
    (void)serif;
    const void* data;
    size_t size;
    if (!platform::systemCjkFont(ordering, data, size)) return nullptr;
    return fz_new_font_from_memory(ctx, name ? name : "SwitchCJK", (const unsigned char*)data, (int)size, 0, 0);
}

fz_font* fallbackFont(fz_context* ctx, int script, int language, int serif, int bold, int italic) {
    (void)bold;
    (void)italic;
    int ord;
    switch (script) {
        case UCDN_SCRIPT_HANGUL: ord = FZ_ADOBE_KOREA; break;
        case UCDN_SCRIPT_HIRAGANA:
        case UCDN_SCRIPT_KATAKANA: ord = FZ_ADOBE_JAPAN; break;
        case UCDN_SCRIPT_BOPOMOFO: ord = FZ_ADOBE_CNS; break;
        case UCDN_SCRIPT_HAN:
            ord = language == FZ_LANG_ja   ? FZ_ADOBE_JAPAN
                  : language == FZ_LANG_ko ? FZ_ADOBE_KOREA
                  : language == FZ_LANG_zh_Hant ? FZ_ADOBE_CNS
                                                : FZ_ADOBE_GB;
            break;
        default: return nullptr;
    }
    return cjkFont(ctx, "SwitchCJK", ord, serif);
}

std::string asciiDigits(const std::string& s) {
    // Persian and Arabic digits typed on the keyboard count too.
    std::string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xDB && i + 1 < s.size() && (unsigned char)s[i + 1] >= 0xB0 && (unsigned char)s[i + 1] <= 0xB9) {
            out += (char)('0' + ((unsigned char)s[i + 1] - 0xB0));
            i += 2;
        } else if (c == 0xD9 && i + 1 < s.size() && (unsigned char)s[i + 1] >= 0xA0 &&
                   (unsigned char)s[i + 1] <= 0xA9) {
            out += (char)('0' + ((unsigned char)s[i + 1] - 0xA0));
            i += 2;
        } else {
            out += (char)c;
            i++;
        }
    }
    return out;
}

}  // namespace

// ================================================================ DocEngine

bool DocEngine::start() {
    if (started_) return true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 << 20);  // laying out a book goes deep
    started_ = pthread_create(&thread_, &attr, main, this) == 0;
    pthread_attr_destroy(&attr);
    return started_;
}

void DocEngine::stop() {
    if (!started_) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
        if (cookie_) ((fz_cookie*)cookie_)->abort = 1;
    }
    searchGen_++;
    cv_.notify_all();
    pthread_join(thread_, nullptr);
    started_ = false;
}

void* DocEngine::main(void* self) {
    ((DocEngine*)self)->loop();
    return nullptr;
}

uint64_t DocEngine::open(const std::string& path, const std::string& password, float w, float h, float em) {
    uint64_t g = ++gen_;
    {
        std::lock_guard<std::mutex> lk(m_);
        jobs_.clear();
        renders_.clear();
        if (cookie_) ((fz_cookie*)cookie_)->abort = 1;
        Job j{Job::OPEN};
        j.doc = g;
        j.path = path;
        j.text = password;
        j.w = w;
        j.h = h;
        j.em = em;
        jobs_.push_back(j);
    }
    searchGen_++;
    if (!start()) {
        std::lock_guard<std::mutex> lk(m_);
        Result r;
        r.kind = Result::OPENED;
        r.doc = g;
        r.error = "The reader couldn't start";
        results_.push_back(r);
    }
    cv_.notify_all();
    return g;
}

void DocEngine::layout(float w, float h, float em, int keepPage) {
    std::lock_guard<std::mutex> lk(m_);
    renders_.clear();
    if (cookie_) ((fz_cookie*)cookie_)->abort = 1;
    Job j{Job::LAYOUT};
    j.doc = gen_;
    j.w = w;
    j.h = h;
    j.em = em;
    j.page = keepPage;
    jobs_.push_back(j);
    cv_.notify_all();
}

void DocEngine::search(const std::string& text, int from, int dir) {
    int g = ++searchGen_;
    std::lock_guard<std::mutex> lk(m_);
    Job j{Job::SEARCH};
    j.doc = gen_;
    j.text = text;
    j.page = from;
    j.dir = dir;
    j.w = (float)g;  // which search this is; a newer one makes it stop
    jobs_.push_back(j);
    cv_.notify_all();
}

void DocEngine::want(const std::vector<Request>& list) {
    std::lock_guard<std::mutex> lk(m_);
    renders_.clear();
    bool keepCurrent = false;
    for (auto& r : list) {
        if (rendering_ && r == current_) {
            keepCurrent = true;
            continue;
        }
        renders_.push_back(r);
    }
    if (rendering_ && !keepCurrent && cookie_) ((fz_cookie*)cookie_)->abort = 1;
    cv_.notify_all();
}

void DocEngine::close() {
    ++gen_;
    searchGen_++;
    std::lock_guard<std::mutex> lk(m_);
    jobs_.clear();
    renders_.clear();
    results_.clear();
    if (cookie_) ((fz_cookie*)cookie_)->abort = 1;
    Job j{Job::CLOSE};
    jobs_.push_back(j);
    cv_.notify_all();
}

bool DocEngine::poll(Result& out) {
    std::lock_guard<std::mutex> lk(m_);
    if (results_.empty()) return false;
    out = std::move(results_.front());
    results_.pop_front();
    return true;
}

void DocEngine::loop() {
    ctx_ = fz_new_context(nullptr, nullptr, 64 << 20);
    if (ctx_) {
        fz_try(ctx_) {
            fz_register_document_handlers(ctx_);
            fz_install_load_system_font_funcs(ctx_, nullptr, cjkFont, fallbackFont);
        }
        fz_catch(ctx_) {}
    }
    for (;;) {
        Job job{Job::CLOSE};
        Request req;
        int what = 0;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return quit_ || !jobs_.empty() || !renders_.empty(); });
            if (quit_) break;
            if (!jobs_.empty()) {
                job = jobs_.front();
                jobs_.pop_front();
                what = 1;
            } else {
                req = renders_.front();
                renders_.erase(renders_.begin());
                current_ = req;
                rendering_ = true;
                what = 2;
            }
        }
        if (what == 1) {
            if (!ctx_ && job.kind == Job::OPEN) {
                Result r;
                r.kind = Result::OPENED;
                r.doc = job.doc;
                r.error = "Not enough memory to open documents";
                std::lock_guard<std::mutex> lk(m_);
                results_.push_back(r);
                continue;
            }
            if (!ctx_) continue;
            switch (job.kind) {
                case Job::OPEN: doOpen(job); break;
                case Job::LAYOUT: doLayout(job); break;
                case Job::SEARCH: doSearch(job); break;
                case Job::CLOSE: closeDoc(); break;
            }
        } else {
            if (ctx_) doRender(req, docGen_);
            std::lock_guard<std::mutex> lk(m_);
            rendering_ = false;
            cookie_ = nullptr;
        }
    }
    closeDoc();
    if (ctx_) fz_drop_context(ctx_);
    ctx_ = nullptr;
}

void DocEngine::closeDoc() {
    if (doc_) fz_drop_document(ctx_, doc_);
    doc_ = nullptr;
    if (ctx_) fz_empty_store(ctx_);  // the cached fonts and pictures go too: video needs the memory
}

void DocEngine::readOutline(Result& res) {
    fz_outline* root = nullptr;
    fz_var(root);
    fz_try(ctx_) { root = fz_load_outline(ctx_, doc_); }
    fz_catch(ctx_) { root = nullptr; }
    if (!root) return;
    // Depth first, as the list shows it.
    std::vector<std::pair<fz_outline*, int>> stack = {{root, 0}};
    while (!stack.empty() && res.outline.size() < 5000) {
        auto [o, depth] = stack.back();
        stack.pop_back();
        if (!o) continue;
        if (o->next) stack.push_back({o->next, depth});
        if (o->down && depth < 8) stack.push_back({o->down, depth + 1});
        int page = -1;
        fz_location loc = o->page;
        fz_var(page);
        fz_try(ctx_) {
            if ((loc.chapter < 0 || loc.page < 0) && o->uri) {
                float x, y;
                loc = fz_resolve_link(ctx_, doc_, o->uri, &x, &y);
            }
            if (loc.chapter >= 0 && loc.page >= 0) page = fz_page_number_from_location(ctx_, doc_, loc);
        }
        fz_catch(ctx_) { page = -1; }
        std::string title = o->title ? o->title : "";
        while (!title.empty() && (title.back() == ' ' || title.back() == '\n' || title.back() == '\r')) title.pop_back();
        if (!title.empty()) res.outline.push_back({title, page, depth});
    }
    fz_drop_outline(ctx_, root);
}

void DocEngine::doOpen(const Job& j) {
    closeDoc();
    docGen_ = j.doc;
    Result res;
    res.kind = Result::OPENED;
    res.doc = j.doc;
    char err[300] = "";
    fz_document* doc = nullptr;
    fz_var(doc);
    fz_try(ctx_) { doc = fz_open_document(ctx_, j.path.c_str()); }
    fz_catch(ctx_) { snprintf(err, sizeof err, "%s", fz_caught_message(ctx_)); }
    if (!doc) {
        res.error = err;
        std::lock_guard<std::mutex> lk(m_);
        results_.push_back(std::move(res));
        return;
    }
    doc_ = doc;
    int locked = 0, reflow = 0, pages = -1;
    char title[400] = "";
    fz_var(locked);
    fz_var(reflow);
    fz_var(pages);
    fz_try(ctx_) {
        if (fz_needs_password(ctx_, doc_) && !fz_authenticate_password(ctx_, doc_, j.text.c_str())) locked = 1;
        if (!locked) {
            reflow = fz_is_document_reflowable(ctx_, doc_);
            if (reflow) {
                // The book's own styles, with some room around the text.
                fz_style_document(ctx_, doc_, 1, "@page{margin:1.6em 1.9em}");
                fz_layout_document(ctx_, doc_, j.w, j.h, j.em);
            }
            pages = fz_count_pages(ctx_, doc_);
            if (fz_lookup_metadata(ctx_, doc_, FZ_META_INFO_TITLE, title, sizeof title) <= 0) title[0] = 0;
        }
    }
    fz_catch(ctx_) {
        snprintf(err, sizeof err, "%s", fz_caught_message(ctx_));
        pages = -1;
    }
    if (locked) {
        res.needsPassword = true;
    } else if (pages <= 0) {
        res.error = pages == 0 ? "The document has no pages" : err;
        closeDoc();
    } else {
        res.ok = true;
        res.reflowable = reflow != 0;
        res.pages = pages;
        res.title = title;
        readOutline(res);
    }
    std::lock_guard<std::mutex> lk(m_);
    results_.push_back(std::move(res));
}

void DocEngine::doLayout(const Job& j) {
    if (!doc_ || j.doc != docGen_) return;
    Result res;
    res.kind = Result::LAID_OUT;
    res.doc = docGen_;
    int keep = j.page, pages = -1;
    fz_var(keep);
    fz_var(pages);
    fz_try(ctx_) {
        int count = fz_count_pages(ctx_, doc_);
        keep = std::max(0, std::min(keep, count - 1));
        fz_bookmark mark = fz_make_bookmark(ctx_, doc_, fz_location_from_page_number(ctx_, doc_, keep));
        fz_layout_document(ctx_, doc_, j.w, j.h, j.em);
        keep = fz_page_number_from_location(ctx_, doc_, fz_lookup_bookmark(ctx_, doc_, mark));
        pages = fz_count_pages(ctx_, doc_);
    }
    fz_catch(ctx_) { pages = -1; }
    res.ok = pages > 0;
    res.pages = std::max(pages, 0);
    res.keepPage = std::max(0, keep);
    res.reflowable = true;
    if (res.ok) readOutline(res);
    std::lock_guard<std::mutex> lk(m_);
    results_.push_back(std::move(res));
}

void DocEngine::doSearch(const Job& j) {
    if (!doc_ || j.doc != docGen_) return;
    int g = (int)j.w;
    Result res;
    res.kind = Result::SEARCH;
    res.doc = docGen_;
    int count = 0;
    fz_var(count);
    fz_try(ctx_) { count = fz_count_pages(ctx_, doc_); }
    fz_catch(ctx_) { count = 0; }
    constexpr int kMax = 64;
    fz_quad quads[kMax];
    int marks[kMax];
    for (int p = j.page; p >= 0 && p < count; p += j.dir) {
        if (searchGen_ != g || quit_) return;
        // Pages asked for in the meantime are still drawn: searching a long book takes a while.
        Request r;
        bool render = false;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!renders_.empty()) {
                r = renders_.front();
                renders_.erase(renders_.begin());
                current_ = r;
                rendering_ = render = true;
            }
        }
        if (render) {
            doRender(r, docGen_);
            std::lock_guard<std::mutex> lk(m_);
            rendering_ = false;
            cookie_ = nullptr;
        }
        int n = 0;
        fz_rect b = fz_empty_rect;
        fz_var(n);
        fz_var(b);
        fz_try(ctx_) {
            n = fz_search_page_number(ctx_, doc_, p, j.text.c_str(), marks, quads, kMax);
            if (n > 0) {
                fz_page* page = fz_load_page(ctx_, doc_, p);
                b = fz_bound_page(ctx_, page);
                fz_drop_page(ctx_, page);
            }
        }
        fz_catch(ctx_) { n = 0; }
        if (n > 0) {
            res.found = p;
            for (int i = 0; i < std::min(n, kMax); i++) {
                fz_rect q = fz_rect_from_quad(quads[i]);
                res.boxes.insert(res.boxes.end(), {q.x0 - b.x0, q.y0 - b.y0, q.x1 - b.x0, q.y1 - b.y0});
            }
            break;
        }
    }
    if (searchGen_ != g) return;
    std::lock_guard<std::mutex> lk(m_);
    results_.push_back(std::move(res));
}

void DocEngine::doRender(const Request& r, uint64_t doc) {
    if (!doc_) return;
    Result res;
    res.kind = Result::PAGE;
    res.doc = doc;
    res.req = r;
    fz_cookie cookie;
    memset(&cookie, 0, sizeof cookie);
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!rendering_ || !(current_ == r)) return;
        cookie_ = &cookie;
    }
    fz_page* page = nullptr;
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    fz_link* links = nullptr;
    fz_rect b = fz_empty_rect;
    int ok = 0;
    fz_var(page);
    fz_var(pix);
    fz_var(dev);
    fz_var(links);
    fz_var(b);
    fz_var(ok);
    fz_try(ctx_) {
        page = fz_load_page(ctx_, doc_, r.page);
        b = fz_bound_page(ctx_, page);
        if (r.scale > 0) {
            fz_irect box = r.tile ? fz_make_irect(r.x, r.y, r.x + r.w, r.y + r.h)
                                  : fz_make_irect(0, 0, (int)ceilf((b.x1 - b.x0) * r.scale),
                                                  (int)ceilf((b.y1 - b.y0) * r.scale));
            pix = fz_new_pixmap_with_bbox(ctx_, fz_device_rgb(ctx_), box, nullptr, 1);
            fz_clear_pixmap_with_value(ctx_, pix, 255);
            fz_matrix ctm = fz_concat(fz_translate(-b.x0, -b.y0), fz_scale(r.scale, r.scale));
            dev = fz_new_draw_device(ctx_, fz_identity, pix);
            fz_run_page(ctx_, page, dev, ctm, &cookie);
            fz_close_device(ctx_, dev);
            if (r.night) fz_invert_pixmap_luminance(ctx_, pix);
            if (!r.tile) links = fz_load_links(ctx_, page);
        }
        ok = 1;
    }
    fz_always(ctx_) {
        fz_drop_device(ctx_, dev);
        dev = nullptr;
    }
    fz_catch(ctx_) { ok = 0; }
    {
        std::lock_guard<std::mutex> lk(m_);
        cookie_ = nullptr;
    }
    if (cookie.abort) {  // no longer wanted
        fz_drop_pixmap(ctx_, pix);
        fz_drop_link(ctx_, links);
        fz_drop_page(ctx_, page);
        return;
    }
    res.pageW = b.x1 - b.x0;
    res.pageH = b.y1 - b.y0;
    res.failed = !ok;
    if (ok && pix) {
        int w = fz_pixmap_width(ctx_, pix), h = fz_pixmap_height(ctx_, pix);
        size_t stride = fz_pixmap_stride(ctx_, pix);
        const unsigned char* s = fz_pixmap_samples(ctx_, pix);
        res.pw = w;
        res.ph = h;
        try {
            res.rgba.resize((size_t)w * h * 4);
            for (int y = 0; y < h; y++) memcpy(&res.rgba[(size_t)y * w * 4], s + y * stride, (size_t)w * 4);
        } catch (...) {
            res.rgba.clear();
            res.failed = true;
        }
    }
    for (fz_link* l = links; l && res.links.size() < 500; l = l->next) {
        if (!l->uri) continue;
        int target = -1;
        fz_var(target);
        fz_try(ctx_) {
            if (!fz_is_external_link(ctx_, l->uri)) {
                float x, y;
                fz_location loc = fz_resolve_link(ctx_, doc_, l->uri, &x, &y);
                target = loc.page >= 0 ? fz_page_number_from_location(ctx_, doc_, loc) : -2;
            }
        }
        fz_catch(ctx_) { target = -2; }
        if (target == -2) continue;
        res.links.push_back({l->rect.x0 - b.x0, l->rect.y0 - b.y0, l->rect.x1 - b.x0, l->rect.y1 - b.y0, target});
    }
    fz_drop_pixmap(ctx_, pix);
    fz_drop_link(ctx_, links);
    fz_drop_page(ctx_, page);
    std::lock_guard<std::mutex> lk(m_);
    results_.push_back(std::move(res));
}

// ================================================================ ReaderScreen

void App::openDocument(const std::string& path) {
    if (!reader) return;
    reader->open(path);
}

ReaderScreen::~ReaderScreen() {
    engine_.stop();
    freeTextures();
}

void ReaderScreen::freeTextures() {
    for (auto& [p, t] : base_)
        if (t.img) nvgDeleteImage(app.vg, t.img);
    base_.clear();
    if (tile_.img) nvgDeleteImage(app.vg, tile_.img);
    tile_ = Tex{};
    wanted_.clear();
}

void ReaderScreen::open(const std::string& path, const std::string& password) {
    freeTextures();
    sizes_.clear();
    outline_.clear();
    hits_.clear();
    search_.clear();
    searchPage_ = -1;
    searching_ = laying_ = false;
    path_ = path;
    password_ = password;
    title_ = stripExtension(displayName(path));
    error_.clear();
    opening_ = true;
    opened_ = false;
    pages_ = page_ = 0;
    zoom_ = 1;
    vx_ = vy_ = 0;
    panel_ = NONE;
    panelA_ = 0;
    rot_ = app.cfg.s.readerRotate & 3;
    showChrome(3.5f);
    resumePage_ = -1;
    resumeFrac_ = -1;
    if (const HistoryItem* h = app.lib.find(path); h && h->duration > 0 && h->position >= 0) {
        resumePage_ = (int)h->position;
        resumeFrac_ = h->position / h->duration;
    }
    savedPage_ = -1;
    // The e-book page size follows the screen; worked out the same way relayout() does.
    UI& ui = app.ui;
    VW = (rot_ & 1) ? ui.H : ui.W;
    VH = (rot_ & 1) ? ui.W : ui.H;
    spread_ = VW > VH * 1.2f;
    layoutW_ = (spread_ ? VW / 2 : VW) / kPt;
    layoutH_ = VH / kPt;
    layoutEm_ = (float)app.cfg.s.readerFont;
    doc_ = engine_.open(path, password, layoutW_, layoutH_, layoutEm_);
    if (app.screen != this) app.switchTo(this);
    app.logEvent("reader: opening " + displayName(path));
}

void ReaderScreen::reopenWithPassword(bool wrong) {
    std::string pw;
    if (app.askText(wrong ? "Wrong password. Try again" : "This document is locked. Enter its password", "", pw) &&
        !pw.empty()) {
        open(path_, pw);
        return;
    }
    exit();
}

void ReaderScreen::saveProgress() {
    if (!opened_ || pages_ <= 0 || page_ == savedPage_) return;
    savedPage_ = page_;
    app.lib.updateHistory(path_, page_, pages_, true);
    app.lib.saveHistoryInBackground();
}

void ReaderScreen::exit() {
    saveProgress();
    engine_.close();
    freeTextures();
    opened_ = opening_ = false;
    panel_ = NONE;
    app.lastPlayed = path_;
    app.switchTo(app.home);
}

void ReaderScreen::showChrome(float secs) {
    chrome_ = true;
    chromeTimer_ = secs;
}

std::pair<float, float> ReaderScreen::sizeOf(int page) const {
    auto it = sizes_.find(page);
    if (it != sizes_.end()) return it->second;
    auto cur = sizes_.find(page_);  // not measured yet: probably like the page in view
    if (cur != sizes_.end()) return cur->second;
    return {612, 792};
}

float ReaderScreen::fitScale(int page) const {
    if (reflowable_) return kPt;
    auto [w, h] = sizeOf(page);
    if (w <= 0 || h <= 0) return 1;
    return app.cfg.s.readerFit == 1 ? VW / w : std::min(VW / w, VH / h);
}

std::string ReaderScreen::pageLabel() const {
    if (pages_ <= 0) return "";
    if (spread_ && reflowable_ && page_ + 1 < pages_)
        return std::to_string(page_ + 1) + "–" + std::to_string(page_ + 2) + " / " + std::to_string(pages_);
    return std::to_string(page_ + 1) + " / " + std::to_string(pages_);
}

void ReaderScreen::receive() {
    DocEngine::Result r;
    while (engine_.poll(r)) {
        if (r.doc != doc_) continue;
        switch (r.kind) {
            case DocEngine::Result::OPENED: {
                opening_ = false;
                if (r.needsPassword) {
                    reopenWithPassword(!password_.empty());
                    return;
                }
                if (!r.ok) {
                    error_ = r.error;
                    app.logEvent("reader: couldn't open " + displayName(path_) + ": " + r.error);
                    break;
                }
                opened_ = true;
                pages_ = r.pages;
                reflowable_ = r.reflowable;
                outline_ = std::move(r.outline);
                if (!r.title.empty() && r.title.size() < 200) title_ = r.title;
                if (reflowable_)
                    for (int i = 0; i < pages_; i++) sizes_[i] = {layoutW_, layoutH_};
                int start = 0;
                if (resumeFrac_ >= 0) {
                    const HistoryItem* h = app.lib.find(path_);
                    start = h && (int)h->duration == pages_ ? resumePage_ : (int)std::lround(resumeFrac_ * pages_);
                }
                page_ = -1;
                goTo(std::max(0, std::min(start, pages_ - 1)));
                slide_ = 0;
                saveProgress();
                break;
            }
            case DocEngine::Result::LAID_OUT: {
                laying_ = false;
                if (!r.ok) break;
                freeTextures();
                sizes_.clear();
                pages_ = r.pages;
                outline_ = std::move(r.outline);
                for (int i = 0; i < pages_; i++) sizes_[i] = {layoutW_, layoutH_};
                hits_.clear();
                searchPage_ = -1;
                page_ = -1;
                goTo(std::min(r.keepPage, pages_ - 1));
                slide_ = 0;
                break;
            }
            case DocEngine::Result::PAGE: {
                const auto& q = r.req;
                if (r.pageW > 0 && r.pageH > 0 && !reflowable_) sizes_[q.page] = {r.pageW, r.pageH};
                if (q.scale <= 0) break;
                Tex t;
                t.req = q;
                t.pw = r.pw;
                t.ph = r.ph;
                t.pageW = r.pageW;
                t.pageH = r.pageH;
                t.links = std::move(r.links);
                if (!r.failed && !r.rgba.empty())
                    t.img = nvgCreateImageRGBA(app.vg, r.pw, r.ph, 0, r.rgba.data());
                if (q.tile) {
                    if (tile_.img) nvgDeleteImage(app.vg, tile_.img);
                    tile_ = std::move(t);
                } else {
                    auto it = base_.find(q.page);
                    // A failed page keeps what was there and isn't asked for again at this size.
                    if (it != base_.end() && it->second.img && !t.img) {
                        it->second.req = q;
                        break;
                    }
                    if (it != base_.end() && it->second.img) nvgDeleteImage(app.vg, it->second.img);
                    base_[q.page] = std::move(t);
                }
                break;
            }
            case DocEngine::Result::SEARCH: {
                searching_ = false;
                if (r.found < 0) {
                    app.ui.toast(searchPage_ < 0 ? "Not found" : "No more matches", Icon::Search);
                    break;
                }
                searchPage_ = r.found;
                hits_ = std::move(r.boxes);
                int target = r.found;
                if (reflowable_ && spread_) target -= target % 2;
                if (target != page_ && !(reflowable_ && spread_ && r.found == page_ + 1)) goTo(target);
                // Zoomed in or reading by width: bring the first match into view.
                if (!reflowable_ && hits_.size() >= 4) {
                    float vs = fitScale(page_) * zoom_;
                    oy_ = VH * 0.35f - hits_[1] * vs;
                    ox_ = VW * 0.5f - (hits_[0] + hits_[2]) * 0.5f * vs;
                    place_ = -1;
                    clampView();
                }
                break;
            }
        }
    }
}

void ReaderScreen::goTo(int p, bool fromEnd) {
    if (pages_ <= 0) return;
    p = std::max(0, std::min(p, pages_ - 1));
    if (reflowable_ && spread_) p -= p % 2;
    if (p == page_) return;
    slide_ = page_ < 0 ? 0 : (p > page_ ? 1.0f : -1.0f);
    page_ = p;
    place_ = fromEnd ? 1 : 0;
    vx_ = vy_ = 0;
    settle_ = 0;
    if (searchPage_ >= 0 && searchPage_ != p && !(reflowable_ && spread_ && searchPage_ == p + 1)) hits_.clear();
    saveTimer_ = 1.5f;
}

void ReaderScreen::step(int dir) {
    int next = page_ + dir * stride();
    if (next < 0 || next >= pages_) {
        app.ui.toast(next < 0 ? "First page" : "Last page", Icon::Book);
        return;
    }
    goTo(next);
}

void ReaderScreen::clampView() {
    if (reflowable_) return;
    auto [pw, ph] = sizeOf(page_);
    float vs = fitScale(page_) * zoom_, dw = pw * vs, dh = ph * vs;
    if (dw <= VW + 0.5f) ox_ = (VW - dw) / 2;
    else ox_ = std::min(0.0f, std::max(ox_, VW - dw));
    if (dh <= VH + 0.5f) oy_ = (VH - dh) / 2;
    else oy_ = std::min(0.0f, std::max(oy_, VH - dh));
}

void ReaderScreen::setZoom(float z, float cx, float cy) {
    z = std::max(1.0f, std::min(z, 8.0f));
    float fs = fitScale(page_);
    float px = (cx - ox_) / (fs * zoom_), py = (cy - oy_) / (fs * zoom_);
    zoom_ = z;
    ox_ = cx - px * fs * zoom_;
    oy_ = cy - py * fs * zoom_;
    clampView();
    settle_ = 0;
}

void ReaderScreen::relayout() {
    if (!reflowable_ || !opened_) return;
    spread_ = VW > VH * 1.2f;
    float w = (spread_ ? VW / 2 : VW) / kPt, h = VH / kPt, em = (float)app.cfg.s.readerFont;
    if (w == layoutW_ && h == layoutH_ && em == layoutEm_) return;
    layoutW_ = w;
    layoutH_ = h;
    layoutEm_ = em;
    laying_ = true;
    freeTextures();
    engine_.layout(w, h, em, std::max(page_, 0));
}

void ReaderScreen::setRotation(int r) {
    rot_ = r & 3;
    app.cfg.s.readerRotate = rot_;
    app.saveSoon();
    UI& ui = app.ui;
    VW = (rot_ & 1) ? ui.H : ui.W;
    VH = (rot_ & 1) ? ui.W : ui.H;
    zoom_ = 1;
    place_ = 0;
    if (tile_.img) nvgDeleteImage(app.vg, tile_.img);
    tile_ = Tex{};
    relayout();
    clampView();
}

// Wanted pages, nearest first; old ones are let go.
void ReaderScreen::requestPages() {
    if (!opened_ || laying_) return;
    std::vector<DocEngine::Request> list;
    bool night = app.cfg.s.readerNight != 0;
    auto wantBase = [&](int p, float zoom) {
        if (p < 0 || p >= pages_) return;
        if (!sizeKnown(p)) {
            DocEngine::Request m;
            m.page = p;
            list.push_back(m);
            return;
        }
        auto [w, h] = sizeOf(p);
        float s = fitScale(p) * zoom * dpr_;
        float px = w * s * h * s;
        if (px > kMaxPixels) s *= std::sqrt(kMaxPixels / px);
        s = std::min(s, kMaxSide / std::max(w, h));
        s = std::floor(s * 1000) / 1000;
        auto it = base_.find(p);
        if (it != base_.end() && std::fabs(it->second.req.scale - s) < s * 0.01f && it->second.req.night == night) return;
        DocEngine::Request q;
        q.page = p;
        q.scale = s;
        q.night = night;
        list.push_back(q);
    };
    if (reflowable_) {
        for (int i = 0; i < stride(); i++) wantBase(page_ + i, 1);
        for (int i = 0; i < stride(); i++) wantBase(page_ + stride() + i, 1);
        for (int i = 0; i < stride(); i++) wantBase(page_ - stride() + i, 1);
    } else {
        wantBase(page_, zoom_ > 1.4f ? 2.0f : 1.0f);
        // The sharp part in view, once the view has settled.
        if (zoom_ > 1.05f && settle_ > 0.15f && sizeKnown(page_)) {
            auto [w, h] = sizeOf(page_);
            float vs = fitScale(page_) * zoom_, s = vs * dpr_;
            float x0 = std::max(0.0f, -ox_), y0 = std::max(0.0f, -oy_);
            float x1 = std::min(w * vs, VW - ox_), y1 = std::min(h * vs, VH - oy_);
            DocEngine::Request t;
            t.page = page_;
            t.scale = std::floor(s * 1000) / 1000;
            t.tile = true;
            t.night = night;
            t.x = (int)std::floor(x0 * dpr_);
            t.y = (int)std::floor(y0 * dpr_);
            t.w = (int)std::ceil((x1 - x0) * dpr_);
            t.h = (int)std::ceil((y1 - y0) * dpr_);
            auto b = base_.find(page_);
            bool baseSharp = b != base_.end() && b->second.req.scale >= t.scale * 0.98f;
            if (t.w > 0 && t.h > 0 && !baseSharp && !(tile_.req == t)) list.push_back(t);
        }
        wantBase(page_ + 1, 1);
        wantBase(page_ - 1, 1);
    }
    if (!(list == wanted_)) {
        wanted_ = list;
        engine_.want(list);
    }
    // Pages well away from this one are let go.
    int keepLo = page_ - 2 * stride(), keepHi = page_ + 3 * stride();
    for (auto it = base_.begin(); it != base_.end();) {
        if (it->first < keepLo || it->first > keepHi) {
            if (it->second.img) nvgDeleteImage(app.vg, it->second.img);
            it = base_.erase(it);
        } else {
            ++it;
        }
    }
}

void ReaderScreen::drawPageTex(const Tex* base, const Tex* tile, Rect at, float alpha) {
    NVGcontext* vg = app.vg;
    bool night = app.cfg.s.readerNight != 0;
    // The paper, under the picture (and on its own while the picture is on its way).
    nvgBeginPath(vg);
    nvgRect(vg, at.x, at.y, at.w, at.h);
    nvgFillColor(vg, night ? nvgRGBAf(0.07f, 0.07f, 0.08f, alpha) : nvgRGBAf(1, 1, 1, alpha));
    nvgFill(vg);
    if (base && base->img) {
        nvgBeginPath(vg);
        nvgRect(vg, at.x, at.y, at.w, at.h);
        nvgFillPaint(vg, nvgImagePattern(vg, at.x, at.y, at.w, at.h, 0, base->img, alpha));
        nvgFill(vg);
    } else {
        app.ui.spinner(at.cx(), at.cy(), 18, night ? hex(0xFFFFFF, 0.6f * alpha) : hex(0x000000, 0.45f * alpha));
    }
    if (tile && tile->img && base && tile->req.scale > base->req.scale && tile->pageW > 0) {
        float k = at.w / (tile->pageW * tile->req.scale);  // view units per tile pixel
        Rect r{at.x + tile->req.x * k, at.y + tile->req.y * k, tile->pw * k, tile->ph * k};
        nvgBeginPath(vg);
        nvgRect(vg, r.x, r.y, r.w, r.h);
        nvgFillPaint(vg, nvgImagePattern(vg, r.x, r.y, r.w, r.h, 0, tile->img, alpha));
        nvgFill(vg);
    }
}

void ReaderScreen::drawFixed() {
    UI& ui = app.ui;
    if (!sizeKnown(page_)) {
        ui.spinner(VW / 2, VH / 2, 22, hex(0xFFFFFF, 0.7f));
        return;
    }
    auto [pw, ph] = sizeOf(page_);
    float vs = fitScale(page_) * zoom_, dw = pw * vs, dh = ph * vs;
    if (place_ >= 0) {  // a page just turned to: start at its top (or bottom, scrolling back)
        ox_ = place_ == 1 ? VW - dw : 0;
        oy_ = place_ == 1 ? VH - dh : 0;
        place_ = -1;
    }
    clampView();
    float slideX = slide_ * VW * 0.18f, a = 1 - std::fabs(slide_) * 0.7f;
    Rect at{ox_ + slideX, oy_, dw, dh};
    ui.shadow(at, 0, 24, 0.5f, 4);
    auto b = base_.find(page_);
    const Tex* base = b != base_.end() ? &b->second : nullptr;
    bool night = app.cfg.s.readerNight != 0;
    const Tex* tile = tile_.req.page == page_ && tile_.req.night == night ? &tile_ : nullptr;
    drawPageTex(base, tile, at, a);
    if (searchPage_ == page_)
        for (size_t i = 0; i + 3 < hits_.size(); i += 4) {
            Rect h{at.x + hits_[i] * vs - 2, at.y + hits_[i + 1] * vs - 1, (hits_[i + 2] - hits_[i]) * vs + 4,
                   (hits_[i + 3] - hits_[i + 1]) * vs + 2};
            ui.fillRect(h, withAlpha(hex(0xFFC400), 0.38f * a), 3);
        }
}

void ReaderScreen::drawBook() {
    UI& ui = app.ui;
    float colW = spread_ ? VW / 2 : VW;
    float slideX = slide_ * VW * 0.12f, a = 1 - std::fabs(slide_) * 0.7f;
    bool night = app.cfg.s.readerNight != 0;
    ui.fillRect({0, 0, VW, VH}, night ? hex(0x111214) : hex(0xFFFFFF));
    for (int i = 0; i < stride(); i++) {
        int p = page_ + i;
        Rect at{i * colW + slideX, 0, colW, VH};
        if (p >= pages_) break;
        auto b = base_.find(p);
        drawPageTex(b != base_.end() ? &b->second : nullptr, nullptr, at, a);
        if (searchPage_ == p)
            for (size_t k = 0; k + 3 < hits_.size(); k += 4)
                ui.fillRect({at.x + hits_[k] * kPt - 2, at.y + hits_[k + 1] * kPt - 1,
                             (hits_[k + 2] - hits_[k]) * kPt + 4, (hits_[k + 3] - hits_[k + 1]) * kPt + 2},
                            withAlpha(hex(0xFFC400), 0.38f * a), 3);
    }
    if (spread_) {  // the fold between the two pages
        nvgBeginPath(app.vg);
        nvgRect(app.vg, VW / 2 - 18, 0, 36, VH);
        nvgFillPaint(app.vg, nvgBoxGradient(app.vg, VW / 2 - 1, -40, 2, VH + 80, 1, 22,
                                            night ? hex(0x000000, 0.55f) : hex(0x000000, 0.12f), hex(0, 0)));
        nvgFill(app.vg);
    }
}

void ReaderScreen::drawPages() {
    if (laying_) {
        app.ui.spinner(VW / 2, VH / 2, 22, hex(0xFFFFFF, 0.7f));
        return;
    }
    if (reflowable_) drawBook();
    else drawFixed();
}

bool ReaderScreen::tapLink(float x, float y) {
    auto hit = [&](const Tex& t, Rect at, float vs) {
        for (auto& l : t.links) {
            Rect r{at.x + l.x0 * vs, at.y + l.y0 * vs, (l.x1 - l.x0) * vs, (l.y1 - l.y0) * vs};
            if (!r.inset(-6).contains(x, y)) continue;
            if (l.page >= 0) goTo(l.page);
            else app.ui.toast("Web links can't be opened from here", Icon::Link);
            return true;
        }
        return false;
    };
    if (reflowable_) {
        float colW = spread_ ? VW / 2 : VW;
        for (int i = 0; i < stride(); i++) {
            auto b = base_.find(page_ + i);
            if (b != base_.end() && hit(b->second, {i * colW, 0, colW, VH}, kPt)) return true;
        }
        return false;
    }
    auto b = base_.find(page_);
    if (b == base_.end()) return false;
    auto [pw, ph] = sizeOf(page_);
    float vs = fitScale(page_) * zoom_;
    return hit(b->second, {ox_, oy_, pw * vs, ph * vs}, vs);
}

// ---------------------------------------------------------------- chrome, menus

void ReaderScreen::drawChrome() {
    UI& ui = app.ui;
    chromeA_ = ui.smooth(uid("reader.chrome"), chrome_ ? 1.0f : 0.0f, 12);
    if (chromeA_ < 0.01f) return;
    float a = chromeA_;
    ui.pushAlpha(a);
    bool active = a > 0.6f && panel_ == NONE;
    // Top: back, the title, and the tools.
    Rect top{0, 0, VW, 68};
    nvgBeginPath(app.vg);
    nvgRect(app.vg, top.x, top.y, top.w, top.h + 30);
    nvgFillPaint(app.vg, nvgLinearGradient(app.vg, 0, 0, 0, top.h + 30, hex(0x000000, 0.82f), hex(0x000000, 0)));
    nvgFill(app.vg);
    auto iconBtn = [&](float cx, Icon ic, bool on = false) {
        Rect r{cx - 24, top.cy() - 24, 48, 48};
        if (ui.pressedVisual(r) || on) ui.fillRect(r, hex(0xFFFFFF, on ? 0.18f : 0.14f), 24);
        ui.icon(ic, cx, top.cy(), 24, hex(0xFFFFFF));
        return active && ui.tapped(r);
    };
    if (iconBtn(36, Icon::Back)) {
        ui.popAlpha();
        exit();
        return;
    }
    float right = VW - 30;
    if (iconBtn(right, Icon::Sliders)) openPanel(MENU);
    right -= 56;
    if (!outline_.empty()) {
        if (iconBtn(right, Icon::List)) openPanel(CONTENTS);
        right -= 56;
    }
    if (iconBtn(right, Icon::Search, !search_.empty())) askSearch();
    right -= 56;
    if (iconBtn(right, Icon::Moon, app.cfg.s.readerNight != 0)) {
        app.cfg.s.readerNight ^= 1;
        app.saveSoon();
    }
    right -= 40;
    ui.text(72, top.cy(), ui.ellipsize(title_, std::max(40.0f, right - 72), 19, true), 19, hex(0xFFFFFF), LEFT, true);

    // Bottom: where you are, and a bar to jump anywhere.
    Rect bot{0, VH - 64, VW, 64};
    float fade = ui.in->usingButtons ? 96 : 34;  // room for the button hints too
    nvgBeginPath(app.vg);
    nvgRect(app.vg, 0, bot.y - fade, VW, bot.h + fade);
    nvgFillPaint(app.vg, nvgLinearGradient(app.vg, 0, bot.y - fade, 0, bot.y + 10, hex(0x000000, 0),
                                           hex(0x000000, 0.86f)));
    nvgFill(app.vg);
    std::string label = searching_ ? tr("Searching…") : pageLabel();
    float lw = ui.textWidth(label, 17, true);
    ui.text(24, bot.cy(), label, 17, hex(0xFFFFFF), LEFT, true);
    float tx = 24 + std::max(lw, 90.0f) + 22;
    float tw = VW - tx - 30;
    // Search: step between matches.
    if (!search_.empty() && !searching_) {
        float bx = VW - 34;
        auto small = [&](float cx, Icon ic) {
            Rect r{cx - 20, bot.cy() - 20, 40, 40};
            if (ui.pressedVisual(r)) ui.fillRect(r, hex(0xFFFFFF, 0.14f), 20);
            ui.icon(ic, cx, bot.cy(), 20, hex(0xFFFFFF));
            return active && ui.tapped(r);
        };
        if (small(bx, Icon::Close)) {
            search_.clear();
            hits_.clear();
            searchPage_ = -1;
        }
        if (small(bx - 46, Icon::ChevronRight)) findNext(1);
        if (small(bx - 92, Icon::ChevronLeft)) findNext(-1);
        tw -= 140;
    }
    if (pages_ > 1 && tw > 60) {
        Rect track{tx, bot.cy() - 3, tw, 6};
        Rect hit = track.inset(-6, -22);
        if (active && ui.in->touchBegan && hit.contains(ui.in->touchX, ui.in->touchY)) scrubbing_ = true;
        float frac = (float)page_ / (pages_ - 1);
        if (scrubbing_) {
            frac = std::max(0.0f, std::min(1.0f, (ui.in->touchX - track.x) / track.w));
            scrubTo_ = (int)std::lround(frac * (pages_ - 1));
            if (!ui.in->touching) {
                scrubbing_ = false;
                goTo(scrubTo_);
            } else {
                std::string bubble = std::to_string(scrubTo_ + 1);
                float bw = ui.textWidth(bubble, 17, true) + 26;
                Rect br{track.x + frac * track.w - bw / 2, track.y - 50, bw, 34};
                ui.fillRect(br, hex(0x000000, 0.85f), 17);
                ui.text(br.cx(), br.cy(), bubble, 17, hex(0xFFFFFF), CENTER, true);
            }
            ui.in->consumeTap();
        }
        ui.fillRect(track, hex(0xFFFFFF, 0.25f), 3);
        ui.fillRect({track.x, track.y, track.w * frac, track.h}, ui.t.accent, 3);
        nvgBeginPath(app.vg);
        nvgCircle(app.vg, track.x + track.w * frac, track.cy(), scrubbing_ ? 11 : 8);
        nvgFillColor(app.vg, hex(0xFFFFFF));
        nvgFill(app.vg);
    }
    // Controller buttons, above the bar.
    if (ui.in->usingButtons) {
        float x = 22, cy = bot.y - 24;
        x += ui.hint(x, cy, BTN_R, reflowable_ ? "Page" : "Next") + 22;
        x += ui.hint(x, cy, BTN_ZR, reflowable_ ? "Text size" : "Zoom") + 22;
        x += ui.hint(x, cy, BTN_X, "Menu") + 22;
        if (!outline_.empty() && x < VW - 200) x += ui.hint(x, cy, BTN_Y, "Contents") + 22;
        if (x < VW - 200) x += ui.hint(x, cy, BTN_PLUS, "Go to page") + 22;
        if (x < VW - 160) ui.hint(x, cy, BTN_MINUS, "Turn");
    }
    ui.popAlpha();
}

void ReaderScreen::openPanel(Panel p) {
    panel_ = p;
    if (p == MENU) buildMenu();
    else buildContents();
    panelScroll_.reset();
    panelScroll_.id = uid("reader.panel");
    panelScroll_.ensureVisible(sel_ * 58.0f, 58);
    panelScroll_.offset = std::max(0.0f, sel_ * 58.0f - VH * 0.35f);
}

void ReaderScreen::buildContents() {
    rows_.clear();
    panelTitle_ = "Contents";
    sel_ = 0;
    for (size_t i = 0; i < outline_.size(); i++) {
        auto& h = outline_[i];
        Row r;
        r.label = h.title;
        r.depth = h.depth;
        r.detail = h.page >= 0 ? std::to_string(h.page + 1) : "";
        if (h.page >= 0 && h.page <= page_ + stride() - 1) sel_ = (int)i;  // the part being read
        int target = h.page;
        r.fn = [this, target] {
            if (target >= 0) goTo(target);
            panel_ = NONE;
        };
        rows_.push_back(std::move(r));
    }
}

void ReaderScreen::buildMenu() {
    rows_.clear();
    panelTitle_ = "Reading options";
    auto add = [&](const std::string& label, Icon ic, std::function<void()> fn, bool checked = false,
                   const std::string& detail = "") {
        Row r;
        r.label = label;
        r.ic = ic;
        r.fn = std::move(fn);
        r.checked = checked;
        r.detail = detail;
        rows_.push_back(std::move(r));
    };
    if (!outline_.empty()) add("Contents", Icon::List, [this] { openPanel(CONTENTS); });
    add("Go to page…", Icon::Book, [this] {
        panel_ = NONE;
        askPage();
    });
    add("Search…", Icon::Search, [this] {
        panel_ = NONE;
        askSearch();
    });
    if (!search_.empty()) {
        add("Next match", Icon::ChevronRight, [this] {
            panel_ = NONE;
            findNext(1);
        });
        add("Previous match", Icon::ChevronLeft, [this] {
            panel_ = NONE;
            findNext(-1);
        });
    }
    if (reflowable_) {
        std::string size = std::to_string(app.cfg.s.readerFont) + " pt";
        add("Larger text", Icon::Plus, [this] {
            app.cfg.s.readerFont = std::min(32, app.cfg.s.readerFont + 1);
            app.saveSoon();
            relayout();
            buildMenu();
        }, false, size);
        add("Smaller text", Icon::Minus, [this] {
            app.cfg.s.readerFont = std::max(6, app.cfg.s.readerFont - 1);
            app.saveSoon();
            relayout();
            buildMenu();
        }, false, size);
    } else {
        add("Fit the whole page", Icon::Expand, [this] {
            app.cfg.s.readerFit = 0;
            app.saveSoon();
            zoom_ = 1;
            place_ = 0;
            buildMenu();
        }, app.cfg.s.readerFit == 0);
        add("Fit the page width", Icon::Expand, [this] {
            app.cfg.s.readerFit = 1;
            app.saveSoon();
            zoom_ = 1;
            place_ = 0;
            buildMenu();
        }, app.cfg.s.readerFit == 1);
    }
    add("Night mode", Icon::Moon, [this] {
        app.cfg.s.readerNight ^= 1;
        app.saveSoon();
        buildMenu();
    }, app.cfg.s.readerNight != 0);
    static const char* turns[] = {"Normal", "Turned right", "Upside down", "Turned left"};
    add("Turn the page", Icon::Rotate, [this] {
        setRotation(rot_ + 1);
        buildMenu();
    }, false, tr(turns[rot_ & 3]));
    if (sel_ >= (int)rows_.size()) sel_ = 0;
}

void ReaderScreen::drawPanel() {
    UI& ui = app.ui;
    panelA_ = ui.smooth(uid("reader.panelA"), panel_ != NONE ? 1.0f : 0.0f, 14);
    if (panelA_ < 0.01f) return;
    ui.pushAlpha(panelA_);
    ui.fillRect({0, 0, VW, VH}, hex(0x000000, 0.45f));
    float pw = std::min(500.0f, VW * 0.92f);
    Rect sheet{VW - pw * panelA_, 0, pw, VH};
    ui.fillRect(sheet, mix(ui.t.bg, hex(0), 0.15f), 0);
    ui.text(sheet.x + 26, 40, panelTitle_, 22, ui.t.text, LEFT, true);
    Rect list{sheet.x, 76, pw, VH - 76};
    const float rowH = 58;
    panelScroll_.update(ui, list, rows_.size() * rowH + 12);
    panelScroll_.begin(ui);
    int run = -1;
    for (size_t i = 0; i < rows_.size(); i++) {
        Rect r{list.x + 10, list.y + i * rowH - panelScroll_.offset, list.w - 20, rowH};
        if (r.bottom() < list.y || r.y > list.bottom()) continue;
        auto& row = rows_[i];
        bool sel = (int)i == sel_;
        if (sel) ui.fillRect(r.inset(0, 3), withAlpha(ui.t.accent, 0.22f), 12);
        else if (ui.pressedVisual(r)) ui.fillRect(r.inset(0, 3), withAlpha(ui.t.text, 0.08f), 12);
        float x = r.x + 18 + row.depth * 20;
        if (row.ic != Icon::None) {
            ui.icon(row.ic, x + 11, r.cy(), 22, sel ? ui.t.accent : ui.t.textDim);
            x += 38;
        }
        float dw = row.detail.empty() ? 0 : ui.textWidth(row.detail, 16) + 14;
        float cw = row.checked ? 34 : 0;
        ui.text(x, r.cy(), ui.ellipsize(row.label, r.right() - x - dw - cw - 14, 18, sel), 18,
                row.depth > 0 ? ui.t.textDim : ui.t.text, LEFT, sel);
        if (!row.detail.empty()) ui.text(r.right() - 14 - cw, r.cy(), row.detail, 16, ui.t.textFaint, RIGHT);
        if (row.checked) ui.icon(Icon::Check, r.right() - 24, r.cy(), 20, ui.t.accent);
        if (ui.tapped(r)) {
            sel_ = (int)i;
            run = (int)i;
        }
    }
    panelScroll_.end(ui);
    panelScroll_.drawScrollbar(ui);
    // A tap outside the sheet closes it.
    if (ui.in->tap && !sheet.contains(ui.in->tapX, ui.in->tapY)) {
        ui.in->consumeTap();
        panel_ = NONE;
    }
    ui.popAlpha();
    if (run >= 0 && run < (int)rows_.size() && rows_[run].fn) {
        auto fn = rows_[run].fn;
        fn();
    }
}

void ReaderScreen::handlePanelInput() {
    UI& ui = app.ui;
    Input& in = *ui.in;
    if (ui.back() || in.down(BTN_X) || in.down(BTN_Y)) {
        panel_ = NONE;
        return;
    }
    int n = (int)rows_.size();
    if (n == 0) return;
    if (in.rep(BTN_DOWN)) sel_ = std::min(n - 1, sel_ + 1);
    if (in.rep(BTN_UP)) sel_ = std::max(0, sel_ - 1);
    if (in.rep(BTN_R)) sel_ = std::min(n - 1, sel_ + 8);
    if (in.rep(BTN_L)) sel_ = std::max(0, sel_ - 8);
    if (in.rep(BTN_DOWN | BTN_UP | BTN_L | BTN_R)) panelScroll_.ensureVisible(sel_ * 58.0f, 58, 70);
    if (ui.confirm() && rows_[sel_].fn) {
        auto fn = rows_[sel_].fn;
        fn();
    }
}

void ReaderScreen::askPage() {
    std::string s;
    if (!app.askText(trf("Go to page (1 to {0})", {std::to_string(pages_)}), std::to_string(page_ + 1), s)) return;
    int n = atoi(asciiDigits(s).c_str());
    if (n < 1 || n > pages_) {
        app.ui.toast(trf("There's no page {0}", {s}), Icon::Book);
        return;
    }
    goTo(n - 1);
}

void ReaderScreen::askSearch() {
    std::string s;
    if (!app.askText("Search in the document", search_, s)) return;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.empty()) return;
    search_ = s;
    hits_.clear();
    searchPage_ = -1;
    searching_ = true;
    showChrome(30);
    engine_.search(s, page_, 1);
}

void ReaderScreen::findNext(int dir) {
    if (search_.empty()) return;
    int from = searchPage_ >= 0 ? searchPage_ + dir : page_;
    if (from < 0 || from >= pages_) {
        app.ui.toast("No more matches", Icon::Search);
        return;
    }
    searching_ = true;
    showChrome(30);
    engine_.search(search_, from, dir);
}

// ---------------------------------------------------------------- input

// Turned for reading upright: what the screen gets is turned the other way, so touches and the
// D-pad match what's on screen.
void ReaderScreen::rotateInput(bool into) {
    if (!rot_) return;
    Input& in = *app.ui.in;
    float W = app.ui.W, H = app.ui.H;
    int k = into ? (4 - rot_) & 3 : rot_;
    auto turn = [k](float& x, float& y) {
        for (int i = 0; i < k; i++) {
            float t = x;
            x = -y;
            y = t;
        }
    };
    auto pt = [&](float& x, float& y) {
        if (into) {
            x -= W / 2;
            y -= H / 2;
            turn(x, y);
            x += VW / 2;
            y += VH / 2;
        } else {
            x -= VW / 2;
            y -= VH / 2;
            turn(x, y);
            x += W / 2;
            y += H / 2;
        }
    };
    pt(in.touchX, in.touchY);
    pt(in.touchStartX, in.touchStartY);
    pt(in.tapX, in.tapY);
    pt(in.confirmedX, in.confirmedY);
    pt(in.pan.startX, in.pan.startY);
    pt(in.pan.x, in.pan.y);
    pt(in.pinch.cx, in.pinch.cy);
    turn(in.pan.dx, in.pan.dy);
    turn(in.pan.vx, in.pan.vy);
    float rx = in.rx, ry = -in.ry;  // the stick's y points up
    turn(rx, ry);
    in.rx = rx;
    in.ry = -ry;
    auto dirs = [&](uint32_t& bits) {
        uint32_t d = bits & BTN_DIRS, out = 0;
        struct {
            uint32_t b;
            float x, y;
        } map[] = {{BTN_RIGHT, 1, 0}, {BTN_LEFT, -1, 0}, {BTN_UP, 0, -1}, {BTN_DOWN, 0, 1}};
        for (auto& m : map) {
            if (!(d & m.b)) continue;
            float x = m.x, y = m.y;
            turn(x, y);
            for (auto& n : map)
                if (std::fabs(n.x - x) < 0.5f && std::fabs(n.y - y) < 0.5f) out |= n.b;
        }
        bits = (bits & ~BTN_DIRS) | out;
    };
    dirs(in.held);
    dirs(in.pressed);
    dirs(in.released);
    dirs(in.repeat);
}

void ReaderScreen::handleInput() {
    UI& ui = app.ui;
    Input& in = *ui.in;
    if (panel_ != NONE) {
        handlePanelInput();
        return;
    }
    if (!opened_) {
        if (ui.back()) exit();
        return;
    }
    if (ui.back()) {
        exit();
        return;
    }
    if (ui.confirm()) {
        if (chrome_) chrome_ = false;
        else showChrome(4);
    }
    if (in.down(BTN_X)) openPanel(MENU);
    if (in.down(BTN_Y)) openPanel(outline_.empty() ? MENU : CONTENTS);
    if (in.down(BTN_PLUS)) askPage();
    if (in.down(BTN_MINUS)) setRotation(rot_ + 1);
    if (in.wheel != 0 || in.held || in.touching) settle_ = 0;
    if (laying_) return;
    if (reflowable_) handleBookInput();
    else handleFixedInput();
}

void ReaderScreen::handleBookInput() {
    UI& ui = app.ui;
    Input& in = *ui.in;
    if (in.rep(BTN_R | BTN_RIGHT | BTN_DOWN)) step(1);
    if (in.rep(BTN_L | BTN_LEFT | BTN_UP)) step(-1);
    int font = app.cfg.s.readerFont;
    if (in.down(BTN_ZR)) font = std::min(32, font + 1);
    if (in.down(BTN_ZL)) font = std::max(6, font - 1);
    if (in.pinch.ended) {
        if (in.pinch.scale > 1.2f) font = std::min(32, font + 2);
        else if (in.pinch.scale < 0.83f) font = std::max(6, font - 2);
    }
    if (font != app.cfg.s.readerFont) {
        app.cfg.s.readerFont = font;
        app.saveSoon();
        ui.toast(trf("Text size {0}", {std::to_string(font)}), Icon::Book);
        relayout();
        return;
    }
    if (in.wheel != 0) step(in.wheel > 0 ? -1 : 1);
    // Swipe to turn, tap the edges to turn, tap the middle for the controls.
    if (in.pan.active && !in.pinch.active) {
        swiping_ = true;
        slide_ = std::max(-1.0f, std::min(1.0f, -(in.pan.x - in.pan.startX) / (VW * 0.6f)));
    }
    if (in.pan.ended && swiping_) {
        swiping_ = false;
        float dx = in.pan.x - in.pan.startX;
        if (std::fabs(dx) > 60 || std::fabs(in.pan.vx) > 600) step(dx < 0 ? 1 : -1);
    }
    if (in.tap && !in.pan.active) {
        float x = in.tapX, y = in.tapY;
        if (x < VW * 0.25f) {
            in.consumeTap();
            step(-1);
        } else if (x > VW * 0.75f) {
            in.consumeTap();
            step(1);
        } else if (tapLink(x, y)) {
            in.consumeTap();
        }
    }
    if (in.tapConfirmed) {
        if (chrome_) chrome_ = false;
        else showChrome(6);
    }
}

void ReaderScreen::handleFixedInput() {
    UI& ui = app.ui;
    Input& in = *ui.in;
    float dt = ui.dt;
    auto [pw, ph] = sizeOf(page_);
    float vs = fitScale(page_) * zoom_, dw = pw * vs, dh = ph * vs;
    bool overX = dw > VW + 1, overY = dh > VH + 1;
    // Pages: L / R always; the D-pad scrolls first when the page is bigger than the screen.
    if (in.rep(BTN_R)) step(1);
    if (in.rep(BTN_L)) step(-1);
    auto scrollOr = [&](bool over, float& o, float view, float size, int dir) {
        if (!over) {
            step(dir);
            return;
        }
        float lo = view - size;
        bool atEnd = dir > 0 ? o <= lo + 1 : o >= -1;
        if (atEnd) {
            if (page_ + dir >= 0 && page_ + dir < pages_) goTo(page_ + dir, dir < 0);
            return;
        }
        o = std::max(lo, std::min(0.0f, o - dir * view * 0.45f));
        settle_ = 0;
    };
    if (in.rep(BTN_DOWN)) scrollOr(overY, oy_, VH, dh, 1);
    if (in.rep(BTN_UP)) scrollOr(overY, oy_, VH, dh, -1);
    if (in.rep(BTN_RIGHT)) {
        if (overX) scrollOr(true, ox_, VW, dw, 1);
        else step(1);
    }
    if (in.rep(BTN_LEFT)) {
        if (overX) scrollOr(true, ox_, VW, dw, -1);
        else step(-1);
    }
    // Zoom: ZL / ZR in steps, the right stick up and down smoothly; panning with the right stick
    // sideways or both ways while zoomed in.
    if (in.rep(BTN_ZR)) setZoom(zoom_ * 1.25f, VW / 2, VH / 2);
    if (in.rep(BTN_ZL)) setZoom(zoom_ / 1.25f, VW / 2, VH / 2);
    if (std::fabs(in.rx) > 0.15f || std::fabs(in.ry) > 0.15f) {
        float sp = 1300 * dt;
        ox_ -= in.rx * sp;
        oy_ += in.ry * sp;
        clampView();
        settle_ = 0;
    }
    if (in.wheel != 0) {
        if (overY) {
            float before = oy_;
            oy_ += in.wheel * 70;
            clampView();
            if (oy_ == before) {
                if (in.wheel < 0 && page_ + 1 < pages_) goTo(page_ + 1);
                else if (in.wheel > 0 && page_ > 0) goTo(page_ - 1, true);
            }
        } else {
            step(in.wheel > 0 ? -1 : 1);
        }
        settle_ = 0;
    }
    // Touch: pinch to zoom, drag to move, swipe to turn, double tap to zoom in and out.
    if (in.pinch.active) {
        if (in.pinch.began) pinchBase_ = zoom_;
        setZoom(pinchBase_ * in.pinch.scale, in.pinch.cx, in.pinch.cy);
        panning_ = swiping_ = false;
        slide_ = 0;
        vx_ = vy_ = 0;
    } else if (in.pan.active) {
        if (in.pan.began) {
            panning_ = true;
            vx_ = vy_ = 0;
        }
        if (overX) ox_ += in.pan.dx;
        if (overY) oy_ += in.pan.dy;
        if (!overX) {
            swiping_ = true;
            slide_ = std::max(-1.0f, std::min(1.0f, -(in.pan.x - in.pan.startX) / (VW * 0.7f)));
        }
        clampView();
        settle_ = 0;
    }
    if (in.pan.ended && panning_) {
        panning_ = false;
        if (overX) vx_ = in.pan.vx;
        if (overY) vy_ = in.pan.vy;
        if (swiping_) {
            swiping_ = false;
            float dx = in.pan.x - in.pan.startX, dy = in.pan.y - in.pan.startY;
            if ((std::fabs(dx) > 70 || std::fabs(in.pan.vx) > 700) && std::fabs(dx) > std::fabs(dy) * 0.8f) {
                int dir = dx < 0 ? 1 : -1;
                if (page_ + dir >= 0 && page_ + dir < pages_) goTo(page_ + dir);
            }
        }
    }
    if (!panning_ && (vx_ != 0 || vy_ != 0)) {
        ox_ += vx_ * dt;
        oy_ += vy_ * dt;
        float k = std::exp(-dt * 4.5f);
        vx_ *= k;
        vy_ *= k;
        if (std::fabs(vx_) < 8 && std::fabs(vy_) < 8) vx_ = vy_ = 0;
        clampView();
        settle_ = 0;
    }
    if (in.doubleTap) {
        in.consumeTap();
        if (zoom_ > 1.05f) setZoom(1, in.tapX, in.tapY);
        else setZoom(2.5f, in.tapX, in.tapY);
        return;
    }
    if (in.tap && !overX && zoom_ <= 1.01f) {
        // At the edges a tap turns the page straight away (the middle waits to see if it's a
        // double tap).
        if (in.tapX < VW * 0.2f) {
            in.consumeTap();
            step(-1);
            return;
        }
        if (in.tapX > VW * 0.8f) {
            in.consumeTap();
            step(1);
            return;
        }
    }
    if (in.tapConfirmed) {
        if (tapLink(in.confirmedX, in.confirmedY)) return;
        if (chrome_) chrome_ = false;
        else showChrome(6);
    }
}

void ReaderScreen::drawMessage() {
    UI& ui = app.ui;
    if (opening_) {
        ui.spinner(VW / 2, VH / 2 - 14, 24, hex(0xFFFFFF, 0.8f));
        ui.text(VW / 2, VH / 2 + 34, ui.ellipsize(title_, VW - 80, 18), 18, hex(0xFFFFFF, 0.75f), CENTER);
        return;
    }
    if (error_.empty()) return;
    Rect card{VW / 2 - std::min(260.0f, VW / 2 - 20), VH / 2 - 120, std::min(520.0f, VW - 40), 240};
    ui.panel(card);
    ui.icon(Icon::Book, card.cx(), card.y + 46, 40, ui.t.danger);
    ui.text(card.cx(), card.y + 96, "This document couldn't be opened", 20, ui.t.text, CENTER, true);
    ui.text(card.cx(), card.y + 128, ui.ellipsize(title_, card.w - 40, 16), 16, ui.t.textDim, CENTER);
    ui.text(card.cx(), card.y + 154, ui.ellipsize(error_, card.w - 40, 14), 14, ui.t.textFaint, CENTER);
    Rect btn{card.cx() - 80, card.bottom() - 62, 160, 44};
    if (ui.button(uid("reader.back"), btn, "Back", Icon::Back, UI::BTN_FILLED)) exit();
}

void ReaderScreen::frame() {
    UI& ui = app.ui;
    float dt = ui.dt;
    time_ += dt;
    VW = (rot_ & 1) ? ui.H : ui.W;
    VH = (rot_ & 1) ? ui.W : ui.H;
    {
        int fbw, fbh;
        platform::drawableSize(fbw, fbh);
        dpr_ = ui.H > 0 ? std::max(1.0f, fbh / ui.H) : 1;
    }
    receive();
    if (app.screen != this) return;  // closed (a password prompt was cancelled)
    bool night = app.cfg.s.readerNight != 0;
    ui.fillRect({0, 0, ui.W, ui.H}, night ? hex(0x000000) : hex(0x24262B));

    NVGcontext* vg = app.vg;
    nvgSave(vg);
    if (rot_) {
        nvgTranslate(vg, ui.W / 2, ui.H / 2);
        nvgRotate(vg, rot_ * NVG_PI / 2);
        nvgTranslate(vg, -VW / 2, -VH / 2);
    }
    rotateInput(true);
    if (opened_) {
        requestPages();
        drawPages();
    }
    drawMessage();
    if (opened_ && app.screen == this) drawChrome();
    if (app.screen == this) drawPanel();
    if (app.screen == this && !ui.blocked) handleInput();
    rotateInput(false);
    nvgRestore(vg);

    settle_ += dt;
    slide_ *= std::exp(-dt * 14);
    if (std::fabs(slide_) < 0.002f) slide_ = 0;
    if (chrome_ && panel_ == NONE && !searching_ && !scrubbing_) {  // A brings them back
        chromeTimer_ -= dt;
        if (chromeTimer_ <= 0) chrome_ = false;
    }
    if (saveTimer_ > 0 && (saveTimer_ -= dt) <= 0) saveProgress();
}
