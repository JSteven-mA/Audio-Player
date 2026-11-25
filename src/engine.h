/* engine.h - Motor de reproducción: hilo decodificador (productor), hilo de
 * salida (consumidor), estado de control y API para la CLI.
 * El protocolo de concurrencia completo está documentado en README.md. */
#ifndef ENGINE_H
#define ENGINE_H
#include <pthread.h>
#include <stdio.h>
#include "playlist.h"
#include "ringbuf.h"
#include "sink.h"

typedef enum { ST_STOPPED, ST_PLAYING, ST_PAUSED } pstate_t;

typedef struct {
    playlist_t pl;
    ringbuf_t  rb;

    /* --- Estado de control: protegido por ctl --- */
    pthread_mutex_t ctl;
    pthread_cond_t  cv;        /* se hace broadcast ante cualquier cambio de estado */
    pstate_t   state;
    uint32_t   epoch;          /* se incrementa en cada discontinuidad (skip/stop/jump...) */
    uint32_t   now_id;         /* pista que suena (0 = ninguna) */
    int        quit;
    unsigned long underruns;

    sink_kind_t sink_kind;
    pthread_t dec_th, out_th;
} engine_t;

int  engine_init(engine_t *e, sink_kind_t kind);
int  engine_start(engine_t *e);
void engine_shutdown(engine_t *e);          /* detiene, hace join y libera todo */

int  engine_add(engine_t *e, const char *path);
int  engine_remove(engine_t *e, size_t idx);
int  engine_move(engine_t *e, size_t from, size_t to);
void engine_clear(engine_t *e);

int  engine_play(engine_t *e);              /* -1 si la lista está vacía */
pstate_t engine_pause_toggle(engine_t *e);
void engine_stop(engine_t *e);
int  engine_next(engine_t *e);
int  engine_prev(engine_t *e);
int  engine_jump(engine_t *e, size_t idx);

void engine_print_list(engine_t *e, FILE *out);
void engine_print_status(engine_t *e, FILE *out);
void engine_wait_idle(engine_t *e);         /* bloquea hasta que termine todo (sin espera activa) */

#endif
