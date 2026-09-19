/*
 * lockfree_stack.c - a Treiber stack: push and pop with compare-and-swap
 *
 * The program runs the same workload on two stacks:
 *   1. a lock-free stack (compare-and-swap on the head pointer)
 *   2. the same stack protected by a mutex
 * In both runs the threads push AND pop at the same time. Afterwards every
 * value must have been seen exactly once (popped, or still in the stack).
 *
 * Why a real-time programmer cares: a thread that holds a mutex can be
 * preempted, and then everybody who needs the mutex waits for it, however
 * high their priority. A lock-free operation never waits for another thread.
 * If its compare-and-swap fails, that is because ANOTHER thread's operation
 * succeeded in the meantime. The system as a whole always makes progress.
 *
 * Memory: all nodes come from one array allocated before the threads start
 * and freed after they have all been joined. Two reasons:
 *   - no malloc/free in the time-critical path (allocation time is unbounded)
 *   - a popped node is never freed or reused while threads run, so reading
 *     old_head->next can never touch freed memory, and an address can never
 *     come back to the head. aba_demo.c shows what goes wrong otherwise.
 *
 * Usage:  ./lockfree_stack [threads] [ops_per_thread]
 *   defaults: 4 threads, 200000 push+pop pairs per thread
 */

#include "rt.h"

#include <stdatomic.h>
#include <stdbool.h>

struct node {
    unsigned long value;
    struct node *next;
};

/* ------------------------------------------------------ lock-free stack ---- */

/*
 * _Atomic(...) marks a variable that several threads may read and write at
 * the same time. It is only touched through the atomic_* functions.
 * We use their default forms everywhere. There are also *_explicit variants
 * that take a "memory order" argument; the default is the safest order, and
 * the explicit ones are an optimisation hint that these labs do not need.
 */
struct lf_stack {
    _Atomic(struct node *) head;
    atomic_long retries; /* failed compare-and-swap in pop, for curiosity */
};

static void lf_push(struct lf_stack *s, struct node *n) {
    /* 1. Read the current head. */
    struct node *old_head = atomic_load(&s->head);
    for (;;) {
        /* 2. Prepare: the new node points at what we believe is the head. */
        n->next = old_head;
        /*
         * 3. Compare-and-swap, one indivisible step:
         *      "if head is still old_head, make it n".
         *    On failure old_head is updated to the current head and we go
         *    round again. A failure means another thread just succeeded, so
         *    nobody was blocked: this is what "lock-free" means.
         */
        if (atomic_compare_exchange_weak(&s->head, &old_head, n)) return;
    }
}

static struct node *lf_pop(struct lf_stack *s) {
    /* 1. Read the current head. */
    struct node *old_head = atomic_load(&s->head);
    while (old_head != NULL) {
        /* 2. Prepare: the head after the pop. Reading old_head->next is safe
         *    only because nodes are never freed while threads run. */
        struct node *next = old_head->next;
        /* 3. Compare-and-swap: "if head is still old_head, make it next". */
        if (atomic_compare_exchange_weak(&s->head, &old_head, next)) return old_head;
        atomic_fetch_add(&s->retries, 1);
    }
    return NULL; /* the stack was empty */
}

/* ---------------------------------------------------------- mutex stack ---- */

/*
 * The classic way. Correct and simple, but blocking: a thread that is
 * preempted between lock and unlock stops every other thread that needs the
 * stack until it runs again.
 */
struct mx_stack {
    pthread_mutex_t mutex;
    struct node *head;
};

static void mx_push(struct mx_stack *s, struct node *n) {
    pthread_mutex_lock(&s->mutex);
    n->next = s->head;
    s->head = n;
    pthread_mutex_unlock(&s->mutex);
}

static struct node *mx_pop(struct mx_stack *s) {
    pthread_mutex_lock(&s->mutex);
    struct node *n = s->head;
    if (n != NULL) s->head = n->next;
    pthread_mutex_unlock(&s->mutex);
    return n;
}

/* -------------------------------------------------------------- workers ---- */

/* Both stacks live here; use_lock_free says which one a run exercises. */
struct shared {
    bool use_lock_free;
    struct lf_stack lf;
    struct mx_stack mx;
    pthread_barrier_t start; /* all threads begin at the same moment */
};

static void stack_push(struct shared *sh, struct node *n) {
    if (sh->use_lock_free) {
        lf_push(&sh->lf, n);
    } else {
        mx_push(&sh->mx, n);
    }
}

static struct node *stack_pop(struct shared *sh) {
    if (sh->use_lock_free) return lf_pop(&sh->lf);
    return mx_pop(&sh->mx);
}

struct work {
    struct shared *shared;
    struct node *nodes;    /* this thread's own slice of the node array */
    unsigned long *popped; /* values this thread popped (room for `ops`) */
    long npopped;
    long ops;
};

