#include "playlist.h"
#include <stdlib.h>
#include <string.h>

/* Debe llamarse con el lock tomado y count > 0. */
static size_t cursor_index(const playlist_t *p)
{
    if (p->cur_id == 0) return 0;
    for (size_t i = 0; i < p->count; i++)
        if (p->items[i].id == p->cur_id) return i;
    return 0;
}

int pl_init(playlist_t *p)
{
    memset(p, 0, sizeof *p);
    p->next_id = 1;
    return pthread_rwlock_init(&p->lock, NULL);
}

void pl_destroy(playlist_t *p)
{
    free(p->items);
    pthread_rwlock_destroy(&p->lock);
}

uint32_t pl_add(playlist_t *p, const char *path)
{
    if (strlen(path) >= PL_PATH_MAX) return 0;
    pthread_rwlock_wrlock(&p->lock);
    if (p->count == p->cap) {
        size_t ncap = p->cap ? p->cap * 2 : 16;
        track_t *n = realloc(p->items, ncap * sizeof *n);
        if (!n) { pthread_rwlock_unlock(&p->lock); return 0; }
        p->items = n; p->cap = ncap;
    }
    track_t *t = &p->items[p->count++];
    t->id = p->next_id++;
    strcpy(t->path, path);
    uint32_t id = t->id;
    pthread_rwlock_unlock(&p->lock);
    return id;
}

int pl_remove(playlist_t *p, size_t idx, uint32_t *removed_id,
              int *was_cursor, int *exhausted)
{
    pthread_rwlock_wrlock(&p->lock);
    if (idx >= p->count) { pthread_rwlock_unlock(&p->lock); return -1; }
    size_t ci = cursor_index(p);
    *removed_id = p->items[idx].id;
    *was_cursor = (idx == ci);
    *exhausted = 0;
    memmove(&p->items[idx], &p->items[idx + 1], (p->count - idx - 1) * sizeof(track_t));
    p->count--;
    if (*was_cursor) {
        if (idx < p->count) p->cur_id = p->items[idx].id;   /* la siguiente pista */
        else { p->cur_id = 0; *exhausted = 1; }
    }
    pthread_rwlock_unlock(&p->lock);
    return 0;
}

int pl_move(playlist_t *p, size_t from, size_t to)
{
    pthread_rwlock_wrlock(&p->lock);
    if (from >= p->count || to >= p->count) { pthread_rwlock_unlock(&p->lock); return -1; }
    if (from != to) {
        track_t tmp = p->items[from];
        if (from < to) memmove(&p->items[from], &p->items[from + 1], (to - from) * sizeof tmp);
        else           memmove(&p->items[to + 1], &p->items[to], (from - to) * sizeof tmp);
        p->items[to] = tmp;
    }
    pthread_rwlock_unlock(&p->lock);
    return 0;
}

void pl_clear(playlist_t *p)
{
    pthread_rwlock_wrlock(&p->lock);
    p->count = 0;
    p->cur_id = 0;
    pthread_rwlock_unlock(&p->lock);
}

size_t pl_count(playlist_t *p)
{
    pthread_rwlock_rdlock(&p->lock);
    size_t n = p->count;
    pthread_rwlock_unlock(&p->lock);
    return n;
}

int pl_current(playlist_t *p, uint32_t *id, char *path, size_t n)
{
    pthread_rwlock_rdlock(&p->lock);
    if (p->count == 0) { pthread_rwlock_unlock(&p->lock); return 0; }
    const track_t *t = &p->items[cursor_index(p)];
    *id = t->id;
    strncpy(path, t->path, n - 1); path[n - 1] = '\0';
    pthread_rwlock_unlock(&p->lock);
    return 1;
}

int pl_path_of(playlist_t *p, uint32_t id, char *path, size_t n)
{
    int found = 0;
    pthread_rwlock_rdlock(&p->lock);
    for (size_t i = 0; i < p->count; i++)
        if (p->items[i].id == id) {
            strncpy(path, p->items[i].path, n - 1); path[n - 1] = '\0';
            found = 1; break;
        }
    pthread_rwlock_unlock(&p->lock);
    return found;
}

int pl_advance(playlist_t *p)
{
    int more = 0;
    pthread_rwlock_wrlock(&p->lock);
    if (p->count > 0) {
        size_t ci = cursor_index(p);
        if (ci + 1 < p->count) { p->cur_id = p->items[ci + 1].id; more = 1; }
        else p->cur_id = 0;
    }
    pthread_rwlock_unlock(&p->lock);
    return more;
}

int pl_prev(playlist_t *p)
{
    int ok = 0;
    pthread_rwlock_wrlock(&p->lock);
    if (p->count > 0) {
        size_t ci = cursor_index(p);
        p->cur_id = p->items[ci > 0 ? ci - 1 : 0].id;
        ok = 1;
    }
    pthread_rwlock_unlock(&p->lock);
    return ok;
}

int pl_jump(playlist_t *p, size_t idx)
{
    int ok = -1;
    pthread_rwlock_wrlock(&p->lock);
    if (idx < p->count) { p->cur_id = p->items[idx].id; ok = 0; }
    pthread_rwlock_unlock(&p->lock);
    return ok;
}

void pl_set_cursor(playlist_t *p, uint32_t id)
{
    pthread_rwlock_wrlock(&p->lock);
    for (size_t i = 0; i < p->count; i++)
        if (p->items[i].id == id) { p->cur_id = id; break; }
    pthread_rwlock_unlock(&p->lock);
}

int pl_snapshot(playlist_t *p, track_t **out, size_t *n)
{
    pthread_rwlock_rdlock(&p->lock);
    *n = p->count;
    *out = NULL;
    if (p->count) {
        *out = malloc(p->count * sizeof(track_t));
        if (!*out) { pthread_rwlock_unlock(&p->lock); return -1; }
        memcpy(*out, p->items, p->count * sizeof(track_t));
    }
    pthread_rwlock_unlock(&p->lock);
    return 0;
}
