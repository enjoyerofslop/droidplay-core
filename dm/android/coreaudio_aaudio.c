// AudioToolbox's output AudioUnit on AAudio (compat/AudioToolbox/AudioToolbox.h): the unit's render callback becomes the stream's data callback.
// The stream is opened at Initialize with the format set on the input scope (PCM16, the host's rate and channel count), in low-latency
// shared mode; AAudio resamples to the device when the rate differs. A disconnected stream (headphones unplugged, route change) is reopened
// on the next Start; hle_audio.c's watchdog notices the missing callbacks and calls Stop / Start.
// A data callback larger than 5 ms (an AYN Thor Pro's 960-frame burst) switches the unit to its pacer for good: a thread renders 5 ms quanta into a ring
// kept one callback ahead, and the data callback copies from it (au_pace.h says why). DM_AAUDIO_RING=0 keeps rendering straight from each data callback.
#include <AudioToolbox/AudioToolbox.h>
#include <aaudio/AAudio.h>
#include <semaphore.h>
#include <sys/resource.h>
#include <time.h>
#include "au_pace.h"
#include "dm.h"

enum { RING_QUANTA = 64, PACE_REPORT_TICKS = 2000 };
struct dm_au_component { int unused; };
struct dm_au {
    AudioStreamBasicDescription fmt;
    AURenderCallbackStruct cb;
    AAudioStream *st;
    pthread_mutex_t mu;
    int ring_off, dbg_frames;                 // DM_AAUDIO_RING=0, DM_DBG_AU_FRAMES
    aup_ring ring; uint32_t quantum, burst;   // the pacer's ring, made with the stream format at Initialize; the stream's burst, the size its callbacks come in
    _Atomic int paced, primed;                // paced: a data callback asked for more than a quantum; primed: one found the ring full since the start
    _Atomic uint32_t cb_max;                  // the most frames a data callback has asked for
    _Atomic uint64_t short_n, short_frames;   // data callbacks the ring could not fill, and the frames padded with zeros
    pthread_t pacer; _Atomic int pacer_run; int pacer_on, pacer_logged; sem_t pacer_sem;
};
static struct dm_au_component g_output;

AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d) {
    return !after && d->componentType == kAudioUnitType_Output ? &g_output : NULL;
}
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out) {
    if (c != &g_output) return -1;
    struct dm_au *au = calloc(1, sizeof *au); if (!au) return -1;
    pthread_mutex_init(&au->mu, NULL); sem_init(&au->pacer_sem, 0, 0);
    const char *e = getenv("DM_AAUDIO_RING"); au->ring_off = e && !atoi(e);
    e = getenv("DM_DBG_AU_FRAMES"); au->dbg_frames = e ? atoi(e) : 0;
    *out = au; return noErr;
}
static void render(struct dm_au *au, void *audio, UInt32 frames) {
    UInt32 bytes = frames * au->fmt.mBytesPerFrame;
    AudioBufferList bl = {1, {{au->fmt.mChannelsPerFrame, bytes, audio}}};
    AudioUnitRenderActionFlags fl = 0; AudioTimeStamp ts = {0};
    if (!au->cb.inputProc || au->cb.inputProc(au->cb.inputProcRefCon, &fl, &ts, 0, frames, &bl)) memset(audio, 0, bytes);
}
static aaudio_data_callback_result_t data_cb(AAudioStream *st, void *ud, void *audio, int32_t frames) {
    (void)st; struct dm_au *au = ud;
    uint32_t n = (uint32_t)frames;
    if (n > atomic_load_explicit(&au->cb_max, memory_order_relaxed)) atomic_store_explicit(&au->cb_max, n, memory_order_relaxed);
    if (atomic_load_explicit(&au->paced, memory_order_acquire)) {
        uint32_t got = aup_read(&au->ring, audio, n);
        if (got < n) {
            memset((int16_t *)audio + (size_t)got * au->ring.ch, 0, (size_t)(n - got) * au->fmt.mBytesPerFrame);
            if (atomic_load_explicit(&au->primed, memory_order_relaxed)) { atomic_fetch_add(&au->short_n, 1); atomic_fetch_add(&au->short_frames, n - got); }
        } else atomic_store_explicit(&au->primed, 1, memory_order_relaxed);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    render(au, audio, n);
    // The render above is this thread's last: the pacer starts only once it sees paced, so the host's callback never runs on two threads at once
    if (au->pacer_on && aup_wanted(n, au->quantum)) { atomic_store_explicit(&au->paced, 1, memory_order_release); sem_post(&au->pacer_sem); }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
// Renders the quanta that keep the ring one data callback ahead, on a steady 5 ms clock. A stream that stops calling back leaves the ring full, so the pacer
// stops rendering too: hle_audio.c's watchdog still sees its callbacks stop and restarts the unit, as without the ring. When the process was stopped for
// more than 100 ms, the clock restarts from now instead of making the time up (hle_audio.c's silent sink does the same)
static void *pacer_main(void *p) {
    struct dm_au *au = p;
    setpriority(PRIO_PROCESS, (id_t)gettid(), -16);   // Android's THREAD_PRIORITY_AUDIO: the ring has only a few ms of slack beyond the device's callback
    while (atomic_load(&au->pacer_run) && !atomic_load_explicit(&au->paced, memory_order_acquire)) sem_wait(&au->pacer_sem);
    const uint32_t q = au->quantum, rate = (uint32_t)au->fmt.mSampleRate;
    const uint64_t tick = (uint64_t)q * 1000000000ull / rate;
    if (atomic_load(&au->pacer_run) && !au->pacer_logged) {
        uint32_t m = atomic_load(&au->cb_max); au->pacer_logged = 1;
        if (au->burst > m) m = au->burst;
        LOG("audio: AAudio data callbacks of up to %u frames (%.1f ms), more than %u: the host renders %u-frame quanta on a pacer thread into a ring kept %u frames ahead (DM_AAUDIO_RING=0: off)",
            m, m * 1000.0 / rate, q, q, aup_target(m, q, au->ring.cap));
    }
    uint64_t t = mono_ns(), last = t, ticks = 0, short0 = atomic_load(&au->short_n), frames0 = atomic_load(&au->short_frames);
    while (atomic_load(&au->pacer_run)) {
        t += tick;
        uint64_t now = mono_ns();
        if (t > now) { struct timespec ts = {(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)}; clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL); }
        else if (now - t > 100000000ull) t = now;
        now = mono_ns();
        uint64_t due = (now - last) * rate / 1000000000ull; last = now;
        uint32_t m = atomic_load_explicit(&au->cb_max, memory_order_relaxed);
        if (au->burst > m) m = au->burst;   // a stream whose callbacks vary (AAudio's legacy path: 64 to 960 frames) takes up to a burst at once
        uint32_t target = aup_target(m, q, au->ring.cap);
        for (uint32_t i = aup_quanta(aup_fill(&au->ring), target, m, q, due); i && atomic_load(&au->pacer_run); i--) { render(au, aup_slot(&au->ring), q); aup_commit(&au->ring); }
        // Only a window with short callbacks is reported: a healthy stream adds no lines beside hle_audio.c's own statistics
        if (++ticks % PACE_REPORT_TICKS == 0) {
            uint64_t s = atomic_load(&au->short_n), f = atomic_load(&au->short_frames);
            if (s != short0) LOG("audio: AAudio ring: %llu data callbacks short in the last %u quanta, %.1f ms padded; callbacks of up to %u frames, ring %u frames ahead",
                                 (unsigned long long)(s - short0), (unsigned)PACE_REPORT_TICKS, (f - frames0) * 1000.0 / rate, atomic_load(&au->cb_max), target);
            short0 = s; frames0 = f;
        }
    }
    return NULL;
}
// Under mu, with the stream stopped: the ring is reset while neither side runs
static void pacer_start(struct dm_au *au) {
    if (au->ring_off || !au->ring.pcm || au->pacer_on) return;
    aup_reset(&au->ring); atomic_store(&au->primed, 0);
    atomic_store(&au->pacer_run, 1);
    au->pacer_on = !pthread_create(&au->pacer, NULL, pacer_main, au);
    if (!au->pacer_on) LOG("[RED] audio: no pacer thread: AAudio data callbacks over %u frames render straight from the callback", au->quantum);
    else if (atomic_load(&au->paced)) sem_post(&au->pacer_sem);
}
static void pacer_stop(struct dm_au *au) {
    if (!au->pacer_on) return;
    atomic_store(&au->pacer_run, 0); sem_post(&au->pacer_sem);
    pthread_join(au->pacer, NULL); au->pacer_on = 0;
    while (!sem_trywait(&au->pacer_sem)) {}
}
// hle_audio.c relies on Core Audio's AudioOutputUnitStop, which returns once the render callback has finished: the silent sink then takes the queue.
// AAudio's requestStop returns at once, so wait for the stream to stop (bounded: a disconnected stream never reports it)
static aaudio_result_t stop_stream(struct dm_au *au) {
    aaudio_result_t r = au->st ? AAudioStream_requestStop(au->st) : AAUDIO_OK;
    if (!au->st || r != AAUDIO_OK) return r;
    aaudio_stream_state_t s = AAudioStream_getState(au->st), next = s;
    for (int i = 0; i < 4 && s == AAUDIO_STREAM_STATE_STOPPING; i++, s = next)
        if (AAudioStream_waitForStateChange(au->st, s, &next, 50000000) != AAUDIO_OK) break;
    return r;
}
static void close_stream(struct dm_au *au) { if (au->st) { AAudioStream_close(au->st); au->st = NULL; } }
static OSStatus open_stream(struct dm_au *au) {
    AAudioStreamBuilder *b; aaudio_result_t r = AAudio_createStreamBuilder(&b);
    if (r != AAUDIO_OK) return r;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)au->fmt.mSampleRate);
    AAudioStreamBuilder_setChannelCount(b, (int32_t)au->fmt.mChannelsPerFrame);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SONIFICATION);
    // DM_DBG_AU_FRAMES=960 makes a phone with a small burst call back as a 20 ms burst does (an AYN Thor Pro's), as 1024 reproduces the iOS default on the Mac
    if (au->dbg_frames > 0) AAudioStreamBuilder_setFramesPerDataCallback(b, au->dbg_frames);
    AAudioStreamBuilder_setDataCallback(b, data_cb, au);
    r = AAudioStreamBuilder_openStream(b, &au->st);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { au->st = NULL; LOG("[RED] audio: AAudio stream open failed: %s", AAudio_convertResultToText(r)); return r; }
    int32_t burst = AAudioStream_getFramesPerBurst(au->st), per_cb = AAudioStream_getFramesPerDataCallback(au->st);
    au->burst = burst > 0 ? (uint32_t)burst : 0;
    LOG("audio: AAudio stream %d Hz %d channels, burst %d frames, buffer %d frames%s", AAudioStream_getSampleRate(au->st), AAudioStream_getChannelCount(au->st),
        burst, AAudioStream_getBufferSizeInFrames(au->st), per_cb > 0 ? ", data callbacks of a fixed size (DM_DBG_AU_FRAMES)" : "");
    // Two bursts of buffering: low latency, and the host's queue (hle_audio.c) absorbs the game's own block sizes. A fixed data callback larger than a burst
    // needs room for a whole callback besides
    AAudioStream_setBufferSizeInFrames(au->st, per_cb > burst ? per_cb + burst : 2 * burst);
    return noErr;
}
OSStatus AudioComponentInstanceDispose(AudioComponentInstance au) {
    if (!au) return noErr;
    pthread_mutex_lock(&au->mu); pacer_stop(au); close_stream(au); pthread_mutex_unlock(&au->mu);
    pthread_mutex_destroy(&au->mu); sem_destroy(&au->pacer_sem); free(au->ring.pcm); free(au); return noErr;
}
OSStatus AudioUnitSetProperty(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, const void *data, UInt32 size) {
    (void)scope; (void)el;
    if (id == kAudioUnitProperty_StreamFormat && size >= sizeof au->fmt) {
        const AudioStreamBasicDescription *f = data;
        if (f->mFormatID != kAudioFormatLinearPCM || f->mBitsPerChannel != 16 || !(f->mFormatFlags & kLinearPCMFormatFlagIsSignedInteger)) return -50;   // paramErr: only PCM16
        au->fmt = *f; return noErr;
    }
    if (id == kAudioUnitProperty_SetRenderCallback && size >= sizeof au->cb) { au->cb = *(const AURenderCallbackStruct *)data; return noErr; }
    return -10879;   // kAudioUnitErr_InvalidProperty
}
OSStatus AudioUnitGetProperty(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, void *data, UInt32 *size) {
    (void)scope; (void)el;
    if (id == kAudioUnitProperty_StreamFormat && *size >= sizeof au->fmt) { *(AudioStreamBasicDescription *)data = au->fmt; *size = sizeof au->fmt; return noErr; }
    return -10879;
}
OSStatus AudioUnitAddPropertyListener(AudioUnit au, AudioUnitPropertyID id, AudioUnitPropertyListenerProc fn, void *ctx) { (void)au; (void)id; (void)fn; (void)ctx; return noErr; }
OSStatus AudioUnitRemovePropertyListenerWithUserData(AudioUnit au, AudioUnitPropertyID id, AudioUnitPropertyListenerProc fn, void *ctx) { (void)au; (void)id; (void)fn; (void)ctx; return noErr; }
OSStatus AudioUnitInitialize(AudioUnit au) {
    if (!au->fmt.mChannelsPerFrame || !au->fmt.mSampleRate) return -50;
    pthread_mutex_lock(&au->mu);
    if (!au->ring.pcm && !au->ring_off) {   // 5 ms quanta, the buffer the host asks Core Audio for (hle_audio.c au_buffer); 64 of them hold any burst seen so far
        uint32_t q = (uint32_t)(au->fmt.mSampleRate / 200 + 0.5), cap = q * RING_QUANTA;
        int16_t *pcm = q ? calloc((size_t)cap * au->fmt.mChannelsPerFrame, sizeof *pcm) : NULL;
        if (pcm) { au->quantum = q; aup_init(&au->ring, pcm, cap, au->fmt.mChannelsPerFrame, q); }
    }
    OSStatus st = au->st ? noErr : open_stream(au);
    pthread_mutex_unlock(&au->mu);
    return st;
}
OSStatus AudioUnitUninitialize(AudioUnit au) { pthread_mutex_lock(&au->mu); pacer_stop(au); close_stream(au); pthread_mutex_unlock(&au->mu); return noErr; }
OSStatus AudioOutputUnitStart(AudioUnit au) {
    pthread_mutex_lock(&au->mu);
    if (au->st && AAudioStream_getState(au->st) == AAUDIO_STREAM_STATE_DISCONNECTED) close_stream(au);
    OSStatus st = au->st ? noErr : open_stream(au);
    if (!st) { pacer_start(au); st = AAudioStream_requestStart(au->st); }
    if (st) pacer_stop(au);
    pthread_mutex_unlock(&au->mu);
    return st;
}
OSStatus AudioOutputUnitStop(AudioUnit au) {
    pthread_mutex_lock(&au->mu);
    OSStatus st = stop_stream(au);
    pacer_stop(au);   // after the stream: a data callback still running reads only the ring
    pthread_mutex_unlock(&au->mu);
    return st;
}

int dm_aaudio_xruns(AudioUnit au) { pthread_mutex_lock(&au->mu); int n = au->st ? AAudioStream_getXRunCount(au->st) : -1; pthread_mutex_unlock(&au->mu); return n; }   // tests/t_audio.c
int dm_aaudio_paced(AudioUnit au) { return atomic_load(&au->paced); }                  // tests/t_audio.c
uint64_t dm_aaudio_short(AudioUnit au) { return atomic_load(&au->short_n); }           // tests/t_audio.c
// AAC decoding for movies (AudioConverter): media_android.c
