#include "engine.h"
#include "decoder.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RB_SLOTS      64     /* 64 * 4096 B ~ 1.5 s a 44.1 kHz estéreo */
#define PREFILL_SLOTS 16     /* colchón antes de empezar a sonar (evita underrun) */

/* ------------------------------------------------------------------ *
 *  Helpers (requieren e->ctl tomado)                                  *
 * ------------------------------------------------------------------ */

/* Corta la reproducción actual: invalida todo lo decodificado y en el búfer. */
static void discontinuity_locked(engine_t *e)
{
    e->epoch++;
    e->now_id = 0;
    rb_flush(&e->rb, e->epoch);
}

/* El cursor del decodificador puede ir ~1.5 s adelantado respecto a lo que
 * suena; antes de next/prev/stop lo reubicamos en la pista que realmente suena. */
static void sync_cursor_locked(engine_t *e)
{
    if (e->now_id) pl_set_cursor(&e->pl, e->now_id);
}

/* ------------------------------------------------------------------ *
 *  Hilo decodificador (PRODUCTOR)                                     *
 * ------------------------------------------------------------------ */

static int decode_track(engine_t *e, uint32_t ep, uint32_t id, const char *path)
{
    decoder_t d;
    if (dec_open(&d, path) != 0) {
        fprintf(stderr, "[decoder] no se pudo abrir/formato no soportado: %s\n", path);
        return 1;                                    /* se salta la pista */
    }
    slot_t s;
    size_t frame = (size_t)d.channels * 2;
    size_t want  = SLOT_BYTES - SLOT_BYTES % frame;
    int ret = 1;
    size_t total = 0;
    for (;;) {
        ssize_t n = dec_read(&d, s.data, want);
        if (n <= 0) break;
        n -= n % (ssize_t)frame;
        if (n == 0) break;
        total += (size_t)n;
        s.len = (uint32_t)n; s.epoch = ep; s.track_id = id;
        s.rate = d.rate; s.channels = d.channels; s.eot = 0; s.last = 0;
        if (rb_write(&e->rb, &s) < 0) { ret = 0; break; }   /* flush o cierre */
    }
    if (ret && total == 0)
        fprintf(stderr, "[decoder] sin audio decodificado en %s%s\n", path,
                d.is_pipe ? " (¿esta instalado ffmpeg? sudo apt install ffmpeg)" : "");
    dec_close(&d);
    return ret;
}

