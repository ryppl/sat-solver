/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * pool.h
 * 
 */

#ifndef SATSOLVER_POOL_H
#define SATSOLVER_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

#include "pooltypes.h"
#include "poolid.h"
#include "solvable.h"
#include "bitmap.h"
#include "queue.h"
#include "strpool.h"

// see initpool_data[] in pool.c

/* well known ids */
#define ID_NULL			STRID_NULL
#define ID_EMPTY		STRID_EMPTY
#define SOLVABLE_NAME		2
#define SOLVABLE_ARCH		3
#define SOLVABLE_EVR		4
#define SOLVABLE_VENDOR		5
#define SOLVABLE_PROVIDES	6
#define SOLVABLE_OBSOLETES	7
#define SOLVABLE_CONFLICTS	8
#define SOLVABLE_REQUIRES	9
#define SOLVABLE_RECOMMENDS	10
#define SOLVABLE_SUGGESTS	11
#define SOLVABLE_SUPPLEMENTS	12
#define SOLVABLE_ENHANCES	13
#define SOLVABLE_FRESHENS	14
#define RPM_RPMDBID		15
#define SOLVABLE_PREREQMARKER	16		// normal requires before this, prereqs after this
#define SOLVABLE_FILEMARKER	17		// normal provides before this, generated file provides after this
#define NAMESPACE_INSTALLED	18
#define NAMESPACE_MODALIAS	19
#define NAMESPACE_SPLITPROVIDES 20
#define NAMESPACE_LANGUAGE	21
#define NAMESPACE_FILESYSTEM	22
#define SYSTEM_SYSTEM		23
#define ARCH_SRC		24
#define ARCH_NOSRC		25
#define ARCH_NOARCH		26
#define REPODATA_EXTERNAL	27
#define REPODATA_KEYS		28
#define REPODATA_LOCATION	29

/* The void type is usable to encode one-valued attributes, they have
   no associated data.  This is useful to encode values which many solvables
   have in common, and whose overall set is relatively limited.  A prime
   example would be the media number.  The actual value is encoded in the
   SIZE member of the key structure.  Be warned: careless use of this
   leads to combinatoric explosion of number of schemas.  */
#define REPOKEY_TYPE_VOID	30
#define REPOKEY_TYPE_CONSTANT	31
#define REPOKEY_TYPE_CONSTANTID	32
#define REPOKEY_TYPE_ID		33
#define REPOKEY_TYPE_NUM	34
#define REPOKEY_TYPE_U32	35
#define REPOKEY_TYPE_DIR	36
#define REPOKEY_TYPE_STR	37
#define REPOKEY_TYPE_IDARRAY	38
#define REPOKEY_TYPE_REL_IDARRAY	39
#define REPOKEY_TYPE_DIRSTRARRAY	40
#define REPOKEY_TYPE_DIRNUMNUMARRAY	41

#define SOLVABLE_SUMMARY	42
#define SOLVABLE_DESCRIPTION	43
#define SOLVABLE_AUTHORS	44
#define SOLVABLE_GROUP		45
#define SOLVABLE_KEYWORDS	46
#define SOLVABLE_LICENSE	47
#define SOLVABLE_BUILDTIME	48
#define SOLVABLE_EULA		49
#define SOLVABLE_MESSAGEINS	50
#define SOLVABLE_MESSAGEDEL	51
#define SOLVABLE_INSTALLSIZE	52
#define SOLVABLE_DISKUSAGE	53
#define SOLVABLE_FILELIST	54
#define SOLVABLE_INSTALLTIME	55
#define SOLVABLE_MEDIADIR	56
#define SOLVABLE_MEDIAFILE	57
#define SOLVABLE_MEDIANR	58
#define SOLVABLE_DOWNLOADSIZE	59
#define SOLVABLE_SOURCEARCH	60
#define SOLVABLE_SOURCENAME	61
#define SOLVABLE_SOURCEEVR	62
#define SOLVABLE_ISVISIBLE	63

#define SOLVABLE_PATCHCATEGORY	64
#define SOLVABLE_HEADEREND      65

