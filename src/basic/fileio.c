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
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "alloc-util.h"
#include "ctype.h"
#include "def.h"
#include "env-util.h"
#include "escape.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "hexdecoct.h"
#include "log.h"
#include "macro.h"
#include "missing.h"
#include "parse-util.h"
#include "path-util.h"
#include "random-util.h"
#include "stdio-util.h"
#include "string-util.h"
#include "strv.h"
#include "time-util.h"
#include "umask-util.h"
#include "utf8.h"

#define READ_FULL_BYTES_MAX (4U*1024U*1024U)

int write_string_stream_ts(FILE *f, const char *line, bool enforce_newline, struct timespec *ts) {

        assert(f);
        assert(line);

        fputs(line, f);
        if (enforce_newline && !endswith(line, "\n"))
                fputc('\n', f);

        if (ts) {
                struct timespec twice[2] = {*ts, *ts};

                if (futimens(fileno(f), twice) < 0)
                        return -errno;
        }

        return fflush_and_check(f);
}

static int write_string_file_atomic(const char *fn, const char *line, bool enforce_newline) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *p = NULL;
        int r;

        assert(fn);
        assert(line);

        r = fopen_temporary(fn, &f, &p);
        if (r < 0)
                return r;

        (void) fchmod_umask(fileno(f), 0644);

        r = write_string_stream(f, line, enforce_newline);
        if (r >= 0) {
                if (rename(p, fn) < 0)
                        r = -errno;
        }

        if (r < 0)
                (void) unlink(p);

        return r;
}

int write_string_file_ts(const char *fn, const char *line, WriteStringFileFlags flags, struct timespec *ts) {
        _cleanup_fclose_ FILE *f = NULL;
        int q, r;

        assert(fn);
        assert(line);

        if (flags & WRITE_STRING_FILE_ATOMIC) {
                assert(flags & WRITE_STRING_FILE_CREATE);

                r = write_string_file_atomic(fn, line, !(flags & WRITE_STRING_FILE_AVOID_NEWLINE));
                if (r < 0)
                        goto fail;

                return r;
        } else
                assert(ts == NULL);

        if (flags & WRITE_STRING_FILE_CREATE) {
                f = fopen(fn, "we");
                if (!f) {
                        r = -errno;
                        goto fail;
                }
        } else {
                int fd;

                /* We manually build our own version of fopen(..., "we") that
                 * works without O_CREAT */
                fd = open(fn, O_WRONLY|O_CLOEXEC|O_NOCTTY);
                if (fd < 0) {
                        r = -errno;
                        goto fail;
                }

                f = fdopen(fd, "we");
                if (!f) {
                        r = -errno;
                        safe_close(fd);
                        goto fail;
                }
        }

        r = write_string_stream_ts(f, line, !(flags & WRITE_STRING_FILE_AVOID_NEWLINE), ts);
        if (r < 0)
                goto fail;

        return 0;

fail:
        if (!(flags & WRITE_STRING_FILE_VERIFY_ON_FAILURE))
                return r;

        f = safe_fclose(f);

        /* OK, the operation failed, but let's see if the right
         * contents in place already. If so, eat up the error. */

        q = verify_file(fn, line, !(flags & WRITE_STRING_FILE_AVOID_NEWLINE));
        if (q <= 0)
                return r;

        return 0;
}

int read_one_line_file(const char *fn, char **line) {
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(fn);
        assert(line);

        f = fopen(fn, "re");
        if (!f)
                return -errno;

        r = read_line(f, LONG_LINE_MAX, line);
        return r < 0 ? r : 0;
}

int verify_file(const char *fn, const char *blob, bool accept_extra_nl) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *buf = NULL;
        size_t l, k;

        assert(fn);
        assert(blob);

        l = strlen(blob);

        if (accept_extra_nl && endswith(blob, "\n"))
                accept_extra_nl = false;

        buf = malloc(l + accept_extra_nl + 1);
        if (!buf)
                return -ENOMEM;

        f = fopen(fn, "re");
        if (!f)
                return -errno;

        /* We try to read one byte more than we need, so that we know whether we hit eof */
        errno = 0;
        k = fread(buf, 1, l + accept_extra_nl + 1, f);
        if (ferror(f))
                return errno > 0 ? -errno : -EIO;

        if (k != l && k != l + accept_extra_nl)
                return 0;
        if (memcmp(buf, blob, l) != 0)
                return 0;
        if (k > l && buf[l] != '\n')
                return 0;

        return 1;
}

