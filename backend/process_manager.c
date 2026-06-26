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

typedef struct {
    pid_t pid;
    pid_t ppid;
    char name[256];
    char state;
    long priority;
    long nice;
    long vm_rss;
    long utime;
    long stime;
} ProcessInfo;

#define RED "\033[1;31m"
#define GREEN "\033[1;32m"
#define YELLOW "\033[1;33m"
#define CYAN "\033[1;36m"
#define RESET "\033[0m"
#define BOLD "\033[1m"

pid_t create_process(const char *program, char *const argv[]);
int terminate_process(pid_t pid, int signal_num);
int change_priority(pid_t pid, int priority);
int read_process_info(pid_t pid, ProcessInfo *info);
void list_processes(void);
void show_process_tree(pid_t root_pid, int depth);
void demonstrate_lifecycle(void);
void signal_handler(int signum);
void print_banner(void);
void print_system_info(void);

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

pid_t create_process(const char *program, char *const argv[]) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork() failed");
        return -1;
    }

    if (pid == 0) {
        printf(GREEN "[CHILD] PID=%d, PPID=%d — executing '%s'\n" RESET,
               getpid(), getppid(), program);

        execvp(program, argv);

        perror("execvp() failed");
        _exit(EXIT_FAILURE);

    } else {
        printf(CYAN "[PARENT] PID=%d — spawned child PID=%d\n" RESET,
               getpid(), pid);
    }

    return pid;
}

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

    if (kill(pid, signal_num) < 0) {
        perror("kill() failed");
        return -1;
    }

    return 0;
}

int change_priority(pid_t pid, int nice_value) {
    if (nice_value < -20 || nice_value > 19) {
        fprintf(stderr, "Nice value must be between -20 and +19\n");
        return -1;
    }

    if (setpriority(PRIO_PROCESS, pid, nice_value) < 0) {
        perror("setpriority() failed");
        return -1;
    }

    int actual = getpriority(PRIO_PROCESS, pid);
    printf(GREEN "[PRIORITY] PID %d nice value set to %d (actual: %d)\n" RESET,
           pid, nice_value, actual);
    return 0;
}

int read_process_info(pid_t pid, ProcessInfo *info) {
    char path[64], stat_buf[1024], status_buf[4096];
    FILE *fp;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fp = fopen(path, "r");
    if (!fp) return -1;

    fread(stat_buf, 1, sizeof(stat_buf) - 1, fp);
    fclose(fp);

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

void list_processes(void) {
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) { perror("opendir /proc"); return; }

    printf("\n" BOLD "%-8s %-8s %-20s %-6s %-8s %-10s\n" RESET,
           "PID", "PPID", "NAME", "STATE", "NICE", "MEM(kB)");
    printf("────────────────────────────────────────────────────────\n");

    struct dirent *entry;
    ProcessInfo info;
    int count = 0;

    while ((entry = readdir(proc_dir)) != NULL) {
        pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0) continue;

        if (read_process_info(pid, &info) < 0) continue;

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

void show_process_tree(pid_t root_pid, int depth) {
    ProcessInfo info;
    if (read_process_info(root_pid, &info) < 0) return;

    for (int i = 0; i < depth; i++) printf("  ");
    if (depth > 0) printf("└─ ");
    printf(CYAN "%s" RESET " [PID: %d, State: %c, Nice: %ld]\n",
           info.name, info.pid, info.state, info.nice);

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

void demonstrate_lifecycle(void) {
    printf(BOLD "\n═══ Process Lifecycle Demonstration ═══\n\n" RESET);

    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    printf("✓ SIGCHLD handler registered\n");

    printf("\n[1] Creating child process...\n");
    char *argv[] = {"sleep", "30", NULL};
    pid_t child = create_process("sleep", argv);
    if (child < 0) return;

    sleep(1);

    printf("\n[2] Reading child process info from /proc...\n");
    ProcessInfo info;
    if (read_process_info(child, &info) == 0) {
        printf("    Name: %s | PID: %d | PPID: %d | State: %c | Nice: %ld\n",
               info.name, info.pid, info.ppid, info.state, info.nice);
    }

    printf("\n[3] Lowering process priority (nice +10)...\n");
    change_priority(child, 10);

    printf("\n[4] Pausing process with SIGSTOP...\n");
    terminate_process(child, SIGSTOP);
    sleep(1);

    printf("\n[5] Resuming process with SIGCONT...\n");
    terminate_process(child, SIGCONT);
    sleep(1);

    printf("\n[6] Terminating process with SIGTERM...\n");
    terminate_process(child, SIGTERM);

    printf("\n[7] Waiting for child to exit (waitpid)...\n");
    int status;
    pid_t waited = waitpid(child, &status, 0);
    if (waited == child) {
        if (WIFEXITED(status))
            printf(GREEN "✓ Child exited with code %d\n" RESET, WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            printf(GREEN "✓ Child terminated by signal %d\n" RESET, WTERMSIG(status));
    }

    printf(GREEN "\n✓ Lifecycle demo complete. No zombie processes left.\n\n" RESET);
}

void print_system_info(void) {
    printf(BOLD "\n─── System Information ───\n" RESET);
    printf("Current PID:  %d\n", getpid());
    printf("Parent PID:   %d\n", getppid());
    printf("Process GID:  %d\n", getpgid(0));
    printf("Session ID:   %d\n", getsid(0));

    FILE *f = fopen("/proc/loadavg", "r");
    if (f) {
        float la1, la5, la15;
        fscanf(f, "%f %f %f", &la1, &la5, &la15);
        fclose(f);
        printf("Load Avg:     %.2f %.2f %.2f (1m 5m 15m)\n", la1, la5, la15);
    }

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

void print_banner(void) {
    printf(CYAN BOLD);
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║      Linux Process Manager — Core Demo       ║\n");
    printf("║   fork · exec · wait · kill · nice · /proc   ║\n");
    printf("╚══════════════════════════════════════════════╝\n");
    printf(RESET "\n");
}

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