#define ID_NUM_INTERNAL		66


/* well known solvable */
#define SYSTEMSOLVABLE		1


/* how many strings to maintain (round robin) */
#define POOL_TMPSPACEBUF 16

//-----------------------------------------------

struct _Repo;
struct _Repodata;
struct _Repokey;
struct _KeyValue;

struct _Pool {
  struct _Stringpool ss;

  Reldep *rels;               // table of rels: Id -> Reldep
  int nrels;                  // number of unique rels
  Hashtable relhashtbl;       // hash table: (name,evr,op ->) Hash -> Id
  Hashmask relhashmask;

  struct _Repo **repos;
  int nrepos;

  Solvable *solvables;
  int nsolvables;

  const char **languages;
  int nlanguages;
  Id *languagecache;
  int languagecacheother;

  int promoteepoch;             /* 0/1  */

  Id *id2arch;			/* map arch ids to scores */
  Id lastarch;			/* last valid entry in id2arch */
  Queue vendormap;		/* map vendor to vendorclasses mask */

  /* providers data, as two-step indirect list
   * whatprovides[Id] -> Offset into whatprovidesdata for name
   * whatprovidesdata[Offset] -> ID_NULL-terminated list of solvables providing Id
   */
  Offset *whatprovides;		/* Offset to providers of a specific name, Id -> Offset  */
  Offset *whatprovides_rel;	/* Offset to providers of a specific relation, Id -> Offset  */

  Id *whatprovidesdata;		/* Ids of solvable providing Id */
  Offset whatprovidesdataoff;	/* next free slot within whatprovidesdata */
  int whatprovidesdataleft;	/* number of 'free slots' within whatprovidesdata */

  /* If nonzero, then consider only the solvables with Ids set in this
     bitmap for solving.  If zero, consider all solvables.  */
  Map *considered;

  Id (*nscallback)(struct _Pool *, void *data, Id name, Id evr);
  void *nscallbackdata;

  /* our tmp space string space */
  char *tmpspacebuf[POOL_TMPSPACEBUF];
  int   tmpspacelen[POOL_TMPSPACEBUF];
  int   tmpspacen;

  /* debug mask and callback */
  int  debugmask;
  void (*debugcallback)(struct _Pool *, void *data, int type, const char *str);
  void *debugcallbackdata;

  /* load callback */
  FILE * (*loadcallback)(struct _Pool *, struct _Repodata *, void *);
  void *loadcallbackdata;
};

#define SAT_FATAL			(1<<0)
#define SAT_ERROR			(1<<1)
#define SAT_WARN			(1<<2)
#define SAT_DEBUG_STATS			(1<<3)
#define SAT_DEBUG_RULE_CREATION		(1<<4)
#define SAT_DEBUG_PROPAGATE		(1<<5)
#define SAT_DEBUG_ANALYZE		(1<<6)
#define SAT_DEBUG_UNSOLVABLE		(1<<7)
#define SAT_DEBUG_SOLUTIONS		(1<<8)
#define SAT_DEBUG_POLICY		(1<<9)
#define SAT_DEBUG_RESULT		(1<<10)
#define SAT_DEBUG_JOB			(1<<11)
#define SAT_DEBUG_SCHUBI		(1<<12)

//-----------------------------------------------


/* mark dependencies with relation by setting bit31 */

#define MAKERELDEP(id) ((id) | 0x80000000)
#define ISRELDEP(id) (((id) & 0x80000000) != 0)
#define GETRELID(id) ((id) ^ 0x80000000)				/* returns Id */
#define GETRELDEP(pool, id) ((pool)->rels + ((id) ^ 0x80000000))	/* returns Reldep* */

#define REL_GT		1
#define REL_EQ		2
#define REL_LT		4

#define REL_AND		16
#define REL_OR		17
#define REL_WITH	18
#define REL_NAMESPACE	19

#if !defined(__GNUC__) && !defined(__attribute__)
# define __attribute__(x)
#endif

/**
 * Creates a new pool
 */
