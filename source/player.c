#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <audio/audio.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "player.h"
#include "net.h"
#include "util.h"

/* ------------------------------------------------------------------ */
/* configuration                                                        */

#define MAX_SLOTS       32
#define SLOT_ARENA      (40 * 1024 * 1024)
#define MAX_W           1920
#define MAX_H           1088
#define AUDIO_RATE      48000
#define AUDIO_RING      (AUDIO_RATE * 3)     /* frames (stereo float) */
#define AUDIO_PREBUF    (AUDIO_RATE * 6 / 10)
#define AUDIO_NBLOCKS   AUDIO_BLOCK_16
#define NMARKERS        256

#define ALIGN64(x) (((x) + 63) & ~63)

enum { SLOT_FREE = 0, SLOT_BUSY, SLOT_READY, SLOT_SHOWN, SLOT_RETIRED };

typedef struct {
    uint8_t *y, *u, *v;
    uint32_t yo, uo, vo;
    int w, h, ys, uvs, sar_n, sar_d;
    double pts;
    uint64_t seq;
    int state;
    int retire;
} vslot;

typedef struct {
    uint64_t start;   /* ring frame index of the first sample of this chunk */
    double pts;
} amarker;

/* ------------------------------------------------------------------ */
/* shared state (protected by g_lock unless noted)                      */

static util_mutex g_lock;
static vslot g_slots[MAX_SLOTS];
static int g_nslots;                  /* 0 until the first frame sets the layout */
static int g_cap_w, g_cap_h;          /* picture size each slot can hold */
static uint8_t *g_arena;
static uint32_t g_arena_off;
static uint64_t g_seq;

static volatile int g_abort;
static volatile player_state g_state = PS_IDLE;
static char g_msg[160];
static char g_info[160];

static sys_ppu_thread_t g_dec_tid;
static int g_dec_running;

static char *g_url, *g_ua, *g_ref;
static int g_maxh;

/* audio ring */
static float *g_aring;
static uint64_t g_apushed;            /* total frames written into the ring */
static uint64_t g_aconsumed;          /* total frames handed to the audio port */
static uint64_t g_aclock_frames;      /* frames actually played (approx) */
static amarker g_markers[NMARKERS];
static int g_mhead, g_mcount;
static int g_has_audio;
static int g_buffering;
static int g_stream_done;             /* demuxer finished: play out what is left */
static uint64_t g_last_push_us;

/* wall clock fallback */
static double g_wall_base_pts;
static uint64_t g_wall_base_us;
static int g_wall_valid;

/* audio thread */
static sys_ppu_thread_t g_audio_tid;
static volatile int g_audio_quit;
static int g_audio_ok;

/* ------------------------------------------------------------------ */

static void set_state(player_state st, const char *msg)
{
    util_mutex_lock(&g_lock);
    g_state = st;
    if (msg)
        util_strlcpy(g_msg, msg, sizeof(g_msg));
    util_mutex_unlock(&g_lock);
}

static void audio_reset_locked(void)
{
    g_apushed = g_aconsumed = g_aclock_frames = 0;
    g_mhead = g_mcount = 0;
    g_buffering = 1;
    g_stream_done = 0;
    g_last_push_us = 0;
}

/* -------------------------- audio output -------------------------- */

