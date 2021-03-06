/* 
 * Copyright (c) 2015 Scott Vokes <vokes.s@gmail.com>
 *  
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *  
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

/* Version 0.1.0. */
#define AUTOCLAVE_VERSION_MAJOR 0
#define AUTOCLAVE_VERSION_MINOR 1
#define AUTOCLAVE_VERSION_PATCH 0
#define AUTOCLAVE_AUTHOR "Scott Vokes <vokes.s@gmail.com>"

#include "types.h"

static const char REASON_UNDEF[] = "undef";
static const char REASON_TIMEOUT[] = "timeout";
static const char REASON_EXIT[] = "exit";
static const char REASON_TERM[] = "term";
static const char REASON_STOP[] = "stop";

static void usage(const char *msg) {
    if (msg) { fprintf(stderr, "%s\n\n", msg); }
    fprintf(stderr, "autoclave v. %d.%d.%d by %s\n",
        AUTOCLAVE_VERSION_MAJOR, AUTOCLAVE_VERSION_MINOR,
        AUTOCLAVE_VERSION_PATCH, AUTOCLAVE_AUTHOR);
    fprintf(stderr,
        "Usage: autoclave [-h] [-c COUNT] [-e] [-l] [-f MAX_FAILURES]\n"
        "                 [-o OUT_PATH] [-r MAX_RUNS] [-t TIMEOUT]\n"
        "                 [-v] [-w WAIT] [-x CMD]\n"
        "\n"
        "    -h:         help\n"
        "    -c COUNT:   rotate log files by count\n"
        "    -e:         log stderr\n"
        "    -f COUNT:   max failures (def. 1)\n"
        "    -l:         log stdout\n"
        "    -o PATH:    log output path\n"
        //"    -p CMD:     command to pipe completed log files through\n"
        "    -r COUNT:   max runs (def. no limit)\n"
        "    -t SEC:     timeout for watched program\n"
        "    -v:         increase verbosity\n"
        "    -w SEC:     wait W seconds between executions (def. 1)\n"
        "    -x CMD:     execute command on error/timeout\n"
        );
    
    exit(1);
}

static void handle_args(config *cfg, int argc, char **argv) {
    int fl = 0;
    while ((fl = getopt(argc, argv, "hc:ef:lo:r:t:vw:x:")) != -1) {
        switch (fl) {
        case 'h':               /* help */
            usage(NULL);
            break;
        case 'c':               /* rotation count */
            cfg->rot.type = ROT_COUNT;
            cfg->rot.u.count.count = atoll(optarg);
            break;
        case 'e':               /* log stderr */
            cfg->log_stderr = true;
            break;
        case 'f':               /* max failures */
            cfg->max_failures = atoll(optarg);
            break;
        case 'l':               /* log stdout */
            cfg->log_stdout = true;
            break;
        case 'o':               /* out path */
            cfg->out_path = optarg;
            break;
#if 0 /* TODO*/
        case 'p':               /* output pipe; NYI */
            cfg->out_pipe = optarg;
            break;
#endif
        case 'r':               /* max runs */
            cfg->max_runs = atoll(optarg);
            break;
        case 't':               /* timeout */
            cfg->timeout = atoi(optarg);
            break;
        case 'v':               /* verbosity */
            cfg->verbosity++;
            break;
        case 'w':               /* wait */
            cfg->wait = atoi(optarg);
            break;
        case 'x':               /* execute error handler */
            cfg->error_handler = optarg;
            break;
        case '?':
        default:
            usage(NULL);
        }
    }

    argc -= (optind - 1);
    argv += (optind - 1);
    cfg->argc = argc - 1;
    cfg->argv = argv + 1;

    if (cfg->argc < 1) { usage(NULL); }
    if (cfg->out_path == NULL) {
        cfg->out_path = cfg->argv[0];
    }
}    

static int log_path(char *buf, size_t buf_size,
        config *cfg, size_t id, char *fdname) {
    if (cfg->rot.type == ROT_COUNT) { id = id % cfg->rot.u.count.count; }

    int res = snprintf(buf, buf_size, "%s.%zd.%s.log",
        cfg->out_path, id, fdname);

    if (buf_size < res) {
        fprintf(stderr, "snprintf: path too long\n");
        exit(1);
    }
    return res;
}

