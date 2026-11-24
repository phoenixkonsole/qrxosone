/* The pthread APIs needed by the native Gecko port, independently of C++
 * and compiler TLS. Run in the EC64 guest with its default task affinity.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

enum { WORKERS = 2, INCREMENTS = 4096, LOCKED_INCREMENTS = 64 };

struct worker {
    const char *failure;
    unsigned destructor_calls;
};

static pthread_key_t state_key;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint init_calls;
static atomic_uint total;
static unsigned locked_total;

static void init_worker_runtime(void)
{
    atomic_fetch_add_explicit(&init_calls, 1, memory_order_relaxed);
}

static void destroy_state(void *value)
{
    struct worker *worker = value;
    ++worker->destructor_calls;
}

static void *run_worker(void *arg)
{
    struct worker *worker = arg;

    if (pthread_once(&once, init_worker_runtime) != 0 ||
        atomic_load_explicit(&init_calls, memory_order_relaxed) != 1) {
        worker->failure = "pthread_once did not initialize the runtime once";
        return NULL;
    }
    if (pthread_getspecific(state_key) != NULL) {
        worker->failure = "key inherited a value";
        return NULL;
    }
    if (pthread_setspecific(state_key, worker) != 0) {
        worker->failure = "pthread_setspecific failed";
        return NULL;
    }
    for (unsigned i = 0; i < INCREMENTS; ++i)
        atomic_fetch_add_explicit(&total, 1, memory_order_relaxed);
    for (unsigned i = 0; i < LOCKED_INCREMENTS; ++i) {
        if (pthread_mutex_lock(&mutex) != 0) {
            worker->failure = "pthread_mutex_lock failed";
            return NULL;
        }
        ++locked_total;
        if (pthread_mutex_unlock(&mutex) != 0) {
            worker->failure = "pthread_mutex_unlock failed";
            return NULL;
        }
    }
    if (pthread_getspecific(state_key) != worker) {
        worker->failure = "key value changed";
        return NULL;
    }
    return worker;
}

int main(void)
{
    static struct worker main_state;
    static struct worker workers[WORKERS];
    pthread_t threads[WORKERS];
    unsigned created = 0;
    int result = 0;
    int error = pthread_key_create(&state_key, destroy_state);

    if (error != 0) {
        printf("FAIL: pthread_key_create (%d)\n", error);
        return 20;
    }
    error = pthread_setspecific(state_key, &main_state);
    if (error != 0) {
        printf("FAIL: pthread_setspecific in main (%d)\n", error);
        pthread_key_delete(state_key);
        return 20;
    }

    for (; created < WORKERS; ++created) {
        error = pthread_create(&threads[created], NULL, run_worker, &workers[created]);
        if (error != 0) {
            printf("FAIL: pthread_create worker %u (%d)\n", created, error);
            result = 20;
            break;
        }
    }

    unsigned joined = 0;
    for (unsigned i = 0; i < created; ++i) {
        void *value = NULL;

        error = pthread_join(threads[i], &value);
        if (error != 0) {
            printf("FAIL: pthread_join worker %u (%d)\n", i, error);
            result = 20;
            continue;
        }
        ++joined;
        if (workers[i].failure != NULL) {
            printf("FAIL: worker %u: %s\n", i, workers[i].failure);
            result = 20;
        } else if (value != &workers[i] || workers[i].destructor_calls != 1) {
            printf("FAIL: worker %u: return value or key destructor\n", i);
            result = 20;
        }
    }
    if (joined != created)
        return 20;

    if (pthread_getspecific(state_key) != &main_state || main_state.destructor_calls != 0) {
        puts("FAIL: main thread's key value or destructor");
        result = 20;
    }
    error = pthread_key_delete(state_key);
    if (error != 0) {
        printf("FAIL: pthread_key_delete (%d)\n", error);
        result = 20;
    }

    unsigned count = atomic_load_explicit(&total, memory_order_relaxed);
    if (count != WORKERS * INCREMENTS) {
        printf("FAIL: worker count %u, expected %u\n", count, (unsigned)(WORKERS * INCREMENTS));
        result = 20;
    }
    if (locked_total != WORKERS * LOCKED_INCREMENTS) {
        printf("FAIL: locked worker count %u, expected %u\n", locked_total,
               (unsigned)(WORKERS * LOCKED_INCREMENTS));
        result = 20;
    }
    if (result == 0)
        puts("PASS: pthread create/join, key isolation and destructors");
    return result;
}
