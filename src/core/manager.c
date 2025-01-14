/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef HAVE_AUDIT
#include <libaudit.h>
#endif

#include "sd-daemon.h"
#include "sd-messages.h"

#include "alloc-util.h"
#include "audit-fd.h"
#include "boot-timestamps.h"
#include "bus-common-errors.h"
#include "bus-error.h"
#include "bus-kernel.h"
#include "bus-util.h"
#include "clean-ipc.h"
#include "dbus-job.h"
#include "dbus-manager.h"
#include "dbus-unit.h"
#include "dbus.h"
#include "dirent-util.h"
#include "env-util.h"
#include "escape.h"
#include "exec-util.h"
#include "exit-status.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "hashmap.h"
#include "io-util.h"
#include "locale-setup.h"
#include "log.h"
#include "macro.h"
#include "manager.h"
#include "missing.h"
#include "mkdir.h"
#include "parse-util.h"
#include "path-lookup.h"
#include "path-util.h"
#include "process-util.h"
#include "ratelimit.h"
#include "rm-rf.h"
#include "signal-util.h"
#include "special.h"
#include "stat-util.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "terminal-util.h"
#include "time-util.h"
#include "transaction.h"
#include "umask-util.h"
#include "unit-name.h"
#include "user-util.h"
#include "util.h"
#include "virt.h"
#include "watchdog.h"

#define NOTIFY_RCVBUF_SIZE (8*1024*1024)
#define CGROUPS_AGENT_RCVBUF_SIZE (8*1024*1024)

/* Initial delay and the interval for printing status messages about running jobs */
#define JOBS_IN_PROGRESS_WAIT_USEC (5*USEC_PER_SEC)
#define JOBS_IN_PROGRESS_PERIOD_USEC (USEC_PER_SEC / 3)
#define JOBS_IN_PROGRESS_PERIOD_DIVISOR 3

static int manager_dispatch_notify_fd(sd_event_source *source, int fd, uint32_t revents, void *userdata);
static int manager_dispatch_cgroups_agent_fd(sd_event_source *source, int fd, uint32_t revents, void *userdata);
static int manager_dispatch_signal_fd(sd_event_source *source, int fd, uint32_t revents, void *userdata);
static int manager_dispatch_time_change_fd(sd_event_source *source, int fd, uint32_t revents, void *userdata);
static int manager_dispatch_idle_pipe_fd(sd_event_source *source, int fd, uint32_t revents, void *userdata);
static int manager_dispatch_user_lookup_fd(sd_event_source *source, int fd, uint32_t revents, void *userdata);
static int manager_dispatch_jobs_in_progress(sd_event_source *source, usec_t usec, void *userdata);
static int manager_dispatch_run_queue(sd_event_source *source, void *userdata);
static int manager_run_environment_generators(Manager *m);
static int manager_run_generators(Manager *m);

static void manager_watch_jobs_in_progress(Manager *m) {
        usec_t next;
        int r;

        assert(m);

        /* We do not want to show the cylon animation if the user
         * needs to confirm service executions otherwise confirmation
         * messages will be screwed by the cylon animation. */
        if (!manager_is_confirm_spawn_disabled(m))
                return;

        if (m->jobs_in_progress_event_source)
                return;

        next = now(CLOCK_MONOTONIC) + JOBS_IN_PROGRESS_WAIT_USEC;
        r = sd_event_add_time(
                        m->event,
                        &m->jobs_in_progress_event_source,
                        CLOCK_MONOTONIC,
                        next, 0,
                        manager_dispatch_jobs_in_progress, m);
        if (r < 0)
                return;

        (void) sd_event_source_set_description(m->jobs_in_progress_event_source, "manager-jobs-in-progress");
}

#define CYLON_BUFFER_EXTRA (2*(sizeof(ANSI_RED)-1) + sizeof(ANSI_HIGHLIGHT_RED)-1 + 2*(sizeof(ANSI_NORMAL)-1))

static void draw_cylon(char buffer[], size_t buflen, unsigned width, unsigned pos) {
        char *p = buffer;

        assert(buflen >= CYLON_BUFFER_EXTRA + width + 1);
        assert(pos <= width+1); /* 0 or width+1 mean that the center light is behind the corner */

        if (pos > 1) {
                if (pos > 2)
                        p = mempset(p, ' ', pos-2);
                if (log_get_show_color())
                        p = stpcpy(p, ANSI_RED);
                *p++ = '*';
        }

        if (pos > 0 && pos <= width) {
                if (log_get_show_color())
                        p = stpcpy(p, ANSI_HIGHLIGHT_RED);
                *p++ = '*';
        }

        if (log_get_show_color())
                p = stpcpy(p, ANSI_NORMAL);

        if (pos < width) {
                if (log_get_show_color())
                        p = stpcpy(p, ANSI_RED);
                *p++ = '*';
                if (pos < width-1)
                        p = mempset(p, ' ', width-1-pos);
                if (log_get_show_color())
                        strcpy(p, ANSI_NORMAL);
        }
}

void manager_flip_auto_status(Manager *m, bool enable) {
        assert(m);

        if (enable) {
                if (m->show_status == SHOW_STATUS_AUTO)
                        manager_set_show_status(m, SHOW_STATUS_TEMPORARY);
        } else {
                if (m->show_status == SHOW_STATUS_TEMPORARY)
                        manager_set_show_status(m, SHOW_STATUS_AUTO);
        }
}

static void manager_print_jobs_in_progress(Manager *m) {
        _cleanup_free_ char *job_of_n = NULL;
        Iterator i;
        Job *j;
        unsigned counter = 0, print_nr;
        char cylon[6 + CYLON_BUFFER_EXTRA + 1];
        unsigned cylon_pos;
        char time[FORMAT_TIMESPAN_MAX], limit[FORMAT_TIMESPAN_MAX] = "no limit";
        uint64_t x;

        assert(m);
        assert(m->n_running_jobs > 0);

        manager_flip_auto_status(m, true);

        print_nr = (m->jobs_in_progress_iteration / JOBS_IN_PROGRESS_PERIOD_DIVISOR) % m->n_running_jobs;

        HASHMAP_FOREACH(j, m->jobs, i)
                if (j->state == JOB_RUNNING && counter++ == print_nr)
                        break;

        /* m->n_running_jobs must be consistent with the contents of m->jobs,
         * so the above loop must have succeeded in finding j. */
        assert(counter == print_nr + 1);
        assert(j);

        cylon_pos = m->jobs_in_progress_iteration % 14;
        if (cylon_pos >= 8)
                cylon_pos = 14 - cylon_pos;
        draw_cylon(cylon, sizeof(cylon), 6, cylon_pos);

        m->jobs_in_progress_iteration++;

        if (m->n_running_jobs > 1) {
                if (asprintf(&job_of_n, "(%u of %u) ", counter, m->n_running_jobs) < 0)
                        job_of_n = NULL;
        }

        format_timespan(time, sizeof(time), now(CLOCK_MONOTONIC) - j->begin_usec, 1*USEC_PER_SEC);
        if (job_get_timeout(j, &x) > 0)
                format_timespan(limit, sizeof(limit), x - j->begin_usec, 1*USEC_PER_SEC);

        manager_status_printf(m, STATUS_TYPE_EPHEMERAL, cylon,
                              "%sA %s job is running for %s (%s / %s)",
                              strempty(job_of_n),
                              job_type_to_string(j->type),
                              unit_description(j->unit),
                              time, limit);
}

static int have_ask_password(void) {
        _cleanup_closedir_ DIR *dir;
        struct dirent *de;

        dir = opendir("/run/systemd/ask-password");
        if (!dir) {
                if (errno == ENOENT)
                        return false;
                else
                        return -errno;
        }

        FOREACH_DIRENT_ALL(de, dir, return -errno) {
                if (startswith(de->d_name, "ask."))
                        return true;
        }
        return false;
}

static int manager_dispatch_ask_password_fd(sd_event_source *source,
                                            int fd, uint32_t revents, void *userdata) {
        Manager *m = userdata;

        assert(m);

        flush_fd(fd);

        m->have_ask_password = have_ask_password();
        if (m->have_ask_password < 0)
                /* Log error but continue. Negative have_ask_password
                 * is treated as unknown status. */
                log_error_errno(m->have_ask_password, "Failed to list /run/systemd/ask-password: %m");

        return 0;
}

static void manager_close_ask_password(Manager *m) {
        assert(m);

        m->ask_password_event_source = sd_event_source_unref(m->ask_password_event_source);
        m->ask_password_inotify_fd = safe_close(m->ask_password_inotify_fd);
        m->have_ask_password = -EINVAL;
}

static int manager_check_ask_password(Manager *m) {
        int r;

        assert(m);

        if (!m->ask_password_event_source) {
                assert(m->ask_password_inotify_fd < 0);

                mkdir_p_label("/run/systemd/ask-password", 0755);

                m->ask_password_inotify_fd = inotify_init1(IN_NONBLOCK|IN_CLOEXEC);
                if (m->ask_password_inotify_fd < 0)
                        return log_error_errno(errno, "inotify_init1() failed: %m");

                if (inotify_add_watch(m->ask_password_inotify_fd, "/run/systemd/ask-password", IN_CREATE|IN_DELETE|IN_MOVE) < 0) {
                        log_error_errno(errno, "Failed to add watch on /run/systemd/ask-password: %m");
                        manager_close_ask_password(m);
                        return -errno;
                }

                r = sd_event_add_io(m->event, &m->ask_password_event_source,
                                    m->ask_password_inotify_fd, EPOLLIN,
                                    manager_dispatch_ask_password_fd, m);
                if (r < 0) {
                        log_error_errno(errno, "Failed to add event source for /run/systemd/ask-password: %m");
                        manager_close_ask_password(m);
                        return -errno;
                }

                (void) sd_event_source_set_description(m->ask_password_event_source, "manager-ask-password");

                /* Queries might have been added meanwhile... */
                manager_dispatch_ask_password_fd(m->ask_password_event_source,
                                                 m->ask_password_inotify_fd, EPOLLIN, m);
        }

        return m->have_ask_password;
}

static int manager_watch_idle_pipe(Manager *m) {
        int r;

        assert(m);

        if (m->idle_pipe_event_source)
                return 0;

        if (m->idle_pipe[2] < 0)
                return 0;

        r = sd_event_add_io(m->event, &m->idle_pipe_event_source, m->idle_pipe[2], EPOLLIN, manager_dispatch_idle_pipe_fd, m);
        if (r < 0)
                return log_error_errno(r, "Failed to watch idle pipe: %m");

        (void) sd_event_source_set_description(m->idle_pipe_event_source, "manager-idle-pipe");

        return 0;
}

static void manager_close_idle_pipe(Manager *m) {
        assert(m);

        m->idle_pipe_event_source = sd_event_source_unref(m->idle_pipe_event_source);

        safe_close_pair(m->idle_pipe);
        safe_close_pair(m->idle_pipe + 2);
}

static int manager_setup_time_change(Manager *m) {
        int r;

        /* We only care for the cancellation event, hence we set the
         * timeout to the latest possible value. */
        struct itimerspec its = {
                .it_value.tv_sec = TIME_T_MAX,
        };

        assert(m);
        assert_cc(sizeof(time_t) == sizeof(TIME_T_MAX));

        if (m->test_run)
                return 0;

        /* Uses TFD_TIMER_CANCEL_ON_SET to get notifications whenever
         * CLOCK_REALTIME makes a jump relative to CLOCK_MONOTONIC */

        m->time_change_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK|TFD_CLOEXEC);
        if (m->time_change_fd < 0)
                return log_error_errno(errno, "Failed to create timerfd: %m");

        if (timerfd_settime(m->time_change_fd, TFD_TIMER_ABSTIME|TFD_TIMER_CANCEL_ON_SET, &its, NULL) < 0) {
                log_debug_errno(errno, "Failed to set up TFD_TIMER_CANCEL_ON_SET, ignoring: %m");
                m->time_change_fd = safe_close(m->time_change_fd);
                return 0;
        }

        r = sd_event_add_io(m->event, &m->time_change_event_source, m->time_change_fd, EPOLLIN, manager_dispatch_time_change_fd, m);
        if (r < 0)
                return log_error_errno(r, "Failed to create time change event source: %m");

        (void) sd_event_source_set_description(m->time_change_event_source, "manager-time-change");

        log_debug("Set up TFD_TIMER_CANCEL_ON_SET timerfd.");

        return 0;
}

static int enable_special_signals(Manager *m) {
        _cleanup_close_ int fd = -1;

        assert(m);

        if (m->test_run)
                return 0;

        /* Enable that we get SIGINT on control-alt-del. In containers
         * this will fail with EPERM (older) or EINVAL (newer), so
         * ignore that. */
        if (reboot(RB_DISABLE_CAD) < 0 && errno != EPERM && errno != EINVAL)
                log_warning_errno(errno, "Failed to enable ctrl-alt-del handling: %m");

        fd = open_terminal("/dev/tty0", O_RDWR|O_NOCTTY|O_CLOEXEC);
        if (fd < 0) {
                /* Support systems without virtual console */
                if (fd != -ENOENT)
                        log_warning_errno(errno, "Failed to open /dev/tty0: %m");
        } else {
                /* Enable that we get SIGWINCH on kbrequest */
                if (ioctl(fd, KDSIGACCEPT, SIGWINCH) < 0)
                        log_warning_errno(errno, "Failed to enable kbrequest handling: %m");
        }

        return 0;
}

