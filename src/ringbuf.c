#include "ringbuf.h"
#include <stdlib.h>
#include <string.h>

int rb_init(ringbuf_t *r, size_t cap)
{
    memset(r, 0, sizeof *r);
    r->slots = calloc(cap, sizeof(slot_t));
    if (!r->slots) return -1;
    r->cap = cap;
    pthread_mutex_init(&r->m, NULL);
    pthread_cond_init(&r->not_full, NULL);
    pthread_cond_init(&r->not_empty, NULL);
    return 0;
}

void rb_destroy(ringbuf_t *r)
{
    free(r->slots);
    pthread_mutex_destroy(&r->m);
    pthread_cond_destroy(&r->not_full);
    pthread_cond_destroy(&r->not_empty);
}

int rb_write(ringbuf_t *r, const slot_t *s)
{
    pthread_mutex_lock(&r->m);
    while (r->count == r->cap && !r->closed && r->epoch == s->epoch)
        pthread_cond_wait(&r->not_full, &r->m);
    if (r->closed || r->epoch != s->epoch) {      /* productor obsoleto */
        pthread_mutex_unlock(&r->m);
        return -1;
    }
    r->slots[r->tail] = *s;
    r->tail = (r->tail + 1) % r->cap;
    r->count++;
    if (s->eot) r->eot_count++;
    pthread_cond_broadcast(&r->not_empty);
    pthread_mutex_unlock(&r->m);
    return 0;
}

int rb_read(ringbuf_t *r, slot_t *out, uint32_t *seen, int *underrun)
{
    pthread_mutex_lock(&r->m);
    if (r->count == 0 && underrun) *underrun = 1;
    while (r->count == 0 && !r->closed && r->epoch == *seen)
        pthread_cond_wait(&r->not_empty, &r->m);
    if (r->closed) { pthread_mutex_unlock(&r->m); return -1; }
    if (r->epoch != *seen) {
        *seen = r->epoch;
        pthread_mutex_unlock(&r->m);
        return 1;
    }
    *out = r->slots[r->head];
    r->head = (r->head + 1) % r->cap;
    r->count--;
    if (out->eot) r->eot_count--;
    pthread_cond_broadcast(&r->not_full);
    pthread_mutex_unlock(&r->m);
    return 0;
}

int rb_wait_level(ringbuf_t *r, size_t level, uint32_t *seen)
{
    pthread_mutex_lock(&r->m);
    while (r->count < level && r->eot_count == 0 && !r->closed && r->epoch == *seen)
        pthread_cond_wait(&r->not_empty, &r->m);
    int rc = 0;
    if (r->closed) rc = -1;
    else if (r->epoch != *seen) { *seen = r->epoch; rc = 1; }
    pthread_mutex_unlock(&r->m);
    return rc;
}

void rb_flush(ringbuf_t *r, uint32_t new_epoch)
{
    pthread_mutex_lock(&r->m);
    r->head = r->tail = r->count = r->eot_count = 0;
    r->epoch = new_epoch;
    pthread_cond_broadcast(&r->not_full);
    pthread_cond_broadcast(&r->not_empty);
    pthread_mutex_unlock(&r->m);
}

void rb_close(ringbuf_t *r)
{
    pthread_mutex_lock(&r->m);
    r->closed = 1;
    pthread_cond_broadcast(&r->not_full);
    pthread_cond_broadcast(&r->not_empty);
    pthread_mutex_unlock(&r->m);
}

size_t rb_count(ringbuf_t *r)
{
    pthread_mutex_lock(&r->m);
    size_t n = r->count;
    pthread_mutex_unlock(&r->m);
    return n;
}