static void audio_thread(void *arg)
{
    audioPortParam params;
    audioPortConfig config;
    u32 port;
    int nblocks, wblock = 1, i;
    int block_frames[32];
    volatile u64 *read_index;
    float *base;

    (void)arg;
    memset(&params, 0, sizeof(params));
    params.numChannels = AUDIO_PORT_2CH;
    params.numBlocks = AUDIO_NBLOCKS;
    params.attrib = AUDIO_PORT_INITLEVEL;
    params.level = 1.0f;
    if (audioPortOpen(&params, &port) != 0)
        return;
    if (audioGetPortConfig(port, &config) != 0) {
        audioPortClose(port);
        return;
    }
    nblocks = (int)config.numBlocks;
    read_index = (volatile u64 *)(u64)config.readIndex;
    base = (float *)(u64)config.audioDataStart;
    memset(base, 0, (size_t)nblocks * 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
    for (i = 0; i < 32; i++)
        block_frames[i] = 0;
    audioPortStart(port);

    while (!g_audio_quit) {
        int cur = (int)(*read_index);
        float *dst;
        int got = 0;

        if (wblock == cur) {
            util_sleep_ms(1);
            continue;
        }
        dst = base + (size_t)wblock * 2 * AUDIO_BLOCK_SAMPLES;

        util_mutex_lock(&g_lock);
        if (g_buffering) {
            uint64_t avail = g_apushed - g_aconsumed;
            if (avail >= AUDIO_PREBUF || (g_stream_done && avail > 0))
                g_buffering = 0;
        }
        if (!g_buffering) {
            uint64_t avail = g_apushed - g_aconsumed;
            got = avail < AUDIO_BLOCK_SAMPLES ? (int)avail : AUDIO_BLOCK_SAMPLES;
            for (i = 0; i < got; i++) {
                size_t idx = (size_t)((g_aconsumed + i) % AUDIO_RING) * 2;
                dst[i * 2 + 0] = g_aring[idx];
                dst[i * 2 + 1] = g_aring[idx + 1];
            }
            g_aconsumed += got;
            if (got == 0 && g_has_audio && !g_stream_done && g_state == PS_PLAYING)
                g_buffering = 1;   /* underrun: rebuffer */
        }
        for (i = got; i < AUDIO_BLOCK_SAMPLES; i++) {
            dst[i * 2 + 0] = 0.0f;
            dst[i * 2 + 1] = 0.0f;
        }
        block_frames[wblock] = got;

        /* frames still queued in the port (including the block being read) */
        {
            int queued = 0, b = cur;
            while (b != wblock) {
                queued += block_frames[b];
                b = (b + 1) % nblocks;
            }
            queued += got;
            g_aclock_frames = g_aconsumed > (uint64_t)queued ? g_aconsumed - queued : 0;
        }
        util_mutex_unlock(&g_lock);

        wblock = (wblock + 1) % nblocks;
    }

    audioPortStop(port);
    audioPortClose(port);
}

/* push converted stereo float frames; blocks while the ring is full */
static int audio_push(const float *samples, int frames, double pts)
{
    int done = 0;

    util_mutex_lock(&g_lock);
    if (!isnan(pts)) {
        int idx = (g_mhead + g_mcount) % NMARKERS;
        if (g_mcount == NMARKERS) {
            g_mhead = (g_mhead + 1) % NMARKERS;
            g_mcount--;
            idx = (g_mhead + g_mcount) % NMARKERS;
        }
        g_markers[idx].start = g_apushed;
        g_markers[idx].pts = pts;
        g_mcount++;
    }
    util_mutex_unlock(&g_lock);

    while (done < frames) {
        int space, n, i;
        if (g_abort)
            return -1;
        util_mutex_lock(&g_lock);
        space = AUDIO_RING - (int)(g_apushed - g_aconsumed);
        n = frames - done;
        if (n > space)
            n = space;
        for (i = 0; i < n; i++) {
            size_t idx = (size_t)((g_apushed + i) % AUDIO_RING) * 2;
            g_aring[idx] = samples[(done + i) * 2];
            g_aring[idx + 1] = samples[(done + i) * 2 + 1];
        }
        g_apushed += n;
        g_last_push_us = util_time_us();
        util_mutex_unlock(&g_lock);
        done += n;
        if (n == 0)
            util_sleep_ms(5);
    }
    return 0;
}

/* master clock in seconds; returns 0 when no clock is available yet */
static int clock_locked(double *out)
{
    uint64_t now = util_time_us();

    if (g_has_audio && g_mcount > 0 && g_last_push_us &&
        (now - g_last_push_us) < 1500000ULL) {
        /* find the newest marker that has started playing */
        int i, best = -1;
        for (i = 0; i < g_mcount; i++) {
            int idx = (g_mhead + i) % NMARKERS;
            if (g_markers[idx].start <= g_aclock_frames)
                best = idx;
        }
        if (best < 0)
            best = g_mhead;
        *out = g_markers[best].pts +
               ((double)g_aclock_frames - (double)g_markers[best].start) / AUDIO_RATE;
        /* keep the wall clock in sync so it can take over seamlessly */
        g_wall_base_pts = *out;
        g_wall_base_us = now;
        g_wall_valid = !g_buffering;
        return 1;
    }
    if (g_wall_valid) {
        *out = g_wall_base_pts + (double)(now - g_wall_base_us) / 1e6;
        return 1;
    }
    return 0;
}

/* --------------------------- video slots --------------------------- */

static int slots_alloc(void)
{
    g_arena = gfx_tex_alloc(SLOT_ARENA, 128, &g_arena_off);
    g_nslots = 0;
    return g_arena ? 0 : -1;
}

#define ALIGN128(x) (((x) + 127) & ~127)

/* Splits the arena into as many slots as fit for a w x h picture. Caller holds g_lock
   and guarantees no slot is in use by the decoder. */
static void slots_layout(int w, int h)
{
    size_t ysz = ALIGN128((size_t)ALIGN64(w) * h);
    size_t csz = ALIGN128((size_t)ALIGN64((w + 1) / 2) * ((h + 1) / 2));
    size_t per = ysz + 2 * csz;
    int n = (int)(SLOT_ARENA / per), i;
    if (n > MAX_SLOTS)
        n = MAX_SLOTS;
    for (i = 0; i < n; i++) {
        vslot *s = &g_slots[i];
        size_t off = per * i;
        s->y = g_arena + off;
        s->u = g_arena + off + ysz;
        s->v = g_arena + off + ysz + csz;
        s->yo = g_arena_off + (uint32_t)off;
        s->uo = g_arena_off + (uint32_t)(off + ysz);
        s->vo = g_arena_off + (uint32_t)(off + ysz + csz);
        s->state = SLOT_FREE;
    }
    g_nslots = n;
    g_cap_w = w;
    g_cap_h = h;
}

static vslot *slot_acquire(void)
{
    uint64_t start = util_time_us();
    for (;;) {
        int i, steal = -1;
        if (g_abort)
            return NULL;
        util_mutex_lock(&g_lock);
        for (i = 0; i < g_nslots; i++) {
            if (g_slots[i].state == SLOT_FREE) {
                g_slots[i].state = SLOT_BUSY;
                util_mutex_unlock(&g_lock);
                return &g_slots[i];
            }
        }
        /* While audio is (re)buffering the clock is frozen, so waiting for the
           display to free a slot would deadlock: drop the oldest queued picture. */
        if (g_buffering || util_time_us() - start > 400000ULL) {
            for (i = 0; i < g_nslots; i++)
                if (g_slots[i].state == SLOT_READY &&
                    (steal < 0 || g_slots[i].seq < g_slots[steal].seq))
                    steal = i;
            if (steal >= 0) {
                g_slots[steal].state = SLOT_BUSY;
                util_mutex_unlock(&g_lock);
                return &g_slots[steal];
            }
        }
        util_mutex_unlock(&g_lock);
        util_sleep_ms(3);
    }
}

static void copy_plane(uint8_t *dst, int dst_stride, const uint8_t *src, int src_stride,
                       int width, int height)
{
    int r;
    for (r = 0; r < height; r++)
        memcpy(dst + (size_t)r * dst_stride, src + (size_t)r * src_stride, width);
}

/* ------------------------------ decoder ----------------------------- */

typedef struct {
    stream_t *s;
    AVFormatContext *fmt;
    AVIOContext *avio;
    AVCodecContext *vctx, *actx;
    int vidx, aidx;
    AVFrame *frame;
    AVPacket *pkt;
    struct SwrContext *swr;
    int swr_rate, swr_fmt;
    int64_t swr_layout;
    struct SwsContext *sws;
    float *abuf;
    int abuf_frames;
    double next_apts;
    double last_vpts;
    int skipping;
} decoder;

static int read_cb(void *opaque, uint8_t *buf, int size)
{
    int n = stream_read((stream_t *)opaque, buf, size);
    if (n > 0)
        return n;
    if (n == 0)
        return AVERROR_EOF;
    return g_abort ? AVERROR_EXIT : AVERROR(EIO);
}

static int interrupt_cb(void *opaque)
{
    (void)opaque;
    return g_abort;
}

static AVCodecContext *open_codec(AVFormatContext *fmt, int idx, int is_video)
{
    AVStream *st = fmt->streams[idx];
    AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *ctx;
    if (!codec)
        return NULL;
    ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        return NULL;
    if (avcodec_parameters_to_context(ctx, st->codecpar) < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }
    ctx->thread_count = 1;
    if (is_video) {
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        ctx->skip_loop_filter = (st->codecpar->height > 576) ? AVDISCARD_ALL : AVDISCARD_NONREF;
    }
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }
    return ctx;
}

