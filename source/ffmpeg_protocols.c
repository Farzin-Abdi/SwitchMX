// FFmpeg's list of network and file protocols, replacing libavformat's protocols.o (FFmpeg 7.1,
// LGPL-2.1+). Also wraps TLS (below). devkitPro's FFmpeg is built with TLS but without the "https" protocol, so mpv refused
// every https:// address — most streams, and the parts inside HLS playlists. FFmpeg's http code
// already speaks https (it switches to TLS by itself for that scheme); this registers it under
// that name too. Switch only: the desktop build uses the system's FFmpeg as it is.
#ifdef __SWITCH__
#include <pthread.h>
#include <string.h>

#include <errno.h>
#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "../third_party/ffmpeg71/url.h"

extern const URLProtocol ff_file_protocol;
extern const URLProtocol ff_ftp_protocol;
extern const URLProtocol ff_http_protocol;
extern const URLProtocol ff_httpproxy_protocol;
extern const URLProtocol ff_rtmp_protocol;
extern const URLProtocol ff_rtp_protocol;
extern const URLProtocol ff_tcp_protocol;
extern const URLProtocol ff_tls_protocol;
extern const URLProtocol ff_udp_protocol;
extern const URLProtocol ff_udplite_protocol;

static URLProtocol https_protocol;  // filled from the http one, see setup()
static URLProtocol tls_protocol;    // devkitPro's, opened non-blocking (see tls_open_nonblocking)
static URLProtocol ftp_protocol;    // FFmpeg's, given the path decoded (see ftp_open_decoded)

// devkitPro's TLS (the console's SSL service) blocks inside the service on a read until data comes:
// it can't be interrupted or timed out, and with keep-alive servers each reply waited until the
// server hung up (13 s per HLS playlist, 20 s for an update check, for ever when a server stayed
// silent). Opened non-blocking instead, FFmpeg's own read loop retries briefly, stops when asked
// to and keeps to its time limits.
static int tls_open_nonblocking(URLContext *h, const char *uri, int flags, AVDictionary **options) {
    int had = h->flags & AVIO_FLAG_NONBLOCK;
    h->flags |= AVIO_FLAG_NONBLOCK;  // what devkitPro's tls_open looks at to pick the mode
    int ret = ff_tls_protocol.url_open2(h, uri, flags | AVIO_FLAG_NONBLOCK, options);
    if (!had) h->flags &= ~AVIO_FLAG_NONBLOCK;  // callers still see a blocking stream
    return ret;
}

// devkitPro's TLS turns the console's own certificate checks off (SkipDefaultVerify) and, even with
// tls_verify, asks only for a trusted CA: never that the certificate is for the host being called.
// Streams are left that way (plenty of IPTV servers have broken certificates), but SwitchMX's own
// requests (updates, which replace the app, subtitles, lyrics) ask for the full check: trusted CA,
// matching host name, in date. net.cpp sets this around opening its connection, on its thread.
static __thread int strict_tls;
void switchmx_tls_strict(int on) { strict_tls = on; }

int __real_sslConnectionSetVerifyOption(void *c, unsigned option);
int __wrap_sslConnectionSetVerifyOption(void *c, unsigned option) {
    if (strict_tls) option |= 1u | 2u | 4u;  // SslVerifyOption_PeerCa | _HostName | _DateCheck
    return __real_sslConnectionSetVerifyOption(c, option);
}

// FFmpeg's FTP sends the path to the server as it appears in the address, so a file called
// "My Film.mkv" (an address must say "My%20Film.mkv") or any name in another alphabet was asked
// for under its encoded name and never found. The user name and password it already decodes.
static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int ftp_open_decoded(URLContext *h, const char *uri, int flags) {
    const char *auth = strstr(uri, "://");
    const char *path = auth ? strchr(auth + 3, '/') : NULL;
    if (!path || !strchr(path, '%')) return ff_ftp_protocol.url_open(h, uri, flags);
    size_t n = strlen(uri);
    char *dec = av_malloc(n + 1);
    if (!dec) return AVERROR(ENOMEM);
    size_t o = (size_t)(path - uri);
    memcpy(dec, uri, o);
    for (const char *c = path; *c; c++) {
        int hi, lo;
        if (c[0] == '%' && (hi = hexval(c[1])) >= 0 && (lo = hexval(c[2])) >= 0 && (hi || lo)) {
            dec[o++] = (char)(hi * 16 + lo);
            c += 2;
        } else {
            dec[o++] = *c;
        }
    }
    dec[o] = 0;
    int ret = ff_ftp_protocol.url_open(h, dec, flags);
    av_free(dec);
    return ret;
}

static const URLProtocol *url_protocols[] = {
    &ff_file_protocol, &ftp_protocol, &ff_http_protocol, &ff_httpproxy_protocol, &https_protocol,
    &ff_rtmp_protocol, &ff_rtp_protocol, &ff_tcp_protocol,  &tls_protocol,       &ff_udp_protocol,
    &ff_udplite_protocol, NULL,
};

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void setup(void) {
    // As FFmpeg defines ff_https_protocol: the http functions, without the server side.
    https_protocol = ff_http_protocol;
    https_protocol.name = "https";
    https_protocol.url_accept = NULL;
    https_protocol.url_handshake = NULL;
    https_protocol.default_whitelist = "http,https,tls,rtp,tcp,udp,crypto,httpproxy";
    tls_protocol = ff_tls_protocol;
    tls_protocol.url_open2 = tls_open_nonblocking;
    ftp_protocol = ff_ftp_protocol;
    ftp_protocol.url_open = ftp_open_decoded;
}
#define READY() pthread_once(&g_once, setup)

const AVClass *ff_urlcontext_child_class_iterate(void **iter) {
    READY();
    const AVClass *ret = NULL;
    uintptr_t i;
    for (i = (uintptr_t)*iter; url_protocols[i]; i++) {
        ret = url_protocols[i]->priv_data_class;
        if (ret) break;
    }
    *iter = (void *)(uintptr_t)(url_protocols[i] ? i + 1 : i);
    return ret;
}

const char *avio_enum_protocols(void **opaque, int output) {
    READY();
    uintptr_t i;
    for (i = (uintptr_t)*opaque; url_protocols[i]; i++) {
        const URLProtocol *p = url_protocols[i];
        if ((output && p->url_write) || (!output && p->url_read)) {
            *opaque = (void *)(uintptr_t)(i + 1);
            return p->name;
        }
    }
    *opaque = NULL;
    return NULL;
}

const AVClass *avio_protocol_get_class(const char *name) {
    READY();
    for (int i = 0; url_protocols[i]; i++)
        if (!strcmp(url_protocols[i]->name, name)) return url_protocols[i]->priv_data_class;
    return NULL;
}

const URLProtocol **ffurl_get_protocols(const char *whitelist, const char *blacklist) {
    READY();
    const URLProtocol **ret = av_calloc(FF_ARRAY_ELEMS(url_protocols), sizeof(*ret));
    int ret_idx = 0;
    if (!ret) return NULL;
    for (int i = 0; url_protocols[i]; i++) {
        const URLProtocol *up = url_protocols[i];
        if (whitelist && *whitelist && !av_match_name(up->name, whitelist)) continue;
        if (blacklist && *blacklist && av_match_name(up->name, blacklist)) continue;
        ret[ret_idx++] = up;
    }
    return ret;
}
#endif
