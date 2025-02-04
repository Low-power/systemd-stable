/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2012 Lennart Poettering

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

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include "mkdir.h"
#include "log.h"
#include "fileio.h"
#include "unit-name.h"
#include "special.h"
#include "path-util.h"
#include "fd-util.h"
#include "dirent-util.h"
#include "util.h"
#include "strv.h"

static const char *arg_dest = "/tmp";

static char *sysv_translate_name(const char *name) {
        char *r;

        r = new(char, strlen(name) + sizeof(".service"));
        if (!r)
                return NULL;

        if (endswith(name, ".sh"))
                /* Drop .sh suffix */
                strcpy(stpcpy(r, name) - 3, ".service");
        else
                /* Normal init script name */
                strcpy(stpcpy(r, name), ".service");

        return r;
}

static int sysv_translate_facility(const char *name, const char *filename, char **_r) {

        /* We silently ignore the $ prefix here. According to the LSB
         * spec it simply indicates whether something is a
         * standardized name or a distribution-specific one. Since we
         * just follow what already exists and do not introduce new
         * uses or names we don't care who introduced a new name. */

        static const char * const table[] = {
                /* LSB defined facilities */
                "local_fs",             SPECIAL_LOCAL_FS_TARGET,
                "network",              SPECIAL_NETWORK_TARGET,
                "named",                SPECIAL_NSS_LOOKUP_TARGET,
                "portmap",              SPECIAL_RPCBIND_TARGET,
                "remote_fs",            SPECIAL_REMOTE_FS_TARGET,
                "syslog",               NULL,
                "time",                 SPECIAL_TIME_SYNC_TARGET,
                /* Debian defined facilities */
                "x-display-manager",    "display-manager.service",
        };

        unsigned i;
        char *r;
        const char *n;

        assert(name);
        assert(_r);

        n = *name == '$' ? name + 1 : name;

        for (i = 0; i < ELEMENTSOF(table); i += 2) {

                if (!streq(table[i], n))
                        continue;

                if (!table[i+1])
                        return 0;

                r = strdup(table[i+1]);
                if (!r)
                        return log_oom();

                goto finish;
        }

        /* If we don't know this name, fallback heuristics to figure
         * out whether something is a target or a service alias. */

        if (*name == '$') {
                if (!unit_prefix_is_valid(n))
                        return -EINVAL;

                /* Facilities starting with $ are most likely targets */
                if (unit_name_build(n, NULL, ".target", &r) < 0) {
                        r = NULL;
                }
        } else if (filename && streq(name, filename))
                /* Names equaling the file name of the services are redundant */
                return 0;
        else
                /* Everything else we assume to be normal service names */
                r = sysv_translate_name(n);

        if (!r)
                return -ENOMEM;

finish:
        *_r = r;

        return 1;
}