int read_full_stream(FILE *f, char **contents, size_t *size) {
        size_t n, l;
        _cleanup_free_ char *buf = NULL;
        struct stat st;

        assert(f);
        assert(contents);

        if (fstat(fileno(f), &st) < 0)
                return -errno;

        n = LINE_MAX;

        if (S_ISREG(st.st_mode)) {

                /* Safety check */
                if (st.st_size > READ_FULL_BYTES_MAX)
                        return -E2BIG;

                /* Start with the right file size, but be prepared for
                 * files from /proc which generally report a file size
                 * of 0 */
                if (st.st_size > 0)
                        n = st.st_size;
        }

        l = 0;
        for (;;) {
                char *t;
                size_t k;

                t = realloc(buf, n + 1);
                if (!t)
                        return -ENOMEM;

                buf = t;
                k = fread(buf + l, 1, n - l, f);
                if (k > 0)
                        l += k;

                if (ferror(f))
                        return -errno;

                if (feof(f))
                        break;

                /* We aren't expecting fread() to return a short read outside
                 * of (error && eof), assert buffer is full and enlarge buffer.
                 */
                assert(l == n);

                /* Safety check */
                if (n >= READ_FULL_BYTES_MAX)
                        return -E2BIG;

                n = MIN(n * 2, READ_FULL_BYTES_MAX);
        }

        buf[l] = 0;
        *contents = buf;
        buf = NULL; /* do not free */

        if (size)
                *size = l;

        return 0;
}

int read_full_file(const char *fn, char **contents, size_t *size) {
        _cleanup_fclose_ FILE *f = NULL;

        assert(fn);
        assert(contents);

        f = fopen(fn, "re");
        if (!f)
                return -errno;

        return read_full_stream(f, contents, size);
}

