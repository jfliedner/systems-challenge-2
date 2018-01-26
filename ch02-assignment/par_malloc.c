#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>

#include "xmalloc.h"
// GENERAL INSPIRATION FOR FUNCTIONS TAKEN FROM hmem.c by NAT TUCK

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct nu_par_free_cell {
    int64_t size;
    struct nu_par_free_cell* next;
    pid_t thread; // thread associated with this cell
} nu_par_free_cell;

typedef struct state {
    int count;
    int index; // cur index
    pid_t th_id; // thread id
    pid_t threads[64]; // threads being used
} state;

__thread state st;

static const int64_t CHUNK_SIZE = 65536;
static const int64_t CELL_SIZE  = (int64_t)sizeof(nu_par_free_cell);

static nu_par_free_cell* nu_free_list = 0;
static nu_par_free_cell* nu_free_lists[64]; // list of free lists, 64 bytes

// inspiration taken from free_list_coalesce in hmem.c
// coalesces lists that are not just the global free_list
static
void
general_free_list_coalesce(nu_par_free_cell* to_coalesce) {
    nu_par_free_cell* pp = to_coalesce;
    while (pp != 0 && pp->next != 0) {
        if (((int64_t)pp) + pp->size == ((int64_t) pp->next)) {
            pp->size += pp->next->size;
            pp->next  = pp->next->next;
        }
        pp = pp->next;
    }
}

static
void
nu_free_list_coalesce(){
    nu_par_free_cell* pp = nu_free_list;
    while (pp != 0 && pp->next != 0) {
        if (((int64_t)pp) + pp->size == ((int64_t) pp->next)) {
            pp->size += pp->next->size;
            pp->next  = pp->next->next;
        }

        pp = pp->next;
    }
}

// inspiration taken from nu_free_list_insert in hmem.c
// insert function for free lists that are not the global one
static
void
general_free_list_insert(nu_par_free_cell* list, nu_par_free_cell* cell) {
    pthread_mutex_lock(&mutex);
    if (list == 0 || ((uint64_t) list) > ((uint64_t) cell)) {
        cell->next = list;
        list = cell;
        pthread_mutex_unlock(&mutex);
        return;
    }

    nu_par_free_cell* pp = list;

    while (pp->next != 0 && ((uint64_t)pp->next) < ((uint64_t) cell)) {
        pp = pp->next;
    }

    cell->next = pp->next;
    pp->next = cell;

    general_free_list_coalesce(list);
    pthread_mutex_unlock(&mutex);
}

static
void
nu_free_list_insert(nu_par_free_cell* cell) {
    pthread_mutex_lock(&mutex);
    if (nu_free_list == 0 || ((uint64_t) nu_free_list) > ((uint64_t) cell)) {
        cell->next = nu_free_list;
        nu_free_list = cell;
        pthread_mutex_unlock(&mutex);
        return;
    }

    nu_par_free_cell* pp = nu_free_list;

    while (pp->next != 0 && ((uint64_t)pp->next) < ((uint64_t) cell)) {
        pp = pp->next;
    }

    cell->next = pp->next;
    pp->next = cell;

    nu_free_list_coalesce();
    pthread_mutex_unlock(&mutex);
}

// inspiration taken from free_list_get_cell from hmem.c
// general version
static
nu_par_free_cell*
general_free_list_get_cell(nu_par_free_cell* free_list, int64_t size) {
    pthread_mutex_lock(&mutex);
    nu_par_free_cell** prev = &free_list;

    nu_par_free_cell* pp = free_list;
    for (; pp != 0; pp = pp->next) {
        if (pp->size >= size) {
            *prev = pp->next;
            pthread_mutex_unlock(&mutex);
            return pp;
        }
        prev = &(pp->next);
    }
    pthread_mutex_unlock(&mutex);
    return 0;
}