static int manager_setup_signals(Manager *m) {
        struct sigaction sa = {
                .sa_handler = SIG_DFL,
                .sa_flags = SA_NOCLDSTOP|SA_RESTART,
        };
        sigset_t mask;
        int r;

        assert(m);

        assert_se(sigaction(SIGCHLD, &sa, NULL) == 0);

        /* We make liberal use of realtime signals here. On
         * Linux/glibc we have 30 of them (with the exception of Linux
         * on hppa, see below), between SIGRTMIN+0 ... SIGRTMIN+30
         * (aka SIGRTMAX). */

        assert_se(sigemptyset(&mask) == 0);
        sigset_add_many(&mask,
                        SIGCHLD,     /* Child died */
                        SIGTERM,     /* Reexecute daemon */
                        SIGHUP,      /* Reload configuration */
                        SIGUSR1,     /* systemd/upstart: reconnect to D-Bus */
                        SIGUSR2,     /* systemd: dump status */
                        SIGINT,      /* Kernel sends us this on control-alt-del */
                        SIGWINCH,    /* Kernel sends us this on kbrequest (alt-arrowup) */
                        SIGPWR,      /* Some kernel drivers and upsd send us this on power failure */

                        SIGRTMIN+0,  /* systemd: start default.target */
                        SIGRTMIN+1,  /* systemd: isolate rescue.target */
                        SIGRTMIN+2,  /* systemd: isolate emergency.target */
                        SIGRTMIN+3,  /* systemd: start halt.target */
                        SIGRTMIN+4,  /* systemd: start poweroff.target */
                        SIGRTMIN+5,  /* systemd: start reboot.target */
                        SIGRTMIN+6,  /* systemd: start kexec.target */

                        /* ... space for more special targets ... */

                        SIGRTMIN+13, /* systemd: Immediate halt */
                        SIGRTMIN+14, /* systemd: Immediate poweroff */
                        SIGRTMIN+15, /* systemd: Immediate reboot */
                        SIGRTMIN+16, /* systemd: Immediate kexec */

                        /* ... space for more immediate system state changes ... */

                        SIGRTMIN+20, /* systemd: enable status messages */
                        SIGRTMIN+21, /* systemd: disable status messages */
                        SIGRTMIN+22, /* systemd: set log level to LOG_DEBUG */
                        SIGRTMIN+23, /* systemd: set log level to LOG_INFO */
                        SIGRTMIN+24, /* systemd: Immediate exit (--user only) */

                        /* .. one free signal here ... */

#if !defined(__hppa64__) && !defined(__hppa__)
                        /* Apparently Linux on hppa has fewer RT
                         * signals (SIGRTMAX is SIGRTMIN+25 there),
                         * hence let's not try to make use of them
                         * here. Since these commands are accessible
                         * by different means and only really a safety
                         * net, the missing functionality on hppa
                         * shouldn't matter. */

                        SIGRTMIN+26, /* systemd: set log target to journal-or-kmsg */
                        SIGRTMIN+27, /* systemd: set log target to console */
                        SIGRTMIN+28, /* systemd: set log target to kmsg */
                        SIGRTMIN+29, /* systemd: set log target to syslog-or-kmsg (obsolete) */

                        /* ... one free signal here SIGRTMIN+30 ... */
#endif
                        -1);
        assert_se(sigprocmask(SIG_SETMASK, &mask, NULL) == 0);

        m->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK|SFD_CLOEXEC);
        if (m->signal_fd < 0)
                return -errno;

        r = sd_event_add_io(m->event, &m->signal_event_source, m->signal_fd, EPOLLIN, manager_dispatch_signal_fd, m);
        if (r < 0)
                return r;

        (void) sd_event_source_set_description(m->signal_event_source, "manager-signal");

        /* Process signals a bit earlier than the rest of things, but later than notify_fd processing, so that the
         * notify processing can still figure out to which process/service a message belongs, before we reap the
         * process. Also, process this before handling cgroup notifications, so that we always collect child exit
         * status information before detecting that there's no process in a cgroup. */
        r = sd_event_source_set_priority(m->signal_event_source, SD_EVENT_PRIORITY_NORMAL-6);
        if (r < 0)
                return r;

        if (MANAGER_IS_SYSTEM(m))
                return enable_special_signals(m);

        return 0;
}

static void manager_clean_environment(Manager *m) {
        assert(m);

        /* Let's remove some environment variables that we
         * need ourselves to communicate with our clients */
        strv_env_unset_many(
                        m->environment,
                        "NOTIFY_SOCKET",
                        "MAINPID",
                        "MANAGERPID",
                        "LISTEN_PID",
                        "LISTEN_FDS",
                        "LISTEN_FDNAMES",
                        "WATCHDOG_PID",
                        "WATCHDOG_USEC",
                        "INVOCATION_ID",
                        NULL);
}

static int manager_default_environment(Manager *m) {
        assert(m);

        if (MANAGER_IS_SYSTEM(m)) {
                /* The system manager always starts with a clean
                 * environment for its children. It does not import
                 * the kernel's or the parents' exported variables.
                 *
                 * The initial passed environment is untouched to keep
                 * /proc/self/environ valid; it is used for tagging
                 * the init process inside containers. */
                m->environment = strv_new("PATH=" DEFAULT_PATH,
                                          NULL);

                /* Import locale variables LC_*= from configuration */
                locale_setup(&m->environment);
        } else
                /* The user manager passes its own environment
                 * along to its children. */
                m->environment = strv_copy(environ);

        if (!m->environment)
                return -ENOMEM;

        manager_clean_environment(m);
        strv_sort(m->environment);

        return 0;
}

int manager_new(UnitFileScope scope, bool test_run, Manager **_m) {
        Manager *m;
        int r;

        assert(_m);
        assert(IN_SET(scope, UNIT_FILE_SYSTEM, UNIT_FILE_USER));

        m = new0(Manager, 1);
        if (!m)
                return -ENOMEM;

        m->unit_file_scope = scope;
        m->exit_code = _MANAGER_EXIT_CODE_INVALID;
        m->default_timer_accuracy_usec = USEC_PER_MINUTE;
        m->default_tasks_accounting = true;
        m->default_tasks_max = UINT64_MAX;

#ifdef ENABLE_EFI
        if (MANAGER_IS_SYSTEM(m) && detect_container() <= 0)
                boot_timestamps(&m->userspace_timestamp, &m->firmware_timestamp, &m->loader_timestamp);
#endif

        /* Prepare log fields we can use for structured logging */
        if (MANAGER_IS_SYSTEM(m)) {
                m->unit_log_field = "UNIT=";
                m->unit_log_format_string = "UNIT=%s";

                m->invocation_log_field = "INVOCATION_ID=";
                m->invocation_log_format_string = "INVOCATION_ID=" SD_ID128_FORMAT_STR;
        } else {
                m->unit_log_field = "USER_UNIT=";
                m->unit_log_format_string = "USER_UNIT=%s";

                m->invocation_log_field = "USER_INVOCATION_ID=";
                m->invocation_log_format_string = "USER_INVOCATION_ID=" SD_ID128_FORMAT_STR;
        }

        m->idle_pipe[0] = m->idle_pipe[1] = m->idle_pipe[2] = m->idle_pipe[3] = -1;

        m->pin_cgroupfs_fd = m->notify_fd = m->cgroups_agent_fd = m->signal_fd = m->time_change_fd =
                m->dev_autofs_fd = m->private_listen_fd = m->utab_inotify_fd = m->cgroup_inotify_fd =
                m->ask_password_inotify_fd = -1;

        m->user_lookup_fds[0] = m->user_lookup_fds[1] = -1;

        m->current_job_id = 1; /* start as id #1, so that we can leave #0 around as "null-like" value */

        m->have_ask_password = -EINVAL; /* we don't know */
        m->first_boot = -1;

        m->test_run = test_run;

        /* Reboot immediately if the user hits C-A-D more often than 7x per 2s */
        RATELIMIT_INIT(m->ctrl_alt_del_ratelimit, 2 * USEC_PER_SEC, 7);

        r = manager_default_environment(m);
        if (r < 0)
                goto fail;

        r = hashmap_ensure_allocated(&m->units, &string_hash_ops);
        if (r < 0)
                goto fail;

        r = hashmap_ensure_allocated(&m->jobs, NULL);
        if (r < 0)
                goto fail;

        r = hashmap_ensure_allocated(&m->cgroup_unit, &string_hash_ops);
        if (r < 0)
                goto fail;

        r = hashmap_ensure_allocated(&m->watch_bus, &string_hash_ops);
        if (r < 0)
                goto fail;

        r = sd_event_default(&m->event);
        if (r < 0)
                goto fail;

        r = sd_event_add_defer(m->event, &m->run_queue_event_source, manager_dispatch_run_queue, m);
        if (r < 0)
                goto fail;

        r = sd_event_source_set_priority(m->run_queue_event_source, SD_EVENT_PRIORITY_IDLE);
        if (r < 0)
                goto fail;

        r = sd_event_source_set_enabled(m->run_queue_event_source, SD_EVENT_OFF);
        if (r < 0)
                goto fail;

        (void) sd_event_source_set_description(m->run_queue_event_source, "manager-run-queue");

        r = manager_setup_signals(m);
        if (r < 0)
                goto fail;

        r = manager_setup_cgroup(m);
        if (r < 0)
                goto fail;

        r = manager_setup_time_change(m);
        if (r < 0)
                goto fail;

        m->udev = udev_new();
        if (!m->udev) {
                r = -ENOMEM;
                goto fail;
        }

        /* Note that we do not set up the notify fd here. We do that after deserialization,
         * since they might have gotten serialized across the reexec. */

        m->taint_usr = dir_is_empty("/usr") > 0;

        *_m = m;
        return 0;

fail:
        manager_free(m);
        return r;
}

static int manager_setup_notify(Manager *m) {
        int r;

        if (m->test_run)
                return 0;

        if (m->notify_fd < 0) {
                _cleanup_close_ int fd = -1;
                union sockaddr_union sa = {
                        .sa.sa_family = AF_UNIX,
                };
                static const int one = 1;
                const char *e;

                /* First free all secondary fields */
                m->notify_socket = mfree(m->notify_socket);
                m->notify_event_source = sd_event_source_unref(m->notify_event_source);

                fd = socket(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
                if (fd < 0)
                        return log_error_errno(errno, "Failed to allocate notification socket: %m");

                fd_inc_rcvbuf(fd, NOTIFY_RCVBUF_SIZE);

                e = manager_get_runtime_prefix(m);
                if (!e) {
                        log_error("Failed to determine runtime prefix.");
                        return -EINVAL;
                }

                m->notify_socket = strappend(e, "/systemd/notify");
                if (!m->notify_socket)
                        return log_oom();

                (void) mkdir_parents_label(m->notify_socket, 0755);
                (void) unlink(m->notify_socket);

                strncpy(sa.un.sun_path, m->notify_socket, sizeof(sa.un.sun_path)-1);
                r = bind(fd, &sa.sa, SOCKADDR_UN_LEN(sa.un));
                if (r < 0)
                        return log_error_errno(errno, "bind(%s) failed: %m", sa.un.sun_path);

                r = setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one));
                if (r < 0)
                        return log_error_errno(errno, "SO_PASSCRED failed: %m");

                m->notify_fd = fd;
                fd = -1;

                log_debug("Using notification socket %s", m->notify_socket);
        }

        if (!m->notify_event_source) {
                r = sd_event_add_io(m->event, &m->notify_event_source, m->notify_fd, EPOLLIN, manager_dispatch_notify_fd, m);
                if (r < 0)
                        return log_error_errno(r, "Failed to allocate notify event source: %m");

                /* Process notification messages a bit earlier than SIGCHLD, so that we can still identify to which
                 * service an exit message belongs. */
                r = sd_event_source_set_priority(m->notify_event_source, SD_EVENT_PRIORITY_NORMAL-7);
                if (r < 0)
                        return log_error_errno(r, "Failed to set priority of notify event source: %m");

                (void) sd_event_source_set_description(m->notify_event_source, "manager-notify");
        }

        return 0;
}