static int parse_env_file_internal(
                FILE *f,
                const char *fname,
                const char *newline,
                int (*push) (const char *filename, unsigned line,
                             const char *key, char *value, void *userdata, int *n_pushed),
                void *userdata,
                int *n_pushed) {

        _cleanup_free_ char *contents = NULL, *key = NULL;
        size_t key_alloc = 0, n_key = 0, value_alloc = 0, n_value = 0, last_value_whitespace = (size_t) -1, last_key_whitespace = (size_t) -1;
        char *p, *value = NULL;
        int r;
        unsigned line = 1;

        enum {
                PRE_KEY,
                KEY,
                PRE_VALUE,
                VALUE,
                VALUE_ESCAPE,
                SINGLE_QUOTE_VALUE,
                SINGLE_QUOTE_VALUE_ESCAPE,
                DOUBLE_QUOTE_VALUE,
                DOUBLE_QUOTE_VALUE_ESCAPE,
                COMMENT,
                COMMENT_ESCAPE
        } state = PRE_KEY;

        assert(newline);

        if (f)
                r = read_full_stream(f, &contents, NULL);
        else
                r = read_full_file(fname, &contents, NULL);
        if (r < 0)
                return r;

        for (p = contents; *p; p++) {
                char c = *p;

                switch (state) {

                case PRE_KEY:
                        if (strchr(COMMENTS, c))
                                state = COMMENT;
                        else if (!strchr(WHITESPACE, c)) {
                                state = KEY;
                                last_key_whitespace = (size_t) -1;

                                if (!GREEDY_REALLOC(key, key_alloc, n_key+2)) {
                                        r = -ENOMEM;
                                        goto fail;
                                }

                                key[n_key++] = c;
                        }
                        break;

                case KEY:
                        if (strchr(newline, c)) {
                                state = PRE_KEY;
                                line++;
                                n_key = 0;
                        } else if (c == '=') {
                                state = PRE_VALUE;
                                last_value_whitespace = (size_t) -1;
                        } else {
                                if (!strchr(WHITESPACE, c))
                                        last_key_whitespace = (size_t) -1;
                                else if (last_key_whitespace == (size_t) -1)
                                         last_key_whitespace = n_key;

                                if (!GREEDY_REALLOC(key, key_alloc, n_key+2)) {
                                        r = -ENOMEM;
                                        goto fail;
                                }

                                key[n_key++] = c;
                        }

                        break;

                case PRE_VALUE:
                        if (strchr(newline, c)) {
                                state = PRE_KEY;
                                line++;
                                key[n_key] = 0;

                                if (value)
                                        value[n_value] = 0;

                                /* strip trailing whitespace from key */
                                if (last_key_whitespace != (size_t) -1)
                                        key[last_key_whitespace] = 0;

                                r = push(fname, line, key, value, userdata, n_pushed);
                                if (r < 0)
                                        goto fail;

                                n_key = 0;
                                value = NULL;
                                value_alloc = n_value = 0;

                        } else if (c == '\'')
                                state = SINGLE_QUOTE_VALUE;
                        else if (c == '\"')
                                state = DOUBLE_QUOTE_VALUE;
                        else if (c == '\\')
                                state = VALUE_ESCAPE;
                        else if (!strchr(WHITESPACE, c)) {
                                state = VALUE;

                                if (!GREEDY_REALLOC(value, value_alloc, n_value+2)) {
                                        r = -ENOMEM;
                                        goto fail;
                                }

                                value[n_value++] = c;
                        }

                        break;

                case VALUE:
                        if (strchr(newline, c)) {
                                state = PRE_KEY;
                                line++;

                                key[n_key] = 0;

                                if (value)
                                        value[n_value] = 0;

                                /* Chomp off trailing whitespace from value */
                                if (last_value_whitespace != (size_t) -1)
                                        value[last_value_whitespace] = 0;

                                /* strip trailing whitespace from key */
                                if (last_key_whitespace != (size_t) -1)
                                        key[last_key_whitespace] = 0;

                                r = push(fname, line, key, value, userdata, n_pushed);
                                if (r < 0)
                                        goto fail;

                                n_key = 0;
                                value = NULL;
                                value_alloc = n_value = 0;

                        } else if (c == '\\') {
                                state = VALUE_ESCAPE;
                                last_value_whitespace = (size_t) -1;
                        } else {
                                if (!strchr(WHITESPACE, c))
                                        last_value_whitespace = (size_t) -1;
                                else if (last_value_whitespace == (size_t) -1)
                                        last_value_whitespace = n_value;

                                if (!GREEDY_REALLOC(value, value_alloc, n_value+2)) {
                                        r = -ENOMEM;
                                        goto fail;
                                }

                                value[n_value++] = c;
                        }

                        break;

                case VALUE_ESCAPE:
                        state = VALUE;

                        if (!strchr(newline, c)) {
                                /* Escaped newlines we eat up entirely */
                                if (!GREEDY_REALLOC(value, value_alloc, n_value+2)) {
                                        r = -ENOMEM;
                                        goto fail;
                                }

                                value[n_value++] = c;
                        }
                        break;

                case SINGLE_QUOTE_VALUE:
                        if (c == '\'')
                                state = PRE_VALUE;
                        else if (c == '\\')
                                state = SINGLE_QUOTE_VALUE_ESCAPE;
                        else {
                                if (!GREEDY_REALLOC(value, value_alloc, n_value+2)) {
                                        r = -ENOMEM;
                                        goto fail;
                                }

                                value[n_value++] = c;
                        }

                        break;

                case SINGLE_QUOTE_VALUE_ESCAPE:
                        state = SINGLE_QUOTE_VALUE;

                        if (!strchr(newline, c)) {
                                if (!GREEDY_REALLOC(value, value_alloc, n_value+2)) {
                                        r = -ENOMEM;
                                        goto fail;
                                }

                                value[n_value++] = c;
                        }
                        break;

                case DOUBLE_QUOTE_VALUE:
                        if (c == '\"')
                                state = PRE_VALUE;
                        else if (c == '\\')
                                state = DOUBLE_QUOTE_VALUE_ESCAPE;
                        else {
                                if (!GREEDY_REALLOC(value, value_alloc, n_value+2)) {
                                        r = -ENOMEM;
                                        goto fail;
                                }

                                value[n_value++] = c;
                        }

                        break;

                case DOUBLE_QUOTE_VALUE_ESCAPE:
                        state = DOUBLE_QUOTE_VALUE;

                        if (!strchr(newline, c)) {
                                if (!GREEDY_REALLOC(value, value_alloc, n_value+2)) {
                                        r = -ENOMEM;
                                        goto fail;
                                }

                                value[n_value++] = c;
                        }
                        break;

                case COMMENT:
                        if (c == '\\')
                                state = COMMENT_ESCAPE;
                        else if (strchr(newline, c)) {
                                state = PRE_KEY;
                                line++;
                        }
                        break;

                case COMMENT_ESCAPE:
                        state = COMMENT;
                        break;
                }
        }

        if (IN_SET(state,
                   PRE_VALUE,
                   VALUE,
                   VALUE_ESCAPE,
                   SINGLE_QUOTE_VALUE,
                   SINGLE_QUOTE_VALUE_ESCAPE,
                   DOUBLE_QUOTE_VALUE,
                   DOUBLE_QUOTE_VALUE_ESCAPE)) {

                key[n_key] = 0;

                if (value)
                        value[n_value] = 0;

                if (state == VALUE)
                        if (last_value_whitespace != (size_t) -1)
                                value[last_value_whitespace] = 0;

                /* strip trailing whitespace from key */
                if (last_key_whitespace != (size_t) -1)
                        key[last_key_whitespace] = 0;

                r = push(fname, line, key, value, userdata, n_pushed);
                if (r < 0)
                        goto fail;
        }

        return 0;

fail:
        free(value);
        return r;
}

