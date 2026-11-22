#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <net/net.h>
#include <sysmodule/sysmodule.h>
#include <curl/curl.h>

#include "net.h"
#include "util.h"

#define RING_SIZE      (6 * 1024 * 1024)
#define PLAYLIST_LIMIT (64 * 1024 * 1024)
#define HLS_MAX_SEGS   4096

struct stream {
    char *url, *ua, *referer;
    int max_height;

    volatile int abort;
    volatile int eof;
    volatile int error;
    char errmsg[160];
    char info[64];

    uint8_t *rb;
    size_t rpos, wpos, fill;
    util_mutex lock;

    sys_ppu_thread_t tid;
    int thread_ok;
    volatile uint64_t total;
};

enum { W_RING = 0, W_MEM = 1, W_DETECT = 2 };

typedef struct {
    stream_t *s;            /* may be NULL for plain downloads */
    int mode;
    util_buf mem;
    size_t limit;
    int is_playlist;
    uint64_t got;
    int overflow;
} wctx;

/* ------------------------------------------------------------------ */

int net_init(void)
{
    netInitialize();
    sysModuleLoad(SYSMODULE_NET);
    curl_global_init(CURL_GLOBAL_ALL);
    return 0;
}

void net_shutdown(void)
{
    curl_global_cleanup();
    netDeinitialize();
    sysModuleUnload(SYSMODULE_NET);
}

/* ------------------------------------------------------------------ */

static void set_error(stream_t *s, const char *msg)
{
    util_mutex_lock(&s->lock);
    if (!s->error) {
        util_strlcpy(s->errmsg, msg, sizeof(s->errmsg));
        s->error = 1;
    }
    util_mutex_unlock(&s->lock);
}

static int ring_write(stream_t *s, const uint8_t *data, size_t n)
{
    while (n > 0) {
        size_t chunk;
        if (s->abort)
            return -1;
        util_mutex_lock(&s->lock);
        chunk = RING_SIZE - s->fill;
        if (chunk > n)
            chunk = n;
        if (chunk > 0) {
            size_t first = RING_SIZE - s->wpos;
            if (first > chunk)
                first = chunk;
            memcpy(s->rb + s->wpos, data, first);
            if (chunk > first)
                memcpy(s->rb, data + first, chunk - first);
            s->wpos = (s->wpos + chunk) % RING_SIZE;
            s->fill += chunk;
        }
        util_mutex_unlock(&s->lock);
        if (chunk == 0) {
            util_sleep_ms(5);
            continue;
        }
        data += chunk;
        n -= chunk;
        s->total += chunk;
    }
    return 0;
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    wctx *w = (wctx *)ud;
    size_t n = size * nmemb;

    if (w->s && w->s->abort)
        return 0;
    w->got += n;

    if (w->mode == W_DETECT) {
        if (util_buf_append(&w->mem, ptr, n) != 0)
            return 0;
        if (w->mem.len < 16 && w->got < 16)
            return n;   /* wait for a few more bytes */
        {
            const char *p = w->mem.data;
            if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB)
                p += 3;
            while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')
                p++;
            if (util_istarts(p, "#EXTM3U")) {
                w->is_playlist = 1;
                w->mode = W_MEM;
                return n;
            }
        }
        /* binary media: flush what we buffered into the ring */
        w->mode = W_RING;
        if (ring_write(w->s, (const uint8_t *)w->mem.data, w->mem.len) != 0)
            return 0;
        util_buf_free(&w->mem);
        return n;
    }

    if (w->mode == W_MEM) {
        if (w->mem.len + n > w->limit) {
            w->overflow = 1;
            return 0;
        }
        if (util_buf_append(&w->mem, ptr, n) != 0)
            return 0;
        return n;
    }

    if (ring_write(w->s, (const uint8_t *)ptr, n) != 0)
        return 0;
    return n;
}

static int xfer_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    wctx *w = (wctx *)ud;
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return (w->s && w->s->abort) ? 1 : 0;
}

/* Runs one GET. effective (optional) receives a malloc'd final URL after redirects.
   content_len receives Content-Length or -1. */