static int manager_setup_cgroups_agent(Manager *m) {

        static const union sockaddr_union sa = {
                .un.sun_family = AF_UNIX,
                .un.sun_path = "/run/systemd/cgroups-agent",
        };
        int r;

        /* This creates a listening socket we receive cgroups agent messages on. We do not use D-Bus for delivering
         * these messages from the cgroups agent binary to PID 1, as the cgroups agent binary is very short-living, and
         * each instance of it needs a new D-Bus connection. Since D-Bus connections are SOCK_STREAM/AF_UNIX, on
         * overloaded systems the backlog of the D-Bus socket becomes relevant, as not more than the configured number
         * of D-Bus connections may be queued until the kernel will start dropping further incoming connections,
         * possibly resulting in lost cgroups agent messages. To avoid this, we'll use a private SOCK_DGRAM/AF_UNIX
         * socket, where no backlog is relevant as communication may take place without an actual connect() cycle, and
         * we thus won't lose messages.
         *
         * Note that PID 1 will forward the agent message to system bus, so that the user systemd instance may listen
         * to it. The system instance hence listens on this special socket, but the user instances listen on the system
         * bus for these messages. */

        if (m->test_run)
                return 0;

        if (!MANAGER_IS_SYSTEM(m))
                return 0;

        r = cg_unified_controller(SYSTEMD_CGROUP_CONTROLLER);
        if (r < 0)
                return log_error_errno(r, "Failed to determine whether unified cgroups hierarchy is used: %m");
        if (r > 0) /* We don't need this anymore on the unified hierarchy */
                return 0;

        if (m->cgroups_agent_fd < 0) {
                _cleanup_close_ int fd = -1;

                /* First free all secondary fields */
                m->cgroups_agent_event_source = sd_event_source_unref(m->cgroups_agent_event_source);

                fd = socket(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
                if (fd < 0)
                        return log_error_errno(errno, "Failed to allocate cgroups agent socket: %m");

                fd_inc_rcvbuf(fd, CGROUPS_AGENT_RCVBUF_SIZE);

                (void) unlink(sa.un.sun_path);

                /* Only allow root to connect to this socket */
                RUN_WITH_UMASK(0077)
                        r = bind(fd, &sa.sa, SOCKADDR_UN_LEN(sa.un));
                if (r < 0)
                        return log_error_errno(errno, "bind(%s) failed: %m", sa.un.sun_path);

                m->cgroups_agent_fd = fd;
                fd = -1;
        }

        if (!m->cgroups_agent_event_source) {
                r = sd_event_add_io(m->event, &m->cgroups_agent_event_source, m->cgroups_agent_fd, EPOLLIN, manager_dispatch_cgroups_agent_fd, m);
                if (r < 0)
                        return log_error_errno(r, "Failed to allocate cgroups agent event source: %m");

                /* Process cgroups notifications early, but after having processed service notification messages or
                 * SIGCHLD signals, so that a cgroup running empty is always just the last safety net of notification,
                 * and we collected the metadata the notification and SIGCHLD stuff offers first. Also see handling of
                 * cgroup inotify for the unified cgroup stuff. */
                r = sd_event_source_set_priority(m->cgroups_agent_event_source, SD_EVENT_PRIORITY_NORMAL-5);
                if (r < 0)
                        return log_error_errno(r, "Failed to set priority of cgroups agent event source: %m");

                (void) sd_event_source_set_description(m->cgroups_agent_event_source, "manager-cgroups-agent");
        }

        return 0;
}

static int manager_setup_user_lookup_fd(Manager *m) {
        int r;

        assert(m);

        /* Set up the socket pair used for passing UID/GID resolution results from forked off processes to PID
         * 1. Background: we can't do name lookups (NSS) from PID 1, since it might involve IPC and thus activation,
         * and we might hence deadlock on ourselves. Hence we do all user/group lookups asynchronously from the forked
         * off processes right before executing the binaries to start. In order to be able to clean up any IPC objects
         * created by a unit (see RemoveIPC=) we need to know in PID 1 the used UID/GID of the executed processes,
         * hence we establish this communication channel so that forked off processes can pass their UID/GID
         * information back to PID 1. The forked off processes send their resolved UID/GID to PID 1 in a simple
         * datagram, along with their unit name, so that we can share one communication socket pair among all units for
         * this purpose.
         *
         * You might wonder why we need a communication channel for this that is independent of the usual notification
         * socket scheme (i.e. $NOTIFY_SOCKET). The primary difference is about trust: data sent via the $NOTIFY_SOCKET
         * channel is only accepted if it originates from the right unit and if reception was enabled for it. The user
         * lookup socket OTOH is only accessible by PID 1 and its children until they exec(), and always available.
         *
         * Note that this function is called under two circumstances: when we first initialize (in which case we
         * allocate both the socket pair and the event source to listen on it), and when we deserialize after a reload
         * (in which case the socket pair already exists but we still need to allocate the event source for it). */

        if (m->user_lookup_fds[0] < 0) {

                /* Free all secondary fields */
                safe_close_pair(m->user_lookup_fds);
                m->user_lookup_event_source = sd_event_source_unref(m->user_lookup_event_source);

                if (socketpair(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC, 0, m->user_lookup_fds) < 0)
                        return log_error_errno(errno, "Failed to allocate user lookup socket: %m");

                (void) fd_inc_rcvbuf(m->user_lookup_fds[0], NOTIFY_RCVBUF_SIZE);
        }

        if (!m->user_lookup_event_source) {
                r = sd_event_add_io(m->event, &m->user_lookup_event_source, m->user_lookup_fds[0], EPOLLIN, manager_dispatch_user_lookup_fd, m);
                if (r < 0)
                        return log_error_errno(errno, "Failed to allocate user lookup event source: %m");

                /* Process even earlier than the notify event source, so that we always know first about valid UID/GID
                 * resolutions */
                r = sd_event_source_set_priority(m->user_lookup_event_source, SD_EVENT_PRIORITY_NORMAL-8);
                if (r < 0)
                        return log_error_errno(errno, "Failed to set priority ot user lookup event source: %m");

                (void) sd_event_source_set_description(m->user_lookup_event_source, "user-lookup");
        }

        return 0;
}

static int manager_connect_bus(Manager *m, bool reexecuting) {
        bool try_bus_connect;
        Unit *u = NULL;

        assert(m);

        if (m->test_run)
                return 0;

        u = manager_get_unit(m, SPECIAL_DBUS_SERVICE);

        try_bus_connect =
                (u && UNIT_IS_ACTIVE_OR_RELOADING(unit_active_state(u))) &&
                (reexecuting ||
                 (MANAGER_IS_USER(m) && getenv("DBUS_SESSION_BUS_ADDRESS")));

        /* Try to connect to the buses, if possible. */
        return bus_init(m, try_bus_connect);
}

static unsigned manager_dispatch_cleanup_queue(Manager *m) {
        Unit *u;
        unsigned n = 0;

        assert(m);

        while ((u = m->cleanup_queue)) {
                assert(u->in_cleanup_queue);

                unit_free(u);
                n++;
        }

        return n;
}

enum {
        GC_OFFSET_IN_PATH,  /* This one is on the path we were traveling */
        GC_OFFSET_UNSURE,   /* No clue */
        GC_OFFSET_GOOD,     /* We still need this unit */
        GC_OFFSET_BAD,      /* We don't need this unit anymore */
        _GC_OFFSET_MAX
};

static void unit_gc_mark_good(Unit *u, unsigned gc_marker) {
        Iterator i;
        Unit *other;

        u->gc_marker = gc_marker + GC_OFFSET_GOOD;

        /* Recursively mark referenced units as GOOD as well */
        SET_FOREACH(other, u->dependencies[UNIT_REFERENCES], i)
                if (other->gc_marker == gc_marker + GC_OFFSET_UNSURE)
                        unit_gc_mark_good(other, gc_marker);
}

static void unit_gc_sweep(Unit *u, unsigned gc_marker) {
        Iterator i;
        Unit *other;
        bool is_bad;

        assert(u);

        if (u->gc_marker == gc_marker + GC_OFFSET_GOOD ||
            u->gc_marker == gc_marker + GC_OFFSET_BAD ||
            u->gc_marker == gc_marker + GC_OFFSET_UNSURE ||
            u->gc_marker == gc_marker + GC_OFFSET_IN_PATH)
                return;

        if (u->in_cleanup_queue)
                goto bad;

        if (unit_check_gc(u))
                goto good;

        u->gc_marker = gc_marker + GC_OFFSET_IN_PATH;

        is_bad = true;

        SET_FOREACH(other, u->dependencies[UNIT_REFERENCED_BY], i) {
                unit_gc_sweep(other, gc_marker);

                if (other->gc_marker == gc_marker + GC_OFFSET_GOOD)
                        goto good;

                if (other->gc_marker != gc_marker + GC_OFFSET_BAD)
                        is_bad = false;
        }

        if (is_bad)
                goto bad;

        /* We were unable to find anything out about this entry, so
         * let's investigate it later */
        u->gc_marker = gc_marker + GC_OFFSET_UNSURE;
        unit_add_to_gc_queue(u);
        return;

bad:
        /* We definitely know that this one is not useful anymore, so
         * let's mark it for deletion */
        u->gc_marker = gc_marker + GC_OFFSET_BAD;
        unit_add_to_cleanup_queue(u);
        return;

good:
        unit_gc_mark_good(u, gc_marker);
}

static unsigned manager_dispatch_gc_unit_queue(Manager *m) {
        unsigned n = 0, gc_marker;
        Unit *u;

        assert(m);

        /* log_debug("Running GC..."); */

        m->gc_marker += _GC_OFFSET_MAX;
        if (m->gc_marker + _GC_OFFSET_MAX <= _GC_OFFSET_MAX)
                m->gc_marker = 1;

        gc_marker = m->gc_marker;

        while ((u = m->gc_unit_queue)) {
                assert(u->in_gc_queue);

                unit_gc_sweep(u, gc_marker);

                LIST_REMOVE(gc_queue, m->gc_unit_queue, u);
                u->in_gc_queue = false;

                n++;

                if (u->gc_marker == gc_marker + GC_OFFSET_BAD ||
                    u->gc_marker == gc_marker + GC_OFFSET_UNSURE) {
                        if (u->id)
                                log_unit_debug(u, "Collecting.");
                        u->gc_marker = gc_marker + GC_OFFSET_BAD;
                        unit_add_to_cleanup_queue(u);
                }
        }

        return n;
}

static unsigned manager_dispatch_gc_job_queue(Manager *m) {
        unsigned n = 0;
        Job *j;

        assert(m);

        while ((j = m->gc_job_queue)) {
                assert(j->in_gc_queue);

                LIST_REMOVE(gc_queue, m->gc_job_queue, j);
                j->in_gc_queue = false;

                n++;

                if (job_check_gc(j))
                        continue;

                log_unit_debug(j->unit, "Collecting job.");
                (void) job_finish_and_invalidate(j, JOB_COLLECTED, false, false);
        }

        return n;
}

static void manager_clear_jobs_and_units(Manager *m) {
        Unit *u;

        assert(m);

        while ((u = hashmap_first(m->units)))
                unit_free(u);

        manager_dispatch_cleanup_queue(m);

        assert(!m->load_queue);
        assert(!m->run_queue);
        assert(!m->dbus_unit_queue);
        assert(!m->dbus_job_queue);
        assert(!m->cleanup_queue);
        assert(!m->gc_unit_queue);
        assert(!m->gc_job_queue);

        assert(hashmap_isempty(m->jobs));
        assert(hashmap_isempty(m->units));

        m->n_on_console = 0;
        m->n_running_jobs = 0;
}

Manager* manager_free(Manager *m) {
        UnitType c;
        int i;

        if (!m)
                return NULL;

        manager_clear_jobs_and_units(m);

        for (c = 0; c < _UNIT_TYPE_MAX; c++)
                if (unit_vtable[c]->shutdown)
                        unit_vtable[c]->shutdown(m);

        /* If we reexecute ourselves, we keep the root cgroup
         * around */
        manager_shutdown_cgroup(m, m->exit_code != MANAGER_REEXECUTE);

        lookup_paths_flush_generator(&m->lookup_paths);

        bus_done(m);

        dynamic_user_vacuum(m, false);
        hashmap_free(m->dynamic_users);

        hashmap_free(m->units);
        hashmap_free(m->units_by_invocation_id);
        hashmap_free(m->jobs);
        hashmap_free(m->watch_pids1);
        hashmap_free(m->watch_pids2);
        hashmap_free(m->watch_bus);

        set_free(m->startup_units);
        set_free(m->failed_units);

        sd_event_source_unref(m->signal_event_source);
        sd_event_source_unref(m->notify_event_source);
        sd_event_source_unref(m->cgroups_agent_event_source);
        sd_event_source_unref(m->time_change_event_source);
        sd_event_source_unref(m->jobs_in_progress_event_source);
        sd_event_source_unref(m->run_queue_event_source);
        sd_event_source_unref(m->user_lookup_event_source);

        safe_close(m->signal_fd);
        safe_close(m->notify_fd);
        safe_close(m->cgroups_agent_fd);
        safe_close(m->time_change_fd);
        safe_close_pair(m->user_lookup_fds);

        manager_close_ask_password(m);

        manager_close_idle_pipe(m);

        udev_unref(m->udev);
        sd_event_unref(m->event);

        free(m->notify_socket);

        lookup_paths_free(&m->lookup_paths);
        strv_free(m->environment);

        hashmap_free(m->cgroup_unit);
        set_free_free(m->unit_path_cache);

        free(m->switch_root);
        free(m->switch_root_init);

        for (i = 0; i < _RLIMIT_MAX; i++)
                m->rlimit[i] = mfree(m->rlimit[i]);

        assert(hashmap_isempty(m->units_requiring_mounts_for));
        hashmap_free(m->units_requiring_mounts_for);

        hashmap_free(m->uid_refs);
        hashmap_free(m->gid_refs);

        return mfree(m);
}

void manager_enumerate(Manager *m) {
        UnitType c;

        assert(m);

        /* Let's ask every type to load all units from disk/kernel
         * that it might know */
        for (c = 0; c < _UNIT_TYPE_MAX; c++) {
                if (!unit_type_supported(c)) {
                        log_debug("Unit type .%s is not supported on this system.", unit_type_to_string(c));
                        continue;
                }

                if (!unit_vtable[c]->enumerate)
                        continue;

                unit_vtable[c]->enumerate(m);
        }

        manager_dispatch_load_queue(m);
}

static void manager_coldplug(Manager *m) {
        Iterator i;
        Unit *u;
        char *k;
        int r;

        assert(m);

        /* Then, let's set up their initial state. */
        HASHMAP_FOREACH_KEY(u, k, m->units, i) {

                /* ignore aliases */
                if (u->id != k)
                        continue;

                r = unit_coldplug(u);
                if (r < 0)
                        log_warning_errno(r, "We couldn't coldplug %s, proceeding anyway: %m", u->id);
        }
}

static void manager_build_unit_path_cache(Manager *m) {
        char **i;
        int r;

        assert(m);

        set_free_free(m->unit_path_cache);

        m->unit_path_cache = set_new(&string_hash_ops);
        if (!m->unit_path_cache) {
                r = -ENOMEM;
                goto fail;
        }

        /* This simply builds a list of files we know exist, so that
         * we don't always have to go to disk */

        STRV_FOREACH(i, m->lookup_paths.search_path) {
                _cleanup_closedir_ DIR *d = NULL;
                struct dirent *de;

                d = opendir(*i);
                if (!d) {
                        if (errno != ENOENT)
                                log_warning_errno(errno, "Failed to open directory %s, ignoring: %m", *i);
                        continue;
                }

                FOREACH_DIRENT(de, d, r = -errno; goto fail) {
                        char *p;

                        p = strjoin(streq(*i, "/") ? "" : *i, "/", de->d_name);
                        if (!p) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        r = set_consume(m->unit_path_cache, p);
                        if (r < 0)
                                goto fail;
                }
        }

        return;

fail:
        log_warning_errno(r, "Failed to build unit path cache, proceeding without: %m");
        m->unit_path_cache = set_free_free(m->unit_path_cache);
}

static void manager_distribute_fds(Manager *m, FDSet *fds) {
        Iterator i;
        Unit *u;

        assert(m);

        HASHMAP_FOREACH(u, m->units, i) {

                if (fdset_size(fds) <= 0)
                        break;

                if (!UNIT_VTABLE(u)->distribute_fds)
                        continue;

                UNIT_VTABLE(u)->distribute_fds(u, fds);
        }
}

int manager_startup(Manager *m, FILE *serialization, FDSet *fds) {
        int r, q;

        assert(m);

        r = lookup_paths_init(&m->lookup_paths, m->unit_file_scope, 0, NULL);
        if (r < 0)
                return r;

        r = manager_run_environment_generators(m);
        if (r < 0)
                return r;

        /* Make sure the transient directory always exists, so that it remains in the search path */
        if (!m->test_run) {
                r = mkdir_p_label(m->lookup_paths.transient, 0755);
                if (r < 0)
                        return r;
        }

        dual_timestamp_get(&m->generators_start_timestamp);
        r = manager_run_generators(m);
        dual_timestamp_get(&m->generators_finish_timestamp);
        if (r < 0)
                return r;

        lookup_paths_reduce(&m->lookup_paths);
        manager_build_unit_path_cache(m);

        /* If we will deserialize make sure that during enumeration
         * this is already known, so we increase the counter here
         * already */
        if (serialization)
                m->n_reloading++;

        /* First, enumerate what we can from all config files */
        dual_timestamp_get(&m->units_load_start_timestamp);
        manager_enumerate(m);
        dual_timestamp_get(&m->units_load_finish_timestamp);

        /* Second, deserialize if there is something to deserialize */
        if (serialization)
                r = manager_deserialize(m, serialization, fds);

        /* Any fds left? Find some unit which wants them. This is
         * useful to allow container managers to pass some file
         * descriptors to us pre-initialized. This enables
         * socket-based activation of entire containers. */
        manager_distribute_fds(m, fds);

        /* We might have deserialized the notify fd, but if we didn't
         * then let's create the bus now */
        q = manager_setup_notify(m);
        if (q < 0 && r == 0)
                r = q;

        q = manager_setup_cgroups_agent(m);
        if (q < 0 && r == 0)
                r = q;

        q = manager_setup_user_lookup_fd(m);
        if (q < 0 && r == 0)
                r = q;

        /* Let's connect to the bus now. */
        (void) manager_connect_bus(m, !!serialization);

        (void) bus_track_coldplug(m, &m->subscribed, false, m->deserialized_subscribed);
        m->deserialized_subscribed = strv_free(m->deserialized_subscribed);

        /* Third, fire things up! */
        manager_coldplug(m);

        /* Release any dynamic users no longer referenced */
        dynamic_user_vacuum(m, true);

        /* Release any references to UIDs/GIDs no longer referenced, and destroy any IPC owned by them */
        manager_vacuum_uid_refs(m);
        manager_vacuum_gid_refs(m);

        if (serialization) {
                assert(m->n_reloading > 0);
                m->n_reloading--;

                /* Let's wait for the UnitNew/JobNew messages being
                 * sent, before we notify that the reload is
                 * finished */
                m->send_reloading_done = true;
        }

        return r;
}

int manager_add_job(Manager *m, JobType type, Unit *unit, JobMode mode, sd_bus_error *e, Job **_ret) {
        int r;
        Transaction *tr;

        assert(m);
        assert(type < _JOB_TYPE_MAX);
        assert(unit);
        assert(mode < _JOB_MODE_MAX);

        if (mode == JOB_ISOLATE && type != JOB_START)
                return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "Isolate is only valid for start.");

        if (mode == JOB_ISOLATE && !unit->allow_isolate)
                return sd_bus_error_setf(e, BUS_ERROR_NO_ISOLATION, "Operation refused, unit may not be isolated.");

        log_unit_debug(unit, "Trying to enqueue job %s/%s/%s", unit->id, job_type_to_string(type), job_mode_to_string(mode));

        type = job_type_collapse(type, unit);

        tr = transaction_new(mode == JOB_REPLACE_IRREVERSIBLY);
        if (!tr)
                return -ENOMEM;

        r = transaction_add_job_and_dependencies(tr, type, unit, NULL, true, false,
                                                 mode == JOB_IGNORE_DEPENDENCIES || mode == JOB_IGNORE_REQUIREMENTS,
                                                 mode == JOB_IGNORE_DEPENDENCIES, e);
        if (r < 0)
                goto tr_abort;

        if (mode == JOB_ISOLATE) {
                r = transaction_add_isolate_jobs(tr, m);
                if (r < 0)
                        goto tr_abort;
        }

        r = transaction_activate(tr, m, mode, e);
        if (r < 0)
                goto tr_abort;

        log_unit_debug(unit,
                       "Enqueued job %s/%s as %u", unit->id,
                       job_type_to_string(type), (unsigned) tr->anchor_job->id);

        if (_ret)
                *_ret = tr->anchor_job;

        transaction_free(tr);
        return 0;

tr_abort:
        transaction_abort(tr);
        transaction_free(tr);
        return r;
}

