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
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sd-id128.h"
#include "sd-messages.h"

#include "alloc-util.h"
#include "bus-common-errors.h"
#include "bus-util.h"
#include "cgroup-util.h"
#include "dbus-unit.h"
#include "dbus.h"
#include "dropin.h"
#include "escape.h"
#include "execute.h"
#include "fileio-label.h"
#include "format-util.h"
#include "id128-util.h"
#include "load-dropin.h"
#include "load-fragment.h"
#include "log.h"
#include "macro.h"
#include "missing.h"
#include "mkdir.h"
#include "parse-util.h"
#include "path-util.h"
#include "process-util.h"
#include "set.h"
#include "signal-util.h"
#include "special.h"
#include "stat-util.h"
#include "stdio-util.h"
#include "string-util.h"
#include "strv.h"
#include "umask-util.h"
#include "unit-name.h"
#include "unit.h"
#include "user-util.h"
#include "virt.h"

const UnitVTable * const unit_vtable[_UNIT_TYPE_MAX] = {
        [UNIT_SERVICE] = &service_vtable,
        [UNIT_SOCKET] = &socket_vtable,
        [UNIT_BUSNAME] = &busname_vtable,
        [UNIT_TARGET] = &target_vtable,
        [UNIT_DEVICE] = &device_vtable,
        [UNIT_MOUNT] = &mount_vtable,
        [UNIT_AUTOMOUNT] = &automount_vtable,
        [UNIT_SWAP] = &swap_vtable,
        [UNIT_TIMER] = &timer_vtable,
        [UNIT_PATH] = &path_vtable,
        [UNIT_SLICE] = &slice_vtable,
        [UNIT_SCOPE] = &scope_vtable
};

static void maybe_warn_about_dependency(Unit *u, const char *other, UnitDependency dependency);

Unit *unit_new(Manager *m, size_t size) {
        Unit *u;

        assert(m);
        assert(size >= sizeof(Unit));

        u = malloc0(size);
        if (!u)
                return NULL;

        u->names = set_new(&string_hash_ops);
        if (!u->names)
                return mfree(u);

        u->manager = m;
        u->type = _UNIT_TYPE_INVALID;
        u->default_dependencies = true;
        u->unit_file_state = _UNIT_FILE_STATE_INVALID;
        u->unit_file_preset = -1;
        u->on_failure_job_mode = JOB_REPLACE;
        u->cgroup_inotify_wd = -1;
        u->job_timeout = USEC_INFINITY;
        u->job_running_timeout = USEC_INFINITY;
        u->ref_uid = UID_INVALID;
        u->ref_gid = GID_INVALID;
        u->cpu_usage_last = NSEC_INFINITY;

        RATELIMIT_INIT(u->start_limit, m->default_start_limit_interval, m->default_start_limit_burst);
        RATELIMIT_INIT(u->auto_stop_ratelimit, 10 * USEC_PER_SEC, 16);

        return u;
}

int unit_new_for_name(Manager *m, size_t size, const char *name, Unit **ret) {
        Unit *u;
        int r;

        u = unit_new(m, size);
        if (!u)
                return -ENOMEM;

        r = unit_add_name(u, name);
        if (r < 0) {
                unit_free(u);
                return r;
        }

        *ret = u;
        return r;
}

bool unit_has_name(Unit *u, const char *name) {
        assert(u);
        assert(name);

        return set_contains(u->names, (char*) name);
}

static void unit_init(Unit *u) {
        CGroupContext *cc;
        ExecContext *ec;
        KillContext *kc;

        assert(u);
        assert(u->manager);
        assert(u->type >= 0);

        cc = unit_get_cgroup_context(u);
        if (cc) {
                cgroup_context_init(cc);

                /* Copy in the manager defaults into the cgroup
                 * context, _before_ the rest of the settings have
                 * been initialized */

                cc->cpu_accounting = u->manager->default_cpu_accounting;
                cc->io_accounting = u->manager->default_io_accounting;
                cc->blockio_accounting = u->manager->default_blockio_accounting;
                cc->memory_accounting = u->manager->default_memory_accounting;
                cc->tasks_accounting = u->manager->default_tasks_accounting;

                if (u->type != UNIT_SLICE)
                        cc->tasks_max = u->manager->default_tasks_max;
        }

        ec = unit_get_exec_context(u);
        if (ec)
                exec_context_init(ec);

        kc = unit_get_kill_context(u);
        if (kc)
                kill_context_init(kc);

        if (UNIT_VTABLE(u)->init)
                UNIT_VTABLE(u)->init(u);
}

int unit_add_name(Unit *u, const char *text) {
        _cleanup_free_ char *s = NULL, *i = NULL;
        UnitType t;
        int r;

        assert(u);
        assert(text);

        if (unit_name_is_valid(text, UNIT_NAME_TEMPLATE)) {

                if (!u->instance)
                        return -EINVAL;

                r = unit_name_replace_instance(text, u->instance, &s);
                if (r < 0)
                        return r;
        } else {
                s = strdup(text);
                if (!s)
                        return -ENOMEM;
        }

        if (set_contains(u->names, s))
                return 0;
        if (hashmap_contains(u->manager->units, s))
                return -EEXIST;

        if (!unit_name_is_valid(s, UNIT_NAME_PLAIN|UNIT_NAME_INSTANCE))
                return -EINVAL;

        t = unit_name_to_type(s);
        if (t < 0)
                return -EINVAL;

        if (u->type != _UNIT_TYPE_INVALID && t != u->type)
                return -EINVAL;

        r = unit_name_to_instance(s, &i);
        if (r < 0)
                return r;

        if (i && !unit_type_may_template(t))
                return -EINVAL;

        /* Ensure that this unit is either instanced or not instanced,
         * but not both. Note that we do allow names with different
         * instance names however! */
        if (u->type != _UNIT_TYPE_INVALID && !u->instance != !i)
                return -EINVAL;

        if (!unit_type_may_alias(t) && !set_isempty(u->names))
                return -EEXIST;

        if (hashmap_size(u->manager->units) >= MANAGER_MAX_NAMES)
                return -E2BIG;

        r = set_put(u->names, s);
        if (r < 0)
                return r;
        assert(r > 0);

        r = hashmap_put(u->manager->units, s, u);
        if (r < 0) {
                (void) set_remove(u->names, s);
                return r;
        }

        if (u->type == _UNIT_TYPE_INVALID) {
                u->type = t;
                u->id = s;
                u->instance = i;

                LIST_PREPEND(units_by_type, u->manager->units_by_type[t], u);

                unit_init(u);

                i = NULL;
        }

        s = NULL;

        unit_add_to_dbus_queue(u);
        return 0;
}

int unit_choose_id(Unit *u, const char *name) {
        _cleanup_free_ char *t = NULL;
        char *s, *i;
        int r;

        assert(u);
        assert(name);

        if (unit_name_is_valid(name, UNIT_NAME_TEMPLATE)) {

                if (!u->instance)
                        return -EINVAL;

                r = unit_name_replace_instance(name, u->instance, &t);
                if (r < 0)
                        return r;

                name = t;
        }

        /* Selects one of the names of this unit as the id */
        s = set_get(u->names, (char*) name);
        if (!s)
                return -ENOENT;

        /* Determine the new instance from the new id */
        r = unit_name_to_instance(s, &i);
        if (r < 0)
                return r;

        u->id = s;

        free(u->instance);
        u->instance = i;

        unit_add_to_dbus_queue(u);

        return 0;
}

int unit_set_description(Unit *u, const char *description) {
        char *s;

        assert(u);

        if (isempty(description))
                s = NULL;
        else {
                s = strdup(description);
                if (!s)
                        return -ENOMEM;
        }

        free(u->description);
        u->description = s;

        unit_add_to_dbus_queue(u);
        return 0;
}

bool unit_check_gc(Unit *u) {
        UnitActiveState state;
        bool inactive;
        assert(u);

        if (u->job)
                return true;

        if (u->nop_job)
                return true;

        state = unit_active_state(u);
        inactive = state == UNIT_INACTIVE;

        /* If the unit is inactive and failed and no job is queued for
         * it, then release its runtime resources */
        if (UNIT_IS_INACTIVE_OR_FAILED(state) &&
            UNIT_VTABLE(u)->release_resources)
                UNIT_VTABLE(u)->release_resources(u, inactive);

        /* But we keep the unit object around for longer when it is
         * referenced or configured to not be gc'ed */
        if (!inactive)
                return true;

        if (u->perpetual)
                return true;

        if (u->refs)
                return true;

        if (sd_bus_track_count(u->bus_track) > 0)
                return true;

        if (UNIT_VTABLE(u)->check_gc)
                if (UNIT_VTABLE(u)->check_gc(u))
                        return true;

        return false;
}

void unit_add_to_load_queue(Unit *u) {
        assert(u);
        assert(u->type != _UNIT_TYPE_INVALID);

        if (u->load_state != UNIT_STUB || u->in_load_queue)
                return;

        LIST_PREPEND(load_queue, u->manager->load_queue, u);
        u->in_load_queue = true;
}

void unit_add_to_cleanup_queue(Unit *u) {
        assert(u);

        if (u->in_cleanup_queue)
                return;

        LIST_PREPEND(cleanup_queue, u->manager->cleanup_queue, u);
        u->in_cleanup_queue = true;
}

void unit_add_to_gc_queue(Unit *u) {
        assert(u);

        if (u->in_gc_queue || u->in_cleanup_queue)
                return;

        if (unit_check_gc(u))
                return;

        LIST_PREPEND(gc_queue, u->manager->gc_unit_queue, u);
        u->in_gc_queue = true;
}

void unit_add_to_dbus_queue(Unit *u) {
        assert(u);
        assert(u->type != _UNIT_TYPE_INVALID);

        if (u->load_state == UNIT_STUB || u->in_dbus_queue)
                return;

        /* Shortcut things if nobody cares */
        if (sd_bus_track_count(u->manager->subscribed) <= 0 &&
            sd_bus_track_count(u->bus_track) <= 0 &&
            set_isempty(u->manager->private_buses)) {
                u->sent_dbus_new_signal = true;
                return;
        }

        LIST_PREPEND(dbus_queue, u->manager->dbus_unit_queue, u);
        u->in_dbus_queue = true;
}

static void bidi_set_free(Unit *u, Set *s) {
        Iterator i;
        Unit *other;

        assert(u);

        /* Frees the set and makes sure we are dropped from the
         * inverse pointers */

        SET_FOREACH(other, s, i) {
                UnitDependency d;

                for (d = 0; d < _UNIT_DEPENDENCY_MAX; d++)
                        set_remove(other->dependencies[d], u);

                unit_add_to_gc_queue(other);
        }

        set_free(s);
}

static void unit_remove_transient(Unit *u) {
        char **i;

        assert(u);

        if (!u->transient)
                return;

        if (u->fragment_path)
                (void) unlink(u->fragment_path);

        STRV_FOREACH(i, u->dropin_paths) {
                _cleanup_free_ char *p = NULL, *pp = NULL;

                p = dirname_malloc(*i); /* Get the drop-in directory from the drop-in file */
                if (!p)
                        continue;

                pp = dirname_malloc(p); /* Get the config directory from the drop-in directory */
                if (!pp)
                        continue;

                /* Only drop transient drop-ins */
                if (!path_equal(u->manager->lookup_paths.transient, pp))
                        continue;

                (void) unlink(*i);
                (void) rmdir(p);
        }
}

static void unit_free_requires_mounts_for(Unit *u) {
        char **j;

        STRV_FOREACH(j, u->requires_mounts_for) {
                char s[strlen(*j) + 1];

                PATH_FOREACH_PREFIX_MORE(s, *j) {
                        char *y;
                        Set *x;

                        x = hashmap_get2(u->manager->units_requiring_mounts_for, s, (void**) &y);
                        if (!x)
                                continue;

                        set_remove(x, u);

                        if (set_isempty(x)) {
                                hashmap_remove(u->manager->units_requiring_mounts_for, y);
                                free(y);
                                set_free(x);
                        }
                }
        }

        u->requires_mounts_for = strv_free(u->requires_mounts_for);
}

static void unit_done(Unit *u) {
        ExecContext *ec;
        CGroupContext *cc;

        assert(u);

        if (u->type < 0)
                return;

        if (UNIT_VTABLE(u)->done)
                UNIT_VTABLE(u)->done(u);

        ec = unit_get_exec_context(u);
        if (ec)
                exec_context_done(ec);

        cc = unit_get_cgroup_context(u);
        if (cc)
                cgroup_context_done(cc);
}

void unit_free(Unit *u) {
        UnitDependency d;
        Iterator i;
        char *t;

        if (!u)
                return;

        if (u->transient_file)
                fclose(u->transient_file);

        if (!MANAGER_IS_RELOADING(u->manager))
                unit_remove_transient(u);

        bus_unit_send_removed_signal(u);

        unit_done(u);

        sd_bus_slot_unref(u->match_bus_slot);

        sd_bus_track_unref(u->bus_track);
        u->deserialized_refs = strv_free(u->deserialized_refs);

        unit_free_requires_mounts_for(u);

        SET_FOREACH(t, u->names, i)
                hashmap_remove_value(u->manager->units, t, u);

        if (!sd_id128_is_null(u->invocation_id))
                hashmap_remove_value(u->manager->units_by_invocation_id, &u->invocation_id, u);

        if (u->job) {
                Job *j = u->job;
                job_uninstall(j);
                job_free(j);
        }

        if (u->nop_job) {
                Job *j = u->nop_job;
                job_uninstall(j);
                job_free(j);
        }

        for (d = 0; d < _UNIT_DEPENDENCY_MAX; d++)
                bidi_set_free(u, u->dependencies[d]);

        if (u->type != _UNIT_TYPE_INVALID)
                LIST_REMOVE(units_by_type, u->manager->units_by_type[u->type], u);

        if (u->in_load_queue)
                LIST_REMOVE(load_queue, u->manager->load_queue, u);

        if (u->in_dbus_queue)
                LIST_REMOVE(dbus_queue, u->manager->dbus_unit_queue, u);

        if (u->in_cleanup_queue)
                LIST_REMOVE(cleanup_queue, u->manager->cleanup_queue, u);

        if (u->in_gc_queue)
                LIST_REMOVE(gc_queue, u->manager->gc_unit_queue, u);

        if (u->in_cgroup_queue)
                LIST_REMOVE(cgroup_queue, u->manager->cgroup_queue, u);

        unit_release_cgroup(u);

        unit_unref_uid_gid(u, false);

        (void) manager_update_failed_units(u->manager, u, false);
        set_remove(u->manager->startup_units, u);

        unit_unwatch_all_pids(u);

        unit_ref_unset(&u->slice);

        while (u->refs)
                unit_ref_unset(u->refs);

        condition_free_list(u->conditions);
        condition_free_list(u->asserts);

        free(u->description);
        strv_free(u->documentation);
        free(u->fragment_path);
        free(u->source_path);
        strv_free(u->dropin_paths);
        free(u->instance);

        free(u->job_timeout_reboot_arg);

        set_free_free(u->names);

        free(u->reboot_arg);

        free(u);
}

UnitActiveState unit_active_state(Unit *u) {
        assert(u);

        if (u->load_state == UNIT_MERGED)
                return unit_active_state(unit_follow_merge(u));

        /* After a reload it might happen that a unit is not correctly
         * loaded but still has a process around. That's why we won't
         * shortcut failed loading to UNIT_INACTIVE_FAILED. */

        return UNIT_VTABLE(u)->active_state(u);
}

const char* unit_sub_state_to_string(Unit *u) {
        assert(u);

        return UNIT_VTABLE(u)->sub_state_to_string(u);
}

static int complete_move(Set **s, Set **other) {
        int r;

        assert(s);
        assert(other);

        if (!*other)
                return 0;

        if (*s) {
                r = set_move(*s, *other);
                if (r < 0)
                        return r;
        } else {
                *s = *other;
                *other = NULL;
        }

        return 0;
}

