/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>

#include "conf-parser.h"
#include "util.h"
#include "macro.h"
#include "strv.h"
#include "log.h"
#include "utf8.h"
#include "path-util.h"
#include "set.h"
#include "exit-status.h"
#include "sd-messages.h"

int log_syntax_internal(const char *unit, int level,
                        const char *file, unsigned line, const char *func,
                        const char *config_file, unsigned config_line,
                        int error, const char *format, ...) {

        _cleanup_free_ char *msg = NULL;
        int r;
        va_list ap;

        va_start(ap, format);
        r = vasprintf(&msg, format, ap);
        va_end(ap);
        if (r < 0)
                return log_oom();

        if (unit)
                r = log_struct_internal(level,
                                        file, line, func,
                                        getpid() == 1 ? "UNIT=%s" : "USER_UNIT=%s", unit,
                                        MESSAGE_ID(SD_MESSAGE_CONFIG_ERROR),
                                        "CONFIG_FILE=%s", config_file,
                                        "CONFIG_LINE=%u", config_line,
                                        "ERRNO=%d", error > 0 ? error : EINVAL,
                                        "MESSAGE=[%s:%u] %s", config_file, config_line, msg,
                                        NULL);
        else
                r = log_struct_internal(level,
                                        file, line, func,
                                        MESSAGE_ID(SD_MESSAGE_CONFIG_ERROR),
                                        "CONFIG_FILE=%s", config_file,
                                        "CONFIG_LINE=%u", config_line,
                                        "ERRNO=%d", error > 0 ? error : EINVAL,
                                        "MESSAGE=[%s:%u] %s", config_file, config_line, msg,
                                        NULL);

        return r;
}

int config_item_table_lookup(
                void *table,
                const char *section,
                const char *lvalue,
                ConfigParserCallback *func,
                int *ltype,
                void **data,
                void *userdata) {

        ConfigTableItem *t;

        assert(table);
        assert(lvalue);
        assert(func);
        assert(ltype);
        assert(data);

        for (t = table; t->lvalue; t++) {

                if (!streq(lvalue, t->lvalue))
                        continue;

                if (!streq_ptr(section, t->section))
                        continue;

                *func = t->parse;
                *ltype = t->ltype;
                *data = t->data;
                return 1;
        }

        return 0;
}

int config_item_perf_lookup(
                void *table,
                const char *section,
                const char *lvalue,
                ConfigParserCallback *func,
                int *ltype,
                void **data,
                void *userdata) {

        ConfigPerfItemLookup lookup = (ConfigPerfItemLookup) table;
        const ConfigPerfItem *p;

        assert(table);
        assert(lvalue);
        assert(func);
        assert(ltype);
        assert(data);

        if (!section)
                p = lookup(lvalue, strlen(lvalue));
        else {
                char *key;

                key = strjoin(section, ".", lvalue, NULL);
                if (!key)
                        return -ENOMEM;

                p = lookup(key, strlen(key));
                free(key);
        }

        if (!p)
                return 0;

        *func = p->parse;
        *ltype = p->ltype;
        *data = (uint8_t*) userdata + p->offset;
        return 1;
}

/* Run the user supplied parser for an assignment */
static int next_assignment(const char *unit,
                           const char *filename,
                           unsigned line,
                           ConfigItemLookup lookup,
                           void *table,
                           const char *section,
                           const char *lvalue,
                           const char *rvalue,
                           bool relaxed,
                           void *userdata) {

        ConfigParserCallback func = NULL;
        int ltype = 0;
        void *data = NULL;
        int r;

        assert(filename);
        assert(line > 0);
        assert(lookup);
        assert(lvalue);
        assert(rvalue);

        r = lookup(table, section, lvalue, &func, &ltype, &data, userdata);
        if (r < 0)
                return r;

        if (r > 0) {
                if (func)
                        return func(unit, filename, line, section, lvalue, ltype,
                                    rvalue, data, userdata);

                return 0;
        }

        /* Warn about unknown non-extension fields. */
        if (!relaxed && !startswith(lvalue, "X-"))
                log_syntax(unit, LOG_WARNING, filename, line, EINVAL,
                           "Unknown lvalue '%s' in section '%s'", lvalue, section);

        return 0;
}

