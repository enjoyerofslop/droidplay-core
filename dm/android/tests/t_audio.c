// The audio output on AAudio (coreaudio_aaudio.c), as hle_audio.c drives it: an output AudioUnit with a PCM16 stream format and a render callback,
// started for two seconds of a quiet 440 Hz tone. Checks that the stream runs at about real time (the frames rendered), that the callback's
// buffers reach the device (no underruns past the start), and that Stop and a second Start work (hle_audio.c's watchdog restarts the unit so).
// Then the same with 20 ms data callbacks (DM_DBG_AU_FRAMES=960, an AYN Thor Pro's burst): the unit switches to its pacer, which calls the render callback
// with 5 ms quanta at real time and keeps the data callbacks fed (au_pace.h); with DM_AAUDIO_RING=0 the callbacks stay 960 frames, rendered straight.
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "dm.h"
int dm_aaudio_xruns(AudioUnit au);   // coreaudio_aaudio.c
int dm_aaudio_paced(AudioUnit au);
uint64_t dm_aaudio_short(AudioUnit au);
static int fails, checks;
#define EXPECT(what, ok) do { checks++; int ok_ = (ok); printf("%-60s %s\n", what, ok_ ? "✓" : "✗"); if (!ok_) fails++; } while (0)
static _Atomic long g_frames, g_calls, g_nmin = 1 << 30, g_nmax; static double g_phase;
static OSStatus render(void *ud, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *bl) {
    (void)ud; (void)fl; (void)ts; (void)bus;
    int16_t *s = bl->mBuffers[0].mData;
    for (UInt32 i = 0; i < n; i++) { int16_t v = (int16_t)(2000 * sin(g_phase)); g_phase += 2 * M_PI * 440 / 48000; s[2 * i] = s[2 * i + 1] = v; }
    g_frames += n; g_calls++;
    if (n < g_nmin) g_nmin = n;
    if (n > g_nmax) g_nmax = n;
    return noErr;
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static AudioUnit unit(void) {
    AudioComponentDescription d = {.componentType = kAudioUnitType_Output}; AudioUnit au = NULL;
    AudioStreamBasicDescription f = {.mSampleRate = 48000, .mFormatID = kAudioFormatLinearPCM, .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                     .mBytesPerPacket = 4, .mFramesPerPacket = 1, .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16};
    AURenderCallbackStruct cb = {render, NULL};
    if (AudioComponentInstanceNew(AudioComponentFindNext(NULL, &d), &au) || AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ||
        AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) || AudioUnitInitialize(au)) return NULL;
    return au;
}
// 20 ms data callbacks: paced (5 ms quanta to the render callback, the data callbacks fed from the ring) unless DM_AAUDIO_RING=0
static void forced_960(int ring) {
    setenv("DM_DBG_AU_FRAMES", "960", 1); if (ring) unsetenv("DM_AAUDIO_RING"); else setenv("DM_AAUDIO_RING", "0", 1);
    AudioUnit au = unit(); EXPECT(ring ? "960-frame data callbacks: unit made" : "960-frame data callbacks, ring off: unit made", au != NULL);
    if (!au) return;
    EXPECT("started", !AudioOutputUnitStart(au));
    usleep(500000); double t1 = now_s(); long fr1 = g_frames; g_nmin = 1 << 30; g_nmax = 0;
    sleep(2); double dt = now_s() - t1; long fr = g_frames - fr1;
    printf("%s: %.0f frames/s, render callbacks of %ld-%ld frames, %llu short data callbacks, %d underruns\n", ring ? "paced" : "ring off", fr / dt, (long)g_nmin, (long)g_nmax,
           (unsigned long long)dm_aaudio_short(au), dm_aaudio_xruns(au));
    EXPECT("about real time (48000 frames/s within 10 %)", fabs(fr / dt - 48000) < 4800);
    if (ring) {
        EXPECT("switched to the pacer", dm_aaudio_paced(au));
        EXPECT("the render callback gets 240-frame quanta", g_nmin == 240 && g_nmax == 240);
        EXPECT("no data callback short of frames", dm_aaudio_short(au) == 0);
    } else EXPECT("rendered straight, 960 frames a callback", !dm_aaudio_paced(au) && g_nmin == 960 && g_nmax == 960);
    EXPECT("stopped", !AudioOutputUnitStop(au));
    long f1 = g_frames; usleep(300000);
    EXPECT("no render callbacks once Stop returned", g_frames == f1);
    EXPECT("started again", !AudioOutputUnitStart(au));
    usleep(500000); EXPECT("callbacks again", g_frames > f1 + 10000);
    if (ring) EXPECT("still paced, still fed", dm_aaudio_paced(au) && dm_aaudio_short(au) == 0);
    AudioOutputUnitStop(au); AudioUnitUninitialize(au); AudioComponentInstanceDispose(au);
    unsetenv("DM_DBG_AU_FRAMES"); unsetenv("DM_AAUDIO_RING");
}
int main(void) {
    AudioComponentDescription d = {.componentType = kAudioUnitType_Output};
    AudioComponent c = AudioComponentFindNext(NULL, &d); AudioUnit au = NULL;
    EXPECT("output component found and instanced", c && !AudioComponentInstanceNew(c, &au));
    AudioStreamBasicDescription f = {.mSampleRate = 48000, .mFormatID = kAudioFormatLinearPCM, .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                     .mBytesPerPacket = 4, .mFramesPerPacket = 1, .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16};
    AURenderCallbackStruct cb = {render, NULL};
    EXPECT("stream format and render callback set, unit initialized", !AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) &&
           !AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) && !AudioUnitInitialize(au));
    double t0 = now_s(); EXPECT("started", !AudioOutputUnitStart(au));
    usleep(500000); double t1 = now_s(); long fr1 = g_frames, c1 = g_calls;   // after the start-up: the steady rate
    sleep(2); double dt = now_s() - t1; long fr = g_frames - fr1;
    printf("first %.2f s: %ld frames; then %ld frames in %ld callbacks over %.2f s (%.0f frames/s), %d underruns\n", t1 - t0, fr1, fr, (long)(g_calls - c1), dt, fr / dt, dm_aaudio_xruns(au));
    EXPECT("about real time (48000 frames/s within 10 %)", fabs(fr / dt - 48000) < 4800);
    EXPECT("no more than 2 underruns (the start may have one)", dm_aaudio_xruns(au) <= 2);
    EXPECT("stopped", !AudioOutputUnitStop(au));
    usleep(200000); long f1 = g_frames; usleep(300000);
    EXPECT("no callbacks while stopped", g_frames == f1);
    EXPECT("started again", !AudioOutputUnitStart(au));
    usleep(500000); EXPECT("callbacks again", g_frames > f1 + 10000);
    AudioOutputUnitStop(au); AudioUnitUninitialize(au); AudioComponentInstanceDispose(au);
    forced_960(1);
    forced_960(0);
    printf("== %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
