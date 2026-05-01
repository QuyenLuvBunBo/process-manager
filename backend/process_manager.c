/**
 * process_manager.c
 * Linux Process Manager - Core Demonstration
 *
 * Demonstrates: fork(), exec(), wait(), waitpid(), kill(),
 *               getpid(), getppid(), nice(), setpriority(),
 *               /proc filesystem access, signal handling
 *
 * Compile: gcc -o process_manager process_manager.c
 * Usage:   ./process_manager [command]
 *   Commands: create, list, kill <pid>, priority <pid> <value>, tree, demo
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>

/* ─── Structures ──────────────────────────────────────────── */

typedef struct {
    pid_t   pid;
    pid_t   ppid;
    char    name[256];
    char    state;
    long    priority;
    long    nice;
    long    vm_rss;     /* Resident Set Size in kB */
    long    utime;      /* User CPU time */
    long    stime;      /* System CPU time */
} ProcessInfo;

/* ─── Color codes ─────────────────────────────────────────── */
#define RED     "\033[1;31m"
#define GREEN   "\033[1;32m"
#define YELLOW  "\033[1;33m"
#define CYAN    "\033[1;36m"
#define RESET   "\033[0m"
#define BOLD    "\033[1m"

/* ─── Function prototypes ─────────────────────────────────── */
pid_t   create_process(const char *program, char *const argv[]);
int     terminate_process(pid_t pid, int signal_num);
int     change_priority(pid_t pid, int priority);
int     read_process_info(pid_t pid, ProcessInfo *info);
void    list_processes(void);
void    show_process_tree(pid_t root_pid, int depth);
void    demonstrate_lifecycle(void);
void    signal_handler(int signum);
void    print_banner(void);
void    print_system_info(void);

/* ─── Signal handler ──────────────────────────────────────── */
volatile sig_atomic_t child_exited = 0;

void signal_handler(int signum) {
    if (signum == SIGCHLD) {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            child_exited = 1;
            if (WIFEXITED(status))
                printf(GREEN "[SIGCHLD] Child PID %d exited with code %d\n" RESET,
                       pid, WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                printf(YELLOW "[SIGCHLD] Child PID %d killed by signal %d\n" RESET,
                       pid, WTERMSIG(status));
        }
    }
}

/* ─── Process Creation ────────────────────────────────────── */
/**
 * create_process() - Forks a new child process and execs a program.
 * Uses fork() to duplicate current process, then exec() in the child
 * to replace the image with the target program.
 *
 * Returns child PID on success, -1 on failure.
 */
pid_t create_process(const char *program, char *const argv[]) {
    pid_t pid = fork();  /* ← Linux API: duplicate process */

    if (pid < 0) {
        perror("fork() failed");
        return -1;
    }

    if (pid == 0) {
        /* ── CHILD PROCESS ── */
        printf(GREEN "[CHILD] PID=%d, PPID=%d — executing '%s'\n" RESET,
               getpid(), getppid(), program);

        /* Replace child image with target program */
        execvp(program, argv);  /* ← Linux API: exec family */

        /* exec() only returns on error */
        perror("execvp() failed");
        _exit(EXIT_FAILURE);

    } else {
        /* ── PARENT PROCESS ── */
        printf(CYAN "[PARENT] PID=%d — spawned child PID=%d\n" RESET,
               getpid(), pid);
    }

    return pid;
}

/* ─── Process Termination ─────────────────────────────────── */
/**
 * terminate_process() - Sends a signal to a process.
 * SIGTERM = graceful shutdown request
 * SIGKILL  = forced immediate termination (cannot be caught)
 * SIGSTOP  = pause process execution
 * SIGCONT  = resume paused process
 */
