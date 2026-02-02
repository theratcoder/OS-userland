/* ratos-init.c - Minimal init + service supervisor for RatOS
 *
 * Build:
 *   gcc -static -O2 -o init init.c
 *
 * Install to /init
 *
 * Service files are simple key=value text files placed in /etc/ratos/services/*.conf
 * Example:
 *   Name=getty-tty1
 *   ExecStart=/bin/sh -c "/bin/login"    # or /bin/sh -l
 *   Restart=on-failure
 *
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SERVICES_DIR "/etc/ratos/services"
#define LOGDIR "/var/log"
#define MAX_SVC 128
#define MAX_LINE 1024

typedef enum { R_NO=0, R_ON_FAILURE=1, R_ALWAYS=2 } restart_t;

typedef struct service {
    char name[128];
    char *execcmd;       /* malloc'd */
    restart_t restart;
    pid_t pid;
    int running;
    char logfile[256];
} service;

static service services[MAX_SVC];
static int nservices = 0;
static volatile sig_atomic_t need_reap = 0;
static volatile sig_atomic_t terminate = 0;

static void sigchld_handler(int sig) { (void)sig; need_reap = 1; }
static void sigterm_handler(int sig) { (void)sig; terminate = 1; }

/* utility: trim */
static char *trim(char *s) {
    while(*s==' '||*s=='\t') s++;
    char *end = s + strlen(s);
    while(end>s && (end[-1]=='\n' || end[-1]=='\r' || end[-1]==' ' || end[-1]=='\t')) end--;
    *end = 0;
    return s;
}

/* parse a simple key=value service file */
static void parse_service_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[MAX_LINE];
    char name[128] = "";
    char exec[MAX_LINE] = "";
    char restart[MAX_LINE] = "no";
    while (fgets(line, sizeof(line), f)) {
        char *p = strchr(line, '=');
        if (!p) continue;
        *p = 0;
        char *key = trim(line);
        char *val = trim(p+1);
        if (strcasecmp(key,"name")==0 || strcasecmp(key,"Name")==0) {
            strncpy(name, val, sizeof(name)-1);
        }
        else if (strcasecmp(key,"execstart")==0 || strcasecmp(key,"ExecStart")==0) {
            strncpy(exec, val, sizeof(exec)-1);
        }
        else if (strcasecmp(key,"restart")==0 || strcasecmp(key,"Restart")==0) {
            strncpy(restart, val, sizeof(restart)-1);
        }
    }
    fclose(f);
    if (name[0]==0 || exec[0]==0) return;
    if (nservices >= MAX_SVC) return;
    service *s = &services[nservices++];
    memset(s,0,sizeof(service));
    strncpy(s->name, name, sizeof(s->name)-1);
    s->execcmd = strdup(exec);
    s->restart = R_NO;
    if (strcasecmp(restart,"always")==0) s->restart = R_ALWAYS;
    else if (strcasecmp(restart,"on-failure")==0) s->restart = R_ON_FAILURE;
    snprintf(s->logfile, sizeof(s->logfile), LOGDIR "/%s.log", s->name);
}

/* scan services directory */
static void load_services(void) {
    DIR *d = opendir(SERVICES_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_type == DT_DIR) continue;
        if (e->d_name[0]=='.') continue;
        char path[512];
        snprintf(path, sizeof(path), SERVICES_DIR "/%s", e->d_name);
        parse_service_file(path);
    }
    closedir(d);
}

/* start a service, redirecting stdout+stderr to logfile */
static void start_service(service *s) {
    if (!s || !s->execcmd) return;
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }
    if (pid == 0) {
        /* child */
        /* reopen /dev/null for stdin */
        int fdnull = open("/dev/null", O_RDONLY);
        if (fdnull >= 0) { dup2(fdnull, 0); close(fdnull); }
        /* open logfile */
        int fdlog = open(s->logfile, O_CREAT|O_WRONLY|O_APPEND, 0644);
        if (fdlog >= 0) { dup2(fdlog, 1); dup2(fdlog, 2); if (fdlog>2) close(fdlog); }
        /* set child process group */
        setsid();
        /* exec via /bin/sh -c so execcmd can be composite */
        execl("/bin/sh", "sh", "-c", s->execcmd, (char*)NULL);
        /* if execl fails */
        perror("execl");
        _exit(127);
    } else {
        s->pid = pid;
        s->running = 1;
        printf("[init] started %s pid=%d\n", s->name, pid);
    }
}

