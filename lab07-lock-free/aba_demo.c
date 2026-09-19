/*
 * aba_demo.c - the ABA problem, step by step, and the tagged-pointer fix
 *
 * Two threads share a lock-free stack  A -> B -> C.  The interleaving is
 * forced with semaphores, so the bad case happens on every run:
 *
 *   thread 1: starts pop, reads head = A and A->next = B, then is "preempted"
 *   thread 2: pops A, pops B, frees B, reuses node A for a new value, pushes A
 *             stack is now  A' -> C   (A' is the same address as A)
 *   thread 1: resumes, CAS(head, A, B) ...
 *
 * A plain compare-and-swap (CAS) compares addresses only. The head is A
 * again, so the CAS succeeds and installs B, a node that was already freed.
 * That is ABA: the value went A -> B -> A and the CAS could not tell.
 *
 * In a real-time system "thread 1 is preempted in the middle of pop" is not
 * rare: it is exactly what a higher-priority thread does to a lower one.
 *
 * The fix used here: the head is a (pointer, tag) pair and every successful
 * push or pop increments the tag. Thread 1 read (A, tag 3); after thread 2's
 * two pops and one push the head is (A, tag 6), so the CAS fails and thread 1
 * retries with the real head.
 *
 * Usage:  ./aba_demo            freed nodes are only marked (no real free)
 *         ./aba_demo delete     really free node B (only in the ASan build:
 *                               make asan && ./build/aba_demo_asan delete)
 */

#include "rt.h"

#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>

enum node_state { IN_STACK, POPPED, FREED };

struct node {
    char name;
    int value;
    enum node_state state;
    struct node *next;
};

static bool real_delete = false;

/*
 * "Free" a node. In normal mode it is only marked, so the program can report
 * what happened. In delete mode the memory really goes back to the allocator.
 */
static void free_node(struct node *n) {
    if (real_delete) {
        free(n);
    } else {
        n->state = FREED;
    }
}