int terminate_process(pid_t pid, int signal_num) {
    const char *sig_name;
    switch (signal_num) {
        case SIGTERM: sig_name = "SIGTERM (graceful)"; break;
        case SIGKILL: sig_name = "SIGKILL (forced)";   break;
        case SIGSTOP: sig_name = "SIGSTOP (pause)";    break;
        case SIGCONT: sig_name = "SIGCONT (resume)";   break;
        default:      sig_name = "signal";              break;
    }

    printf(YELLOW "[KILL] Sending %s to PID %d...\n" RESET, sig_name, pid);

    if (kill(pid, signal_num) < 0) {  /* ← Linux API: send signal */
        perror("kill() failed");
        return -1;
    }

    return 0;
}

/* ─── Priority / Scheduling ───────────────────────────────── */
/**
 * change_priority() - Adjusts process scheduling priority using nice value.
 * Nice range: -20 (highest priority) to +19 (lowest priority).
 * Only root can set negative nice values.
 */
int change_priority(pid_t pid, int nice_value) {
    if (nice_value < -20 || nice_value > 19) {
        fprintf(stderr, "Nice value must be between -20 and +19\n");
        return -1;
    }

    /* setpriority(PRIO_PROCESS, pid, niceness) */
    if (setpriority(PRIO_PROCESS, pid, nice_value) < 0) {  /* ← Linux API */
        perror("setpriority() failed");
        return -1;
    }

    int actual = getpriority(PRIO_PROCESS, pid);  /* ← Linux API: read back */
    printf(GREEN "[PRIORITY] PID %d nice value set to %d (actual: %d)\n" RESET,
           pid, nice_value, actual);
    return 0;
}

/* ─── /proc Filesystem Reader ─────────────────────────────── */
/**
 * read_process_info() - Reads process details from /proc/<pid>/stat
 * The /proc filesystem is a virtual filesystem in Linux that exposes
 * kernel data structures as files. No disk I/O occurs.
 */
int read_process_info(pid_t pid, ProcessInfo *info) {
    char path[64], stat_buf[1024], status_buf[4096];
    FILE *fp;

    /* Read /proc/<pid>/stat */
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fp = fopen(path, "r");
    if (!fp) return -1;

    fread(stat_buf, 1, sizeof(stat_buf) - 1, fp);
    fclose(fp);

    /* Parse: pid (name) state ppid ... */
    char name_buf[256];
    long utime, stime, priority, nice_val;
    pid_t ppid;
    char state;

    sscanf(stat_buf, "%d (%255[^)]) %c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
           "%ld %ld %*d %*d %ld %ld",
           &info->pid, name_buf, &state, &ppid,
           &utime, &stime, &priority, &nice_val);

    strncpy(info->name, name_buf, 255);
    info->state    = state;
    info->ppid     = ppid;
    info->utime    = utime;
    info->stime    = stime;
    info->priority = priority;
    info->nice     = nice_val;

    /* Read /proc/<pid>/status for memory */
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    fp = fopen(path, "r");
    if (fp) {
        while (fgets(status_buf, sizeof(status_buf), fp)) {
            if (strncmp(status_buf, "VmRSS:", 6) == 0) {
                sscanf(status_buf, "VmRSS: %ld", &info->vm_rss);
                break;
            }
        }
        fclose(fp);
    }

    return 0;
}

/* ─── List Processes ──────────────────────────────────────── */
void list_processes(void) {
    DIR *proc_dir = opendir("/proc");  /* ← /proc filesystem */
    if (!proc_dir) { perror("opendir /proc"); return; }

    printf("\n" BOLD "%-8s %-8s %-20s %-6s %-8s %-10s\n" RESET,
           "PID", "PPID", "NAME", "STATE", "NICE", "MEM(kB)");
    printf("────────────────────────────────────────────────────────\n");

    struct dirent *entry;
    ProcessInfo info;
    int count = 0;

    while ((entry = readdir(proc_dir)) != NULL) {
        /* /proc entries that are pure numbers are PID directories */
        pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0) continue;

        if (read_process_info(pid, &info) < 0) continue;

        /* Color by state */
        const char *color;
        char state_name[16];
        switch (info.state) {
            case 'R': color = GREEN;  strcpy(state_name, "Running");  break;
            case 'S': color = CYAN;   strcpy(state_name, "Sleeping"); break;
            case 'D': color = YELLOW; strcpy(state_name, "Waiting");  break;
            case 'Z': color = RED;    strcpy(state_name, "Zombie");   break;
            case 'T': color = YELLOW; strcpy(state_name, "Stopped");  break;
            default:  color = RESET;  snprintf(state_name, 16, "%c", info.state);
        }

        printf("%s%-8d %-8d %-20s %-6s %-8ld %-10ld\n" RESET,
               color, info.pid, info.ppid, info.name,
               state_name, info.nice, info.vm_rss);
        count++;
    }

    closedir(proc_dir);
    printf("────────────────────────────────────────────────────────\n");
    printf("Total: %d processes\n\n", count);
}