static int merge_names(Unit *u, Unit *other) {
        char *t;
        Iterator i;
        int r;

        assert(u);
        assert(other);

        r = complete_move(&u->names, &other->names);
        if (r < 0)
                return r;

        set_free_free(other->names);
        other->names = NULL;
        other->id = NULL;

        SET_FOREACH(t, u->names, i)
                assert_se(hashmap_replace(u->manager->units, t, u) == 0);

        return 0;
}

static int reserve_dependencies(Unit *u, Unit *other, UnitDependency d) {
        unsigned n_reserve;

        assert(u);
        assert(other);
        assert(d < _UNIT_DEPENDENCY_MAX);

        /*
         * If u does not have this dependency set allocated, there is no need
         * to reserve anything. In that case other's set will be transferred
         * as a whole to u by complete_move().
         */
        if (!u->dependencies[d])
                return 0;

        /* merge_dependencies() will skip a u-on-u dependency */
        n_reserve = set_size(other->dependencies[d]) - !!set_get(other->dependencies[d], u);

        return set_reserve(u->dependencies[d], n_reserve);
}

static void merge_dependencies(Unit *u, Unit *other, const char *other_id, UnitDependency d) {
        Iterator i;
        Unit *back;
        int r;

        assert(u);
        assert(other);
        assert(d < _UNIT_DEPENDENCY_MAX);

        /* Fix backwards pointers */
        SET_FOREACH(back, other->dependencies[d], i) {
                UnitDependency k;

                for (k = 0; k < _UNIT_DEPENDENCY_MAX; k++) {
                        /* Do not add dependencies between u and itself */
                        if (back == u) {
                                if (set_remove(back->dependencies[k], other))
                                        maybe_warn_about_dependency(u, other_id, k);
                        } else {
                                r = set_remove_and_put(back->dependencies[k], other, u);
                                if (r == -EEXIST)
                                        set_remove(back->dependencies[k], other);
                                else
                                        assert(r >= 0 || r == -ENOENT);
                        }
                }
        }

        /* Also do not move dependencies on u to itself */
        back = set_remove(other->dependencies[d], u);
        if (back)
                maybe_warn_about_dependency(u, other_id, d);

        /* The move cannot fail. The caller must have performed a reservation. */
        assert_se(complete_move(&u->dependencies[d], &other->dependencies[d]) == 0);

        other->dependencies[d] = set_free(other->dependencies[d]);
}

int unit_merge(Unit *u, Unit *other) {
        UnitDependency d;
        const char *other_id = NULL;
        int r;

        assert(u);
        assert(other);
        assert(u->manager == other->manager);
        assert(u->type != _UNIT_TYPE_INVALID);

        other = unit_follow_merge(other);

        if (other == u)
                return 0;

        if (u->type != other->type)
                return -EINVAL;

        if (!u->instance != !other->instance)
                return -EINVAL;

        if (!unit_type_may_alias(u->type)) /* Merging only applies to unit names that support aliases */
                return -EEXIST;

        if (other->load_state != UNIT_STUB &&
            other->load_state != UNIT_NOT_FOUND)
                return -EEXIST;

        if (other->job)
                return -EEXIST;

        if (other->nop_job)
                return -EEXIST;

        if (!UNIT_IS_INACTIVE_OR_FAILED(unit_active_state(other)))
                return -EEXIST;

        if (other->id)
                other_id = strdupa(other->id);

        /* Make reservations to ensure merge_dependencies() won't fail */
        for (d = 0; d < _UNIT_DEPENDENCY_MAX; d++) {
                r = reserve_dependencies(u, other, d);
                /*
                 * We don't rollback reservations if we fail. We don't have
                 * a way to undo reservations. A reservation is not a leak.
                 */
                if (r < 0)
                        return r;
        }

        /* Merge names */
        r = merge_names(u, other);
        if (r < 0)
                return r;

        /* Redirect all references */
        while (other->refs)
                unit_ref_set(other->refs, u);

        /* Merge dependencies */
        for (d = 0; d < _UNIT_DEPENDENCY_MAX; d++)
                merge_dependencies(u, other, other_id, d);

        other->load_state = UNIT_MERGED;
        other->merged_into = u;

        /* If there is still some data attached to the other node, we
         * don't need it anymore, and can free it. */
        if (other->load_state != UNIT_STUB)
                if (UNIT_VTABLE(other)->done)
                        UNIT_VTABLE(other)->done(other);

        unit_add_to_dbus_queue(u);
        unit_add_to_cleanup_queue(other);

        return 0;
}

int unit_merge_by_name(Unit *u, const char *name) {
        _cleanup_free_ char *s = NULL;
        Unit *other;
        int r;

        assert(u);
        assert(name);

        if (unit_name_is_valid(name, UNIT_NAME_TEMPLATE)) {
                if (!u->instance)
                        return -EINVAL;

                r = unit_name_replace_instance(name, u->instance, &s);
                if (r < 0)
                        return r;

                name = s;
        }

        other = manager_get_unit(u->manager, name);
        if (other)
                return unit_merge(u, other);

        return unit_add_name(u, name);
}

Unit* unit_follow_merge(Unit *u) {
        assert(u);

        while (u->load_state == UNIT_MERGED)
                assert_se(u = u->merged_into);

        return u;
}

int unit_add_exec_dependencies(Unit *u, ExecContext *c) {
        int r;

        assert(u);
        assert(c);

        if (c->working_directory) {
                r = unit_require_mounts_for(u, c->working_directory);
                if (r < 0)
                        return r;
        }

        if (c->root_directory) {
                r = unit_require_mounts_for(u, c->root_directory);
                if (r < 0)
                        return r;
        }

        if (c->root_image) {
                r = unit_require_mounts_for(u, c->root_image);
                if (r < 0)
                        return r;
        }

        if (!MANAGER_IS_SYSTEM(u->manager))
                return 0;

        if (c->private_tmp) {
                const char *p;

                FOREACH_STRING(p, "/tmp", "/var/tmp") {
                        r = unit_require_mounts_for(u, p);
                        if (r < 0)
                                return r;
                }

                r = unit_add_dependency_by_name(u, UNIT_AFTER, SPECIAL_TMPFILES_SETUP_SERVICE, NULL, true);
                if (r < 0)
                        return r;
        }

        if (!IN_SET(c->std_output,
                    EXEC_OUTPUT_JOURNAL, EXEC_OUTPUT_JOURNAL_AND_CONSOLE,
                    EXEC_OUTPUT_KMSG, EXEC_OUTPUT_KMSG_AND_CONSOLE,
                    EXEC_OUTPUT_SYSLOG, EXEC_OUTPUT_SYSLOG_AND_CONSOLE) &&
            !IN_SET(c->std_error,
                    EXEC_OUTPUT_JOURNAL, EXEC_OUTPUT_JOURNAL_AND_CONSOLE,
                    EXEC_OUTPUT_KMSG, EXEC_OUTPUT_KMSG_AND_CONSOLE,
                    EXEC_OUTPUT_SYSLOG, EXEC_OUTPUT_SYSLOG_AND_CONSOLE))
                return 0;

        /* If syslog or kernel logging is requested, make sure our own
         * logging daemon is run first. */

        r = unit_add_dependency_by_name(u, UNIT_AFTER, SPECIAL_JOURNALD_SOCKET, NULL, true);
        if (r < 0)
                return r;

        return 0;
}

const char *unit_description(Unit *u) {
        assert(u);

        if (u->description)
                return u->description;

        return strna(u->id);
}

void unit_dump(Unit *u, FILE *f, const char *prefix) {
        char *t, **j;
        UnitDependency d;
        Iterator i;
        const char *prefix2;
        char
                timestamp0[FORMAT_TIMESTAMP_MAX],
                timestamp1[FORMAT_TIMESTAMP_MAX],
                timestamp2[FORMAT_TIMESTAMP_MAX],
                timestamp3[FORMAT_TIMESTAMP_MAX],
                timestamp4[FORMAT_TIMESTAMP_MAX],
                timespan[FORMAT_TIMESPAN_MAX];
        Unit *following;
        _cleanup_set_free_ Set *following_set = NULL;
        int r;
        const char *n;

        assert(u);
        assert(u->type >= 0);

        prefix = strempty(prefix);
        prefix2 = strjoina(prefix, "\t");

        fprintf(f,
                "%s-> Unit %s:\n"
                "%s\tDescription: %s\n"
                "%s\tInstance: %s\n"
                "%s\tUnit Load State: %s\n"
                "%s\tUnit Active State: %s\n"
                "%s\tState Change Timestamp: %s\n"
                "%s\tInactive Exit Timestamp: %s\n"
                "%s\tActive Enter Timestamp: %s\n"
                "%s\tActive Exit Timestamp: %s\n"
                "%s\tInactive Enter Timestamp: %s\n"
                "%s\tGC Check Good: %s\n"
                "%s\tNeed Daemon Reload: %s\n"
                "%s\tTransient: %s\n"
                "%s\tPerpetual: %s\n"
                "%s\tSlice: %s\n"
                "%s\tCGroup: %s\n"
                "%s\tCGroup realized: %s\n",
                prefix, u->id,
                prefix, unit_description(u),
                prefix, strna(u->instance),
                prefix, unit_load_state_to_string(u->load_state),
                prefix, unit_active_state_to_string(unit_active_state(u)),
                prefix, strna(format_timestamp(timestamp0, sizeof(timestamp0), u->state_change_timestamp.realtime)),
                prefix, strna(format_timestamp(timestamp1, sizeof(timestamp1), u->inactive_exit_timestamp.realtime)),
                prefix, strna(format_timestamp(timestamp2, sizeof(timestamp2), u->active_enter_timestamp.realtime)),
                prefix, strna(format_timestamp(timestamp3, sizeof(timestamp3), u->active_exit_timestamp.realtime)),
                prefix, strna(format_timestamp(timestamp4, sizeof(timestamp4), u->inactive_enter_timestamp.realtime)),
                prefix, yes_no(unit_check_gc(u)),
                prefix, yes_no(unit_need_daemon_reload(u)),
                prefix, yes_no(u->transient),
                prefix, yes_no(u->perpetual),
                prefix, strna(unit_slice_name(u)),
                prefix, strna(u->cgroup_path),
                prefix, yes_no(u->cgroup_realized));

        if (u->cgroup_realized_mask != 0) {
                _cleanup_free_ char *s = NULL;
                (void) cg_mask_to_string(u->cgroup_realized_mask, &s);
                fprintf(f, "%s\tCGroup mask: %s\n", prefix, strnull(s));
        }
        if (u->cgroup_members_mask != 0) {
                _cleanup_free_ char *s = NULL;
                (void) cg_mask_to_string(u->cgroup_members_mask, &s);
                fprintf(f, "%s\tCGroup members mask: %s\n", prefix, strnull(s));
        }

        SET_FOREACH(t, u->names, i)
                fprintf(f, "%s\tName: %s\n", prefix, t);

        if (!sd_id128_is_null(u->invocation_id))
                fprintf(f, "%s\tInvocation ID: " SD_ID128_FORMAT_STR "\n",
                        prefix, SD_ID128_FORMAT_VAL(u->invocation_id));

        STRV_FOREACH(j, u->documentation)
                fprintf(f, "%s\tDocumentation: %s\n", prefix, *j);

        following = unit_following(u);
        if (following)
                fprintf(f, "%s\tFollowing: %s\n", prefix, following->id);

        r = unit_following_set(u, &following_set);
        if (r >= 0) {
                Unit *other;

                SET_FOREACH(other, following_set, i)
                        fprintf(f, "%s\tFollowing Set Member: %s\n", prefix, other->id);
        }

        if (u->fragment_path)
                fprintf(f, "%s\tFragment Path: %s\n", prefix, u->fragment_path);

        if (u->source_path)
                fprintf(f, "%s\tSource Path: %s\n", prefix, u->source_path);

        STRV_FOREACH(j, u->dropin_paths)
                fprintf(f, "%s\tDropIn Path: %s\n", prefix, *j);

        if (u->job_timeout != USEC_INFINITY)
                fprintf(f, "%s\tJob Timeout: %s\n", prefix, format_timespan(timespan, sizeof(timespan), u->job_timeout, 0));

        if (u->job_timeout_action != EMERGENCY_ACTION_NONE)
                fprintf(f, "%s\tJob Timeout Action: %s\n", prefix, emergency_action_to_string(u->job_timeout_action));

        if (u->job_timeout_reboot_arg)
                fprintf(f, "%s\tJob Timeout Reboot Argument: %s\n", prefix, u->job_timeout_reboot_arg);

        condition_dump_list(u->conditions, f, prefix, condition_type_to_string);
        condition_dump_list(u->asserts, f, prefix, assert_type_to_string);

        if (dual_timestamp_is_set(&u->condition_timestamp))
                fprintf(f,
                        "%s\tCondition Timestamp: %s\n"
                        "%s\tCondition Result: %s\n",
                        prefix, strna(format_timestamp(timestamp1, sizeof(timestamp1), u->condition_timestamp.realtime)),
                        prefix, yes_no(u->condition_result));

        if (dual_timestamp_is_set(&u->assert_timestamp))
                fprintf(f,
                        "%s\tAssert Timestamp: %s\n"
                        "%s\tAssert Result: %s\n",
                        prefix, strna(format_timestamp(timestamp1, sizeof(timestamp1), u->assert_timestamp.realtime)),
                        prefix, yes_no(u->assert_result));

        for (d = 0; d < _UNIT_DEPENDENCY_MAX; d++) {
                Unit *other;

                SET_FOREACH(other, u->dependencies[d], i)
                        fprintf(f, "%s\t%s: %s\n", prefix, unit_dependency_to_string(d), other->id);
        }

        if (!strv_isempty(u->requires_mounts_for)) {
                fprintf(f,
                        "%s\tRequiresMountsFor:", prefix);

                STRV_FOREACH(j, u->requires_mounts_for)
                        fprintf(f, " %s", *j);

                fputs("\n", f);
        }

        if (u->load_state == UNIT_LOADED) {

                fprintf(f,
                        "%s\tStopWhenUnneeded: %s\n"
                        "%s\tRefuseManualStart: %s\n"
                        "%s\tRefuseManualStop: %s\n"
                        "%s\tDefaultDependencies: %s\n"
                        "%s\tOnFailureJobMode: %s\n"
                        "%s\tIgnoreOnIsolate: %s\n",
                        prefix, yes_no(u->stop_when_unneeded),
                        prefix, yes_no(u->refuse_manual_start),
                        prefix, yes_no(u->refuse_manual_stop),
                        prefix, yes_no(u->default_dependencies),
                        prefix, job_mode_to_string(u->on_failure_job_mode),
                        prefix, yes_no(u->ignore_on_isolate));

                if (UNIT_VTABLE(u)->dump)
                        UNIT_VTABLE(u)->dump(u, f, prefix2);

        } else if (u->load_state == UNIT_MERGED)
                fprintf(f,
                        "%s\tMerged into: %s\n",
                        prefix, u->merged_into->id);
        else if (u->load_state == UNIT_ERROR)
                fprintf(f, "%s\tLoad Error Code: %s\n", prefix, strerror(-u->load_error));

        for (n = sd_bus_track_first(u->bus_track); n; n = sd_bus_track_next(u->bus_track))
                fprintf(f, "%s\tBus Ref: %s\n", prefix, n);

        if (u->job)
                job_dump(u->job, f, prefix2);

        if (u->nop_job)
                job_dump(u->nop_job, f, prefix2);
}

/* Common implementation for multiple backends */
int unit_load_fragment_and_dropin(Unit *u) {
        int r;

        assert(u);

        /* Load a .{service,socket,...} file */
        r = unit_load_fragment(u);
        if (r < 0)
                return r;

        if (u->load_state == UNIT_STUB)
                return -ENOENT;

        /* Load drop-in directory data. If u is an alias, we might be reloading the
         * target unit needlessly. But we cannot be sure which drops-ins have already
         * been loaded and which not, at least without doing complicated book-keeping,
         * so let's always reread all drop-ins. */
        return unit_load_dropin(unit_follow_merge(u));
}

