// Movie decoding for the Linux host on FFmpeg's libavcodec, in the places dm/android/media_android.c and av1_android.c fill with Android's MediaCodec:
//   H.264 for the movie decoder (hle_movie_avc.h; VideoToolbox on Apple, hle_movie_avc.c), AAC behind AudioToolbox's AudioConverter (hle_movie.c md_aac) and AV1 for the
//   Switch 2's nn::codec video decoders (hle_movie_av1.h; VideoToolbox or dav1d on Apple, hle_movie_av1.c). The parameter-set and NAL handling, the picture queue and the
//   NV12 / RGBA output are media_android.c's; the decode loops follow FFmpeg's doc/examples/decode_video.c and decode_audio.c (send a packet, receive every frame it
//   gives). Decoding is synchronous on the calling thread, with slice threads only, so a picture leaves as soon as libavcodec has it: frame threads would hold pictures
//   back by their count. AV1 takes libdav1d when FFmpeg has it (its max_frame_delay 1, as hle_movie_av1.c sets dav1d), else FFmpeg's own decoder.
#include <AudioToolbox/AudioToolbox.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <math.h>
#include "dm.h"
#include "hle_movie_avc.h"
#include "hle_movie_av1.h"

static void av_red(int *red, const char *what, int r) {
    if ((*red)++) return;
    char e[128]; av_strerror(r, e, sizeof e); LOG("[RED] movie: %s (%s)", what, e);
}
static AVCodecContext *codec_open(const AVCodec *c, const uint8_t *extra, size_t n, AVDictionary **opt) {
    AVCodecContext *x = c ? avcodec_alloc_context3(c) : NULL; if (!x) return NULL;
    x->thread_type = FF_THREAD_SLICE; x->thread_count = 2;
    if (n) {
        x->extradata = av_mallocz(n + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!x->extradata) { avcodec_free_context(&x); return NULL; }
        memcpy(x->extradata, extra, n); x->extradata_size = (int)n;
    }
    if (avcodec_open2(x, c, opt) < 0) avcodec_free_context(&x);
    return x;
}
// A decoded 4:2:0 frame as NV12 of its own size (w, h even): 8-bit planes copied, the chroma interleaved; 10- and 12-bit shifted to 8 bits, as hle_movie_av1.c does dav1d's
static uint8_t *frame_nv12(const AVFrame *f, int *w, int *h) {
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(f->format);
    if (!d || d->nb_components < 3 || d->log2_chroma_w != 1 || d->log2_chroma_h != 1 || (d->flags & AV_PIX_FMT_FLAG_RGB)) return NULL;
    int sh = d->comp[0].depth - 8, ww = f->width & ~1, hh = f->height & ~1, semi = d->comp[1].plane == d->comp[2].plane;
    uint8_t *buf = ww > 0 && hh > 0 ? malloc((size_t)ww * hh * 3 / 2) : NULL; if (!buf) return NULL;
    for (int r = 0; r < hh; r++) {
        const uint8_t *s = f->data[0] + (ptrdiff_t)r * f->linesize[0]; uint8_t *o = buf + (size_t)r * ww;
        if (!sh) memcpy(o, s, (size_t)ww); else for (int x = 0; x < ww; x++) o[x] = (uint8_t)(((const uint16_t *)s)[x] >> d->comp[0].shift >> sh);
    }
    uint8_t *uv = buf + (size_t)ww * hh;
    for (int r = 0; r < hh / 2; r++) {
        uint8_t *o = uv + (size_t)r * ww;
        if (semi) {   // NV12 already (P010 and the like shifted)
            const uint8_t *s = f->data[1] + (ptrdiff_t)r * f->linesize[1];
            if (!sh) memcpy(o, s, (size_t)ww); else for (int x = 0; x < ww; x++) o[x] = (uint8_t)(((const uint16_t *)s)[x] >> d->comp[1].shift >> sh);
            continue;
        }
        const uint8_t *u = f->data[1] + (ptrdiff_t)r * f->linesize[1], *v = f->data[2] + (ptrdiff_t)r * f->linesize[2];
        for (int x = 0; x < ww / 2; x++) {
            o[2 * x] = sh ? (uint8_t)(((const uint16_t *)u)[x] >> sh) : u[x];
            o[2 * x + 1] = sh ? (uint8_t)(((const uint16_t *)v)[x] >> sh) : v[x];
        }
    }
    *w = ww; *h = hh; return buf;
}

