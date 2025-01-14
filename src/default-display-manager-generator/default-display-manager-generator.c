/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Canonical

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
#include <stdbool.h>
#include <unistd.h>

#include "alloc-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "log.h"
#include "mkdir.h"
#include "string-util.h"
#include "util.h"


/*
 * Ensure that the started display-manager is matching
 * /etc/X11/default-display-manager if present, otherwise
 * let display-manager.service symlinks pick the preferred one
 * if any.
 */

static const char *dm_service_unit = SYSTEM_CONFIG_UNIT_PATH "/display-manager.service";
static const char *default_dm_file = "/etc/X11/default-display-manager";
static const char *dest = NULL;

static int generate_display_manager_alias(void) {

        _cleanup_free_ char *default_dm_path = NULL, *enabled_dm_unit = NULL;
        const char *default_dm = NULL, *in_mem_symlink = NULL, *target_unit_path = NULL;
        bool dm_service_exists = true;
        int r;

        r = read_full_file(default_dm_file, &default_dm_path, NULL);
        if (r < 0) {
                log_debug("No %s file, nothing to generate", default_dm_file);
                return 0;
        }
        default_dm = strstrip(basename(default_dm_path));

        r = readlink_value(dm_service_unit, &enabled_dm_unit);
        if (r < 0) {
                enabled_dm_unit = strdup("");
                dm_service_exists = false;
        }

        /* all is fine if the info matches */
        if (streq(strjoina(default_dm, ".service"), enabled_dm_unit))
                return 0;

        target_unit_path = strjoina(SYSTEM_DATA_UNIT_PATH, "/", default_dm, ".service");

        /* we only create the alias symlink for non sysvinit services */
        if (access(target_unit_path, F_OK) < 0 && (errno == ENOENT)) {
                /* if the dm service was already disabled, nothing to be done */
                if (!dm_service_exists) {
                        log_debug("No %s file, nothing to mask", dm_service_unit);
                        return 0;
                }
                log_warning("%s is not a systemd unit, we disable the systemd enabled display manager", target_unit_path);
                target_unit_path = "/dev/null";
        } else {
                log_warning("%s points at %s while the default systemd unit is %s. Reconfiguring %s as default.",
                            default_dm_file, default_dm, enabled_dm_unit, default_dm);
        }

        in_mem_symlink = strjoina(dest, "/display-manager.service");
        mkdir_parents_label(in_mem_symlink, 0755);
        if (symlink(target_unit_path, in_mem_symlink) < 0) {
                log_error("Failed to create symlink %s: %m", in_mem_symlink);
                return -errno;
        }

        return 0;
}

int main(int argc, char *argv[]) {
        int r;

        if (argc != 4) {
                log_error("This program takes three arguments.");
                return EXIT_FAILURE;
        }

        dest = argv[2];

        log_set_target(LOG_TARGET_SAFE);
        log_parse_environment();
        log_open();

        umask(0022);

        r = generate_display_manager_alias();

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
