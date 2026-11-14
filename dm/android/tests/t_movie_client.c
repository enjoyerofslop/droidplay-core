// A movie client on the host's movie decoder (hle_movie.c on media_android.c), fed the way The World Ends with You: Final Remix's player feeds it (droidplay-core#480):
//   t_movie_client <mp4> <packets.csv> [strip] [noeos] [lead=<ms>] [chunk=<packets>] [gate=<ms>]
// The clip is an MP4 of H.264 and AAC (the README's Checks make it with ffmpeg), its packets listed by ffprobe in file order
// (stream_index,pts_time,dts_time,size,pos). The client, as the game's DM_MOVIE_TRACE logs show it:
//   · two decoders, AAC (PCM16) and H.264 (NV12), 4 inputs each; the SPS / PPS in Configure's csd-0 / csd-1 (strip: the SPS without its bitstream_restriction);
//   · one feeder thread sends the packets in the game's file order (below), each waiting for an input of its own track;
//   · one acquirer thread takes every input and output it is offered (it holds up to all 32 AAC outputs and 16 video outputs);
//   · a video consumer shows each picture once the movie's clock reaches its time (the clock runs from the first picture, as the game's ran on after its audio had stopped),
//     and releases the one it showed before;
//   · an audio consumer releases an audio output once the clock is near its time and, past the movie's first half second, a picture at least gate= ms later (450) has been
//     acquired. In both logs the audio consumer moved with the newest picture (it released its outputs 0.39-0.5 s behind it, and the less the video led, the more pictures
//     came before the release), and both froze holding the output 0.45 s behind the newest picture, while the next pictures were still in the decoder. The logs do not show
//     what the game waits on, so the gate is a model: one that freezes the way the game froze;
//   · at the end an end-of-stream packet on each track (noeos: none, as some players send none).
// It reports the video decoder's lag (packets sent minus pictures acquired; and the same less the packets still queued in hle_movie.c for an output slot, what the decoder itself
// holds), every picture's order and content (the clip codes each frame's number in two grey blocks), and either the end or a deadlock: no call for 3 s before the end, with what
// each side holds. Exit 0 when every picture played (noeos: all but 2).
#include "hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "hle_movie.c"
#include "../avc_sps.h"
#include <sys/mman.h>
#include <time.h>

// ---- what the host's other files and the game's SDK give hle_movie.c ----
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
__thread _Atomic int dm_host_work;
void dm_host_lock(pthread_mutex_t *m) { pthread_mutex_lock(m); }
void dm_host_helper_qos(qos_class_t q) { (void)q; }
typedef struct { void (*fn)(void *); void *arg; } spawn_t;
static void *spawn_main(void *p) { spawn_t s = *(spawn_t *)p; free(p); s.fn(s.arg); return NULL; }
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size) {
    (void)prio; (void)stack_size; spawn_t *s = malloc(sizeof *s); s->fn = fn; s->arg = arg; pthread_t t;
    if (pthread_create(&t, NULL, spawn_main, s)) die("spawn"); pthread_detach(t); return (void *)1;
}
static uint8_t vtable[32];
void *guest_sym(const char *name) { return !strcmp(name, "_ZTVN5movie7DecoderE") ? vtable : NULL; }
int nvn_host_write_texture(const void *t, const void *s, int w, int h, size_t row, int *tw, int *th) { (void)t; (void)s; (void)w; (void)h; (void)row; (void)tw; (void)th; return -1; }
int nvn_host_signal_sync(void *s) { (void)s; return 0; }
// movie::Buffer, movie::MediaData and nn::os events, as the client sees them. Events and the client's event table live in mmap'd memory: hle_movie.c reads them with
// vm_read_overwrite (process_vm_readv), which takes no tagged heap pointers
typedef struct { uint8_t *data; size_t cap; int off, size; } mbuf;
typedef struct { const char *k[8]; int v[8]; mbuf *b[8]; int n; } mdata;
typedef struct { _Atomic int set; } evt;
static pthread_mutex_t cmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t ccv = PTHREAD_COND_INITIALIZER;   // the client's own lock and wake-up
static _Atomic unsigned long progress;
static void c_signal(void *e) { atomic_store(&((evt *)e)->set, 1); pthread_mutex_lock(&cmu); pthread_cond_broadcast(&ccv); pthread_mutex_unlock(&cmu); }
static void c_set_data(void *b, void *d, size_t cap) { ((mbuf *)b)->data = d; ((mbuf *)b)->cap = cap; }
static void c_set_range(void *b, int off, int size) { ((mbuf *)b)->off = off; ((mbuf *)b)->size = size; }
static void *c_data(const void *b) { return ((mbuf *)b)->data + ((mbuf *)b)->off; }
static size_t c_cap(const void *b) { return ((mbuf *)b)->cap; }
static size_t c_size(const void *b) { return (size_t)((mbuf *)b)->size; }
static int c_find_i32(const void *md, const char *k, int *v) { const mdata *m = md; for (int i = 0; i < m->n; i++) if (!strcmp(m->k[i], k) && !m->b[i]) { *v = m->v[i]; return 1; } return 0; }
static int c_find_buf(const void *md, const char *k, void **b) { const mdata *m = md; for (int i = 0; i < m->n; i++) if (!strcmp(m->k[i], k) && m->b[i]) { *b = m->b[i]; return 1; } return 0; }
static void c_set_i32(void *md, const char *k, int v) { (void)md; (void)k; (void)v; }
static void c_set_rect(void *md, const char *k, int l, int t, int r, int b) { (void)md; (void)k; (void)l; (void)t; (void)r; (void)b; }

