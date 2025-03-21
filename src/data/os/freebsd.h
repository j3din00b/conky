/*
 *
 * Conky, a system monitor, based on torsmo
 *
 * Any original torsmo code is licensed under the BSD license
 *
 * All code written since the fork of torsmo is licensed under the GPL
 *
 * Please see COPYING for details
 *
 * Copyright (c) 2004, Hannu Saransaari and Lauri Hakkarainen
 * Copyright (c) 2005-2024 Brenden Matthews, Philip Kovacs, et. al.
 *	(see AUTHORS)
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef FREEBSD_H_
#define FREEBSD_H_

#include <fcntl.h>
#include <kvm.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/ucred.h>
#include "../../common.h"
#if (defined(i386) || defined(__i386__))
#include <machine/apm_bios.h>
#endif /* i386 || __i386__ */

extern kvm_t *kd;

int get_entropy_avail(unsigned int *);
int get_entropy_poolsize(unsigned int *);
void print_sysctlbyname(struct text_object *, char *, unsigned int);

bool is_conky_already_running(void);

#endif /*FREEBSD_H_*/