static int parse_insserv_conf(const char* filename) {
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        if (!(f = fopen(filename, "re"))) {
                log_debug("Failed to open file %s", filename);
                r = errno == ENOENT ? 0 : -errno;
                return r;
        }

        while (!feof(f)) {
                char l[LINE_MAX], *t;
                _cleanup_strv_free_ char **parsed = NULL;

                if (!fgets(l, sizeof(l), f)) {
                        if (feof(f))
                                break;

                        r = -errno;
                        log_error("Failed to read configuration file '%s': %s", filename, strerror(-r));
                        return -r;
                }

                t = strstrip(l);
                if (*t != '$' && *t != '<')
                        continue;

                parsed = strv_split(t,WHITESPACE);
                /* we ignore <interactive>, not used, equivalent to X-Interactive */
                if (parsed && !startswith_no_case (parsed[0], "<interactive>")) {
                        _cleanup_free_ char *facility = NULL;
                        if (sysv_translate_facility(parsed[0], NULL, &facility) < 0 || !facility)
                                continue;
                        if (streq(facility, SPECIAL_REMOTE_FS_TARGET)) {
                                _cleanup_free_ char *unit = NULL;
                                /* insert also a Wants dependency from remote-fs-pre on remote-fs */
                                unit = strjoin(arg_dest, "/remote-fs.target.d/50-",basename(filename),".conf", NULL);
                                if (!unit)
                                        return log_oom();

                                mkdir_parents_label(unit, 0755);

                                r = write_string_file(unit,
                                                "# Automatically generated by systemd-insserv-generator\n\n"
                                                "[Unit]\n"
                                                "Wants=remote-fs-pre.target\n",
                                                WRITE_STRING_FILE_CREATE);
                                if (r)
                                        return r;
                                free (facility);
                                facility=strdup(SPECIAL_REMOTE_FS_PRE_TARGET);
                        }
                        if (facility && endswith(facility, ".target")) {
                                char *name, **j;
                                FILE *file = NULL;

                                STRV_FOREACH (j, parsed+1) {
                                        _cleanup_free_ char *unit = NULL;
                                        _cleanup_free_ char *dep = NULL;
                                        _cleanup_free_ char *initscript = NULL;

                                        /* targets should not pull in and activate other targets so skip them */
                                        if (*j[0] == '$')
                                                continue;

                                        if (*j[0] == '+')
                                                name = *j+1;
                                        else
                                                name = *j;
                                        if ((sysv_translate_facility(name, NULL, &dep) < 0) || !dep)
                                                continue;

                                        /* don't create any drop-in configs if the
                                         * corresponding SysV init script does not exist */
                                        initscript = strjoin("/etc/init.d/", name, NULL);
                                        if (access(initscript, F_OK) < 0) {
                                                _cleanup_free_ char *initscript_sh = NULL;

                                                /* Try *.sh source'able init scripts */
                                                initscript_sh = strjoin(initscript, ".sh", NULL);
                                                if (access(initscript_sh, F_OK) < 0) {
                                                        continue;
                                                }
                                        }

                                        unit = strjoin(arg_dest, "/", dep, ".d/50-",basename(filename),"-",parsed[0],".conf", NULL);
                                        if (!unit)
                                                return log_oom();

                                        mkdir_parents_label(unit, 0755);

                                        file = fopen(unit, "wxe");
                                        if (!file) {
                                                if (errno == EEXIST)
                                                        log_error("Failed to create drop-in file %s", unit);
                                                else
                                                        log_error("Failed to create drop-in file %s: %m", unit);
                                                return -errno;
                                        }

                                        fprintf(file,
                                                        "# Automatically generated by systemd-insserv-generator\n\n"
                                                        "[Unit]\n"
                                                        "Wants=%s\n"
                                                        "Before=%s\n",
                                                        facility, facility);

                                        fflush(file);
                                        if (ferror(file)) {
                                                log_error("Failed to write unit file %s: %m", unit);
                                                return -errno;
                                        }
                                        fclose(file);

                                        if (*j[0] != '+') {
                                                free (unit);
                                                unit = strjoin(arg_dest, "/", facility, ".d/50-hard-dependency-",basename(filename),"-",parsed[0],".conf", NULL);
                                                if (!unit)
                                                        return log_oom();

                                                mkdir_parents_label(unit, 0755);

                                                file = fopen(unit, "wxe");
                                                if (!file) {
                                                        if (errno == EEXIST)
                                                                log_error("Failed to create drop-in file %s, as it already exists", unit);
                                                        else
                                                                log_error("Failed to create drop-in file %s: %m", unit);
                                                        return -errno;
                                                }


                                                fprintf(file,
                                                                "# Automatically generated by systemd-insserv-generator\n\n"
                                                                "[Unit]\n"
                                                                "SourcePath=%s\n"
                                                                "Requires=%s\n",
                                                                filename, dep);
                                                fflush(file);
                                                if (ferror(file)) {
                                                        log_error("Failed to write unit file %s: %m", unit);
                                                        return -errno;
                                                }
                                                fclose(file);
                                        }
                                }
                        }
                }
        }
        return r;
}

static int parse_insserv(void) {
        DIR *d = NULL;
        struct dirent *de;
        int r = 0;

        if (!(d = opendir("/etc/insserv.conf.d/"))) {
                if (errno != ENOENT) {
                        log_debug("opendir() failed on /etc/insserv.conf.d/ %s", strerror(errno));
                }
        } else {
                FOREACH_DIRENT(de, d, break) {
                        char *path = strjoin("/etc/insserv.conf.d/", de->d_name, NULL);
                        parse_insserv_conf(path);
                        free(path);
                }
                closedir (d);
        }

        r = parse_insserv_conf("/etc/insserv.conf");

        return r;
}

int main(int argc, char *argv[]) {
        int r = 0;

        if (argc > 1 && argc != 4) {
                log_error("This program takes three or no arguments.");
                return EXIT_FAILURE;
        }

        if (argc > 1)
                arg_dest = argv[1];

        log_set_target(LOG_TARGET_SAFE);
        log_parse_environment();
        log_open();

        umask(0022);

        r = parse_insserv();

        return (r < 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