static struct node *new_node(char name, int value) {
    struct node *n = malloc(sizeof(struct node));
    if (n == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    n->name = name;
    n->value = value;
    n->state = FREED;
    n->next = NULL;
    return n;
}

/* ---------------------------------------------------------------- stack ---- */

/*
 * What a thread remembers between reading the head and its CAS.
 * It is also the type of the tagged head: two 8-byte fields and no padding.
 * That matters, because a CAS on a struct compares all of its bytes.
 */
struct tagged {
    struct node *ptr;
    unsigned long tag;
};
_Static_assert(sizeof(struct tagged) == 2 * sizeof(void *), "struct tagged must have no padding");

/*
 * One stack type for both experiments:
 *   use_tag = false  the head is plain_head, a bare pointer
 *   use_tag = true   the head is tagged_head, pointer + tag
 * All atomic_* calls use their default form; see lockfree_stack.c.
 */
struct stack {
    bool use_tag;
    _Atomic(struct node *) plain_head;
    _Atomic(struct tagged) tagged_head;
};

static void stack_init(struct stack *s, bool use_tag) {
    struct tagged empty;
    memset(&empty, 0, sizeof(empty));
    s->use_tag = use_tag;
    atomic_init(&s->plain_head, NULL);
    atomic_init(&s->tagged_head, empty);
}

/* Read the head (and its tag, if the stack has one). */
static struct tagged stack_read(struct stack *s) {
    struct tagged seen;
    memset(&seen, 0, sizeof(seen));
    if (s->use_tag) {
        seen = atomic_load(&s->tagged_head);
    } else {
        seen.ptr = atomic_load(&s->plain_head);
    }
    return seen;
}

/*
 * The CAS at the heart of pop: "if the head is still what I saw, make it
 * `next`". Returns true on success. On failure *seen is refreshed to the
 * current head.
 *   plain:  compares the pointer only
 *   tagged: compares pointer AND tag, and installs tag + 1
 */
static bool stack_try_pop(struct stack *s, struct tagged *seen, struct node *next) {
    struct node *popped = seen->ptr;
    bool ok;
    if (s->use_tag) {
        struct tagged wanted;
        memset(&wanted, 0, sizeof(wanted));
        wanted.ptr = next;
        wanted.tag = seen->tag + 1;
        ok = atomic_compare_exchange_strong(&s->tagged_head, seen, wanted);
    } else {
        ok = atomic_compare_exchange_strong(&s->plain_head, &seen->ptr, next);
    }
    if (ok) popped->state = POPPED;
    return ok;
}

static struct node *stack_pop(struct stack *s) {
    struct tagged seen = stack_read(s);
    while (seen.ptr != NULL) {
        if (stack_try_pop(s, &seen, seen.ptr->next)) return seen.ptr;
    }
    return NULL;
}

static void stack_push(struct stack *s, struct node *n) {
    struct tagged seen = stack_read(s);
    for (;;) {
        n->next = seen.ptr;
        bool ok;
        if (s->use_tag) {
            struct tagged wanted;
            memset(&wanted, 0, sizeof(wanted));
            wanted.ptr = n;
            wanted.tag = seen.tag + 1;
            ok = atomic_compare_exchange_strong(&s->tagged_head, &seen, wanted);
        } else {
            ok = atomic_compare_exchange_strong(&s->plain_head, &seen.ptr, n);
        }
        if (ok) break;
    }
    n->state = IN_STACK;
}

/* "A" for the plain stack, "A tag 3" for the tagged one. */
static void describe(struct stack *s, struct tagged seen, char *buf, size_t size) {
    if (s->use_tag) {
        snprintf(buf, size, "%c tag %lu", seen.ptr->name, seen.tag);
    } else {
        snprintf(buf, size, "%c", seen.ptr->name);
    }
}

/* ------------------------------------------------------------- scenario ---- */

struct scenario {
    struct stack stack;
    sem_t t1_paused; /* thread 1 has read the head and "is preempted" */
    sem_t t2_done;   /* thread 2 has finished; thread 1 may resume */
    struct node *t1_popped;
    bool t1_first_cas_ok;
};

static void *thread1(void *arg) {
    struct scenario *sc = arg;
    char text[32];

    /* First half of a pop: read the head and its next pointer. */
    struct tagged seen = stack_read(&sc->stack);
    struct node *next = seen.ptr->next;
    char next_name = next->name; /* remembered, B may be freed later */
    describe(&sc->stack, seen, text, sizeof(text));
    printf("thread 1: read head %s, next %c, then gets preempted\n", text, next_name);

    sem_post(&sc->t1_paused);
    sem_wait(&sc->t2_done);

    /* Second half of the pop, with values that are now stale. */
    sc->t1_first_cas_ok = stack_try_pop(&sc->stack, &seen, next);
    if (sc->t1_first_cas_ok) {
        printf("thread 1: CAS(head, A -> %c) succeeded\n", next_name);
    } else {
        describe(&sc->stack, seen, text, sizeof(text));
        printf("thread 1: CAS failed, head is now %s. Retrying.\n", text);
        while (!stack_try_pop(&sc->stack, &seen, seen.ptr->next)) {
        }
    }
    sc->t1_popped = seen.ptr;
    return NULL;
}

static void *thread2(void *arg) {
    struct scenario *sc = arg;
    sem_wait(&sc->t1_paused);

    struct node *x = stack_pop(&sc->stack);
    struct node *y = stack_pop(&sc->stack);
    printf("thread 2: popped %c(%d) and %c(%d), frees %c\n", x->name, x->value, y->name,
           y->value, y->name);
    free_node(y);
    x->value = 4; /* node A is recycled for a new value */
    stack_push(&sc->stack, x);
    printf("thread 2: pushed recycled node A(4). Stack: A(4) -> C(3)\n");

    sem_post(&sc->t2_done);
    return NULL;
}

/* Runs the story once. Returns true if ABA corrupted the stack. */
static bool run_scenario(bool use_tag) {
    printf("=== %s ===\n", use_tag ? "tagged pointer CAS" : "plain pointer CAS");

    struct scenario sc;
    stack_init(&sc.stack, use_tag);
    sem_init(&sc.t1_paused, 0, 0);
    sem_init(&sc.t2_done, 0, 0);
    sc.t1_popped = NULL;
    sc.t1_first_cas_ok = false;

    struct node *a = new_node('A', 1);
    struct node *b = new_node('B', 2);
    struct node *c = new_node('C', 3);
    stack_push(&sc.stack, c);
    stack_push(&sc.stack, b);
    stack_push(&sc.stack, a);
    printf("initial stack: A(1) -> B(2) -> C(3)\n");

    pthread_t t1, t2;
    if (pthread_create(&t1, NULL, thread1, &sc) != 0) exit(1);
    if (pthread_create(&t2, NULL, thread2, &sc) != 0) exit(1);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    /*
     * Values 1, 2, 3 and 4 were pushed. 1 and 2 were popped by thread 2,
     * thread 1 popped one more, so exactly one value must be left.
     */
    printf("thread 1 popped value %d\n", sc.t1_popped->value);
    printf("stack now:");
    int left = 0;
    bool freed_node_reachable = false;
    struct node *n = stack_read(&sc.stack).ptr;
    while (n != NULL && left < 10) {
        bool freed = (n->state == FREED);
        if (freed) freed_node_reachable = true;
        printf(" %c(%d)%s ->", n->name, n->value, freed ? " [FREED]" : "");
        n = n->next;
        left++;
    }
    printf(" end\n");

    bool aba = sc.t1_first_cas_ok && (freed_node_reachable || left != 1);
    if (aba) {
        printf("ABA happened: the head was A again, so the stale CAS succeeded.\n");
        printf("  The stack now starts at freed node B, value 2 is 'in' the stack a\n");
        printf("  second time and the %d left entries should have been 1.\n\n", left);
    } else if (!sc.t1_first_cas_ok) {
        printf("tagged CAS rejected stale head: the tag changed while thread 1 was away.\n");
        printf("  The retry popped the real head, %d entry left, no freed node reachable.\n\n",
               left);
    }

    /* In delete mode B is already gone; the walk above read freed memory. */
    free(a);
    if (!real_delete) free(b);
    free(c);
    sem_destroy(&sc.t1_paused);
    sem_destroy(&sc.t2_done);
    return aba;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "delete") == 0) {
#if defined(__SANITIZE_ADDRESS__)
        real_delete = true;
        printf("delete mode: node B is really freed. AddressSanitizer will report\n"
               "the read of freed memory below.\n\n");
        run_scenario(false);
        return 0;
#else
        fprintf(stderr, "delete mode reads freed memory (undefined behaviour).\n"
                        "Run it only in the ASan build: make asan && ./build/aba_demo_asan delete\n");
        return 2;
#endif
    }

    /* A 16-byte CAS. arm64 goes through libatomic; x86-64 needs -mcx16 to
     * use the CMPXCHG16B instruction. See the README. */
    struct stack probe;
    stack_init(&probe, true);
    printf("_Atomic(struct tagged) is lock-free here: %s\n\n",
           atomic_is_lock_free(&probe.tagged_head) ? "yes" : "no (uses a lock)");

    bool plain_aba = run_scenario(false);
    bool tagged_aba = run_scenario(true);

    /* Expected: ABA with the plain stack, no ABA with the tagged one. */
    if (plain_aba && !tagged_aba) return 0;
    return 1;
}
