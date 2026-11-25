/* sink.h - Salida de audio: envía PCM s16le a un programa reproductor externo
 * (aplay/ALSA o paplay/PulseAudio) mediante pipe, o a un sumidero "null" que
 * simula el tiempo real (clock_nanosleep) para pruebas sin tarjeta de sonido.
 */
#ifndef SINK_H
#define SINK_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

typedef enum { SINK_APLAY, SINK_PAPLAY, SINK_NULL } sink_kind_t;

typedef struct {
    sink_kind_t kind;
    int fd; pid_t pid;
    int rate, ch;              /* rate == 0 -> cerrado */
    struct timespec next;      /* para el pacing del sumidero null */
    int warned;
} sink_t;

void sink_init(sink_t *s, sink_kind_t kind);
int  sink_open(sink_t *s, int rate, int channels);
int  sink_write(sink_t *s, const uint8_t *buf, size_t len);
void sink_close(sink_t *s, int drain);   /* drain=1 deja terminar lo ya enviado; 0 corta ya */

#endif