static CURLcode do_get(const char *url, const char *ua, const char *referer, wctx *w,
                       int for_text, char **effective, double *content_len, long *http_code)
{
    CURL *c = curl_easy_init();
    CURLcode rc;
    if (!c)
        return CURLE_FAILED_INIT;

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, (ua && *ua) ? ua : NET_DEFAULT_UA);
    if (referer && *referer)
        curl_easy_setopt(c, CURLOPT_REFERER, referer);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 65536L);
    if (for_text)
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, w);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, w);

    rc = curl_easy_perform(c);

    if (effective) {
        char *eff = NULL;
        *effective = NULL;
        if (curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff)
            *effective = util_strdup(eff);
    }
    if (content_len) {
        double cl = -1;
        if (curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &cl) != CURLE_OK)
            cl = -1;
        *content_len = cl;
    }
    if (http_code) {
        long hc = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &hc);
        *http_code = hc;
    }
    curl_easy_cleanup(c);
    return rc;
}

static void describe_error(CURLcode rc, long http_code, char *out, size_t outlen)
{
    if (rc == CURLE_HTTP_RETURNED_ERROR && http_code > 0)
        snprintf(out, outlen, "Sunucu hatası: HTTP %ld", http_code);
    else
        snprintf(out, outlen, "Bağlantı hatası: %s", curl_easy_strerror(rc));
}

char *net_get(const char *url, const char *ua, size_t *out_len, char *err, size_t errlen)
{
    wctx w;
    CURLcode rc;
    long hc = 0;

    if (url[0] == '/') {
        char *d = util_read_file(url, out_len);
        if (!d && err)
            snprintf(err, errlen, "Dosya okunamadı: %s", url);
        return d;
    }

    memset(&w, 0, sizeof(w));
    w.mode = W_MEM;
    w.limit = PLAYLIST_LIMIT;
    rc = do_get(url, ua, NULL, &w, 1, NULL, NULL, &hc);
    if (rc != CURLE_OK) {
        if (err) {
            if (w.overflow)
                snprintf(err, errlen, "Liste çok büyük (64 MB sınırı)");
            else
                describe_error(rc, hc, err, errlen);
        }
        util_buf_free(&w.mem);
        return NULL;
    }
    if (!w.mem.data) {
        util_buf_append(&w.mem, "", 0);
    }
    if (out_len)
        *out_len = w.mem.len;
    return w.mem.data;
}

/* ------------------------------------------------------------------ */
/* URL helpers                                                          */

static char *url_resolve(const char *base, const char *rel)
{
    char *out;
    const char *scheme_end, *host_end, *q, *last_slash;
    size_t n;

    if (util_istarts(rel, "http://") || util_istarts(rel, "https://"))
        return util_strdup(rel);

    scheme_end = strstr(base, "://");
    if (!scheme_end)
        return util_strdup(rel);

    if (rel[0] == '/' && rel[1] == '/') {
        n = (scheme_end - base) + 1;  /* "http:" */
        out = malloc(n + strlen(rel) + 1);
        if (!out)
            return NULL;
        memcpy(out, base, n);
        strcpy(out + n, rel);
        return out;
    }

    host_end = strchr(scheme_end + 3, '/');
    if (!host_end)
        host_end = base + strlen(base);

    if (rel[0] == '/') {
        n = host_end - base;
        out = malloc(n + strlen(rel) + 1);
        if (!out)
            return NULL;
        memcpy(out, base, n);
        strcpy(out + n, rel);
        return out;
    }

    /* relative to directory of base (ignore query string) */
    q = strchr(base, '?');
    if (!q)
        q = base + strlen(base);
    last_slash = NULL;
    {
        const char *p;
        for (p = host_end; p < q; p++)
            if (*p == '/')
                last_slash = p;
    }
    if (!last_slash) {
        n = host_end - base;
        out = malloc(n + 1 + strlen(rel) + 1);
        if (!out)
            return NULL;
        memcpy(out, base, n);
        out[n] = '/';
        strcpy(out + n + 1, rel);
        return out;
    }
    n = (last_slash - base) + 1;
    out = malloc(n + strlen(rel) + 1);
    if (!out)
        return NULL;
    memcpy(out, base, n);
    strcpy(out + n, rel);
    return out;
}