/* Common implementation for multiple backends */
int unit_load_fragment_and_dropin_optional(Unit *u) {
        int r;

        assert(u);

        /* Same as unit_load_fragment_and_dropin(), but whether
         * something can be loaded or not doesn't matter. */

        /* Load a .service file */
        r = unit_load_fragment(u);
        if (r < 0)
                return r;

        if (u->load_state == UNIT_STUB)
                u->load_state = UNIT_LOADED;

        /* Load drop-in directory data */
        return unit_load_dropin(unit_follow_merge(u));
}

int unit_add_default_target_dependency(Unit *u, Unit *target) {
        assert(u);
        assert(target);

        if (target->type != UNIT_TARGET)
                return 0;

        /* Only add the dependency if both units are loaded, so that
         * that loop check below is reliable */
        if (u->load_state != UNIT_LOADED ||
            target->load_state != UNIT_LOADED)
                return 0;

        /* If either side wants no automatic dependencies, then let's
         * skip this */
        if (!u->default_dependencies ||
            !target->default_dependencies)
                return 0;

        /* Don't create loops */
        if (set_get(target->dependencies[UNIT_BEFORE], u))
                return 0;

        return unit_add_dependency(target, UNIT_AFTER, u, true);
}

static int unit_add_target_dependencies(Unit *u) {

        static const UnitDependency deps[] = {
                UNIT_REQUIRED_BY,
                UNIT_REQUISITE_OF,
                UNIT_WANTED_BY,
                UNIT_BOUND_BY
        };

        Unit *target;
        Iterator i;
        unsigned k;
        int r = 0;

        assert(u);

        for (k = 0; k < ELEMENTSOF(deps); k++)
                SET_FOREACH(target, u->dependencies[deps[k]], i) {
                        r = unit_add_default_target_dependency(u, target);
                        if (r < 0)
                                return r;
                }

        return r;
}

static int unit_add_slice_dependencies(Unit *u) {
        assert(u);

        if (!UNIT_HAS_CGROUP_CONTEXT(u))
                return 0;

        if (UNIT_ISSET(u->slice))
                return unit_add_two_dependencies(u, UNIT_AFTER, UNIT_REQUIRES, UNIT_DEREF(u->slice), true);

        if (unit_has_name(u, SPECIAL_ROOT_SLICE))
                return 0;

        return unit_add_two_dependencies_by_name(u, UNIT_AFTER, UNIT_REQUIRES, SPECIAL_ROOT_SLICE, NULL, true);
}

static int unit_add_mount_dependencies(Unit *u) {
        char **i;
        int r;

        assert(u);

        STRV_FOREACH(i, u->requires_mounts_for) {
                char prefix[strlen(*i) + 1];

                PATH_FOREACH_PREFIX_MORE(prefix, *i) {
                        _cleanup_free_ char *p = NULL;
                        Unit *m;

                        r = unit_name_from_path(prefix, ".mount", &p);
                        if (r < 0)
                                return r;

                        m = manager_get_unit(u->manager, p);
                        if (!m) {
                                /* Make sure to load the mount unit if
                                 * it exists. If so the dependencies
                                 * on this unit will be added later
                                 * during the loading of the mount
                                 * unit. */
                                (void) manager_load_unit_prepare(u->manager, p, NULL, NULL, &m);
                                continue;
                        }
                        if (m == u)
                                continue;

                        if (m->load_state != UNIT_LOADED)
                                continue;

                        r = unit_add_dependency(u, UNIT_AFTER, m, true);
                        if (r < 0)
                                return r;

                        if (m->fragment_path) {
                                r = unit_add_dependency(u, UNIT_REQUIRES, m, true);
                                if (r < 0)
                                        return r;
                        }
                }
        }

        return 0;
}

static int unit_add_startup_units(Unit *u) {
        CGroupContext *c;
        int r;

        c = unit_get_cgroup_context(u);
        if (!c)
                return 0;

        if (c->startup_cpu_shares == CGROUP_CPU_SHARES_INVALID &&
            c->startup_io_weight == CGROUP_WEIGHT_INVALID &&
            c->startup_blockio_weight == CGROUP_BLKIO_WEIGHT_INVALID)
                return 0;

        r = set_ensure_allocated(&u->manager->startup_units, NULL);
        if (r < 0)
                return r;

        return set_put(u->manager->startup_units, u);
}

int unit_load(Unit *u) {
        int r;

        assert(u);

        if (u->in_load_queue) {
                LIST_REMOVE(load_queue, u->manager->load_queue, u);
                u->in_load_queue = false;
        }

        if (u->type == _UNIT_TYPE_INVALID)
                return -EINVAL;

        if (u->load_state != UNIT_STUB)
                return 0;

        if (u->transient_file) {
                r = fflush_and_check(u->transient_file);
                if (r < 0)
                        goto fail;

                fclose(u->transient_file);
                u->transient_file = NULL;

                u->fragment_mtime = now(CLOCK_REALTIME);
        }

        if (UNIT_VTABLE(u)->load) {
                r = UNIT_VTABLE(u)->load(u);
                if (r < 0)
                        goto fail;
        }

        if (u->load_state == UNIT_STUB) {
                r = -ENOENT;
                goto fail;
        }

        if (u->load_state == UNIT_LOADED) {

                r = unit_add_target_dependencies(u);
                if (r < 0)
                        goto fail;

                r = unit_add_slice_dependencies(u);
                if (r < 0)
                        goto fail;

                r = unit_add_mount_dependencies(u);
                if (r < 0)
                        goto fail;

                r = unit_add_startup_units(u);
                if (r < 0)
                        goto fail;

                if (u->on_failure_job_mode == JOB_ISOLATE && set_size(u->dependencies[UNIT_ON_FAILURE]) > 1) {
                        log_unit_error(u, "More than one OnFailure= dependencies specified but OnFailureJobMode=isolate set. Refusing.");
                        r = -EINVAL;
                        goto fail;
                }

                if (u->job_running_timeout != USEC_INFINITY && u->job_running_timeout > u->job_timeout)
                        log_unit_warning(u, "JobRunningTimeoutSec= is greater than JobTimeoutSec=, it has no effect.");

                unit_update_cgroup_members_masks(u);
        }

        assert((u->load_state != UNIT_MERGED) == !u->merged_into);

        unit_add_to_dbus_queue(unit_follow_merge(u));
        unit_add_to_gc_queue(u);

        return 0;

fail:
        u->load_state = u->load_state == UNIT_STUB ? UNIT_NOT_FOUND : UNIT_ERROR;
        u->load_error = r;
        unit_add_to_dbus_queue(u);
        unit_add_to_gc_queue(u);

        log_unit_debug_errno(u, r, "Failed to load configuration: %m");

        return r;
}

static bool unit_condition_test_list(Unit *u, Condition *first, const char *(*to_string)(ConditionType t)) {
        Condition *c;
        int triggered = -1;

        assert(u);
        assert(to_string);

        /* If the condition list is empty, then it is true */
        if (!first)
                return true;

        /* Otherwise, if all of the non-trigger conditions apply and
         * if any of the trigger conditions apply (unless there are
         * none) we return true */
        LIST_FOREACH(conditions, c, first) {
                int r;

                r = condition_test(c);
                if (r < 0)
                        log_unit_warning(u,
                                         "Couldn't determine result for %s=%s%s%s, assuming failed: %m",
                                         to_string(c->type),
                                         c->trigger ? "|" : "",
                                         c->negate ? "!" : "",
                                         c->parameter);
                else
                        log_unit_debug(u,
                                       "%s=%s%s%s %s.",
                                       to_string(c->type),
                                       c->trigger ? "|" : "",
                                       c->negate ? "!" : "",
                                       c->parameter,
                                       condition_result_to_string(c->result));

                if (!c->trigger && r <= 0)
                        return false;

                if (c->trigger && triggered <= 0)
                        triggered = r > 0;
        }

        return triggered != 0;
}

static bool unit_condition_test(Unit *u) {
        assert(u);

        dual_timestamp_get(&u->condition_timestamp);
        u->condition_result = unit_condition_test_list(u, u->conditions, condition_type_to_string);

        return u->condition_result;
}

static bool unit_assert_test(Unit *u) {
        assert(u);

        dual_timestamp_get(&u->assert_timestamp);
        u->assert_result = unit_condition_test_list(u, u->asserts, assert_type_to_string);

        return u->assert_result;
}

void unit_status_printf(Unit *u, const char *status, const char *unit_status_msg_format) {
        DISABLE_WARNING_FORMAT_NONLITERAL;
        manager_status_printf(u->manager, STATUS_TYPE_NORMAL, status, unit_status_msg_format, unit_description(u));
        REENABLE_WARNING;
}

_pure_ static const char* unit_get_status_message_format(Unit *u, JobType t) {
        const char *format;
        const UnitStatusMessageFormats *format_table;

        assert(u);
        assert(IN_SET(t, JOB_START, JOB_STOP, JOB_RELOAD));

        if (t != JOB_RELOAD) {
                format_table = &UNIT_VTABLE(u)->status_message_formats;
                if (format_table) {
                        format = format_table->starting_stopping[t == JOB_STOP];
                        if (format)
                                return format;
                }
        }

        /* Return generic strings */
        if (t == JOB_START)
                return "Starting %s.";
        else if (t == JOB_STOP)
                return "Stopping %s.";
        else
                return "Reloading %s.";
}

static void unit_status_print_starting_stopping(Unit *u, JobType t) {
        const char *format;

        assert(u);

        /* Reload status messages have traditionally not been printed to console. */
        if (!IN_SET(t, JOB_START, JOB_STOP))
                return;

        format = unit_get_status_message_format(u, t);

        DISABLE_WARNING_FORMAT_NONLITERAL;
        unit_status_printf(u, "", format);
        REENABLE_WARNING;
}

static void unit_status_log_starting_stopping_reloading(Unit *u, JobType t) {
        const char *format, *mid;
        char buf[LINE_MAX];

        assert(u);

        if (!IN_SET(t, JOB_START, JOB_STOP, JOB_RELOAD))
                return;

        if (log_on_console())
                return;

        /* We log status messages for all units and all operations. */

        format = unit_get_status_message_format(u, t);

        DISABLE_WARNING_FORMAT_NONLITERAL;
        snprintf(buf, sizeof buf, format, unit_description(u));
        REENABLE_WARNING;

        mid = t == JOB_START ? "MESSAGE_ID=" SD_MESSAGE_UNIT_STARTING_STR :
              t == JOB_STOP  ? "MESSAGE_ID=" SD_MESSAGE_UNIT_STOPPING_STR :
                               "MESSAGE_ID=" SD_MESSAGE_UNIT_RELOADING_STR;

        /* Note that we deliberately use LOG_MESSAGE() instead of
         * LOG_UNIT_MESSAGE() here, since this is supposed to mimic
         * closely what is written to screen using the status output,
         * which is supposed the highest level, friendliest output
         * possible, which means we should avoid the low-level unit
         * name. */
        log_struct(LOG_INFO,
                   LOG_MESSAGE("%s", buf),
                   LOG_UNIT_ID(u),
                   mid,
                   NULL);
}

void unit_status_emit_starting_stopping_reloading(Unit *u, JobType t) {
        assert(u);
        assert(t >= 0);
        assert(t < _JOB_TYPE_MAX);

        unit_status_log_starting_stopping_reloading(u, t);
        unit_status_print_starting_stopping(u, t);
}

int unit_start_limit_test(Unit *u) {
        assert(u);

        if (ratelimit_test(&u->start_limit)) {
                u->start_limit_hit = false;
                return 0;
        }

        log_unit_warning(u, "Start request repeated too quickly.");
        u->start_limit_hit = true;

        return emergency_action(u->manager, u->start_limit_action, u->reboot_arg, "unit failed");
}

bool unit_shall_confirm_spawn(Unit *u) {
        assert(u);

        if (manager_is_confirm_spawn_disabled(u->manager))
                return false;

        /* For some reasons units remaining in the same process group
         * as PID 1 fail to acquire the console even if it's not used
         * by any process. So skip the confirmation question for them. */
        return !unit_get_exec_context(u)->same_pgrp;
}

static bool unit_verify_deps(Unit *u) {
        Unit *other;
        Iterator j;

        assert(u);

        /* Checks whether all BindsTo= dependencies of this unit are fulfilled — if they are also combined with
         * After=. We do not check Requires= or Requisite= here as they only should have an effect on the job
         * processing, but do not have any effect afterwards. We don't check BindsTo= dependencies that are not used in
         * conjunction with After= as for them any such check would make things entirely racy. */

        SET_FOREACH(other, u->dependencies[UNIT_BINDS_TO], j) {

                if (!set_contains(u->dependencies[UNIT_AFTER], other))
                        continue;

                if (!UNIT_IS_ACTIVE_OR_RELOADING(unit_active_state(other))) {
                        log_unit_notice(u, "Bound to unit %s, but unit isn't active.", other->id);
                        return false;
                }
        }

        return true;
}

/* Errors:
 *         -EBADR:      This unit type does not support starting.
 *         -EALREADY:   Unit is already started.
 *         -EAGAIN:     An operation is already in progress. Retry later.
 *         -ECANCELED:  Too many requests for now.
 *         -EPROTO:     Assert failed
 *         -EINVAL:     Unit not loaded
 *         -EOPNOTSUPP: Unit type not supported
 *         -ENOLINK:    The necessary dependencies are not fulfilled.
 */
int unit_start(Unit *u) {
        UnitActiveState state;
        Unit *following;

        assert(u);

        /* If this is already started, then this will succeed. Note
         * that this will even succeed if this unit is not startable
         * by the user. This is relied on to detect when we need to
         * wait for units and when waiting is finished. */
        state = unit_active_state(u);
        if (UNIT_IS_ACTIVE_OR_RELOADING(state))
                return -EALREADY;

        /* Units that aren't loaded cannot be started */
        if (u->load_state != UNIT_LOADED)
                return -EINVAL;

        /* If the conditions failed, don't do anything at all. If we
         * already are activating this call might still be useful to
         * speed up activation in case there is some hold-off time,
         * but we don't want to recheck the condition in that case. */
        if (state != UNIT_ACTIVATING &&
            !unit_condition_test(u)) {
                log_unit_debug(u, "Starting requested but condition failed. Not starting unit.");
                return -EALREADY;
        }

        /* If the asserts failed, fail the entire job */
        if (state != UNIT_ACTIVATING &&
            !unit_assert_test(u)) {
                log_unit_notice(u, "Starting requested but asserts failed.");
                return -EPROTO;
        }

        /* Units of types that aren't supported cannot be
         * started. Note that we do this test only after the condition
         * checks, so that we rather return condition check errors
         * (which are usually not considered a true failure) than "not
         * supported" errors (which are considered a failure).
         */
        if (!unit_supported(u))
                return -EOPNOTSUPP;

        /* Let's make sure that the deps really are in order before we start this. Normally the job engine should have
         * taken care of this already, but let's check this here again. After all, our dependencies might not be in
         * effect anymore, due to a reload or due to a failed condition. */
        if (!unit_verify_deps(u))
                return -ENOLINK;

        /* Forward to the main object, if we aren't it. */
        following = unit_following(u);
        if (following) {
                log_unit_debug(u, "Redirecting start request from %s to %s.", u->id, following->id);
                return unit_start(following);
        }

        /* If it is stopped, but we cannot start it, then fail */
        if (!UNIT_VTABLE(u)->start)
                return -EBADR;

        /* We don't suppress calls to ->start() here when we are
         * already starting, to allow this request to be used as a
         * "hurry up" call, for example when the unit is in some "auto
         * restart" state where it waits for a holdoff timer to elapse
         * before it will start again. */

        unit_add_to_dbus_queue(u);

        return UNIT_VTABLE(u)->start(u);
}

bool unit_can_start(Unit *u) {
        assert(u);

        if (u->load_state != UNIT_LOADED)
                return false;

        if (!unit_supported(u))
                return false;

        return !!UNIT_VTABLE(u)->start;
}

bool unit_can_isolate(Unit *u) {
        assert(u);

        return unit_can_start(u) &&
                u->allow_isolate;
}

