/* decoder.h - Lectura/decodificación de audio a PCM s16le.
 * - .wav (PCM 16 bit): parser nativo.
 * - otros formatos (mp3, flac, ogg...): se delega a `ffmpeg` por pipe, si está instalado.
 */
#ifndef DECODER_H
#define DECODER_H
#include <stdio.h>
#include <sys/types.h>

typedef struct {
    FILE *fp;
    int is_pipe;
    int rate, channels;
    long remaining;      /* bytes de datos restantes (WAV); -1 = hasta EOF */
} decoder_t;

int     dec_open(decoder_t *d, const char *path);   /* 0 ok, -1 error */
ssize_t dec_read(decoder_t *d, void *buf, size_t n);
void    dec_close(decoder_t *d);

#endif