static void handle_video(decoder *d, AVFrame *f)
{
    AVStream *st = d->fmt->streams[d->vidx];
    double pts = NAN, clk;
    int have_clk;
    vslot *s;
    int w = f->width, h = f->height;

    if (f->best_effort_timestamp != AV_NOPTS_VALUE)
        pts = f->best_effort_timestamp * av_q2d(st->time_base);
    if (isnan(pts)) {
        double dur = av_q2d(st->avg_frame_rate) > 0 ? 1.0 / av_q2d(st->avg_frame_rate) : 0.04;
        pts = isnan(d->last_vpts) ? 0.0 : d->last_vpts + dur;
    }
    d->last_vpts = pts;

    /* drop late frames and let the decoder skip non-reference frames to catch up */
    util_mutex_lock(&g_lock);
    have_clk = clock_locked(&clk) && !g_buffering;
    util_mutex_unlock(&g_lock);
    if (have_clk) {
        double late = clk - pts;
        if (late > 0.25 && late < 5.0) {
            if (!d->skipping) {
                d->vctx->skip_frame = AVDISCARD_NONREF;
                d->skipping = 1;
            }
            return;
        }
        if (d->skipping && late < 0.05) {
            d->vctx->skip_frame = AVDISCARD_DEFAULT;
            d->skipping = 0;
        }
    }

    if (w <= 0 || h <= 0)
        return;

    /* the first picture decides how the texture arena is split into slots */
    util_mutex_lock(&g_lock);
    if (g_nslots == 0) {
        int lw = w, lh = h;
        if (lw > MAX_W) { lh = lh * MAX_W / lw; lw = MAX_W; }
        if (lh > MAX_H) { lw = lw * MAX_H / lh; lh = MAX_H; }
        slots_layout(lw & ~1, lh & ~1);
    }
    util_mutex_unlock(&g_lock);

    s = slot_acquire();
    if (!s)
        return;

    if ((f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) &&
        w <= g_cap_w && h <= g_cap_h) {
        s->w = w;
        s->h = h;
        s->ys = ALIGN64(w);
        s->uvs = ALIGN64((w + 1) / 2);
        copy_plane(s->y, s->ys, f->data[0], f->linesize[0], w, h);
        copy_plane(s->u, s->uvs, f->data[1], f->linesize[1], (w + 1) / 2, (h + 1) / 2);
        copy_plane(s->v, s->uvs, f->data[2], f->linesize[2], (w + 1) / 2, (h + 1) / 2);
    } else {
        int dw = w, dh = h;
        uint8_t *dst[4];
        int dst_stride[4];
        if (dw > g_cap_w) { dh = dh * g_cap_w / dw; dw = g_cap_w; }
        if (dh > g_cap_h) { dw = dw * g_cap_h / dh; dh = g_cap_h; }
        dw &= ~1;
        dh &= ~1;
        d->sws = sws_getCachedContext(d->sws, w, h, f->format, dw, dh, AV_PIX_FMT_YUV420P,
                                      SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!d->sws || dw <= 0 || dh <= 0) {
            util_mutex_lock(&g_lock);
            s->state = SLOT_FREE;
            util_mutex_unlock(&g_lock);
            return;
        }
        s->w = dw;
        s->h = dh;
        s->ys = ALIGN64(dw);
        s->uvs = ALIGN64(dw / 2);
        dst[0] = s->y; dst[1] = s->u; dst[2] = s->v; dst[3] = NULL;
        dst_stride[0] = s->ys; dst_stride[1] = s->uvs; dst_stride[2] = s->uvs; dst_stride[3] = 0;
        sws_scale(d->sws, (const uint8_t *const *)f->data, f->linesize, 0, h, dst, dst_stride);
    }

    s->sar_n = f->sample_aspect_ratio.num;
    s->sar_d = f->sample_aspect_ratio.den;
    if (s->sar_n <= 0 || s->sar_d <= 0) {
        s->sar_n = st->sample_aspect_ratio.num;
        s->sar_d = st->sample_aspect_ratio.den;
    }
    s->pts = pts;

    util_mutex_lock(&g_lock);
    s->seq = ++g_seq;
    s->state = SLOT_READY;
    util_mutex_unlock(&g_lock);
}