static int check_utf8ness_and_warn(
                const char *filename, unsigned line,
                const char *key, char *value) {

        if (!utf8_is_valid(key)) {
                _cleanup_free_ char *p = NULL;

                p = utf8_escape_invalid(key);
                log_error("%s:%u: invalid UTF-8 in key '%s', ignoring.", strna(filename), line, p);
                return -EINVAL;
        }

        if (value && !utf8_is_valid(value)) {
                _cleanup_free_ char *p = NULL;

                p = utf8_escape_invalid(value);
                log_error("%s:%u: invalid UTF-8 value for key %s: '%s', ignoring.", strna(filename), line, key, p);
                return -EINVAL;
        }

        return 0;
}

static int parse_env_file_push(
                const char *filename, unsigned line,
                const char *key, char *value,
                void *userdata,
                int *n_pushed) {

        const char *k;
        va_list aq, *ap = userdata;
        int r;

        r = check_utf8ness_and_warn(filename, line, key, value);
        if (r < 0)
                return r;

        va_copy(aq, *ap);

        while ((k = va_arg(aq, const char *))) {
                char **v;

                v = va_arg(aq, char **);

                if (streq(key, k)) {
                        va_end(aq);
                        free(*v);
                        *v = value;

                        if (n_pushed)
                                (*n_pushed)++;

                        return 1;
                }
        }

        va_end(aq);
        free(value);

        return 0;
}

int parse_env_file(
                const char *fname,
                const char *newline, ...) {

        va_list ap;
        int r, n_pushed = 0;

        if (!newline)
                newline = NEWLINE;

        va_start(ap, newline);
        r = parse_env_file_internal(NULL, fname, newline, parse_env_file_push, &ap, &n_pushed);
        va_end(ap);

        return r < 0 ? r : n_pushed;
}

static int load_env_file_push(
                const char *filename, unsigned line,
                const char *key, char *value,
                void *userdata,
                int *n_pushed) {
        char ***m = userdata;
        char *p;
        int r;

        r = check_utf8ness_and_warn(filename, line, key, value);
        if (r < 0)
                return r;

        p = strjoin(key, "=", value);
        if (!p)
                return -ENOMEM;

        r = strv_env_replace(m, p);
        if (r < 0) {
                free(p);
                return r;
        }

        if (n_pushed)
                (*n_pushed)++;

        free(value);
        return 0;
}

int load_env_file(FILE *f, const char *fname, const char *newline, char ***rl) {
        char **m = NULL;
        int r;

        if (!newline)
                newline = NEWLINE;

        r = parse_env_file_internal(f, fname, newline, load_env_file_push, &m, NULL);
        if (r < 0) {
                strv_free(m);
                return r;
        }

        *rl = m;
        return 0;
}

static int load_env_file_push_pairs(
                const char *filename, unsigned line,
                const char *key, char *value,
                void *userdata,
                int *n_pushed) {
        char ***m = userdata;
        int r;

        r = check_utf8ness_and_warn(filename, line, key, value);
        if (r < 0)
                return r;

        r = strv_extend(m, key);
        if (r < 0)
                return -ENOMEM;

        if (!value) {
                r = strv_extend(m, "");
                if (r < 0)
                        return -ENOMEM;
        } else {
                r = strv_push(m, value);
                if (r < 0)
                        return r;
        }

        if (n_pushed)
                (*n_pushed)++;

        return 0;
}

int load_env_file_pairs(FILE *f, const char *fname, const char *newline, char ***rl) {
        char **m = NULL;
        int r;

        if (!newline)
                newline = NEWLINE;

        r = parse_env_file_internal(f, fname, newline, load_env_file_push_pairs, &m, NULL);
        if (r < 0) {
                strv_free(m);
                return r;
        }

        *rl = m;
        return 0;
}

static int merge_env_file_push(
                const char *filename, unsigned line,
                const char *key, char *value,
                void *userdata,
                int *n_pushed) {

        char ***env = userdata;
        char *expanded_value;

        assert(env);

        if (!value) {
                log_error("%s:%u: invalid syntax (around \"%s\"), ignoring.", strna(filename), line, key);
                return 0;
        }

        if (!env_name_is_valid(key)) {
                log_error("%s:%u: invalid variable name \"%s\", ignoring.", strna(filename), line, key);
                free(value);
                return 0;
        }

        expanded_value = replace_env(value, *env,
                                     REPLACE_ENV_USE_ENVIRONMENT|
                                     REPLACE_ENV_ALLOW_BRACELESS|
                                     REPLACE_ENV_ALLOW_EXTENDED);
        if (!expanded_value)
                return -ENOMEM;

        free_and_replace(value, expanded_value);

        return load_env_file_push(filename, line, key, value, env, n_pushed);
}