// ---- the clip ----
typedef struct { int st; int64_t pts, key; size_t size, pos; } pkt;
static pkt *P; static int NP, NV; static uint8_t *M; static size_t MN;
static int64_t g_lead = 130000, g_gate = 450000; static int g_strip, g_eos = 1, g_chunk = 5;
static int cmp_key(const void *a, const void *b) { const pkt *x = a, *y = b; return x->key < y->key ? -1 : x->key > y->key ? 1 : x->pos < y->pos ? -1 : 1; }
static void load(const char *mp4, const char *csv) {
    FILE *f = fopen(mp4, "rb"); if (!f) { printf("cannot open %s\n", mp4); exit(2); }
    fseek(f, 0, SEEK_END); MN = (size_t)ftell(f); fseek(f, 0, SEEK_SET); M = malloc(MN); if (fread(M, 1, MN, f) != MN) exit(2); fclose(f);
    f = fopen(csv, "r"); if (!f) { printf("cannot open %s\n", csv); exit(2); }
    char line[256]; P = calloc(40000, sizeof *P);
    while (fgets(line, sizeof line, f) && NP < 40000) {
        int st; double pts, dts; long size, pos;
        if (sscanf(line, "%d,%lf,%lf,%ld,%ld", &st, &pts, &dts, &size, &pos) != 5 || st > 1) continue;
        int64_t p = (int64_t)(pts * 1e6 + (pts < 0 ? -0.5 : 0.5)), d = (int64_t)(dts * 1e6 + (dts < 0 ? -0.5 : 0.5));
        P[NP++] = (pkt){st, p, st ? p - g_lead : d, (size_t)size, (size_t)pos}; NV += !st;
    }
    fclose(f);
    // The game's file keeps its audio in chunks of 5 packets (107 ms), each placed where the video's decode time is 130 ms before the chunk's last packet: in its logs the
    // audio sent leads the video sent by 130 ms right after a chunk and by 15 ms just before the next. The feeder then needs 5 audio inputs, more than a decoder has, to reach the next video packet
    for (int i = 0; i < NP; i++) if (P[i].st) {
        int last = i; for (int j = i, c = 0; j < NP && c < g_chunk; j++) if (P[j].st) { last = j; c++; }
        for (int j = i; j <= last; j++) if (P[j].st) P[j].key = P[last].pts - g_lead;
        i = last;
    }
    qsort(P, (size_t)NP, sizeof *P, cmp_key);   // file order
}
// the SPS NAL unit with its bitstream_restriction_flag cleared and what followed it dropped (t_media's strip_restriction)
static size_t strip_sps(const uint8_t *nal, size_t n, uint8_t *o) {
    avc_rbsp r; avc_sps sp;
    if (!avc_sps_parse(nal, n, &r, &sp) || !sp.br) { memcpy(o, nal, n); return n; }
    uint8_t rb[sizeof r.b] = {0}; size_t bits = sp.br_at + 2, on = 0;
    for (size_t k = 0; k < sp.br_at; k++) if (r.b[k / 8] >> (7 - k % 8) & 1) rb[k / 8] |= (uint8_t)(0x80 >> (k % 8));
    rb[(sp.br_at + 1) / 8] |= (uint8_t)(0x80 >> ((sp.br_at + 1) % 8));
    o[on++] = nal[0];
    for (size_t k = 0, z = 0; k < (bits + 7) / 8; k++) { if (z >= 2 && rb[k] <= 3) { o[on++] = 3; z = 0; } o[on++] = rb[k]; z = rb[k] ? 0 : z + 1; }
    return on;
}