// ---------------- H.264 ----------------
#define QMAX 16
typedef struct { uint8_t *buf; int w, h; int64_t pts; } pic_t;   // a picture as it came out: NV12 (Y then interleaved UV, stride w) of w × h
struct mavc {
    uint8_t sps[512], pps[512]; int nsps, npps, dirty, red, rgba;
    AVCodecContext *cx; AVPacket *pkt; AVFrame *fr;
    pic_t q[QMAX]; int nq;
    uint8_t *au; size_t aucap;
};
mavc *mavc_new(void) {
    mavc *a = calloc(1, sizeof *a); if (!a) die("movie: AVC decoder allocation failed");
    if (!(a->pkt = av_packet_alloc()) || !(a->fr = av_frame_alloc())) die("movie: AVC decoder allocation failed");
    return a;
}
static void close_codec(mavc *a) { avcodec_free_context(&a->cx); }
void mavc_drop(mavc *a) {
    for (int i = 0; i < a->nq; i++) free(a->q[i].buf);
    a->nq = 0;
    if (a->cx) avcodec_flush_buffers(a->cx);
}
void mavc_free(mavc *a) { if (!a) return; close_codec(a); mavc_drop(a); av_packet_free(&a->pkt); av_frame_free(&a->fr); free(a->au); free(a); }
int mavc_queued(const mavc *a) { return a->nq; }
int mavc_bt709(const mavc *a, int h) { (void)a; return h >= 720; }   // as media_android.c: BT.709 from 720 lines up, BT.601 below (hle_movie_avc.c's fallback)
void mavc_set_rgba(mavc *a) { a->rgba = 1; }