static void *decoder_main(void *arg)
{
    engine_t *e = arg;
    int done = 0;                 /* la lista ya se decodificó completa en la época done_ep */
    uint32_t done_ep = 0;
    for (;;) {
        pthread_mutex_lock(&e->ctl);
        while (!e->quit && (e->state == ST_STOPPED || (done && e->epoch == done_ep)))
            pthread_cond_wait(&e->cv, &e->ctl);
        if (e->quit) { pthread_mutex_unlock(&e->ctl); break; }
        uint32_t ep = e->epoch;
        pthread_mutex_unlock(&e->ctl);

        for (;;) {                                   /* pistas consecutivas, misma época */
            char path[PL_PATH_MAX];
            uint32_t id;
            if (!pl_current(&e->pl, &id, path, sizeof path)) {   /* lista vacía */
                pthread_mutex_lock(&e->ctl);
                if (e->epoch == ep && e->state != ST_STOPPED) {
                    e->state = ST_STOPPED;
                    pthread_cond_broadcast(&e->cv);
                }
                pthread_mutex_unlock(&e->ctl);
                break;
            }
            if (!decode_track(e, ep, id, path)) break;           /* abortada */

            /* Fin natural de pista: avanzar cursor SOLO si nadie cambió la época. */
            pthread_mutex_lock(&e->ctl);
            if (e->quit || e->epoch != ep) { pthread_mutex_unlock(&e->ctl); break; }
            int more = pl_advance(&e->pl);
            pthread_mutex_unlock(&e->ctl);
            /* Si !more NO se pasa a STOPPED aquí: aún hay audio en el búfer. Lo hace
             * el consumidor al reproducir el slot "last". */

            slot_t eot;
            memset(&eot, 0, sizeof eot);
            eot.epoch = ep; eot.track_id = id; eot.eot = 1; eot.last = !more;
            if (rb_write(&e->rb, &eot) < 0) break;
            if (!more) { done = 1; done_ep = ep; break; }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 *  Hilo de salida de audio (CONSUMIDOR)                               *
 * ------------------------------------------------------------------ */

static void *output_main(void *arg)
{
    engine_t *e = arg;
    sink_t sink;
    sink_init(&sink, e->sink_kind);
    uint32_t seen = 0;
    int need_prefill = 1;
    slot_t *s = malloc(sizeof *s);
    if (!s) return NULL;

    for (;;) {
        int r;
        if (need_prefill) {
            r = rb_wait_level(&e->rb, PREFILL_SLOTS, &seen);
            if (r < 0) break;
            if (r == 1) { sink_close(&sink, 0); continue; }
            need_prefill = 0;
        }
        int underrun = 0;
        r = rb_read(&e->rb, s, &seen, &underrun);
        if (r < 0) break;
        if (r == 1) { sink_close(&sink, 0); need_prefill = 1; continue; }  /* skip/stop: silencio inmediato */

        pthread_mutex_lock(&e->ctl);
        while (!e->quit && e->state == ST_PAUSED && s->epoch == e->epoch)
            pthread_cond_wait(&e->cv, &e->ctl);      /* PAUSA: duerme, sin consumir CPU */
        if (e->quit) { pthread_mutex_unlock(&e->ctl); break; }
        if (s->epoch != e->epoch) {                  /* slot obsoleto tomado antes de un flush */
            pthread_mutex_unlock(&e->ctl);
            sink_close(&sink, 0);
            need_prefill = 1;
            continue;
        }
        if (underrun) { e->underruns++; need_prefill = 1; }   /* re-buffering tras underrun */
        if (s->len > 0) e->now_id = s->track_id;
        pthread_mutex_unlock(&e->ctl);

        if (s->len > 0) {
            if (sink.rate != s->rate || sink.ch != s->channels) {   /* cambio de formato */
                sink_close(&sink, 1);
                sink_open(&sink, s->rate, s->channels);
            }
            sink_write(&sink, s->data, s->len);
        }
        if (s->last) {                               /* fin de la lista: dejar drenar y avisar */
            sink_close(&sink, 1);
            pthread_mutex_lock(&e->ctl);
            if (s->epoch == e->epoch) { e->now_id = 0; e->state = ST_STOPPED; }
            pthread_cond_broadcast(&e->cv);
            pthread_mutex_unlock(&e->ctl);
            need_prefill = 1;
        }
    }
    sink_close(&sink, 0);
    free(s);
    return NULL;
}

/* ------------------------------------------------------------------ *
 *  Ciclo de vida                                                      *
 * ------------------------------------------------------------------ */

int engine_init(engine_t *e, sink_kind_t kind)
{
    memset(e, 0, sizeof *e);
    e->sink_kind = kind;
    e->state = ST_STOPPED;
    if (pl_init(&e->pl) != 0) return -1;
    if (rb_init(&e->rb, RB_SLOTS) != 0) { pl_destroy(&e->pl); return -1; }
    pthread_mutex_init(&e->ctl, NULL);
    pthread_cond_init(&e->cv, NULL);
    return 0;
}

int engine_start(engine_t *e)
{
    if (pthread_create(&e->dec_th, NULL, decoder_main, e) != 0) return -1;
    if (pthread_create(&e->out_th, NULL, output_main, e) != 0) {
        pthread_mutex_lock(&e->ctl); e->quit = 1; pthread_cond_broadcast(&e->cv); pthread_mutex_unlock(&e->ctl);
        rb_close(&e->rb);
        pthread_join(e->dec_th, NULL);
        return -1;
    }
    return 0;
}

void engine_shutdown(engine_t *e)
{
    pthread_mutex_lock(&e->ctl);
    e->quit = 1;
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->ctl);
    rb_close(&e->rb);                    /* despierta productor/consumidor dormidos en el búfer */
    pthread_join(e->dec_th, NULL);
    pthread_join(e->out_th, NULL);
    rb_destroy(&e->rb);
    pl_destroy(&e->pl);
    pthread_mutex_destroy(&e->ctl);
    pthread_cond_destroy(&e->cv);
}

/* ------------------------------------------------------------------ *
 *  Playlist: operaciones del usuario                                  *
 *  add / move / list NO tocan el cursor -> no necesitan ctl, corren   *
 *  en paralelo con el decodificador (que lee con rdlock).             *
 * ------------------------------------------------------------------ */

int engine_add(engine_t *e, const char *path)
{
    if (access(path, R_OK) != 0) return -1;
    return pl_add(&e->pl, path) ? 0 : -1;
}

int engine_move(engine_t *e, size_t from, size_t to)
{
    return pl_move(&e->pl, from, to);
}

int engine_remove(engine_t *e, size_t idx)
{
    pthread_mutex_lock(&e->ctl);
    uint32_t id; int was_cursor, exhausted;
    if (pl_remove(&e->pl, idx, &id, &was_cursor, &exhausted) != 0) {
        pthread_mutex_unlock(&e->ctl);
        return -1;
    }
    if (was_cursor || id == e->now_id) {     /* se eliminó la pista que suena / se decodifica */
        discontinuity_locked(e);
        if (exhausted && e->state != ST_STOPPED) e->state = ST_STOPPED;
    }
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->ctl);
    return 0;
}

void engine_clear(engine_t *e)
{
    pthread_mutex_lock(&e->ctl);
    pl_clear(&e->pl);
    discontinuity_locked(e);
    e->state = ST_STOPPED;
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->ctl);
}