int merge_env_file(
                char ***env,
                FILE *f,
                const char *fname) {

        /* NOTE: this function supports braceful and braceless variable expansions,
         * plus "extended" substitutions, unlike other exported parsing functions.
         */

        return parse_env_file_internal(f, fname, NEWLINE, merge_env_file_push, env, NULL);
}

static void write_env_var(FILE *f, const char *v) {
        const char *p;

        p = strchr(v, '=');
        if (!p) {
                /* Fallback */
                fputs(v, f);
                fputc('\n', f);
                return;
        }

        p++;
        fwrite(v, 1, p-v, f);

        if (string_has_cc(p, NULL) || chars_intersect(p, WHITESPACE SHELL_NEED_QUOTES)) {
                fputc('\"', f);

                for (; *p; p++) {
                        if (strchr(SHELL_NEED_ESCAPE, *p))
                                fputc('\\', f);

                        fputc(*p, f);
                }

                fputc('\"', f);
        } else
                fputs(p, f);

        fputc('\n', f);
}

int write_env_file(const char *fname, char **l) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *p = NULL;
        char **i;
        int r;

        assert(fname);

        r = fopen_temporary(fname, &f, &p);
        if (r < 0)
                return r;

        fchmod_umask(fileno(f), 0644);

        STRV_FOREACH(i, l)
                write_env_var(f, *i);

        r = fflush_and_check(f);
        if (r >= 0) {
                if (rename(p, fname) >= 0)
                        return 0;

                r = -errno;
        }

        unlink(p);
        return r;
}

int executable_is_script(const char *path, char **interpreter) {
        int r;
        _cleanup_free_ char *line = NULL;
        int len;
        char *ans;

        assert(path);

        r = read_one_line_file(path, &line);
        if (r < 0)
                return r;

        if (!startswith(line, "#!"))
                return 0;

        ans = strstrip(line + 2);
        len = strcspn(ans, " \t");

        if (len == 0)
                return 0;

        ans = strndup(ans, len);
        if (!ans)
                return -ENOMEM;

        *interpreter = ans;
        return 1;
}

/**
 * Retrieve one field from a file like /proc/self/status.  pattern
 * should not include whitespace or the delimiter (':'). pattern matches only
 * the beginning of a line. Whitespace before ':' is skipped. Whitespace and
 * zeros after the ':' will be skipped. field must be freed afterwards.
 * terminator specifies the terminating characters of the field value (not
 * included in the value).
 */
int get_proc_field(const char *filename, const char *pattern, const char *terminator, char **field) {
        _cleanup_free_ char *status = NULL;
        char *t, *f;
        size_t len;
        int r;

        assert(terminator);
        assert(filename);
        assert(pattern);
        assert(field);

        r = read_full_file(filename, &status, NULL);
        if (r < 0)
                return r;

        t = status;

        do {
                bool pattern_ok;

                do {
                        t = strstr(t, pattern);
                        if (!t)
                                return -ENOENT;

                        /* Check that pattern occurs in beginning of line. */
                        pattern_ok = (t == status || t[-1] == '\n');

                        t += strlen(pattern);

                } while (!pattern_ok);

                t += strspn(t, " \t");
                if (!*t)
                        return -ENOENT;

        } while (*t != ':');

        t++;

        if (*t) {
                t += strspn(t, " \t");

                /* Also skip zeros, because when this is used for
                 * capabilities, we don't want the zeros. This way the
                 * same capability set always maps to the same string,
                 * irrespective of the total capability set size. For
                 * other numbers it shouldn't matter. */
                t += strspn(t, "0");
                /* Back off one char if there's nothing but whitespace
                   and zeros */
                if (!*t || isspace(*t))
                        t--;
        }

        len = strcspn(t, terminator);

        f = strndup(t, len);
        if (!f)
                return -ENOMEM;

        *field = f;
        return 0;
}

DIR *xopendirat(int fd, const char *name, int flags) {
        int nfd;
        DIR *d;

        assert(!(flags & O_CREAT));

        nfd = openat(fd, name, O_RDONLY|O_NONBLOCK|O_DIRECTORY|O_CLOEXEC|flags, 0);
        if (nfd < 0)
                return NULL;

        d = fdopendir(nfd);
        if (!d) {
                safe_close(nfd);
                return NULL;
        }

        return d;
}