static const char *attr_find(const char *line, const char *key)
{
    size_t kl = strlen(key);
    const char *p = line;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == line || p[-1] == ':' || p[-1] == ',') && p[kl] == '=')
            return p + kl + 1;
        p += kl;
    }
    return NULL;
}

static int is_m3u8_url(const char *url)
{
    const char *q = strchr(url, '?');
    size_t n = q ? (size_t)(q - url) : strlen(url);
    return n > 5 && !strncasecmp(url + n - 5, ".m3u8", 5);
}

/* ------------------------------------------------------------------ */
/* HLS                                                                  */

typedef struct {
    char *uri;
    long bw;
    int w, h;
} hls_variant;

static char *next_line(char **pp)
{
    char *p = *pp, *line, *nl;
    for (;;) {
        if (!*p)
            return NULL;
        line = p;
        nl = strchr(p, '\n');
        if (nl) {
            *nl = 0;
            p = nl + 1;
        } else {
            p += strlen(p);
        }
        util_trim(line);
        if (*line) {
            *pp = p;
            return line;
        }
    }
}

static char *hls_pick_variant(stream_t *s, char *pl)
{
    hls_variant v[64];
    int nv = 0, i, best = -1;
    char *p = pl, *line;
    long pending_bw = 0;
    int pending_w = 0, pending_h = 0, pending = 0;

    while ((line = next_line(&p)) != NULL) {
        if (util_istarts(line, "#EXT-X-STREAM-INF")) {
            const char *a;
            pending = 1;
            pending_bw = 0;
            pending_w = pending_h = 0;
            if ((a = attr_find(line, "BANDWIDTH")) != NULL)
                pending_bw = atol(a);
            if ((a = attr_find(line, "RESOLUTION")) != NULL)
                sscanf(a, "%dx%d", &pending_w, &pending_h);
        } else if (line[0] != '#' && pending) {
            if (nv < 64) {
                v[nv].uri = line;
                v[nv].bw = pending_bw;
                v[nv].w = pending_w;
                v[nv].h = pending_h;
                nv++;
            }
            pending = 0;
        }
    }
    if (nv == 0)
        return NULL;

    if (s->max_height >= 1080) {
        for (i = 0; i < nv; i++)
            if (best < 0 || v[i].bw > v[best].bw)
                best = i;
    } else {
        for (i = 0; i < nv; i++)
            if (v[i].h > 0 && v[i].h <= s->max_height && (best < 0 || v[i].bw > v[best].bw))
                best = i;
        if (best < 0)
            for (i = 0; i < nv; i++)
                if (best < 0 || (v[i].bw > 0 && v[i].bw < v[best].bw))
                    best = i;
    }
    if (v[best].h > 0)
        snprintf(s->info, sizeof(s->info), "HLS %dx%d", v[best].w, v[best].h);
    else
        snprintf(s->info, sizeof(s->info), "HLS %ld kbps", v[best].bw / 1000);
    return v[best].uri;
}

static char *fetch_text(stream_t *s, const char *url, char **effective, char *err, size_t errlen)
{
    wctx w;
    CURLcode rc;
    long hc = 0;
    memset(&w, 0, sizeof(w));
    w.s = s;
    w.mode = W_MEM;
    w.limit = 4 * 1024 * 1024;
    rc = do_get(url, s->ua, s->referer, &w, 1, effective, NULL, &hc);
    if (rc != CURLE_OK) {
        describe_error(rc, hc, err, errlen);
        util_buf_free(&w.mem);
        if (effective) {
            free(*effective);
            *effective = NULL;
        }
        return NULL;
    }
    if (!w.mem.data)
        util_buf_append(&w.mem, "", 0);
    return w.mem.data;
}

static int download_segment(stream_t *s, const char *url)
{
    int attempt;
    for (attempt = 0; attempt < 2 && !s->abort; attempt++) {
        wctx w;
        CURLcode rc;
        memset(&w, 0, sizeof(w));
        w.s = s;
        w.mode = W_RING;
        rc = do_get(url, s->ua, s->referer, &w, 0, NULL, NULL, NULL);
        if (rc == CURLE_OK)
            return 0;
        if (w.got > 0)   /* partial data already in the ring; don't duplicate */
            return 0;
    }
    return -1;
}