/* Parse a variable assignment line */
static int parse_line(const char* unit,
                      const char *filename,
                      unsigned line,
                      const char *sections,
                      ConfigItemLookup lookup,
                      void *table,
                      bool relaxed,
                      bool allow_include,
                      char **section,
                      char *l,
                      void *userdata) {

        char *e;

        assert(filename);
        assert(line > 0);
        assert(lookup);
        assert(l);

        l = strstrip(l);

        if (!*l)
                return 0;

        if (strchr(COMMENTS "\n", *l))
                return 0;

        if (startswith(l, ".include ")) {
                _cleanup_free_ char *fn = NULL;

                if (!allow_include) {
                        log_syntax(unit, LOG_ERR, filename, line, EBADMSG,
                                   ".include not allowed here. Ignoring.");
                        return 0;
                }

                fn = file_in_same_dir(filename, strstrip(l+9));
                if (!fn)
                        return -ENOMEM;

                return config_parse(unit, fn, NULL, sections, lookup, table, relaxed, false, userdata);
        }

        if (*l == '[') {
                size_t k;
                char *n;

                k = strlen(l);
                assert(k > 0);

                if (l[k-1] != ']') {
                        log_syntax(unit, LOG_ERR, filename, line, EBADMSG,
                                   "Invalid section header '%s'", l);
                        return -EBADMSG;
                }

                n = strndup(l+1, k-2);
                if (!n)
                        return -ENOMEM;

                if (sections && !nulstr_contains(sections, n)) {

                        if (!relaxed)
                                log_syntax(unit, LOG_WARNING, filename, line, EINVAL,
                                           "Unknown section '%s'. Ignoring.", n);

                        free(n);
                        *section = NULL;
                } else {
                        free(*section);
                        *section = n;
                }

                return 0;
        }

        if (sections && !*section) {

                if (!relaxed)
                        log_syntax(unit, LOG_WARNING, filename, line, EINVAL,
                                   "Assignment outside of section. Ignoring.");

                return 0;
        }

        e = strchr(l, '=');
        if (!e) {
                log_syntax(unit, LOG_WARNING, filename, line, EINVAL, "Missing '='.");
                return -EBADMSG;
        }

        *e = 0;
        e++;

        return next_assignment(unit,
                               filename,
                               line,
                               lookup,
                               table,
                               *section,
                               strstrip(l),
                               strstrip(e),
                               relaxed,
                               userdata);
}

/* Go through the file and parse each line */
int config_parse(const char *unit,
                 const char *filename,
                 FILE *f,
                 const char *sections,
                 ConfigItemLookup lookup,
                 void *table,
                 bool relaxed,
                 bool allow_include,
                 void *userdata) {

        _cleanup_free_ char *section = NULL, *continuation = NULL;
        _cleanup_fclose_ FILE *ours = NULL;
        unsigned line = 0;
        int r;

        assert(filename);
        assert(lookup);

        if (!f) {
                f = ours = fopen(filename, "re");
                if (!f) {
                        log_full(errno == ENOENT ? LOG_DEBUG : LOG_ERR, "Failed to open configuration file '%s': %m", filename);
                        return errno == ENOENT ? 0 : -errno;
                }
        }

        while (!feof(f)) {
                char l[LINE_MAX], *p, *c = NULL, *e;
                bool escaped = false;

                if (!fgets(l, sizeof(l), f)) {
                        if (feof(f))
                                break;

                        log_error("Failed to read configuration file '%s': %m", filename);
                        return -errno;
                }

                truncate_nl(l);

                if (continuation) {
                        c = strappend(continuation, l);
                        if (!c)
                                return -ENOMEM;

                        free(continuation);
                        continuation = NULL;
                        p = c;
                } else
                        p = l;

                for (e = p; *e; e++) {
                        if (escaped)
                                escaped = false;
                        else if (*e == '\\')
                                escaped = true;
                }

                if (escaped) {
                        *(e-1) = ' ';

                        if (c)
                                continuation = c;
                        else {
                                continuation = strdup(l);
                                if (!continuation)
                                        return -ENOMEM;
                        }

                        continue;
                }

                r = parse_line(unit,
                               filename,
                               ++line,
                               sections,
                               lookup,
                               table,
                               relaxed,
                               allow_include,
                               &section,
                               p,
                               userdata);
                free(c);

                if (r < 0)
                        return r;
        }

        return 0;
}