static void keep_ps(mavc *a, const uint8_t *nal, size_t n) {   // a parameter set NAL unit (without start code)
    int t = nal[0] & 31; uint8_t *dst = t == 7 ? a->sps : a->pps; int *dn = t == 7 ? &a->nsps : &a->npps;
    if (n > sizeof a->sps) { if (!a->red++) LOG("[RED] movie: AVC parameter set of %zu bytes, too long", n); return; }
    if ((size_t)*dn == n && !memcmp(dst, nal, n)) return;
    memcpy(dst, nal, n); *dn = (int)n; a->dirty = 1;
}
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
static void each_nal(const uint8_t *p, size_t n, void (*fn)(void *, const uint8_t *, size_t), void *ctx) {   // as hle_movie_avc.c: Annex B, or else 4-byte lengths
    size_t s = next_sc(p, n, 0);
    if (s < n && s <= 1) {
        while (s < n) { size_t b = s + 3, e = next_sc(p, n, b), end = e; while (end > b && !p[end - 1]) end--; if (end > b) fn(ctx, p + b, end - b); s = e; }
    } else for (size_t i = 0; i + 4 <= n;) {
        size_t len = (size_t)p[i] << 24 | (size_t)p[i + 1] << 16 | (size_t)p[i + 2] << 8 | p[i + 3]; i += 4;
        if (!len || len > n - i) break;
        fn(ctx, p + i, len); i += len;
    }
}
static void ps_only(void *ctx, const uint8_t *nal, size_t n) { int t = nal[0] & 31; if (t == 7 || t == 8) keep_ps(ctx, nal, n); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { each_nal(p, n, ps_only, a); }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) {
    if (n < 7 || p[0] != 1) { if (!a->red++) LOG("[RED] movie: unrecognized avcC record (%zu bytes, version %d)", n, n ? p[0] : -1); return; }
    size_t i = 5;
    for (int set = 0; set < 2 && i < n; set++) {
        int cnt = set ? p[i] : p[i] & 31; i++;
        for (int k = 0; k < cnt && i + 2 <= n; k++) { size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2; if (len > n - i) return; if (len) keep_ps(a, p + i, len); i += len; }
    }
}
static int open_codec(mavc *a) {
    close_codec(a);
    if (!a->nsps || !a->npps) { if (!a->red++) LOG("[RED] movie: AVC has no SPS / PPS, outputting black frames"); return 0; }
    uint8_t x[1040]; size_t n = 0; static const uint8_t sc[4] = {0, 0, 0, 1};   // the parameter sets as Annex B extradata, which FFmpeg's H.264 decoder takes
    memcpy(x, sc, 4); memcpy(x + 4, a->sps, (size_t)a->nsps); n = 4 + (size_t)a->nsps;
    memcpy(x + n, sc, 4); memcpy(x + n + 4, a->pps, (size_t)a->npps); n += 4 + (size_t)a->npps;
    a->cx = codec_open(avcodec_find_decoder(AV_CODEC_ID_H264), x, n, NULL);
    if (!a->cx) { if (!a->red++) LOG("[RED] movie: FFmpeg has no H.264 decoder, outputting black frames"); return 0; }
    LOG("[movie] AVC: libavcodec %s decoding, profile %d", a->cx->codec->name, a->nsps > 1 ? a->sps[1] : 0);
    a->dirty = 0; return 1;
}
static void receive(mavc *a) {   // every picture the decoder has → the queue
    while (a->cx && avcodec_receive_frame(a->cx, a->fr) >= 0) {
        int w = 0, h = 0; uint8_t *buf = frame_nv12(a->fr, &w, &h);
        if (!buf) { if (!a->red++) LOG("[RED] movie: an AVC picture in pixel format %d not converted: dropped", a->fr->format); }
        else {
            if (a->nq == QMAX) { if (!a->red++) LOG("[RED] movie: AVC picture queue full, dropping the oldest picture"); free(a->q[0].buf); memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]); }
            a->q[a->nq++] = (pic_t){buf, w, h, a->fr->best_effort_timestamp};
        }
        av_frame_unref(a->fr);
    }
}
typedef struct { mavc *a; size_t len; } au_ctx;
static void au_add(void *ctx, const uint8_t *nal, size_t n) {
    au_ctx *c = ctx; mavc *a = c->a; int t = nal[0] & 31;
    if (t == 7 || t == 8) { keep_ps(a, nal, n); return; }
    if (t == 9) return;   // access unit delimiter
    if (c->len + 4 + n + AV_INPUT_BUFFER_PADDING_SIZE > a->aucap) { a->aucap = (c->len + 4 + n + AV_INPUT_BUFFER_PADDING_SIZE) * 2; a->au = realloc(a->au, a->aucap); if (!a->au) die("movie: AVC buffer allocation failed"); }
    uint8_t *o = a->au + c->len; o[0] = o[1] = o[2] = 0; o[3] = 1; memcpy(o + 4, nal, n); c->len += 4 + n;
}
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    au_ctx c = {a, 0}; each_nal(p, (size_t)n, au_add, &c);
    if (!c.len) return 0;
    if ((a->dirty || !a->cx) && !open_codec(a)) return -1;
    memset(a->au + c.len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    a->pkt->data = a->au; a->pkt->size = (int)c.len; a->pkt->pts = pts_us;
    int r = avcodec_send_packet(a->cx, a->pkt);
    if (r == AVERROR(EAGAIN)) { receive(a); r = avcodec_send_packet(a->cx, a->pkt); }
    av_packet_unref(a->pkt);
    if (r < 0) { av_red(&a->red, "the H.264 decoder refused an access unit", r); return -1; }
    receive(a);
    return 0;
}
// YUV (video range) → RGBA: BT.709 from 720 lines up, BT.601 below (media_android.c's)
static void nv12_rgba(const uint8_t *src, int sw, int sh, uint8_t *dst, int w, int h) {
    int hd = sh >= 720, cw = sw < w ? sw : w, chh = sh < h ? sh : h;
    int kr = hd ? 459 : 409, kgu = hd ? 55 : 100, kgv = hd ? 136 : 208, kb = hd ? 541 : 516;   // ×256, video range: 1.164 (Y), the matrix's coefficients
    if (cw < w || chh < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
    const uint8_t *uvp = src + (size_t)sw * sh;
    for (int r = 0; r < chh; r++) {
        const uint8_t *y = src + (size_t)r * sw, *uv = uvp + (size_t)(r / 2) * sw; uint8_t *o = dst + (size_t)r * w * 4;
        for (int x = 0; x < cw; x++) {
            int Y = (y[x] - 16) * 298, U = uv[x & ~1] - 128, V = uv[(x & ~1) + 1] - 128;
            int R = (Y + kr * V + 128) >> 8, G = (Y - kgu * U - kgv * V + 128) >> 8, B = (Y + kb * U + 128) >> 8;
            o[4 * x] = (uint8_t)(R < 0 ? 0 : R > 255 ? 255 : R); o[4 * x + 1] = (uint8_t)(G < 0 ? 0 : G > 255 ? 255 : G); o[4 * x + 2] = (uint8_t)(B < 0 ? 0 : B > 255 ? 255 : B); o[4 * x + 3] = 255;
        }
    }
}
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (flush && a->cx) { avcodec_send_packet(a->cx, NULL); receive(a); avcodec_flush_buffers(a->cx); }   // the end of the stream: every picture comes out
    if (!a->nq) return 0;
    pic_t p = a->q[0]; memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]);
    if (pts_us) *pts_us = p.pts;
    if (a->rgba) nv12_rgba(p.buf, p.w, p.h, dst, w, h);
    else {
        int cw = p.w < w ? p.w : w, chh = p.h < h ? p.h : h;
        if (cw < w || chh < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
        for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w, p.buf + (size_t)r * p.w, (size_t)cw);
        for (int r = 0; r < chh / 2; r++) memcpy(dst + (size_t)w * h + (size_t)r * w, p.buf + (size_t)p.w * p.h + (size_t)r * p.w, (size_t)cw);
    }
    free(p.buf);
    return 1;
}