/* ------------------------------------------------------------------ *
 *  Control de reproducción (asíncrono e inmediato)                    *
 * ------------------------------------------------------------------ */

int engine_play(engine_t *e)
{
    pthread_mutex_lock(&e->ctl);
    if (pl_count(&e->pl) == 0) { pthread_mutex_unlock(&e->ctl); return -1; }
    if (e->state == ST_STOPPED) { discontinuity_locked(e); e->state = ST_PLAYING; }
    else if (e->state == ST_PAUSED) e->state = ST_PLAYING;
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->ctl);
    return 0;
}

pstate_t engine_pause_toggle(engine_t *e)
{
    pthread_mutex_lock(&e->ctl);
    if (e->state == ST_PLAYING) e->state = ST_PAUSED;
    else if (e->state == ST_PAUSED) e->state = ST_PLAYING;
    pstate_t st = e->state;
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->ctl);
    return st;
}

void engine_stop(engine_t *e)
{
    pthread_mutex_lock(&e->ctl);
    sync_cursor_locked(e);
    discontinuity_locked(e);
    e->state = ST_STOPPED;
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->ctl);
}

int engine_next(engine_t *e)
{
    pthread_mutex_lock(&e->ctl);
    sync_cursor_locked(e);
    int more = pl_advance(&e->pl);
    if (e->state != ST_STOPPED) {
        discontinuity_locked(e);
        if (!more) e->state = ST_STOPPED;
    }
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->ctl);
    return more;
}

int engine_prev(engine_t *e)
{
    pthread_mutex_lock(&e->ctl);
    sync_cursor_locked(e);
    int ok = pl_prev(&e->pl);
    if (ok && e->state != ST_STOPPED) discontinuity_locked(e);
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->ctl);
    return ok ? 0 : -1;
}

int engine_jump(engine_t *e, size_t idx)
{
    pthread_mutex_lock(&e->ctl);
    if (pl_jump(&e->pl, idx) != 0) { pthread_mutex_unlock(&e->ctl); return -1; }
    discontinuity_locked(e);
    e->state = ST_PLAYING;
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->ctl);
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Consultas                                                          *
 * ------------------------------------------------------------------ */

void engine_print_list(engine_t *e, FILE *out)
{
    track_t *t; size_t n;
    pthread_mutex_lock(&e->ctl);
    uint32_t now = e->now_id;
    pthread_mutex_unlock(&e->ctl);
    if (pl_snapshot(&e->pl, &t, &n) != 0) { fprintf(out, "sin memoria\n"); return; }
    if (n == 0) fprintf(out, "(lista vacia)\n");
    for (size_t i = 0; i < n; i++)
        fprintf(out, "%c %2zu. %s\n", t[i].id == now ? '>' : ' ', i + 1, t[i].path);
    free(t);
}

void engine_print_status(engine_t *e, FILE *out)
{
    static const char *names[] = { "STOPPED", "PLAYING", "PAUSED" };
    char path[PL_PATH_MAX] = "-";
    pthread_mutex_lock(&e->ctl);
    pstate_t st = e->state; uint32_t now = e->now_id; unsigned long ur = e->underruns;
    pthread_mutex_unlock(&e->ctl);
    if (now && !pl_path_of(&e->pl, now, path, sizeof path)) strcpy(path, "(eliminada)");
    fprintf(out, "estado=%s  pista=%s  buffer=%zu/%d  underruns=%lu  pistas=%zu\n",
            names[st], path, rb_count(&e->rb), RB_SLOTS, ur, pl_count(&e->pl));
}

void engine_wait_idle(engine_t *e)
{
    pthread_mutex_lock(&e->ctl);
    while (!e->quit && !(e->state == ST_STOPPED && e->now_id == 0))
        pthread_cond_wait(&e->cv, &e->ctl);
    pthread_mutex_unlock(&e->ctl);
}