#define DEFINE_PARSER(type, vartype, conv_func)                         \
        int config_parse_##type(const char *unit,                       \
                                const char *filename,                   \
                                unsigned line,                          \
                                const char *section,                    \
                                const char *lvalue,                     \
                                int ltype,                              \
                                const char *rvalue,                     \
                                void *data,                             \
                                void *userdata) {                       \
                                                                        \
                vartype *i = data;                                      \
                int r;                                                  \
                                                                        \
                assert(filename);                                       \
                assert(lvalue);                                         \
                assert(rvalue);                                         \
                assert(data);                                           \
                                                                        \
                r = conv_func(rvalue, i);                               \
                if (r < 0)                                              \
                        log_syntax(unit, LOG_ERR, filename, line, -r,   \
                                   "Failed to parse %s value, ignoring: %s", \
                                   #vartype, rvalue);                   \
                                                                        \
                return 0;                                               \
        }

DEFINE_PARSER(int, int, safe_atoi)
DEFINE_PARSER(long, long, safe_atoli)
DEFINE_PARSER(uint64, uint64_t, safe_atou64)
DEFINE_PARSER(unsigned, unsigned, safe_atou)
DEFINE_PARSER(double, double, safe_atod)
DEFINE_PARSER(nsec, nsec_t, parse_nsec)
DEFINE_PARSER(sec, usec_t, parse_sec)


int config_parse_bytes_size(const char* unit,
                            const char *filename,
                            unsigned line,
                            const char *section,
                            const char *lvalue,
                            int ltype,
                            const char *rvalue,
                            void *data,
                            void *userdata) {

        size_t *sz = data;
        off_t o;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = parse_bytes(rvalue, &o);
        if (r < 0 || (off_t) (size_t) o != o) {
                log_syntax(unit, LOG_ERR, filename, line, -r,
                           "Failed to parse byte value, ignoring: %s", rvalue);
                return 0;
        }

        *sz = (size_t) o;
        return 0;
}


int config_parse_bytes_off(const char* unit,
                           const char *filename,
                           unsigned line,
                           const char *section,
                           const char *lvalue,
                           int ltype,
                           const char *rvalue,
                           void *data,
                           void *userdata) {

        off_t *bytes = data;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        assert_cc(sizeof(off_t) == sizeof(uint64_t));

        r = parse_bytes(rvalue, bytes);
        if (r < 0)
                log_syntax(unit, LOG_ERR, filename, line, -r,
                           "Failed to parse bytes value, ignoring: %s", rvalue);

        return 0;
}

int config_parse_bool(const char* unit,
                      const char *filename,
                      unsigned line,
                      const char *section,
                      const char *lvalue,
                      int ltype,
                      const char *rvalue,
                      void *data,
                      void *userdata) {

        int k;
        bool *b = data;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        k = parse_boolean(rvalue);
        if (k < 0) {
                log_syntax(unit, LOG_ERR, filename, line, -k,
                           "Failed to parse boolean value, ignoring: %s", rvalue);
                return 0;
        }

        *b = !!k;
        return 0;
}

int config_parse_tristate(const char *unit,
                          const char *filename,
                          unsigned line,
                          const char *section,
                          const char *lvalue,
                          int ltype,
                          const char *rvalue,
                          void *data,
                          void *userdata) {

        int k;
        int *b = data;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        /* Tristates are like booleans, but can also take the 'default' value, i.e. "-1" */

        k = parse_boolean(rvalue);
        if (k < 0) {
                log_syntax(unit, LOG_ERR, filename, line, -k,
                           "Failed to parse boolean value, ignoring: %s", rvalue);
                return 0;
        }

        *b = !!k;
        return 0;
}

int config_parse_string(const char *unit,
                        const char *filename,
                        unsigned line,
                        const char *section,
                        const char *lvalue,
                        int ltype,
                        const char *rvalue,
                        void *data,
                        void *userdata) {

        char **s = data;
        char *n;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        n = strdup(rvalue);
        if (!n)
                return log_oom();

        if (!utf8_is_valid(n)) {
                log_syntax(unit, LOG_ERR, filename, line, EINVAL,
                           "String is not UTF-8 clean, ignoring assignment: %s", rvalue);
                free(n);
                return 0;
        }

        free(*s);
        if (*n)
                *s = n;
        else {
                free(n);
                *s = NULL;
        }

        return 0;
}

int config_parse_path(const char *unit,
                      const char *filename,
                      unsigned line,
                      const char *section,
                      const char *lvalue,
                      int ltype,
                      const char *rvalue,
                      void *data,
                      void *userdata) {

        char **s = data;
        char *n;
        int offset;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (!utf8_is_valid(rvalue)) {
                log_invalid_utf8(unit, LOG_ERR, filename, line, EINVAL, rvalue);
                return 0;
        }

        offset = rvalue[0] == '-' && (streq(lvalue, "InaccessibleDirectories") ||
                                      streq(lvalue, "ReadOnlyDirectories"));
        if (!path_is_absolute(rvalue + offset)) {
                log_syntax(unit, LOG_ERR, filename, line, EINVAL,
                           "Not an absolute path, ignoring: %s", rvalue);
                return 0;
        }

        n = strdup(rvalue);
        if (!n)
                return log_oom();

        path_kill_slashes(n);

        free(*s);
        *s = n;

        return 0;
}

int config_parse_strv(const char *unit,
                      const char *filename,
                      unsigned line,
                      const char *section,
                      const char *lvalue,
                      int ltype,
                      const char *rvalue,
                      void *data,
                      void *userdata) {

        char *** sv = data, *w, *state;
        size_t l;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                char **empty;

                /* Empty assignment resets the list. As a special rule
                 * we actually fill in a real empty array here rather
                 * than NULL, since some code wants to know if
                 * something was set at all... */
                empty = strv_new(NULL, NULL);
                if (!empty)
                        return log_oom();

                strv_free(*sv);
                *sv = empty;
                return 0;
        }

        FOREACH_WORD_QUOTED(w, l, rvalue, state) {
                _cleanup_free_ char *n;

                n = cunescape_length(w, l);
                if (!n)
                        return log_oom();

                if (!utf8_is_valid(n)) {
                        log_invalid_utf8(unit, LOG_ERR, filename, line, EINVAL, rvalue);
                        continue;
                }

                r = strv_extend(sv, n);
                if (r < 0)
                        return log_oom();
        }

        return 0;
}

