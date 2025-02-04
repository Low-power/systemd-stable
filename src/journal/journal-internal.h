#pragma once

/***
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

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

#include <inttypes.h>
#include <stdbool.h>
#include <sys/types.h>

#include "sd-id128.h"
#include "sd-journal.h"

#include "hashmap.h"
#include "journal-def.h"
#include "journal-file.h"
#include "list.h"
#include "set.h"

typedef struct Match Match;
typedef struct Location Location;
typedef struct Directory Directory;

typedef enum MatchType {
        MATCH_DISCRETE,
        MATCH_OR_TERM,
        MATCH_AND_TERM
} MatchType;

struct Match {
        MatchType type;
        Match *parent;
        LIST_FIELDS(Match, matches);

        /* For concrete matches */
        char *data;
        size_t size;
        le64_t le_hash;

        /* For terms */
        LIST_HEAD(Match, matches);
};

struct Location {
        LocationType type;

        bool seqnum_set;
        bool realtime_set;
        bool monotonic_set;
        bool xor_hash_set;

        uint64_t seqnum;
        sd_id128_t seqnum_id;

        uint64_t realtime;

        uint64_t monotonic;
        sd_id128_t boot_id;

        uint64_t xor_hash;
};

struct Directory {
        char *path;
        int wd;
        bool is_root;
        unsigned last_seen_generation;
};

struct sd_journal {
        int toplevel_fd;

        char *path;
        char *prefix;

        OrderedHashmap *files;
        MMapCache *mmap;

        Location current_location;

        JournalFile *current_file;
        uint64_t current_field;

        Match *level0, *level1, *level2;

        pid_t original_pid;

        int inotify_fd;
        unsigned current_invalidate_counter, last_invalidate_counter;
        usec_t last_process_usec;
        unsigned generation;

        /* Iterating through unique fields and their data values */
        char *unique_field;
        JournalFile *unique_file;
        uint64_t unique_offset;

        /* Iterating through known fields */
        JournalFile *fields_file;
        uint64_t fields_offset;
        uint64_t fields_hash_table_index;
        char *fields_buffer;
        size_t fields_buffer_allocated;

        int flags;

        bool on_network:1;
        bool no_new_files:1;
        bool no_inotify:1;
        bool unique_file_lost:1; /* File we were iterating over got
                                    removed, and there were no more
                                    files, so sd_j_enumerate_unique
                                    will return a value equal to 0. */
        bool fields_file_lost:1;
        bool has_runtime_files:1;
        bool has_persistent_files:1;

        size_t data_threshold;

        Hashmap *directories_by_path;
        Hashmap *directories_by_wd;

        Hashmap *errors;
};

char *journal_make_match_string(sd_journal *j);
void journal_print_header(sd_journal *j);

#define JOURNAL_FOREACH_DATA_RETVAL(j, data, l, retval)                     \
        for (sd_journal_restart_data(j); ((retval) = sd_journal_enumerate_data((j), &(data), &(l))) > 0; )