static int search_and_fopen_internal(const char *path, const char *mode, const char *root, char **search, FILE **_f) {
        char **i;

        assert(path);
        assert(mode);
        assert(_f);

        if (!path_strv_resolve_uniq(search, root))
                return -ENOMEM;

        STRV_FOREACH(i, search) {
                _cleanup_free_ char *p = NULL;
                FILE *f;

                if (root)
                        p = strjoin(root, *i, "/", path);
                else
                        p = strjoin(*i, "/", path);
                if (!p)
                        return -ENOMEM;

                f = fopen(p, mode);
                if (f) {
                        *_f = f;
                        return 0;
                }

                if (errno != ENOENT)
                        return -errno;
        }

        return -ENOENT;
}

int search_and_fopen(const char *path, const char *mode, const char *root, const char **search, FILE **_f) {
        _cleanup_strv_free_ char **copy = NULL;

        assert(path);
        assert(mode);
        assert(_f);

        if (path_is_absolute(path)) {
                FILE *f;

                f = fopen(path, mode);
                if (f) {
                        *_f = f;
                        return 0;
                }

                return -errno;
        }

        copy = strv_copy((char**) search);
        if (!copy)
                return -ENOMEM;

        return search_and_fopen_internal(path, mode, root, copy, _f);
}

int search_and_fopen_nulstr(const char *path, const char *mode, const char *root, const char *search, FILE **_f) {
        _cleanup_strv_free_ char **s = NULL;

        if (path_is_absolute(path)) {
                FILE *f;

                f = fopen(path, mode);
                if (f) {
                        *_f = f;
                        return 0;
                }

                return -errno;
        }

        s = strv_split_nulstr(search);
        if (!s)
                return -ENOMEM;

        return search_and_fopen_internal(path, mode, root, s, _f);
}

int fopen_temporary(const char *path, FILE **_f, char **_temp_path) {
        FILE *f;
        char *t;
        int r, fd;

        assert(path);
        assert(_f);
        assert(_temp_path);

        r = tempfn_xxxxxx(path, NULL, &t);
        if (r < 0)
                return r;

        fd = mkostemp_safe(t);
        if (fd < 0) {
                free(t);
                return -errno;
        }

        f = fdopen(fd, "we");
        if (!f) {
                unlink_noerrno(t);
                free(t);
                safe_close(fd);
                return -errno;
        }

        *_f = f;
        *_temp_path = t;

        return 0;
}

int fflush_and_check(FILE *f) {
        assert(f);

        errno = 0;
        fflush(f);

        if (ferror(f))
                return errno > 0 ? -errno : -EIO;

        return 0;
}

/* This is much like mkostemp() but is subject to umask(). */
int mkostemp_safe(char *pattern) {
        _cleanup_umask_ mode_t u = 0;
        int fd;

        assert(pattern);

        u = umask(077);

        fd = mkostemp(pattern, O_CLOEXEC);
        if (fd < 0)
                return -errno;

        return fd;
}

int tempfn_xxxxxx(const char *p, const char *extra, char **ret) {
        const char *fn;
        char *t;

        assert(p);
        assert(ret);

        /*
         * Turns this:
         *         /foo/bar/waldo
         *
         * Into this:
         *         /foo/bar/.#<extra>waldoXXXXXX
         */

        fn = basename(p);
        if (!filename_is_valid(fn))
                return -EINVAL;

        if (extra == NULL)
                extra = "";

        t = new(char, strlen(p) + 2 + strlen(extra) + 6 + 1);
        if (!t)
                return -ENOMEM;

        strcpy(stpcpy(stpcpy(stpcpy(mempcpy(t, p, fn - p), ".#"), extra), fn), "XXXXXX");

        *ret = path_kill_slashes(t);
        return 0;
}

int tempfn_random(const char *p, const char *extra, char **ret) {
        const char *fn;
        char *t, *x;
        uint64_t u;
        unsigned i;

        assert(p);
        assert(ret);

        /*
         * Turns this:
         *         /foo/bar/waldo
         *
         * Into this:
         *         /foo/bar/.#<extra>waldobaa2a261115984a9
         */

        fn = basename(p);
        if (!filename_is_valid(fn))
                return -EINVAL;

        if (!extra)
                extra = "";

        t = new(char, strlen(p) + 2 + strlen(extra) + 16 + 1);
        if (!t)
                return -ENOMEM;

        x = stpcpy(stpcpy(stpcpy(mempcpy(t, p, fn - p), ".#"), extra), fn);

        u = random_u64();
        for (i = 0; i < 16; i++) {
                *(x++) = hexchar(u & 0xF);
                u >>= 4;
        }

        *x = 0;

        *ret = path_kill_slashes(t);
        return 0;
}

