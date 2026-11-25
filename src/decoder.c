#include "decoder.h"
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int wav_parse(decoder_t *d)
{
    FILE *fp = d->fp;
    uint8_t h[12];
    if (fread(h, 1, 12, fp) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;
    int have_fmt = 0, fmt = 0, bits = 0;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, fp) != 8) return -1;
        uint32_t sz = rd32(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t f[16];
            if (sz < 16 || fread(f, 1, 16, fp) != 16) return -1;
            fmt = rd16(f); d->channels = rd16(f + 2); d->rate = (int)rd32(f + 4); bits = rd16(f + 14);
            if (fseek(fp, (long)(sz - 16 + (sz & 1)), SEEK_CUR) != 0) return -1;
            have_fmt = 1;
        } else if (!memcmp(ch, "data", 4)) {
            if (!have_fmt) return -1;
            d->remaining = (sz == 0 || sz == 0xFFFFFFFFu) ? -1 : (long)sz;
            break;
        } else if (fseek(fp, (long)(sz + (sz & 1)), SEEK_CUR) != 0) return -1;
    }
    if ((fmt != 1 && fmt != 0xFFFE) || bits != 16) return -1;
    if (d->channels < 1 || d->channels > 8 || d->rate < 4000 || d->rate > 192000) return -1;
    return 0;
}

int dec_open(decoder_t *d, const char *path)
{
    memset(d, 0, sizeof *d);
    if (access(path, R_OK) != 0) return -1;
    const char *dot = strrchr(path, '.');
    if (dot && !strcasecmp(dot, ".wav")) {
        d->fp = fopen(path, "rb");
        if (!d->fp) return -1;
        if (wav_parse(d) != 0) { fclose(d->fp); d->fp = NULL; return -1; }
        return 0;
    }
    /* Otros formatos: ffmpeg -> s16le 44.1 kHz estéreo */
    char esc[2 * 512 + 8], cmd[2 * 512 + 128];
    size_t j = 0;
    for (const char *s = path; *s && j < sizeof esc - 6; s++) {
        if (*s == '\'') { memcpy(esc + j, "'\\''", 4); j += 4; }
        else esc[j++] = *s;
    }
    esc[j] = '\0';
    snprintf(cmd, sizeof cmd, "ffmpeg -v quiet -nostdin -i '%s' -f s16le -ar 44100 -ac 2 - 2>/dev/null", esc);
    d->fp = popen(cmd, "r");
    if (!d->fp) return -1;
    d->is_pipe = 1; d->rate = 44100; d->channels = 2; d->remaining = -1;
    return 0;
}

ssize_t dec_read(decoder_t *d, void *buf, size_t n)
{
    if (d->remaining >= 0 && (long)n > d->remaining) n = (size_t)d->remaining;
    if (n == 0) return 0;
    size_t got = fread(buf, 1, n, d->fp);
    if (d->remaining >= 0) d->remaining -= (long)got;
    return (ssize_t)got;
}

void dec_close(decoder_t *d)
{
    if (!d->fp) return;
    if (d->is_pipe) pclose(d->fp); else fclose(d->fp);
    d->fp = NULL;
}