/* ─── Process Tree ────────────────────────────────────────── */
void show_process_tree(pid_t root_pid, int depth) {
    ProcessInfo info;
    if (read_process_info(root_pid, &info) < 0) return;

    for (int i = 0; i < depth; i++) printf("  ");
    if (depth > 0) printf("└─ ");
    printf(CYAN "%s" RESET " [PID: %d, State: %c, Nice: %ld]\n",
           info.name, info.pid, info.state, info.nice);

    /* Find children by scanning /proc */
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) return;

    struct dirent *entry;
    while ((entry = readdir(proc_dir)) != NULL) {
        pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0 || pid == root_pid) continue;

        ProcessInfo child;
        if (read_process_info(pid, &child) < 0) continue;
        if (child.ppid == root_pid)
            show_process_tree(pid, depth + 1);
    }
    closedir(proc_dir);
}

/* ─── Full Lifecycle Demo ─────────────────────────────────── */
/**
 * demonstrate_lifecycle() - Shows full process lifecycle:
 * 1. Register SIGCHLD handler
 * 2. Fork child process
 * 3. Adjust priority via nice()
 * 4. Send SIGSTOP to pause
 * 5. Send SIGCONT to resume
 * 6. Send SIGTERM to terminate
 * 7. waitpid() to reap zombie
 */
void demonstrate_lifecycle(void) {
    printf(BOLD "\n═══ Process Lifecycle Demonstration ═══\n\n" RESET);

    /* Step 1: Register signal handler for SIGCHLD */
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);  /* ← Linux API: signal handling */
    printf("✓ SIGCHLD handler registered\n");

    /* Step 2: Create a long-running child */
    printf("\n[1] Creating child process...\n");
    char *argv[] = {"sleep", "30", NULL};
    pid_t child = create_process("sleep", argv);
    if (child < 0) return;

    sleep(1);

    /* Step 3: Read and display child info */
    printf("\n[2] Reading child process info from /proc...\n");
    ProcessInfo info;
    if (read_process_info(child, &info) == 0) {
        printf("    Name: %s | PID: %d | PPID: %d | State: %c | Nice: %ld\n",
               info.name, info.pid, info.ppid, info.state, info.nice);
    }

    /* Step 4: Adjust priority */
    printf("\n[3] Lowering process priority (nice +10)...\n");
    change_priority(child, 10);

    /* Step 5: Pause the process */
    printf("\n[4] Pausing process with SIGSTOP...\n");
    terminate_process(child, SIGSTOP);
    sleep(1);

    /* Step 6: Resume the process */
    printf("\n[5] Resuming process with SIGCONT...\n");
    terminate_process(child, SIGCONT);
    sleep(1);

    /* Step 7: Terminate gracefully */
    printf("\n[6] Terminating process with SIGTERM...\n");
    terminate_process(child, SIGTERM);

    /* Step 8: Wait to reap zombie */
    printf("\n[7] Waiting for child to exit (waitpid)...\n");
    int status;
    pid_t waited = waitpid(child, &status, 0);  /* ← Linux API: reap zombie */
    if (waited == child) {
        if (WIFEXITED(status))
            printf(GREEN "✓ Child exited with code %d\n" RESET, WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            printf(GREEN "✓ Child terminated by signal %d\n" RESET, WTERMSIG(status));
    }

    printf(GREEN "\n✓ Lifecycle demo complete. No zombie processes left.\n\n" RESET);
}