static void handle_audio(decoder *d, AVFrame *f)
{
    AVStream *st = d->fmt->streams[d->aidx];
    int64_t layout = f->channel_layout ? (int64_t)f->channel_layout
                                       : av_get_default_channel_layout(f->channels);
    double pts = NAN;
    int out_frames, n;

    if (!d->swr || d->swr_rate != f->sample_rate || d->swr_fmt != f->format ||
        d->swr_layout != layout) {
        swr_free(&d->swr);
        d->swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT, AUDIO_RATE,
                                    layout, f->format, f->sample_rate, 0, NULL);
        if (!d->swr || swr_init(d->swr) < 0) {
            swr_free(&d->swr);
            return;
        }
        d->swr_rate = f->sample_rate;
        d->swr_fmt = f->format;
        d->swr_layout = layout;
    }

    out_frames = swr_get_out_samples(d->swr, f->nb_samples) + 256;
    if (out_frames > d->abuf_frames) {
        float *nb = realloc(d->abuf, sizeof(float) * 2 * out_frames);
        if (!nb)
            return;
        d->abuf = nb;
        d->abuf_frames = out_frames;
    }
    n = swr_convert(d->swr, (uint8_t **)&d->abuf, out_frames,
                    (const uint8_t **)f->extended_data, f->nb_samples);
    if (n <= 0)
        return;

    if (f->best_effort_timestamp != AV_NOPTS_VALUE)
        pts = f->best_effort_timestamp * av_q2d(st->time_base);
    if (isnan(pts))
        pts = d->next_apts;
    if (!isnan(pts))
        d->next_apts = pts + (double)n / AUDIO_RATE;

    audio_push(d->abuf, n, pts);
}

