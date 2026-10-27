/*
 * scheduler.c — ESQUELETO DEL LABORATORIO
 *
 * Este archivo contiene el núcleo del scheduler round-robin de miniOS.
 * Las funciones de infraestructura (init, getters, install_sigchld, stop,
 * timespec_diff_ms) ya están implementadas.
 *
 * Tu trabajo es implementar las CUATRO funciones marcadas con [TODO]:
 *   1. scheduler_create_process  — fork + exec + SIGSTOP + PCB init
 *   2. scheduler_start           — arrancar el primer proceso y el timer
 *   3. scheduler_tick            — handler de SIGALRM (context switch)
 *   4. scheduler_sigchld         — handler de SIGCHLD (terminación)
 *
 * Cada función viene con comentarios numerados que describen el flujo
 * paso a paso. Tu trabajo es traducir cada paso a código C usando las
 * APIs de POSIX y las funciones de infraestructura disponibles.
 *
 * APIs disponibles:
 *   - POSIX:       fork, execl, waitpid, kill, clock_gettime
 *   - platform_*:  ver src/platform/platform.h
 *   - pcb_*:       ver src/pcb.h
 *   - rq_*:        ver src/ready_queue.h
 *   - timer_*:     ver src/timer.h
 *   - monitor_*:   ver src/monitor.h
 *
 * REGLAS DE SEGURIDAD EN SEÑALES (importantes para scheduler_tick y
 * scheduler_sigchld):
 *   - NO uses printf/fprintf dentro de los handlers (no son
 *     async-signal-safe). Solo kill, waitpid, clock_gettime, write.
 *   - El shell bloquea SIGALRM con sigprocmask durante sus operaciones
 *     críticas, por lo que no necesitas mutex manual sobre process_table.
 */

#include "scheduler.h"
#include "timer.h"
#include "monitor.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <libgen.h>
#include <errno.h>

// Estado global del scheduler
static volatile int current_running = -1;   // índice en process_table del proceso RUNNING, -1 si ninguno
static volatile int scheduler_active = 0;   // 1 si el scheduler está corriendo

// ============================================================
// Helpers ya implementados — NO los modifiques
// ============================================================

double timespec_diff_ms(struct timespec end, struct timespec start) {
    double sec = (double)(end.tv_sec - start.tv_sec);
    double nsec = (double)(end.tv_nsec - start.tv_nsec);
    return sec * 1000.0 + nsec / 1000000.0;
}

void scheduler_init(void) {
    process_count = 0;
    current_running = -1;
    scheduler_active = 0;
    rq_init();
}

int scheduler_get_running(void) {
    return current_running;
}

int scheduler_is_running(void) {
    return scheduler_active;
}

void scheduler_install_sigchld(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = scheduler_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
}

void scheduler_stop(void) {
    timer_stop();
    scheduler_active = 0;

    for (int i = 0; i < process_count; i++) {
        if (process_table[i].state != PROC_TERMINATED) {
            kill(process_table[i].pid, SIGKILL);
            int status;
            waitpid(process_table[i].pid, &status, 0);
            process_table[i].state = PROC_TERMINATED;
        }
    }
    current_running = -1;
}