int manager_add_job_by_name(Manager *m, JobType type, const char *name, JobMode mode, sd_bus_error *e, Job **ret) {
        Unit *unit = NULL;  /* just to appease gcc, initialization is not really necessary */
        int r;

        assert(m);
        assert(type < _JOB_TYPE_MAX);
        assert(name);
        assert(mode < _JOB_MODE_MAX);

        r = manager_load_unit(m, name, NULL, NULL, &unit);
        if (r < 0)
                return r;
        assert(unit);

        return manager_add_job(m, type, unit, mode, e, ret);
}

int manager_add_job_by_name_and_warn(Manager *m, JobType type, const char *name, JobMode mode, Job **ret) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        int r;

        assert(m);
        assert(type < _JOB_TYPE_MAX);
        assert(name);
        assert(mode < _JOB_MODE_MAX);

        r = manager_add_job_by_name(m, type, name, mode, &error, ret);
        if (r < 0)
                return log_warning_errno(r, "Failed to enqueue %s job for %s: %s", job_mode_to_string(mode), name, bus_error_message(&error, r));

        return r;
}

Job *manager_get_job(Manager *m, uint32_t id) {
        assert(m);

        return hashmap_get(m->jobs, UINT32_TO_PTR(id));
}

Unit *manager_get_unit(Manager *m, const char *name) {
        assert(m);
        assert(name);

        return hashmap_get(m->units, name);
}

unsigned manager_dispatch_load_queue(Manager *m) {
        Unit *u;
        unsigned n = 0;

        assert(m);

        /* Make sure we are not run recursively */
        if (m->dispatching_load_queue)
                return 0;

        m->dispatching_load_queue = true;

        /* Dispatches the load queue. Takes a unit from the queue and
         * tries to load its data until the queue is empty */

        while ((u = m->load_queue)) {
                assert(u->in_load_queue);

                unit_load(u);
                n++;
        }

        m->dispatching_load_queue = false;
        return n;
}

int manager_load_unit_prepare(
                Manager *m,
                const char *name,
                const char *path,
                sd_bus_error *e,
                Unit **_ret) {

        Unit *ret;
        UnitType t;
        int r;

        assert(m);
        assert(name || path);
        assert(_ret);

        /* This will prepare the unit for loading, but not actually
         * load anything from disk. */

        if (path && !is_path(path))
                return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "Path %s is not absolute.", path);

        if (!name)
                name = basename(path);

        t = unit_name_to_type(name);

        if (t == _UNIT_TYPE_INVALID || !unit_name_is_valid(name, UNIT_NAME_PLAIN|UNIT_NAME_INSTANCE)) {
                if (unit_name_is_valid(name, UNIT_NAME_TEMPLATE))
                        return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "Unit name %s is missing the instance name.", name);

                return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "Unit name %s is not valid.", name);
        }

        ret = manager_get_unit(m, name);
        if (ret) {
                *_ret = ret;
                return 1;
        }

        ret = unit_new(m, unit_vtable[t]->object_size);
        if (!ret)
                return -ENOMEM;

        if (path) {
                ret->fragment_path = strdup(path);
                if (!ret->fragment_path) {
                        unit_free(ret);
                        return -ENOMEM;
                }
        }

        r = unit_add_name(ret, name);
        if (r < 0) {
                unit_free(ret);
                return r;
        }

        unit_add_to_load_queue(ret);
        unit_add_to_dbus_queue(ret);
        unit_add_to_gc_queue(ret);

        *_ret = ret;

        return 0;
}

int manager_load_unit(
                Manager *m,
                const char *name,
                const char *path,
                sd_bus_error *e,
                Unit **_ret) {

        int r;

        assert(m);
        assert(_ret);

        /* This will load the service information files, but not actually
         * start any services or anything. */

        r = manager_load_unit_prepare(m, name, path, e, _ret);
        if (r != 0)
                return r;

        manager_dispatch_load_queue(m);

        *_ret = unit_follow_merge(*_ret);

        return 0;
}

void manager_dump_jobs(Manager *s, FILE *f, const char *prefix) {
        Iterator i;
        Job *j;

        assert(s);
        assert(f);

        HASHMAP_FOREACH(j, s->jobs, i)
                job_dump(j, f, prefix);
}

void manager_dump_units(Manager *s, FILE *f, const char *prefix) {
        Iterator i;
        Unit *u;
        const char *t;

        assert(s);
        assert(f);

        HASHMAP_FOREACH_KEY(u, t, s->units, i)
                if (u->id == t)
                        unit_dump(u, f, prefix);
}

void manager_clear_jobs(Manager *m) {
        Job *j;

        assert(m);

        while ((j = hashmap_first(m->jobs)))
                /* No need to recurse. We're cancelling all jobs. */
                job_finish_and_invalidate(j, JOB_CANCELED, false, false);
}

static int manager_dispatch_run_queue(sd_event_source *source, void *userdata) {
        Manager *m = userdata;
        Job *j;

        assert(source);
        assert(m);

        while ((j = m->run_queue)) {
                assert(j->installed);
                assert(j->in_run_queue);

                job_run_and_invalidate(j);
        }

        if (m->n_running_jobs > 0)
                manager_watch_jobs_in_progress(m);

        if (m->n_on_console > 0)
                manager_watch_idle_pipe(m);

        return 1;
}

static unsigned manager_dispatch_dbus_queue(Manager *m) {
        Job *j;
        Unit *u;
        unsigned n = 0;

        assert(m);

        if (m->dispatching_dbus_queue)
                return 0;

        m->dispatching_dbus_queue = true;

        while ((u = m->dbus_unit_queue)) {
                assert(u->in_dbus_queue);

                bus_unit_send_change_signal(u);
                n++;
        }

        while ((j = m->dbus_job_queue)) {
                assert(j->in_dbus_queue);

                bus_job_send_change_signal(j);
                n++;
        }

        m->dispatching_dbus_queue = false;

        if (m->send_reloading_done) {
                m->send_reloading_done = false;

                bus_manager_send_reloading(m, false);
        }

        if (m->queued_message)
                bus_send_queued_message(m);

        return n;
}

static int manager_dispatch_cgroups_agent_fd(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        Manager *m = userdata;
        char buf[PATH_MAX+1];
        ssize_t n;

        n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0)
                return log_error_errno(errno, "Failed to read cgroups agent message: %m");
        if (n == 0) {
                log_error("Got zero-length cgroups agent message, ignoring.");
                return 0;
        }
        if ((size_t) n >= sizeof(buf)) {
                log_error("Got overly long cgroups agent message, ignoring.");
                return 0;
        }

        if (memchr(buf, 0, n)) {
                log_error("Got cgroups agent message with embedded NUL byte, ignoring.");
                return 0;
        }
        buf[n] = 0;

        manager_notify_cgroup_empty(m, buf);
        bus_forward_agent_released(m, buf);

        return 0;
}

static void manager_invoke_notify_message(Manager *m, Unit *u, pid_t pid, const char *buf, FDSet *fds) {
        _cleanup_strv_free_ char **tags = NULL;

        assert(m);
        assert(u);
        assert(buf);

        tags = strv_split(buf, "\n\r");
        if (!tags) {
                log_oom();
                return;
        }

        if (UNIT_VTABLE(u)->notify_message)
                UNIT_VTABLE(u)->notify_message(u, pid, tags, fds);
        else if (_unlikely_(log_get_max_level() >= LOG_DEBUG)) {
                _cleanup_free_ char *x = NULL, *y = NULL;

                x = cescape(buf);
                if (x)
                        y = ellipsize(x, 20, 90);
                log_unit_debug(u, "Got notification message \"%s\", ignoring.", strnull(y));
        }
}

static int manager_dispatch_notify_fd(sd_event_source *source, int fd, uint32_t revents, void *userdata) {

        _cleanup_fdset_free_ FDSet *fds = NULL;
        Manager *m = userdata;
        char buf[NOTIFY_BUFFER_MAX+1];
        struct iovec iovec = {
                .iov_base = buf,
                .iov_len = sizeof(buf)-1,
        };
        union {
                struct cmsghdr cmsghdr;
                uint8_t buf[CMSG_SPACE(sizeof(struct ucred)) +
                            CMSG_SPACE(sizeof(int) * NOTIFY_FD_MAX)];
        } control = {};
        struct msghdr msghdr = {
                .msg_iov = &iovec,
                .msg_iovlen = 1,
                .msg_control = &control,
                .msg_controllen = sizeof(control),
        };

        struct cmsghdr *cmsg;
        struct ucred *ucred = NULL;
        Unit *u1, *u2, *u3;
        int r, *fd_array = NULL;
        unsigned n_fds = 0;
        ssize_t n;

        assert(m);
        assert(m->notify_fd == fd);

        if (revents != EPOLLIN) {
                log_warning("Got unexpected poll event for notify fd.");
                return 0;
        }

        n = recvmsg(m->notify_fd, &msghdr, MSG_DONTWAIT|MSG_CMSG_CLOEXEC|MSG_TRUNC);
        if (n < 0) {
                if (IN_SET(errno, EAGAIN, EINTR))
                        return 0; /* Spurious wakeup, try again */

                /* If this is any other, real error, then let's stop processing this socket. This of course means we
                 * won't take notification messages anymore, but that's still better than busy looping around this:
                 * being woken up over and over again but being unable to actually read the message off the socket. */
                return log_error_errno(errno, "Failed to receive notification message: %m");
        }

        CMSG_FOREACH(cmsg, &msghdr) {
                if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {

                        fd_array = (int*) CMSG_DATA(cmsg);
                        n_fds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);

                } else if (cmsg->cmsg_level == SOL_SOCKET &&
                           cmsg->cmsg_type == SCM_CREDENTIALS &&
                           cmsg->cmsg_len == CMSG_LEN(sizeof(struct ucred))) {

                        ucred = (struct ucred*) CMSG_DATA(cmsg);
                }
        }

        if (n_fds > 0) {
                assert(fd_array);

                r = fdset_new_array(&fds, fd_array, n_fds);
                if (r < 0) {
                        close_many(fd_array, n_fds);
                        log_oom();
                        return 0;
                }
        }

        if (!ucred || ucred->pid <= 0) {
                log_warning("Received notify message without valid credentials. Ignoring.");
                return 0;
        }

        if ((size_t) n >= sizeof(buf) || (msghdr.msg_flags & MSG_TRUNC)) {
                log_warning("Received notify message exceeded maximum size. Ignoring.");
                return 0;
        }

        /* As extra safety check, let's make sure the string we get doesn't contain embedded NUL bytes. We permit one
         * trailing NUL byte in the message, but don't expect it. */
        if (n > 1 && memchr(buf, 0, n-1)) {
                log_warning("Received notify message with embedded NUL bytes. Ignoring.");
                return 0;
        }

        /* Make sure it's NUL-terminated. */
        buf[n] = 0;

        /* Notify every unit that might be interested, but try
         * to avoid notifying the same one multiple times. */
        u1 = manager_get_unit_by_pid_cgroup(m, ucred->pid);
        if (u1)
                manager_invoke_notify_message(m, u1, ucred->pid, buf, fds);

        u2 = hashmap_get(m->watch_pids1, PID_TO_PTR(ucred->pid));
        if (u2 && u2 != u1)
                manager_invoke_notify_message(m, u2, ucred->pid, buf, fds);

        u3 = hashmap_get(m->watch_pids2, PID_TO_PTR(ucred->pid));
        if (u3 && u3 != u2 && u3 != u1)
                manager_invoke_notify_message(m, u3, ucred->pid, buf, fds);

        if (!u1 && !u2 && !u3)
                log_warning("Cannot find unit for notify message of PID "PID_FMT".", ucred->pid);

        if (fdset_size(fds) > 0)
                log_warning("Got extra auxiliary fds with notification message, closing them.");

        return 0;
}

static void invoke_sigchld_event(Manager *m, Unit *u, const siginfo_t *si) {
        uint64_t iteration;

        assert(m);
        assert(u);
        assert(si);

        sd_event_get_iteration(m->event, &iteration);

        log_unit_debug(u, "Child "PID_FMT" belongs to %s", si->si_pid, u->id);

        unit_unwatch_pid(u, si->si_pid);

        if (UNIT_VTABLE(u)->sigchld_event) {
                if (set_size(u->pids) <= 1 ||
                    iteration != u->sigchldgen ||
                    unit_main_pid(u) == si->si_pid ||
                    unit_control_pid(u) == si->si_pid) {
                        UNIT_VTABLE(u)->sigchld_event(u, si->si_pid, si->si_code, si->si_status);
                        u->sigchldgen = iteration;
                } else
                        log_debug("%s already issued a sigchld this iteration %" PRIu64 ", skipping. Pids still being watched %d", u->id, iteration, set_size(u->pids));
         }
}

