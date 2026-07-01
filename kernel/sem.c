// sem.c — semáforos de Dijkstra (wait/signal), bloqueantes de verdad. Sin
// scheduler no tenía sentido: dormir a alguien exige tener a quién más
// correr mientras espera (ver kernel/sched.c).
#include "kernel.h"

#define MAX_SEMS 4
#define MAX_PROCS 4 // igual que en sched.c: acá solo hace falta el tamaño

typedef struct {
    int count;
    u8 waiting[MAX_PROCS]; // bitmap de pids bloqueados en este semáforo
} Semaphore;

static Semaphore sems[MAX_SEMS];

void sem_init(int idx, int value) {
    sems[idx].count = value;
    for (int i = 0; i < MAX_PROCS; i++) {
        sems[idx].waiting[i] = 0;
    }
}

int sem_wait(TrapFrame *tf, int idx) {
    Semaphore *s = &sems[idx];
    s->count--;
    if (s->count < 0) {
        int pid = sched_current_pid();
        s->waiting[pid] = 1;
        tf->epc += 4; // no reejecutar el syscall cuando lo despierten
        sched_block_current(tf);
        return 1;
    }
    return 0;
}

void sem_signal(int idx) {
    Semaphore *s = &sems[idx];
    s->count++;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (s->waiting[i]) {
            s->waiting[i] = 0;
            sched_wake(i);
            break;
        }
    }
}
