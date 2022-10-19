/*	$OpenBSD: kdump.c,v 1.143 2020/04/05 08:32:14 mpi Exp $	*/

/*-
 * Copyright (c) 2021, 2022 Martijn van Duren <martijn_openbsd@imperialat.at>
 * Copyright (c) 2020 Otto Moerbeek <otto@drijf.net>
 * Copyright (c) 1988, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>	/* MAXCOMLEN nitems */
#include <sys/time.h>
#include <sys/signal.h>
#include <sys/ktrace.h>
#include <sys/ioctl.h>
#include <sys/tree.h>

#include <ctype.h>
#include <err.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <util.h>
#include <vis.h>

struct object {
	uintptr_t f;
	char fname[PATH_MAX];
	char *sname;
	RB_ENTRY(object) entry;
};

struct malloc {
	uintptr_t p;
	size_t size;
	struct object *obj[200];
	RB_ENTRY(malloc) entry;
};

enum {
	TIMESTAMP_NONE,
	TIMESTAMP_ABSOLUTE,
	TIMESTAMP_RELATIVE,
	TIMESTAMP_ELAPSED
} timestamp = TIMESTAMP_NONE;

int tail, dump;
char *tracefile = "ktrace.out";
char *malloc_aout = "a.out";
struct ktr_header ktr_header;
pid_t pid_opt = -1;
uintptr_t ptrtrace = 0;
struct malloc *nmptr;
int verbose = 0;
size_t mcur = 0, mmax = 0, mtrigger = 0;
RB_HEAD(objectshead, object) objects = RB_INITIALIZER(&objects);
RB_HEAD(mallocshead, malloc) mallocs = RB_INITIALIZER(&mallocs);

static int fread_tail(void *, size_t, size_t);

static void ktruser(struct ktr_user *, size_t);
static void usage(void);
static void *xmalloc(size_t);
RB_PROTOTYPE_STATIC(objectshead, object, entry, objectcmp)
RB_PROTOTYPE_STATIC(mallocshead, malloc, entry, malloccmp)

int
main(int argc, char *argv[])
{
	int ch, silent;
	size_t ktrlen;
	int trpoints = KTRFAC_USER;
	const char *errstr;
	long long llresult;
	char *endptr;
	uint8_t m[KTR_USER_MAXLEN];
	struct malloc *mptr;
	size_t i;

	while ((ch = getopt(argc, argv, "e:f:Dlm:p:P:v")) != -1)
		switch (ch) {
		case 'e':
			malloc_aout = optarg;
			break;
		case 'f':
			tracefile = optarg;
			break;
		case 'D':
			dump = 1; 
			break;
		case 'l':
			tail = 1;
			break;
		case 'm':
			if ((mtrigger = scan_scaled(optarg, &llresult)) == -1 ||
			    llresult <= 0 || llresult > SIZE_T_MAX)
				err(1, "Invalid -m");
			mtrigger = llresult;
			break;
		case 'p':
			pid_opt = strtonum(optarg, 1, INT_MAX, &errstr);
			if (errstr)
				errx(1, "-p %s: %s", optarg, errstr);
			break;
		case 'P':
			ptrtrace = strtoull(optarg, &endptr, 16);
			if (ptrtrace == 0 || endptr[0] != '\0')
				errx(1, "-P %s: invalid", optarg);
			break;
		case 'v':
			verbose++;
			break;
		default:
			usage();
		}
	if (argc > optind)
		usage();

	if (pledge("stdio rpath getpw", NULL) == -1)
		err(1, "pledge");

	if (strcmp(tracefile, "-") != 0)
		if (!freopen(tracefile, "r", stdin))
			err(1, "%s", tracefile);

	if (fread_tail(&ktr_header, sizeof(struct ktr_header), 1) == 0 ||
	    ktr_header.ktr_type != htobe32(KTR_START))
		errx(1, "%s: not a dump", tracefile);
	while (fread_tail(&ktr_header, sizeof(struct ktr_header), 1)) {
		silent = 0;
		if (pid_opt != -1 && pid_opt != ktr_header.ktr_pid)
			silent = 1;
		if (silent == 0) {
			static pid_t pid;
			if (pid)  {
				if (pid != ktr_header.ktr_pid)
					errx(1, "-M and multiple pids seen, "
					    "select one using -p");
			} else
				pid = ktr_header.ktr_pid;
		}

		ktrlen = ktr_header.ktr_len;
		if (ktrlen && fread_tail(m, ktrlen, 1) == 0)
			errx(1, "data too short");
		if (silent)
			continue;
		if ((trpoints & (1<<ktr_header.ktr_type)) == 0)
			continue;
		switch (ktr_header.ktr_type) {
		case KTR_USER:
			ktruser((struct ktr_user *)m, ktrlen);
			break;
		default:
			break;
		}
		if (mtrigger != 0 && mcur > mtrigger)
			break;
		if (tail)
			(void)fflush(stdout);
	}

	if (!RB_EMPTY(&mallocs) && ptrtrace == 0) {
		printf("Leaks detected:\n");
		RB_FOREACH(mptr, mallocshead, &mallocs) {
			printf("%p: %zu bytes:\n", (void *)mptr->p, mptr->size);
			for (i = 0; mptr->obj[i] != NULL; i++)
				printf("%s", mptr->obj[i]->sname);
		}
	}
	printf("Total memory leaked: %zu\n", mcur);
	printf("Maximum memory: %zu\n", mmax);
		
	return(0);
}