/* ─── System Info ─────────────────────────────────────────── */
void print_system_info(void) {
    printf(BOLD "\n─── System Information ───\n" RESET);
    printf("Current PID:  %d\n", getpid());
    printf("Parent PID:   %d\n", getppid());
    printf("Process GID:  %d\n", getpgid(0));
    printf("Session ID:   %d\n", getsid(0));

    /* Read /proc/loadavg */
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) {
        float la1, la5, la15;
        fscanf(f, "%f %f %f", &la1, &la5, &la15);
        fclose(f);
        printf("Load Avg:     %.2f %.2f %.2f (1m 5m 15m)\n", la1, la5, la15);
    }

    /* Read /proc/meminfo */
    f = fopen("/proc/meminfo", "r");
    if (f) {
        long total = 0, free_mem = 0, available = 0;
        char line[128], key[64];
        long val;
        while (fgets(line, sizeof(line), f)) {
            sscanf(line, "%s %ld", key, &val);
            if (strcmp(key, "MemTotal:") == 0)     total     = val;
            if (strcmp(key, "MemFree:") == 0)      free_mem  = val;
            if (strcmp(key, "MemAvailable:") == 0) available = val;
        }
        fclose(f);
        printf("Memory:       Total=%ld MB | Free=%ld MB | Available=%ld MB\n",
               total/1024, free_mem/1024, available/1024);
    }
    printf("\n");
}

/* ─── Banner ──────────────────────────────────────────────── */
void print_banner(void) {
    printf(CYAN BOLD);
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║      Linux Process Manager — Core Demo       ║\n");
    printf("║   fork · exec · wait · kill · nice · /proc   ║\n");
    printf("╚══════════════════════════════════════════════╝\n");
    printf(RESET "\n");
}

/* ─── Main ────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    print_banner();

    if (argc < 2) {
        printf("Usage: %s <command> [args]\n\n", argv[0]);
        printf("Commands:\n");
        printf("  demo              — Full lifecycle demonstration\n");
        printf("  list              — List all running processes\n");
        printf("  tree <pid>        — Show process tree from PID\n");
        printf("  create <program>  — Fork + exec a program\n");
        printf("  kill <pid>        — Send SIGTERM to process\n");
        printf("  priority <pid> <n>— Change nice value (-20 to 19)\n");
        printf("  info              — System information\n\n");
        return 0;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "demo") == 0) {
        demonstrate_lifecycle();

    } else if (strcmp(cmd, "list") == 0) {
        list_processes();

    } else if (strcmp(cmd, "info") == 0) {
        print_system_info();

    } else if (strcmp(cmd, "tree") == 0) {
        pid_t root = (argc >= 3) ? atoi(argv[2]) : 1;
        printf("\nProcess tree from PID %d:\n\n", root);
        show_process_tree(root, 0);
        printf("\n");

    } else if (strcmp(cmd, "create") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: create <program> [args...]\n"); return 1; }
        pid_t child = create_process(argv[2], &argv[2]);
        if (child > 0) {
            printf("Waiting for child PID %d...\n", child);
            int status;
            waitpid(child, &status, 0);
            printf("Child exited.\n");
        }

    } else if (strcmp(cmd, "kill") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: kill <pid>\n"); return 1; }
        terminate_process(atoi(argv[2]), SIGTERM);

    } else if (strcmp(cmd, "priority") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: priority <pid> <nice>\n"); return 1; }
        change_priority(atoi(argv[2]), atoi(argv[3]));

    } else {
        fprintf(stderr, RED "Unknown command: %s\n" RESET, cmd);
        return 1;
    }

    return 0;
}