static void decode_packet(decoder *d, AVCodecContext *ctx, AVPacket *pkt, int is_video)
{
    if (avcodec_send_packet(ctx, pkt) < 0)
        return;
    while (!g_abort && avcodec_receive_frame(ctx, d->frame) == 0) {
        if (is_video)
            handle_video(d, d->frame);
        else
            handle_audio(d, d->frame);
        av_frame_unref(d->frame);
    }
}

static void decoder_cleanup(decoder *d)
{
    if (d->vctx)
        avcodec_free_context(&d->vctx);
    if (d->actx)
        avcodec_free_context(&d->actx);
    if (d->fmt)
        avformat_close_input(&d->fmt);
    if (d->avio) {
        av_freep(&d->avio->buffer);
        avio_context_free(&d->avio);
    }
    if (d->frame)
        av_frame_free(&d->frame);
    if (d->pkt)
        av_packet_free(&d->pkt);
    swr_free(&d->swr);
    if (d->sws)
        sws_freeContext(d->sws);
    free(d->abuf);
    if (d->s)
        stream_close(d->s);
}

static void fail(decoder *d, const char *fallback)
{
    char msg[160];
    if (g_abort)
        return;
    if (d->s && stream_failed(d->s, msg, sizeof(msg)))
        set_state(PS_ERROR, msg);
    else
        set_state(PS_ERROR, fallback);
}