static int
fread_tail(void *buf, size_t size, size_t num)
{
	int i;

	while ((i = fread(buf, size, num, stdin)) == 0 && tail) {
		(void)sleep(1);
		clearerr(stdin);
	}
	return (i);
}

/*
 * Base Formatters
 */

static int
objectcmp(const struct object *o1, const struct object *o2)
{
	return o1->f < o2->f ? -1 : o1->f > o2->f;
}

static int
malloccmp(const struct malloc *m1, const struct malloc *m2)
{
	return m1->p < m2->p ? -1 : m1->p > m2->p;
}

void addr2line(const char *object, uintptr_t addr, char **name);

static void
ktruser(struct ktr_user *usr, size_t len)
{
	uint8_t *u = (uint8_t *)(usr + 1);
	struct object *obj, osearch;
	struct malloc *mptr, msearch;
	size_t i;

	if (len < sizeof(struct ktr_user))
		errx(1, "invalid ktr user length %zu", len);
	len -= sizeof(struct ktr_user);

#if 0
	iwarnxf (dump == 1) {
		if (strcmp(usr->ktr_id, "mallocdumpline") == 0)
			printf("%.*s", (int)len, (unsigned char *)(usr + 1));
		return;
	}
#endif

	if (strcmp(usr->ktr_id, "malloctrobjecterr") == 0) {
		uintptr_t offptr;
		char errmsg[KTR_USER_MAXLEN];

		memcpy(&offptr, u, sizeof(offptr));
		u += sizeof(offptr);
		len -= sizeof(offptr);

		strlcpy(errmsg, u, len);

		warnx("Failed to get tracepoint for %p: %s",
		    (void *)offptr, errmsg);
		return;
	}

	if (strcmp(usr->ktr_id, "malloctrobject") == 0) {
		uintptr_t offptr;

		memcpy(&(osearch.f), u, sizeof(osearch.f));
		u += sizeof(osearch.f);
		len -= sizeof(osearch.f);
		if (RB_FIND(objectshead, &objects, &osearch) != NULL)
			return;
		obj = xmalloc(sizeof(*obj));
		obj->f = osearch.f;
		memcpy(&offptr, u, sizeof(offptr));
		u += sizeof(offptr);
		len -= sizeof(obj->f);
		if (len > sizeof(obj->fname)) {
			warnx("Invalid path size");
			return;
		}
		memcpy(obj->fname, u, len);
		obj->fname[len] = '\0';
		addr2line(len == 0 ? malloc_aout : obj->fname, offptr,
		    &(obj->sname));
		RB_INSERT(objectshead, &objects, obj);
		return;
	}

	if (strcmp(usr->ktr_id, "malloc") == 0) {
		struct malloc *m;

		mptr = xmalloc(sizeof(*mptr));
		memcpy(&(mptr->p), u, sizeof(mptr->p));
		u += sizeof(mptr->p);
		len -= sizeof(mptr->p);
		memcpy(&mptr->size, u, sizeof(mptr->size));
		u += sizeof(mptr->size);
		len -= sizeof(mptr->size);
		for (i = 0; len > 0; i++) {
			if (i >= 200)
				errx(1, "stack trace too long");

			memcpy(&(osearch.f), u, sizeof(osearch.f));
			mptr->obj[i] = RB_FIND(objectshead, &objects, &osearch);
			if (mptr->obj[i] == NULL) {
				obj = xmalloc(sizeof(*obj));
				obj->f = osearch.f;
				memcpy(obj->fname, "???", 3);
				asprintf(&obj->sname, "%#010lx at ?", osearch.f);
				RB_INSERT(objectshead, &objects, obj);
				mptr->obj[i] = obj;
			}
			mptr->obj[i + 1] = NULL;
			u += sizeof(osearch.f);
			len -= sizeof(osearch.f);
		}

		if ((m = RB_INSERT(mallocshead, &mallocs, mptr)) != NULL) {
			fprintf(stderr, "Duplicate malloc found at (%p):\n",
			    (void *)m->p);
			for (i = 0; mptr->obj[i] != NULL; i++)
				fprintf(stderr, "%s", mptr->obj[i]->sname);
			fprintf(stderr, "original:\n");
			for (i = 0; m->obj[i] != NULL; i++)
				fprintf(stderr, "%s", m->obj[i]->sname);
			free(mptr);
			return;
		}

		if (mptr->p == ptrtrace || verbose)
			printf("%p = malloc(%zu): %s", (void *)mptr->p, mptr->size,
			    mptr->obj[0]->sname);

		mcur += mptr->size;
		if (mcur > mmax)
			mmax = mcur;
		return;
	}
	if (strcmp(usr->ktr_id, "realloc") == 0) {
		uintptr_t newptr;
		size_t size;
		struct malloc *m;

		memcpy(&newptr, u, sizeof(newptr));
		u += sizeof(newptr);
		len -= sizeof(newptr);
		memcpy(&(msearch.p), u, sizeof(msearch.p));
		u += sizeof(msearch.p);
		len -= sizeof(msearch.p);
		memcpy(&(size), u, sizeof(size));
		u += sizeof(size);
		len -= sizeof(size);

		memcpy(&(osearch.f), u, sizeof(osearch.f));
		obj = RB_FIND(objectshead, &objects, &osearch);
		if (msearch.p != 0) {
			if ((mptr = RB_FIND(mallocshead, &mallocs,
			    &msearch)) == NULL) {
				if (obj == NULL)
					warnx("realloc ptr %p not found",
					    (void *)msearch.p);
				else
					warnx("realloc ptr %p not found: %s",
					    (void *)msearch.p, obj->sname);
				mptr = xmalloc(sizeof(*mptr));
			} else {
				RB_REMOVE(mallocshead, &mallocs, mptr);
				mcur -= mptr->size;
			}
		} else
			mptr = xmalloc(sizeof(*mptr));
		if (verbose || (ptrtrace != 0 &&
		    (newptr == ptrtrace || msearch.p == ptrtrace)))
			printf("%p = realloc(%p, %zu): %s", (void *)newptr,
			    (void *)msearch.p, size, obj->sname);
		mcur += size;
		if (mcur > mmax)
			mmax = mcur;
		mptr->size = size;
		mptr->p = newptr;
		mptr->obj[0] = obj;
		mptr->obj[1] = NULL;
		if ((m = RB_INSERT(mallocshead, &mallocs, mptr)) != NULL) {
			fprintf(stderr, "Duplicate realloc found at:\n");
			for (i = 0; mptr->obj[i] != NULL; i++)
				fprintf(stderr, "%s", mptr->obj[i]->sname);
			fprintf(stderr, "original:\n");
			for (i = 0; m->obj[i] != NULL; i++)
				fprintf(stderr, "%s", m->obj[i]->sname);
			return;
		}
		return;
	}

	if (strcmp(usr->ktr_id, "free") == 0) {
		memcpy(&(msearch.p), u, sizeof(msearch.p));
		u += sizeof(msearch.p);
		len -= sizeof(msearch.p);
		memcpy(&(osearch.f), u, sizeof(osearch.f));
		obj = RB_FIND(objectshead, &objects, &osearch);

		if ((mptr = RB_FIND(mallocshead, &mallocs, &msearch)) == NULL) {
			if ((obj) == NULL)
				warnx("free ptr %p not found", (void *)msearch.p);
			else
				warnx("free ptr %p not found: %s", (void *)msearch.p, obj->sname);
				
			return;
		}
		if (verbose || mptr->p == ptrtrace)
			printf("free(%p): %s", (void *)mptr->p, obj->sname);
		mcur -= mptr->size;

		RB_REMOVE(mallocshead, &mallocs, mptr);
		free(mptr);
		return;
	}
}

static void
usage(void)
{

	extern char *__progname;
	fprintf(stderr, "usage: %s "
	    "[-Dl] [-e file] [-f file] [-p pid]\n",
	    __progname);
	exit(1);
}

static void *
xmalloc(size_t sz)
{
	void *p = malloc(sz);

	if (p == NULL)
		err(1, NULL);
	return p;
}

RB_GENERATE_STATIC(objectshead, object, entry, objectcmp);
RB_GENERATE_STATIC(mallocshead, malloc, entry, malloccmp);
