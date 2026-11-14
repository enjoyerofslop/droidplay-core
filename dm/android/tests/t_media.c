// The movie decoders on MediaCodec (media_android.c): t_media <h264 file> <aac file> [<h264 file without B-frames> [<720p h264 file with B-frames>]], made by ffmpeg (the README's Checks):
//   an Annex B H.264 stream of 30 frames of solid 0x2060c0 at 320×240 (High profile with B-frames, access unit delimiters), decoded as hle_movie.c does it (one access
//   unit per packet, pictures taken as they come and the rest at the end), as NV12 (Y 91, U 180, V 93: ffmpeg's BT.601 video range) and as RGBA (about 32 96 192);
//   an ADTS AAC stream of one second of a 1 kHz sine at 48 kHz, stereo, decoded through AudioConverter as md_aac does (the configuration in an esds cookie);
//   the same picture in Main profile without B-frames (pic_order_cnt_type 2), whose every picture must leave before the end of stream, as a player that sends none
//   needs: as x264 writes it (its VUI says max_num_reorder_frames 0), and with its SPS's bitstream_restriction taken out, as Donkey Kong Country Returns HD's movie has it
//   (avc_sps.h then goes by pic_order_cnt_type 2);
//   1280×720 High profile with B-frames (I P B B B, as The World Ends with You: Final Remix's opening movie), each frame's number in two grey blocks, taken one picture per access unit
//   as hle_movie.c takes them: every picture in order, and the takes no further behind the access units sent than the decoder may keep pictures back (max_num_reorder_frames
//   and one more; without bitstream_restriction, its picture buffer). Before the takes waited for them they ran 6 behind, 9 without bitstream_restriction (droidplay-core#480).
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dm.h"
#include "hle_movie_avc.h"
#include "../avc_sps.h"
static int fails, checks;
#define EXPECT(what, ok) do { checks++; int ok_ = (ok); printf("%-70s %s\n", what, ok_ ? "✓" : "✗"); if (!ok_) fails++; } while (0)
static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) { printf("cannot open %s\n", p); exit(2); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET); uint8_t *b = malloc(*n); fread(b, 1, *n, f); fclose(f); return b;
}
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
static int g_early;   // the pictures the last video() took before the end of stream
static int video(const uint8_t *s, size_t n, int rgba, int *pics, uint8_t px[4]) {
    mavc *a = mavc_new(); if (rgba) mavc_set_rgba(a);
    int w = 320, h = 240; uint8_t *out = malloc((size_t)w * h * 4); int64_t pts; int frame = 0;
    size_t i = next_sc(s, n, 0), start = i;
    for (;;) {   // access units: from one delimiter (NAL type 9) to the next
        size_t j = next_sc(s, n, i + 3);
        int last = j >= n, aud = !last && (s[j + 3] & 31) == 9;
        if (last || aud) {
            size_t e = last ? n : (j > 0 && !s[j - 1] ? j - 1 : j);
            mavc_decode(a, s + start, (int)(e - start), (int64_t)frame * 33333); frame++;
            while (mavc_take(a, 0, out, w, h, &pts)) { (*pics)++; memcpy(px, rgba ? out + ((size_t)120 * w + 160) * 4 : out + (size_t)120 * w + 160, rgba ? 4 : 1); if (!rgba) { px[1] = out[(size_t)w * h + 60 * w + 160]; px[2] = out[(size_t)w * h + 60 * w + 161]; } }
            start = e;
        }
        if (last) break;
        i = j;
    }
    g_early = *pics;
    while (mavc_take(a, 1, out, w, h, &pts)) { (*pics)++; memcpy(px, rgba ? out + ((size_t)120 * w + 160) * 4 : out + (size_t)120 * w + 160, rgba ? 4 : 1); if (!rgba) { px[1] = out[(size_t)w * h + 60 * w + 160]; px[2] = out[(size_t)w * h + 60 * w + 161]; } }
    mavc_free(a); free(out);
    return frame;
}
// The frame number the B-frame clip's two blocks carry (grey 16 + 8 k: k for the frame number mod 28, then for the number / 28)
static int frame_code(const uint8_t *y, int w) { return (y[32 * w + 32] - 12) / 8 + 28 * ((y[32 * w + 96] - 12) / 8); }
// One take per access unit, as md_avc does, then the rest at the end of stream: → pictures; *ordered: each carried the next frame number; *behind: the most access units sent past the pictures taken
static int bframes(const uint8_t *s, size_t n, int *aus, int *ordered, int *behind) {
    mavc *a = mavc_new(); int w = 1280, h = 720, pics = 0; uint8_t *out = malloc((size_t)w * h * 3 / 2); int64_t pts;
    *aus = 0; *ordered = 1; *behind = 0;
    size_t i = next_sc(s, n, 0), start = i;
    for (;;) {
        size_t j = next_sc(s, n, i + 3);
        int last = j >= n, aud = !last && (s[j + 3] & 31) == 9;
        if (last || aud) {
            size_t e = last ? n : (j > 0 && !s[j - 1] ? j - 1 : j);
            mavc_decode(a, s + start, (int)(e - start), (int64_t)*aus * 33333); (*aus)++;
            if (mavc_take(a, 0, out, w, h, &pts)) { if (frame_code(out, w) != pics % 784) *ordered = 0; pics++; }
            if (*aus - pics > *behind) *behind = *aus - pics;
            start = e;
        }
        if (last) break;
        i = j;
    }
    while (mavc_take(a, 1, out, w, h, &pts)) { if (frame_code(out, w) != pics % 784) *ordered = 0; pics++; }
    mavc_free(a); free(out);
    return pics;
}
static int first_sps(const uint8_t *s, size_t n, avc_sps *sp) {   // the stream's first SPS
    avc_rbsp r;
    for (size_t b = next_sc(s, n, 0); b < n; b = next_sc(s, n, b + 3)) {
        size_t nb = b + 3, e = next_sc(s, n, nb); while (e > nb && !s[e - 1]) e--;
        if (nb < n && (s[nb] & 31) == 7) return avc_sps_parse(s + nb, e - nb, &r, sp);
    }
    return 0;
}
// s with every SPS's bitstream_restriction_flag cleared (and what followed it dropped) → o (n + n / 4 + 16 bytes are enough: each 3-byte start code comes out as 4 bytes); its length
static size_t strip_restriction(const uint8_t *s, size_t n, uint8_t *o) {
    size_t i = next_sc(s, n, 0), on = 0;
    while (i < n) {
        size_t b = i + 3, e = next_sc(s, n, b), end = e; while (end > b && !s[end - 1]) end--;
        avc_rbsp r; avc_sps sp; memcpy(o + on, "\0\0\0\1", 4); on += 4;
        if ((s[b] & 31) == 7 && avc_sps_parse(s + b, end - b, &r, &sp) && sp.br) {
            uint8_t rb[sizeof r.b] = {0}; size_t bits = sp.br_at + 2;   // the bits before the flag, the flag (0) and the stop bit
            for (size_t k = 0; k < sp.br_at; k++) if (r.b[k / 8] >> (7 - k % 8) & 1) rb[k / 8] |= (uint8_t)(0x80 >> (k % 8));
            rb[(sp.br_at + 1) / 8] |= (uint8_t)(0x80 >> ((sp.br_at + 1) % 8));
            o[on++] = s[b];
            for (size_t k = 0, z = 0; k < (bits + 7) / 8; k++) { if (z >= 2 && rb[k] <= 3) { o[on++] = 3; z = 0; } o[on++] = rb[k]; z = rb[k] ? 0 : z + 1; }   // emulation prevention
        } else { memcpy(o + on, s + b, end - b); on += end - b; }
        i = e;
    }
    return on;
}
static const uint8_t *g_pkt; static UInt32 g_pkt_n;
static OSStatus feed(AudioConverterRef c, UInt32 *np, AudioBufferList *bl, AudioStreamPacketDescription **pd, void *ctx) {
    (void)c; (void)ctx; (void)pd;
    if (!g_pkt) { *np = 0; return 1; }
    bl->mNumberBuffers = 1; bl->mBuffers[0].mData = (void *)g_pkt; bl->mBuffers[0].mDataByteSize = g_pkt_n; *np = 1; g_pkt = NULL; return 0;
}
int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: t_media <h264> <aac> [<h264 without B-frames> [<720p h264 with B-frames>]]\n"); return 2; }
    size_t n; uint8_t *s = slurp(argv[1], &n);
    int pics = 0; uint8_t px[4] = {0};
    int frames = video(s, n, 0, &pics, px);
    printf("H.264 NV12: %d access units, %d pictures, centre Y %d, U %d, V %d\n", frames, pics, px[0], px[1], px[2]);
    EXPECT("NV12: every access unit gives a picture", frames == 30 && pics == 30);
    EXPECT("NV12: the colour (Y 91 U 180 V 93, within 3)", abs(px[0] - 91) <= 3 && abs(px[1] - 180) <= 3 && abs(px[2] - 93) <= 3);
    pics = 0; frames = video(s, n, 1, &pics, px);
    printf("H.264 RGBA: %d pictures, centre %d %d %d %d\n", pics, px[0], px[1], px[2], px[3]);
    EXPECT("RGBA: every access unit gives a picture", pics == 30);
    EXPECT("RGBA: the colour (32 96 192, within 8, opaque)", abs(px[0] - 32) <= 8 && abs(px[1] - 96) <= 8 && abs(px[2] - 192) <= 8 && px[3] == 255);
    free(s);
    // AAC: ADTS frames → raw packets; the AudioSpecificConfig from the first header, in an esds cookie as hle_movie.c makes it
    s = slurp(argv[2], &n);
    int prof = (s[2] >> 6) + 1, fi = s[2] >> 2 & 15, ch = (s[2] & 1) << 2 | s[3] >> 6;
    uint8_t asc[2] = {(uint8_t)(prof << 3 | fi >> 1), (uint8_t)((fi & 1) << 7 | ch << 3)};
    uint8_t ck[64], *p = ck;
    *p++ = 3; *p++ = 3 + 2 + 13 + 2 + 2; *p++ = 0; *p++ = 0; *p++ = 0;
    *p++ = 4; *p++ = 13 + 2 + 2; *p++ = 0x40; *p++ = 0x15; memset(p, 0, 11); p += 11;
    *p++ = 5; *p++ = 2; memcpy(p, asc, 2); p += 2;
    AudioStreamBasicDescription in = {.mSampleRate = 48000, .mFormatID = kAudioFormatMPEG4AAC, .mFramesPerPacket = 1024, .mChannelsPerFrame = (UInt32)ch};
    AudioStreamBasicDescription out = {.mSampleRate = 48000, .mFormatID = kAudioFormatLinearPCM, .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                       .mBytesPerPacket = 2u * ch, .mFramesPerPacket = 1, .mBytesPerFrame = 2u * ch, .mChannelsPerFrame = (UInt32)ch, .mBitsPerChannel = 16};
    AudioConverterRef c = NULL;
    EXPECT("AAC converter made and configured", !AudioConverterNew(&in, &out, &c) && !AudioConverterSetProperty(c, kAudioConverterDecompressionMagicCookie, (UInt32)(p - ck), ck));
    long total = 0, cross = 0; double sq = 0; int16_t prev = 0, buf[8192 * 2]; int packets = 0;
    for (size_t i = 0; i + 7 <= n;) {
        size_t len = (size_t)(s[i + 3] & 3) << 11 | (size_t)s[i + 4] << 3 | s[i + 5] >> 5, hl = s[i + 1] & 1 ? 7 : 9;
        if (len < hl || i + len > n) break;
        g_pkt = s + i + hl; g_pkt_n = (UInt32)(len - hl); packets++;
        UInt32 fr = 8192; AudioBufferList ob = {1, {{(UInt32)ch, sizeof buf, buf}}};
        AudioConverterFillComplexBuffer(c, feed, NULL, &fr, &ob, NULL);
        for (UInt32 k = 0; k < fr; k++) { int16_t v = buf[k * ch]; sq += (double)v * v; if ((v >= 0) != (prev >= 0)) cross++; prev = v; }
        total += fr; i += len;
    }
    double rms = total ? sqrt(sq / total) : 0;
    printf("AAC: %d packets, %ld frames out, RMS %.0f, %ld zero crossings (%.0f Hz)\n", packets, total, rms, cross, total ? cross / 2.0 / (total / 48000.0) : 0);
    EXPECT("AAC: about the stream's length (all but the decoder's last packets)", total > 40000 && total <= 49152);
    EXPECT("AAC: a 1 kHz tone (within 5 %, not silent)", rms > 1000 && total && fabs(cross / 2.0 / (total / 48000.0) - 1000) < 50);
    AudioConverterDispose(c); free(s);
    if (argc > 3) {   // pictures in decode order: each leaves once decoded, so a player that never ends the stream still gets the last
        s = slurp(argv[3], &n);
        pics = 0; frames = video(s, n, 0, &pics, px);
        printf("H.264 without B-frames: %d access units, %d pictures, %d of them before the end of stream\n", frames, pics, g_early);
        EXPECT("decode order, as the SPS says: every picture before the end of stream", frames == 30 && pics == 30 && g_early == 30);
        uint8_t *t = malloc(n + n / 4 + 64); size_t tn = strip_restriction(s, n, t);   // each 3-byte start code comes out as 4 bytes
        avc_rbsp r; avc_sps sp; size_t sb = next_sc(t, tn, 0) + 3; while (sb < tn && (t[sb] & 31) != 7) sb = next_sc(t, tn, sb) + 3;   // the first SPS, after the delimiter
        size_t se = next_sc(t, tn, sb); while (se > sb && !t[se - 1]) se--;
        EXPECT("the SPS without bitstream_restriction (pic_order_cnt_type 2)", sb < tn && avc_sps_parse(t + sb, se - sb, &r, &sp) && !sp.br && sp.poc_type == 2);
        pics = 0; frames = video(t, tn, 0, &pics, px);
        printf("H.264 without B-frames, no bitstream_restriction: %d pictures, %d of them before the end of stream\n", pics, g_early);
        EXPECT("decode order, the SPS silent: every picture before the end of stream", frames == 30 && pics == 30 && g_early == 30);
        free(t); free(s);
    }
    if (argc > 4) {   // B-frames at 720p: the takes keep up with the access units sent, as far as the decoder lets them
        s = slurp(argv[4], &n);
        avc_sps sp; int aus, ordered, behind, ok = first_sps(s, n, &sp);
        pics = bframes(s, n, &aus, &ordered, &behind);
        printf("H.264 720p with B-frames (max_num_reorder_frames %d): %d access units, %d pictures, %s, the takes at most %d behind\n", sp.reorder, aus, pics, ordered ? "in order" : "out of order", behind);
        EXPECT("B-frames: every picture, in order, its frame number in its blocks", ok && sp.br && pics == aus && ordered);
        EXPECT("B-frames: the takes at most max_num_reorder_frames + 1 behind", ok && behind <= sp.reorder + 1);
        uint8_t *t = malloc(n + n / 4 + 64); size_t tn = strip_restriction(s, n, t);   // each 3-byte start code comes out as 4 bytes
        ok = first_sps(t, tn, &sp) && !sp.br;
        pics = bframes(t, tn, &aus, &ordered, &behind);
        printf("H.264 720p with B-frames, no bitstream_restriction (picture buffer %d): %d pictures, %s, the takes at most %d behind\n", avc_sps_dpb_frames(&sp), pics, ordered ? "in order" : "out of order", behind);
        EXPECT("B-frames, the SPS silent: every picture, in order", ok && pics == aus && ordered);
        EXPECT("B-frames, the SPS silent: the takes at most the picture buffer behind", ok && behind <= avc_sps_dpb_frames(&sp));
        free(t); free(s);
    }
    printf("== %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