int config_parse_path_strv(const char *unit,
                           const char *filename,
                           unsigned line,
                           const char *section,
                           const char *lvalue,
                           int ltype,
                           const char *rvalue,
                           void *data,
                           void *userdata) {

        char*** sv = data, *w, *state;
        size_t l;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                /* Empty assignment resets the list */
                strv_free(*sv);
                *sv = NULL;
                return 0;
        }

        FOREACH_WORD_QUOTED(w, l, rvalue, state) {
                _cleanup_free_ char *n;
                int offset;

                n = strndup(w, l);
                if (!n)
                        return log_oom();

                if (!utf8_is_valid(n)) {
                        log_syntax(unit, LOG_ERR, filename, line, EINVAL,
                                   "Path is not UTF-8 clean, ignoring assignment: %s", rvalue);
                        continue;
                }

                offset = n[0] == '-' && (streq(lvalue, "InaccessibleDirectories") ||
                                         streq(lvalue, "ReadOnlyDirectories"));
                if (!path_is_absolute(n + offset)) {
                        log_syntax(unit, LOG_ERR, filename, line, EINVAL,
                                   "Not an absolute path, ignoring: %s", rvalue);
                        continue;
                }

                path_kill_slashes(n);
                r = strv_extend(sv, n);
                if (r < 0)
                        return log_oom();
        }

        return 0;
}