static bool try_exec(config *cfg, size_t id, child_status *status) {
    memset(status, 0, sizeof(*status));
    char outlogbuf[PATH_MAX];
    int outlog = -1;

    if (cfg->log_stdout) {
        log_path(outlogbuf, PATH_MAX, cfg, id, "stdout");
        outlog = open(outlogbuf, O_WRONLY | O_TRUNC | O_CREAT, 0644);
        if (outlog == -1) { err(1, "open"); }
    }

    char errlogbuf[PATH_MAX];
    int errlog = -1;
    if (cfg->log_stderr) {
        log_path(errlogbuf, PATH_MAX, cfg, id, "stderr");
        errlog = open(errlogbuf, O_WRONLY | O_TRUNC | O_CREAT, 0644);
        if (errlog == -1) { err(1, "open"); }
    }

    bool failed = false;
    int kid = fork();
    if (kid == -1) {
        err(1, "fork");
    } else if (kid == 0) {      /* child */
        if (outlog != -1) {
            if (-1 == dup2(outlog, STDOUT_FILENO)) { err(1, "dup2"); }
            /* TODO: pipe through out_pipe */
        }
        if (errlog != -1) {
            if (-1 == dup2(errlog, STDERR_FILENO)) { err(1, "dup2"); }
            /* TODO: pipe through out_pipe */
        }
        
        /* TODO: could write id into argument if ARGV[n] is "%" */
        int res = execv(cfg->argv[0], &cfg->argv[1]);
        if (res == -1) { err(1, "execv"); }
    } else {                    /* parent */
        status->pid = kid;
        status->run_id = id;
        int stat_loc = 0;
        size_t ticks = 0;
        const int sleep_msec = 100;
        const size_t max_ticks = (size_t)cfg->timeout * (1000 / sleep_msec);
        while (ticks < max_ticks) {
            pid_t res = waitpid(kid, &stat_loc, WNOHANG);
            if (res == -1) {
                if (errno == EINTR) {
                    errno = 0;
                } else {
                    err(1, "wait");
                }
            } else if (res == 0) {
                poll(NULL, 0, sleep_msec);
                if (cfg->timeout != NO_TIMEOUT) { ticks++; }
            } else {
                assert(res == kid);
                break;
            }
        }

        status->reason = REASON_UNDEF;
#ifdef WCOREDUMP
        status->dumped_core = WCOREDUMP(stat_loc);
#endif

        if (ticks == max_ticks) {
            status->reason = REASON_TIMEOUT;
            failed = true;
        } else if (WIFEXITED(stat_loc)) {
            status->reason = REASON_EXIT;
            status->exit_status = WEXITSTATUS(stat_loc);
            failed = (status->exit_status != EXIT_SUCCESS);
        } else if (WIFSIGNALED(stat_loc)) {
            status->reason = REASON_TERM;
            status->term_signal = WTERMSIG(stat_loc);
            failed = true;
        } else if (WIFSTOPPED(stat_loc)) {
            status->reason = REASON_STOP;
            status->stop_signal = WSTOPSIG(stat_loc);
            failed = true;
        }

        if (cfg->verbosity > 1) {
            printf(" -- type: %s, core? %d, exit: %d, term: %d, stop: %d\n",
                status->reason, status->dumped_core,
                status->exit_status, status->term_signal,
                status->stop_signal);
        }

        if (failed && cfg->error_handler != NULL) {
            setenv_and_call_handler(cfg, status);
        } else if (status->reason == REASON_TIMEOUT) {
            int res = kill(kid, SIGINT);
            if (res == -1) {
                if (errno == ESRCH) {
                    errno = 0;  /* race: child terminated on its own? */
                } else {
                    err(1, "kill");
                }
            }
        }
    }

    if (outlog != -1) {
        if (-1 == close(outlog)) { err(1, "close"); }
    }
    if (errlog != -1) {
        if (-1 == close(errlog)) { err(1, "close"); }
    }
    return failed;
}

static void setenv_and_call_handler(config *cfg, child_status *status) {
    setenv("AUTOCLAVE_DUMPED_CORE",
        status->dumped_core ? "1" : "0", 1);
    setenv("AUTOCLAVE_FAIL_TYPE", status->reason, 1);

    const int sz = 32;
    char exit_buf[sz];
    if (sz >= snprintf(exit_buf, sz, "%d", (int)status->exit_status)) {
        setenv("AUTOCLAVE_EXIT_STATUS", exit_buf, 1);
    }
    char term_buf[sz];
    if (sz >= snprintf(term_buf, sz, "%d", (int)status->term_signal)) {
        setenv("AUTOCLAVE_TERM_SIGNAL", term_buf, 1);
    }
    char stop_buf[sz];
    if (sz >= snprintf(stop_buf, sz, "%d", (int)status->stop_signal)) {
        setenv("AUTOCLAVE_STOP_SIGNAL", stop_buf, 1);
    }
    char child_pid_buf[sz];
    if (sz >= snprintf(child_pid_buf, sz, "%d", (int)status->pid)) {
        setenv("AUTOCLAVE_CHILD_PID", child_pid_buf, 1);
    }    
    char id_buf[sz];
    if (sz >= snprintf(id_buf, sz, "%d", (int)status->run_id)) {
        setenv("AUTOCLAVE_RUN_ID", id_buf, 1);
    }
    setenv("AUTOCLAVE_CMD", cfg->argv[0], 1);
    
    if (-1 == system(cfg->error_handler)) {
        err(1, "system");
    }
}

static int mainloop(config *cfg) {
    size_t failures = 0;

    for (size_t run_id = 1; run_id <= cfg->max_runs; run_id++) {
        struct timeval tv;
        if (0 != gettimeofday(&tv, NULL)) { err(1, "gettimeofday"); }
        if (cfg->verbosity > 0 && run_id > 0) {
            printf("%08lld.%08lld -- %zd run%s, %zd failure%s\n",
                (long long)tv.tv_sec, (long long)tv.tv_usec,
                run_id, run_id == 1 ? "" : "s",
                failures, failures == 1 ? "" : "s");
        }
        child_status s;
        bool failed = try_exec(cfg, run_id, &s);
        if (failed) { failures++; }
        if (failures >= cfg->max_failures) { break; }
        if (cfg->wait > 0) { sleep(cfg->wait); }
    }
    return (failures > 0 ? 1 : 0);
}

int main(int argc, char **argv) {
    config cfg = {
        .max_failures = 1,
        .max_runs = (size_t)-1,
        .wait = 1,
        .timeout = NO_TIMEOUT,
    };
    handle_args(&cfg, argc, argv);

    return mainloop(&cfg);
}