static void hls_run(stream_t *s, const char *first_url, char *pl)
{
    char err[160];
    char *media_url = util_strdup(first_url);
    long long last_seq = -1;
    int first = 1, fails = 0;

    if (strstr(pl, "#EXT-X-STREAM-INF")) {
        char *uri = hls_pick_variant(s, pl);
        char *vurl, *eff = NULL;
        if (!uri) {
            set_error(s, "HLS listesinde oynatılabilir akış yok");
            goto out;
        }
        vurl = url_resolve(media_url, uri);
        free(pl);
        pl = fetch_text(s, vurl, &eff, err, sizeof(err));
        free(vurl);
        if (!pl) {
            set_error(s, err);
            goto out;
        }
        free(media_url);
        media_url = eff ? eff : util_strdup(first_url);
    } else if (!s->info[0]) {
        util_strlcpy(s->info, "HLS", sizeof(s->info));
    }

    while (!s->abort) {
        char *segs[HLS_MAX_SEGS];
        int nseg = 0, endlist = 0, i, newsegs = 0;
        long long mseq = 0;
        double target = 6.0;
        char *p = pl, *line;

        while ((line = next_line(&p)) != NULL) {
            if (util_istarts(line, "#EXT-X-TARGETDURATION:"))
                target = atof(line + 22);
            else if (util_istarts(line, "#EXT-X-MEDIA-SEQUENCE:"))
                mseq = atoll(line + 22);
            else if (util_istarts(line, "#EXT-X-ENDLIST"))
                endlist = 1;
            else if (util_istarts(line, "#EXT-X-KEY:")) {
                const char *m = attr_find(line, "METHOD");
                if (m && !util_istarts(m, "NONE")) {
                    set_error(s, "Şifreli HLS akışı desteklenmiyor");
                    goto out;
                }
            } else if (util_istarts(line, "#EXT-X-MAP:")) {
                set_error(s, "fMP4 HLS akışı desteklenmiyor");
                goto out;
            } else if (line[0] != '#' && nseg < HLS_MAX_SEGS) {
                segs[nseg++] = line;
            }
        }
        if (target < 1.0)
            target = 1.0;

        if (nseg == 0) {
            if (++fails > 5) {
                set_error(s, "HLS listesi boş");
                goto out;
            }
        } else {
            if (first) {
                int start = endlist ? 0 : (nseg > 3 ? nseg - 3 : 0);
                last_seq = mseq + start - 1;
                first = 0;
            } else if (mseq + nseg - 1 < last_seq - 10) {
                /* sequence was reset by the server */
                int start = nseg > 3 ? nseg - 3 : 0;
                last_seq = mseq + start - 1;
            }
            for (i = 0; i < nseg && !s->abort; i++) {
                long long seq = mseq + i;
                char *surl;
                if (seq <= last_seq)
                    continue;
                surl = url_resolve(media_url, segs[i]);
                if (!surl)
                    continue;
                download_segment(s, surl);
                free(surl);
                last_seq = seq;
                newsegs++;
            }
            if (endlist && last_seq >= mseq + nseg - 1) {
                s->eof = 1;
                goto out;
            }
        }

        if (newsegs == 0) {
            int waited;
            for (waited = 0; waited < (int)(target * 500) && !s->abort; waited += 100)
                util_sleep_ms(100);
        }
        if (s->abort)
            break;

        /* refresh playlist */
        {
            char *npl = NULL;
            int tries;
            for (tries = 0; tries < 5 && !s->abort; tries++) {
                npl = fetch_text(s, media_url, NULL, err, sizeof(err));
                if (npl)
                    break;
                util_sleep_ms(1000);
            }
            if (!npl) {
                if (!s->abort)
                    set_error(s, err);
                goto out;
            }
            free(pl);
            pl = npl;
            fails = 0;
        }
    }
out:
    free(pl);
    free(media_url);
}

/* ------------------------------------------------------------------ */