/* Errors:
 *         -EBADR:    This unit type does not support stopping.
 *         -EALREADY: Unit is already stopped.
 *         -EAGAIN:   An operation is already in progress. Retry later.
 */
int unit_stop(Unit *u) {
        UnitActiveState state;
        Unit *following;

        assert(u);

        state = unit_active_state(u);
        if (UNIT_IS_INACTIVE_OR_FAILED(state))
                return -EALREADY;

        following = unit_following(u);
        if (following) {
                log_unit_debug(u, "Redirecting stop request from %s to %s.", u->id, following->id);
                return unit_stop(following);
        }

        if (!UNIT_VTABLE(u)->stop)
                return -EBADR;

        unit_add_to_dbus_queue(u);

        return UNIT_VTABLE(u)->stop(u);
}

bool unit_can_stop(Unit *u) {
        assert(u);

        if (!unit_supported(u))
                return false;

        if (u->perpetual)
                return false;

        return !!UNIT_VTABLE(u)->stop;
}

/* Errors:
 *         -EBADR:    This unit type does not support reloading.
 *         -ENOEXEC:  Unit is not started.
 *         -EAGAIN:   An operation is already in progress. Retry later.
 */
int unit_reload(Unit *u) {
        UnitActiveState state;
        Unit *following;

        assert(u);

        if (u->load_state != UNIT_LOADED)
                return -EINVAL;

        if (!unit_can_reload(u))
                return -EBADR;

        state = unit_active_state(u);
        if (state == UNIT_RELOADING)
                return -EALREADY;

        if (state != UNIT_ACTIVE) {
                log_unit_warning(u, "Unit cannot be reloaded because it is inactive.");
                return -ENOEXEC;
        }

        following = unit_following(u);
        if (following) {
                log_unit_debug(u, "Redirecting reload request from %s to %s.", u->id, following->id);
                return unit_reload(following);
        }

        unit_add_to_dbus_queue(u);

        return UNIT_VTABLE(u)->reload(u);
}

bool unit_can_reload(Unit *u) {
        assert(u);

        if (!UNIT_VTABLE(u)->reload)
                return false;

        if (!UNIT_VTABLE(u)->can_reload)
                return true;

        return UNIT_VTABLE(u)->can_reload(u);
}

static void unit_check_unneeded(Unit *u) {

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;

        static const UnitDependency needed_dependencies[] = {
                UNIT_REQUIRED_BY,
                UNIT_REQUISITE_OF,
                UNIT_WANTED_BY,
                UNIT_BOUND_BY,
        };

        Unit *other;
        Iterator i;
        unsigned j;
        int r;

        assert(u);

        /* If this service shall be shut down when unneeded then do
         * so. */

        if (!u->stop_when_unneeded)
                return;

        if (!UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(u)))
                return;

        for (j = 0; j < ELEMENTSOF(needed_dependencies); j++)
                SET_FOREACH(other, u->dependencies[needed_dependencies[j]], i)
                        if (unit_active_or_pending(other))
                                return;

        /* If stopping a unit fails continuously we might enter a stop
         * loop here, hence stop acting on the service being
         * unnecessary after a while. */
        if (!ratelimit_test(&u->auto_stop_ratelimit)) {
                log_unit_warning(u, "Unit not needed anymore, but not stopping since we tried this too often recently.");
                return;
        }

        log_unit_info(u, "Unit not needed anymore. Stopping.");

        /* Ok, nobody needs us anymore. Sniff. Then let's commit suicide */
        r = manager_add_job(u->manager, JOB_STOP, u, JOB_FAIL, &error, NULL);
        if (r < 0)
                log_unit_warning_errno(u, r, "Failed to enqueue stop job, ignoring: %s", bus_error_message(&error, r));
}

static void unit_check_binds_to(Unit *u) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        bool stop = false;
        Unit *other;
        Iterator i;
        int r;

        assert(u);

        if (u->job)
                return;

        if (unit_active_state(u) != UNIT_ACTIVE)
                return;

        SET_FOREACH(other, u->dependencies[UNIT_BINDS_TO], i) {
                if (other->job)
                        continue;

                if (!other->coldplugged)
                        /* We might yet create a job for the other unit… */
                        continue;

                if (!UNIT_IS_INACTIVE_OR_FAILED(unit_active_state(other)))
                        continue;

                stop = true;
                break;
        }

        if (!stop)
                return;

        /* If stopping a unit fails continuously we might enter a stop
         * loop here, hence stop acting on the service being
         * unnecessary after a while. */
        if (!ratelimit_test(&u->auto_stop_ratelimit)) {
                log_unit_warning(u, "Unit is bound to inactive unit %s, but not stopping since we tried this too often recently.", other->id);
                return;
        }

        assert(other);
        log_unit_info(u, "Unit is bound to inactive unit %s. Stopping, too.", other->id);

        /* A unit we need to run is gone. Sniff. Let's stop this. */
        r = manager_add_job(u->manager, JOB_STOP, u, JOB_FAIL, &error, NULL);
        if (r < 0)
                log_unit_warning_errno(u, r, "Failed to enqueue stop job, ignoring: %s", bus_error_message(&error, r));
}

