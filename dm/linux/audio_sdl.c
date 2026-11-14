// AudioToolbox's output AudioUnit on SDL3's audio (dm/android/compat/AudioToolbox/AudioToolbox.h), for the Linux host: the unit's render callback feeds an
// SDL audio stream from its get callback, as dm/android/coreaudio_aaudio.c feeds AAudio's data callback. SDL plays it on the system's sound server
// (PipeWire, PulseAudio or ALSA) and converts the rate when the device's differs. The stream is opened at Initialize with the format set on the input scope
// (PCM16, the host's rate and channel count); the device buffer is asked for in 5 ms (SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES), the quantum hle_audio.c renders in.
// The callback follows SDL's example examples/audio/02-simple-playback-callback (SDL3, zlib licence): each call asks for the bytes SDL needs next.
#include <AudioToolbox/AudioToolbox.h>
#include <SDL3/SDL.h>
#include "dm.h"

struct dm_au_component { int unused; };
struct dm_au {
    AudioStreamBasicDescription fmt;
    AURenderCallbackStruct cb;
    SDL_AudioStream *st;
    pthread_mutex_t mu;
    int16_t *pcm; uint32_t cap;   // the render buffer, in frames
};
static struct dm_au_component g_output;

AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d) {
    return !after && d->componentType == kAudioUnitType_Output ? &g_output : NULL;
}
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out) {
    if (c != &g_output) return -1;
    struct dm_au *au = calloc(1, sizeof *au); if (!au) return -1;
    pthread_mutex_init(&au->mu, NULL);
    *out = au; return noErr;
}
static void SDLCALL get_cb(void *ud, SDL_AudioStream *st, int additional, int total) {
    (void)total; struct dm_au *au = ud;
    uint32_t bpf = au->fmt.mBytesPerFrame, frames = additional > 0 ? (uint32_t)additional / bpf : 0;
    while (frames) {
        uint32_t n = frames < au->cap ? frames : au->cap, bytes = n * bpf;
        AudioBufferList bl = {1, {{au->fmt.mChannelsPerFrame, bytes, au->pcm}}};
        AudioUnitRenderActionFlags fl = 0; AudioTimeStamp ts = {0};
        if (!au->cb.inputProc || au->cb.inputProc(au->cb.inputProcRefCon, &fl, &ts, 0, n, &bl)) memset(au->pcm, 0, bytes);
        SDL_PutAudioStreamData(st, au->pcm, (int)bytes);
        frames -= n;
    }
}
static void close_stream(struct dm_au *au) { if (au->st) { SDL_DestroyAudioStream(au->st); au->st = NULL; } }
static OSStatus open_stream(struct dm_au *au) {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) { LOG("[RED] audio: SDL audio: %s", SDL_GetError()); return -1; }
    char q[16]; snprintf(q, sizeof q, "%u", (unsigned)(au->fmt.mSampleRate / 200 + 0.5));   // 5 ms
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, q);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "Game");
    SDL_AudioSpec spec = {SDL_AUDIO_S16LE, (int)au->fmt.mChannelsPerFrame, (int)au->fmt.mSampleRate};
    au->st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, get_cb, au);
    if (!au->st) { LOG("[RED] audio: SDL audio stream open failed: %s", SDL_GetError()); return -1; }
    SDL_AudioSpec dev; int frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(au->st), &dev, &frames))
        LOG("audio: SDL %s stream %d Hz %d channels, device %d Hz %d channels, %d-frame buffer", SDL_GetCurrentAudioDriver(), (int)au->fmt.mSampleRate,
            (int)au->fmt.mChannelsPerFrame, dev.freq, dev.channels, frames);
    return noErr;
}
OSStatus AudioComponentInstanceDispose(AudioComponentInstance au) {
    if (!au) return noErr;
    pthread_mutex_lock(&au->mu); close_stream(au); pthread_mutex_unlock(&au->mu);
    pthread_mutex_destroy(&au->mu); free(au->pcm); free(au); return noErr;
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
    if (!au->fmt.mChannelsPerFrame || !au->fmt.mSampleRate || !au->fmt.mBytesPerFrame) return -50;
    pthread_mutex_lock(&au->mu);
    if (!au->pcm) { au->cap = 4096; au->pcm = calloc((size_t)au->cap, au->fmt.mBytesPerFrame); }
    OSStatus st = !au->pcm ? -1 : au->st ? noErr : open_stream(au);
    pthread_mutex_unlock(&au->mu);
    return st;
}
OSStatus AudioUnitUninitialize(AudioUnit au) { pthread_mutex_lock(&au->mu); close_stream(au); pthread_mutex_unlock(&au->mu); return noErr; }
OSStatus AudioOutputUnitStart(AudioUnit au) {
    pthread_mutex_lock(&au->mu);
    OSStatus st = au->st ? noErr : open_stream(au);
    if (!st && !SDL_ResumeAudioStreamDevice(au->st)) { LOG("[RED] audio: SDL could not start the stream: %s", SDL_GetError()); st = -1; }
    pthread_mutex_unlock(&au->mu);
    return st;
}
// hle_audio.c relies on Core Audio's AudioOutputUnitStop returning once the render callback has finished: the stream's lock is held while SDL runs it
OSStatus AudioOutputUnitStop(AudioUnit au) {
    pthread_mutex_lock(&au->mu);
    if (au->st) { SDL_PauseAudioStreamDevice(au->st); SDL_LockAudioStream(au->st); SDL_ClearAudioStream(au->st); SDL_UnlockAudioStream(au->st); }
    pthread_mutex_unlock(&au->mu);
    return noErr;
}
