/*
 * Bite 63 — Audio, hands dirty: read, measure, and mangle real PCM
 *
 * "Media" is just bytes with a clock attached. This program works with raw
 * PCM: 16-bit signed little-endian ("S16_LE"), 48 kHz, mono. That wire
 * format is the currency of real-time audio:
 *
 *   byte rate = 48000 samples/s * 2 bytes/sample = 96000 bytes/s (~96 KB/s)
 *   one 20 ms "frame" (an RTP payload's worth, bite 59's tick)
 *             = 0.020 s * 48000 * 2 = 1920 bytes = 960 samples
 *
 * The file has NO header — that's what WAV/AIFF/ogg add. Raw PCM forces you
 * to agree on format out-of-band, exactly like SDP negotiates it (bite 73).
 *
 * Modes:
 *   rms     in.raw              per-20ms-chunk RMS + ASCII waveform bars
 *   gen     out.raw [seconds]   synthesize a 3 s test tone (no mic needed!)
 *   gain    in.raw out.raw F    multiply samples by F, saturating at +-32768
 *   gainu   in.raw out.raw F    ... WITHOUT saturation (wraps: crackle!)
 *   asun    in.raw out.raw F    reinterpret signed as UNSIGNED (break-it)
 *
 * RECORD/PLAY commands (run these yourself in WSL; needs a sound device or
 * pulseaudio/pipewire bridge — the C program itself needs neither):
 *
 *   arecord -f S16_LE -r 48000 -c 1 -d 3 test.raw     # 3 s raw capture
 *   ffmpeg -f alsa -i default -ar 48000 -ac 1 \
 *          -f s16le -acodec pcm_s16le test.raw        # same via ffmpeg
 *   aplay -f S16_LE -r 48000 -c 1 test.raw            # play it back
 *   aplay -f S16_LE -r 48000 -c 1 out_hungry.raw      # play a processed one
 *   (ffmpeg can also convert: ffmpeg -f s16le -ar 48000 -ac 1 -i test.raw out.wav)
 *
 * Compile + run (from the repo root, in WSL):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/63-audio/rms.c -o /tmp/b63
 *   /tmp/b63 gen  /tmp/tone.raw 3
 *   /tmp/b63 rms  /tmp/tone.raw
 *   /tmp/b63 gain /tmp/tone.raw /tmp/loud.raw 2.0
 *   /tmp/b63 gainu /tmp/tone.raw /tmp/wrap.raw 2.0
 *   /tmp/b63 asun /tmp/tone.raw /tmp/dc.raw 1.0
 *
 * BREAK IT ON PURPOSE (from phases/phase-6-rt-media.md):
 *   Interpret the PCM as unsigned instead of signed — the `asun` mode does
 *   exactly that (reads each pair of bytes as uint16_t instead of int16_t).
 *   Play it: `aplay -f S16_LE -r 48000 -c 1 /tmp/dc.raw` — a loud DC-shifted
 *   roar. WHY: silence (sample 0) becomes 32768, halfway up the scale; the
 *   waveform is displaced by half its range and every negative excursion
 *   wraps. One wrong type interpretation = broken media. This is why the
 *   header/type discipline of bites 41–42 mattered: the bytes don't carry
 *   their own meaning, the CONTRACT does.
 *   Also try `gainu` on a hot signal: samples past +32768 wrap to negative
 *   — audible crackle. `gain` (saturating) instead flattens the tops:
 *   distortion, but not garbage. Saturation: heard.
 *
 * Done when: you've generated/recorded, measured (RMS bars), scaled, and
 * can state the byte rate of 48 kHz/16-bit/mono from memory: 96 000 B/s.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>  /* -lm */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 48000
#define CHUNK_MS 20
#define CHUNK_SAMPLES (SAMPLE_RATE * CHUNK_MS / 1000) /* 960 samples */
#define CHUNK_BYTES (CHUNK_SAMPLES * 2)               /* 1920 bytes    */
#define BAR_WIDTH 60
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void die(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

/* Read the whole file as int16 samples. Caller frees. */
static int16_t *load_pcm(const char *path, size_t *n_samples)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (bytes < 0 || bytes % 2) {
        fprintf(stderr, "%s: not a whole number of 16-bit samples (%ld bytes)\n",
                path, bytes);
        exit(1);
    }
    int16_t *samples = malloc((size_t)bytes);
    if (!samples)
        die("out of memory");
    if (fread(samples, 1, (size_t)bytes, f) != (size_t)bytes)
        die("short read");
    fclose(f);
    *n_samples = (size_t)bytes / 2;
    return samples;
}

static void store_pcm(const char *path, const int16_t *s, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    if (fwrite(s, 2, n, f) != n)
        die("short write");
    fclose(f);
}

/* ---- gen: synthesize a test signal (440 Hz + envelope + a loud burst) -- */
static void mode_gen(const char *path, double seconds)
{
    size_t n = (size_t)(seconds * SAMPLE_RATE);
    int16_t *s = malloc(n * 2);
    if (!s)
        die("oom");
    for (size_t i = 0; i < n; i++) {
        double t = (double)i / SAMPLE_RATE;
        /* 440 Hz sine with fade-in/out so playback clicks less, PLUS a
         * deliberately near-full-scale burst between 1.0 s and 1.5 s —
         * that burst is what gain 2.0 will clip or wrap later. */
        double env = t < 0.1 ? t / 0.1
                   : t > seconds - 0.1 ? (seconds - t) / 0.1
                                       : 1.0;
        double a = (t > 1.0 && t < 1.5) ? 0.92 : 0.35;
        double v = sin(2.0 * M_PI * 440.0 * t) * a * env;
        s[i] = (int16_t)lround(v * 32767.0);
    }
    store_pcm(path, s, n);
    free(s);
    printf("generated %.2f s = %zu samples = %zu bytes (%.0f KB/s wire rate)\n",
           seconds, n, n * 2, SAMPLE_RATE * 2 / 1000.0);
}