static void stream_thread(void *arg)
{
    stream_t *s = (stream_t *)arg;
    int retries = 0, first = 1;
    char err[160];

    while (!s->abort) {
        wctx w;
        CURLcode rc;
        char *eff = NULL;
        double clen = -1;
        long hc = 0;

        memset(&w, 0, sizeof(w));
        w.s = s;
        w.mode = first ? W_DETECT : W_RING;
        w.limit = 4 * 1024 * 1024;

        rc = do_get(s->url, s->ua, s->referer, &w, is_m3u8_url(s->url), &eff, &clen, &hc);
        if (s->abort) {
            util_buf_free(&w.mem);
            free(eff);
            break;
        }
        if (w.is_playlist) {
            if (rc == CURLE_OK) {
                char *pl = w.mem.data;
                w.mem.data = NULL;
                hls_run(s, eff ? eff : s->url, pl);
            } else {
                describe_error(rc, hc, err, sizeof(err));
                set_error(s, err);
            }
            util_buf_free(&w.mem);
            free(eff);
            break;
        }
        util_buf_free(&w.mem);
        free(eff);

        if (w.got > 0)
            first = 0;

        if (rc == CURLE_OK) {
            if (w.got > 0 && clen > 0 && (double)w.got >= clen) {
                s->eof = 1;     /* finite file fully downloaded */
                break;
            }
            if (w.got > 0)
                retries = 0;    /* live stream closed by server: reconnect */
            else
                retries++;
        } else {
            describe_error(rc, hc, err, sizeof(err));
            retries++;
        }
        if (retries >= 3) {
            if (rc == CURLE_OK)
                set_error(s, "Sunucu veri göndermedi");
            else
                set_error(s, err);
            break;
        }
        util_sleep_ms(retries ? 1000 : 200);
    }
    s->eof = 1;
}

stream_t *stream_open(const char *url, const char *ua, const char *referer, int max_height)
{
    stream_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->rb = malloc(RING_SIZE);
    if (!s->rb) {
        free(s);
        return NULL;
    }
    s->url = util_strdup(url);
    s->ua = util_strdup(ua ? ua : "");
    s->referer = util_strdup(referer ? referer : "");
    s->max_height = max_height;
    util_mutex_init(&s->lock);
    if (util_thread_start(&s->tid, stream_thread, s, "net_stream", 1300, 512) == 0)
        s->thread_ok = 1;
    else
        set_error(s, "Ağ iş parçacığı başlatılamadı");
    return s;
}

int stream_read(stream_t *s, uint8_t *buf, int size)
{
    for (;;) {
        size_t n = 0;
        if (s->abort)
            return -1;
        util_mutex_lock(&s->lock);
        if (s->fill > 0) {
            size_t first;
            n = s->fill < (size_t)size ? s->fill : (size_t)size;
            first = RING_SIZE - s->rpos;
            if (first > n)
                first = n;
            memcpy(buf, s->rb + s->rpos, first);
            if (n > first)
                memcpy(buf + first, s->rb, n - first);
            s->rpos = (s->rpos + n) % RING_SIZE;
            s->fill -= n;
        }
        util_mutex_unlock(&s->lock);
        if (n > 0)
            return (int)n;
        if (s->error)
            return -1;
        if (s->eof)
            return 0;
        util_sleep_ms(5);
    }
}

void stream_abort(stream_t *s)
{
    if (s)
        s->abort = 1;
}

void stream_close(stream_t *s)
{
    if (!s)
        return;
    s->abort = 1;
    if (s->thread_ok)
        util_thread_join(s->tid);
    util_mutex_destroy(&s->lock);
    free(s->rb);
    free(s->url);
    free(s->ua);
    free(s->referer);
    free(s);
}

int stream_fill_percent(stream_t *s)
{
    return (int)((s->fill * 100) / RING_SIZE);
}

int stream_failed(stream_t *s, char *msg, size_t msglen)
{
    if (!s->error)
        return 0;
    util_mutex_lock(&s->lock);
    util_strlcpy(msg, s->errmsg, msglen);
    util_mutex_unlock(&s->lock);
    return 1;
}

void stream_info(stream_t *s, char *out, size_t outlen)
{
    util_strlcpy(out, s->info, outlen);
}

uint64_t stream_bytes(stream_t *s)
{
    return s->total;
}