/* stop a service (SIGTERM then SIGKILL) */
static void stop_service(service *s) {
    if (!s || !s->running) return;
    kill(-s->pid, SIGTERM); /* send to group */
    /* wait up to a little while */
    int i;
    for (i=0;i<50;i++) {
        if (waitpid(s->pid, NULL, WNOHANG) == s->pid) break;
        usleep(100000);
    }
    if (waitpid(s->pid, NULL, WNOHANG) != s->pid) {
        kill(-s->pid, SIGKILL);
    }
    s->running = 0;
    printf("[init] stopped %s pid=%d\n", s->name, s->pid);
}

/* supervise reaped child */
static void handle_reaped(pid_t pid, int status) {
    for (int i=0;i<nservices;i++) {
        service *s = &services[i];
        if (s->running && s->pid == pid) {
            s->running = 0;
            printf("[init] service %s exited pid=%d status=%d\n", s->name, pid, status);
            int exitcode = -1;
            if (WIFEXITED(status)) exitcode = WEXITSTATUS(status);
            if (s->restart == R_ALWAYS || (s->restart == R_ON_FAILURE && exitcode != 0)) {
                sleep(1); /* backoff */
                start_service(s);
            }
            return;
        }
    }
    /* if not a supervised service, maybe it was the login child - ignore */
}

/* spawn login on tty1 (simple: open /dev/tty1 and dup2) */
static void spawn_getty_or_shell() {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        /* Child: open /dev/tty1 */
        int fd = open("/dev/tty1", O_RDWR);
        if (fd < 0) {
            /* fallback to /dev/console or stdio */
            fd = open("/dev/console", O_RDWR);
            if (fd < 0) {
                /* fallback to stdout/stderr unchanged */
            }
        }
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) close(fd);
        }
        /* try /bin/login, else start shell */
        execl("/bin/login", "login", (char*)NULL);
        execl("/bin/sh", "sh", "-l", (char*)NULL);
        _exit(127);
    }
    else {
        /* parent: optionally monitor this pid or let system supervise it via service file */
        int st;
        waitpid(pid, &st, 0);
        /* if login exited, loop to respawn */
    }
}

/* main */
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    /* basic signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* mount proc/sys if missing */
    mkdir("/proc",0755); mkdir("/sys",0755); mkdir("/dev",0755);
    mount("proc","/proc","proc",0,"");
    mount("sysfs","/sys","sysfs",0,"");
    mount("devtmpfs","/dev","devtmpfs",0,"");

    /* create logdir */
    mkdir(LOGDIR,0755);

    /* load services */
    load_services();

    /* start all services */
    for (int i=0;i<nservices;i++) {
        start_service(&services[i]);
    }

    /* spawn getty in a loop (in background) */
    pid_t getty_pid = fork();
    if (getty_pid == 0) {
        /* child: spawn a persistent getty loop */
        while (1) {
            pid_t p = fork();
            if (p == 0) {
                /* child: attach to tty1 and exec login or shell */
                int fd = open("/dev/tty1", O_RDWR);
                if (fd >= 0) {
                    dup2(fd,0); dup2(fd,1); dup2(fd,2);
                    if (fd>2) close(fd);
                }
                execl("/bin/login","login",(char*)NULL);
                execl("/bin/sh","sh","-l",(char*)NULL);
                _exit(127);
            }
            int st;
            waitpid(p,&st,0);
            sleep(1);
        }
        _exit(0);
    }

    /* main supervise loop */
    while (!terminate) {
        if (need_reap) {
            need_reap = 0;
            /* reap all children */
            int status;
            pid_t pid;
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                handle_reaped(pid, status);
            }
        }
        /* periodic work */
        sleep(1);
    }

    /* termination: stop services */
    printf("[init] shutting down services\n");
    for (int i=0;i<nservices;i++) stop_service(&services[i]);

    /* try to sync and poweroff (if present) */
    sync();
    if (access("/sbin/poweroff", X_OK)==0) {
        execl("/sbin/poweroff", "poweroff", NULL);
    }
    return 0;
}