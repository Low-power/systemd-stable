#pragma once

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering
  Copyright 2014 Michal Schmidt

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

#include "macro.h"
#include "siphash24.h"

typedef void (*hash_func_t)(const void *p, struct siphash *state);
typedef int (*compare_func_t)(const void *a, const void *b);

struct hash_ops {
        hash_func_t hash;
        compare_func_t compare;
};

void string_hash_func(const void *p, struct siphash *state);
int string_compare_func(const void *a, const void *b) _pure_;
extern const struct hash_ops string_hash_ops;

void path_hash_func(const void *p, struct siphash *state);
int path_compare_func(const void *a, const void *b) _pure_;
extern const struct hash_ops path_hash_ops;

/* This will compare the passed pointers directly, and will not dereference them. This is hence not useful for strings
 * or suchlike. */
void trivial_hash_func(const void *p, struct siphash *state);
int trivial_compare_func(const void *a, const void *b) _const_;
extern const struct hash_ops trivial_hash_ops;

/* 32bit values we can always just embed in the pointer itself, but in order to support 32bit archs we need store 64bit
 * values indirectly, since they don't fit in a pointer. */
void uint64_hash_func(const void *p, struct siphash *state);
int uint64_compare_func(const void *a, const void *b) _pure_;
extern const struct hash_ops uint64_hash_ops;

/* On some archs dev_t is 32bit, and on others 64bit. And sometimes it's 64bit on 32bit archs, and sometimes 32bit on
 * 64bit archs. Yuck! */
#if SIZEOF_DEV_T != 8
void devt_hash_func(const void *p, struct siphash *state) _pure_;
int devt_compare_func(const void *a, const void *b) _pure_;
extern const struct hash_ops devt_hash_ops;
#else
#define devt_hash_func uint64_hash_func
#define devt_compare_func uint64_compare_func
#define devt_hash_ops uint64_hash_ops
#endif