// ============================================================
// [TODO 1/4] scheduler_create_process
// ------------------------------------------------------------
// Crea un proceso nuevo a partir de un binario, lo deja detenido
// con estado PROC_READY y lo encola en la ready queue.
//
// Retorna el índice del nuevo PCB en process_table, o -1 en error.
//
// Observable correcto: `ps aux | grep <binario>` debe mostrar el
// proceso en estado T (stopped) justo después de crearlo.
// ============================================================
int scheduler_create_process(const char *path, const char *arg) {
    int status;

    // Paso 1. Validar espacio en process_table
    if (process_count >= MAX_PROCESSES) {
        fprintf(stderr, "Error: process_table llena (max %d procesos)\n", MAX_PROCESSES);
        return -1;
    }

    // Paso 2. fork
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }

    // Paso 3. Hijo: ptrace (si aplica) + exec
    if (pid == 0) {
        if (platform_uses_ptrace()) {
            platform_trace_child();
        }
        if (arg != NULL) {
            execl(path, path, arg, (char *)NULL);
        } else {
            execl(path, path, (char *)NULL);
        }
        perror("execl");
        _exit(1);
    }

    // Paso 4. Padre: esperar el SIGTRAP post-exec (solo con ptrace)
    if (platform_uses_ptrace()) {
        if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
            fprintf(stderr, "Error: el hijo no se detuvo tras exec\n");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
    }

    // Paso 5. Inicializar el PCB
    int idx = process_count;
    char *path_copy = strdup(path);
    if (!path_copy) {
        perror("strdup");
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return -1;
    }
    char *short_name = basename(path_copy);
    pcb_init(&process_table[idx], pid, short_name);
    free(path_copy);

    // Paso 6. Registros iniciales y detach (solo con ptrace)
    if (platform_uses_ptrace()) {
        if (platform_get_registers(pid, &process_table[idx].registers) == 0) {
            process_table[idx].regs_valid = 1;
        }
        platform_detach(pid);
    }

    // Paso 7. Detener el proceso
    if (platform_stop_process(pid) < 0) {
        perror("platform_stop_process");
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return -1;
    }

    // Paso 8. Confirmar que quedo detenido
    if (waitpid(pid, &status, WUNTRACED) < 0) {
        perror("waitpid");
        kill(pid, SIGKILL);
        return -1;
    }

    // Paso 9. Marcar READY, encolar y notificar
    process_table[idx].state = PROC_READY;
    process_count++;
    rq_enqueue(idx);
    monitor_emit_created(pid, process_table[idx].name);
    if (process_table[idx].regs_valid) {
        monitor_emit_registers(pid,
            (unsigned long long)process_table[idx].registers.program_counter,
            (unsigned long long)process_table[idx].registers.stack_pointer);
    }
    return idx;
}


// ============================================================
// [TODO 2/4] scheduler_start
// ------------------------------------------------------------
// Arranca el scheduler: desencola el primer proceso, lo pone en
// RUNNING, le manda SIGCONT e instala el timer que dispara el
// context switch periódicamente.
//
// Observable correcto: tras crear 1 proceso y llamar start, ese
// proceso empieza a producir salida en la terminal.
// ============================================================
void scheduler_start(int slice_ms) {
    // Paso 1. Nada que arrancar si la cola esta vacia
    if (rq_is_empty()) {
        printf("No hay procesos en la ready queue.\n");
        return;
    }

    // Paso 2. Primer proceso de la cola
    int idx = rq_dequeue();

    // Paso 3. Actualizar PCB entrante
    process_table[idx].state = PROC_RUNNING;
    clock_gettime(CLOCK_MONOTONIC, &process_table[idx].last_started);
    current_running = idx;

    // Paso 4. Reanudar el proceso
    platform_resume_process(process_table[idx].pid);

    // Paso 5. Activar scheduler y timer
    scheduler_active = 1;
    timer_init(slice_ms, scheduler_tick);
    timer_start();
}