int tempfn_random_child(const char *p, const char *extra, char **ret) {
        char *t, *x;
        uint64_t u;
        unsigned i;
        int r;

        assert(ret);

        /* Turns this:
         *         /foo/bar/waldo
         * Into this:
         *         /foo/bar/waldo/.#<extra>3c2b6219aa75d7d0
         */

        if (!p) {
                r = tmp_dir(&p);
                if (r < 0)
                        return r;
        }

        if (!extra)
                extra = "";

        t = new(char, strlen(p) + 3 + strlen(extra) + 16 + 1);
        if (!t)
                return -ENOMEM;

        x = stpcpy(stpcpy(stpcpy(t, p), "/.#"), extra);

        u = random_u64();
        for (i = 0; i < 16; i++) {
                *(x++) = hexchar(u & 0xF);
                u >>= 4;
        }

        *x = 0;

        *ret = path_kill_slashes(t);
        return 0;
}

int write_timestamp_file_atomic(const char *fn, usec_t n) {
        char ln[DECIMAL_STR_MAX(n)+2];

        /* Creates a "timestamp" file, that contains nothing but a
         * usec_t timestamp, formatted in ASCII. */

        if (n <= 0 || n >= USEC_INFINITY)
                return -ERANGE;

        xsprintf(ln, USEC_FMT "\n", n);

        return write_string_file(fn, ln, WRITE_STRING_FILE_CREATE|WRITE_STRING_FILE_ATOMIC);
}

int read_timestamp_file(const char *fn, usec_t *ret) {
        _cleanup_free_ char *ln = NULL;
        uint64_t t;
        int r;

        r = read_one_line_file(fn, &ln);
        if (r < 0)
                return r;

        r = safe_atou64(ln, &t);
        if (r < 0)
                return r;

        if (t <= 0 || t >= (uint64_t) USEC_INFINITY)
                return -ERANGE;

        *ret = (usec_t) t;
        return 0;
}

int fputs_with_space(FILE *f, const char *s, const char *separator, bool *space) {
        int r;

        assert(s);

        /* Outputs the specified string with fputs(), but optionally prefixes it with a separator. The *space parameter
         * when specified shall initially point to a boolean variable initialized to false. It is set to true after the
         * first invocation. This call is supposed to be use in loops, where a separator shall be inserted between each
         * element, but not before the first one. */

        if (!f)
                f = stdout;

        if (space) {
                if (!separator)
                        separator = " ";

                if (*space) {
                        r = fputs(separator, f);
                        if (r < 0)
                                return r;
                }

                *space = true;
        }

        return fputs(s, f);
}

int open_tmpfile_unlinkable(const char *directory, int flags) {
        char *p;
        int fd, r;

        if (!directory) {
                r = tmp_dir(&directory);
                if (r < 0)
                        return r;
        }

        /* Returns an unlinked temporary file that cannot be linked into the file system anymore */

        /* Try O_TMPFILE first, if it is supported */
        fd = open(directory, flags|O_TMPFILE|O_EXCL, S_IRUSR|S_IWUSR);
        if (fd >= 0)
                return fd;

        /* Fall back to unguessable name + unlinking */
        p = strjoina(directory, "/systemd-tmp-XXXXXX");

        fd = mkostemp_safe(p);
        if (fd < 0)
                return fd;

        (void) unlink(p);

        return fd;
}

int open_tmpfile_linkable(const char *target, int flags, char **ret_path) {
        _cleanup_free_ char *tmp = NULL;
        int r, fd;

        assert(target);
        assert(ret_path);

        /* Don't allow O_EXCL, as that has a special meaning for O_TMPFILE */
        assert((flags & O_EXCL) == 0);

        /* Creates a temporary file, that shall be renamed to "target" later. If possible, this uses O_TMPFILE – in
         * which case "ret_path" will be returned as NULL. If not possible a the tempoary path name used is returned in
         * "ret_path". Use link_tmpfile() below to rename the result after writing the file in full. */

        {
                _cleanup_free_ char *dn = NULL;

                dn = dirname_malloc(target);
                if (!dn)
                        return -ENOMEM;

                fd = open(dn, O_TMPFILE|flags, 0640);
                if (fd >= 0) {
                        *ret_path = NULL;
                        return fd;
                }

                log_debug_errno(errno, "Failed to use O_TMPFILE on %s: %m", dn);
        }

        r = tempfn_random(target, NULL, &tmp);
        if (r < 0)
                return r;

        fd = open(tmp, O_CREAT|O_EXCL|O_NOFOLLOW|O_NOCTTY|flags, 0640);
        if (fd < 0)
                return -errno;

        *ret_path = tmp;
        tmp = NULL;

        return fd;
}