static void decode_thread(void *arg)
{
    decoder dd, *d = &dd;
    uint8_t *iobuf;
    int ret;
    char sinfo[64];

    (void)arg;
    memset(d, 0, sizeof(*d));
    d->vidx = d->aidx = -1;
    d->next_apts = NAN;
    d->last_vpts = NAN;

    set_state(PS_CONNECTING, "Bağlanıyor...");

    d->s = stream_open(g_url, g_ua, g_ref, g_maxh);
    if (!d->s) {
        fail(d, "Bellek yetersiz");
        goto done;
    }

    iobuf = av_malloc(65536);
    d->avio = iobuf ? avio_alloc_context(iobuf, 65536, 0, d->s, read_cb, NULL, NULL) : NULL;
    d->fmt = avformat_alloc_context();
    if (!d->avio || !d->fmt) {
        if (!d->avio && iobuf)
            av_free(iobuf);
        fail(d, "Bellek yetersiz");
        goto done;
    }
    d->avio->seekable = 0;
    d->fmt->pb = d->avio;
    d->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    d->fmt->interrupt_callback.callback = interrupt_cb;
    d->fmt->probesize = 2 * 1024 * 1024;
    d->fmt->max_analyze_duration = 4 * AV_TIME_BASE;

    ret = avformat_open_input(&d->fmt, "stream", NULL, NULL);
    if (ret < 0) {
        d->fmt = NULL;   /* freed by avformat_open_input on failure */
        fail(d, "Akış açılamadı veya formatı tanınmadı");
        goto done;
    }
    if (g_abort)
        goto done;
    set_state(PS_BUFFERING, "Akış çözümleniyor...");
    avformat_find_stream_info(d->fmt, NULL);
    if (g_abort)
        goto done;

    d->vidx = av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    d->aidx = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, d->vidx, NULL, 0);
    if (d->vidx >= 0 && !(d->vctx = open_codec(d->fmt, d->vidx, 1)))
        d->vidx = -1;
    if (d->aidx >= 0 && !(d->actx = open_codec(d->fmt, d->aidx, 0)))
        d->aidx = -1;
    if (d->vidx < 0 && d->aidx < 0) {
        fail(d, "Desteklenen ses/görüntü akışı bulunamadı");
        goto done;
    }

    stream_info(d->s, sinfo, sizeof(sinfo));
    util_mutex_lock(&g_lock);
    g_has_audio = d->aidx >= 0;
    snprintf(g_info, sizeof(g_info), "%s%s%dx%d  %s  %s",
             sinfo, sinfo[0] ? "  " : "",
             d->vctx ? d->vctx->width : 0, d->vctx ? d->vctx->height : 0,
             d->vctx ? avcodec_get_name(d->vctx->codec_id) : "-",
             d->actx ? avcodec_get_name(d->actx->codec_id) : "sessiz");
    util_mutex_unlock(&g_lock);

    d->frame = av_frame_alloc();
    d->pkt = av_packet_alloc();
    if (!d->frame || !d->pkt) {
        fail(d, "Bellek yetersiz");
        goto done;
    }

    while (!g_abort) {
        ret = av_read_frame(d->fmt, d->pkt);
        if (ret == AVERROR(EAGAIN))
            continue;
        if (ret < 0)
            break;
        if (d->pkt->stream_index == d->vidx)
            decode_packet(d, d->vctx, d->pkt, 1);
        else if (d->pkt->stream_index == d->aidx)
            decode_packet(d, d->actx, d->pkt, 0);
        av_packet_unref(d->pkt);
    }

    if (!g_abort) {
        char msg[160];
        util_mutex_lock(&g_lock);
        g_stream_done = 1;
        util_mutex_unlock(&g_lock);
        if (stream_failed(d->s, msg, sizeof(msg))) {
            set_state(PS_ERROR, msg);
        } else {
            /* let the remaining audio play out */
            while (!g_abort) {
                uint64_t left;
                util_mutex_lock(&g_lock);
                left = g_apushed - g_aconsumed;
                util_mutex_unlock(&g_lock);
                if (left == 0)
                    break;
                util_sleep_ms(50);
            }
            if (!g_abort)
                set_state(PS_ENDED, "Yayın sona erdi");
        }
    }