// ---------------- AAC (AudioConverter) ----------------
struct dm_audio_converter {
    AVCodecContext *cx; AVPacket *pkt; AVFrame *fr; int rate, ch; uint8_t asc[64]; int nasc, started, red;
    int16_t *pend; size_t npend, cappend;   // decoded frames not handed out yet (interleaved, ch channels)
};
OSStatus AudioConverterNew(const AudioStreamBasicDescription *in, const AudioStreamBasicDescription *out, AudioConverterRef *c) {
    *c = NULL;
    if (in->mFormatID != kAudioFormatMPEG4AAC || out->mFormatID != kAudioFormatLinearPCM || out->mBitsPerChannel != 16) return -50;
    struct dm_audio_converter *a = calloc(1, sizeof *a); if (!a) return -108;
    a->rate = (int)in->mSampleRate; a->ch = (int)out->mChannelsPerFrame;
    *c = a; return noErr;
}
// the magic cookie: an esds descriptor chain (ES 3 → decoder configuration 4 → decoder specific 5 = the AudioSpecificConfig); media_android.c's reading
static int descr(const uint8_t *p, size_t n, size_t *i, int *tag, size_t *len) {
    if (*i >= n) return 0; *tag = p[(*i)++]; *len = 0;
    for (int k = 0; k < 4 && *i < n; k++) { uint8_t b = p[(*i)++]; *len = *len << 7 | (b & 0x7f); if (!(b & 0x80)) break; }
    return 1;
}
OSStatus AudioConverterSetProperty(AudioConverterRef a, AudioConverterPropertyID id, UInt32 size, const void *data) {
    if (id != kAudioConverterDecompressionMagicCookie) return -50;
    const uint8_t *p = data; size_t i = 0; int tag; size_t len;
    while (descr(p, size, &i, &tag, &len)) {
        if (tag == 3) { i += 3; continue; }          // ES ID, flags: the decoder configuration follows inside
        if (tag == 4) { i += 13; continue; }         // object type, stream type, buffer size, bit rates
        if (tag == 5) { if (len > sizeof a->asc || i + len > size) return -50; memcpy(a->asc, p + i, len); a->nasc = (int)len; return noErr; }
        i += len;
    }
    return -50;
}
static int aac_open(struct dm_audio_converter *a) {
    a->started = -1;
    a->cx = codec_open(avcodec_find_decoder(AV_CODEC_ID_AAC), a->asc, (size_t)a->nasc, NULL);
    if (!a->cx || !(a->pkt = av_packet_alloc()) || !(a->fr = av_frame_alloc())) { LOG("[RED] movie: no AAC decoder in FFmpeg"); return 0; }
    a->started = 1; return 1;
}
static int16_t s16(float v) { v *= 32768.f; return (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int)lrintf(v)); }
static void aac_out(struct dm_audio_converter *a) {   // what the decoder has ready → pend, in the channel count asked (mono duplicated, extra channels dropped)
    while (avcodec_receive_frame(a->cx, a->fr) >= 0) {
        AVFrame *f = a->fr; int fch = f->ch_layout.nb_channels; size_t frames = (size_t)f->nb_samples;
        if (fch > 0 && a->npend + frames > a->cappend) { a->cappend = (a->npend + frames) * 2; a->pend = realloc(a->pend, a->cappend * (size_t)a->ch * 2); }
        for (size_t s = 0; fch > 0 && a->pend && s < frames; s++) for (int c = 0; c < a->ch; c++) {
            int k = c < fch ? c : fch - 1; int16_t v;
            switch (f->format) {
            case AV_SAMPLE_FMT_FLTP: v = s16(((const float *)f->extended_data[k])[s]); break;
            case AV_SAMPLE_FMT_FLT: v = s16(((const float *)f->extended_data[0])[s * fch + k]); break;
            case AV_SAMPLE_FMT_S16P: v = ((const int16_t *)f->extended_data[k])[s]; break;
            case AV_SAMPLE_FMT_S16: v = ((const int16_t *)f->extended_data[0])[s * fch + k]; break;
            default: if (!a->red++) LOG("[RED] movie: AAC samples in format %d not converted", f->format); v = 0; break;
            }
            a->pend[(a->npend + s) * (size_t)a->ch + (size_t)c] = v;
        }
        if (fch > 0 && a->pend) a->npend += frames;
        av_frame_unref(f);
    }
}
OSStatus AudioConverterFillComplexBuffer(AudioConverterRef a, AudioConverterComplexInputDataProc fn, void *ctx, UInt32 *packets, AudioBufferList *out, AudioStreamPacketDescription *desc) {
    (void)desc;
    UInt32 want = *packets; *packets = 0;
    if (!a->started && !aac_open(a)) return -1;
    if (a->started < 0) return -1;
    for (;;) {   // the packets the input procedure has
        UInt32 np = 1; AudioBufferList bl = {1, {{0, 0, NULL}}}; AudioStreamPacketDescription *pd = NULL;
        OSStatus st = fn(a, &np, &bl, &pd, ctx);
        if (st || !np || !bl.mBuffers[0].mData) break;
        if (av_new_packet(a->pkt, (int)bl.mBuffers[0].mDataByteSize) < 0) break;
        memcpy(a->pkt->data, bl.mBuffers[0].mData, bl.mBuffers[0].mDataByteSize);
        int r = avcodec_send_packet(a->cx, a->pkt);
        if (r == AVERROR(EAGAIN)) { aac_out(a); r = avcodec_send_packet(a->cx, a->pkt); }
        av_packet_unref(a->pkt);
        if (r < 0) av_red(&a->red, "the AAC decoder refused a packet", r);
        aac_out(a);
    }
    UInt32 n = (UInt32)(a->npend < want ? a->npend : want);
    size_t room = out->mBuffers[0].mDataByteSize / (2u * (size_t)a->ch); if (n > room) n = (UInt32)room;
    if (n) { memcpy(out->mBuffers[0].mData, a->pend, (size_t)n * 2 * (size_t)a->ch); memmove(a->pend, a->pend + (size_t)n * a->ch, (a->npend - n) * 2 * (size_t)a->ch); a->npend -= n; }
    out->mBuffers[0].mDataByteSize = n * 2u * (UInt32)a->ch; *packets = n;
    return noErr;
}
OSStatus AudioConverterDispose(AudioConverterRef a) {
    if (!a) return noErr;
    avcodec_free_context(&a->cx); av_packet_free(&a->pkt); av_frame_free(&a->fr);
    free(a->pend); free(a); return noErr;
}

