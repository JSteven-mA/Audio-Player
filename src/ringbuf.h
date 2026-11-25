/* ringbuf.h - Búfer circular productor/consumidor (problema del búfer acotado).
 *
 * Unidad de transferencia: "slot" de SLOT_BYTES de PCM s16le + metadatos.
 * Sincronización: 1 mutex + 2 variables de condición (not_full / not_empty).
 * Nunca hay espera activa: el productor duerme si está lleno, el consumidor
 * duerme si está vacío.
 *
 * "epoch": contador de discontinuidades (skip/stop/jump...). rb_flush() vacía
 * el búfer y cambia la época; todo productor con época vieja es rechazado y
 * todo consumidor dormido es despertado con código 1 ("flushed").
 */
#ifndef RINGBUF_H
#define RINGBUF_H
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define SLOT_BYTES 4096

typedef struct {
    uint8_t  data[SLOT_BYTES];
    uint32_t len;        /* bytes válidos (0 = marcador de fin de pista) */
    uint32_t epoch;
    uint32_t track_id;
    int      rate, channels;
    int      eot;        /* último slot de una pista */
    int      last;       /* fin de la lista de reproducción */
} slot_t;

typedef struct {
    slot_t *slots;
    size_t cap, head, tail, count;
    size_t eot_count;    /* marcadores de fin de pista presentes en el búfer */
    uint32_t epoch;
    int closed;
    pthread_mutex_t m;
    pthread_cond_t not_full, not_empty;
} ringbuf_t;

int    rb_init(ringbuf_t *r, size_t cap_slots);
void   rb_destroy(ringbuf_t *r);
int    rb_write(ringbuf_t *r, const slot_t *s);                    /* 0 ok, -1 rechazado/cerrado */
int    rb_read(ringbuf_t *r, slot_t *out, uint32_t *seen_epoch, int *underrun); /* 0 slot, 1 flushed, -1 cerrado */
int    rb_wait_level(ringbuf_t *r, size_t level, uint32_t *seen_epoch);        /* 0 listo, 1 flushed, -1 cerrado */
void   rb_flush(ringbuf_t *r, uint32_t new_epoch);
void   rb_close(ringbuf_t *r);
size_t rb_count(ringbuf_t *r);

#endif