static
nu_par_free_cell*
free_list_get_cell(int64_t size) {
    pthread_mutex_lock(&mutex);
    nu_par_free_cell** prev = &nu_free_list;

    nu_par_free_cell* pp = nu_free_list;
    for (; pp != 0; pp = pp->next) {
        if (pp->size >= size) {
            *prev = pp->next;
            pthread_mutex_unlock(&mutex);
            return pp;
        }
        prev = &(pp->next);
    }
    pthread_mutex_unlock(&mutex);
    return 0;
}

static
nu_par_free_cell*
make_cell()
{
    void* addr = mmap(0, CHUNK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    nu_par_free_cell* cell = (nu_par_free_cell*) addr;
    cell->thread = pthread_self(); // thread for this cell
    cell->size = CHUNK_SIZE;
    return cell;
}

static
void*
opt_malloc(size_t bytes) {
    int64_t size = (int64_t) bytes + sizeof(int64_t) + sizeof(pid_t);
    if (st.count == 0) { // initialize the threads
        pid_t current = pthread_self();
        st.th_id = current;
        pthread_mutex_lock(&mutex);
        int ii = 0;
        while (ii < 64) {
            if (st.threads[ii] == 0) {
                st.threads[ii] = current;
                st.index = ii;
                break;
            }
            ++ii;
        }
        pthread_mutex_unlock(&mutex);
    }
    st.count = 1;

    if (size < CELL_SIZE) {
        size = CELL_SIZE;
    }

    if (size < CHUNK_SIZE) { // smaller allocations
        nu_par_free_cell* cell = free_list_get_cell(size);
        if (cell == 0) { // none available
            cell = general_free_list_get_cell(nu_free_lists[st.index], size);
            if (cell == 0) { // still none available with this thread, so have to make one
                cell = make_cell();
            }
        }

        int64_t leftover = cell->size - size;
        if (leftover >= CELL_SIZE) { // sending memory back
            nu_par_free_cell* add = (nu_par_free_cell*) ((void*)cell + size);
            add->size = leftover;
            nu_free_list_insert(add);
        }
        *((int64_t*)cell) = size;
        return (void*) cell + (sizeof(int64_t) + sizeof(pid_t));
    }
    else  { // larger allocations
        void* mem = mmap(0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
        *((int64_t*)mem) = size;
        return mem + (sizeof(int64_t) + sizeof(pid_t));
    }
}

static
void
opt_free(void* ptr) {
    nu_par_free_cell *free = (nu_par_free_cell *) (ptr - (sizeof(int64_t) + sizeof(pid_t))); // modified size
    int64_t free_size = *((int64_t *) free);
    if (free_size > CHUNK_SIZE) {
        munmap((void *) free, free_size);
        return;
    }

    free->size = free_size;
    if (pthread_equal(free->thread, st.th_id)) { // at this thread
        nu_free_list_insert(free);
        return;
    }

    int ii = 0;
    while (ii < 64) {
        pid_t thread = st.threads[ii];
        if (pthread_equal(thread, free->thread)) { // at the cell's thread
            general_free_list_insert(nu_free_lists[ii], free);
        }
        else if (pthread_equal(thread, st.th_id)) { // at the cur thread
            nu_par_free_cell* add = nu_free_lists[ii];
            while (add) {
                nu_free_list_insert(add);
                add = add->next;
            }

            break;
        }
        ++ii;
    }
}

//static
void*
xmalloc(size_t bytes)
{
    return opt_malloc(bytes);
}

void
xfree(void* ptr)
{
    opt_free(ptr);
}

void*
opt_realloc(void* prev, size_t bytes) {
    void* alloc = xmalloc(bytes);
    nu_par_free_cell* curr = (nu_par_free_cell*)(prev - (sizeof(int64_t) + sizeof(pid_t)));
    int64_t curr_size = *((int64_t*) curr);
    memcpy(alloc, prev, (curr_size - (sizeof(int64_t) + sizeof(pid_t))));
    xfree(prev);
    return alloc;
}

void*
xrealloc(void* prev, size_t bytes)
{
    return opt_realloc(prev, bytes);
}