done:
    decoder_cleanup(d);
}

/* ------------------------------ public ------------------------------ */

int player_init(void)
{
    util_mutex_init(&g_lock);
    av_log_set_level(AV_LOG_QUIET);
    if (slots_alloc() != 0)
        return -1;
    g_aring = malloc(sizeof(float) * 2 * AUDIO_RING);
    if (!g_aring)
        return -1;
    audio_reset_locked();
    if (audioInit() == 0) {
        g_audio_ok = 1;
        g_audio_quit = 0;
        if (util_thread_start(&g_audio_tid, audio_thread, NULL, "audio_out", 900, 64) != 0)
            g_audio_ok = 0;
    }
    return 0;
}

void player_shutdown(void)
{
    player_stop();
    if (g_audio_ok) {
        g_audio_quit = 1;
        util_thread_join(g_audio_tid);
        audioQuit();
        g_audio_ok = 0;
    }
}

void player_play(const char *url, const char *ua, const char *referer, int max_height)
{
    int i;
    player_stop();

    free(g_url);
    free(g_ua);
    free(g_ref);
    g_url = util_strdup(url);
    g_ua = util_strdup(ua ? ua : "");
    g_ref = util_strdup(referer ? referer : "");
    g_maxh = max_height;

    util_mutex_lock(&g_lock);
    for (i = 0; i < g_nslots; i++)
        g_slots[i].state = SLOT_FREE;
    g_nslots = 0;        /* re-layout for the new stream's resolution */
    audio_reset_locked();
    g_has_audio = 0;
    g_wall_valid = 0;
    g_info[0] = 0;
    g_msg[0] = 0;
    g_state = PS_CONNECTING;
    util_mutex_unlock(&g_lock);

    g_abort = 0;
    if (util_thread_start(&g_dec_tid, decode_thread, NULL, "decoder", 1100, 512) == 0)
        g_dec_running = 1;
    else
        set_state(PS_ERROR, "Çözücü iş parçacığı başlatılamadı");
}

void player_stop(void)
{
    int i;
    if (g_dec_running) {
        g_abort = 1;
        util_thread_join(g_dec_tid);
        g_dec_running = 0;
    }
    util_mutex_lock(&g_lock);
    audio_reset_locked();
    g_has_audio = 0;
    g_wall_valid = 0;
    for (i = 0; i < g_nslots; i++)
        if (g_slots[i].state != SLOT_SHOWN && g_slots[i].state != SLOT_RETIRED)
            g_slots[i].state = SLOT_FREE;
    g_state = PS_IDLE;
    util_mutex_unlock(&g_lock);
}

player_state player_get_state(char *msg, size_t msglen)
{
    player_state st;
    util_mutex_lock(&g_lock);
    st = g_state;
    if (st == PS_BUFFERING || st == PS_PLAYING) {
        int has_ready = 0, i;
        for (i = 0; i < g_nslots; i++)
            if (g_slots[i].state == SLOT_SHOWN || g_slots[i].state == SLOT_READY)
                has_ready = 1;
        if ((g_has_audio && g_buffering) || (!has_ready && !g_has_audio)) {
            st = PS_BUFFERING;
            if (msg) {
                uint64_t avail = g_apushed - g_aconsumed;
                int pct = (int)(avail * 100 / AUDIO_PREBUF);
                if (pct > 100)
                    pct = 100;
                snprintf(msg, msglen, "Arabelleğe alınıyor... %%%d", g_has_audio ? pct : 0);
            }
        } else {
            st = PS_PLAYING;
            if (g_state != PS_PLAYING)
                g_state = PS_PLAYING;
            if (msg)
                msg[0] = 0;
        }
    } else if (msg) {
        util_strlcpy(msg, g_msg, msglen);
    }
    util_mutex_unlock(&g_lock);
    return st;
}