static int manager_dispatch_sigchld(Manager *m) {
        assert(m);

        for (;;) {
                siginfo_t si = {};

                /* First we call waitd() for a PID and do not reap the
                 * zombie. That way we can still access /proc/$PID for
                 * it while it is a zombie. */
                if (waitid(P_ALL, 0, &si, WEXITED|WNOHANG|WNOWAIT) < 0) {

                        if (errno == ECHILD)
                                break;

                        if (errno == EINTR)
                                continue;

                        return -errno;
                }

                if (si.si_pid <= 0)
                        break;

                if (si.si_code == CLD_EXITED || si.si_code == CLD_KILLED || si.si_code == CLD_DUMPED) {
                        _cleanup_free_ char *name = NULL;
                        Unit *u1, *u2, *u3;

                        get_process_comm(si.si_pid, &name);

                        log_debug("Child "PID_FMT" (%s) died (code=%s, status=%i/%s)",
                                  si.si_pid, strna(name),
                                  sigchld_code_to_string(si.si_code),
                                  si.si_status,
                                  strna(si.si_code == CLD_EXITED
                                        ? exit_status_to_string(si.si_status, EXIT_STATUS_FULL)
                                        : signal_to_string(si.si_status)));

                        /* And now figure out the unit this belongs
                         * to, it might be multiple... */
                        u1 = manager_get_unit_by_pid_cgroup(m, si.si_pid);
                        if (u1)
                                invoke_sigchld_event(m, u1, &si);
                        u2 = hashmap_get(m->watch_pids1, PID_TO_PTR(si.si_pid));
                        if (u2 && u2 != u1)
                                invoke_sigchld_event(m, u2, &si);
                        u3 = hashmap_get(m->watch_pids2, PID_TO_PTR(si.si_pid));
                        if (u3 && u3 != u2 && u3 != u1)
                                invoke_sigchld_event(m, u3, &si);
                }

                /* And now, we actually reap the zombie. */
                if (waitid(P_PID, si.si_pid, &si, WEXITED) < 0) {
                        if (errno == EINTR)
                                continue;

                        return -errno;
                }
        }

        return 0;
}

static int manager_start_target(Manager *m, const char *name, JobMode mode) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        int r;

        log_debug("Activating special unit %s", name);

        r = manager_add_job_by_name(m, JOB_START, name, mode, &error, NULL);
        if (r < 0)
                log_error("Failed to enqueue %s job: %s", name, bus_error_message(&error, r));

        return r;
}

static void manager_handle_ctrl_alt_del(Manager *m) {
        /* If the user presses C-A-D more than
         * 7 times within 2s, we reboot/shutdown immediately,
         * unless it was disabled in system.conf */

        if (ratelimit_test(&m->ctrl_alt_del_ratelimit) || m->cad_burst_action == EMERGENCY_ACTION_NONE)
                manager_start_target(m, SPECIAL_CTRL_ALT_DEL_TARGET, JOB_REPLACE_IRREVERSIBLY);
        else
                emergency_action(m, m->cad_burst_action, NULL,
                                "Ctrl-Alt-Del was pressed more than 7 times within 2s");
}

static int manager_dispatch_signal_fd(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        Manager *m = userdata;
        ssize_t n;
        struct signalfd_siginfo sfsi;
        bool sigchld = false;
        int r;

        assert(m);
        assert(m->signal_fd == fd);

        if (revents != EPOLLIN) {
                log_warning("Got unexpected events from signal file descriptor.");
                return 0;
        }

        for (;;) {
                n = read(m->signal_fd, &sfsi, sizeof(sfsi));
                if (n != sizeof(sfsi)) {
                        if (n >= 0) {
                                log_warning("Truncated read from signal fd (%zu bytes)!", n);
                                return 0;
                        }

                        if (IN_SET(errno, EINTR, EAGAIN))
                                break;

                        /* We return an error here, which will kill this handler,
                         * to avoid a busy loop on read error. */
                        return log_error_errno(errno, "Reading from signal fd failed: %m");
                }

                log_received_signal(sfsi.ssi_signo == SIGCHLD ||
                                    (sfsi.ssi_signo == SIGTERM && MANAGER_IS_USER(m))
                                    ? LOG_DEBUG : LOG_INFO,
                                    &sfsi);

                switch (sfsi.ssi_signo) {

                case SIGCHLD:
                        sigchld = true;
                        break;

                case SIGTERM:
                        if (MANAGER_IS_SYSTEM(m)) {
                                /* This is for compatibility with the
                                 * original sysvinit */
                                r = verify_run_space_and_log("Refusing to reexecute");
                                if (r >= 0)
                                        m->exit_code = MANAGER_REEXECUTE;
                                break;
                        }

                        /* Fall through */

                case SIGINT:
                        if (MANAGER_IS_SYSTEM(m)) {
                                manager_handle_ctrl_alt_del(m);
                                break;
                        }

                        /* Run the exit target if there is one, if not, just exit. */
                        if (manager_start_target(m, SPECIAL_EXIT_TARGET, JOB_REPLACE) < 0) {
                                m->exit_code = MANAGER_EXIT;
                                return 0;
                        }

                        break;

                case SIGWINCH:
                        if (MANAGER_IS_SYSTEM(m))
                                manager_start_target(m, SPECIAL_KBREQUEST_TARGET, JOB_REPLACE);

                        /* This is a nop on non-init */
                        break;

                case SIGPWR:
                        if (MANAGER_IS_SYSTEM(m))
                                manager_start_target(m, SPECIAL_SIGPWR_TARGET, JOB_REPLACE);

                        /* This is a nop on non-init */
                        break;

                case SIGUSR1: {
                        Unit *u;

                        u = manager_get_unit(m, SPECIAL_DBUS_SERVICE);

                        if (!u || UNIT_IS_ACTIVE_OR_RELOADING(unit_active_state(u))) {
                                log_info("Trying to reconnect to bus...");
                                bus_init(m, true);
                        }

                        if (!u || !UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(u))) {
                                log_info("Loading D-Bus service...");
                                manager_start_target(m, SPECIAL_DBUS_SERVICE, JOB_REPLACE);
                        }

                        break;
                }

                case SIGUSR2: {
                        _cleanup_free_ char *dump = NULL;
                        _cleanup_fclose_ FILE *f = NULL;
                        size_t size;

                        f = open_memstream(&dump, &size);
                        if (!f) {
                                log_warning_errno(errno, "Failed to allocate memory stream: %m");
                                break;
                        }

                        manager_dump_units(m, f, "\t");
                        manager_dump_jobs(m, f, "\t");

                        r = fflush_and_check(f);
                        if (r < 0) {
                                log_warning_errno(r, "Failed to write status stream: %m");
                                break;
                        }

                        log_dump(LOG_INFO, dump);
                        break;
                }

                case SIGHUP:
                        r = verify_run_space_and_log("Refusing to reload");
                        if (r >= 0)
                                m->exit_code = MANAGER_RELOAD;
                        break;

                default: {

                        /* Starting SIGRTMIN+0 */
                        static const char * const target_table[] = {
                                [0] = SPECIAL_DEFAULT_TARGET,
                                [1] = SPECIAL_RESCUE_TARGET,
                                [2] = SPECIAL_EMERGENCY_TARGET,
                                [3] = SPECIAL_HALT_TARGET,
                                [4] = SPECIAL_POWEROFF_TARGET,
                                [5] = SPECIAL_REBOOT_TARGET,
                                [6] = SPECIAL_KEXEC_TARGET
                        };

                        /* Starting SIGRTMIN+13, so that target halt and system halt are 10 apart */
                        static const ManagerExitCode code_table[] = {
                                [0] = MANAGER_HALT,
                                [1] = MANAGER_POWEROFF,
                                [2] = MANAGER_REBOOT,
                                [3] = MANAGER_KEXEC
                        };

                        if ((int) sfsi.ssi_signo >= SIGRTMIN+0 &&
                            (int) sfsi.ssi_signo < SIGRTMIN+(int) ELEMENTSOF(target_table)) {
                                int idx = (int) sfsi.ssi_signo - SIGRTMIN;
                                manager_start_target(m, target_table[idx],
                                                     (idx == 1 || idx == 2) ? JOB_ISOLATE : JOB_REPLACE);
                                break;
                        }

                        if ((int) sfsi.ssi_signo >= SIGRTMIN+13 &&
                            (int) sfsi.ssi_signo < SIGRTMIN+13+(int) ELEMENTSOF(code_table)) {
                                m->exit_code = code_table[sfsi.ssi_signo - SIGRTMIN - 13];
                                break;
                        }

                        switch (sfsi.ssi_signo - SIGRTMIN) {

                        case 20:
                                manager_set_show_status(m, SHOW_STATUS_YES);
                                break;

                        case 21:
                                manager_set_show_status(m, SHOW_STATUS_NO);
                                break;

                        case 22:
                                log_set_max_level(LOG_DEBUG);
                                log_info("Setting log level to debug.");
                                break;

                        case 23:
                                log_set_max_level(LOG_INFO);
                                log_info("Setting log level to info.");
                                break;

                        case 24:
                                if (MANAGER_IS_USER(m)) {
                                        m->exit_code = MANAGER_EXIT;
                                        return 0;
                                }

                                /* This is a nop on init */
                                break;

                        case 26:
                        case 29: /* compatibility: used to be mapped to LOG_TARGET_SYSLOG_OR_KMSG */
                                log_set_target(LOG_TARGET_JOURNAL_OR_KMSG);
                                log_notice("Setting log target to journal-or-kmsg.");
                                break;

                        case 27:
                                log_set_target(LOG_TARGET_CONSOLE);
                                log_notice("Setting log target to console.");
                                break;

                        case 28:
                                log_set_target(LOG_TARGET_KMSG);
                                log_notice("Setting log target to kmsg.");
                                break;

                        default:
                                log_warning("Got unhandled signal <%s>.", signal_to_string(sfsi.ssi_signo));
                        }
                }
                }
        }

        if (sigchld)
                manager_dispatch_sigchld(m);

        return 0;
}

static int manager_dispatch_time_change_fd(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        Manager *m = userdata;
        Iterator i;
        Unit *u;

        assert(m);
        assert(m->time_change_fd == fd);

        log_struct(LOG_DEBUG,
                   "MESSAGE_ID=" SD_MESSAGE_TIME_CHANGE_STR,
                   LOG_MESSAGE("Time has been changed"),
                   NULL);

        /* Restart the watch */
        m->time_change_event_source = sd_event_source_unref(m->time_change_event_source);
        m->time_change_fd = safe_close(m->time_change_fd);

        manager_setup_time_change(m);

        HASHMAP_FOREACH(u, m->units, i)
                if (UNIT_VTABLE(u)->time_change)
                        UNIT_VTABLE(u)->time_change(u);

        return 0;
}

static int manager_dispatch_idle_pipe_fd(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        Manager *m = userdata;

        assert(m);
        assert(m->idle_pipe[2] == fd);

        m->no_console_output = m->n_on_console > 0;

        manager_close_idle_pipe(m);

        return 0;
}

static int manager_dispatch_jobs_in_progress(sd_event_source *source, usec_t usec, void *userdata) {
        Manager *m = userdata;
        int r;
        uint64_t next;

        assert(m);
        assert(source);

        manager_print_jobs_in_progress(m);

        next = now(CLOCK_MONOTONIC) + JOBS_IN_PROGRESS_PERIOD_USEC;
        r = sd_event_source_set_time(source, next);
        if (r < 0)
                return r;

        return sd_event_source_set_enabled(source, SD_EVENT_ONESHOT);
}

int manager_loop(Manager *m) {
        int r;

        RATELIMIT_DEFINE(rl, 1*USEC_PER_SEC, 50000);

        assert(m);
        m->exit_code = MANAGER_OK;

        /* Release the path cache */
        m->unit_path_cache = set_free_free(m->unit_path_cache);

        manager_check_finished(m);

        /* There might still be some zombies hanging around from
         * before we were exec()'ed. Let's reap them. */
        r = manager_dispatch_sigchld(m);
        if (r < 0)
                return r;

        while (m->exit_code == MANAGER_OK) {
                usec_t wait_usec;

                if (m->runtime_watchdog > 0 && m->runtime_watchdog != USEC_INFINITY && MANAGER_IS_SYSTEM(m))
                        watchdog_ping();

                if (!ratelimit_test(&rl)) {
                        /* Yay, something is going seriously wrong, pause a little */
                        log_warning("Looping too fast. Throttling execution a little.");
                        sleep(1);
                }

                if (manager_dispatch_load_queue(m) > 0)
                        continue;

                if (manager_dispatch_gc_job_queue(m) > 0)
                        continue;

                if (manager_dispatch_gc_unit_queue(m) > 0)
                        continue;

                if (manager_dispatch_cleanup_queue(m) > 0)
                        continue;

                if (manager_dispatch_cgroup_queue(m) > 0)
                        continue;

                if (manager_dispatch_dbus_queue(m) > 0)
                        continue;

                /* Sleep for half the watchdog time */
                if (m->runtime_watchdog > 0 && m->runtime_watchdog != USEC_INFINITY && MANAGER_IS_SYSTEM(m)) {
                        wait_usec = m->runtime_watchdog / 2;
                        if (wait_usec <= 0)
                                wait_usec = 1;
                } else
                        wait_usec = USEC_INFINITY;

                r = sd_event_run(m->event, wait_usec);
                if (r < 0)
                        return log_error_errno(r, "Failed to run event loop: %m");
        }

        return m->exit_code;
}

int manager_load_unit_from_dbus_path(Manager *m, const char *s, sd_bus_error *e, Unit **_u) {
        _cleanup_free_ char *n = NULL;
        sd_id128_t invocation_id;
        Unit *u;
        int r;

        assert(m);
        assert(s);
        assert(_u);

        r = unit_name_from_dbus_path(s, &n);
        if (r < 0)
                return r;

        /* Permit addressing units by invocation ID: if the passed bus path is suffixed by a 128bit ID then we use it
         * as invocation ID. */
        r = sd_id128_from_string(n, &invocation_id);
        if (r >= 0) {
                u = hashmap_get(m->units_by_invocation_id, &invocation_id);
                if (u) {
                        *_u = u;
                        return 0;
                }

                return sd_bus_error_setf(e, BUS_ERROR_NO_UNIT_FOR_INVOCATION_ID, "No unit with the specified invocation ID " SD_ID128_FORMAT_STR " known.", SD_ID128_FORMAT_VAL(invocation_id));
        }

        /* If this didn't work, we check if this is a unit name */
        if (!unit_name_is_valid(n, UNIT_NAME_PLAIN|UNIT_NAME_INSTANCE))
                return sd_bus_error_setf(e, SD_BUS_ERROR_INVALID_ARGS, "Unit name %s is neither a valid invocation ID nor unit name.", n);

        r = manager_load_unit(m, n, NULL, e, &u);
        if (r < 0)
                return r;

        *_u = u;
        return 0;
}