extern Pool *pool_create(void);
/**
 * Delete a pool
 */
extern void pool_free(Pool *pool);

extern void pool_debug(Pool *pool, int type, const char *format, ...) __attribute__((format(printf, 3, 4)));

extern char *pool_alloctmpspace(Pool *pool, int len);

/**
 * Solvable management
 */
extern Id pool_add_solvable(Pool *pool);
extern Id pool_add_solvable_block(Pool *pool, int count);

extern void pool_free_solvable_block(Pool *pool, Id start, int count, int reuseids);
static inline Solvable *pool_id2solvable(Pool *pool, Id p)
{
  return pool->solvables + p;
}
extern const char *solvable2str(Pool *pool, Solvable *s);

void pool_set_languages(Pool *pool, const char **languages, int nlanguages);

unsigned int solvable_lookup_num(Solvable *s, Id keyname, unsigned int notfound);
const char *solvable_lookup_str(Solvable *s, Id keyname);
const char *solvable_lookup_str_lang(Solvable *s, Id keyname);
int solvable_lookup_bool(Solvable *s, Id keyname);
char * solvable_get_location(Solvable *s, unsigned int *medianrp);



/**
 * Prepares a pool for solving
 */
extern void pool_createwhatprovides(Pool *pool);
extern void pool_addfileprovides(Pool *pool, struct _Repo *installed);
extern void pool_freewhatprovides(Pool *pool);
extern Id pool_queuetowhatprovides(Pool *pool, Queue *q);

static inline int pool_installable(Pool *pool, Solvable *s)
{
  if (!s->arch || s->arch == ARCH_SRC || s->arch == ARCH_NOSRC)
    return 0;
  if (pool->id2arch && (s->arch > pool->lastarch || !pool->id2arch[s->arch]))
    return 0;
  if (pool->considered)
    { 
      Id id = s - pool->solvables;
      if (!MAPTST(pool->considered, id))
	return 0;
    }
  return 1;
}

extern Id *pool_addrelproviders(Pool *pool, Id d);

static inline Id *pool_whatprovides(Pool *pool, Id d)
{
  Id v;
  if (!ISRELDEP(d))
    return pool->whatprovidesdata + pool->whatprovides[d];
  v = GETRELID(d);
  if (pool->whatprovides_rel[v])
    return pool->whatprovidesdata + pool->whatprovides_rel[v];
  return pool_addrelproviders(pool, d);
}

extern void pool_setdebuglevel(Pool *pool, int level);

static inline void pool_setdebugcallback(Pool *pool, void (*debugcallback)(struct _Pool *, void *data, int type, const char *str), void *debugcallbackdata)
{
  pool->debugcallback = debugcallback;
  pool->debugcallbackdata = debugcallbackdata;
}

static inline void pool_setdebugmask(Pool *pool, int mask)
{
  pool->debugmask = mask;
}

static inline void pool_setloadcallback(Pool *pool, FILE *(*cb)(struct _Pool *, struct _Repodata *, void *), void *loadcbdata)
{
  pool->loadcallback = cb;
  pool->loadcallbackdata = loadcbdata;
}

/* search the pool. the following filters are available:
 *   p     - search just this solvable
 *   key   - search only this key
 *   match - key must match this string
 */
void pool_search(Pool *pool, Id p, Id key, const char *match, int flags, int (*callback)(void *cbdata, Solvable *s, struct _Repodata *data, struct _Repokey *key, struct _KeyValue *kv), void *cbdata);

/* loop over all providers of d */
#define FOR_PROVIDES(v, vp, d) 						\
  for (vp = pool_whatprovides(pool, d) ; (v = *vp++) != 0; )

#define POOL_DEBUG(type, ...) do {if ((pool->debugmask & (type)) != 0) pool_debug(pool, (type), __VA_ARGS__);} while (0)
#define IF_POOLDEBUG(type) if ((pool->debugmask & (type)) != 0)

#ifdef __cplusplus
}
#endif


#endif /* SATSOLVER_POOL_H */