int open_serialization_fd(const char *ident) {
        int fd = -1;

        fd = memfd_create(ident, MFD_CLOEXEC);
        if (fd < 0) {
                const char *path;

                path = getpid() == 1 ? "/run/systemd" : "/tmp";
                fd = open_tmpfile_unlinkable(path, O_RDWR|O_CLOEXEC);
                if (fd < 0)
                        return fd;

                log_debug("Serializing %s to %s.", ident, path);
        } else
                log_debug("Serializing %s to memfd.", ident);

        return fd;
}

int link_tmpfile(int fd, const char *path, const char *target) {

        assert(fd >= 0);
        assert(target);

        /* Moves a temporary file created with open_tmpfile() above into its final place. if "path" is NULL an fd
         * created with O_TMPFILE is assumed, and linkat() is used. Otherwise it is assumed O_TMPFILE is not supported
         * on the directory, and renameat2() is used instead.
         *
         * Note that in both cases we will not replace existing files. This is because linkat() does not support this
         * operation currently (renameat2() does), and there is no nice way to emulate this. */

        if (path) {
                if (rename_noreplace(AT_FDCWD, path, AT_FDCWD, target) < 0)
                        return -errno;
        } else {
                char proc_fd_path[strlen("/proc/self/fd/") + DECIMAL_STR_MAX(fd) + 1];

                xsprintf(proc_fd_path, "/proc/self/fd/%i", fd);

                if (linkat(AT_FDCWD, proc_fd_path, AT_FDCWD, target, AT_SYMLINK_FOLLOW) < 0)
                        return -errno;
        }

        return 0;
}

int read_nul_string(FILE *f, char **ret) {
        _cleanup_free_ char *x = NULL;
        size_t allocated = 0, n = 0;

        assert(f);
        assert(ret);

        /* Reads a NUL-terminated string from the specified file. */

        for (;;) {
                int c;

                if (!GREEDY_REALLOC(x, allocated, n+2))
                        return -ENOMEM;

                c = fgetc(f);
                if (c == 0) /* Terminate at NUL byte */
                        break;
                if (c == EOF) {
                        if (ferror(f))
                                return -errno;
                        break; /* Terminate at EOF */
                }

                x[n++] = (char) c;
        }

        if (x)
                x[n] = 0;
        else {
                x = new0(char, 1);
                if (!x)
                        return -ENOMEM;
        }

        *ret = x;
        x = NULL;

        return 0;
}

int mkdtemp_malloc(const char *template, char **ret) {
        char *p;

        assert(template);
        assert(ret);

        p = strdup(template);
        if (!p)
                return -ENOMEM;

        if (!mkdtemp(p)) {
                free(p);
                return -errno;
        }

        *ret = p;
        return 0;
}

static inline void funlockfilep(FILE **f) {
        funlockfile(*f);
}

int read_line(FILE *f, size_t limit, char **ret) {
        _cleanup_free_ char *buffer = NULL;
        size_t n = 0, allocated = 0, count = 0;

        assert(f);

        /* Something like a bounded version of getline().
         *
         * Considers EOF, \n and \0 end of line delimiters, and does not include these delimiters in the string
         * returned.
         *
         * Returns the number of bytes read from the files (i.e. including delimiters — this hence usually differs from
         * the number of characters in the returned string). When EOF is hit, 0 is returned.
         *
         * The input parameter limit is the maximum numbers of characters in the returned string, i.e. excluding
         * delimiters. If the limit is hit we fail and return -ENOBUFS.
         *
         * If a line shall be skipped ret may be initialized as NULL. */

        if (ret) {
                if (!GREEDY_REALLOC(buffer, allocated, 1))
                        return -ENOMEM;
        }

        {
                _cleanup_(funlockfilep) FILE *flocked = f;
                flockfile(f);

                for (;;) {
                        int c;

                        if (n >= limit)
                                return -ENOBUFS;

                        errno = 0;
                        c = fgetc_unlocked(f);
                        if (c == EOF) {
                                /* if we read an error, and have no data to return, then propagate the error */
                                if (ferror_unlocked(f) && n == 0)
                                        return errno > 0 ? -errno : -EIO;

                                break;
                        }

                        count++;

                        if (IN_SET(c, '\n', 0)) /* Reached a delimiter */
                                break;

                        if (ret) {
                                if (!GREEDY_REALLOC(buffer, allocated, n + 2))
                                        return -ENOMEM;

                                buffer[n] = (char) c;
                        }

                        n++;
                }
        }

        if (ret) {
                buffer[n] = 0;

                *ret = buffer;
                buffer = NULL;
        }

        return (int) count;
}