int manager_get_job_from_dbus_path(Manager *m, const char *s, Job **_j) {
        const char *p;
        unsigned id;
        Job *j;
        int r;

        assert(m);
        assert(s);
        assert(_j);

        p = startswith(s, "/org/freedesktop/systemd1/job/");
        if (!p)
                return -EINVAL;

        r = safe_atou(p, &id);
        if (r < 0)
                return r;

        j = manager_get_job(m, id);
        if (!j)
                return -ENOENT;

        *_j = j;

        return 0;
}

void manager_send_unit_audit(Manager *m, Unit *u, int type, bool success) {

#ifdef HAVE_AUDIT
        _cleanup_free_ char *p = NULL;
        const char *msg;
        int audit_fd, r;

        if (!MANAGER_IS_SYSTEM(m))
                return;

        audit_fd = get_audit_fd();
        if (audit_fd < 0)
                return;

        /* Don't generate audit events if the service was already
         * started and we're just deserializing */
        if (MANAGER_IS_RELOADING(m))
                return;

        if (u->type != UNIT_SERVICE)
                return;

        r = unit_name_to_prefix_and_instance(u->id, &p);
        if (r < 0) {
                log_error_errno(r, "Failed to extract prefix and instance of unit name: %m");
                return;
        }

        msg = strjoina("unit=", p);
        if (audit_log_user_comm_message(audit_fd, type, msg, "systemd", NULL, NULL, NULL, success) < 0) {
                if (errno == EPERM)
                        /* We aren't allowed to send audit messages?
                         * Then let's not retry again. */
                        close_audit_fd();
                else
                        log_warning_errno(errno, "Failed to send audit message: %m");
        }
#endif

}

void manager_send_unit_plymouth(Manager *m, Unit *u) {
        static const union sockaddr_union sa = PLYMOUTH_SOCKET;
        _cleanup_free_ char *message = NULL;
        _cleanup_close_ int fd = -1;
        int n = 0;

        /* Don't generate plymouth events if the service was already
         * started and we're just deserializing */
        if (MANAGER_IS_RELOADING(m))
                return;

        if (!MANAGER_IS_SYSTEM(m))
                return;

        if (detect_container() > 0)
                return;

        if (u->type != UNIT_SERVICE &&
            u->type != UNIT_MOUNT &&
            u->type != UNIT_SWAP)
                return;

        /* We set SOCK_NONBLOCK here so that we rather drop the
         * message then wait for plymouth */
        fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
        if (fd < 0) {
                log_error_errno(errno, "socket() failed: %m");
                return;
        }

        if (connect(fd, &sa.sa, SOCKADDR_UN_LEN(sa.un)) < 0) {

                if (!IN_SET(errno, EPIPE, EAGAIN, ENOENT, ECONNREFUSED, ECONNRESET, ECONNABORTED))
                        log_error_errno(errno, "connect() failed: %m");
                return;
        }

        if (asprintf(&message, "U\002%c%s%n", (int) (strlen(u->id) + 1), u->id, &n) < 0) {
                log_oom();
                return;
        }

        errno = 0;
        if (write(fd, message, n + 1) != n + 1)
                if (!IN_SET(errno, EPIPE, EAGAIN, ENOENT, ECONNREFUSED, ECONNRESET, ECONNABORTED))
                        log_error_errno(errno, "Failed to write Plymouth message: %m");
}

int manager_open_serialization(Manager *m, FILE **_f) {
        int fd;
        FILE *f;

        assert(_f);

        fd = open_serialization_fd("systemd-state");
        if (fd < 0)
                return fd;

        f = fdopen(fd, "w+");
        if (!f) {
                safe_close(fd);
                return -errno;
        }

        *_f = f;
        return 0;
}

int manager_serialize(Manager *m, FILE *f, FDSet *fds, bool switching_root) {
        Iterator i;
        Unit *u;
        const char *t;
        int r;

        assert(m);
        assert(f);
        assert(fds);

        m->n_reloading++;

        fprintf(f, "current-job-id=%"PRIu32"\n", m->current_job_id);
        fprintf(f, "taint-usr=%s\n", yes_no(m->taint_usr));
        fprintf(f, "n-installed-jobs=%u\n", m->n_installed_jobs);
        fprintf(f, "n-failed-jobs=%u\n", m->n_failed_jobs);

        dual_timestamp_serialize(f, "firmware-timestamp", &m->firmware_timestamp);
        dual_timestamp_serialize(f, "loader-timestamp", &m->loader_timestamp);
        dual_timestamp_serialize(f, "kernel-timestamp", &m->kernel_timestamp);
        dual_timestamp_serialize(f, "initrd-timestamp", &m->initrd_timestamp);

        if (!in_initrd()) {
                dual_timestamp_serialize(f, "userspace-timestamp", &m->userspace_timestamp);
                dual_timestamp_serialize(f, "finish-timestamp", &m->finish_timestamp);
                dual_timestamp_serialize(f, "security-start-timestamp", &m->security_start_timestamp);
                dual_timestamp_serialize(f, "security-finish-timestamp", &m->security_finish_timestamp);
                dual_timestamp_serialize(f, "generators-start-timestamp", &m->generators_start_timestamp);
                dual_timestamp_serialize(f, "generators-finish-timestamp", &m->generators_finish_timestamp);
                dual_timestamp_serialize(f, "units-load-start-timestamp", &m->units_load_start_timestamp);
                dual_timestamp_serialize(f, "units-load-finish-timestamp", &m->units_load_finish_timestamp);
        }

        if (!switching_root)
                (void) serialize_environment(f, m->environment);

        if (m->notify_fd >= 0) {
                int copy;

                copy = fdset_put_dup(fds, m->notify_fd);
                if (copy < 0)
                        return copy;

                fprintf(f, "notify-fd=%i\n", copy);
                fprintf(f, "notify-socket=%s\n", m->notify_socket);
        }

        if (m->cgroups_agent_fd >= 0) {
                int copy;

                copy = fdset_put_dup(fds, m->cgroups_agent_fd);
                if (copy < 0)
                        return copy;

                fprintf(f, "cgroups-agent-fd=%i\n", copy);
        }

        if (m->user_lookup_fds[0] >= 0) {
                int copy0, copy1;

                copy0 = fdset_put_dup(fds, m->user_lookup_fds[0]);
                if (copy0 < 0)
                        return copy0;

                copy1 = fdset_put_dup(fds, m->user_lookup_fds[1]);
                if (copy1 < 0)
                        return copy1;

                fprintf(f, "user-lookup=%i %i\n", copy0, copy1);
        }

        bus_track_serialize(m->subscribed, f, "subscribed");

        r = dynamic_user_serialize(m, f, fds);
        if (r < 0)
                return r;

        manager_serialize_uid_refs(m, f);
        manager_serialize_gid_refs(m, f);

        fputc('\n', f);

        HASHMAP_FOREACH_KEY(u, t, m->units, i) {
                if (u->id != t)
                        continue;

                /* Start marker */
                fputs(u->id, f);
                fputc('\n', f);

                r = unit_serialize(u, f, fds, !switching_root);
                if (r < 0) {
                        m->n_reloading--;
                        return r;
                }
        }

        assert(m->n_reloading > 0);
        m->n_reloading--;

        if (ferror(f))
                return -EIO;

        r = bus_fdset_add_all(m, fds);
        if (r < 0)
                return r;

        return 0;
}

int manager_deserialize(Manager *m, FILE *f, FDSet *fds) {
        int r = 0;

        assert(m);
        assert(f);

        log_debug("Deserializing state...");

        m->n_reloading++;

        for (;;) {
                char line[LINE_MAX];
                const char *val, *l;

                if (!fgets(line, sizeof(line), f)) {
                        if (feof(f))
                                r = 0;
                        else
                                r = -errno;

                        goto finish;
                }

                char_array_0(line);
                l = strstrip(line);

                if (l[0] == 0)
                        break;

                if ((val = startswith(l, "current-job-id="))) {
                        uint32_t id;

                        if (safe_atou32(val, &id) < 0)
                                log_notice("Failed to parse current job id value %s", val);
                        else
                                m->current_job_id = MAX(m->current_job_id, id);

                } else if ((val = startswith(l, "n-installed-jobs="))) {
                        uint32_t n;

                        if (safe_atou32(val, &n) < 0)
                                log_notice("Failed to parse installed jobs counter %s", val);
                        else
                                m->n_installed_jobs += n;

                } else if ((val = startswith(l, "n-failed-jobs="))) {
                        uint32_t n;

                        if (safe_atou32(val, &n) < 0)
                                log_notice("Failed to parse failed jobs counter %s", val);
                        else
                                m->n_failed_jobs += n;

                } else if ((val = startswith(l, "taint-usr="))) {
                        int b;

                        b = parse_boolean(val);
                        if (b < 0)
                                log_notice("Failed to parse taint /usr flag %s", val);
                        else
                                m->taint_usr = m->taint_usr || b;

                } else if ((val = startswith(l, "firmware-timestamp=")))
                        dual_timestamp_deserialize(val, &m->firmware_timestamp);
                else if ((val = startswith(l, "loader-timestamp=")))
                        dual_timestamp_deserialize(val, &m->loader_timestamp);
                else if ((val = startswith(l, "kernel-timestamp=")))
                        dual_timestamp_deserialize(val, &m->kernel_timestamp);
                else if ((val = startswith(l, "initrd-timestamp=")))
                        dual_timestamp_deserialize(val, &m->initrd_timestamp);
                else if ((val = startswith(l, "userspace-timestamp=")))
                        dual_timestamp_deserialize(val, &m->userspace_timestamp);
                else if ((val = startswith(l, "finish-timestamp=")))
                        dual_timestamp_deserialize(val, &m->finish_timestamp);
                else if ((val = startswith(l, "security-start-timestamp=")))
                        dual_timestamp_deserialize(val, &m->security_start_timestamp);
                else if ((val = startswith(l, "security-finish-timestamp=")))
                        dual_timestamp_deserialize(val, &m->security_finish_timestamp);
                else if ((val = startswith(l, "generators-start-timestamp=")))
                        dual_timestamp_deserialize(val, &m->generators_start_timestamp);
                else if ((val = startswith(l, "generators-finish-timestamp=")))
                        dual_timestamp_deserialize(val, &m->generators_finish_timestamp);
                else if ((val = startswith(l, "units-load-start-timestamp=")))
                        dual_timestamp_deserialize(val, &m->units_load_start_timestamp);
                else if ((val = startswith(l, "units-load-finish-timestamp=")))
                        dual_timestamp_deserialize(val, &m->units_load_finish_timestamp);
                else if (startswith(l, "env=")) {
                        r = deserialize_environment(&m->environment, l);
                        if (r == -ENOMEM)
                                goto finish;
                        if (r < 0)
                                log_notice_errno(r, "Failed to parse environment entry: \"%s\": %m", l);

                } else if ((val = startswith(l, "notify-fd="))) {
                        int fd;

                        if (safe_atoi(val, &fd) < 0 || fd < 0 || !fdset_contains(fds, fd))
                                log_notice("Failed to parse notify fd: \"%s\"", val);
                        else {
                                m->notify_event_source = sd_event_source_unref(m->notify_event_source);
                                safe_close(m->notify_fd);
                                m->notify_fd = fdset_remove(fds, fd);
                        }

                } else if ((val = startswith(l, "notify-socket="))) {
                        char *n;

                        n = strdup(val);
                        if (!n) {
                                r = -ENOMEM;
                                goto finish;
                        }

                        free(m->notify_socket);
                        m->notify_socket = n;

                } else if ((val = startswith(l, "cgroups-agent-fd="))) {
                        int fd;

                        if (safe_atoi(val, &fd) < 0 || fd < 0 || !fdset_contains(fds, fd))
                                log_notice("Failed to parse cgroups agent fd: %s", val);
                        else {
                                m->cgroups_agent_event_source = sd_event_source_unref(m->cgroups_agent_event_source);
                                safe_close(m->cgroups_agent_fd);
                                m->cgroups_agent_fd = fdset_remove(fds, fd);
                        }

                } else if ((val = startswith(l, "user-lookup="))) {
                        int fd0, fd1;

                        if (sscanf(val, "%i %i", &fd0, &fd1) != 2 || fd0 < 0 || fd1 < 0 || fd0 == fd1 || !fdset_contains(fds, fd0) || !fdset_contains(fds, fd1))
                                log_notice("Failed to parse user lookup fd: %s", val);
                        else {
                                m->user_lookup_event_source = sd_event_source_unref(m->user_lookup_event_source);
                                safe_close_pair(m->user_lookup_fds);
                                m->user_lookup_fds[0] = fdset_remove(fds, fd0);
                                m->user_lookup_fds[1] = fdset_remove(fds, fd1);
                        }

                } else if ((val = startswith(l, "dynamic-user=")))
                        dynamic_user_deserialize_one(m, val, fds);
                else if ((val = startswith(l, "destroy-ipc-uid=")))
                        manager_deserialize_uid_refs_one(m, val);
                else if ((val = startswith(l, "destroy-ipc-gid=")))
                        manager_deserialize_gid_refs_one(m, val);
                else if ((val = startswith(l, "subscribed="))) {

                        if (strv_extend(&m->deserialized_subscribed, val) < 0)
                                log_oom();

                } else if (!startswith(l, "kdbus-fd=")) /* ignore this one */
                        log_notice("Unknown serialization item '%s'", l);
        }

        for (;;) {
                Unit *u;
                char name[UNIT_NAME_MAX+2];

                /* Start marker */
                if (!fgets(name, sizeof(name), f)) {
                        if (feof(f))
                                r = 0;
                        else
                                r = -errno;

                        goto finish;
                }

                char_array_0(name);

                r = manager_load_unit(m, strstrip(name), NULL, NULL, &u);
                if (r < 0)
                        goto finish;

                r = unit_deserialize(u, f, fds);
                if (r < 0)
                        goto finish;
        }

finish:
        if (ferror(f))
                r = -EIO;

        assert(m->n_reloading > 0);
        m->n_reloading--;

        return r;
}

