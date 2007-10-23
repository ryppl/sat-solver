/*
 * pool.c
 * 
 * The pool contains information about solvables
 * stored optimized for memory consumption and fast retrieval.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "pool.h"
#include "poolid.h"
#include "poolid_private.h"
#include "poolarch.h"
#include "util.h"
#include "evr.h"


// reset all whatprovides
// 
void
pool_freewhatprovides(Pool *pool)
{
  pool->whatprovides = xfree(pool->whatprovides);
  pool->whatprovidesdata = xfree(pool->whatprovidesdata);
  pool->whatprovidesdataoff = 0;
  pool->whatprovidesdataleft = 0;
}


// list of string constants, so we can do pointer/Id instead of string comparison
// index into array matches ID_xxx constants in pool.h

static char *initpool_data[] = {
  "<NULL>",                   // ID_NULL
  "",                         // ID_EMPTY
  "solvable:name",
  "solvable:arch",
  "solvable:evr",
  "solvable:provides",
  "solvable:obsoletes",
  "solvable:conflicts",
  "solvable:requires",
  "solvable:recommends",
  "solvable:suggests",
  "solvable:supplements",
  "solvable:enhances",
  "solvable:freshens",
  "rpm:dbid",			       /* direct key into rpmdb */
  "solvable:prereqmarker",
  "solvable:filemarker",
  "namespace:installed",
  "namespace:modalias",
  "src",
  "nosrc",
  "noarch"
};

// create pool
// 
Pool *
pool_create(void)
{
  int count, totalsize = 0;
  Pool *pool;

  pool = (Pool *)xcalloc(1, sizeof(*pool));

  // count number and total size of predefined strings
  for (count = 0; count < sizeof(initpool_data)/sizeof(*initpool_data); count++)
    totalsize += strlen(initpool_data[count]) + 1;

  // alloc appropriate space
  pool->stringspace = (char *)xmalloc((totalsize + STRINGSPACE_BLOCK) & ~STRINGSPACE_BLOCK);
  pool->strings = (Offset *)xmalloc(((count + STRING_BLOCK) & ~STRING_BLOCK) * sizeof(Offset));

  // now copy predefined strings into allocated space
  pool->sstrings = 0;
  for (count = 0; count < sizeof(initpool_data)/sizeof(*initpool_data); count++)
    {
      strcpy(pool->stringspace + pool->sstrings, initpool_data[count]);
      pool->strings[count] = pool->sstrings;
      pool->sstrings += strlen(initpool_data[count]) + 1;
    }
  pool->nstrings = count;

  // pre-alloc space for a RelDep
  pool->rels = (Reldep *)xcalloc(1 + REL_BLOCK, sizeof(Reldep));
  pool->nrels = 1;

  // pre-alloc space for a Solvable
  pool->solvables = (Solvable *)xcalloc(1, sizeof(Solvable));
  pool->nsolvables = 1;

  return pool;
}


// empty the pool
// 
void
pool_free(Pool *pool)
{
  int i;
  Source *source;

  pool_freewhatprovides(pool);
  pool_freeidhashes(pool);
  for (i = 0; i < pool->nsources; i++)
    {
      source = pool->sources[i];
      xfree(source->idarraydata);
      xfree(source->rpmdbid);
      xfree(source);
    }
  xfree(pool->solvables);
  xfree(pool->sources);
  xfree(pool->stringspace);
  xfree(pool->strings);
  xfree(pool->rels);
  xfree(pool);
}


static Pool *pool_shrink_whatprovides_sortcmp_data;

static int
pool_shrink_whatprovides_sortcmp(const void *ap, const void *bp)
{
  int r;
  Pool *pool = pool_shrink_whatprovides_sortcmp_data;
  Id oa, ob, *da, *db;
  oa = pool->whatprovides[*(Id *)ap];
  ob = pool->whatprovides[*(Id *)bp];
  if (oa == ob)
    return *(Id *)ap - *(Id *)bp;
  if (!oa)
    return -1;
  if (!ob)
    return 1;
  da = pool->whatprovidesdata + oa;
  db = pool->whatprovidesdata + ob;
  while (*db)
    if ((r = (*da++ - *db++)) != 0)
      return r;
  if (*da)
    return *da;
  return *(Id *)ap - *(Id *)bp;
}