// ---------------- AV1 ----------------
struct mav1 { AVCodecContext *cx; AVPacket *pkt; AVFrame *fr; int red, seq; uint8_t *cur; char name[64]; };
mav1 *mav1_new(void) {
    mav1 *a = calloc(1, sizeof *a); if (!a) die("movie: AV1 decoder allocation failed");
    if (!(a->pkt = av_packet_alloc()) || !(a->fr = av_frame_alloc())) die("movie: AV1 decoder allocation failed");
    return a;
}
void mav1_reset(mav1 *a) { avcodec_free_context(&a->cx); a->seq = 0; }
void mav1_free(mav1 *a) { if (!a) return; mav1_reset(a); av_packet_free(&a->pkt); av_frame_free(&a->fr); free(a->cur); free(a); }
const char *mav1_backend(const mav1 *a) { return a->name[0] ? a->name : "libavcodec"; }
int mav1_av1c(const uint8_t *obu, size_t n, uint8_t c[4], int *w, int *h) { (void)obu; (void)n; (void)c; (void)w; (void)h; return 0; }   // as av1_android.c: the decoder reads the sequence header in-band
// whether a temporal unit (OBUs with their sizes) holds a sequence header (OBU type 1): the decoder starts there (av1_android.c's)
static int has_seq_header(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t h = p[i++]; int type = h >> 3 & 15, ext = h >> 2 & 1, sized = h >> 1 & 1;
        if (ext) i++;
        size_t len = n - (i < n ? i : n);
        if (sized) { len = 0; for (int s = 0; s < 56 && i < n; s += 7) { uint8_t b = p[i++]; len |= (size_t)(b & 0x7f) << s; if (!(b & 0x80)) break; } }
        if (type == 1) return 1;
        if (len > n - (i < n ? i : n)) return 0;
        i += len;
    }
    return 0;
}
static int av1_open(mav1 *a) {
    const AVCodec *c = avcodec_find_decoder_by_name("libdav1d"); AVDictionary *opt = NULL;
    if (c) av_dict_set(&opt, "max_frame_delay", "1", 0);   // a temporal unit's picture before its send returns, as hle_movie_av1.c sets dav1d
    else c = avcodec_find_decoder(AV_CODEC_ID_AV1);
    a->cx = codec_open(c, NULL, 0, &opt); av_dict_free(&opt);
    if (!a->cx) { if (!a->red++) LOG("[RED] movie: FFmpeg has no AV1 decoder: the movie stays black"); return 0; }
    snprintf(a->name, sizeof a->name, "libavcodec %s", a->cx->codec->name);
    LOG("[movie] AV1: %s", a->name);
    return 1;
}
int mav1_decode(mav1 *a, const uint8_t *data, size_t n, mav1_picture *out) {
    if (!a->seq) { if (!has_seq_header(data, n)) return 0; a->seq = 1; }
    if (!a->cx && !av1_open(a)) return -1;
    if (av_new_packet(a->pkt, (int)n) < 0) return -1;
    memcpy(a->pkt->data, data, n);
    int r = avcodec_send_packet(a->cx, a->pkt); av_packet_unref(a->pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) { av_red(&a->red, "the AV1 decoder refused a temporal unit", r); return -1; }
    int got = 0, w = 0, h = 0, mc = AVCOL_SPC_UNSPECIFIED, full = 0;
    while (avcodec_receive_frame(a->cx, a->fr) >= 0) {   // one shown picture a temporal unit; a later one replaces it
        uint8_t *b = frame_nv12(a->fr, &w, &h);
        if (b) { free(a->cur); a->cur = b; got = 1; mc = a->fr->colorspace; full = a->fr->color_range == AVCOL_RANGE_JPEG; }
        else if (!a->red++) LOG("[RED] movie: an AV1 picture in pixel format %d not converted: dropped", a->fr->format);
        av_frame_unref(a->fr);
    }
    if (!got) return 0;
    if (mc == AVCOL_SPC_UNSPECIFIED) mc = h < 720 ? AVCOL_SPC_BT470BG : AVCOL_SPC_BT709;   // hle_movie_av1.c's rule for an unspecified matrix
    *out = (mav1_picture){a->cur, a->cur + (size_t)w * h, (size_t)w, (size_t)w, w, h, mc == AVCOL_SPC_BT709 ? 2 : mc == AVCOL_SPC_BT2020_NCL || mc == AVCOL_SPC_BT2020_CL ? 5 : 1, full};
    return 1;
}