int manager_reload(Manager *m) {
        int r, q;
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_fdset_free_ FDSet *fds = NULL;

        assert(m);

        r = manager_open_serialization(m, &f);
        if (r < 0)
                return r;

        m->n_reloading++;
        bus_manager_send_reloading(m, true);

        fds = fdset_new();
        if (!fds) {
                m->n_reloading--;
                return -ENOMEM;
        }

        r = manager_serialize(m, f, fds, false);
        if (r < 0) {
                m->n_reloading--;
                return r;
        }

        if (fseeko(f, 0, SEEK_SET) < 0) {
                m->n_reloading--;
                return -errno;
        }

        /* From here on there is no way back. */
        manager_clear_jobs_and_units(m);
        lookup_paths_flush_generator(&m->lookup_paths);
        lookup_paths_free(&m->lookup_paths);
        dynamic_user_vacuum(m, false);
        m->uid_refs = hashmap_free(m->uid_refs);
        m->gid_refs = hashmap_free(m->gid_refs);

        q = lookup_paths_init(&m->lookup_paths, m->unit_file_scope, 0, NULL);
        if (q < 0 && r >= 0)
                r = q;

        q = manager_run_environment_generators(m);
        if (q < 0 && r >= 0)
                r = q;

        /* Find new unit paths */
        q = manager_run_generators(m);
        if (q < 0 && r >= 0)
                r = q;

        lookup_paths_reduce(&m->lookup_paths);
        manager_build_unit_path_cache(m);

        /* First, enumerate what we can from all config files */
        manager_enumerate(m);

        /* Second, deserialize our stored data */
        q = manager_deserialize(m, f, fds);
        if (q < 0 && r >= 0)
                r = q;

        fclose(f);
        f = NULL;

        /* Re-register notify_fd as event source */
        q = manager_setup_notify(m);
        if (q < 0 && r >= 0)
                r = q;

        q = manager_setup_cgroups_agent(m);
        if (q < 0 && r >= 0)
                r = q;

        q = manager_setup_user_lookup_fd(m);
        if (q < 0 && r >= 0)
                r = q;

        /* Third, fire things up! */
        manager_coldplug(m);

        /* Release any dynamic users no longer referenced */
        dynamic_user_vacuum(m, true);

        /* Release any references to UIDs/GIDs no longer referenced, and destroy any IPC owned by them */
        manager_vacuum_uid_refs(m);
        manager_vacuum_gid_refs(m);

        /* Sync current state of bus names with our set of listening units */
        if (m->api_bus)
                manager_sync_bus_names(m, m->api_bus);

        assert(m->n_reloading > 0);
        m->n_reloading--;

        m->send_reloading_done = true;

        return r;
}

void manager_reset_failed(Manager *m) {
        Unit *u;
        Iterator i;

        assert(m);

        HASHMAP_FOREACH(u, m->units, i)
                unit_reset_failed(u);
}

bool manager_unit_inactive_or_pending(Manager *m, const char *name) {
        Unit *u;

        assert(m);
        assert(name);

        /* Returns true if the unit is inactive or going down */
        u = manager_get_unit(m, name);
        if (!u)
                return true;

        return unit_inactive_or_pending(u);
}

static void manager_notify_finished(Manager *m) {
        char userspace[FORMAT_TIMESPAN_MAX], initrd[FORMAT_TIMESPAN_MAX], kernel[FORMAT_TIMESPAN_MAX], sum[FORMAT_TIMESPAN_MAX];
        usec_t firmware_usec, loader_usec, kernel_usec, initrd_usec, userspace_usec, total_usec;

        if (m->test_run)
                return;

        if (MANAGER_IS_SYSTEM(m) && detect_container() <= 0) {

                /* Note that m->kernel_usec.monotonic is always at 0,
                 * and m->firmware_usec.monotonic and
                 * m->loader_usec.monotonic should be considered
                 * negative values. */

                firmware_usec = m->firmware_timestamp.monotonic - m->loader_timestamp.monotonic;
                loader_usec = m->loader_timestamp.monotonic - m->kernel_timestamp.monotonic;
                userspace_usec = m->finish_timestamp.monotonic - m->userspace_timestamp.monotonic;
                total_usec = m->firmware_timestamp.monotonic + m->finish_timestamp.monotonic;

                if (dual_timestamp_is_set(&m->initrd_timestamp)) {

                        kernel_usec = m->initrd_timestamp.monotonic - m->kernel_timestamp.monotonic;
                        initrd_usec = m->userspace_timestamp.monotonic - m->initrd_timestamp.monotonic;

                        log_struct(LOG_INFO,
                                   "MESSAGE_ID=" SD_MESSAGE_STARTUP_FINISHED_STR,
                                   "KERNEL_USEC="USEC_FMT, kernel_usec,
                                   "INITRD_USEC="USEC_FMT, initrd_usec,
                                   "USERSPACE_USEC="USEC_FMT, userspace_usec,
                                   LOG_MESSAGE("Startup finished in %s (kernel) + %s (initrd) + %s (userspace) = %s.",
                                               format_timespan(kernel, sizeof(kernel), kernel_usec, USEC_PER_MSEC),
                                               format_timespan(initrd, sizeof(initrd), initrd_usec, USEC_PER_MSEC),
                                               format_timespan(userspace, sizeof(userspace), userspace_usec, USEC_PER_MSEC),
                                               format_timespan(sum, sizeof(sum), total_usec, USEC_PER_MSEC)),
                                   NULL);
                } else {
                        kernel_usec = m->userspace_timestamp.monotonic - m->kernel_timestamp.monotonic;
                        initrd_usec = 0;

                        log_struct(LOG_INFO,
                                   "MESSAGE_ID=" SD_MESSAGE_STARTUP_FINISHED_STR,
                                   "KERNEL_USEC="USEC_FMT, kernel_usec,
                                   "USERSPACE_USEC="USEC_FMT, userspace_usec,
                                   LOG_MESSAGE("Startup finished in %s (kernel) + %s (userspace) = %s.",
                                               format_timespan(kernel, sizeof(kernel), kernel_usec, USEC_PER_MSEC),
                                               format_timespan(userspace, sizeof(userspace), userspace_usec, USEC_PER_MSEC),
                                               format_timespan(sum, sizeof(sum), total_usec, USEC_PER_MSEC)),
                                   NULL);
                }
        } else {
                firmware_usec = loader_usec = initrd_usec = kernel_usec = 0;
                total_usec = userspace_usec = m->finish_timestamp.monotonic - m->userspace_timestamp.monotonic;

                log_struct(LOG_INFO,
                           "MESSAGE_ID=" SD_MESSAGE_USER_STARTUP_FINISHED_STR,
                           "USERSPACE_USEC="USEC_FMT, userspace_usec,
                           LOG_MESSAGE("Startup finished in %s.",
                                       format_timespan(sum, sizeof(sum), total_usec, USEC_PER_MSEC)),
                           NULL);
        }

        bus_manager_send_finished(m, firmware_usec, loader_usec, kernel_usec, initrd_usec, userspace_usec, total_usec);

        sd_notifyf(false,
                   "READY=1\n"
                   "STATUS=Startup finished in %s.",
                   format_timespan(sum, sizeof(sum), total_usec, USEC_PER_MSEC));
}

void manager_check_finished(Manager *m) {
        assert(m);

        if (MANAGER_IS_RELOADING(m))
                return;

        /* Verify that we are actually running currently. Initially
         * the exit code is set to invalid, and during operation it is
         * then set to MANAGER_OK */
        if (m->exit_code != MANAGER_OK)
                return;

        if (hashmap_size(m->jobs) > 0) {
                if (m->jobs_in_progress_event_source)
                        /* Ignore any failure, this is only for feedback */
                        (void) sd_event_source_set_time(m->jobs_in_progress_event_source, now(CLOCK_MONOTONIC) + JOBS_IN_PROGRESS_WAIT_USEC);

                return;
        }

        manager_flip_auto_status(m, false);

        /* Notify Type=idle units that we are done now */
        manager_close_idle_pipe(m);

        /* Turn off confirm spawn now */
        m->confirm_spawn = NULL;

        /* No need to update ask password status when we're going non-interactive */
        manager_close_ask_password(m);

        /* This is no longer the first boot */
        manager_set_first_boot(m, false);

        if (dual_timestamp_is_set(&m->finish_timestamp))
                return;

        dual_timestamp_get(&m->finish_timestamp);

        manager_notify_finished(m);

        manager_invalidate_startup_units(m);
}

static bool generator_path_any(const char* const* paths) {
        char **path;
        bool found = false;

        /* Optimize by skipping the whole process by not creating output directories
         * if no generators are found. */
        STRV_FOREACH(path, (char**) paths)
                if (access(*path, F_OK) == 0)
                        found = true;
                else if (errno != ENOENT)
                        log_warning_errno(errno, "Failed to open generator directory %s: %m", *path);

        return found;
}

static const char* system_env_generator_binary_paths[] = {
        "/run/systemd/system-environment-generators",
        "/etc/systemd/system-environment-generators",
        "/usr/local/lib/systemd/system-environment-generators",
        SYSTEM_ENV_GENERATOR_PATH,
        NULL
};

static const char* user_env_generator_binary_paths[] = {
        "/run/systemd/user-environment-generators",
        "/etc/systemd/user-environment-generators",
        "/usr/local/lib/systemd/user-environment-generators",
        USER_ENV_GENERATOR_PATH,
        NULL
};

static int manager_run_environment_generators(Manager *m) {
        char **tmp = NULL; /* this is only used in the forked process, no cleanup here */
        const char **paths;
        void* args[] = {&tmp, &tmp, &m->environment};

        if (m->test_run)
                return 0;

        paths = MANAGER_IS_SYSTEM(m) ? system_env_generator_binary_paths : user_env_generator_binary_paths;

        if (!generator_path_any(paths))
                return 0;

        return execute_directories(paths, DEFAULT_TIMEOUT_USEC, gather_environment, args, NULL);
}

static int manager_run_generators(Manager *m) {
        _cleanup_strv_free_ char **paths = NULL;
        const char *argv[5];
        int r;

        assert(m);

        if (m->test_run)
                return 0;

        paths = generator_binary_paths(m->unit_file_scope);
        if (!paths)
                return log_oom();

        if (!generator_path_any((const char* const*) paths))
                return 0;

        r = lookup_paths_mkdir_generator(&m->lookup_paths);
        if (r < 0)
                goto finish;

        argv[0] = NULL; /* Leave this empty, execute_directory() will fill something in */
        argv[1] = m->lookup_paths.generator;
        argv[2] = m->lookup_paths.generator_early;
        argv[3] = m->lookup_paths.generator_late;
        argv[4] = NULL;

        RUN_WITH_UMASK(0022)
                execute_directories((const char* const*) paths, DEFAULT_TIMEOUT_USEC,
                                    NULL, NULL, (char**) argv);

finish:
        lookup_paths_trim_generator(&m->lookup_paths);
        return r;
}

int manager_environment_add(Manager *m, char **minus, char **plus) {
        char **a = NULL, **b = NULL, **l;
        assert(m);

        l = m->environment;

        if (!strv_isempty(minus)) {
                a = strv_env_delete(l, 1, minus);
                if (!a)
                        return -ENOMEM;

                l = a;
        }

        if (!strv_isempty(plus)) {
                b = strv_env_merge(2, l, plus);
                if (!b) {
                        strv_free(a);
                        return -ENOMEM;
                }

                l = b;
        }

        if (m->environment != l)
                strv_free(m->environment);
        if (a != l)
                strv_free(a);
        if (b != l)
                strv_free(b);

        m->environment = l;
        manager_clean_environment(m);
        strv_sort(m->environment);

        return 0;
}

int manager_set_default_rlimits(Manager *m, struct rlimit **default_rlimit) {
        int i;

        assert(m);

        for (i = 0; i < _RLIMIT_MAX; i++) {
                m->rlimit[i] = mfree(m->rlimit[i]);

                if (!default_rlimit[i])
                        continue;

                m->rlimit[i] = newdup(struct rlimit, default_rlimit[i], 1);
                if (!m->rlimit[i])
                        return log_oom();
        }

        return 0;
}

void manager_recheck_journal(Manager *m) {
        Unit *u;

        assert(m);

        if (!MANAGER_IS_SYSTEM(m))
                return;

        u = manager_get_unit(m, SPECIAL_JOURNALD_SOCKET);
        if (u && SOCKET(u)->state != SOCKET_RUNNING) {
                log_close_journal();
                return;
        }

        u = manager_get_unit(m, SPECIAL_JOURNALD_SERVICE);
        if (u && SERVICE(u)->state != SERVICE_RUNNING) {
                log_close_journal();
                return;
        }

        /* Hmm, OK, so the socket is fully up and the service is up
         * too, then let's make use of the thing. */
        log_open();
}

void manager_set_show_status(Manager *m, ShowStatus mode) {
        assert(m);
        assert(IN_SET(mode, SHOW_STATUS_AUTO, SHOW_STATUS_NO, SHOW_STATUS_YES, SHOW_STATUS_TEMPORARY));

        if (!MANAGER_IS_SYSTEM(m))
                return;

        if (m->show_status != mode)
                log_debug("%s showing of status.",
                          mode == SHOW_STATUS_NO ? "Disabling" : "Enabling");
        m->show_status = mode;

        if (mode > 0)
                (void) touch("/run/systemd/show-status");
        else
                (void) unlink("/run/systemd/show-status");
}

static bool manager_get_show_status(Manager *m, StatusType type) {
        assert(m);

        if (!MANAGER_IS_SYSTEM(m))
                return false;

        if (m->no_console_output)
                return false;

        if (!IN_SET(manager_state(m), MANAGER_INITIALIZING, MANAGER_STARTING, MANAGER_STOPPING))
                return false;

        /* If we cannot find out the status properly, just proceed. */
        if (type != STATUS_TYPE_EMERGENCY && manager_check_ask_password(m) > 0)
                return false;

        if (m->show_status > 0)
                return true;

        return false;
}

const char *manager_get_confirm_spawn(Manager *m) {
        static int last_errno = 0;
        const char *vc = m->confirm_spawn;
        struct stat st;
        int r;

        /* Here's the deal: we want to test the validity of the console but don't want
         * PID1 to go through the whole console process which might block. But we also
         * want to warn the user only once if something is wrong with the console so we
         * cannot do the sanity checks after spawning our children. So here we simply do
         * really basic tests to hopefully trap common errors.
         *
         * If the console suddenly disappear at the time our children will really it
         * then they will simply fail to acquire it and a positive answer will be
         * assumed. New children will fallback to /dev/console though.
         *
         * Note: TTYs are devices that can come and go any time, and frequently aren't
         * available yet during early boot (consider a USB rs232 dongle...). If for any
         * reason the configured console is not ready, we fallback to the default
         * console. */

        if (!vc || path_equal(vc, "/dev/console"))
                return vc;

        r = stat(vc, &st);
        if (r < 0)
                goto fail;

        if (!S_ISCHR(st.st_mode)) {
                errno = ENOTTY;
                goto fail;
        }

        last_errno = 0;
        return vc;
fail:
        if (last_errno != errno) {
                last_errno = errno;
                log_warning_errno(errno, "Failed to open %s: %m, using default console", vc);
        }
        return "/dev/console";
}

