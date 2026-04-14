#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define CHILD_COMMAND_LEN 256
#define CONTROL_MESSAGE_LEN 1024
#define DEFAULT_SOFT_LIMIT (128UL << 10)  // 128 KB (Very low)
#define DEFAULT_HARD_LIMIT (10UL << 20)   // 10 MB (Plenty of room)
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_PS
} command_kind_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int log_fd;
} child_config_t;

typedef struct {
    char id[32];
    pid_t pid;
} container_info_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    container_info_t containers[100];
    int container_count;
} supervisor_ctx_t;

// --- Helper Functions ---

int register_with_monitor(int monitor_fd, const char *id, pid_t pid, unsigned long soft, unsigned long hard) {
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;
    strncpy(req.container_id, id, sizeof(req.container_id) - 1);
    return ioctl(monitor_fd, MONITOR_REGISTER, &req);
}

// --- Child Logic (Namespace Isolation) ---

int child_fn(void *arg) {
    child_config_t *config = (child_config_t *)arg;
    
    // Redirect output to log file
    if (config->log_fd >= 0) {
        dup2(config->log_fd, STDOUT_FILENO);
        dup2(config->log_fd, STDERR_FILENO);
        close(config->log_fd);
    }

    sethostname(config->id, strlen(config->id));

    if (chroot(config->rootfs) != 0 || chdir("/") != 0) {
        perror("chroot failed");
        return 1;
    }

    mount("proc", "/proc", "proc", 0, NULL);

    char *argv[] = { "/bin/sh", "-c", config->command, NULL };
    execv("/bin/sh", argv);
    return 0;
}

// --- Supervisor Logic ---

static int run_supervisor() {
    supervisor_ctx_t ctx = {0};
    struct sockaddr_un addr;

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) { perror("Device open failed"); }

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("socket bind failed");
        return 1;
    }
      
    listen(ctx.server_fd, 5);
    printf("Supervisor live on %s\n", CONTROL_PATH);

    while (1) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        control_request_t req;
        recv(client_fd, &req, sizeof(req), 0);

        if (req.kind == CMD_START) {
            child_config_t *config = malloc(sizeof(child_config_t));
            memset(config, 0, sizeof(child_config_t));
            strncpy(config->id, req.container_id, CONTAINER_ID_LEN - 1);
            strncpy(config->rootfs, req.rootfs, PATH_MAX - 1);
            strncpy(config->command, req.command, CHILD_COMMAND_LEN - 1);

            mkdir("logs", 0755);
            char log_path[512];
            snprintf(log_path, sizeof(log_path), "logs/%s.log", req.container_id);
            config->log_fd = open(log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);

            char *stack = malloc(STACK_SIZE);
            pid_t pid = clone(child_fn, stack + STACK_SIZE, CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, config);
            
            if (pid > 0) {
                if (ctx.monitor_fd >= 0) {
                    register_with_monitor(ctx.monitor_fd, req.container_id, pid, req.soft_limit_bytes, req.hard_limit_bytes);
                }
                
                // Store metadata correctly
                strncpy(ctx.containers[ctx.container_count].id, req.container_id, 31);
                ctx.containers[ctx.container_count].pid = pid;
                ctx.container_count++;

                control_response_t res = {0, "Container Started Successfully"};
                send(client_fd, &res, sizeof(res), 0);
                printf("Started %s (PID: %d). Total tracked: %d\n", req.container_id, pid, ctx.container_count);
            } else {
                control_response_t res = {-1, "Clone Failed"};
                send(client_fd, &res, sizeof(res), 0);
            }
        } 
        else if (req.kind == CMD_PS) {
            control_response_t res = {0};
            char table[CONTROL_MESSAGE_LEN] = "ID\t\tPID\n------------------\n";
            for (int i = 0; i < ctx.container_count; i++) {
                char line[64];
                snprintf(line, sizeof(line), "%s\t\t%d\n", ctx.containers[i].id, ctx.containers[i].pid);
                strncat(table, line, sizeof(table) - strlen(table) - 1);
            }
            strncpy(res.message, table, sizeof(res.message) - 1);
            send(client_fd, &res, sizeof(res), 0);
        }
        close(client_fd);
    }
    return 0;
}

// --- Client Logic ---

static int send_control_request(control_request_t *req) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Connect to supervisor failed. Is it running?");
        return -1;
    }

    send(fd, req, sizeof(control_request_t), 0);
    control_response_t res;
    recv(fd, &res, sizeof(res), 0);
    
    // Print the message (this shows the PS table!)
    printf("%s\n", res.message);
    
    close(fd);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s [supervisor|start|ps]\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        return run_supervisor();
    }

    control_request_t req = {0};
    if (strcmp(argv[1], "start") == 0 && argc >= 5) {
        req.kind = CMD_START;
        strncpy(req.container_id, argv[2], 31);
        strncpy(req.rootfs, argv[3], PATH_MAX - 1);
        strncpy(req.command, argv[4], CHILD_COMMAND_LEN - 1);
        req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
        req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
        return send_control_request(&req);
    } 
    else if (strcmp(argv[1], "ps") == 0) {
        req.kind = CMD_PS;
        return send_control_request(&req);
    }

    return 0;
}