void player_info(char *out, size_t outlen)
{
    util_mutex_lock(&g_lock);
    util_strlcpy(out, g_info, outlen);
    util_mutex_unlock(&g_lock);
}

int player_video_frame(video_tex *out)
{
    int i, chosen = -1, shown = -1, oldest = -1;
    double clk = 0;
    int have_clk;

    util_mutex_lock(&g_lock);
    have_clk = clock_locked(&clk);

    for (i = 0; i < g_nslots; i++) {
        vslot *s = &g_slots[i];
        if (s->state == SLOT_SHOWN)
            shown = i;
        if (s->state != SLOT_READY)
            continue;
        if (oldest < 0 || s->seq < g_slots[oldest].seq)
            oldest = i;
    }

    if (oldest >= 0) {
        if (shown < 0 && !have_clk) {
            chosen = oldest;      /* first picture: show immediately */
        } else if (!have_clk) {
            if (!g_has_audio) {
                /* no audio: start the wall clock at the first frame */
                g_wall_base_pts = g_slots[oldest].pts;
                g_wall_base_us = util_time_us();
                g_wall_valid = 1;
                chosen = oldest;
            }
        } else {
            double d = g_slots[oldest].pts - clk;
            if (d > 3.0 || d < -3.0) {
                /* timestamp discontinuity */
                chosen = oldest;
                if (!g_has_audio) {
                    g_wall_base_pts = g_slots[oldest].pts;
                    g_wall_base_us = util_time_us();
                }
            } else {
                /* newest frame that is due; drop older due frames */
                for (i = 0; i < g_nslots; i++) {
                    vslot *s = &g_slots[i];
                    if (s->state == SLOT_READY && s->pts <= clk + 0.008 &&
                        (chosen < 0 || s->seq > g_slots[chosen].seq))
                        chosen = i;
                }
                if (chosen >= 0)
                    for (i = 0; i < g_nslots; i++)
                        if (i != chosen && g_slots[i].state == SLOT_READY &&
                            g_slots[i].seq < g_slots[chosen].seq)
                            g_slots[i].state = SLOT_FREE;
            }
        }
    }

    if (chosen >= 0) {
        if (shown >= 0) {
            g_slots[shown].state = SLOT_RETIRED;
            g_slots[shown].retire = 2;
        }
        g_slots[chosen].state = SLOT_SHOWN;
        shown = chosen;
    }

    if (shown >= 0 && g_state != PS_IDLE) {
        vslot *s = &g_slots[shown];
        out->y_off = s->yo;
        out->u_off = s->uo;
        out->v_off = s->vo;
        out->w = s->w;
        out->h = s->h;
        out->y_stride = s->ys;
        out->uv_stride = s->uvs;
        out->sar_num = s->sar_n;
        out->sar_den = s->sar_d;
        util_mutex_unlock(&g_lock);
        return 1;
    }
    util_mutex_unlock(&g_lock);
    return 0;
}

void player_frame_done(void)
{
    int i;
    util_mutex_lock(&g_lock);
    for (i = 0; i < g_nslots; i++) {
        vslot *s = &g_slots[i];
        if (s->state == SLOT_RETIRED && --s->retire <= 0)
            s->state = SLOT_FREE;
        if (g_state == PS_IDLE && s->state == SLOT_SHOWN) {
            s->state = SLOT_RETIRED;
            s->retire = 2;
        }
    }
    util_mutex_unlock(&g_lock);
}