void manager_set_first_boot(Manager *m, bool b) {
        assert(m);

        if (!MANAGER_IS_SYSTEM(m))
                return;

        if (m->first_boot != (int) b) {
                if (b)
                        (void) touch("/run/systemd/first-boot");
                else
                        (void) unlink("/run/systemd/first-boot");
        }

        m->first_boot = b;
}

void manager_disable_confirm_spawn(void) {
        (void) touch("/run/systemd/confirm_spawn_disabled");
}

bool manager_is_confirm_spawn_disabled(Manager *m) {
        if (!m->confirm_spawn)
                return true;

        return access("/run/systemd/confirm_spawn_disabled", F_OK) >= 0;
}

void manager_status_printf(Manager *m, StatusType type, const char *status, const char *format, ...) {
        va_list ap;

        /* If m is NULL, assume we're after shutdown and let the messages through. */

        if (m && !manager_get_show_status(m, type))
                return;

        /* XXX We should totally drop the check for ephemeral here
         * and thus effectively make 'Type=idle' pointless. */
        if (type == STATUS_TYPE_EPHEMERAL && m && m->n_on_console > 0)
                return;

        va_start(ap, format);
        status_vprintf(status, true, type == STATUS_TYPE_EPHEMERAL, format, ap);
        va_end(ap);
}

Set *manager_get_units_requiring_mounts_for(Manager *m, const char *path) {
        char p[strlen(path)+1];

        assert(m);
        assert(path);

        strcpy(p, path);
        path_kill_slashes(p);

        return hashmap_get(m->units_requiring_mounts_for, streq(p, "/") ? "" : p);
}

const char *manager_get_runtime_prefix(Manager *m) {
        assert(m);

        return MANAGER_IS_SYSTEM(m) ?
               "/run" :
               getenv("XDG_RUNTIME_DIR");
}

int manager_update_failed_units(Manager *m, Unit *u, bool failed) {
        unsigned size;
        int r;

        assert(m);
        assert(u->manager == m);

        size = set_size(m->failed_units);

        if (failed) {
                r = set_ensure_allocated(&m->failed_units, NULL);
                if (r < 0)
                        return log_oom();

                if (set_put(m->failed_units, u) < 0)
                        return log_oom();
        } else
                (void) set_remove(m->failed_units, u);

        if (set_size(m->failed_units) != size)
                bus_manager_send_change_signal(m);

        return 0;
}

ManagerState manager_state(Manager *m) {
        Unit *u;

        assert(m);

        /* Did we ever finish booting? If not then we are still starting up */
        if (!dual_timestamp_is_set(&m->finish_timestamp)) {

                u = manager_get_unit(m, SPECIAL_BASIC_TARGET);
                if (!u || !UNIT_IS_ACTIVE_OR_RELOADING(unit_active_state(u)))
                        return MANAGER_INITIALIZING;

                return MANAGER_STARTING;
        }

        /* Is the special shutdown target queued? If so, we are in shutdown state */
        u = manager_get_unit(m, SPECIAL_SHUTDOWN_TARGET);
        if (u && u->job && IN_SET(u->job->type, JOB_START, JOB_RESTART, JOB_RELOAD_OR_START))
                return MANAGER_STOPPING;

        /* Are the rescue or emergency targets active or queued? If so we are in maintenance state */
        u = manager_get_unit(m, SPECIAL_RESCUE_TARGET);
        if (u && (UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(u)) ||
                  (u->job && IN_SET(u->job->type, JOB_START, JOB_RESTART, JOB_RELOAD_OR_START))))
                return MANAGER_MAINTENANCE;

        u = manager_get_unit(m, SPECIAL_EMERGENCY_TARGET);
        if (u && (UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(u)) ||
                  (u->job && IN_SET(u->job->type, JOB_START, JOB_RESTART, JOB_RELOAD_OR_START))))
                return MANAGER_MAINTENANCE;

        /* Are there any failed units? If so, we are in degraded mode */
        if (set_size(m->failed_units) > 0)
                return MANAGER_DEGRADED;

        return MANAGER_RUNNING;
}

#define DESTROY_IPC_FLAG (UINT32_C(1) << 31)

static void manager_unref_uid_internal(
                Manager *m,
                Hashmap **uid_refs,
                uid_t uid,
                bool destroy_now,
                int (*_clean_ipc)(uid_t uid)) {

        uint32_t c, n;

        assert(m);
        assert(uid_refs);
        assert(uid_is_valid(uid));
        assert(_clean_ipc);

        /* A generic implementation, covering both manager_unref_uid() and manager_unref_gid(), under the assumption
         * that uid_t and gid_t are actually defined the same way, with the same validity rules.
         *
         * We store a hashmap where the UID/GID is they key and the value is a 32bit reference counter, whose highest
         * bit is used as flag for marking UIDs/GIDs whose IPC objects to remove when the last reference to the UID/GID
         * is dropped. The flag is set to on, once at least one reference from a unit where RemoveIPC= is set is added
         * on a UID/GID. It is reset when the UID's/GID's reference counter drops to 0 again. */

        assert_cc(sizeof(uid_t) == sizeof(gid_t));
        assert_cc(UID_INVALID == (uid_t) GID_INVALID);

        if (uid == 0) /* We don't keep track of root, and will never destroy it */
                return;

        c = PTR_TO_UINT32(hashmap_get(*uid_refs, UID_TO_PTR(uid)));

        n = c & ~DESTROY_IPC_FLAG;
        assert(n > 0);
        n--;

        if (destroy_now && n == 0) {
                hashmap_remove(*uid_refs, UID_TO_PTR(uid));

                if (c & DESTROY_IPC_FLAG) {
                        log_debug("%s " UID_FMT " is no longer referenced, cleaning up its IPC.",
                                  _clean_ipc == clean_ipc_by_uid ? "UID" : "GID",
                                  uid);
                        (void) _clean_ipc(uid);
                }
        } else {
                c = n | (c & DESTROY_IPC_FLAG);
                assert_se(hashmap_update(*uid_refs, UID_TO_PTR(uid), UINT32_TO_PTR(c)) >= 0);
        }
}

void manager_unref_uid(Manager *m, uid_t uid, bool destroy_now) {
        manager_unref_uid_internal(m, &m->uid_refs, uid, destroy_now, clean_ipc_by_uid);
}

void manager_unref_gid(Manager *m, gid_t gid, bool destroy_now) {
        manager_unref_uid_internal(m, &m->gid_refs, (uid_t) gid, destroy_now, clean_ipc_by_gid);
}

static int manager_ref_uid_internal(
                Manager *m,
                Hashmap **uid_refs,
                uid_t uid,
                bool clean_ipc) {

        uint32_t c, n;
        int r;

        assert(m);
        assert(uid_refs);
        assert(uid_is_valid(uid));

        /* A generic implementation, covering both manager_ref_uid() and manager_ref_gid(), under the assumption
         * that uid_t and gid_t are actually defined the same way, with the same validity rules. */

        assert_cc(sizeof(uid_t) == sizeof(gid_t));
        assert_cc(UID_INVALID == (uid_t) GID_INVALID);

        if (uid == 0) /* We don't keep track of root, and will never destroy it */
                return 0;

        r = hashmap_ensure_allocated(uid_refs, &trivial_hash_ops);
        if (r < 0)
                return r;

        c = PTR_TO_UINT32(hashmap_get(*uid_refs, UID_TO_PTR(uid)));

        n = c & ~DESTROY_IPC_FLAG;
        n++;

        if (n & DESTROY_IPC_FLAG) /* check for overflow */
                return -EOVERFLOW;

        c = n | (c & DESTROY_IPC_FLAG) | (clean_ipc ? DESTROY_IPC_FLAG : 0);

        return hashmap_replace(*uid_refs, UID_TO_PTR(uid), UINT32_TO_PTR(c));
}

int manager_ref_uid(Manager *m, uid_t uid, bool clean_ipc) {
        return manager_ref_uid_internal(m, &m->uid_refs, uid, clean_ipc);
}

int manager_ref_gid(Manager *m, gid_t gid, bool clean_ipc) {
        return manager_ref_uid_internal(m, &m->gid_refs, (uid_t) gid, clean_ipc);
}

static void manager_vacuum_uid_refs_internal(
                Manager *m,
                Hashmap **uid_refs,
                int (*_clean_ipc)(uid_t uid)) {

        Iterator i;
        void *p, *k;

        assert(m);
        assert(uid_refs);
        assert(_clean_ipc);

        HASHMAP_FOREACH_KEY(p, k, *uid_refs, i) {
                uint32_t c, n;
                uid_t uid;

                uid = PTR_TO_UID(k);
                c = PTR_TO_UINT32(p);

                n = c & ~DESTROY_IPC_FLAG;
                if (n > 0)
                        continue;

                if (c & DESTROY_IPC_FLAG) {
                        log_debug("Found unreferenced %s " UID_FMT " after reload/reexec. Cleaning up.",
                                  _clean_ipc == clean_ipc_by_uid ? "UID" : "GID",
                                  uid);
                        (void) _clean_ipc(uid);
                }

                assert_se(hashmap_remove(*uid_refs, k) == p);
        }
}

void manager_vacuum_uid_refs(Manager *m) {
        manager_vacuum_uid_refs_internal(m, &m->uid_refs, clean_ipc_by_uid);
}

void manager_vacuum_gid_refs(Manager *m) {
        manager_vacuum_uid_refs_internal(m, &m->gid_refs, clean_ipc_by_gid);
}

static void manager_serialize_uid_refs_internal(
                Manager *m,
                FILE *f,
                Hashmap **uid_refs,
                const char *field_name) {

        Iterator i;
        void *p, *k;

        assert(m);
        assert(f);
        assert(uid_refs);
        assert(field_name);

        /* Serialize the UID reference table. Or actually, just the IPC destruction flag of it, as the actual counter
         * of it is better rebuild after a reload/reexec. */

        HASHMAP_FOREACH_KEY(p, k, *uid_refs, i) {
                uint32_t c;
                uid_t uid;

                uid = PTR_TO_UID(k);
                c = PTR_TO_UINT32(p);

                if (!(c & DESTROY_IPC_FLAG))
                        continue;

                fprintf(f, "%s=" UID_FMT "\n", field_name, uid);
        }
}

void manager_serialize_uid_refs(Manager *m, FILE *f) {
        manager_serialize_uid_refs_internal(m, f, &m->uid_refs, "destroy-ipc-uid");
}

void manager_serialize_gid_refs(Manager *m, FILE *f) {
        manager_serialize_uid_refs_internal(m, f, &m->gid_refs, "destroy-ipc-gid");
}

static void manager_deserialize_uid_refs_one_internal(
                Manager *m,
                Hashmap** uid_refs,
                const char *value) {

        uid_t uid;
        uint32_t c;
        int r;

        assert(m);
        assert(uid_refs);
        assert(value);

        r = parse_uid(value, &uid);
        if (r < 0 || uid == 0) {
                log_debug("Unable to parse UID reference serialization");
                return;
        }

        r = hashmap_ensure_allocated(uid_refs, &trivial_hash_ops);
        if (r < 0) {
                log_oom();
                return;
        }

        c = PTR_TO_UINT32(hashmap_get(*uid_refs, UID_TO_PTR(uid)));
        if (c & DESTROY_IPC_FLAG)
                return;

        c |= DESTROY_IPC_FLAG;

        r = hashmap_replace(*uid_refs, UID_TO_PTR(uid), UINT32_TO_PTR(c));
        if (r < 0) {
                log_debug("Failed to add UID reference entry");
                return;
        }
}

void manager_deserialize_uid_refs_one(Manager *m, const char *value) {
        manager_deserialize_uid_refs_one_internal(m, &m->uid_refs, value);
}

void manager_deserialize_gid_refs_one(Manager *m, const char *value) {
        manager_deserialize_uid_refs_one_internal(m, &m->gid_refs, value);
}

int manager_dispatch_user_lookup_fd(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        struct buffer {
                uid_t uid;
                gid_t gid;
                char unit_name[UNIT_NAME_MAX+1];
        } _packed_ buffer;

        Manager *m = userdata;
        ssize_t l;
        size_t n;
        Unit *u;

        assert_se(source);
        assert_se(m);

        /* Invoked whenever a child process succeeded resolving its user/group to use and sent us the resulting UID/GID
         * in a datagram. We parse the datagram here and pass it off to the unit, so that it can add a reference to the
         * UID/GID so that it can destroy the UID/GID's IPC objects when the reference counter drops to 0. */

        l = recv(fd, &buffer, sizeof(buffer), MSG_DONTWAIT);
        if (l < 0) {
                if (errno == EINTR || errno == EAGAIN)
                        return 0;

                return log_error_errno(errno, "Failed to read from user lookup fd: %m");
        }

        if ((size_t) l <= offsetof(struct buffer, unit_name)) {
                log_warning("Received too short user lookup message, ignoring.");
                return 0;
        }

        if ((size_t) l > offsetof(struct buffer, unit_name) + UNIT_NAME_MAX) {
                log_warning("Received too long user lookup message, ignoring.");
                return 0;
        }

        if (!uid_is_valid(buffer.uid) && !gid_is_valid(buffer.gid)) {
                log_warning("Got user lookup message with invalid UID/GID pair, ignoring.");
                return 0;
        }

        n = (size_t) l - offsetof(struct buffer, unit_name);
        if (memchr(buffer.unit_name, 0, n)) {
                log_warning("Received lookup message with embedded NUL character, ignoring.");
                return 0;
        }

        buffer.unit_name[n] = 0;
        u = manager_get_unit(m, buffer.unit_name);
        if (!u) {
                log_debug("Got user lookup message but unit doesn't exist, ignoring.");
                return 0;
        }

        log_unit_debug(u, "User lookup succeeded: uid=" UID_FMT " gid=" GID_FMT, buffer.uid, buffer.gid);

        unit_notify_user_lookup(u, buffer.uid, buffer.gid);
        return 0;
}

static const char *const manager_state_table[_MANAGER_STATE_MAX] = {
        [MANAGER_INITIALIZING] = "initializing",
        [MANAGER_STARTING] = "starting",
        [MANAGER_RUNNING] = "running",
        [MANAGER_DEGRADED] = "degraded",
        [MANAGER_MAINTENANCE] = "maintenance",
        [MANAGER_STOPPING] = "stopping",
};

DEFINE_STRING_TABLE_LOOKUP(manager_state, ManagerState);