static void
pool_shrink_whatprovides(Pool *pool)
{
  Id i, id;
  Id *sorted;
  Id lastid, *last, *dp, *lp;
  Offset o;
  int r;

  if (pool->nstrings < 3)
    return;
  sorted = xmalloc2(pool->nstrings, sizeof(Id));
  for (id = 0; id < pool->nstrings; id++)
    sorted[id] = id;
  pool_shrink_whatprovides_sortcmp_data = pool;
  qsort(sorted + 1, pool->nstrings - 1, sizeof(Id), pool_shrink_whatprovides_sortcmp);
  last = 0;
  lastid = 0;
  for (i = 1; i < pool->nstrings; i++)
    {
      id = sorted[i];
      o = pool->whatprovides[id];
      if (o == 0 || o == 1)
	continue;
      dp = pool->whatprovidesdata + o;
      if (last)
	{
	  lp = last;
	  while (*dp)	
	    if (*dp++ != *lp++)
	      {
		last = 0;
	        break;
	      }
	  if (last && *lp)
	    last = 0;
	  if (last)
	    {
	      pool->whatprovides[id] = -lastid;
	      continue;
	    }
	}
      last = pool->whatprovidesdata + o;
      lastid = id;
    }
  xfree(sorted);
  dp = pool->whatprovidesdata + 2;
  for (id = 1; id < pool->nstrings; id++)
    {
      o = pool->whatprovides[id];
      if (o == 0 || o == 1)
	continue;
      if ((Id)o < 0)
	{
	  i = -(Id)o;
	  if (i >= id)
	    abort();
	  pool->whatprovides[id] = pool->whatprovides[i];
	  continue;
	}
      lp = pool->whatprovidesdata + o;
      if (lp < dp)
	abort();
      pool->whatprovides[id] = dp - pool->whatprovidesdata;
      while ((*dp++ = *lp++) != 0)
	;
    }
  o = dp - pool->whatprovidesdata;
  if (pool->verbose)
    printf("shrunk whatprovidesdata from %d to %d\n", pool->whatprovidesdataoff, o);
  if (pool->whatprovidesdataoff == o)
    return;
  r = pool->whatprovidesdataoff - o;
  pool->whatprovidesdataoff = o;
  pool->whatprovidesdata = xrealloc(pool->whatprovidesdata, (o + pool->whatprovidesdataleft) * sizeof(Id));
  if (r > pool->whatprovidesdataleft)
    r = pool->whatprovidesdataleft;
  memset(pool->whatprovidesdata + o, 0, r * sizeof(Id));
}


/*
 * pool_prepare()
 * 
 * create hashes over complete pool to ease lookups
 * 
 */

void
pool_prepare(Pool *pool)
{
  int i, num, np, extra;
  unsigned int n;
  Offset off;
  Solvable *s;
  Id id;
  Offset *idp;
  Offset *whatprovides;
  Id *whatprovidesdata, *d;

  if (pool->verbose)
    printf("number of solvables: %d\n", pool->nsolvables);
  if (pool->verbose)
    printf("number of ids: %d + %d\n", pool->nstrings, pool->nrels);

  pool_freeidhashes(pool);
  pool_freewhatprovides(pool);
  num = pool->nstrings + pool->nrels;
  whatprovides = (Offset *)xcalloc(num, sizeof(Offset));

  /* count providers for each name */
  for (i = 1; i < pool->nsolvables; i++)
    {
      Id *pp;
      s = pool->solvables + i;
      if (!s->provides)
	continue;
      if (!pool_installable(pool, s))
	continue;
      pp = s->source->idarraydata + s->provides;
      while ((id = *pp++) != ID_NULL)
	{
	  if (ISRELDEP(id))
	    {
	      Reldep *rd = GETRELDEP(pool, id);
	      id = rd->name;
	    }
	  whatprovides[id]++;	       /* inc count of providers */
	}
    }

  off = 2;	/* first entry is undef, second is empty list */
  idp = whatprovides;
  np = 0;			       /* number of names provided */
  for (i = 0; i < num; i++, idp++)
    {
      n = *idp;
      if (!n)			       /* no providers */
	continue;
      *idp = off;		       /* move from counts to offsets into whatprovidesdata */
      off += n + 1;		       /* make space for all providers + terminating ID_NULL */
      np++;			       /* inc # of provider 'slots' */
    }

  if (pool->verbose)
    printf("provide ids: %d\n", np);
  extra = 2 * pool->nrels;

  if (extra < 256)
    extra = 256;

  if (pool->verbose)
    printf("provide space needed: %d + %d\n", off, extra);

  /* alloc space for all providers + extra */
  whatprovidesdata = (Id *)xcalloc(off + extra, sizeof(Id));

  /* now fill data for all provides */
  for (i = 1; i < pool->nsolvables; i++)
    {
      Id *pp;
      s = pool->solvables + i;
      if (!s->provides)
	continue;
      if (!pool_installable(pool, s))
	continue;

      /* for all provides of this solvable */
      pp = s->source->idarraydata + s->provides;
      while ((id = *pp++) != 0)
	{
	  if (ISRELDEP(id))
	    {
	      Reldep *rd = GETRELDEP(pool, id);
	      id = rd->name;
	    }
	  d = whatprovidesdata + whatprovides[id];   /* offset into whatprovidesdata */
	  if (*d)
	    {
	      d++;
	      while (*d)	       /* find free slot */
		d++;
	      if (d[-1] == i)
		{
#if 0
		  if (pool->verbose) printf("duplicate entry for %s in package %s.%s\n", id2str(pool, id), id2str(pool, s->name), id2str(pool, s->arch));
#endif
		  continue;
		}
	    }
	  *d = i;		       /* put solvable Id into data */
	}
    }
  pool->whatprovides = whatprovides;
  pool->whatprovidesdata = whatprovidesdata;
  pool->whatprovidesdataoff = off;
  pool->whatprovidesdataleft = extra;
  pool_shrink_whatprovides(pool);
}