int config_parse_mode(const char *unit,
                      const char *filename,
                      unsigned line,
                      const char *section,
                      const char *lvalue,
                      int ltype,
                      const char *rvalue,
                      void *data,
                      void *userdata) {

        mode_t *m = data;
        long l;
        char *x = NULL;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        errno = 0;
        l = strtol(rvalue, &x, 8);
        if (!x || x == rvalue || *x || errno) {
                log_syntax(unit, LOG_ERR, filename, line, errno,
                           "Failed to parse mode value, ignoring: %s", rvalue);
                return 0;
        }

        if (l < 0000 || l > 07777) {
                log_syntax(unit, LOG_ERR, filename, line, ERANGE,
                           "Mode value out of range, ignoring: %s", rvalue);
                return 0;
        }

        *m = (mode_t) l;
        return 0;
}

int config_parse_facility(const char *unit,
                          const char *filename,
                          unsigned line,
                          const char *section,
                          const char *lvalue,
                          int ltype,
                          const char *rvalue,
                          void *data,
                          void *userdata) {


        int *o = data, x;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        x = log_facility_unshifted_from_string(rvalue);
        if (x < 0) {
                log_syntax(unit, LOG_ERR, filename, line, EINVAL,
                           "Failed to parse log facility, ignoring: %s", rvalue);
                return 0;
        }

        *o = (x << 3) | LOG_PRI(*o);

        return 0;
}

int config_parse_level(const char *unit,
                       const char *filename,
                       unsigned line,
                       const char *section,
                       const char *lvalue,
                       int ltype,
                       const char *rvalue,
                       void *data,
                       void *userdata) {


        int *o = data, x;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        x = log_level_from_string(rvalue);
        if (x < 0) {
                log_syntax(unit, LOG_ERR, filename, line, EINVAL,
                           "Failed to parse log level, ignoring: %s", rvalue);
                return 0;
        }

        *o = (*o & LOG_FACMASK) | x;
        return 0;
}

int config_parse_set_status(const char *unit,
                            const char *filename,
                            unsigned line,
                            const char *section,
                            const char *lvalue,
                            int ltype,
                            const char *rvalue,
                            void *data,
                            void *userdata) {

        char *w;
        size_t l;
        char *state;
        int r;
        ExitStatusSet *status_set = data;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                /* Empty assignment resets the list */

                set_free(status_set->signal);
                set_free(status_set->code);

                status_set->signal = status_set->code = NULL;
                return 0;
        }

        FOREACH_WORD(w, l, rvalue, state) {
                int val;
                char *temp;

                temp = strndup(w, l);
                if (!temp)
                        return log_oom();

                r = safe_atoi(temp, &val);
                if (r < 0) {
                        val = signal_from_string_try_harder(temp);
                        free(temp);

                        if (val > 0) {
                                r = set_ensure_allocated(&status_set->signal, trivial_hash_func, trivial_compare_func);
                                if (r < 0)
                                        return log_oom();

                                r = set_put(status_set->signal, INT_TO_PTR(val));
                                if (r < 0) {
                                        log_syntax(unit, LOG_ERR, filename, line, -r,
                                                   "Unable to store: %s", w);
                                        return r;
                                }
                        } else {
                                log_syntax(unit, LOG_ERR, filename, line, -val,
                                           "Failed to parse value, ignoring: %s", w);
                                return 0;
                        }
                } else {
                        free(temp);

                        if (val < 0 || val > 255)
                                log_syntax(unit, LOG_ERR, filename, line, ERANGE,
                                           "Value %d is outside range 0-255, ignoring", val);
                        else {
                                r = set_ensure_allocated(&status_set->code, trivial_hash_func, trivial_compare_func);
                                if (r < 0)
                                        return log_oom();

                                r = set_put(status_set->code, INT_TO_PTR(val));
                                if (r < 0) {
                                        log_syntax(unit, LOG_ERR, filename, line, -r,
                                                   "Unable to store: %s", w);
                                        return r;
                                }
                        }
                }
        }

        return 0;
}