// ============================================================
// [TODO 3/4] scheduler_tick  — handler de SIGALRM
// ------------------------------------------------------------
// Se invoca CADA vez que expira el time slice. Realiza el
// context switch: detiene al proceso actual, actualiza su PCB,
// lo manda al final de la cola, saca al siguiente y lo reanuda.
//
// ¡IMPORTANTE! Esta función corre en un signal handler.
//   - NO llames printf/fprintf/malloc.
//   - Solo kill, waitpid, clock_gettime, write son seguros.
//   - Las funciones monitor_emit_* internamente usan snprintf + write,
//     que es aceptable para este proyecto educativo.
//
// Observable correcto: el Gantt chart del dashboard muestra
// segmentos alternados entre procesos cada ~slice_ms.
// ============================================================
void scheduler_tick(int signum) {
    (void)signum;

    // Paso 1. Salida temprana
    if (current_running < 0 || !scheduler_active) return;

    // Paso 2. PCB del proceso actual
    pcb_t *current = &process_table[current_running];

    // Paso 3. Detener el proceso actual
    platform_stop_process(current->pid);

    // Paso 4. Actualizar PCB del saliente
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = timespec_diff_ms(now, current->last_started);
    current->cpu_time_ms += elapsed;
    current->state = PROC_READY;
    current->context_switches++;

    // Tiempo de espera de todos los procesos que estan en la cola READY:
    // (tiempo de vida - CPU consumida). Solo aritmetica, async-signal-safe.
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].state == PROC_READY) {
            process_table[i].wait_time_ms =
                timespec_diff_ms(now, process_table[i].created_at) - process_table[i].cpu_time_ms;
            if (process_table[i].wait_time_ms < 0) process_table[i].wait_time_ms = 0;
        }
    }

    // Paso 5. Encolar el saliente
    int out_idx = current_running;
    rq_enqueue(out_idx);

    // Paso 6. Cola vacia: no deberia ocurrir (acabamos de encolar), por seguridad
    if (rq_is_empty()) {
        current_running = -1;
        timer_stop();
        return;
    }

    // Paso 7. Siguiente proceso
    int next_idx = rq_dequeue();
    pcb_t *next = &process_table[next_idx];
    next->state = PROC_RUNNING;
    clock_gettime(CLOCK_MONOTONIC, &next->last_started);
    platform_resume_process(next->pid);
    current_running = next_idx;

    // Solo emitir evento si realmente cambio de proceso
    if (next_idx != out_idx) {
        monitor_emit_switch(current->pid, next->pid, timer_get_slice());
    }
}


// ============================================================
// [TODO 4/4] scheduler_sigchld  — handler de SIGCHLD
// ------------------------------------------------------------
// Se invoca cuando un proceso hijo termina. Debe:
//   - Detectar TODOS los hijos terminados (puede haber varios).
//   - Actualizar su PCB a PROC_TERMINATED.
//   - Si el proceso que terminó era el RUNNING, despachar al siguiente.
//   - Si estaba en la ready queue, removerlo con rq_remove().
//
// Observable correcto: tras `exit` en miniOS, `ps aux | grep defunct`
// no muestra zombies.
// ============================================================
void scheduler_sigchld(int signum) {
    (void)signum;
    int saved_errno = errno;
    int status;
    pid_t pid;

    // Paso 1. Recoger TODOS los hijos pendientes sin bloquear
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        // Paso 2. Ignorar paradas (SIGSTOP): solo nos interesan las terminaciones
        if (!WIFEXITED(status) && !WIFSIGNALED(status)) continue;

        // Paso 3. Buscar el PID en la tabla
        int i;
        for (i = 0; i < process_count; i++) {
            if (process_table[i].pid == pid) break;
        }
        if (i >= process_count) continue;
        if (process_table[i].state == PROC_TERMINATED) continue;

        // Paso 5a. Si era el RUNNING, sumar el ultimo tramo de CPU
        int was_running = (i == current_running);
        if (was_running) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            process_table[i].cpu_time_ms += timespec_diff_ms(now, process_table[i].last_started);
        }

        // Paso 4. Marcar TERMINATED y notificar
        process_table[i].state = PROC_TERMINATED;
        monitor_emit_terminated(pid, process_table[i].cpu_time_ms, process_table[i].context_switches);

        if (was_running) {
            // Paso 5b-d. Despachar al siguiente o apagar el timer
            current_running = -1;
            if (!rq_is_empty()) {
                int next = rq_dequeue();
                process_table[next].state = PROC_RUNNING;
                clock_gettime(CLOCK_MONOTONIC, &process_table[next].last_started);
                platform_resume_process(process_table[next].pid);
                current_running = next;
                monitor_emit_switch(pid, process_table[next].pid, timer_get_slice());
            } else {
                timer_stop();
                scheduler_active = 0;
            }
        } else {
            // Paso 6. Estaba esperando en la cola
            rq_remove(i);
        }
    }
    errno = saved_errno;
}