static void *worker(void *arg) {
    struct work *w = arg;
    pthread_barrier_wait(&w->shared->start);
    for (long i = 0; i < w->ops; i++) {
        stack_push(w->shared, &w->nodes[i]);
        /* Pop right away: pushes and pops of all threads interleave. */
        struct node *n = stack_pop(w->shared);
        if (n != NULL) {
            w->popped[w->npopped] = n->value;
            w->npopped++;
        }
    }
    return NULL;
}

struct result {
    double seconds;
    bool correct;
    long retries;
};

/* Count one sighting of value v. Returns false if v is bad or seen twice. */
static bool mark_seen(unsigned char *seen, unsigned long total, unsigned long v) {
    if (v >= total) return false;
    if (seen[v] != 0) return false;
    seen[v] = 1;
    return true;
}

static struct result run(bool use_lock_free, int threads, long ops) {
    struct result r = {0.0, true, 0};
    const unsigned long total = (unsigned long)threads * (unsigned long)ops;

    /* Everything is allocated here, before any thread exists. */
    struct node *nodes = malloc(total * sizeof(struct node));
    unsigned long *popped = malloc(total * sizeof(unsigned long));
    unsigned char *seen = calloc(total, 1);
    struct work *work = calloc(threads, sizeof(struct work));
    pthread_t *ids = calloc(threads, sizeof(pthread_t));
    if (!nodes || !popped || !seen || !work || !ids) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    for (unsigned long i = 0; i < total; i++) {
        nodes[i].value = i;
        nodes[i].next = NULL;
    }

    struct shared sh;
    sh.use_lock_free = use_lock_free;
    atomic_init(&sh.lf.head, NULL);
    atomic_init(&sh.lf.retries, 0);
    sh.mx.head = NULL;
    if (rt_mutex_init(&sh.mx.mutex, 0) != 0) exit(1);
    pthread_barrier_init(&sh.start, NULL, threads + 1);

    for (int t = 0; t < threads; t++) {
        work[t].shared = &sh;
        work[t].nodes = &nodes[(unsigned long)t * ops];
        work[t].popped = &popped[(unsigned long)t * ops];
        work[t].npopped = 0;
        work[t].ops = ops;
        if (rt_start_thread(&ids[t], worker, &work[t], 0, -1) != 0) exit(1);
    }

    struct timespec t0 = rt_now();
    pthread_barrier_wait(&sh.start);
    for (int t = 0; t < threads; t++) pthread_join(ids[t], NULL);
    r.seconds = rt_elapsed_ns(t0) / 1e9;

    /* Every value must appear exactly once: popped by some thread, or left. */
    for (int t = 0; t < threads; t++) {
        for (long i = 0; i < work[t].npopped; i++) {
            if (!mark_seen(seen, total, work[t].popped[i])) r.correct = false;
        }
    }
    for (struct node *n = stack_pop(&sh); n != NULL; n = stack_pop(&sh)) {
        if (!mark_seen(seen, total, n->value)) r.correct = false;
    }
    for (unsigned long i = 0; i < total; i++) {
        if (seen[i] != 1) r.correct = false;
    }
    r.retries = atomic_load(&sh.lf.retries);

    pthread_barrier_destroy(&sh.start);
    pthread_mutex_destroy(&sh.mx.mutex);
    free(ids);
    free(work);
    free(seen);
    free(popped);
    free(nodes); /* only now, with every thread joined */
    return r;
}

int main(int argc, char **argv) {
    const int threads = (int)rt_arg_long(argc, argv, 1, 4, 1, 64);
    const long ops = rt_arg_long(argc, argv, 2, 200000, 1, 10000000);
    const double total_ops = 2.0 * threads * ops; /* one push + one pop */

    struct lf_stack probe;
    atomic_init(&probe.head, NULL);
    printf("Treiber stack: %d threads, %ld push+pop pairs each\n", threads, ops);
    printf("_Atomic(struct node *) is lock-free on this machine: %s\n\n",
           atomic_is_lock_free(&probe.head) ? "yes" : "no");

    struct result lf = run(true, threads, ops);
    printf("lock-free  : %s, %.3f s, %.2f M ops/s, %ld CAS retries in pop\n",
           lf.correct ? "correct" : "BROKEN", lf.seconds, total_ops / lf.seconds / 1e6,
           lf.retries);

    struct result mx = run(false, threads, ops);
    printf("mutex      : %s, %.3f s, %.2f M ops/s\n", mx.correct ? "correct" : "BROKEN",
           mx.seconds, total_ops / mx.seconds / 1e6);

    if (lf.seconds < mx.seconds) {
        printf("\nfaster here: lock-free (%.1fx)\n", mx.seconds / lf.seconds);
    } else {
        printf("\nfaster here: mutex (%.1fx)\n", lf.seconds / mx.seconds);
    }
    printf("Throughput is an average. Neither number says anything about the worst case.\n");

    if (lf.correct && mx.correct) return 0;
    return 1;
}
