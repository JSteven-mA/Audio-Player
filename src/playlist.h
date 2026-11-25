/* playlist.h - Lista de reproducción compartida protegida con pthread_rwlock.
 *
 * Escritores (wrlock): add, remove, move, clear, advance, prev, jump, set_cursor
 * Lectores   (rdlock): current, path_of, count, snapshot
 *
 * El "cursor" se guarda como ID de pista (no como índice): así sobrevive a
 * reordenamientos y a eliminaciones de pistas intermedias.
 * cur_id == 0 significa "ninguna seleccionada: empezar por la primera".
 * Las pistas se copian hacia afuera (path por valor): nadie conserva punteros
 * a nodos internos, por lo que no hay acceso a memoria liberada.
 */
#ifndef PLAYLIST_H
#define PLAYLIST_H
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define PL_PATH_MAX 512

typedef struct { uint32_t id; char path[PL_PATH_MAX]; } track_t;

typedef struct {
    pthread_rwlock_t lock;
    track_t *items;
    size_t count, cap;
    uint32_t next_id;
    uint32_t cur_id;
} playlist_t;

int      pl_init(playlist_t *p);
void     pl_destroy(playlist_t *p);

uint32_t pl_add(playlist_t *p, const char *path);               /* id o 0 si error */
int      pl_remove(playlist_t *p, size_t idx, uint32_t *removed_id,
                   int *was_cursor, int *exhausted);            /* 0 ok, -1 idx inválido */
int      pl_move(playlist_t *p, size_t from, size_t to);        /* 0 ok, -1 inválido */
void     pl_clear(playlist_t *p);

size_t   pl_count(playlist_t *p);
int      pl_current(playlist_t *p, uint32_t *id, char *path, size_t n); /* 1 ok, 0 vacía */
int      pl_path_of(playlist_t *p, uint32_t id, char *path, size_t n);  /* 1 ok, 0 no existe */

int      pl_advance(playlist_t *p);   /* 1 avanzó, 0 fin de lista (cursor -> 0) */
int      pl_prev(playlist_t *p);      /* 1 ok (si ya es la primera, la reinicia), 0 vacía */
int      pl_jump(playlist_t *p, size_t idx);
void     pl_set_cursor(playlist_t *p, uint32_t id);

/* Copia consistente de la lista (el llamador hace free). */
int      pl_snapshot(playlist_t *p, track_t **out, size_t *n);

#endif