static void retroactively_start_dependencies(Unit *u) {
        Iterator i;
        Unit *other;

        assert(u);
        assert(UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(u)));

        SET_FOREACH(other, u->dependencies[UNIT_REQUIRES], i)
                if (!set_get(u->dependencies[UNIT_AFTER], other) &&
                    !UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(other)))
                        manager_add_job(u->manager, JOB_START, other, JOB_REPLACE, NULL, NULL);

        SET_FOREACH(other, u->dependencies[UNIT_BINDS_TO], i)
                if (!set_get(u->dependencies[UNIT_AFTER], other) &&
                    !UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(other)))
                        manager_add_job(u->manager, JOB_START, other, JOB_REPLACE, NULL, NULL);

        SET_FOREACH(other, u->dependencies[UNIT_WANTS], i)
                if (!set_get(u->dependencies[UNIT_AFTER], other) &&
                    !UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(other)))
                        manager_add_job(u->manager, JOB_START, other, JOB_FAIL, NULL, NULL);

        SET_FOREACH(other, u->dependencies[UNIT_CONFLICTS], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        manager_add_job(u->manager, JOB_STOP, other, JOB_REPLACE, NULL, NULL);

        SET_FOREACH(other, u->dependencies[UNIT_CONFLICTED_BY], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        manager_add_job(u->manager, JOB_STOP, other, JOB_REPLACE, NULL, NULL);
}

static void retroactively_stop_dependencies(Unit *u) {
        Iterator i;
        Unit *other;

        assert(u);
        assert(UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(u)));

        /* Pull down units which are bound to us recursively if enabled */
        SET_FOREACH(other, u->dependencies[UNIT_BOUND_BY], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        manager_add_job(u->manager, JOB_STOP, other, JOB_REPLACE, NULL, NULL);
}

static void check_unneeded_dependencies(Unit *u) {
        Iterator i;
        Unit *other;

        assert(u);
        assert(UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(u)));

        /* Garbage collect services that might not be needed anymore, if enabled */
        SET_FOREACH(other, u->dependencies[UNIT_REQUIRES], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        unit_check_unneeded(other);
        SET_FOREACH(other, u->dependencies[UNIT_WANTS], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        unit_check_unneeded(other);
        SET_FOREACH(other, u->dependencies[UNIT_REQUISITE], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        unit_check_unneeded(other);
        SET_FOREACH(other, u->dependencies[UNIT_BINDS_TO], i)
                if (!UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(other)))
                        unit_check_unneeded(other);
}

void unit_start_on_failure(Unit *u) {
        Unit *other;
        Iterator i;

        assert(u);

        if (set_size(u->dependencies[UNIT_ON_FAILURE]) <= 0)
                return;

        log_unit_info(u, "Triggering OnFailure= dependencies.");

        SET_FOREACH(other, u->dependencies[UNIT_ON_FAILURE], i) {
                int r;

                r = manager_add_job(u->manager, JOB_START, other, u->on_failure_job_mode, NULL, NULL);
                if (r < 0)
                        log_unit_error_errno(u, r, "Failed to enqueue OnFailure= job: %m");
        }
}

void unit_trigger_notify(Unit *u) {
        Unit *other;
        Iterator i;

        assert(u);

        SET_FOREACH(other, u->dependencies[UNIT_TRIGGERED_BY], i)
                if (UNIT_VTABLE(other)->trigger_notify)
                        UNIT_VTABLE(other)->trigger_notify(other, u);
}

void unit_notify(Unit *u, UnitActiveState os, UnitActiveState ns, bool reload_success) {
        Manager *m;
        bool unexpected;

        assert(u);
        assert(os < _UNIT_ACTIVE_STATE_MAX);
        assert(ns < _UNIT_ACTIVE_STATE_MAX);

        /* Note that this is called for all low-level state changes,
         * even if they might map to the same high-level
         * UnitActiveState! That means that ns == os is an expected
         * behavior here. For example: if a mount point is remounted
         * this function will be called too! */

        m = u->manager;

        /* Update timestamps for state changes */
        if (!MANAGER_IS_RELOADING(m)) {
                dual_timestamp_get(&u->state_change_timestamp);

                if (UNIT_IS_INACTIVE_OR_FAILED(os) && !UNIT_IS_INACTIVE_OR_FAILED(ns))
                        u->inactive_exit_timestamp = u->state_change_timestamp;
                else if (!UNIT_IS_INACTIVE_OR_FAILED(os) && UNIT_IS_INACTIVE_OR_FAILED(ns))
                        u->inactive_enter_timestamp = u->state_change_timestamp;

                if (!UNIT_IS_ACTIVE_OR_RELOADING(os) && UNIT_IS_ACTIVE_OR_RELOADING(ns))
                        u->active_enter_timestamp = u->state_change_timestamp;
                else if (UNIT_IS_ACTIVE_OR_RELOADING(os) && !UNIT_IS_ACTIVE_OR_RELOADING(ns))
                        u->active_exit_timestamp = u->state_change_timestamp;
        }

        /* Keep track of failed units */
        (void) manager_update_failed_units(u->manager, u, ns == UNIT_FAILED);

        /* Make sure the cgroup is always removed when we become inactive */
        if (UNIT_IS_INACTIVE_OR_FAILED(ns))
                unit_prune_cgroup(u);

        /* Note that this doesn't apply to RemainAfterExit services exiting
         * successfully, since there's no change of state in that case. Which is
         * why it is handled in service_set_state() */
        if (UNIT_IS_INACTIVE_OR_FAILED(os) != UNIT_IS_INACTIVE_OR_FAILED(ns)) {
                ExecContext *ec;

                ec = unit_get_exec_context(u);
                if (ec && exec_context_may_touch_console(ec)) {
                        if (UNIT_IS_INACTIVE_OR_FAILED(ns)) {
                                m->n_on_console--;

                                if (m->n_on_console == 0)
                                        /* unset no_console_output flag, since the console is free */
                                        m->no_console_output = false;
                        } else
                                m->n_on_console++;
                }
        }

        if (u->job) {
                unexpected = false;

                if (u->job->state == JOB_WAITING)

                        /* So we reached a different state for this
                         * job. Let's see if we can run it now if it
                         * failed previously due to EAGAIN. */
                        job_add_to_run_queue(u->job);

                /* Let's check whether this state change constitutes a
                 * finished job, or maybe contradicts a running job and
                 * hence needs to invalidate jobs. */

                switch (u->job->type) {

                case JOB_START:
                case JOB_VERIFY_ACTIVE:

                        if (UNIT_IS_ACTIVE_OR_RELOADING(ns))
                                job_finish_and_invalidate(u->job, JOB_DONE, true, false);
                        else if (u->job->state == JOB_RUNNING && ns != UNIT_ACTIVATING) {
                                unexpected = true;

                                if (UNIT_IS_INACTIVE_OR_FAILED(ns))
                                        job_finish_and_invalidate(u->job, ns == UNIT_FAILED ? JOB_FAILED : JOB_DONE, true, false);
                        }

                        break;

                case JOB_RELOAD:
                case JOB_RELOAD_OR_START:
                case JOB_TRY_RELOAD:

                        if (u->job->state == JOB_RUNNING) {
                                if (ns == UNIT_ACTIVE)
                                        job_finish_and_invalidate(u->job, reload_success ? JOB_DONE : JOB_FAILED, true, false);
                                else if (ns != UNIT_ACTIVATING && ns != UNIT_RELOADING) {
                                        unexpected = true;

                                        if (UNIT_IS_INACTIVE_OR_FAILED(ns))
                                                job_finish_and_invalidate(u->job, ns == UNIT_FAILED ? JOB_FAILED : JOB_DONE, true, false);
                                }
                        }

                        break;

                case JOB_STOP:
                case JOB_RESTART:
                case JOB_TRY_RESTART:

                        if (UNIT_IS_INACTIVE_OR_FAILED(ns))
                                job_finish_and_invalidate(u->job, JOB_DONE, true, false);
                        else if (u->job->state == JOB_RUNNING && ns != UNIT_DEACTIVATING) {
                                unexpected = true;
                                job_finish_and_invalidate(u->job, JOB_FAILED, true, false);
                        }

                        break;

                default:
                        assert_not_reached("Job type unknown");
                }

        } else
                unexpected = true;

        if (!MANAGER_IS_RELOADING(m)) {

                /* If this state change happened without being
                 * requested by a job, then let's retroactively start
                 * or stop dependencies. We skip that step when
                 * deserializing, since we don't want to create any
                 * additional jobs just because something is already
                 * activated. */

                if (unexpected) {
                        if (UNIT_IS_INACTIVE_OR_FAILED(os) && UNIT_IS_ACTIVE_OR_ACTIVATING(ns))
                                retroactively_start_dependencies(u);
                        else if (UNIT_IS_ACTIVE_OR_ACTIVATING(os) && UNIT_IS_INACTIVE_OR_DEACTIVATING(ns))
                                retroactively_stop_dependencies(u);
                }

                /* stop unneeded units regardless if going down was expected or not */
                if (UNIT_IS_INACTIVE_OR_DEACTIVATING(ns))
                        check_unneeded_dependencies(u);

                if (ns != os && ns == UNIT_FAILED) {
                        log_unit_notice(u, "Unit entered failed state.");
                        unit_start_on_failure(u);
                }
        }

        /* Some names are special */
        if (UNIT_IS_ACTIVE_OR_RELOADING(ns)) {

                if (unit_has_name(u, SPECIAL_DBUS_SERVICE))
                        /* The bus might have just become available,
                         * hence try to connect to it, if we aren't
                         * yet connected. */
                        bus_init(m, true);

                if (u->type == UNIT_SERVICE &&
                    !UNIT_IS_ACTIVE_OR_RELOADING(os) &&
                    !MANAGER_IS_RELOADING(m)) {
                        /* Write audit record if we have just finished starting up */
                        manager_send_unit_audit(m, u, AUDIT_SERVICE_START, true);
                        u->in_audit = true;
                }

                if (!UNIT_IS_ACTIVE_OR_RELOADING(os))
                        manager_send_unit_plymouth(m, u);

        } else {

                /* We don't care about D-Bus here, since we'll get an
                 * asynchronous notification for it anyway. */

                if (u->type == UNIT_SERVICE &&
                    UNIT_IS_INACTIVE_OR_FAILED(ns) &&
                    !UNIT_IS_INACTIVE_OR_FAILED(os) &&
                    !MANAGER_IS_RELOADING(m)) {

                        /* Hmm, if there was no start record written
                         * write it now, so that we always have a nice
                         * pair */
                        if (!u->in_audit) {
                                manager_send_unit_audit(m, u, AUDIT_SERVICE_START, ns == UNIT_INACTIVE);

                                if (ns == UNIT_INACTIVE)
                                        manager_send_unit_audit(m, u, AUDIT_SERVICE_STOP, true);
                        } else
                                /* Write audit record if we have just finished shutting down */
                                manager_send_unit_audit(m, u, AUDIT_SERVICE_STOP, ns == UNIT_INACTIVE);

                        u->in_audit = false;
                }
        }

        manager_recheck_journal(m);
        unit_trigger_notify(u);

        if (!MANAGER_IS_RELOADING(u->manager)) {
                /* Maybe we finished startup and are now ready for
                 * being stopped because unneeded? */
                unit_check_unneeded(u);

                /* Maybe we finished startup, but something we needed
                 * has vanished? Let's die then. (This happens when
                 * something BindsTo= to a Type=oneshot unit, as these
                 * units go directly from starting to inactive,
                 * without ever entering started.) */
                unit_check_binds_to(u);
        }

        unit_add_to_dbus_queue(u);
        unit_add_to_gc_queue(u);
}

int unit_watch_pid(Unit *u, pid_t pid) {
        int q, r;

        assert(u);
        assert(pid >= 1);

        /* Watch a specific PID. We only support one or two units
         * watching each PID for now, not more. */

        r = set_ensure_allocated(&u->pids, NULL);
        if (r < 0)
                return r;

        r = hashmap_ensure_allocated(&u->manager->watch_pids1, NULL);
        if (r < 0)
                return r;

        r = hashmap_put(u->manager->watch_pids1, PID_TO_PTR(pid), u);
        if (r == -EEXIST) {
                r = hashmap_ensure_allocated(&u->manager->watch_pids2, NULL);
                if (r < 0)
                        return r;

                r = hashmap_put(u->manager->watch_pids2, PID_TO_PTR(pid), u);
        }

        q = set_put(u->pids, PID_TO_PTR(pid));
        if (q < 0)
                return q;

        return r;
}

void unit_unwatch_pid(Unit *u, pid_t pid) {
        assert(u);
        assert(pid >= 1);

        (void) hashmap_remove_value(u->manager->watch_pids1, PID_TO_PTR(pid), u);
        (void) hashmap_remove_value(u->manager->watch_pids2, PID_TO_PTR(pid), u);
        (void) set_remove(u->pids, PID_TO_PTR(pid));
}

void unit_unwatch_all_pids(Unit *u) {
        assert(u);

        while (!set_isempty(u->pids))
                unit_unwatch_pid(u, PTR_TO_PID(set_first(u->pids)));

        u->pids = set_free(u->pids);
}

void unit_tidy_watch_pids(Unit *u, pid_t except1, pid_t except2) {
        Iterator i;
        void *e;

        assert(u);

        /* Cleans dead PIDs from our list */

        SET_FOREACH(e, u->pids, i) {
                pid_t pid = PTR_TO_PID(e);

                if (pid == except1 || pid == except2)
                        continue;

                if (!pid_is_unwaited(pid))
                        unit_unwatch_pid(u, pid);
        }
}

bool unit_job_is_applicable(Unit *u, JobType j) {
        assert(u);
        assert(j >= 0 && j < _JOB_TYPE_MAX);

        switch (j) {

        case JOB_VERIFY_ACTIVE:
        case JOB_START:
        case JOB_NOP:
                /* Note that we don't check unit_can_start() here. That's because .device units and suchlike are not
                 * startable by us but may appear due to external events, and it thus makes sense to permit enqueing
                 * jobs for it. */
                return true;

        case JOB_STOP:
                /* Similar as above. However, perpetual units can never be stopped (neither explicitly nor due to
                 * external events), hence it makes no sense to permit enqueing such a request either. */
                return !u->perpetual;

        case JOB_RESTART:
        case JOB_TRY_RESTART:
                return unit_can_stop(u) && unit_can_start(u);

        case JOB_RELOAD:
        case JOB_TRY_RELOAD:
                return unit_can_reload(u);

        case JOB_RELOAD_OR_START:
                return unit_can_reload(u) && unit_can_start(u);

        default:
                assert_not_reached("Invalid job type");
        }
}

static void maybe_warn_about_dependency(Unit *u, const char *other, UnitDependency dependency) {
        assert(u);

        /* Only warn about some unit types */
        if (!IN_SET(dependency, UNIT_CONFLICTS, UNIT_CONFLICTED_BY, UNIT_BEFORE, UNIT_AFTER, UNIT_ON_FAILURE, UNIT_TRIGGERS, UNIT_TRIGGERED_BY))
                return;

        if (streq_ptr(u->id, other))
                log_unit_warning(u, "Dependency %s=%s dropped", unit_dependency_to_string(dependency), u->id);
        else
                log_unit_warning(u, "Dependency %s=%s dropped, merged into %s", unit_dependency_to_string(dependency), strna(other), u->id);
}

int unit_add_dependency(Unit *u, UnitDependency d, Unit *other, bool add_reference) {

        static const UnitDependency inverse_table[_UNIT_DEPENDENCY_MAX] = {
                [UNIT_REQUIRES] = UNIT_REQUIRED_BY,
                [UNIT_WANTS] = UNIT_WANTED_BY,
                [UNIT_REQUISITE] = UNIT_REQUISITE_OF,
                [UNIT_BINDS_TO] = UNIT_BOUND_BY,
                [UNIT_PART_OF] = UNIT_CONSISTS_OF,
                [UNIT_REQUIRED_BY] = UNIT_REQUIRES,
                [UNIT_REQUISITE_OF] = UNIT_REQUISITE,
                [UNIT_WANTED_BY] = UNIT_WANTS,
                [UNIT_BOUND_BY] = UNIT_BINDS_TO,
                [UNIT_CONSISTS_OF] = UNIT_PART_OF,
                [UNIT_CONFLICTS] = UNIT_CONFLICTED_BY,
                [UNIT_CONFLICTED_BY] = UNIT_CONFLICTS,
                [UNIT_BEFORE] = UNIT_AFTER,
                [UNIT_AFTER] = UNIT_BEFORE,
                [UNIT_ON_FAILURE] = _UNIT_DEPENDENCY_INVALID,
                [UNIT_REFERENCES] = UNIT_REFERENCED_BY,
                [UNIT_REFERENCED_BY] = UNIT_REFERENCES,
                [UNIT_TRIGGERS] = UNIT_TRIGGERED_BY,
                [UNIT_TRIGGERED_BY] = UNIT_TRIGGERS,
                [UNIT_PROPAGATES_RELOAD_TO] = UNIT_RELOAD_PROPAGATED_FROM,
                [UNIT_RELOAD_PROPAGATED_FROM] = UNIT_PROPAGATES_RELOAD_TO,
                [UNIT_JOINS_NAMESPACE_OF] = UNIT_JOINS_NAMESPACE_OF,
        };
        int r, q = 0, v = 0, w = 0;
        Unit *orig_u = u, *orig_other = other;

        assert(u);
        assert(d >= 0 && d < _UNIT_DEPENDENCY_MAX);
        assert(other);

        u = unit_follow_merge(u);
        other = unit_follow_merge(other);

        /* We won't allow dependencies on ourselves. We will not
         * consider them an error however. */
        if (u == other) {
                maybe_warn_about_dependency(orig_u, orig_other->id, d);
                return 0;
        }

        if (d == UNIT_BEFORE && other->type == UNIT_DEVICE) {
                log_unit_warning(u, "Dependency Before=%s ignored (.device units cannot be delayed)", other->id);
                return 0;
        }

        r = set_ensure_allocated(&u->dependencies[d], NULL);
        if (r < 0)
                return r;

        if (inverse_table[d] != _UNIT_DEPENDENCY_INVALID) {
                r = set_ensure_allocated(&other->dependencies[inverse_table[d]], NULL);
                if (r < 0)
                        return r;
        }

        if (add_reference) {
                r = set_ensure_allocated(&u->dependencies[UNIT_REFERENCES], NULL);
                if (r < 0)
                        return r;

                r = set_ensure_allocated(&other->dependencies[UNIT_REFERENCED_BY], NULL);
                if (r < 0)
                        return r;
        }

        q = set_put(u->dependencies[d], other);
        if (q < 0)
                return q;

        if (inverse_table[d] != _UNIT_DEPENDENCY_INVALID && inverse_table[d] != d) {
                v = set_put(other->dependencies[inverse_table[d]], u);
                if (v < 0) {
                        r = v;
                        goto fail;
                }
        }

        if (add_reference) {
                w = set_put(u->dependencies[UNIT_REFERENCES], other);
                if (w < 0) {
                        r = w;
                        goto fail;
                }

                r = set_put(other->dependencies[UNIT_REFERENCED_BY], u);
                if (r < 0)
                        goto fail;
        }

        unit_add_to_dbus_queue(u);
        return 0;

fail:
        if (q > 0)
                set_remove(u->dependencies[d], other);

        if (v > 0)
                set_remove(other->dependencies[inverse_table[d]], u);

        if (w > 0)
                set_remove(u->dependencies[UNIT_REFERENCES], other);

        return r;
}

int unit_add_two_dependencies(Unit *u, UnitDependency d, UnitDependency e, Unit *other, bool add_reference) {
        int r;

        assert(u);

        r = unit_add_dependency(u, d, other, add_reference);
        if (r < 0)
                return r;

        return unit_add_dependency(u, e, other, add_reference);
}

static int resolve_template(Unit *u, const char *name, const char*path, char **buf, const char **ret) {
        int r;

        assert(u);
        assert(name || path);
        assert(buf);
        assert(ret);

        if (!name)
                name = basename(path);

        if (!unit_name_is_valid(name, UNIT_NAME_TEMPLATE)) {
                *buf = NULL;
                *ret = name;
                return 0;
        }

        if (u->instance)
                r = unit_name_replace_instance(name, u->instance, buf);
        else {
                _cleanup_free_ char *i = NULL;

                r = unit_name_to_prefix(u->id, &i);
                if (r < 0)
                        return r;

                r = unit_name_replace_instance(name, i, buf);
        }
        if (r < 0)
                return r;

        *ret = *buf;
        return 0;
}

int unit_add_dependency_by_name(Unit *u, UnitDependency d, const char *name, const char *path, bool add_reference) {
        _cleanup_free_ char *buf = NULL;
        Unit *other;
        int r;

        assert(u);
        assert(name || path);

        r = resolve_template(u, name, path, &buf, &name);
        if (r < 0)
                return r;

        r = manager_load_unit(u->manager, name, path, NULL, &other);
        if (r < 0)
                return r;

        return unit_add_dependency(u, d, other, add_reference);
}

int unit_add_two_dependencies_by_name(Unit *u, UnitDependency d, UnitDependency e, const char *name, const char *path, bool add_reference) {
        _cleanup_free_ char *buf = NULL;
        Unit *other;
        int r;

        assert(u);
        assert(name || path);

        r = resolve_template(u, name, path, &buf, &name);
        if (r < 0)
                return r;

        r = manager_load_unit(u->manager, name, path, NULL, &other);
        if (r < 0)
                return r;

        return unit_add_two_dependencies(u, d, e, other, add_reference);
}

int set_unit_path(const char *p) {
        /* This is mostly for debug purposes */
        if (setenv("SYSTEMD_UNIT_PATH", p, 1) < 0)
                return -errno;

        return 0;
}

char *unit_dbus_path(Unit *u) {
        assert(u);

        if (!u->id)
                return NULL;

        return unit_dbus_path_from_name(u->id);
}

char *unit_dbus_path_invocation_id(Unit *u) {
        assert(u);

        if (sd_id128_is_null(u->invocation_id))
                return NULL;

        return unit_dbus_path_from_name(u->invocation_id_string);
}

int unit_set_slice(Unit *u, Unit *slice) {
        assert(u);
        assert(slice);

        /* Sets the unit slice if it has not been set before. Is extra
         * careful, to only allow this for units that actually have a
         * cgroup context. Also, we don't allow to set this for slices
         * (since the parent slice is derived from the name). Make
         * sure the unit we set is actually a slice. */

        if (!UNIT_HAS_CGROUP_CONTEXT(u))
                return -EOPNOTSUPP;

        if (u->type == UNIT_SLICE)
                return -EINVAL;

        if (unit_active_state(u) != UNIT_INACTIVE)
                return -EBUSY;

        if (slice->type != UNIT_SLICE)
                return -EINVAL;

        if (unit_has_name(u, SPECIAL_INIT_SCOPE) &&
            !unit_has_name(slice, SPECIAL_ROOT_SLICE))
                return -EPERM;

        if (UNIT_DEREF(u->slice) == slice)
                return 0;

        /* Disallow slice changes if @u is already bound to cgroups */
        if (UNIT_ISSET(u->slice) && u->cgroup_realized)
                return -EBUSY;

        unit_ref_unset(&u->slice);
        unit_ref_set(&u->slice, slice);
        return 1;
}

int unit_set_default_slice(Unit *u) {
        _cleanup_free_ char *b = NULL;
        const char *slice_name;
        Unit *slice;
        int r;

        assert(u);

        if (UNIT_ISSET(u->slice))
                return 0;

        if (u->instance) {
                _cleanup_free_ char *prefix = NULL, *escaped = NULL;

                /* Implicitly place all instantiated units in their
                 * own per-template slice */

                r = unit_name_to_prefix(u->id, &prefix);
                if (r < 0)
                        return r;

                /* The prefix is already escaped, but it might include
                 * "-" which has a special meaning for slice units,
                 * hence escape it here extra. */
                escaped = unit_name_escape(prefix);
                if (!escaped)
                        return -ENOMEM;

                if (MANAGER_IS_SYSTEM(u->manager))
                        b = strjoin("system-", escaped, ".slice");
                else
                        b = strappend(escaped, ".slice");
                if (!b)
                        return -ENOMEM;

                slice_name = b;
        } else
                slice_name =
                        MANAGER_IS_SYSTEM(u->manager) && !unit_has_name(u, SPECIAL_INIT_SCOPE)
                        ? SPECIAL_SYSTEM_SLICE
                        : SPECIAL_ROOT_SLICE;

        r = manager_load_unit(u->manager, slice_name, NULL, NULL, &slice);
        if (r < 0)
                return r;

        return unit_set_slice(u, slice);
}

const char *unit_slice_name(Unit *u) {
        assert(u);

        if (!UNIT_ISSET(u->slice))
                return NULL;

        return UNIT_DEREF(u->slice)->id;
}

int unit_load_related_unit(Unit *u, const char *type, Unit **_found) {
        _cleanup_free_ char *t = NULL;
        int r;

        assert(u);
        assert(type);
        assert(_found);

        r = unit_name_change_suffix(u->id, type, &t);
        if (r < 0)
                return r;
        if (unit_has_name(u, t))
                return -EINVAL;

        r = manager_load_unit(u->manager, t, NULL, NULL, _found);
        assert(r < 0 || *_found != u);
        return r;
}

static int signal_name_owner_changed(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        const char *name, *old_owner, *new_owner;
        Unit *u = userdata;
        int r;

        assert(message);
        assert(u);

        r = sd_bus_message_read(message, "sss", &name, &old_owner, &new_owner);
        if (r < 0) {
                bus_log_parse_error(r);
                return 0;
        }

        old_owner = isempty(old_owner) ? NULL : old_owner;
        new_owner = isempty(new_owner) ? NULL : new_owner;

        if (UNIT_VTABLE(u)->bus_name_owner_change)
                UNIT_VTABLE(u)->bus_name_owner_change(u, name, old_owner, new_owner);

        return 0;
}

int unit_install_bus_match(Unit *u, sd_bus *bus, const char *name) {
        const char *match;

        assert(u);
        assert(bus);
        assert(name);

        if (u->match_bus_slot)
                return -EBUSY;

        match = strjoina("type='signal',"
                         "sender='org.freedesktop.DBus',"
                         "path='/org/freedesktop/DBus',"
                         "interface='org.freedesktop.DBus',"
                         "member='NameOwnerChanged',"
                         "arg0='", name, "'");

        return sd_bus_add_match(bus, &u->match_bus_slot, match, signal_name_owner_changed, u);
}

int unit_watch_bus_name(Unit *u, const char *name) {
        int r;

        assert(u);
        assert(name);

        /* Watch a specific name on the bus. We only support one unit
         * watching each name for now. */

        if (u->manager->api_bus) {
                /* If the bus is already available, install the match directly.
                 * Otherwise, just put the name in the list. bus_setup_api() will take care later. */
                r = unit_install_bus_match(u, u->manager->api_bus, name);
                if (r < 0)
                        return log_warning_errno(r, "Failed to subscribe to NameOwnerChanged signal for '%s': %m", name);
        }

        r = hashmap_put(u->manager->watch_bus, name, u);
        if (r < 0) {
                u->match_bus_slot = sd_bus_slot_unref(u->match_bus_slot);
                return log_warning_errno(r, "Failed to put bus name to hashmap: %m");
        }

        return 0;
}

void unit_unwatch_bus_name(Unit *u, const char *name) {
        assert(u);
        assert(name);

        (void) hashmap_remove_value(u->manager->watch_bus, name, u);
        u->match_bus_slot = sd_bus_slot_unref(u->match_bus_slot);
}

bool unit_can_serialize(Unit *u) {
        assert(u);

        return UNIT_VTABLE(u)->serialize && UNIT_VTABLE(u)->deserialize_item;
}

static int unit_serialize_cgroup_mask(FILE *f, const char *key, CGroupMask mask) {
        _cleanup_free_ char *s = NULL;
        int r = 0;

        assert(f);
        assert(key);

        if (mask != 0) {
                r = cg_mask_to_string(mask, &s);
                if (r >= 0) {
                        fputs(key, f);
                        fputc('=', f);
                        fputs(s, f);
                        fputc('\n', f);
                }
        }
        return r;
}

int unit_serialize(Unit *u, FILE *f, FDSet *fds, bool serialize_jobs) {
        int r;

        assert(u);
        assert(f);
        assert(fds);

        if (unit_can_serialize(u)) {
                ExecRuntime *rt;

                r = UNIT_VTABLE(u)->serialize(u, f, fds);
                if (r < 0)
                        return r;

                rt = unit_get_exec_runtime(u);
                if (rt) {
                        r = exec_runtime_serialize(u, rt, f, fds);
                        if (r < 0)
                                return r;
                }
        }

        dual_timestamp_serialize(f, "state-change-timestamp", &u->state_change_timestamp);

        dual_timestamp_serialize(f, "inactive-exit-timestamp", &u->inactive_exit_timestamp);
        dual_timestamp_serialize(f, "active-enter-timestamp", &u->active_enter_timestamp);
        dual_timestamp_serialize(f, "active-exit-timestamp", &u->active_exit_timestamp);
        dual_timestamp_serialize(f, "inactive-enter-timestamp", &u->inactive_enter_timestamp);

        dual_timestamp_serialize(f, "condition-timestamp", &u->condition_timestamp);
        dual_timestamp_serialize(f, "assert-timestamp", &u->assert_timestamp);

        if (dual_timestamp_is_set(&u->condition_timestamp))
                unit_serialize_item(u, f, "condition-result", yes_no(u->condition_result));

        if (dual_timestamp_is_set(&u->assert_timestamp))
                unit_serialize_item(u, f, "assert-result", yes_no(u->assert_result));

        unit_serialize_item(u, f, "transient", yes_no(u->transient));

        unit_serialize_item_format(u, f, "cpu-usage-base", "%" PRIu64, u->cpu_usage_base);
        if (u->cpu_usage_last != NSEC_INFINITY)
                unit_serialize_item_format(u, f, "cpu-usage-last", "%" PRIu64, u->cpu_usage_last);

        if (u->cgroup_path)
                unit_serialize_item(u, f, "cgroup", u->cgroup_path);
        unit_serialize_item(u, f, "cgroup-realized", yes_no(u->cgroup_realized));
        (void) unit_serialize_cgroup_mask(f, "cgroup-realized-mask", u->cgroup_realized_mask);
        (void) unit_serialize_cgroup_mask(f, "cgroup-enabled-mask", u->cgroup_enabled_mask);

        if (uid_is_valid(u->ref_uid))
                unit_serialize_item_format(u, f, "ref-uid", UID_FMT, u->ref_uid);
        if (gid_is_valid(u->ref_gid))
                unit_serialize_item_format(u, f, "ref-gid", GID_FMT, u->ref_gid);

        if (!sd_id128_is_null(u->invocation_id))
                unit_serialize_item_format(u, f, "invocation-id", SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(u->invocation_id));

        bus_track_serialize(u->bus_track, f, "ref");

        if (serialize_jobs) {
                if (u->job) {
                        fprintf(f, "job\n");
                        job_serialize(u->job, f);
                }

                if (u->nop_job) {
                        fprintf(f, "job\n");
                        job_serialize(u->nop_job, f);
                }
        }

        /* End marker */
        fputc('\n', f);
        return 0;
}

int unit_serialize_item(Unit *u, FILE *f, const char *key, const char *value) {
        assert(u);
        assert(f);
        assert(key);

        if (!value)
                return 0;

        fputs(key, f);
        fputc('=', f);
        fputs(value, f);
        fputc('\n', f);

        return 1;
}

int unit_serialize_item_escaped(Unit *u, FILE *f, const char *key, const char *value) {
        _cleanup_free_ char *c = NULL;

        assert(u);
        assert(f);
        assert(key);

        if (!value)
                return 0;

        c = cescape(value);
        if (!c)
                return -ENOMEM;

        fputs(key, f);
        fputc('=', f);
        fputs(c, f);
        fputc('\n', f);

        return 1;
}

int unit_serialize_item_fd(Unit *u, FILE *f, FDSet *fds, const char *key, int fd) {
        int copy;

        assert(u);
        assert(f);
        assert(key);

        if (fd < 0)
                return 0;

        copy = fdset_put_dup(fds, fd);
        if (copy < 0)
                return copy;

        fprintf(f, "%s=%i\n", key, copy);
        return 1;
}

void unit_serialize_item_format(Unit *u, FILE *f, const char *key, const char *format, ...) {
        va_list ap;

        assert(u);
        assert(f);
        assert(key);
        assert(format);

        fputs(key, f);
        fputc('=', f);

        va_start(ap, format);
        vfprintf(f, format, ap);
        va_end(ap);

        fputc('\n', f);
}

int unit_deserialize(Unit *u, FILE *f, FDSet *fds) {
        ExecRuntime **rt = NULL;
        size_t offset;
        int r;

        assert(u);
        assert(f);
        assert(fds);

        offset = UNIT_VTABLE(u)->exec_runtime_offset;
        if (offset > 0)
                rt = (ExecRuntime**) ((uint8_t*) u + offset);

        for (;;) {
                char line[LINE_MAX], *l, *v;
                size_t k;

                if (!fgets(line, sizeof(line), f)) {
                        if (feof(f))
                                return 0;
                        return -errno;
                }

                char_array_0(line);
                l = strstrip(line);

                /* End marker */
                if (isempty(l))
                        break;

                k = strcspn(l, "=");

                if (l[k] == '=') {
                        l[k] = 0;
                        v = l+k+1;
                } else
                        v = l+k;

                if (streq(l, "job")) {
                        if (v[0] == '\0') {
                                /* new-style serialized job */
                                Job *j;

                                j = job_new_raw(u);
                                if (!j)
                                        return log_oom();

                                r = job_deserialize(j, f);
                                if (r < 0) {
                                        job_free(j);
                                        return r;
                                }

                                r = hashmap_put(u->manager->jobs, UINT32_TO_PTR(j->id), j);
                                if (r < 0) {
                                        job_free(j);
                                        return r;
                                }

                                r = job_install_deserialized(j);
                                if (r < 0) {
                                        hashmap_remove(u->manager->jobs, UINT32_TO_PTR(j->id));
                                        job_free(j);
                                        return r;
                                }
                        } else  /* legacy for pre-44 */
                                log_unit_warning(u, "Update from too old systemd versions are unsupported, cannot deserialize job: %s", v);
                        continue;
                } else if (streq(l, "state-change-timestamp")) {
                        dual_timestamp_deserialize(v, &u->state_change_timestamp);
                        continue;
                } else if (streq(l, "inactive-exit-timestamp")) {
                        dual_timestamp_deserialize(v, &u->inactive_exit_timestamp);
                        continue;
                } else if (streq(l, "active-enter-timestamp")) {
                        dual_timestamp_deserialize(v, &u->active_enter_timestamp);
                        continue;
                } else if (streq(l, "active-exit-timestamp")) {
                        dual_timestamp_deserialize(v, &u->active_exit_timestamp);
                        continue;
                } else if (streq(l, "inactive-enter-timestamp")) {
                        dual_timestamp_deserialize(v, &u->inactive_enter_timestamp);
                        continue;
                } else if (streq(l, "condition-timestamp")) {
                        dual_timestamp_deserialize(v, &u->condition_timestamp);
                        continue;
                } else if (streq(l, "assert-timestamp")) {
                        dual_timestamp_deserialize(v, &u->assert_timestamp);
                        continue;
                } else if (streq(l, "condition-result")) {

                        r = parse_boolean(v);
                        if (r < 0)
                                log_unit_debug(u, "Failed to parse condition result value %s, ignoring.", v);
                        else
                                u->condition_result = r;

                        continue;

                } else if (streq(l, "assert-result")) {

                        r = parse_boolean(v);
                        if (r < 0)
                                log_unit_debug(u, "Failed to parse assert result value %s, ignoring.", v);
                        else
                                u->assert_result = r;

                        continue;

                } else if (streq(l, "transient")) {

                        r = parse_boolean(v);
                        if (r < 0)
                                log_unit_debug(u, "Failed to parse transient bool %s, ignoring.", v);
                        else
                                u->transient = r;

                        continue;

                } else if (STR_IN_SET(l, "cpu-usage-base", "cpuacct-usage-base")) {

                        r = safe_atou64(v, &u->cpu_usage_base);
                        if (r < 0)
                                log_unit_debug(u, "Failed to parse CPU usage base %s, ignoring.", v);

                        continue;

                } else if (streq(l, "cpu-usage-last")) {

                        r = safe_atou64(v, &u->cpu_usage_last);
                        if (r < 0)
                                log_unit_debug(u, "Failed to read CPU usage last %s, ignoring.", v);

                        continue;

                } else if (streq(l, "cgroup")) {

                        r = unit_set_cgroup_path(u, v);
                        if (r < 0)
                                log_unit_debug_errno(u, r, "Failed to set cgroup path %s, ignoring: %m", v);

                        (void) unit_watch_cgroup(u);

                        continue;
                } else if (streq(l, "cgroup-realized")) {
                        int b;

                        b = parse_boolean(v);
                        if (b < 0)
                                log_unit_debug(u, "Failed to parse cgroup-realized bool %s, ignoring.", v);
                        else
                                u->cgroup_realized = b;

                        continue;

                } else if (streq(l, "cgroup-realized-mask")) {

                        r = cg_mask_from_string(v, &u->cgroup_realized_mask);
                        if (r < 0)
                                log_unit_debug(u, "Failed to parse cgroup-realized-mask %s, ignoring.", v);
                        continue;

                } else if (streq(l, "cgroup-enabled-mask")) {

                        r = cg_mask_from_string(v, &u->cgroup_enabled_mask);
                        if (r < 0)
                                log_unit_debug(u, "Failed to parse cgroup-enabled-mask %s, ignoring.", v);
                        continue;

                } else if (streq(l, "ref-uid")) {
                        uid_t uid;

                        r = parse_uid(v, &uid);
                        if (r < 0)
                                log_unit_debug(u, "Failed to parse referenced UID %s, ignoring.", v);
                        else
                                unit_ref_uid_gid(u, uid, GID_INVALID);

                        continue;

                } else if (streq(l, "ref-gid")) {
                        gid_t gid;

                        r = parse_gid(v, &gid);
                        if (r < 0)
                                log_unit_debug(u, "Failed to parse referenced GID %s, ignoring.", v);
                        else
                                unit_ref_uid_gid(u, UID_INVALID, gid);

                } else if (streq(l, "ref")) {

                        r = strv_extend(&u->deserialized_refs, v);
                        if (r < 0)
                                log_oom();

                        continue;
                } else if (streq(l, "invocation-id")) {
                        sd_id128_t id;

                        r = sd_id128_from_string(v, &id);
                        if (r < 0)
                                log_unit_debug(u, "Failed to parse invocation id %s, ignoring.", v);
                        else {
                                r = unit_set_invocation_id(u, id);
                                if (r < 0)
                                        log_unit_warning_errno(u, r, "Failed to set invocation ID for unit: %m");
                        }

                        continue;
                }

                if (unit_can_serialize(u)) {
                        if (rt) {
                                r = exec_runtime_deserialize_item(u, rt, l, v, fds);
                                if (r < 0) {
                                        log_unit_warning(u, "Failed to deserialize runtime parameter '%s', ignoring.", l);
                                        continue;
                                }

                                /* Returns positive if key was handled by the call */
                                if (r > 0)
                                        continue;
                        }

                        r = UNIT_VTABLE(u)->deserialize_item(u, l, v, fds);
                        if (r < 0)
                                log_unit_warning(u, "Failed to deserialize unit parameter '%s', ignoring.", l);
                }
        }

        /* Versions before 228 did not carry a state change timestamp. In this case, take the current time. This is
         * useful, so that timeouts based on this timestamp don't trigger too early, and is in-line with the logic from
         * before 228 where the base for timeouts was not persistent across reboots. */

        if (!dual_timestamp_is_set(&u->state_change_timestamp))
                dual_timestamp_get(&u->state_change_timestamp);

        return 0;
}

int unit_add_node_link(Unit *u, const char *what, bool wants, UnitDependency dep) {
        Unit *device;
        _cleanup_free_ char *e = NULL;
        int r;

        assert(u);

        /* Adds in links to the device node that this unit is based on */
        if (isempty(what))
                return 0;

        if (!is_device_path(what))
                return 0;

        /* When device units aren't supported (such as in a
         * container), don't create dependencies on them. */
        if (!unit_type_supported(UNIT_DEVICE))
                return 0;

        r = unit_name_from_path(what, ".device", &e);
        if (r < 0)
                return r;

        r = manager_load_unit(u->manager, e, NULL, NULL, &device);
        if (r < 0)
                return r;

        if (dep == UNIT_REQUIRES && device_shall_be_bound_by(device, u))
                dep = UNIT_BINDS_TO;

        r = unit_add_two_dependencies(u, UNIT_AFTER,
                                      MANAGER_IS_SYSTEM(u->manager) ? dep : UNIT_WANTS,
                                      device, true);
        if (r < 0)
                return r;

        if (wants) {
                r = unit_add_dependency(device, UNIT_WANTS, u, false);
                if (r < 0)
                        return r;
        }

        return 0;
}

int unit_coldplug(Unit *u) {
        int r = 0, q;
        char **i;

        assert(u);

        /* Make sure we don't enter a loop, when coldplugging
         * recursively. */
        if (u->coldplugged)
                return 0;

        u->coldplugged = true;

        STRV_FOREACH(i, u->deserialized_refs) {
                q = bus_unit_track_add_name(u, *i);
                if (q < 0 && r >= 0)
                        r = q;
        }
        u->deserialized_refs = strv_free(u->deserialized_refs);

        if (UNIT_VTABLE(u)->coldplug) {
                q = UNIT_VTABLE(u)->coldplug(u);
                if (q < 0 && r >= 0)
                        r = q;
        }

        if (u->job) {
                q = job_coldplug(u->job);
                if (q < 0 && r >= 0)
                        r = q;
        }

        return r;
}

static bool fragment_mtime_newer(const char *path, usec_t mtime, bool path_masked) {
        struct stat st;

        if (!path)
                return false;

        /* If the source is some virtual kernel file system, then we assume we watch it anyway, and hence pretend we
         * are never out-of-date. */
        if (PATH_STARTSWITH_SET(path, "/proc", "/sys"))
                return false;

        if (stat(path, &st) < 0)
                /* What, cannot access this anymore? */
                return true;

        if (path_masked)
                /* For masked files check if they are still so */
                return !null_or_empty(&st);
        else
                /* For non-empty files check the mtime */
                return timespec_load(&st.st_mtim) > mtime;

        return false;
}

bool unit_need_daemon_reload(Unit *u) {
        _cleanup_strv_free_ char **t = NULL;
        char **path;

        assert(u);

        /* For unit files, we allow masking… */
        if (fragment_mtime_newer(u->fragment_path, u->fragment_mtime,
                                 u->load_state == UNIT_MASKED))
                return true;

        /* Source paths should not be masked… */
        if (fragment_mtime_newer(u->source_path, u->source_mtime, false))
                return true;

        (void) unit_find_dropin_paths(u, &t);
        if (!strv_equal(u->dropin_paths, t))
                return true;

        /* … any drop-ins that are masked are simply omitted from the list. */
        STRV_FOREACH(path, u->dropin_paths)
                if (fragment_mtime_newer(*path, u->dropin_mtime, false))
                        return true;

        return false;
}

void unit_reset_failed(Unit *u) {
        assert(u);

        if (UNIT_VTABLE(u)->reset_failed)
                UNIT_VTABLE(u)->reset_failed(u);

        RATELIMIT_RESET(u->start_limit);
        u->start_limit_hit = false;
}

Unit *unit_following(Unit *u) {
        assert(u);

        if (UNIT_VTABLE(u)->following)
                return UNIT_VTABLE(u)->following(u);

        return NULL;
}

bool unit_stop_pending(Unit *u) {
        assert(u);

        /* This call does check the current state of the unit. It's
         * hence useful to be called from state change calls of the
         * unit itself, where the state isn't updated yet. This is
         * different from unit_inactive_or_pending() which checks both
         * the current state and for a queued job. */

        return u->job && u->job->type == JOB_STOP;
}

bool unit_inactive_or_pending(Unit *u) {
        assert(u);

        /* Returns true if the unit is inactive or going down */

        if (UNIT_IS_INACTIVE_OR_DEACTIVATING(unit_active_state(u)))
                return true;

        if (unit_stop_pending(u))
                return true;

        return false;
}

bool unit_active_or_pending(Unit *u) {
        assert(u);

        /* Returns true if the unit is active or going up */

        if (UNIT_IS_ACTIVE_OR_ACTIVATING(unit_active_state(u)))
                return true;

        if (u->job &&
            (u->job->type == JOB_START ||
             u->job->type == JOB_RELOAD_OR_START ||
             u->job->type == JOB_RESTART))
                return true;

        return false;
}

int unit_kill(Unit *u, KillWho w, int signo, sd_bus_error *error) {
        assert(u);
        assert(w >= 0 && w < _KILL_WHO_MAX);
        assert(SIGNAL_VALID(signo));

        if (!UNIT_VTABLE(u)->kill)
                return -EOPNOTSUPP;

        return UNIT_VTABLE(u)->kill(u, w, signo, error);
}

static Set *unit_pid_set(pid_t main_pid, pid_t control_pid) {
        Set *pid_set;
        int r;

        pid_set = set_new(NULL);
        if (!pid_set)
                return NULL;

        /* Exclude the main/control pids from being killed via the cgroup */
        if (main_pid > 0) {
                r = set_put(pid_set, PID_TO_PTR(main_pid));
                if (r < 0)
                        goto fail;
        }

        if (control_pid > 0) {
                r = set_put(pid_set, PID_TO_PTR(control_pid));
                if (r < 0)
                        goto fail;
        }

        return pid_set;

fail:
        set_free(pid_set);
        return NULL;
}

int unit_kill_common(
                Unit *u,
                KillWho who,
                int signo,
                pid_t main_pid,
                pid_t control_pid,
                sd_bus_error *error) {

        int r = 0;
        bool killed = false;

        if (IN_SET(who, KILL_MAIN, KILL_MAIN_FAIL)) {
                if (main_pid < 0)
                        return sd_bus_error_setf(error, BUS_ERROR_NO_SUCH_PROCESS, "%s units have no main processes", unit_type_to_string(u->type));
                else if (main_pid == 0)
                        return sd_bus_error_set_const(error, BUS_ERROR_NO_SUCH_PROCESS, "No main process to kill");
        }

        if (IN_SET(who, KILL_CONTROL, KILL_CONTROL_FAIL)) {
                if (control_pid < 0)
                        return sd_bus_error_setf(error, BUS_ERROR_NO_SUCH_PROCESS, "%s units have no control processes", unit_type_to_string(u->type));
                else if (control_pid == 0)
                        return sd_bus_error_set_const(error, BUS_ERROR_NO_SUCH_PROCESS, "No control process to kill");
        }

        if (IN_SET(who, KILL_CONTROL, KILL_CONTROL_FAIL, KILL_ALL, KILL_ALL_FAIL))
                if (control_pid > 0) {
                        if (kill(control_pid, signo) < 0)
                                r = -errno;
                        else
                                killed = true;
                }

        if (IN_SET(who, KILL_MAIN, KILL_MAIN_FAIL, KILL_ALL, KILL_ALL_FAIL))
                if (main_pid > 0) {
                        if (kill(main_pid, signo) < 0)
                                r = -errno;
                        else
                                killed = true;
                }

        if (IN_SET(who, KILL_ALL, KILL_ALL_FAIL) && u->cgroup_path) {
                _cleanup_set_free_ Set *pid_set = NULL;
                int q;

                /* Exclude the main/control pids from being killed via the cgroup */
                pid_set = unit_pid_set(main_pid, control_pid);
                if (!pid_set)
                        return -ENOMEM;

                q = cg_kill_recursive(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path, signo, 0, pid_set, NULL, NULL);
                if (q < 0 && q != -EAGAIN && q != -ESRCH && q != -ENOENT)
                        r = q;
                else
                        killed = true;
        }

        if (r == 0 && !killed && IN_SET(who, KILL_ALL_FAIL, KILL_CONTROL_FAIL))
                return -ESRCH;

        return r;
}

int unit_following_set(Unit *u, Set **s) {
        assert(u);
        assert(s);

        if (UNIT_VTABLE(u)->following_set)
                return UNIT_VTABLE(u)->following_set(u, s);

        *s = NULL;
        return 0;
}

UnitFileState unit_get_unit_file_state(Unit *u) {
        int r;

        assert(u);

        if (u->unit_file_state < 0 && u->fragment_path) {
                r = unit_file_get_state(
                                u->manager->unit_file_scope,
                                NULL,
                                u->id,
                                &u->unit_file_state);
                if (r < 0)
                        u->unit_file_state = UNIT_FILE_BAD;
        }

        return u->unit_file_state;
}

int unit_get_unit_file_preset(Unit *u) {
        assert(u);

        if (u->unit_file_preset < 0 && u->fragment_path)
                u->unit_file_preset = unit_file_query_preset(
                                u->manager->unit_file_scope,
                                NULL,
                                basename(u->fragment_path));

        return u->unit_file_preset;
}

Unit* unit_ref_set(UnitRef *ref, Unit *u) {
        assert(ref);
        assert(u);

        if (ref->unit)
                unit_ref_unset(ref);

        ref->unit = u;
        LIST_PREPEND(refs, u->refs, ref);
        return u;
}

void unit_ref_unset(UnitRef *ref) {
        assert(ref);

        if (!ref->unit)
                return;

        /* We are about to drop a reference to the unit, make sure the garbage collection has a look at it as it might
         * be unreferenced now. */
        unit_add_to_gc_queue(ref->unit);

        LIST_REMOVE(refs, ref->unit->refs, ref);
        ref->unit = NULL;
}

static int user_from_unit_name(Unit *u, char **ret) {

        static const uint8_t hash_key[] = {
                0x58, 0x1a, 0xaf, 0xe6, 0x28, 0x58, 0x4e, 0x96,
                0xb4, 0x4e, 0xf5, 0x3b, 0x8c, 0x92, 0x07, 0xec
        };

        _cleanup_free_ char *n = NULL;
        int r;

        r = unit_name_to_prefix(u->id, &n);
        if (r < 0)
                return r;

        if (valid_user_group_name(n)) {
                *ret = n;
                n = NULL;
                return 0;
        }

        /* If we can't use the unit name as a user name, then let's hash it and use that */
        if (asprintf(ret, "_du%016" PRIx64, siphash24(n, strlen(n), hash_key)) < 0)
                return -ENOMEM;

        return 0;
}

int unit_patch_contexts(Unit *u) {
        CGroupContext *cc;
        ExecContext *ec;
        unsigned i;
        int r;

        assert(u);

        /* Patch in the manager defaults into the exec and cgroup
         * contexts, _after_ the rest of the settings have been
         * initialized */

        ec = unit_get_exec_context(u);
        if (ec) {
                /* This only copies in the ones that need memory */
                for (i = 0; i < _RLIMIT_MAX; i++)
                        if (u->manager->rlimit[i] && !ec->rlimit[i]) {
                                ec->rlimit[i] = newdup(struct rlimit, u->manager->rlimit[i], 1);
                                if (!ec->rlimit[i])
                                        return -ENOMEM;
                        }

                if (MANAGER_IS_USER(u->manager) &&
                    !ec->working_directory) {

                        r = get_home_dir(&ec->working_directory);
                        if (r < 0)
                                return r;

                        /* Allow user services to run, even if the
                         * home directory is missing */
                        ec->working_directory_missing_ok = true;
                }

                if (ec->private_devices)
                        ec->capability_bounding_set &= ~((UINT64_C(1) << CAP_MKNOD) | (UINT64_C(1) << CAP_SYS_RAWIO));

                if (ec->protect_kernel_modules)
                        ec->capability_bounding_set &= ~(UINT64_C(1) << CAP_SYS_MODULE);

                if (ec->dynamic_user) {
                        if (!ec->user) {
                                r = user_from_unit_name(u, &ec->user);
                                if (r < 0)
                                        return r;
                        }

                        if (!ec->group) {
                                ec->group = strdup(ec->user);
                                if (!ec->group)
                                        return -ENOMEM;
                        }

                        /* If the dynamic user option is on, let's make sure that the unit can't leave its UID/GID
                         * around in the file system or on IPC objects. Hence enforce a strict sandbox. */

                        ec->private_tmp = true;
                        ec->remove_ipc = true;
                        ec->protect_system = PROTECT_SYSTEM_STRICT;
                        if (ec->protect_home == PROTECT_HOME_NO)
                                ec->protect_home = PROTECT_HOME_READ_ONLY;
                }
        }

        cc = unit_get_cgroup_context(u);
        if (cc) {

                if (ec &&
                    ec->private_devices &&
                    cc->device_policy == CGROUP_AUTO)
                        cc->device_policy = CGROUP_CLOSED;
        }

        return 0;
}

ExecContext *unit_get_exec_context(Unit *u) {
        size_t offset;
        assert(u);

        if (u->type < 0)
                return NULL;

        offset = UNIT_VTABLE(u)->exec_context_offset;
        if (offset <= 0)
                return NULL;

        return (ExecContext*) ((uint8_t*) u + offset);
}

KillContext *unit_get_kill_context(Unit *u) {
        size_t offset;
        assert(u);

        if (u->type < 0)
                return NULL;

        offset = UNIT_VTABLE(u)->kill_context_offset;
        if (offset <= 0)
                return NULL;

        return (KillContext*) ((uint8_t*) u + offset);
}

CGroupContext *unit_get_cgroup_context(Unit *u) {
        size_t offset;

        if (u->type < 0)
                return NULL;

        offset = UNIT_VTABLE(u)->cgroup_context_offset;
        if (offset <= 0)
                return NULL;

        return (CGroupContext*) ((uint8_t*) u + offset);
}

ExecRuntime *unit_get_exec_runtime(Unit *u) {
        size_t offset;

        if (u->type < 0)
                return NULL;

        offset = UNIT_VTABLE(u)->exec_runtime_offset;
        if (offset <= 0)
                return NULL;

        return *(ExecRuntime**) ((uint8_t*) u + offset);
}

static const char* unit_drop_in_dir(Unit *u, UnitSetPropertiesMode mode) {
        assert(u);

        if (!IN_SET(mode, UNIT_RUNTIME, UNIT_PERSISTENT))
                return NULL;

        if (u->transient) /* Redirect drop-ins for transient units always into the transient directory. */
                return u->manager->lookup_paths.transient;

        if (mode == UNIT_RUNTIME)
                return u->manager->lookup_paths.runtime_control;

        if (mode == UNIT_PERSISTENT)
                return u->manager->lookup_paths.persistent_control;

        return NULL;
}

int unit_write_drop_in(Unit *u, UnitSetPropertiesMode mode, const char *name, const char *data) {
        _cleanup_free_ char *p = NULL, *q = NULL;
        const char *dir, *wrapped;
        int r;

        assert(u);

        if (u->transient_file) {
                /* When this is a transient unit file in creation, then let's not create a new drop-in but instead
                 * write to the transient unit file. */
                fputs(data, u->transient_file);
                fputc('\n', u->transient_file);
                return 0;
        }

        if (!IN_SET(mode, UNIT_PERSISTENT, UNIT_RUNTIME))
                return 0;

        dir = unit_drop_in_dir(u, mode);
        if (!dir)
                return -EINVAL;

        wrapped = strjoina("# This is a drop-in unit file extension, created via \"systemctl set-property\"\n"
                           "# or an equivalent operation. Do not edit.\n",
                           data,
                           "\n");

        r = drop_in_file(dir, u->id, 50, name, &p, &q);
        if (r < 0)
                return r;

        (void) mkdir_p(p, 0755);
        r = write_string_file_atomic_label(q, wrapped);
        if (r < 0)
                return r;

        r = strv_push(&u->dropin_paths, q);
        if (r < 0)
                return r;
        q = NULL;

        strv_uniq(u->dropin_paths);

        u->dropin_mtime = now(CLOCK_REALTIME);

        return 0;
}

int unit_write_drop_in_format(Unit *u, UnitSetPropertiesMode mode, const char *name, const char *format, ...) {
        _cleanup_free_ char *p = NULL;
        va_list ap;
        int r;

        assert(u);
        assert(name);
        assert(format);

        if (!IN_SET(mode, UNIT_PERSISTENT, UNIT_RUNTIME))
                return 0;

        va_start(ap, format);
        r = vasprintf(&p, format, ap);
        va_end(ap);

        if (r < 0)
                return -ENOMEM;

        return unit_write_drop_in(u, mode, name, p);
}

int unit_write_drop_in_private(Unit *u, UnitSetPropertiesMode mode, const char *name, const char *data) {
        const char *ndata;

        assert(u);
        assert(name);
        assert(data);

        if (!UNIT_VTABLE(u)->private_section)
                return -EINVAL;

        if (!IN_SET(mode, UNIT_PERSISTENT, UNIT_RUNTIME))
                return 0;

        ndata = strjoina("[", UNIT_VTABLE(u)->private_section, "]\n", data);

        return unit_write_drop_in(u, mode, name, ndata);
}

int unit_write_drop_in_private_format(Unit *u, UnitSetPropertiesMode mode, const char *name, const char *format, ...) {
        _cleanup_free_ char *p = NULL;
        va_list ap;
        int r;

        assert(u);
        assert(name);
        assert(format);

        if (!IN_SET(mode, UNIT_PERSISTENT, UNIT_RUNTIME))
                return 0;

        va_start(ap, format);
        r = vasprintf(&p, format, ap);
        va_end(ap);

        if (r < 0)
                return -ENOMEM;

        return unit_write_drop_in_private(u, mode, name, p);
}

int unit_make_transient(Unit *u) {
        FILE *f;
        char *path;

        assert(u);

        if (!UNIT_VTABLE(u)->can_transient)
                return -EOPNOTSUPP;

        path = strjoin(u->manager->lookup_paths.transient, "/", u->id);
        if (!path)
                return -ENOMEM;

        /* Let's open the file we'll write the transient settings into. This file is kept open as long as we are
         * creating the transient, and is closed in unit_load(), as soon as we start loading the file. */

        RUN_WITH_UMASK(0022) {
                f = fopen(path, "we");
                if (!f) {
                        free(path);
                        return -errno;
                }
        }

        if (u->transient_file)
                fclose(u->transient_file);
        u->transient_file = f;

        free(u->fragment_path);
        u->fragment_path = path;

        u->source_path = mfree(u->source_path);
        u->dropin_paths = strv_free(u->dropin_paths);
        u->fragment_mtime = u->source_mtime = u->dropin_mtime = 0;

        u->load_state = UNIT_STUB;
        u->load_error = 0;
        u->transient = true;

        unit_add_to_dbus_queue(u);
        unit_add_to_gc_queue(u);

        fputs("# This is a transient unit file, created programmatically via the systemd API. Do not edit.\n",
              u->transient_file);

        return 0;
}

static void log_kill(pid_t pid, int sig, void *userdata) {
        _cleanup_free_ char *comm = NULL;

        (void) get_process_comm(pid, &comm);

        /* Don't log about processes marked with brackets, under the assumption that these are temporary processes
           only, like for example systemd's own PAM stub process. */
        if (comm && comm[0] == '(')
                return;

        log_unit_notice(userdata,
                        "Killing process " PID_FMT " (%s) with signal SIG%s.",
                        pid,
                        strna(comm),
                        signal_to_string(sig));
}

static int operation_to_signal(KillContext *c, KillOperation k) {
        assert(c);

        switch (k) {

        case KILL_TERMINATE:
        case KILL_TERMINATE_AND_LOG:
                return c->kill_signal;

        case KILL_KILL:
                return SIGKILL;

        case KILL_ABORT:
                return SIGABRT;

        default:
                assert_not_reached("KillOperation unknown");
        }
}

int unit_kill_context(
                Unit *u,
                KillContext *c,
                KillOperation k,
                pid_t main_pid,
                pid_t control_pid,
                bool main_pid_alien) {

        bool wait_for_exit = false, send_sighup;
        cg_kill_log_func_t log_func = NULL;
        int sig, r;

        assert(u);
        assert(c);

        /* Kill the processes belonging to this unit, in preparation for shutting the unit down.
         * Returns > 0 if we killed something worth waiting for, 0 otherwise. */

        if (c->kill_mode == KILL_NONE)
                return 0;

        sig = operation_to_signal(c, k);

        send_sighup =
                c->send_sighup &&
                IN_SET(k, KILL_TERMINATE, KILL_TERMINATE_AND_LOG) &&
                sig != SIGHUP;

        if (k != KILL_TERMINATE || IN_SET(sig, SIGKILL, SIGABRT))
                log_func = log_kill;

        if (main_pid > 0) {
                if (log_func)
                        log_func(main_pid, sig, u);

                r = kill_and_sigcont(main_pid, sig);
                if (r < 0 && r != -ESRCH) {
                        _cleanup_free_ char *comm = NULL;
                        (void) get_process_comm(main_pid, &comm);

                        log_unit_warning_errno(u, r, "Failed to kill main process " PID_FMT " (%s), ignoring: %m", main_pid, strna(comm));
                } else {
                        if (!main_pid_alien)
                                wait_for_exit = true;

                        if (r != -ESRCH && send_sighup)
                                (void) kill(main_pid, SIGHUP);
                }
        }

        if (control_pid > 0) {
                if (log_func)
                        log_func(control_pid, sig, u);

                r = kill_and_sigcont(control_pid, sig);
                if (r < 0 && r != -ESRCH) {
                        _cleanup_free_ char *comm = NULL;
                        (void) get_process_comm(control_pid, &comm);

                        log_unit_warning_errno(u, r, "Failed to kill control process " PID_FMT " (%s), ignoring: %m", control_pid, strna(comm));
                } else {
                        wait_for_exit = true;

                        if (r != -ESRCH && send_sighup)
                                (void) kill(control_pid, SIGHUP);
                }
        }

        if (u->cgroup_path &&
            (c->kill_mode == KILL_CONTROL_GROUP || (c->kill_mode == KILL_MIXED && k == KILL_KILL))) {
                _cleanup_set_free_ Set *pid_set = NULL;

                /* Exclude the main/control pids from being killed via the cgroup */
                pid_set = unit_pid_set(main_pid, control_pid);
                if (!pid_set)
                        return -ENOMEM;

                r = cg_kill_recursive(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path,
                                      sig,
                                      CGROUP_SIGCONT|CGROUP_IGNORE_SELF,
                                      pid_set,
                                      log_func, u);
                if (r < 0) {
                        if (r != -EAGAIN && r != -ESRCH && r != -ENOENT)
                                log_unit_warning_errno(u, r, "Failed to kill control group %s, ignoring: %m", u->cgroup_path);

                } else if (r > 0) {

                        wait_for_exit = true;

                        if (send_sighup) {
                                set_free(pid_set);

                                pid_set = unit_pid_set(main_pid, control_pid);
                                if (!pid_set)
                                        return -ENOMEM;

                                cg_kill_recursive(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path,
                                                  SIGHUP,
                                                  CGROUP_IGNORE_SELF,
                                                  pid_set,
                                                  NULL, NULL);
                        }
                }
        }

        return wait_for_exit;
}

int unit_require_mounts_for(Unit *u, const char *path) {
        char prefix[strlen(path) + 1], *p;
        int r;

        assert(u);
        assert(path);

        /* Registers a unit for requiring a certain path and all its
         * prefixes. We keep a simple array of these paths in the
         * unit, since its usually short. However, we build a prefix
         * table for all possible prefixes so that new appearing mount
         * units can easily determine which units to make themselves a
         * dependency of. */

        if (!path_is_absolute(path))
                return -EINVAL;

        p = strdup(path);
        if (!p)
                return -ENOMEM;

        path_kill_slashes(p);

        if (!path_is_safe(p)) {
                free(p);
                return -EPERM;
        }

        if (strv_contains(u->requires_mounts_for, p)) {
                free(p);
                return 0;
        }

        r = strv_consume(&u->requires_mounts_for, p);
        if (r < 0)
                return r;

        PATH_FOREACH_PREFIX_MORE(prefix, p) {
                Set *x;

                x = hashmap_get(u->manager->units_requiring_mounts_for, prefix);
                if (!x) {
                        char *q;

                        r = hashmap_ensure_allocated(&u->manager->units_requiring_mounts_for, &string_hash_ops);
                        if (r < 0)
                                return r;

                        q = strdup(prefix);
                        if (!q)
                                return -ENOMEM;

                        x = set_new(NULL);
                        if (!x) {
                                free(q);
                                return -ENOMEM;
                        }

                        r = hashmap_put(u->manager->units_requiring_mounts_for, q, x);
                        if (r < 0) {
                                free(q);
                                set_free(x);
                                return r;
                        }
                }

                r = set_put(x, u);
                if (r < 0)
                        return r;
        }

        return 0;
}

int unit_setup_exec_runtime(Unit *u) {
        ExecRuntime **rt;
        size_t offset;
        Iterator i;
        Unit *other;

        offset = UNIT_VTABLE(u)->exec_runtime_offset;
        assert(offset > 0);

        /* Check if there already is an ExecRuntime for this unit? */
        rt = (ExecRuntime**) ((uint8_t*) u + offset);
        if (*rt)
                return 0;

        /* Try to get it from somebody else */
        SET_FOREACH(other, u->dependencies[UNIT_JOINS_NAMESPACE_OF], i) {

                *rt = unit_get_exec_runtime(other);
                if (*rt) {
                        exec_runtime_ref(*rt);
                        return 0;
                }
        }

        return exec_runtime_make(rt, unit_get_exec_context(u), u->id);
}

int unit_setup_dynamic_creds(Unit *u) {
        ExecContext *ec;
        DynamicCreds *dcreds;
        size_t offset;

        assert(u);

        offset = UNIT_VTABLE(u)->dynamic_creds_offset;
        assert(offset > 0);
        dcreds = (DynamicCreds*) ((uint8_t*) u + offset);

        ec = unit_get_exec_context(u);
        assert(ec);

        if (!ec->dynamic_user)
                return 0;

        return dynamic_creds_acquire(dcreds, u->manager, ec->user, ec->group);
}

bool unit_type_supported(UnitType t) {
        if (_unlikely_(t < 0))
                return false;
        if (_unlikely_(t >= _UNIT_TYPE_MAX))
                return false;

        if (!unit_vtable[t]->supported)
                return true;

        return unit_vtable[t]->supported();
}

void unit_warn_if_dir_nonempty(Unit *u, const char* where) {
        int r;

        assert(u);
        assert(where);

        r = dir_is_empty(where);
        if (r > 0 || r == -ENOTDIR)
                return;
        if (r < 0) {
                log_unit_warning_errno(u, r, "Failed to check directory %s: %m", where);
                return;
        }

        log_struct(LOG_NOTICE,
                   "MESSAGE_ID=" SD_MESSAGE_OVERMOUNTING_STR,
                   LOG_UNIT_ID(u),
                   LOG_UNIT_MESSAGE(u, "Directory %s to mount over is not empty, mounting anyway.", where),
                   "WHERE=%s", where,
                   NULL);
}

int unit_fail_if_symlink(Unit *u, const char* where) {
        int r;

        assert(u);
        assert(where);

        r = is_symlink(where);
        if (r < 0) {
                log_unit_debug_errno(u, r, "Failed to check symlink %s, ignoring: %m", where);
                return 0;
        }
        if (r == 0)
                return 0;

        log_struct(LOG_ERR,
                   "MESSAGE_ID=" SD_MESSAGE_OVERMOUNTING_STR,
                   LOG_UNIT_ID(u),
                   LOG_UNIT_MESSAGE(u, "Mount on symlink %s not allowed.", where),
                   "WHERE=%s", where,
                   NULL);

        return -ELOOP;
}

bool unit_is_pristine(Unit *u) {
        assert(u);

        /* Check if the unit already exists or is already around,
         * in a number of different ways. Note that to cater for unit
         * types such as slice, we are generally fine with units that
         * are marked UNIT_LOADED even though nothing was
         * actually loaded, as those unit types don't require a file
         * on disk to validly load. */

        return !(!IN_SET(u->load_state, UNIT_NOT_FOUND, UNIT_LOADED) ||
                 u->fragment_path ||
                 u->source_path ||
                 !strv_isempty(u->dropin_paths) ||
                 u->job ||
                 u->merged_into);
}

pid_t unit_control_pid(Unit *u) {
        assert(u);

        if (UNIT_VTABLE(u)->control_pid)
                return UNIT_VTABLE(u)->control_pid(u);

        return 0;
}

pid_t unit_main_pid(Unit *u) {
        assert(u);

        if (UNIT_VTABLE(u)->main_pid)
                return UNIT_VTABLE(u)->main_pid(u);

        return 0;
}

static void unit_unref_uid_internal(
                Unit *u,
                uid_t *ref_uid,
                bool destroy_now,
                void (*_manager_unref_uid)(Manager *m, uid_t uid, bool destroy_now)) {

        assert(u);
        assert(ref_uid);
        assert(_manager_unref_uid);

        /* Generic implementation of both unit_unref_uid() and unit_unref_gid(), under the assumption that uid_t and
         * gid_t are actually the same time, with the same validity rules.
         *
         * Drops a reference to UID/GID from a unit. */

        assert_cc(sizeof(uid_t) == sizeof(gid_t));
        assert_cc(UID_INVALID == (uid_t) GID_INVALID);

        if (!uid_is_valid(*ref_uid))
                return;

        _manager_unref_uid(u->manager, *ref_uid, destroy_now);
        *ref_uid = UID_INVALID;
}

void unit_unref_uid(Unit *u, bool destroy_now) {
        unit_unref_uid_internal(u, &u->ref_uid, destroy_now, manager_unref_uid);
}

void unit_unref_gid(Unit *u, bool destroy_now) {
        unit_unref_uid_internal(u, (uid_t*) &u->ref_gid, destroy_now, manager_unref_gid);
}

static int unit_ref_uid_internal(
                Unit *u,
                uid_t *ref_uid,
                uid_t uid,
                bool clean_ipc,
                int (*_manager_ref_uid)(Manager *m, uid_t uid, bool clean_ipc)) {

        int r;

        assert(u);
        assert(ref_uid);
        assert(uid_is_valid(uid));
        assert(_manager_ref_uid);

        /* Generic implementation of both unit_ref_uid() and unit_ref_guid(), under the assumption that uid_t and gid_t
         * are actually the same type, and have the same validity rules.
         *
         * Adds a reference on a specific UID/GID to this unit. Each unit referencing the same UID/GID maintains a
         * reference so that we can destroy the UID/GID's IPC resources as soon as this is requested and the counter
         * drops to zero. */

        assert_cc(sizeof(uid_t) == sizeof(gid_t));
        assert_cc(UID_INVALID == (uid_t) GID_INVALID);

        if (*ref_uid == uid)
                return 0;

        if (uid_is_valid(*ref_uid)) /* Already set? */
                return -EBUSY;

        r = _manager_ref_uid(u->manager, uid, clean_ipc);
        if (r < 0)
                return r;

        *ref_uid = uid;
        return 1;
}

int unit_ref_uid(Unit *u, uid_t uid, bool clean_ipc) {
        return unit_ref_uid_internal(u, &u->ref_uid, uid, clean_ipc, manager_ref_uid);
}

int unit_ref_gid(Unit *u, gid_t gid, bool clean_ipc) {
        return unit_ref_uid_internal(u, (uid_t*) &u->ref_gid, (uid_t) gid, clean_ipc, manager_ref_gid);
}

static int unit_ref_uid_gid_internal(Unit *u, uid_t uid, gid_t gid, bool clean_ipc) {
        int r = 0, q = 0;

        assert(u);

        /* Reference both a UID and a GID in one go. Either references both, or neither. */

        if (uid_is_valid(uid)) {
                r = unit_ref_uid(u, uid, clean_ipc);
                if (r < 0)
                        return r;
        }

        if (gid_is_valid(gid)) {
                q = unit_ref_gid(u, gid, clean_ipc);
                if (q < 0) {
                        if (r > 0)
                                unit_unref_uid(u, false);

                        return q;
                }
        }

        return r > 0 || q > 0;
}

int unit_ref_uid_gid(Unit *u, uid_t uid, gid_t gid) {
        ExecContext *c;
        int r;

        assert(u);

        c = unit_get_exec_context(u);

        r = unit_ref_uid_gid_internal(u, uid, gid, c ? c->remove_ipc : false);
        if (r < 0)
                return log_unit_warning_errno(u, r, "Couldn't add UID/GID reference to unit, proceeding without: %m");

        return r;
}

void unit_unref_uid_gid(Unit *u, bool destroy_now) {
        assert(u);

        unit_unref_uid(u, destroy_now);
        unit_unref_gid(u, destroy_now);
}

void unit_notify_user_lookup(Unit *u, uid_t uid, gid_t gid) {
        int r;

        assert(u);

        /* This is invoked whenever one of the forked off processes let's us know the UID/GID its user name/group names
         * resolved to. We keep track of which UID/GID is currently assigned in order to be able to destroy its IPC
         * objects when no service references the UID/GID anymore. */

        r = unit_ref_uid_gid(u, uid, gid);
        if (r > 0)
                bus_unit_send_change_signal(u);
}

int unit_set_invocation_id(Unit *u, sd_id128_t id) {
        int r;

        assert(u);

        /* Set the invocation ID for this unit. If we cannot, this will not roll back, but reset the whole thing. */

        if (sd_id128_equal(u->invocation_id, id))
                return 0;

        if (!sd_id128_is_null(u->invocation_id))
                (void) hashmap_remove_value(u->manager->units_by_invocation_id, &u->invocation_id, u);

        if (sd_id128_is_null(id)) {
                r = 0;
                goto reset;
        }

        r = hashmap_ensure_allocated(&u->manager->units_by_invocation_id, &id128_hash_ops);
        if (r < 0)
                goto reset;

        u->invocation_id = id;
        sd_id128_to_string(id, u->invocation_id_string);

        r = hashmap_put(u->manager->units_by_invocation_id, &u->invocation_id, u);
        if (r < 0)
                goto reset;

        return 0;

reset:
        u->invocation_id = SD_ID128_NULL;
        u->invocation_id_string[0] = 0;
        return r;
}

int unit_acquire_invocation_id(Unit *u) {
        sd_id128_t id;
        int r;

        assert(u);

        r = sd_id128_randomize(&id);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to generate invocation ID for unit: %m");

        r = unit_set_invocation_id(u, id);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to set invocation ID for unit: %m");

        return 0;
}
