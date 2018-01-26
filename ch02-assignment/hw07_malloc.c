

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include "hmem.c"

/* CH02 TODO:
 *  - This should call / use your HW07 alloctor, *DONE*
 *    modified to be thread-safe and have a realloc function. *DONE*
 */

/*
 * Code taken from hmem.c by Nat Tuck
 */


static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void*
xmalloc(size_t bytes)
{
    pthread_mutex_lock(&mutex);
    void* hmalloc_return = hmalloc(bytes);
    pthread_mutex_unlock(&mutex);
    return hmalloc_return;

}

void
xfree(void* ptr)
{
    pthread_mutex_lock(&mutex);
    hfree(ptr);
    pthread_mutex_unlock(&mutex);
}

void*
hrealloc(void* prev, size_t bytes) {

    void* realloc = xmalloc(bytes);
    void* free_cell = prev - sizeof(int64_t);
    free_cell = (nu_free_cell*)free_cell;
    int64_t cell_size = *((int64_t*) free_cell);
    memcpy(realloc, prev, (cell_size - sizeof(int64_t)));
    xfree(prev);
    return realloc;
}

void*
xrealloc(void* prev, size_t bytes)
{
    return hrealloc(prev, bytes);

}