/******************************************************************************/

/*
 * pool_queuetowhatprovides
 * 
 * on-demand filling of provider information
 * move queue data into whatprovidesdata
 * q: queue of Ids
 * returns: Offset into whatprovides
 */

Id
pool_queuetowhatprovides(Pool *pool, Queue *q)
{
  Offset off;
  int count = q->count;

  if (count == 0)		       /* queue empty -> ID_EMPTY */
    return ID_EMPTY;

  /* extend whatprovidesdata if needed, +1 for ID_NULL-termination */
  if (pool->whatprovidesdataleft < count + 1)
    {
      if (pool->verbose)
        printf("growing provides hash data...\n");
      pool->whatprovidesdata = (Id *)xrealloc(pool->whatprovidesdata, (pool->whatprovidesdataoff + count + 4096) * sizeof(Id));
      pool->whatprovidesdataleft = count + 4096;
    }

  /* copy queue to next free slot */
  off = pool->whatprovidesdataoff;
  memcpy(pool->whatprovidesdata + pool->whatprovidesdataoff, q->elements, count * sizeof(Id));

  /* adapt count and ID_NULL-terminate */
  pool->whatprovidesdataoff += count;
  pool->whatprovidesdata[pool->whatprovidesdataoff++] = ID_NULL;
  pool->whatprovidesdataleft -= count + 1;

  return (Id)off;
}


/*
 * addrangedep
 * 
 * add RangeDep to whatprovides
 * no exact providers, do range match
 * 
 */

Id *
pool_addrelproviders(Pool *pool, Id d)
{
  Reldep *rd = GETRELDEP(pool, d);
  Reldep *prd;
  Queue plist;
  Id buf[16];
  Id name = rd->name;
  Id evr = rd->evr;
  int flags = rd->flags;
  Id pid, *pidp;
  Id p, *pp, *pp2, *pp3;

  d = GETRELID(pool, d);
  queueinit_buffer(&plist, buf, sizeof(buf)/sizeof(*buf));
  switch (flags)
    {
    case REL_AND:
    case REL_WITH:
      pp = GET_PROVIDESP(name, p);
      pp2 = GET_PROVIDESP(evr, p);
      while ((p = *pp++) != 0)
	{
	  for (pp3 = pp2; *pp3;)
	    if (*pp3++ == p)
	      {
	        queuepush(&plist, p);
		break;
	      }
	}
      break;
    case REL_OR:
      pp = GET_PROVIDESP(name, p);
      while ((p = *pp++) != 0)
	queuepush(&plist, p);
      pp = GET_PROVIDESP(evr, p);
      while ((p = *pp++) != 0)
	queuepushunique(&plist, p);
      break;
    case REL_NAMESPACE:
#if 0
      /* unknown namespace, just pass through */
      pp = GET_PROVIDESP(evr, p);
      while ((p = *pp++) != 0)
	queuepush(&plist, p);
#endif
      break;
    default:
      break;
    }

  /* convert to whatprovides id */
#if 0
  if (pool->verbose)
    printf("addrelproviders: what provides %s?\n", id2str(pool, name));
#endif
  if (flags && flags < 8)
    {
      FOR_PROVIDES(p, pp, name)
	{
#if 0
	  if (pool->verbose)
	    printf("addrelproviders: checking package %s\n", id2str(pool, pool->p[p].name));
#endif
	  /* solvable p provides name in some rels */
	  pidp = pool->solvables[p].source->idarraydata + pool->solvables[p].provides;
	  while ((pid = *pidp++) != 0)
	    {
	      int pflags;
	      Id pevr;

	      if (pid == name)
		break;		/* yes, provides all versions */
	      if (!ISRELDEP(pid))
		continue;		/* wrong provides name */
	      prd = GETRELDEP(pool, pid);
	      if (prd->name != name)
		continue;		/* wrong provides name */
	      /* right package, both deps are rels */
	      pflags = prd->flags;
	      if (!pflags)
		continue;
	      if (flags == 7 || pflags == 7)
		break; /* included */
	      if ((pflags & flags & 5) != 0)
		break; /* same direction, match */
	      pevr = prd->evr;
	      if (pevr == evr)
		{
		  if ((pflags & flags & 2) != 0)
		    break; /* both have =, match */
		}
	      else
		{
		  int f = flags == 5 ? 5 : flags == 2 ? pflags : (flags ^ 5) & (pflags | 5);
		  if ((f & (1 << (1 + evrcmp(pool, pevr, evr)))) != 0)
		    break;
		}
	    }
	  if (!pid)
	    continue;	/* no rel match */
	  queuepush(&plist, p);
	}
    }
  /* add providers to whatprovides */
#if 0
  if (pool->verbose) printf("addrelproviders: adding %d packages to %d\n", plist.count, d);
#endif
  pool->whatprovides[d] = pool_queuetowhatprovides(pool, &plist);
  queuefree(&plist);

  return pool->whatprovidesdata + pool->whatprovides[d];
}

// EOF