// ---- the client ----
enum { A, V };
static void *dec[2][2]; static evt *ev[2][4]; static void **evtab[2];
static int freein[2][8], nfree[2];                     // input indices acquired, not yet sent
static struct { int idx; int64_t pts; uint32_t flags; } outq[2][64]; static int nout[2];   // outputs acquired, not yet consumed
static int held[2], held_max[2];                       // outputs acquired and not released
static int sent[2], acquired[2], vshown, vbad_order, vbad_content, aconsumed, feeder_done, feeder_wait = -1, veos, aeos;
static int64_t newest_v = INT64_MIN, shown_pts = INT64_MIN, clock0_pts; static uint64_t clock0_ns;
static int lag_hist[64], lag_max, dlag_hist[64], dlag_max; static long lag_sum, lag_n, dlag_sum;
static int64_t lead_min = INT64_MAX; static uint64_t gate_ns;   // the least video lead an audio output was released at; the audio consumer's time waiting on the video alone
static int64_t movie_clock(void) { return clock0_ns ? clock0_pts + (int64_t)((now_ns() - clock0_ns) / 1000) : INT64_MIN; }
static void cwait(int ms) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += ms * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; } pthread_cond_timedwait(&ccv, &cmu, &ts); }
static void *acquirer(void *arg) {
    (void)arg;
    for (;;) {
        for (int t = 0; t < 2; t++) {
            if (atomic_exchange(&ev[t][2]->set, 0)) { mdata md = {0}; h_movie_OutFmt(dec[t], &md); }
            int ein = atomic_exchange(&ev[t][0]->set, 0), eout = atomic_exchange(&ev[t][1]->set, 0);
            (void)ein; (void)eout;   // each wake-up looks at both: an event and a call can cross
            int idx, left; int64_t pts; uint32_t fl;
            while (!h_movie_AcqIn(dec[t], &idx, &left)) { pthread_mutex_lock(&cmu); freein[t][nfree[t]++] = idx; progress++; pthread_cond_broadcast(&ccv); pthread_mutex_unlock(&cmu); }
            while (!h_movie_AcqOut(dec[t], &idx, &pts, &fl, &left)) {
                pthread_mutex_lock(&cmu);
                outq[t][nout[t]].idx = idx; outq[t][nout[t]].pts = pts; outq[t][nout[t]].flags = fl; nout[t]++; held[t]++; if (held[t] > held_max[t]) held_max[t] = held[t];
                progress++;
                if (t == V && !(fl & 4)) {
                    acquired[V]++; if (pts > newest_v) newest_v = pts;
                    if (!clock0_ns) { clock0_ns = now_ns(); clock0_pts = pts; }
                    int lag = sent[V] - acquired[V]; if (lag > lag_max) lag_max = lag; lag_hist[lag < 63 ? lag : 63]++; lag_sum += lag; lag_n++;
                    mdec_t *vd = md_of(dec[V]); pthread_mutex_lock(&vd->mu); int dlag = lag - vd->npend; pthread_mutex_unlock(&vd->mu);   // less the packets hle_movie.c still queues
                    if (dlag < 0) dlag = 0; if (dlag > dlag_max) dlag_max = dlag; dlag_hist[dlag < 63 ? dlag : 63]++; dlag_sum += dlag;
                }
                if (t == A && !(fl & 4)) acquired[A]++;
                if (fl & 4) { if (t == V) veos = 1; else aeos = 1; }
                pthread_cond_broadcast(&ccv); pthread_mutex_unlock(&cmu);
            }
        }
        pthread_mutex_lock(&cmu); cwait(5); pthread_mutex_unlock(&cmu);
    }
    return NULL;
}
static void *feeder(void *arg) {
    (void)arg; mbuf b = {0}; uint8_t *tmp = malloc(MD_INCAP);
    for (int i = 0; i <= NP; i++) {
        for (int t = 0; t < 2 && i == NP; t++) {   // the end: an end-of-stream packet on each track, or none
            if (!g_eos) break;
            pthread_mutex_lock(&cmu); feeder_wait = t; while (!nfree[t]) cwait(20); int idx = freein[t][0]; memmove(freein[t], freein[t] + 1, (size_t)--nfree[t] * sizeof(int)); feeder_wait = -1; pthread_mutex_unlock(&cmu);
            h_movie_SendIn(dec[t], idx, 0, 0, 0, 4); progress++;
        }
        if (i == NP) break;
        int t = P[i].st == 0 ? V : A;
        pthread_mutex_lock(&cmu); feeder_wait = t; while (!nfree[t]) cwait(20); int idx = freein[t][0]; memmove(freein[t], freein[t] + 1, (size_t)--nfree[t] * sizeof(int)); feeder_wait = -1; pthread_mutex_unlock(&cmu);
        h_movie_GetIn(dec[t], idx, &b);
        size_t n = 0; const uint8_t *s = M + P[i].pos;
        if (t == V) for (size_t k = 0; k + 4 <= P[i].size;) {   // AVCC lengths → Annex B start codes, as the SDK's demuxer hands H.264 over
            size_t len = (size_t)s[k] << 24 | (size_t)s[k + 1] << 16 | (size_t)s[k + 2] << 8 | s[k + 3]; k += 4; if (len > P[i].size - k) break;
            memcpy(tmp + n, "\0\0\0\1", 4); memcpy(tmp + n + 4, s + k, len); n += 4 + len; k += len;
        } else { memcpy(tmp, s, P[i].size); n = P[i].size; }
        memcpy(c_data(&b), tmp, n);
        pthread_mutex_lock(&cmu); sent[t]++; pthread_mutex_unlock(&cmu);
        h_movie_SendIn(dec[t], idx, 0, (int)n, P[i].pts, 0); progress++;
    }
    pthread_mutex_lock(&cmu); feeder_done = 1; pthread_mutex_unlock(&cmu);
    return NULL;
}
static void *video_consumer(void *arg) {
    (void)arg; mbuf b = {malloc(1280 * 720 * 3 / 2), 1280 * 720 * 3 / 2, 0, 0}; int prev = -1;
    for (;;) {
        pthread_mutex_lock(&cmu);
        while (nout[V] && (outq[V][0].flags & 4 || outq[V][0].pts <= movie_clock())) {
            int idx = outq[V][0].idx; int64_t pts = outq[V][0].pts; uint32_t fl = outq[V][0].flags; memmove(outq[V], outq[V] + 1, (size_t)--nout[V] * sizeof outq[V][0]);
            pthread_mutex_unlock(&cmu);
            if (!(fl & 4)) {
                h_movie_GetOut(dec[V], idx, &b);
                const uint8_t *y = c_data(&b); int code = (y[32 * 1280 + 32] - 12) / 8 + 28 * ((y[32 * 1280 + 96] - 12) / 8), frame = (int)((pts + 16666) / 33333);
                pthread_mutex_lock(&cmu);
                if (pts <= shown_pts) vbad_order++;
                if (code != frame % 784) { if (!vbad_content++) printf("picture %d (time %lld) codes frame %d\n", frame, (long long)pts, code); }
                shown_pts = pts; vshown++; pthread_mutex_unlock(&cmu);
            }
            if (prev >= 0) { h_movie_RelOut(dec[V], prev); pthread_mutex_lock(&cmu); held[V]--; pthread_mutex_unlock(&cmu); }
            prev = fl & 4 ? -1 : idx;
            if (fl & 4) { h_movie_RelOut(dec[V], idx); pthread_mutex_lock(&cmu); held[V]--; pthread_mutex_unlock(&cmu); }
            progress++;
            pthread_mutex_lock(&cmu);
        }
        cwait(16); pthread_mutex_unlock(&cmu);
    }
    return NULL;
}
static void *audio_consumer(void *arg) {
    (void)arg; mbuf b = {malloc(65536), 65536, 0, 0};
    for (;;) {
        pthread_mutex_lock(&cmu);
        // near the clock (150 ms ahead, an audio renderer's queue), and the video decoded far enough past it, unless the file is all sent
        for (;;) {
            if (nout[A] && outq[A][0].flags & 4) break;
            int near = nout[A] && outq[A][0].pts <= movie_clock() + 150000, ahead = nout[A] && (feeder_done || outq[A][0].pts < 500000 || newest_v >= outq[A][0].pts + g_gate);
            if (near && ahead) break;
            uint64_t t = now_ns(); cwait(5); if (near && !ahead) gate_ns += now_ns() - t;
        }
        if (!(outq[A][0].flags & 4) && !feeder_done && outq[A][0].pts >= 500000 && newest_v - outq[A][0].pts < lead_min) lead_min = newest_v - outq[A][0].pts;
        int idx = outq[A][0].idx; memmove(outq[A], outq[A] + 1, (size_t)--nout[A] * sizeof outq[A][0]);
        pthread_mutex_unlock(&cmu);
        h_movie_GetOut(dec[A], idx, &b); h_movie_RelOut(dec[A], idx);
        pthread_mutex_lock(&cmu); held[A]--; aconsumed++; progress++; pthread_mutex_unlock(&cmu);
    }
    return NULL;
}
static void *zalloc_low(size_t n) { void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); if (p == MAP_FAILED) die("mmap"); return p; }
static void make(int t, int type, int fmt, mdata *md) {
    h_movie_Decoder(dec[t], type, fmt, 1, 0);
    for (int i = 0; i < 4; i++) { ev[t][i] = zalloc_low(4096); evtab[t][i] = ev[t][i]; }
    h_movie_RegEv(dec[t], evtab[t]); h_movie_Configure(dec[t], md);
}
int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: t_movie_client <mp4> <packets.csv> [strip] [noeos] [lead=<ms>] [gate=<ms>]\n"); return 2; }
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "strip")) g_strip = 1; else if (!strcmp(argv[i], "noeos")) g_eos = 0;
        else if (!strncmp(argv[i], "lead=", 5)) g_lead = atoll(argv[i] + 5) * 1000; else if (!strncmp(argv[i], "gate=", 5)) g_gate = atoll(argv[i] + 5) * 1000;
        else if (!strncmp(argv[i], "chunk=", 6)) g_chunk = atoi(argv[i] + 6) > 0 ? atoi(argv[i] + 6) : 1;
    }
    load(argv[1], argv[2]);
    G.signal = c_signal; G.set_data = c_set_data; G.set_range = c_set_range; G.data = c_data; G.cap = c_cap; G.size = c_size;
    G.set_i32 = c_set_i32; G.set_rect = c_set_rect; G.find_i32 = c_find_i32; G.find_buf = c_find_buf;
    for (int t = 0; t < 2; t++) { dec[t][0] = dec[t][1] = NULL; evtab[t] = zalloc_low(4096); }
    // the parameter sets from the MP4's avcC, as csd-0 / csd-1 (Annex B)
    const uint8_t *avcc = NULL; for (size_t i = 4; i + 8 < MN && !avcc; i++) if (!memcmp(M + i, "avcC", 4)) avcc = M + i + 4;
    if (!avcc) { printf("no avcC box\n"); return 2; }
    static uint8_t c0[1024], c1[1024]; size_t n0 = 0, n1 = 0; const uint8_t *p = avcc + 5;
    for (int k = 0, ns = *p++ & 31; k < ns; k++) { size_t len = (size_t)p[0] << 8 | p[1]; p += 2; memcpy(c0 + n0, "\0\0\0\1", 4); n0 += 4; n0 += g_strip ? strip_sps(p, len, c0 + n0) : (memcpy(c0 + n0, p, len), len); p += len; }
    for (int k = 0, np = *p++; k < np; k++) { size_t len = (size_t)p[0] << 8 | p[1]; p += 2; memcpy(c1 + n1, "\0\0\0\1", 4); memcpy(c1 + n1 + 4, p, len); n1 += 4 + len; p += len; }
    avc_rbsp r; avc_sps sps; avc_sps_parse(c0 + 4, n0 - 4, &r, &sps);
    printf("clip: %d packets, %d video; SPS profile %d, pic_order_cnt_type %d, bitstream_restriction %d (max_num_reorder_frames %d); audio lead %lld ms, gate %lld ms, %s\n",
           NP, NV, sps.profile, sps.poc_type, sps.br, sps.reorder, (long long)g_lead / 1000, (long long)g_gate / 1000, g_eos ? "end of stream sent" : "no end of stream");
    static uint8_t asc[2] = {0x11, 0x90};   // AAC-LC, 48 kHz, 2 channels
    mbuf ascb = {asc, 2, 0, 2}, c0b = {c0, n0, 0, (int)n0}, c1b = {c1, n1, 0, (int)n1};
    mdata amd = {{"channel-count", "sample-rate", "csd-0"}, {2, 48000, 0}, {NULL, NULL, &ascb}, 3};
    mdata vmd = {{"width", "height", "csd-0", "csd-1"}, {1280, 720, 0, 0}, {NULL, NULL, &c0b, &c1b}, 4};
    static void *sa[2], *sv[2]; dec[A][0] = NULL;
    make(A, MD_TYPE_AAC, MD_FMT_PCM16, &amd); make(V, MD_TYPE_AVC, MD_FMT_NV12, &vmd);
    h_movie_Start(dec[A]); h_movie_Start(dec[V]);
    pthread_t th[4]; pthread_create(&th[0], NULL, acquirer, NULL); pthread_create(&th[1], NULL, video_consumer, NULL); pthread_create(&th[2], NULL, audio_consumer, NULL); pthread_create(&th[3], NULL, feeder, NULL);
    uint64_t t0 = now_ns(), last_ns = now_ns(); unsigned long last = 0; int result = 1;
    for (;;) {
        usleep(100000);
        unsigned long pr = atomic_load(&progress);
        if (pr != last) { last = pr; last_ns = now_ns(); }
        pthread_mutex_lock(&cmu);
        int done = feeder_done && vshown == NV && (!g_eos || veos), idle = now_ns() - last_ns > 3000000000ull;
        if (done || idle) {
            mdec_t *vd = md_of(dec[V]), *ad = md_of(dec[A]);
            printf("%s after %.1f s: video %d packets sent, %d pictures acquired, %d shown (of %d), %d never delivered; audio %d sent, %d outputs acquired, %d consumed\n",
                   done ? "END" : feeder_done ? "STALL AT THE END" : "DEADLOCK", (now_ns() - t0) / 1e9, sent[V], acquired[V], vshown, NV, NV - acquired[V], sent[A], acquired[A], aconsumed);
            printf("  client holds %d audio / %d video outputs (most %d / %d), inputs ready %d audio / %d video; feeder %s; decoders: audio %d packets queued, video %d queued, %d pictures in the AVC queue\n",
                   held[A], held[V], held_max[A], held_max[V], nfree[A], nfree[V], feeder_done ? "done" : feeder_wait == A ? "waits for an audio input" : feeder_wait == V ? "waits for a video input" : "running",
                   ad ? ad->npend : -1, vd ? vd->npend : -1, vd && vd->avc ? mavc_queued(vd->avc) : -1);
            printf("  newest picture %.3f s, last shown %.3f s, next audio %.3f s; order errors %d, content errors %d; audio released with the video %lld ms ahead at least, %.2f s waiting on the video\n",
                   newest_v / 1e6, shown_pts / 1e6, nout[A] ? outq[A][0].pts / 1e6 : -1.0, vbad_order, vbad_content, lead_min == INT64_MAX ? -1LL : (long long)lead_min / 1000, gate_ns / 1e9);
            printf("  video lag (packets sent - pictures acquired, at each picture): most %d, mean %.2f; histogram", lag_max, lag_n ? (double)lag_sum / lag_n : 0.0);
            for (int k = 0; k < 64; k++) if (lag_hist[k]) printf(" %d:%d", k, lag_hist[k]);
            printf("\n  in the decoder (less the packets queued for an output slot): most %d, mean %.2f; histogram", dlag_max, lag_n ? (double)dlag_sum / lag_n : 0.0);
            for (int k = 0; k < 64; k++) if (dlag_hist[k]) printf(" %d:%d", k, dlag_hist[k]);
            printf("\n");
            result = !(vbad_order == 0 && vbad_content == 0 && (done || (!g_eos && feeder_done && NV - acquired[V] <= 2)));
            pthread_mutex_unlock(&cmu);
            break;
        }
        pthread_mutex_unlock(&cmu);
    }
    printf("== %s ==\n", result ? "FAIL" : "PASS");
    fflush(stdout);
    _exit(result);   // the decoders' threads may be blocked in a deadlock: no teardown
}