/* ---- rms: level per 20 ms chunk, as an ASCII bar ------------------------ */
static void mode_rms(const char *path)
{
    size_t n;
    int16_t *s = load_pcm(path, &n);

    printf("file: %s, %zu samples = %.3f s at 48 kHz\n", path, n,
           (double)n / SAMPLE_RATE);
    printf("chunk = %d ms = %d samples = %d bytes (one RTP payload)\n\n",
           CHUNK_MS, CHUNK_SAMPLES, CHUNK_BYTES);

    for (size_t off = 0; off + CHUNK_SAMPLES <= n; off += CHUNK_SAMPLES) {
        /* RMS = sqrt(mean(x^2)) — the energy of the chunk. Peak would be
         * max|x|; RMS matches perceived loudness better and is what
         * "audio level" metrics (and WebRTC's getStats) report. */
        double sum_sq = 0.0;
        int16_t peak = 0;
        for (size_t i = 0; i < CHUNK_SAMPLES; i++) {
            double v = s[off + i];
            sum_sq += v * v;
            if (abs(s[off + i]) > peak)
                peak = abs(s[off + i]);
        }
        double rms = sqrt(sum_sq / CHUNK_SAMPLES);

        int bar = (int)(rms / 32768.0 * BAR_WIDTH);
        if (bar > BAR_WIDTH)
            bar = BAR_WIDTH;
        printf("t=%6.3fs rms=%7.1f peak=%6d |", (double)off / SAMPLE_RATE,
               rms, peak);
        for (int i = 0; i < bar; i++)
            putchar('#');
        for (int i = bar; i < BAR_WIDTH; i++)
            putchar(' ');
        printf("|\n");
    }
    free(s);
}

/* ---- gain: scale samples; saturating vs wrapping ------------------------ */
static void mode_gain(const char *in, const char *out, double factor,
                      int saturate)
{
    size_t n;
    int16_t *s = load_pcm(in, &n);
    int clipped = 0, wrapped = 0;

    for (size_t i = 0; i < n; i++) {
        double v = (double)s[i] * factor;
        if (saturate) {
            if (v > 32767.0)  { v = 32767.0;  clipped++; }
            if (v < -32768.0) { v = -32768.0; clipped++; }
            s[i] = (int16_t)lround(v);
        } else {
            /* The classic overflow bug: plain int truncation of an
             * out-of-range value. (int16_t) of 40000 is implementation-
             * defined; on x86 it wraps to 40000-65536 = -25536. LOUD
             * becomes LOUD-AND-BACKWARDS: the crackle you can hear. */
            int iv = (int)v;
            int16_t w = (int16_t)iv;
            if (iv > 32767 || iv < -32768)
                wrapped++;
            s[i] = w;
        }
    }
    store_pcm(out, s, n);
    free(s);
    printf("%s -> %s: factor %.2f, %s %d sample(s)\n", in, out, factor,
           saturate ? "clipped (saturated)" : "wrapped (unsaturated)",
           saturate ? clipped : wrapped);
}

/* ---- asun: the deliberate type-confusion bug ---------------------------- */
static void mode_asun(const char *in, const char *out, double factor)
{
    size_t n;
    int16_t *s = load_pcm(in, &n);

    /* Reinterpret every sample as UNSIGNED (uint16), scale, store back.
     * Silence (0) becomes 32768 * factor: a huge DC offset. Play it and
     * hear the difference a single wrong cast makes. */
    for (size_t i = 0; i < n; i++) {
        unsigned u = (unsigned)(uint16_t)s[i]; /* <-- the wrong cast */
        double v = (double)u * factor;
        if (v > 65535.0)
            v = 65535.0;
        s[i] = (int16_t)((uint16_t)(int)v);
    }
    store_pcm(out, s, n);
    free(s);
    printf("%s -> %s: samples reinterpreted as UNSIGNED (DC-shifted).\n"
           "Play it to hear type confusion: aplay -f S16_LE -r 48000 -c 1 %s\n",
           in, out, out);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s gen <out.raw> [seconds]\n"
                "       %s rms <in.raw>\n"
                "       %s gain <in> <out> <factor>      (saturating)\n"
                "       %s gainu <in> <out> <factor>     (wrapping)\n"
                "       %s asun <in> <out> [factor]      (unsigned bug demo)\n",
                argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }
    const char *mode = argv[1];

    if (!strcmp(mode, "gen")) {
        mode_gen(argv[2], argc > 3 ? atof(argv[3]) : 3.0);
        return 0;
    }
    if (!strcmp(mode, "rms")) {
        mode_rms(argv[2]);
        return 0;
    }
    if (!strcmp(mode, "gain") || !strcmp(mode, "gainu")) {
        if (argc < 5) {
            fprintf(stderr, "gain needs <in> <out> <factor>\n");
            return 1;
        }
        mode_gain(argv[2], argv[3], atof(argv[4]), strcmp(mode, "gain") == 0);
        return 0;
    }
    if (!strcmp(mode, "asun")) {
        mode_asun(argv[2], argv[3], argc > 4 ? atof(argv[4]) : 1.0);
        return 0;
    }
    fprintf(stderr, "unknown mode: %s\n", mode);
    return 1;
}
