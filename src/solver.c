/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * solver.c
 *
 * SAT based dependency solver
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "solver.h"
#include "bitmap.h"
#include "pool.h"
#include "util.h"
#include "evr.h"
#include "poolvendor.h"

#include "policy.h"

#define RULES_BLOCK 63

static Pool *prune_best_version_arch_sortcmp_data;

/*-----------------------------------------------------------------*/

/*
 * prep for prune_best_version_arch
 *   sort by name
 */

static int
prune_best_version_arch_sortcmp(const void *ap, const void *bp)
{
  Pool *pool = prune_best_version_arch_sortcmp_data;
  int r;
  Id a = *(Id *)ap;
  Id b = *(Id *)bp;
  r = pool->solvables[a].name - pool->solvables[b].name;
  if (r)
    {
      const char *na, *nb;
      /* different names. We use real strcmp here so that the result
       * is not depending on some random solvable order */
      na = id2str(pool, pool->solvables[a].name);
      nb = id2str(pool, pool->solvables[b].name);
      /* bring selections and patterns to the front */
      if (!strncmp(na, "pattern:", 8))
	{
          if (strncmp(nb, "pattern:", 8))
	    return -1;
	}
      else if (!strncmp(nb, "pattern:", 8))
	{
          if (strncmp(na, "pattern:", 8))
	    return 1;
	}
      if (!strncmp(na, "selection:", 10))
	{
          if (strncmp(nb, "selection:", 10))
	    return -1;
	}
      else if (!strncmp(nb, "selection:", 10))
	{
          if (strncmp(na, "selection:", 10))
	    return 1;
	}
      return strcmp(na, nb);
    }
  return a - b;
}


#if 0
static Id
replaces_installed(Solver *solv, Id id)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  Id *name = pool->solvables[id].name;

  FOR_PROVIDES(p, pp, id)
    {
      s = pool->solvables + p;
      if (s->name != name)
	continue;
      if (p >= installed->start && p < installed->start + installed->nsolvables)
	return p;
    }
}
#endif

static int
dep_installed(Solver *solv, Id dep)
{
  /* disable for now, splitprovides don't work anyway and it breaks
     a testcase */
#if 0
  Pool *pool = solv->pool;
  Id p, *pp;

  if (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags == REL_AND)
	{
	  if (!dep_installed(solv, rd->name))
	    return 0;
	  return dep_installed(solv, rd->evr);
	}
      if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_INSTALLED)
	return dep_installed(solv, rd->evr);
    }
  FOR_PROVIDES(p, pp, dep)
    {
      if (p >= solv->installed->start && p < solv->installed->start + solv->installed->nsolvables)
	return 1;
    }
#endif
  return 0;
}

static inline int
dep_fulfilled(Solver *solv, Id dep)
{
  Pool *pool = solv->pool;
  Id p, *pp;

  if (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags == REL_AND)
	{
	  if (!dep_fulfilled(solv, rd->name))
	    return 0;
	  return dep_fulfilled(solv, rd->evr);
	}
      if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_INSTALLED)
	return dep_installed(solv, rd->evr);
    }
  FOR_PROVIDES(p, pp, dep)
    {
      if (solv->decisionmap[p] > 0)
	return 1;
    }
  return 0;
}

static inline int
dep_possible(Solver *solv, Id dep, Map *m)
{
  Pool *pool = solv->pool;
  Id p, *pp;

  if (ISRELDEP(dep))
    {
      Reldep *rd = GETRELDEP(pool, dep);
      if (rd->flags == REL_AND)
	{
	  if (!dep_possible(solv, rd->name, m))
	    return 0;
	  return dep_possible(solv, rd->evr, m);
	}
      if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_INSTALLED)
	return dep_installed(solv, rd->evr);
    }
  FOR_PROVIDES(p, pp, dep)
    {
      if (MAPTST(m, p))
	return 1;
    }
  return 0;
}

static inline int
is_supplemented(Solver *solv, Solvable *s)
{
  Id sup, *supp;
  if (!s->supplements && !s->freshens)
    return 0;
  if (s->supplements)
    {
      supp = s->repo->idarraydata + s->supplements;
      while ((sup = *supp++) != 0)
	if (dep_fulfilled(solv, sup))
	  break;
      if (!sup)
	return 0;
    }
  if (s->freshens)
    {
      supp = s->repo->idarraydata + s->freshens;
      while ((sup = *supp++) != 0)
	if (dep_fulfilled(solv, sup))
	  break;
      if (!sup)
	return 0;
    }
  return 1;
}

static void
prune_to_highest_prio(Pool *pool, Queue *plist)
{
  int i, j;
  Solvable *s;
  int bestprio = 0;

  /* prune to highest priority */
  for (i = 0; i < plist->count; i++)
    {
      s = pool->solvables + plist->elements[i];
      if (i == 0 || s->repo->priority > bestprio)
	bestprio = s->repo->priority;
    }
  for (i = j = 0; i < plist->count; i++)
    {
      s = pool->solvables + plist->elements[i];
      if (s->repo->priority == bestprio)
	plist->elements[j++] = plist->elements[i];
    }
  plist->count = j;
}

/*
 * prune_to_recommended
 *
 * XXX: should we prune to requires/suggests that are already
 * fulfilled by other packages?
 */
static void
prune_to_recommended(Solver *solv, Queue *plist)
{
  Pool *pool = solv->pool;
  int i, j;
  Solvable *s;
  Id p, *pp, rec, *recp, sug, *sugp, enh, *enhp;

  if (solv->recommends_index < 0)
    {
      MAPZERO(&solv->recommendsmap);
      MAPZERO(&solv->suggestsmap);
      solv->recommends_index = 0;
    }
  while (solv->recommends_index < solv->decisionq.count)
    {
      p = solv->decisionq.elements[solv->recommends_index++];
      if (p < 0)
	continue;
      s = pool->solvables + p;
      if (s->recommends)
	{
	  recp = s->repo->idarraydata + s->recommends;
          while ((rec = *recp++) != 0)
	    FOR_PROVIDES(p, pp, rec)
	      MAPSET(&solv->recommendsmap, p);
	}
      if (s->suggests)
	{
	  sugp = s->repo->idarraydata + s->suggests;
          while ((sug = *sugp++) != 0)
	    FOR_PROVIDES(p, pp, sug)
	      MAPSET(&solv->suggestsmap, p);
	}
    }
  /* prune to recommended/supplemented */
  for (i = j = 0; i < plist->count; i++)
    {
      p = plist->elements[i];
      if (MAPTST(&solv->recommendsmap, p))
	{
	  plist->elements[j++] = p;
	  continue;
	}
      if (is_supplemented(solv, pool->solvables + p))
        plist->elements[j++] = p;
    }
  if (j)
    plist->count = j;

  /* prune to suggested/enhanced*/
  if (plist->count < 2)
    return;
  for (i = j = 0; i < plist->count; i++)
    {
      p = plist->elements[i];
      if (MAPTST(&solv->suggestsmap, p))
	{
	  plist->elements[j++] = p;
	  continue;
	}
      s = pool->solvables + p;
      if (!s->enhances)
	continue;
      enhp = s->repo->idarraydata + s->enhances;
      while ((enh = *enhp++) != 0)
	if (dep_fulfilled(solv, enh))
	  break;
      if (!enh)
	continue;
      plist->elements[j++] = s - pool->solvables;
    }
  if (j)
    plist->count = j;
}

/*
 * prune_best_version_arch
 * 
 * sort list of packages (given through plist) by name and evr
 * return result through plist
 * 
 */

/* FIXME: should prefer installed if identical version */

void
prune_best_version_arch(Pool *pool, Queue *plist)
{
  Id best = ID_NULL;
  int i, j;
  Solvable *s;
  Id a, bestscore;

  if (plist->count < 2)		/* no need to prune for a single entry */
    return;
  if (pool->verbose > 1) printf("prune_best_version_arch %d\n", plist->count);

  /* prune to best architecture */
  if (pool->id2arch)
    {
      bestscore = 0;
      for (i = 0; i < plist->count; i++)
	{
	  s = pool->solvables + plist->elements[i];
	  a = s->arch;
	  a = a <= pool->lastarch ? pool->id2arch[a] : 0;
	  if (a && a != 1 && (!bestscore || a < bestscore))
	    bestscore = a;
	}
      for (i = j = 0; i < plist->count; i++)
	{
	  s = pool->solvables + plist->elements[i];
	  a = s->arch;
	  if (a > pool->lastarch)
	    continue;
	  a = pool->id2arch[a];
	  /* a == 1 -> noarch */
	  if (a != 1 && ((a ^ bestscore) & 0xffff0000) != 0)
	    continue;
	  plist->elements[j++] = plist->elements[i];
	}
      if (j)
        plist->count = j;
    }

  prune_best_version_arch_sortcmp_data = pool;
  /* sort by name first */
  qsort(plist->elements, plist->count, sizeof(Id), prune_best_version_arch_sortcmp);

  /* delete obsoleted. hmm, looks expensive! */
  /* FIXME maybe also check provides depending on noupdateprovide? */
  /* FIXME do not prune cycles */
  for (i = 0; i < plist->count; i++)
    {
      Id p, *pp, obs, *obsp;
      s = pool->solvables + plist->elements[i];
      if (!s->obsoletes)
	continue;
      obsp = s->repo->idarraydata + s->obsoletes;
      while ((obs = *obsp++) != 0)
	{
	  FOR_PROVIDES(p, pp, obs)
	    {
	      if (pool->solvables[p].name == s->name)
		continue;
	      for (j = 0; j < plist->count; j++)
		{
		  if (i == j)
		    continue;
		  if (plist->elements[j] == p)
		    plist->elements[j] = 0;
		}
	    }
	}
    }
  for (i = j = 0; i < plist->count; i++)
    if (plist->elements[i])
      plist->elements[j++] = plist->elements[i];
  plist->count = j;

  /* now find best 'per name' */
  for (i = j = 0; i < plist->count; i++)
    {
      s = pool->solvables + plist->elements[i];

      if (pool->verbose > 1) printf("- %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));

      if (!best)		       /* if no best yet, the current is best */
        {
          best = plist->elements[i];
          continue;
        }

      /* name switch: re-init */
      if (pool->solvables[best].name != s->name)   /* new name */
        {
          plist->elements[j++] = best; /* move old best to front */
          best = plist->elements[i];   /* take current as new best */
          continue;
        }

      if (pool->solvables[best].evr != s->evr)   /* compare evr */
        {
          if (evrcmp(pool, pool->solvables[best].evr, s->evr) < 0)
            best = plist->elements[i];
        }
    }

  if (best == ID_NULL)
    best = plist->elements[0];

  plist->elements[j++] = best;
  plist->count = j;

}

/*-----------------------------------------------------------------*/

/*
 * print rules
 */

static void
printruleelement(Solver *solv, Rule *r, Id v)
{
  Pool *pool = solv->pool;
  Solvable *s;
  if (v < 0)
    {
      s = pool->solvables + -v;
      printf("    !%s-%s.%s [%d]", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), -v);
    }
  else
    {
      s = pool->solvables + v;
      printf("    %s-%s.%s [%d]", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), v);
    }
  if (r)
    {
      if (r->w1 == v)
	printf(" (w1)");
      if (r->w2 == v)
	printf(" (w2)");
    }
  if (solv->decisionmap[s - pool->solvables] > 0)
    printf(" I.%d", solv->decisionmap[s - pool->solvables]);
  if (solv->decisionmap[s - pool->solvables] < 0)
    printf(" C.%d", -solv->decisionmap[s - pool->solvables]);
  printf("\n");
}


/*
 * print rule
 */

static void
printrule(Solver *solv, Rule *r)
{
  int i;
  Id v;

  if (r >= solv->rules && r < solv->rules + solv->nrules)   /* r is a solver rule */
    printf("Rule #%d:\n", (int)(r - solv->rules));
  else
    printf("Rule:\n");		       /* r is any rule */
  for (i = 0; ; i++)
    {
      if (i == 0)
	v = r->p;
      else if (r->d == ID_NULL)
	{
	  if (i == 2)
	    break;
	  v = r->w2;
	}
      else
	v = solv->pool->whatprovidesdata[r->d + i - 1];
      if (v == ID_NULL)
	break;
      printruleelement(solv, r, v);
    }
  printf("    next: %d %d\n", r->n1, r->n2);
}


/*-----------------------------------------------------------------*/

/*
 * Rule handling
 */

static Pool *unifyrules_sortcmp_data;

/*
 * compare rules for unification sort
 */

static int
unifyrules_sortcmp(const void *ap, const void *bp)
{
  Pool *pool = unifyrules_sortcmp_data;
  Rule *a = (Rule *)ap;
  Rule *b = (Rule *)bp;
  Id *ad, *bd;
  int x;
  
  x = a->p - b->p;
  if (x)
    return x;			       /* p differs */

  /* identical p */
  if (a->d == 0 && b->d == 0)
    return a->w2 - b->w2;	       /* assertion: return w2 diff */

  if (a->d == 0)		       /* a is assertion, b not */
    {
      x = a->w2 - pool->whatprovidesdata[b->d];
      return x ? x : -1;
    }

  if (b->d == 0)		       /* b is assertion, a not */
    {
      x = pool->whatprovidesdata[a->d] - b->w2;
      return x ? x : 1;
    }

  /* compare whatprovidesdata */
  ad = pool->whatprovidesdata + a->d;
  bd = pool->whatprovidesdata + b->d;
  while (*bd)
    if ((x = *ad++ - *bd++) != 0)
      return x;
  return *ad;
}


/*
 * unify rules
 */

static void
unifyrules(Solver *solv)
{
  int i, j;
  Rule *ir, *jr;

  if (solv->nrules <= 1)	       /* nothing to unify */
    return;

  /* sort rules first */
  unifyrules_sortcmp_data = solv->pool;
  qsort(solv->rules + 1, solv->nrules - 1, sizeof(Rule), unifyrules_sortcmp);

  /* prune rules
   * i = unpruned
   * j = pruned
   */
  jr = 0;
  for (i = j = 1, ir = solv->rules + 1; i < solv->nrules; i++, ir++)
    {
      if (jr && !unifyrules_sortcmp(ir, jr))
	continue;		       /* prune! */
      jr = solv->rules + j++;	       /* keep! */
      if (ir != jr)
        *jr = *ir;
    }

  /* reduced count from nrules to j rules */
  if (solv->pool->verbose) printf("pruned rules from %d to %d\n", solv->nrules, j);

  /* adapt rule buffer */
  solv->rules = (Rule *)xrealloc(solv->rules, ((solv->nrules + RULES_BLOCK) & ~RULES_BLOCK) * sizeof(Rule));
  solv->nrules = j;
#if 1
  if (solv->pool->verbose)
    {
      int binr = 0;
      int lits = 0;
      Id *dp;
      Rule *r;

      for (i = 1; i < solv->nrules; i++)
	{
	  r = solv->rules + i;
	  if (r->d == 0)
	    binr++;
	  else
	    {
	      dp = solv->pool->whatprovidesdata + r->d;
	      while (*dp++)
		lits++;
	    }
	}
      printf("  binary: %d\n", binr);
      printf("  normal: %d, %d literals\n", solv->nrules - 1 - binr, lits);
    }
#endif
}

#if 0

/*
 * hash rule
 */

static Hashval
hashrule(Solver *solv, Id p, Id d, int n)
{
  unsigned int x = (unsigned int)p;
  int *dp;

  if (n <= 1)
    return (x * 37) ^ (unsigned int)d; 
  dp = solv->pool->whatprovidesdata + d;
  while (*dp)
    x = (x * 37) ^ (unsigned int)*dp++;
  return x;
}
#endif


/*
 * add rule
 *  p = direct literal; > 0 for learnt, < 0 for installed pkg (rpm)
 *  d, if < 0 direct literal, if > 0 offset into whatprovides, if == 0 rule is assertion (look at p only)
 *
 *
 * A requires b, b provided by B1,B2,B3 => (-A|B1|B2|B3)
 * 
 * p < 0 : rule from rpm (installed pkg)
 * d > 0 : Offset in whatprovidesdata (list of providers)
 * 
 * A conflicts b, b provided by B1,B2,B3 => (-A|-B1), (-A|-B2), (-A|-B3)
 *  d < 0: Id of solvable (e.g. B1)
 * 
 * d == 0: unary rule, assertion => (A) or (-A)
 * 
 *   Install:    p > 0, d = 0   (A)             user requested install
 *   Remove:     p < 0, d = 0   (-A)            user requested remove
 *   Requires:   p < 0, d > 0   (-A|B1|B2|...)  d: <list of providers for requirement of p>
 *   Updates:    p > 0, d > 0   (A|B1|B2|...)   d: <list of updates for solvable p>
 *   Conflicts:  p < 0, d < 0   (-A|-B)         either p (conflict issuer) or d (conflict provider)
 *   ?           p > 0, d < 0   (A|-B)
 *   No-op ?:    p = 0, d = 0   (null)          (used as policy rule placeholder)
 */

static Rule *
addrule(Solver *solv, Id p, Id d)
{
  Rule *r = 0;
  Id *dp = 0;

  int n = 0;			       /* number of literals in rule - 1
					  0 = direct assertion (single literal)
					  1 = binary rule
					*/

  /* it often happenes that requires lead to adding the same rpm rule
   * multiple times, so we prune those duplicates right away to make
   * the work for unifyrules a bit easier */

  if (solv->nrules && !solv->jobrules)
    {
      r = solv->rules + solv->nrules - 1;   /* get the last added rule */
      if (r->p == p && r->d == d && d != 0)   /* identical and not user requested */
	return r;
    }

  if (d < 0)
    {
      if (p == d)
	return 0;		       /* ignore self conflict */
      n = 1;
    }
  else if (d == 0)		       /* user requested */
    n = 0;
  else
    {
      for (dp = solv->pool->whatprovidesdata + d; *dp; dp++, n++)
	if (*dp == -p)
	  return 0;			/* rule is self-fulfilling */
      if (n == 1)
	d = dp[-1];
    }

  if (n == 0)			       /* direct assertion */
    {
      if (!solv->jobrules)
	{
	  /* this is a rpm rule assertion, we do not have to allocate it */
	  /* it can be identified by a level of 1 and a zero reason */
	  /* we must not drop those rules from the decisionq when rewinding! */
	  if (p > 0)
	    abort();
	  if (solv->decisionmap[-p] > 0 || solv->decisionmap[-p] < -1)
	    abort();
	  if (solv->decisionmap[-p])
	    return NULL;
	  queue_push(&solv->decisionq, p);
	  queue_push(&solv->decisionq_why, 0);
	  solv->decisionmap[-p] = -1;
	  return 0;
	}
    }
  else if (n == 1 && p > d)
    {
      /* smallest literal first so we can find dups */
      n = p;
      p = d;
      d = n;
      n = 1;			       /* re-set n, was used as temp var */
    }

  /* check if the last added rule is exactly the same as what we're looking for. */
  if (r && n == 1 && !r->d && r->p == p && r->w2 == d)
    return r;

  if (r && n > 1 && r->d && r->p == p)
    {
      Id *dp2;
      if (d == r->d)
	return r;
      dp2 = solv->pool->whatprovidesdata + r->d;
      for (dp = solv->pool->whatprovidesdata + d; *dp; dp++, dp2++)
	if (*dp != *dp2)
	  break;
      if (*dp == *dp2)
	return r;
   }
  
  /*
   * allocate new rule
   */

  /* check and extend rule buffer */
  if ((solv->nrules & RULES_BLOCK) == 0)
    {
      solv->rules = (Rule *)xrealloc(solv->rules, (solv->nrules + (RULES_BLOCK + 1)) * sizeof(Rule));
    }

  r = solv->rules + solv->nrules++;    /* point to rule space */

  r->p = p;
  if (n == 0)
    {
      /* direct assertion, no watch needed */
      r->d = 0;
      r->w1 = p;
      r->w2 = 0;
    }
  else if (n == 1)
    {
      /* binary rule */
      r->d = 0;
      r->w1 = p;
      r->w2 = d;
    }
  else
    {
      r->d = d;
      r->w1 = p;
      r->w2 = solv->pool->whatprovidesdata[d];
    }
  r->n1 = 0;
  r->n2 = 0;
  return r;
}


/* go through system and job rules and add direct assertions
 * to the decisionqueue. If we find a conflict, disable rules and
 * add them to problem queue.
 */
static void
makeruledecisions(Solver *solv)
{
  int i, ri;
  Rule *r, *rr;
  Id v, vv;

  /* no learnt rules for now */
  if (solv->learntrules && solv->learntrules != solv->nrules)
    abort();

  for (ri = solv->jobrules, r = solv->rules + ri; ri < solv->nrules; ri++, r++)
    {
      if (!r->w1 || r->w2)
        continue;
      v = r->p;
      vv = v > 0 ? v : -v;
      if (solv->decisionmap[vv] == 0)
	{
	  queue_push(&solv->decisionq, v);
	  queue_push(&solv->decisionq_why, r - solv->rules);
	  solv->decisionmap[vv] = v > 0 ? 1 : -1;
	  continue;
	}
      if (v > 0 && solv->decisionmap[vv] > 0)
        continue;
      if (v < 0 && solv->decisionmap[vv] < 0)
        continue;
      /* found a conflict! */
      /* if we are weak, just disable ourself */
      if (ri >= solv->weakrules)
	{
	  printf("conflict, but I am weak, disabling ");
	  printrule(solv, r);
	  r->w1 = 0;
	  continue;
	}
      for (i = 0; i < solv->decisionq.count; i++)
	if (solv->decisionq.elements[i] == -v)
	  break;
      if (i == solv->decisionq.count)
	abort();
      if (solv->decisionq_why.elements[i] == 0)
	{
	  /* conflict with rpm rule, need only disable our rule */
	  printf("conflict with rpm rule, disabling rule #%d\n", ri);
	  queue_push(&solv->problems, r - solv->rules);
	  queue_push(&solv->problems, 0);
	  r->w1 = 0;	/* disable */
	  continue;
	}
      /* conflict with another job or system rule */
      /* remove old decision */
      printf("conflicting system/job rules over literal %d\n", vv);
      solv->decisionmap[vv] = 0;
      for (; i + 1 < solv->decisionq.count; i++)
	{
	  solv->decisionq.elements[i] = solv->decisionq.elements[i + 1];
	  solv->decisionq_why.elements[i] = solv->decisionq_why.elements[i + 1];
	}
      solv->decisionq.count--;
      solv->decisionq_why.count--;
      /* push all of our rules asserting this literal on the problem stack */
      for (i = solv->jobrules, rr = solv->rules + i; i < solv->nrules; i++, rr++)
	{
	  if (!rr->w1 || rr->w2)
	    continue;
	  if (rr->p != v && rr->p != -v)
	    continue;
	  printf(" - disabling rule #%d\n", i);
	  queue_push(&solv->problems, i);
	  rr->w1 = 0;	/* disable */
	}
      queue_push(&solv->problems, 0);
    }
}


/*
 * add (install) rules for solvable
 * 
 */

static void
addrulesforsolvable(Solver *solv, Solvable *s, Map *m)
{
  Pool *pool = solv->pool;
  Repo *installed = solv->installed;
  Queue q;
  Id qbuf[64];
  int i;
  int dontfix;
  Id req, *reqp;
  Id con, *conp;
  Id obs, *obsp;
  Id rec, *recp;
  Id sug, *sugp;
  Id p, *pp;
  Id *dp;
  Id n;

  queue_init_buffer(&q, qbuf, sizeof(qbuf)/sizeof(*qbuf));
  queue_push(&q, s - pool->solvables);	/* push solvable Id */

  while (q.count)
    {
      /*
       * n: Id of solvable
       * s: Pointer to solvable
       */
      
      n = queue_shift(&q);
      if (MAPTST(m, n))		       /* continue if already done */
	continue;

      MAPSET(m, n);
      s = pool->solvables + n;	       /* s = Solvable in question */

      dontfix = 0;
      if (installed
	  && !solv->fixsystem
	  && n >= installed->start	       /* is it installed? */
	  && n < installed->start + installed->nsolvables)
      {
	dontfix = 1;		       /* dont care about broken rpm deps */
      }

      /*-----------------------------------------
       * check requires of s
       */
      
      if (s->requires)
	{
	  reqp = s->repo->idarraydata + s->requires;
	  while ((req = *reqp++) != 0)
	    {
	      if (req == SOLVABLE_PREREQMARKER)   /* skip the marker */
		continue;

	      dp = GET_PROVIDESP(req, p);	/* get providers of req */

	      if (*dp == SYSTEMSOLVABLE)	/* always installed */
		continue;

	      if (dontfix)
		{
		  /* the strategy here is to not insist on dependencies
                   * that are already broken. so if we find one provider
                   * that was already installed, we know that the
                   * dependency was not broken before so we enforce it */
		  for (i = 0; dp[i]; i++)	/* for all providers */
		    {
		      if (dp[i] >= installed->start && dp[i] < installed->start + installed->nsolvables)
			break;		/* provider was installed */
		    }
		  if (!dp[i])		/* previously broken dependency */
		    {
		      if (pool->verbose) printf("ignoring broken requires %s of installed package %s-%s.%s\n", dep2str(pool, req), id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
		      continue;
		    }
		}

	      if (!*dp)
		{
		  /* nothing provides req! */
  #if 1
		  if (pool->verbose) printf("package %s-%s.%s [%ld] is not installable (%s)\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), (long int)(s - pool->solvables), dep2str(pool, req));
  #endif
		  addrule(solv, -n, 0); /* mark requestor as uninstallable */
		  if (solv->rc_output)
		    printf(">!> !unflag %s-%s.%s[%s]\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), repo_name(s->repo));
		  continue;
		}

  #if 0
	      printf("addrule %s-%s.%s %s %d %d\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), dep2str(pool, req), -n, dp - pool->whatprovidesdata);
	      for (i = 0; dp[i]; i++)
		printf("  %s-%s.%s\n", id2str(pool, pool->solvables[dp[i]].name), id2str(pool, pool->solvables[dp[i]].evr), id2str(pool, pool->solvables[dp[i]].arch));
  #endif
	      /* add 'requires' dependency */
              /* rule: (-requestor|provider1|provider2|...|providerN) */
	      addrule(solv, -n, dp - pool->whatprovidesdata);

	      /* descend the dependency tree */
	      for (; *dp; dp++)   /* loop through all providers */
		{
		  if (!MAPTST(m, *dp))
		    queue_push(&q, *dp);
		}

	    } /* while, requirements of n */

	} /* if, requirements */

      
      /*-----------------------------------------
       * check conflicts of s
       */
      
      if (s->conflicts)
	{
	  conp = s->repo->idarraydata + s->conflicts;
	  while ((con = *conp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, con)
		{
		   /* dontfix: dont care about conflicts with already installed packs */
		  if (dontfix && p >= installed->start && p < installed->start + installed->nsolvables)
		    continue;
                 /* rule: -n|-p: either solvable _or_ provider of conflict */
		  addrule(solv, -n, -p);
		}
	    }
	}

      /*-----------------------------------------
       * check obsoletes if not installed
       */
      if (!installed || n < installed->start || n >= (installed->start + installed->nsolvables))
	{			       /* not installed */
	  if (s->obsoletes)
	    {
	      obsp = s->repo->idarraydata + s->obsoletes;
	      while ((obs = *obsp++) != 0)
		{
		  FOR_PROVIDES(p, pp, obs)
		    addrule(solv, -n, -p);
		}
	    }
	  FOR_PROVIDES(p, pp, s->name)
	    {
	      if (s->name == pool->solvables[p].name)
		addrule(solv, -n, -p);
	    }
	}

      /*-----------------------------------------
       * add recommends to the rule list
       */
      if (s->recommends)
	{
	  recp = s->repo->idarraydata + s->recommends;
	  while ((rec = *recp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, rec)
		if (!MAPTST(m, p))
		  queue_push(&q, p);
	    }
	}
      if (s->suggests)
	{
	  sugp = s->repo->idarraydata + s->suggests;
	  while ((sug = *sugp++) != 0)
	    {
	      FOR_PROVIDES(p, pp, sug)
		if (!MAPTST(m, p))
		  queue_push(&q, p);
	    }
	}
    }
  queue_free(&q);
}

static void
addrulesforweak(Solver *solv, Map *m)
{
  Pool *pool = solv->pool;
  Solvable *s;
  Id sup, *supp;
  int i, n;

  if (pool->verbose) printf("addrulesforweak... (%d)\n", solv->nrules);
  for (i = n = 1; n < pool->nsolvables; i++, n++)
    {
      if (i == pool->nsolvables)
	i = 1;
      if (MAPTST(m, i))
	continue;
      s = pool->solvables + i;
      if (!pool_installable(pool, s))
	continue;
      sup = 0;
      if (s->supplements)
	{
	  supp = s->repo->idarraydata + s->supplements;
	  while ((sup = *supp++) != ID_NULL)
	    if (dep_possible(solv, sup, m))
	      break;
	}
      if (!sup && s->freshens)
	{
	  supp = s->repo->idarraydata + s->freshens;
	  while ((sup = *supp++) != ID_NULL)
	    if (dep_possible(solv, sup, m))
	      break;
	}
      if (!sup && s->enhances)
	{
	  supp = s->repo->idarraydata + s->enhances;
	  while ((sup = *supp++) != ID_NULL)
	    if (dep_possible(solv, sup, m))
	      break;
	}
      if (!sup)
	continue;
      addrulesforsolvable(solv, s, m);
      n = 0;
    }
  if (pool->verbose) printf("done. (%d)\n", solv->nrules);
}

int
archchanges(Pool *pool, Solvable *s1, Solvable *s2)
{
  Id a1 = s1->arch, a2 = s2->arch;

  /* we allow changes to/from noarch */
  if (a1 == a2 || a1 == ARCH_NOARCH || a2 == ARCH_NOARCH)
    return 0;
  if (!pool->id2arch)
    return 0;
  a1 = a1 <= pool->lastarch ? pool->id2arch[a1] : 0;
  a2 = a2 <= pool->lastarch ? pool->id2arch[a2] : 0;
  if (((a1 ^ a2) & 0xffff0000) != 0)
    return 1;
  return 0;
}

static void
findupdatepackages(Solver *solv, Solvable *s, Queue *qs, Map *m, int allowdowngrade, int allowarchchange, int allowvendorchange)
{
  /* installed packages get a special upgrade allowed rule */
  Pool *pool = solv->pool;
  Id p, *pp, n, p2, *pp2;
  Id obs, *obsp;
  Solvable *ps;
  Id vendormask;

  queue_empty(qs);
  /*
   * s = solvable ptr
   * n = solvable Id
   */
  n = s - pool->solvables;
  vendormask = pool_vendor2mask(pool, s->vendor);

  if (m && !MAPTST(m, n))	/* add rule for s if not already done */
    addrulesforsolvable(solv, s, m);

  /*
   * look for updates for s
   */
  FOR_PROVIDES(p, pp, s->name)	/* every provider of s' name */
    {
      if (p == n)		/* skip itself */
	continue;

      ps = pool->solvables + p;
      if (s->name == ps->name)	/* name match */
	{
	  if (!allowdowngrade			/* consider downgrades ? */
	      && evrcmp(pool, s->evr, ps->evr) > 0)
	    continue;
	  /* XXX */
	  if (!allowarchchange && archchanges(pool, s, ps))
	    continue;
	  if (!allowvendorchange && s->vendor != ps->vendor && vendormask && (vendormask & pool_vendor2mask(pool, ps->vendor)) == 0)
	    continue;
	}
      else if (!solv->noupdateprovide && ps->obsoletes)   /* provides/obsoletes combination ? */
	{
	  obsp = ps->repo->idarraydata + ps->obsoletes;
	  while ((obs = *obsp++) != 0)	/* for all obsoletes */
	    {
	      FOR_PROVIDES(p2, pp2, obs)   /* and all matching providers of the obsoletes */
		{
		  if (p2 == n)		/* match ! */
		    break;
		}
	      if (p2)			/* match! */
		break;
	    }
	  if (!obs)			/* continue if no match */
	    continue;
	  /* here we have 'p' with a matching provides/obsoletes combination
	   * thus flagging p as a valid update candidate for s
	   */
	}
      else
        continue;
      queue_push(qs, p);

      if (m && !MAPTST(m, p))		/* mark p for install if not already done */
	addrulesforsolvable(solv, pool->solvables + p, m);
    }
  if (solv->noupdateprovide && solv->obsoletes && solv->obsoletes[n - solv->installed->start])
    {
      for (pp = solv->obsoletes_data + solv->obsoletes[n - solv->installed->start]; (p = *pp++) != 0;)
	{
	  queue_push(qs, p);
	  if (m && !MAPTST(m, p))		/* mark p for install if not already done */
	    addrulesforsolvable(solv, pool->solvables + p, m);
	}
    }
}

/*
 * add rule for update
 *   (A|A1|A2|A3...)  An = update candidates for A
 * 
 * s = (installed) solvable
 * m = 'addedmap', bit set if 'install' rule for solvable exists
 */

static void
addupdaterule(Solver *solv, Solvable *s, Map *m, int allowdowngrade, int allowarchchange, int allowvendorchange, int dontaddrule)
{
  /* installed packages get a special upgrade allowed rule */
  Pool *pool = solv->pool;
  Id p, d;
  Rule *r;
  Queue qs;
  Id qsbuf[64];

  queue_init_buffer(&qs, qsbuf, sizeof(qsbuf)/sizeof(*qsbuf));
  findupdatepackages(solv, s, &qs, m, allowdowngrade, allowarchchange, allowvendorchange);
  p = s - pool->solvables;
  if (dontaddrule)	/* we consider update candidates but dont force them */
    {
      queue_free(&qs);
      return;
    }

  if (qs.count == 0)		       /* no updates found */
    {
#if 0
      printf("new update rule: must keep %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
#endif
      addrule(solv, p, 0);		/* request 'install' of s */
      queue_free(&qs);
      return;
    }

  d = pool_queuetowhatprovides(pool, &qs);   /* intern computed provider queue */
  queue_free(&qs);
  r = addrule(solv, p, d);	       /* allow update of s */
#if 0
  printf("new update rule ");
  if (r)
    printrule(solv, r);
#endif
}


/*-----------------------------------------------------------------*/
/* watches */


/*
 * makewatches
 * 
 * initial setup for all watches
 */

static void
makewatches(Solver *solv)
{
  Rule *r;
  int i;
  int nsolvables = solv->pool->nsolvables;

  xfree(solv->watches);
				       /* lower half for removals, upper half for installs */
  solv->watches = (Id *)xcalloc(2 * nsolvables, sizeof(Id));
#if 1
  /* do it reverse so rpm rules get triggered first */
  for (i = 1, r = solv->rules + solv->nrules - 1; i < solv->nrules; i++, r--)
#else
  for (i = 1, r = solv->rules + 1; i < solv->nrules; i++, r++)
#endif
    {
      if (!r->w1	               /* rule is disabled */
	  || !r->w2)		       /* rule is assertion */
	continue;

      /* see addwatches(solv, r) */
      r->n1 = solv->watches[nsolvables + r->w1];
      solv->watches[nsolvables + r->w1] = r - solv->rules;

      r->n2 = solv->watches[nsolvables + r->w2];
      solv->watches[nsolvables + r->w2] = r - solv->rules;
    }
}


/*
 * add watches (for rule)
 */

static void
addwatches(Solver *solv, Rule *r)
{
  int nsolvables = solv->pool->nsolvables;

  r->n1 = solv->watches[nsolvables + r->w1];
  solv->watches[nsolvables + r->w1] = r - solv->rules;

  r->n2 = solv->watches[nsolvables + r->w2];
  solv->watches[nsolvables + r->w2] = r - solv->rules;
}


/*-----------------------------------------------------------------*/
/* rule propagation */

#define DECISIONMAP_TRUE(p) ((p) > 0 ? (decisionmap[p] > 0) : (decisionmap[-p] < 0))

/*
 * propagate
 * 
 * propagate decision to all rules
 */

static Rule *
propagate(Solver *solv, int level)
{
  Pool *pool = solv->pool;
  Id *rp, *nrp;
  Rule *r;
  Id p, pkg, ow;
  Id *dp;
  Id *decisionmap = solv->decisionmap;
  Id *watches = solv->watches + pool->nsolvables;

  while (solv->propagate_index < solv->decisionq.count)
    {
      /* negative because our watches trigger if literal goes FALSE */
      pkg = -solv->decisionq.elements[solv->propagate_index++];
#if 0
  printf("popagate for decision %d level %d\n", -pkg, level);
  printruleelement(solv, 0, -pkg);
#endif
      for (rp = watches + pkg; *rp; rp = nrp)
	{
	  r = solv->rules + *rp;
#if 0
  printf("  watch triggered ");
  printrule(solv, r);
#endif
	  if (pkg == r->w1)
	    {
	      ow = r->w2;
	      nrp = &r->n1;
	    }
	  else
	    {
	      ow = r->w1;
	      nrp = &r->n2;
	    }
	  /* if clause is TRUE, nothing to do */
	  if (DECISIONMAP_TRUE(ow))
	    continue;

          if (r->d)
	    {
	      /* not a binary clause, check if we need to move our watch */
	      if (r->p && r->p != ow && !DECISIONMAP_TRUE(-r->p))
		p = r->p;
	      else
		for (dp = pool->whatprovidesdata + r->d; (p = *dp++) != 0;)
		  if (p != ow && !DECISIONMAP_TRUE(-p))
		    break;
	      if (p)
		{
		  /* p is free to watch, move watch to p */
#if 0
		  if (p > 0)
		    printf("    -> move w%d to %s-%s.%s\n", (pkg == r->w1 ? 1 : 2), id2str(pool, pool->solvables[p].name), id2str(pool, pool->solvables[p].evr), id2str(pool, pool->solvables[p].arch));
		  else
		    printf("    -> move w%d to !%s-%s.%s\n", (pkg == r->w1 ? 1 : 2), id2str(pool, pool->solvables[-p].name), id2str(pool, pool->solvables[-p].evr), id2str(pool, pool->solvables[-p].arch));
#endif
		  *rp = *nrp;
		  nrp = rp;
		  if (pkg == r->w1)
		    {
		      r->w1 = p;
		      r->n1 = watches[p];
		    }
		  else
		    {
		      r->w2 = p;
		      r->n2 = watches[p];
		    }
		  watches[p] = r - solv->rules;
		  continue;
		}
	    }
          /* unit clause found, set other watch to TRUE */
	  if (DECISIONMAP_TRUE(-ow))
	    return r;		/* eek, a conflict! */
	  if (pool->verbose > 2)
	    {
	      printf("unit ");
	      printrule(solv, r);
	    }
	  if (ow > 0)
            decisionmap[ow] = level;
	  else
            decisionmap[-ow] = -level;
	  queue_push(&solv->decisionq, ow);
	  queue_push(&solv->decisionq_why, r - solv->rules);
#if 0
	    {
	      Solvable *s = pool->solvables + (ow > 0 ? ow : -ow);
	      if (ow > 0)
		printf("  -> decided to install %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	      else
		printf("  -> decided to conflict %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	    }
#endif
	}
    }
  return 0;	/* all is well */
}


/*-----------------------------------------------------------------*/
/* Analysis */

/*
 * analyze
 *   and learn
 */

static int
analyze(Solver *solv, int level, Rule *c, int *pr, int *dr, int *why)
{
  Pool *pool = solv->pool;
  Queue r;
  int rlevel = 1;
  Map seen;		/* global? */
  Id v, vv, *dp;
  int l, i, idx;
  int num = 0;
  int learnt_why = solv->learnt_pool.count;
  Id *decisionmap = solv->decisionmap;
 
  queue_init(&r);

  if (pool->verbose > 1) printf("ANALYZE at %d ----------------------\n", level);
  map_init(&seen, pool->nsolvables);
  idx = solv->decisionq.count;
  for (;;)
    {
      if (pool->verbose > 1) printrule(solv, c);
      queue_push(&solv->learnt_pool, c - solv->rules);
      dp = c->d ? pool->whatprovidesdata + c->d : 0;
      for (i = -1; ; i++)
	{
	  if (i == -1)
	    v = c->p;
	  else if (c->d == 0)
	    v = i ? 0 : c->w2;
	  else
	    v = *dp++;
	  if (v == 0)
	    break;
	  if (DECISIONMAP_TRUE(v))	/* the one true literal */
	      continue;
	  vv = v > 0 ? v : -v;
	  if (MAPTST(&seen, vv))
	    continue;
	  l = solv->decisionmap[vv];
	  if (l < 0)
	    l = -l;
	  if (l == 1)
	    {
#if 0
	      int j;
	      for (j = 0; j < solv->decisionq.count; j++)
		if (solv->decisionq.elements[j] == v)
		  break;
	      if (j == solv->decisionq.count)
		abort();
	      queue_push(&rulq, -(j + 1));
#endif
	      continue;			/* initial setting */
	    }
	  MAPSET(&seen, vv);
	  if (l == level)
	    num++;			/* need to do this one as well */
	  else
	    {
	      queue_push(&r, v);
#if 0
  printf("PUSH %d ", v);
  printruleelement(solv, 0, v);
#endif
	      if (l > rlevel)
		rlevel = l;
	    }
	}
#if 0
      printf("num = %d\n", num);
#endif
      if (num <= 0)
	abort();
      for (;;)
	{
	  v = solv->decisionq.elements[--idx];
	  vv = v > 0 ? v : -v;
	  if (MAPTST(&seen, vv))
	    break;
	}
      c = solv->rules + solv->decisionq_why.elements[idx];
      MAPCLR(&seen, vv);
      if (--num == 0)
	break;
    }
  *pr = -v;
  if (r.count == 0)
    *dr = 0;
  else if (r.count == 1 && r.elements[0] < 0)
    *dr = r.elements[0];
  else
    *dr = pool_queuetowhatprovides(pool, &r);
  if (pool->verbose > 1)
    {
      printf("learned rule for level %d (am %d)\n", rlevel, level);
      printruleelement(solv, 0, -v);
      for (i = 0; i < r.count; i++)
        {
          v = r.elements[i];
          printruleelement(solv, 0, v);
        }
    }
  map_free(&seen);
  queue_push(&solv->learnt_pool, 0);
#if 0
  for (i = learnt_why; solv->learnt_pool.elements[i]; i++)
    {
      printf("learnt_why ");
      printrule(solv, solv->rules + solv->learnt_pool.elements[i]);
    }
#endif
  if (why)
    *why = learnt_why;
  return rlevel;
}


/*
 * reset_solver
 * reset the solver decisions to right after the rpm rules
 */

static void
reset_solver(Solver *solv)
{
  int i;
  Id v;

  /* delete all learnt rules */
  solv->nrules = solv->learntrules;
  queue_empty(&solv->learnt_why);
  queue_empty(&solv->learnt_pool);

  /* redo all direct rpm rule decisions */
  /* we break at the first decision with a why attached, this is
   * either a job/system rule decision of a propagated decision */
  for (i = 0; i < solv->decisionq.count; i++)
    {
      v = solv->decisionq.elements[i];
      solv->decisionmap[v > 0 ? v : -v] = 0;
    }
  for (i = 0; i < solv->decisionq_why.count; i++)
    if (solv->decisionq_why.elements[i])
      break;
    else
      {
        v = solv->decisionq.elements[i];
        solv->decisionmap[v > 0 ? v : -v] = v > 0 ? 1 : -1;
      }

  if (solv->pool->verbose > 1)
    printf("decisions done reduced from %d to %d\n", solv->decisionq.count, i);

  solv->decisionq_why.count = i;
  solv->decisionq.count = i;
  solv->recommends_index = -1;
  solv->propagate_index = 0;

  /* redo all job/system decisions */
  makeruledecisions(solv);
  if (solv->pool->verbose > 1)
    printf("decisions after adding job and system rules: %d\n", solv->decisionq.count);
  /* recreate watches */
  makewatches(solv);
}


/*
 * analyze_unsolvable_rule
 */

static void
analyze_unsolvable_rule(Solver *solv, Rule *r)
{
  int i;
  Id why = r - solv->rules;
#if 0
  if (why >= solv->jobrules && why < solv->systemrules)
    printf("JOB ");
  if (why >= solv->systemrules && why < solv->weakrules)
    printf("SYSTEM %d ", why - solv->systemrules);
  if (why >= solv->weakrules && why < solv->learntrules)
    printf("WEAK ");
  if (solv->learntrules && why >= solv->learntrules)
    printf("LEARNED ");
  printrule(solv, r);
#endif
  if (solv->learntrules && why >= solv->learntrules)
    {
      for (i = solv->learnt_why.elements[why - solv->learntrules]; solv->learnt_pool.elements[i]; i++)
	analyze_unsolvable_rule(solv, solv->rules + solv->learnt_pool.elements[i]);
      return;
    }
  /* do not add rpm rules to problem */
  if (why < solv->jobrules)
    return;
  /* return if problem already countains the rule */
  if (solv->problems.count)
    {
      for (i = solv->problems.count - 1; i >= 0; i--)
	if (solv->problems.elements[i] == 0)
	  break;
	else if (solv->problems.elements[i] == why)
	  return;
    }
  queue_push(&solv->problems, why);
}


/*
 * analyze_unsolvable
 *
 * return: 1 - disabled some rules, try again
 *         0 - hopeless
 */

static int
analyze_unsolvable(Solver *solv, Rule *r, int disablerules)
{
  Pool *pool = solv->pool;
  Map seen;		/* global? */
  Id v, vv, *dp, why;
  int l, i, idx;
  Id *decisionmap = solv->decisionmap;
  int oldproblemcount;
  int lastweak;

#if 0
  printf("ANALYZE UNSOLVABLE ----------------------\n");
#endif
  oldproblemcount = solv->problems.count;
  map_init(&seen, pool->nsolvables);
  analyze_unsolvable_rule(solv, r);
  dp = r->d ? pool->whatprovidesdata + r->d : 0;
  for (i = -1; ; i++)
    {
      if (i == -1)
	v = r->p;
      else if (r->d == 0)
	v = i ? 0 : r->w2;
      else
	v = *dp++;
      if (v == 0)
	break;
      if (DECISIONMAP_TRUE(v))	/* the one true literal */
	  continue;
      vv = v > 0 ? v : -v;
      l = solv->decisionmap[vv];
      if (l < 0)
	l = -l;
      MAPSET(&seen, vv);
    }
  idx = solv->decisionq.count;
  while (idx > 0)
    {
      v = solv->decisionq.elements[--idx];
      vv = v > 0 ? v : -v;
      if (!MAPTST(&seen, vv))
	continue;
      why = solv->decisionq_why.elements[idx];
      if (!why)
	{
#if 0
	  printf("RPM ");
	  printruleelement(solv, 0, v);
#endif
	  continue;
	}
      r = solv->rules + why;
      analyze_unsolvable_rule(solv, r);
      dp = r->d ? pool->whatprovidesdata + r->d : 0;
      for (i = -1; ; i++)
	{
	  if (i == -1)
	    v = r->p;
	  else if (r->d == 0)
	    v = i ? 0 : r->w2;
	  else
	    v = *dp++;
	  if (v == 0)
	    break;
	  if (DECISIONMAP_TRUE(v))	/* the one true literal */
	      continue;
	  vv = v > 0 ? v : -v;
	  l = solv->decisionmap[vv];
	  if (l < 0)
	    l = -l;
	  MAPSET(&seen, vv);
	}
    }
  map_free(&seen);
  queue_push(&solv->problems, 0);	/* mark end of this problem */

  lastweak = 0;
  if (solv->weakrules != solv->learntrules)
    {
      for (i = oldproblemcount; i < solv->problems.count - 1; i++)
	{
	  why = solv->problems.elements[i];
	  if (why < solv->weakrules || why >= solv->learntrules)
	    continue;
	  if (!lastweak || lastweak < why)
	    lastweak = why;
	}
    }
  if (lastweak)
    {
      /* disable last weak rule */
      solv->problems.count = oldproblemcount;
      r = solv->rules + lastweak;
      printf("disabling weak ");
      printrule(solv, r);
      r->w1 = 0;
      reset_solver(solv);
      return 1;
    }
  else if (disablerules)
    {
      for (i = oldproblemcount; i < solv->problems.count - 1; i++)
	{
	  r = solv->rules + solv->problems.elements[i];
	  r->w1 = 0;
	}
      reset_solver(solv);
      return 1;
    }
  return 0;
}


/*-----------------------------------------------------------------*/
/* Decision revert */

/*
 * revert
 * revert decision at level
 */

static void
revert(Solver *solv, int level)
{
  Id v, vv;
  while (solv->decisionq.count)
    {
      v = solv->decisionq.elements[solv->decisionq.count - 1];
      vv = v > 0 ? v : -v;
      if (solv->decisionmap[vv] <= level && solv->decisionmap[vv] >= -level)
        break;
#if 0
      printf("reverting decision %d at %d\n", v, solv->decisionmap[vv]);
#endif
      solv->decisionmap[vv] = 0;
      solv->decisionq.count--;
      solv->decisionq_why.count--;
      solv->propagate_index = solv->decisionq.count;
    }
  while (solv->branches.count && solv->branches.elements[solv->branches.count - 1] <= -level)
    {
      solv->branches.count--;
      while (solv->branches.count && solv->branches.elements[solv->branches.count - 1] >= 0)
	solv->branches.count--;
    }
  solv->recommends_index = -1;
}


/*
 * watch2onhighest - put watch2 on literal with highest level
 */

static void
watch2onhighest(Solver *solv, Rule *r)
{
  int l, wl = 0;
  Id v, *dp;

  if (!r->d)
    return;	/* binary rule, both watches are set */
  dp = solv->pool->whatprovidesdata + r->d;
  while ((v = *dp++) != 0)
    {
      l = solv->decisionmap[v < 0 ? -v : v];
      if (l < 0)
	l = -l;
      if (l > wl)
	{
	  r->w2 = dp[-1];
	  wl = l;
	}
    }
}


/*
 * setpropagatelearn
 */

static int
setpropagatelearn(Solver *solv, int level, Id decision, int disablerules)
{
  Rule *r;
  Id p, d;
  int l, why;

  if (decision)
    {
      level++;
      if (decision > 0)
        solv->decisionmap[decision] = level;
      else
        solv->decisionmap[-decision] = -level;
      queue_push(&solv->decisionq, decision);
      queue_push(&solv->decisionq_why, 0);
    }
  for (;;)
    {
      r = propagate(solv, level);
      if (!r)
	break;
      if (level == 1)
	return analyze_unsolvable(solv, r, disablerules);
      printf("conflict with rule #%d\n", (int)(r - solv->rules));
      l = analyze(solv, level, r, &p, &d, &why);
      if (l >= level || l <= 0)
	abort();
      printf("reverting decisions (level %d -> %d)\n", level, l);
      level = l;
      revert(solv, level);
      r = addrule(solv, p, d);       /* p requires d */
      if (!r)
	abort();
      if (solv->learnt_why.count != (r - solv->rules) - solv->learntrules)
	{
	  printf("%d %d\n", solv->learnt_why.count, (int)(r - solv->rules) - solv->learntrules);
	  abort();
	}
      queue_push(&solv->learnt_why, why);
      if (d)
	{
	  /* at least 2 literals, needs watches */
	  watch2onhighest(solv, r);
	  addwatches(solv, r);
	}
      solv->decisionmap[p > 0 ? p : -p] = p > 0 ? level : -level;
      queue_push(&solv->decisionq, p);
      queue_push(&solv->decisionq_why, r - solv->rules);
      if (solv->pool->verbose > 1)
	{
	  printf("decision: ");
	  printruleelement(solv, 0, p);
	  printf("new rule: ");
	  printrule(solv, r);
	}
    }
  return level;
}

/*-----------------------------------------------------------------*/
/* Main solver interface */


/*
 * solver_create
 * create solver structure
 *
 * pool: all available solvables
 * installed: installed Solvables
 *
 *
 * Upon solving, rules are created to flag the Solvables
 * of the 'installed' Repo as installed.
 */

Solver *
solver_create(Pool *pool, Repo *installed)
{
  Solver *solv;
  solv = (Solver *)xcalloc(1, sizeof(Solver));
  solv->pool = pool;
  solv->installed = installed;

  queue_init(&solv->ruletojob);
  queue_init(&solv->decisionq);
  queue_init(&solv->decisionq_why);
  queue_init(&solv->problems);
  queue_init(&solv->suggestions);
  queue_init(&solv->learnt_why);
  queue_init(&solv->learnt_pool);
  queue_init(&solv->branches);

  map_init(&solv->recommendsmap, pool->nsolvables);
  map_init(&solv->suggestsmap, pool->nsolvables);
  solv->recommends_index = 0;

  solv->decisionmap = (Id *)xcalloc(pool->nsolvables, sizeof(Id));
  solv->rules = (Rule *)xmalloc((solv->nrules + (RULES_BLOCK + 1)) * sizeof(Rule));
  memset(solv->rules, 0, sizeof(Rule));
  solv->nrules = 1;

  return solv;
}


/*
 * solver_free
 */

void
solver_free(Solver *solv)
{
  queue_free(&solv->ruletojob);
  queue_free(&solv->decisionq);
  queue_free(&solv->decisionq_why);
  queue_free(&solv->learnt_why);
  queue_free(&solv->learnt_pool);
  queue_free(&solv->problems);
  queue_free(&solv->suggestions);
  queue_free(&solv->branches);

  map_free(&solv->recommendsmap);
  map_free(&solv->suggestsmap);
  xfree(solv->decisionmap);
  xfree(solv->rules);
  xfree(solv->watches);
  xfree(solv->weaksystemrules);
  xfree(solv->obsoletes);
  xfree(solv->obsoletes_data);
  xfree(solv);
}


/*-------------------------------------------------------*/

/*
 * run_solver
 * 
 * all rules have been set up, now actually run the solver
 *
 */

static void
run_solver(Solver *solv, int disablerules, int doweak)
{
  Queue dq;
  int systemlevel;
  int level, olevel;
  Rule *r;
  int i, j, n;
  Solvable *s;
  Pool *pool = solv->pool;
  Id p, *dp;

#if 0
  printf("number of rules: %d\n", solv->nrules);
  for (i = 0; i < solv->nrules; i++)
    printrule(solv, solv->rules + i);
#endif

  /* all new rules are learnt after this point */
  solv->learntrules = solv->nrules;
  /* crate watches lists */
  makewatches(solv);

  if (pool->verbose) printf("initial decisions: %d\n", solv->decisionq.count);

  /* start SAT algorithm */
  level = 1;
  systemlevel = level + 1;
  if (pool->verbose) printf("solving...\n");

  queue_init(&dq);
  for (;;)
    {
      /*
       * propagate
       */
      
      if (level == 1)
	{
	  if (pool->verbose) printf("propagating (%d %d)...\n", solv->propagate_index, solv->decisionq.count);
	  if ((r = propagate(solv, level)) != 0)
	    {
	      if (analyze_unsolvable(solv, r, disablerules))
		continue;
	      printf("UNSOLVABLE\n");
	      queue_free(&dq);
	      return;
	    }
	}

      /*
       * installed packages
       */
      
      if (level < systemlevel && solv->installed->nsolvables)
	{
	  if (!solv->updatesystem)
	    {
	      /* try to keep as many packages as possible */
	      if (pool->verbose) printf("installing system packages\n");
	      for (i = solv->installed->start, n = 0; ; i++, n++)
		{
		  if (n == solv->installed->nsolvables)
		    break;
		  if (i == solv->installed->start + solv->installed->nsolvables)
		    i = solv->installed->start;
		  s = pool->solvables + i;
		  if (solv->decisionmap[i] != 0)
		    continue;
#if 0
		  printf("keeping %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
#endif
		  olevel = level;
		  level = setpropagatelearn(solv, level, i, disablerules);
		  if (level == 0)
		    {
		      printf("UNSOLVABLE\n");
		      queue_free(&dq);
		      return;
		    }
		  if (level <= olevel)
		    n = 0;
		}
	    }
	  if (solv->weaksystemrules)
	    {
	      if (pool->verbose) printf("installing weak system packages\n");
	      for (i = solv->installed->start, n = 0; ; i++, n++)
		{
		  if (n == solv->installed->nsolvables)
		    break;
		  if (solv->decisionmap[i] > 0 || (solv->decisionmap[i] < 0 && solv->weaksystemrules[i - solv->installed->start] == 0))
		    continue;
		  queue_empty(&dq);
		  if (solv->weaksystemrules[i - solv->installed->start])
		    {
		      dp = pool->whatprovidesdata + solv->weaksystemrules[i - solv->installed->start];
		      while ((p = *dp++) != 0)
			{
			  if (solv->decisionmap[p] > 0)
			    break;
			  if (solv->decisionmap[p] == 0)
			    queue_push(&dq, p);
			}
		      if (p)
			continue;	/* rule is already true */
		    }
		  if (!dq.count && solv->decisionmap[i] != 0)
		    continue;

		  if (dq.count > 1)
		    prune_to_highest_prio(pool, &dq);
		  if (dq.count > 1)
		    prune_to_recommended(solv, &dq);
		  /* FIXME we push it that late so that it doesn't get
                   * pruned. should do things a bit different instead! */
		  if (solv->decisionmap[i] == 0)
		    queue_push(&dq, i);
		  if (dq.count > 1)
		    prune_best_version_arch(pool, &dq);
#if 0
		  s = pool->solvables + dq.elements[0];
		  printf("weak system installing %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
#endif
		  olevel = level;
		  level = setpropagatelearn(solv, level, dq.elements[0], disablerules);
		  if (level == 0)
		    {
		      printf("UNSOLVABLE\n");
		      queue_free(&dq);
		      return;
		    }
		  if (level <= olevel)
		    {
		      n = 0;
		      break;
		    }
		}
	      if (n != solv->installed->nsolvables)
		continue;
	    }
	  systemlevel = level;
	}

      /*
       * decide
       */
      
      if (pool->verbose) printf("deciding unresolved rules\n");
      for (i = 1, n = 1; ; i++, n++)
	{
	  if (n == solv->nrules)
	    break;
	  if (i == solv->nrules)
	    i = 1;
	  r = solv->rules + i;
	  if (!r->w1)
	    continue;
	  queue_empty(&dq);
	  if (r->d == 0)
	    {
	      /* binary or unary rule */
	      /* need two positive undecided literals */
	      if (r->p < 0 || r->w2 <= 0)
		continue;
	      if (solv->decisionmap[r->p] || solv->decisionmap[r->w2])
		continue;
	      queue_push(&dq, r->p);
	      queue_push(&dq, r->w2);
	    }
	  else
	    {
	      /* make sure that
               * all negative literals are installed
               * no positive literal is installed
	       * i.e. the rule is not fulfilled and we
               * just need to decide on the positive literals
               */
	      if (r->p < 0)
		{
		  if (solv->decisionmap[-r->p] <= 0)
		    continue;
		}
	      else
		{
		  if (solv->decisionmap[r->p] > 0)
		    continue;
		  if (solv->decisionmap[r->p] == 0)
		    queue_push(&dq, r->p);
		}
	      dp = pool->whatprovidesdata + r->d;
	      while ((p = *dp++) != 0)
		{
		  if (p < 0)
		    {
		      if (solv->decisionmap[-p] <= 0)
			break;
		    }
		  else
		    {
		      if (solv->decisionmap[p] > 0)
			break;
		      if (solv->decisionmap[p] == 0)
			queue_push(&dq, p);
		    }
		}
	      if (p)
		continue;
	    }
	  if (dq.count < 2)
	    {
	      /* cannot happen as this means that
               * the rule is unit */
	      printrule(solv, r);
	      abort();
	    }
	  if (pool->verbose > 2)
	    printrule(solv, r);
	  prune_to_highest_prio(pool, &dq);
	  if (dq.count > 1)
	    prune_to_recommended(solv, &dq);
	  if (dq.count > 1)
	    prune_best_version_arch(pool, &dq);
	  j = 0;
          if (dq.count > 1)
	    {
	      /* choose the supplemented one */
	      for (j = 0; j < dq.count; j++)
		if (is_supplemented(solv, pool->solvables + dq.elements[j]))
		  break;
	      if (j == dq.count)
		{
		  for (j = 1; j < dq.count; j++)
		    queue_push(&solv->branches, dq.elements[j]);
		  queue_push(&solv->branches, -level);
		  j = 0;
		}
	    }
	  p = dq.elements[j];
	  s = pool->solvables + p;
#if 0
	  printf("installing %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
#endif
	  olevel = level;
	  level = setpropagatelearn(solv, level, p, disablerules);
	  if (level == 0)
	    {
	      printf("UNSOLVABLE\n");
	      queue_free(&dq);
	      return;
	    }
	  if (level < systemlevel)
	    break;
	  n = 0;
	} /* for(), decide */

      if (n != solv->nrules)	/* continue if level < systemlevel */
	continue;
      
      if (doweak && !solv->problems.count)
	{
	  int qcount;

	  if (pool->verbose) printf("installing recommended packages\n");
	  queue_empty(&dq);
	  for (i = 1; i < pool->nsolvables; i++)
	    {
	      if (solv->decisionmap[i] < 0)
		continue;
	      if (solv->decisionmap[i] > 0)
		{
		  Id *recp, rec, *pp, p;
		  s = pool->solvables + i;
		  /* installed, check for recommends */
		  /* XXX need to special case AND ? */
		  if (s->recommends)
		    {
		      recp = s->repo->idarraydata + s->recommends;
		      while ((rec = *recp++) != 0)
			{
			  qcount = dq.count;
			  FOR_PROVIDES(p, pp, rec)
			    {
			      if (solv->decisionmap[p] > 0)
				{
				  dq.count = qcount;
				  break;
				}
			      else if (solv->decisionmap[p] == 0)
				{
				  queue_pushunique(&dq, p);
				}
			    }
			}
		    }
		}
	      else
		{
		  s = pool->solvables + i;
		  if (!s->supplements && !s->freshens)
		    continue;
		  if (!pool_installable(pool, s))
		    continue;
		  if (is_supplemented(solv, s))
		    queue_pushunique(&dq, i);
		}
	    }
	  if (dq.count)
	    {
	      if (dq.count > 1)
	        prune_to_highest_prio(pool, &dq);
	      if (dq.count > 1)
	        prune_best_version_arch(pool, &dq);
	      p = dq.elements[0];
	      s = pool->solvables + p;
#if 1
	      printf("installing recommended %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
#endif
	      level = setpropagatelearn(solv, level, p, 0);
	      continue;
	    }
	}

     if (solv->solution_callback)
	{
	  solv->solution_callback(solv, solv->solution_callback_data);
	  if (solv->branches.count)
	    {
	      int i = solv->branches.count - 1;
	      int l = -solv->branches.elements[i];
	      for (; i > 0; i--)
		if (solv->branches.elements[i - 1] < 0)
		  break;
	      p = solv->branches.elements[i];
#if 1
	      s = pool->solvables + p;
	      printf("branching with %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
#endif
	      queue_empty(&dq);
	      for (j = i + 1; j < solv->branches.count; j++)
		queue_push(&dq, solv->branches.elements[j]);
	      solv->branches.count = i;
	      level = l;
	      revert(solv, level);
	      if (dq.count > 1)
	        for (j = 0; j < dq.count; j++)
		  queue_push(&solv->branches, dq.elements[j]);
	      olevel = level;
	      level = setpropagatelearn(solv, level, p, disablerules);
	      if (level == 0)
		{
		  printf("UNSOLVABLE\n");
		  queue_free(&dq);
		  return;
		}
	      continue;
	    }
	  /* all branches done, we're finally finished */
	  break;
	}

      /* minimization step */
     if (solv->branches.count)
	{
	  int l = 0, lasti = -1, lastl = -1;
	  p = 0;
	  for (i = solv->branches.count - 1; i >= 0; i--)
	    {
	      p = solv->branches.elements[i];
	      if (p < 0)
		l = -p;
	      else if (p > 0 && solv->decisionmap[p] > l + 1)
		{
		  lasti = i;
		  lastl = l;
		}
	    }
	  if (lasti >= 0)
	    {
	      /* kill old solvable so that we do not loop */
	      p = solv->branches.elements[lasti];
	      solv->branches.elements[lasti] = 0;
	      s = pool->solvables + p;
#if 1
	      printf("minimizing %d -> %d with %s-%s.%s\n", solv->decisionmap[p], l, id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
#endif
	      level = lastl;
	      revert(solv, level);
	      olevel = level;
	      level = setpropagatelearn(solv, level, p, disablerules);
	      if (level == 0)
		{
		  printf("UNSOLVABLE\n");
		  queue_free(&dq);
		  return;
		}
	      continue;
	    }
	}
      break;
    }
  queue_free(&dq);
}

  
/*
 * refine_suggestion
 */
  
void
refine_suggestion(Solver *solv, Id *problem, Id sug, Queue *refined)
{
  Rule *r;
  int i, j, sugseen, sugjob = -1;
  Id v, sugassert;
  Queue disabled;
  int disabledcnt;

  printf("refine_suggestion start\n");

  if (sug >= solv->jobrules && sug < solv->systemrules)
    sugjob = solv->ruletojob.elements[sug - solv->jobrules];

  queue_init(&disabled);
  queue_empty(refined);
  queue_push(refined, sug);

  /* re-enable all rules but rule "sug" of the problem */
  revert(solv, 1);
  reset_solver(solv);

  sugassert = 0;
  sugseen = 0;
  r = solv->rules + sug;
  if (r->w2 == 0)
    sugassert = r->p;

  for (i = 0; problem[i]; i++)
    {
      if (problem[i] == sug)
	{
	  continue;
	  sugseen = 1;
	}
      if (sugjob >= 0 && problem[i] >= solv->jobrules && problem[i] < solv->systemrules && sugjob == solv->ruletojob.elements[problem[i] - solv->jobrules])
	{
	  /* rule belongs to same job */
	  continue;
	}
      r = solv->rules + problem[i];
#if 0
      printf("enable ");
      printrule(solv, r);
#endif
      if (r->w2 == 0)
	{
	  /* direct assertion */
	  if (r->p == sugassert && sugseen)
	    {
	      /* also leave this assertion disabled */
	      continue;
	    }
	  v = r->p > 0 ? r->p : -r->p;
	  if (solv->decisionmap[v])
	    {
	      if ((solv->decisionmap[v] > 0 && r->p < 0) ||
		  (solv->decisionmap[v] < 0 && r->p > 0))
		{
		  printf("direct assertion failure, no solution found!\n");
		  while (--i >= 0)
		    {
		      r = solv->rules + problem[i];
		      r->w1 = 0;
		    }
		  return;
		}
	    }
	}
      if (r->d == 0 || r->w2 != r->p)
	r->w1 = r->p;
      else
	r->w1 = solv->pool->whatprovidesdata[r->d];
    }
  for (;;)
    {
      /* re-enable as many weak rules as possible */
      for (i = solv->weakrules; i < solv->learntrules; i++)
	{
	  r = solv->rules + i;
	  if (r->w1)
	    continue;
	  if (r->d == 0 || r->w2 != r->p)
	    r->w1 = r->p;
	  else
	    r->w1 = solv->pool->whatprovidesdata[r->d];
	}

      queue_empty(&solv->problems);
      revert(solv, 1);		/* XXX move to reset_solver? */
      reset_solver(solv);
      run_solver(solv, 0, 0);
      if (!solv->problems.count)
	{
	  printf("no more problems!\n");
#if 0
	  printdecisions(solv);
#endif
	  break;		/* great, no more problems */
	}
      disabledcnt = disabled.count;
      for (i = 0; i < solv->problems.elements[i]; i++)
	{
	  /* ignore solutions in refined */
          v = solv->problems.elements[i];
	  for (j = 0; problem[j]; j++)
	    if (problem[j] != sug && problem[j] == v)
	      break;
	  if (problem[j])
	    continue;
	  queue_push(&disabled, v);
	  queue_push(&disabled, 0);	/* room for watch */
	}
      if (disabled.count == disabledcnt)
	{
	  /* no solution found, this was an invalid suggestion! */
	  printf("no solution found!\n");
	  refined->count = 0;
	  break;
	}
      if (disabled.count == disabledcnt + 2)
	{
	  /* just one suggestion, add it to refined list */
	  queue_push(refined, disabled.elements[disabledcnt]);
	}
      else
	{
#if 0
	  printf("##############################################   more than one solution found.\n");
	  for (i = 0; i < solv->problems.elements[i]; i++)
	    printrule(solv, solv->rules + solv->problems.elements[i]);
	  printf("##############################################\n");
#endif
	  /* more than one solution, keep all disabled */
	}
      for (i = disabledcnt; i < disabled.count; i += 2)
	{
	  /* disable em */
	  r = solv->rules + disabled.elements[i];
	  disabled.elements[i + 1] = r->w1;
	  r->w1 = 0;
#if 0
	  printf("disable ");
	  printrule(solv, r);
#endif
	}
    }
  /* enable refined rules again */
  for (i = 0; i < disabled.count; i += 2)
    {
      r = solv->rules + disabled.elements[i];
      r->w1 = disabled.elements[i + 1];
    }
  /* disable problem rules again so that we are in the same state as before */
  for (i = 0; problem[i]; i++)
    {
      r = solv->rules + problem[i];
      r->w1 = 0;
    }
  printf("refine_suggestion end\n");
}

  
/*
 * printdecisions
 */
  
static const char *
id2rc(Solver *solv, Id id)
{
  const char *evr;
  if (solv->rc_output != 2)
    return "";
  evr = id2str(solv->pool, id);
  if (*evr < '0' || *evr > '9')
    return "0:";
  while (*evr >= '0' && *evr <= '9')
    evr++;
  if (*evr != ':')
    return "0:";
  return "";
}

void
printdecisions(Solver *solv)
{
  Pool *pool = solv->pool;
  Id p, *obsoletesmap;
  int i;
  Solvable *s;

  obsoletesmap = (Id *)xcalloc(pool->nsolvables, sizeof(Id));
  for (i = 0; i < solv->decisionq.count; i++)
    {
      Id *pp, n;

      n = solv->decisionq.elements[i];
      if (n < 0)
	continue;
      if (n == SYSTEMSOLVABLE)
	continue;
      if (n >= solv->installed->start && n < solv->installed->start + solv->installed->nsolvables)
	continue;
      s = pool->solvables + n;
      FOR_PROVIDES(p, pp, s->name)
	if (s->name == pool->solvables[p].name)
	  {
	    if (p >= solv->installed->start && p < solv->installed->start + solv->installed->nsolvables && !obsoletesmap[p])
	      {
	        obsoletesmap[p] = n;
	        obsoletesmap[n]++;
	      }
	  }
    }
  for (i = 0; i < solv->decisionq.count; i++)
    {
      Id obs, *obsp;
      Id *pp, n;

      n = solv->decisionq.elements[i];
      if (n < 0)
	continue;
      if (n == SYSTEMSOLVABLE)
	continue;
      if (n >= solv->installed->start && n < solv->installed->start + solv->installed->nsolvables)
	continue;
      s = pool->solvables + n;
      if (!s->obsoletes)
	continue;
      obsp = s->repo->idarraydata + s->obsoletes;
      while ((obs = *obsp++) != 0)
	FOR_PROVIDES(p, pp, obs)
	  {
	    if (p >= solv->installed->start && p < solv->installed->start + solv->installed->nsolvables && !obsoletesmap[p])
	      {
		obsoletesmap[p] = n;
		obsoletesmap[n]++;
	      }
	  }
    }

  if (solv->rc_output)
    printf(">!> Solution #1:\n");

  int installs = 0, uninstalls = 0, upgrades = 0;
  
  /* print solvables to be erased */

  for (i = solv->installed->start; i < solv->installed->start + solv->installed->nsolvables; i++)
    {
      if (solv->decisionmap[i] > 0)
	continue;
      if (obsoletesmap[i])
	continue;
      s = pool->solvables + i;
      if (solv->rc_output == 2)
	printf(">!> remove  %s-%s%s\n", id2str(pool, s->name), id2rc(solv, s->evr), id2str(pool, s->evr));
      else if (solv->rc_output)
	printf(">!> remove  %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
      else
	printf("erase   %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
      uninstalls++;
    }

  /* print solvables to be installed */

  for (i = 0; i < solv->decisionq.count; i++)
    {
      int j;
      p = solv->decisionq.elements[i];
      if (p < 0)
	continue;
      if (p == SYSTEMSOLVABLE)
	continue;
      if (p >= solv->installed->start && p < solv->installed->start + solv->installed->nsolvables)
	continue;
      s = pool->solvables + p;

      if (!obsoletesmap[p])
        {
	  if (solv->rc_output)
	    printf(">!> ");
          printf("install %s-%s%s", id2str(pool, s->name), id2rc(solv, s->evr), id2str(pool, s->evr));
	  if (solv->rc_output != 2)
            printf(".%s", id2str(pool, s->arch));
	  installs++;
        }
      else if (!solv->rc_output)
	{
	  printf("update  %s-%s.%s  (obsoletes", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	  for (j = solv->installed->start; j < solv->installed->start + solv->installed->nsolvables; j++)
	    {
	      if (obsoletesmap[j] != p)
		continue;
	      s = pool->solvables + j;
	      printf(" %s-%s.%s", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	    }
	  printf(")");
	  upgrades++;
	}
      else
	{
	  Solvable *f, *fn = 0;
	  for (j = solv->installed->start; j < solv->installed->start + solv->installed->nsolvables; j++)
	    {
	      if (obsoletesmap[j] != p)
		continue;
	      f = pool->solvables + j;
	      if (fn || f->name != s->name)
		{
		  if (solv->rc_output == 2)
		    printf(">!> remove  %s-%s%s\n", id2str(pool, f->name), id2rc(solv, f->evr), id2str(pool, f->evr));
		  else if (solv->rc_output)
		    printf(">!> remove  %s-%s.%s\n", id2str(pool, f->name), id2str(pool, f->evr), id2str(pool, f->arch));
		  uninstalls++;
		}
	      else
		fn = f;
	    }
	  if (!fn)
	    {
	      printf(">!> install %s-%s%s", id2str(pool, s->name), id2rc(solv, s->evr), id2str(pool, s->evr));
	      if (solv->rc_output != 2)
	        printf(".%s", id2str(pool, s->arch));
	      installs++;
	    }
	  else
	    {
	      if (solv->rc_output == 2)
	        printf(">!> upgrade %s-%s => %s-%s%s", id2str(pool, fn->name), id2str(pool, fn->evr), id2str(pool, s->name), id2rc(solv, s->evr), id2str(pool, s->evr));
	      else
	        printf(">!> upgrade %s-%s.%s => %s-%s.%s", id2str(pool, fn->name), id2str(pool, fn->evr), id2str(pool, fn->arch), id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	      upgrades++;
	    }
	}
      if (solv->rc_output)
	{
	  Repo *repo = s->repo;
	  if (repo && strcmp(repo_name(repo), "locales"))
	    printf("[%s]", repo_name(repo));
        }
      printf("\n");
    }

  if (solv->rc_output)
    printf(">!> installs=%d, upgrades=%d, uninstalls=%d\n", installs, upgrades, uninstalls);
  
  xfree(obsoletesmap);
}

static void
create_obsolete_index(Solver *solv)
{
  Pool *pool = solv->pool;
  Solvable *s;
  Repo *installed = solv->installed;
  Id p, *pp, obs, *obsp, *obsoletes, *obsoletes_data;
  int i, n;

  /* create reverse obsoletes map for installed solvables */
  solv->obsoletes = obsoletes = xcalloc(installed->nsolvables, sizeof(Id));
  for (i = 1; i < pool->nsolvables; i++)
    {
      s = pool->solvables + i;
      if (!s->obsoletes)
	continue;
      if (!pool_installable(pool, s))
	continue;
      obsp = s->repo->idarraydata + s->obsoletes;
      while ((obs = *obsp++) != 0)
        FOR_PROVIDES(p, pp, obs)
	  {
	    if (p < installed->start || p >= installed->start + installed->nsolvables)
	      continue;
	    if (pool->solvables[p].name == s->name)
	      continue;
	    obsoletes[p - installed->start]++;
	  }
    }
  n = 0;
  for (i = 0; i < installed->nsolvables; i++)
    if (obsoletes[i])
      {
        n += obsoletes[i] + 1;
        obsoletes[i] = n;
      }
  solv->obsoletes_data = obsoletes_data = xcalloc(n + 1, sizeof(Id));
  if (pool->verbose) printf("obsoletes data: %d entries\n", n + 1);
  for (i = pool->nsolvables - 1; i > 0; i--)
    {
      s = pool->solvables + i;
      if (!s->obsoletes)
	continue;
      if (!pool_installable(pool, s))
	continue;
      obsp = s->repo->idarraydata + s->obsoletes;
      while ((obs = *obsp++) != 0)
        FOR_PROVIDES(p, pp, obs)
	  {
	    if (p < installed->start || p >= installed->start + installed->nsolvables)
	      continue;
	    if (pool->solvables[p].name == s->name)
	      continue;
	    p -= installed->start;
	    if (obsoletes_data[obsoletes[p]] != i)
	      obsoletes_data[--obsoletes[p]] = i;
	  }
    }
}

/*-----------------------------------------------------------------*/
/* main() */

/*
 *
 * solve job queue
 *
 */

void
solve(Solver *solv, Queue *job)
{
  Pool *pool = solv->pool;
  int i;
  Map addedmap;			       /* '1' == have rule for solvable */
  Map noupdaterule;		       /* '1' == don't update (scheduled for removal) */
  Id how, what, p, *pp, d;
  Queue q;
  Rule *r;
  Solvable *s;

  /*
   * create basic rule set of all involved packages
   * as bitmaps
   * 
   */

  map_init(&addedmap, pool->nsolvables);
  map_init(&noupdaterule, pool->nsolvables);

  queue_init(&q);

  /*
   * always install our system solvable
   */
  MAPSET(&addedmap, SYSTEMSOLVABLE);
  queue_push(&solv->decisionq, SYSTEMSOLVABLE);
  queue_push(&solv->decisionq_why, 0);
  solv->decisionmap[SYSTEMSOLVABLE] = 1;

  /*
   * create rules for installed solvables -> keep them installed
   * so called: rpm rules
   * 
   */

  for (i = solv->installed->start; i < solv->installed->start + solv->installed->nsolvables; i++)
    addrulesforsolvable(solv, pool->solvables + i, &addedmap);

  /*
   * create install rules
   * 
   * two passes, as we want to keep the rpm rules distinct from the job rules
   * 
   */

  if (solv->noupdateprovide && solv->installed->nsolvables)
    create_obsolete_index(solv);

  /*
   * solvable rules
   *  process job rules for solvables
   */
  
  for (i = 0; i < job->count; i += 2)
    {
      how = job->elements[i];
      what = job->elements[i + 1];

      switch(how)
	{
	case SOLVER_INSTALL_SOLVABLE:
	  addrulesforsolvable(solv, pool->solvables + what, &addedmap);
	  break;
	case SOLVER_INSTALL_SOLVABLE_NAME:
	case SOLVER_INSTALL_SOLVABLE_PROVIDES:
	  queue_empty(&q);
	  FOR_PROVIDES(p, pp, what)
	    {
				       /* if by name, ensure that the name matches */
	      if (how == SOLVER_INSTALL_SOLVABLE_NAME && pool->solvables[p].name != what)
		continue;
	      addrulesforsolvable(solv, pool->solvables + p, &addedmap);
	    }
	  break;
	case SOLVER_INSTALL_SOLVABLE_UPDATE:
	  /* dont allow downgrade */
	  addupdaterule(solv, pool->solvables + what, &addedmap, 0, 0, 0, 1);
	  break;
	}
    }

  /*
   * if unstalls are disallowed, add update rules for every
   * installed solvables in the hope to circumvent uninstall
   * by upgrading
   * 
   */
  
#if 0
  if (!solv->allowuninstall)
    {
      /* add update rule for every installed package */
      for (i = solv->installed->start; i < solv->installed->start + solv->installed->nsolvables; i++)
        addupdaterule(solv, pool->solvables + i, &addedmap, solv->allowdowngrade, solv->allowarchchange, solv->allowvendorchange, 1);
    }
#else  /* this is just to add the needed rpm rules to our set */
  for (i = solv->installed->start; i < solv->installed->start + solv->installed->nsolvables; i++)
    addupdaterule(solv, pool->solvables + i, &addedmap, 1, 1, 1, 1);
#endif

  addrulesforweak(solv, &addedmap);
#if 1
  if (pool->verbose)
    {
      int possible = 0, installable = 0;
      for (i = 1; i < pool->nsolvables; i++)
	{
	  if (pool_installable(pool, pool->solvables + i))
	    installable++;
	  if (MAPTST(&addedmap, i))
	    possible++;
	}
      printf("%d of %d installable solvables used for solving\n", possible, installable);
    }
#endif

  /*
   * first pass done
   * 
   * unify existing rules before going over all job rules
   * 
   */
  
  unifyrules(solv);	/* remove duplicate rpm rules */

  /*
   * at this point the system is always solvable,
   * as an empty system (remove all packages) is a valid solution
   */
  if (pool->verbose) printf("decisions based on rpms: %d\n", solv->decisionq.count);

  /*
   * now add all job rules
   */
  
  solv->jobrules = solv->nrules;

  for (i = 0; i < job->count; i += 2)
    {
      how = job->elements[i];
      what = job->elements[i + 1];
      switch(how)
	{
	case SOLVER_INSTALL_SOLVABLE:			/* install specific solvable */
	  s = pool->solvables + what;
	  if (solv->rc_output)
            {
	      printf(">!> Installing %s from channel %s\n", id2str(pool, s->name), repo_name(s->repo));
	    }
	  if (pool->verbose)
	    printf("job: install solvable %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
          addrule(solv, what, 0);			/* install by Id */
	  queue_push(&solv->ruletojob, i);
	  FOR_PROVIDES(p, pp, s->name)
	    if (pool->solvables[p].name == s->name)
	      MAPSET(&noupdaterule, p);
	  break;
	case SOLVER_ERASE_SOLVABLE:
	  s = pool->solvables + what;
	  if (pool->verbose)
	    printf("job: erase solvable %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
          addrule(solv, -what, 0);			/* remove by Id */
	  queue_push(&solv->ruletojob, i);
	  MAPSET(&noupdaterule, what);
	  break;
	case SOLVER_INSTALL_SOLVABLE_NAME:		/* install by capability */
	case SOLVER_INSTALL_SOLVABLE_PROVIDES:
	  if (pool->verbose && how == SOLVER_INSTALL_SOLVABLE_NAME)
	    printf("job: install name %s\n", id2str(pool, what));
	  if (pool->verbose && how == SOLVER_INSTALL_SOLVABLE_PROVIDES)
	    printf("job: install provides %s\n", dep2str(pool, what));
	  queue_empty(&q);
	  FOR_PROVIDES(p, pp, what)
	    {
              /* if by name, ensure that the name matches */
	      if (how == SOLVER_INSTALL_SOLVABLE_NAME && pool->solvables[p].name != what)
		continue;
	      queue_push(&q, p);
	    }
	  if (!q.count)	
	    {
	      /* no provider, make this an impossible rule */
	      queue_push(&q, -SYSTEMSOLVABLE);
	    }

	  p = queue_shift(&q);	       /* get first provider */
	  if (!q.count)
	    d = 0;		       /* single provider ? -> make assertion */
	  else
	    d = pool_queuetowhatprovides(pool, &q);   /* get all providers */
	  addrule(solv, p, d);	       /* add 'requires' rule */
	  queue_push(&solv->ruletojob, i);
	  break;
	case SOLVER_ERASE_SOLVABLE_NAME:                  /* remove by capability */
	case SOLVER_ERASE_SOLVABLE_PROVIDES:
	  if (pool->verbose && how == SOLVER_ERASE_SOLVABLE_NAME)
	    printf("job: erase name %s\n", id2str(pool, what));
	  if (pool->verbose && how == SOLVER_ERASE_SOLVABLE_PROVIDES)
	    printf("job: erase provides %s\n", dep2str(pool, what));
	  FOR_PROVIDES(p, pp, what)
	    {
				       /* if by name, ensure that the name matches */
	      if (how == SOLVER_ERASE_SOLVABLE_NAME && pool->solvables[p].name != what)
	        continue;

	      addrule(solv, -p, 0);  /* add 'remove' rule */
	      queue_push(&solv->ruletojob, i);
	      MAPSET(&noupdaterule, p);
	    }
	  break;
	case SOLVER_INSTALL_SOLVABLE_UPDATE:              /* find update for solvable */
	  s = pool->solvables + what;
	  MAPSET(&noupdaterule, what);
	  if (pool->verbose)
	    printf("job: update %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	  addupdaterule(solv, s, &addedmap, 0, 0, 0, 0);
	  queue_push(&solv->ruletojob, i);
	  break;
	}
    }

  if (solv->ruletojob.count != solv->nrules - solv->jobrules)
    abort();

  if (pool->verbose) printf("problems so far: %d\n", solv->problems.count);
  
  /*
   * now add policy rules
   * 
   */
  
  solv->systemrules = solv->nrules;

  /*
   * create rules for updating installed solvables
   * 
   * (Again ?)
   * 
   */
  
  if (!solv->allowuninstall)
    {				       /* loop over all installed solvables */
      for (i = solv->installed->start; i < solv->installed->start + solv->installed->nsolvables; i++)
      {
	if (!MAPTST(&noupdaterule, i)) /* if not marked as 'noupdate' */
	  addupdaterule(solv, pool->solvables + i, &addedmap, solv->allowdowngrade, solv->allowarchchange, solv->allowvendorchange, 0);
        else
	  addrule(solv, 0, 0);		/* place holder */
      }
      /* consistency check: we added a rule for _every_ system solvable */
      if (solv->nrules - solv->systemrules != solv->installed->nsolvables)
	abort();
    }

  if (pool->verbose) printf("problems so far: %d\n", solv->problems.count);

  /* create special weak system rules */
  if (solv->installed->nsolvables)
    {
      solv->weaksystemrules = xcalloc(solv->installed->nsolvables, sizeof(Id));
      for (i = 0; i < solv->installed->nsolvables; i++)
	{
	  if (MAPTST(&noupdaterule, solv->installed->start + i))
	    continue;
	  findupdatepackages(solv, pool->solvables + solv->installed->start + i, &q, (Map *)0, 1, 1, 1);
	  if (q.count)
	    solv->weaksystemrules[i] = pool_queuetowhatprovides(pool, &q);
	}
    }

  /* free unneeded memory */
  map_free(&addedmap);
  map_free(&noupdaterule);
  queue_free(&q);

  solv->weakrules = solv->nrules;

  /* try real hard to keep packages installed */
  if (0)
    {
      for (i = 0; i < solv->installed->nsolvables; i++)
	{
	  d = solv->weaksystemrules[i];
	  addrule(solv, solv->installed->start + i, d);
	}
    }

  /*
   * solve !
   * 
   */
  
  makeruledecisions(solv);
  run_solver(solv, 1, 1);

  /* find suggested packages */
  if (!solv->problems.count)
    {
      Id sug, *sugp, enh, *enhp, p, *pp;

      /* create map of all suggests that are still open */
      solv->recommends_index = -1;
      MAPZERO(&solv->suggestsmap);
      for (i = 0; i < solv->decisionq.count; i++)
	{
	  p = solv->decisionq.elements[i];
	  if (p < 0)
	    continue;
	  s = pool->solvables + p;
	  if (s->suggests)
	    {
	      sugp = s->repo->idarraydata + s->suggests;
	      while ((sug = *sugp++) != 0)
		{
		  FOR_PROVIDES(p, pp, sug)
		    if (solv->decisionmap[p] > 0)
		      break;
		  if (p)
		    continue;	/* already fulfilled */
		  FOR_PROVIDES(p, pp, sug)
		    MAPSET(&solv->suggestsmap, p);
		}
	    }
	}
      for (i = 1; i < pool->nsolvables; i++)
	{
	  if (solv->decisionmap[i] != 0)
	    continue;
	  s = pool->solvables + i;
	  if (!MAPTST(&solv->suggestsmap, i))
	    {
	      if (!s->enhances)
		continue;
	      if (!pool_installable(pool, s))
		continue;
	      enhp = s->repo->idarraydata + s->enhances;
	      while ((enh = *enhp++) != 0)
		if (dep_fulfilled(solv, enh))
		  break;
	      if (!enh)
		continue;
	    }
	  queue_push(&solv->suggestions, i);
	}
      prune_best_version_arch(pool, &solv->suggestions);
    }

  /*
   *
   * print solver result
   * 
   */

  if (pool->verbose) printf("-------------------------------------------------------------\n");

  if (solv->problems.count)
    {
      Queue problems;
      Queue solution;
      Id *problem;
      Id why, what;
      int j, ji, pcnt;

      if (!pool->verbose)
	return;
      queue_clone(&problems, &solv->problems);
      queue_init(&solution);
      printf("Encountered problems! Here are the solutions:\n");
      problem = problems.elements;
      pcnt = 1;
      printf("\n");
      printf("Problem %d:\n", pcnt);
      printf("====================================\n");
      for (i = 0; i < problems.count; i++)
	{
	  Id v = problems.elements[i];
	  if (v == 0)
	    {
	      printf("\n");
	      if (i + 1 == problems.count)
		break;
	      printf("Problem %d:\n", ++pcnt);
	      printf("====================================\n");
	      problem = problems.elements + i + 1;
	      continue;
	    }
	  if (v >= solv->jobrules && v < solv->systemrules)
	    {
	      ji = solv->ruletojob.elements[v - solv->jobrules];
	      for (j = 0; ; j++)
		{
		  if (problem[j] >= solv->jobrules && problem[j] < solv->systemrules && ji == solv->ruletojob.elements[problem[j] - solv->jobrules])
		    break;
		}
	      if (problem + j < problems.elements + i)
		continue;
	    }
	  refine_suggestion(solv, problem, v, &solution);
	  for (j = 0; j < solution.count; j++)
	    {
	      r = solv->rules + solution.elements[j];
	      why = solution.elements[j];
#if 0
	      printrule(solv, r);
#endif
	      if (why >= solv->jobrules && why < solv->systemrules)
		{
		  ji = solv->ruletojob.elements[why - solv->jobrules];
		  what = job->elements[ji + 1];
		  switch (job->elements[ji])
		    {
		    case SOLVER_INSTALL_SOLVABLE:
		      s = pool->solvables + what;
		      if (what >= solv->installed->start && what < solv->installed->start + solv->installed->nsolvables)
		        printf("- do not keep %s-%s.%s installed\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
		      else
		        printf("- do not install %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
		      break;
		    case SOLVER_ERASE_SOLVABLE:
		      s = pool->solvables + what;
		      if (what >= solv->installed->start && what < solv->installed->start + solv->installed->nsolvables)
		        printf("- do not deinstall %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
		      else
		        printf("- do not forbid installation of %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
		      break;
		    case SOLVER_INSTALL_SOLVABLE_NAME:
		      printf("- do not install %s\n", id2str(pool, what));
		      break;
		    case SOLVER_ERASE_SOLVABLE_NAME:
		      printf("- do not deinstall %s\n", id2str(pool, what));
		      break;
		    case SOLVER_INSTALL_SOLVABLE_PROVIDES:
		      printf("- do not install a solvable providing %s\n", dep2str(pool, what));
		      break;
		    case SOLVER_ERASE_SOLVABLE_PROVIDES:
		      printf("- do not deinstall all solvables providing %s\n", dep2str(pool, what));
		      break;
		    case SOLVER_INSTALL_SOLVABLE_UPDATE:
		      s = pool->solvables + what;
		      printf("- do not install most recent version of %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
		      break;
		    default:
		      printf("- do something different\n");
		      break;
		    }
		}
	      else if (why >= solv->systemrules && why < solv->weakrules)
		{
		  Solvable *sd = 0;
		  s = pool->solvables + solv->installed->start + (why - solv->systemrules);
		  if (solv->weaksystemrules && solv->weaksystemrules[why - solv->systemrules])
		    {
		      Id *dp = pool->whatprovidesdata + solv->weaksystemrules[why - solv->systemrules];
		      for (; *dp; dp++)
			{
			  if (*dp >= solv->installed->start && *dp < solv->installed->start + solv->installed->nsolvables)
			    continue;
			  if (solv->decisionmap[*dp] > 0)
			    {
			      sd = pool->solvables + *dp;
			      break;
			    }
			}
		    }
		  if (sd)
		    {
		      int gotone = 0;
		      if (evrcmp(pool, sd->evr, s->evr) < 0)
			{
		          printf("- allow downgrade of %s-%s.%s to %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), id2str(pool, sd->name), id2str(pool, sd->evr), id2str(pool, sd->arch));
			  gotone = 1;
			}
		      if (!solv->allowarchchange && s->name == sd->name && archchanges(pool, sd, s))
			{
		          printf("- allow architecture change of %s-%s.%s to %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), id2str(pool, sd->name), id2str(pool, sd->evr), id2str(pool, sd->arch));
			  gotone = 1;
			}
		      if (!solv->allowvendorchange && s->name == sd->name && s->vendor != sd->vendor && pool_vendor2mask(pool, s->vendor) && (pool_vendor2mask(pool, s->vendor) & pool_vendor2mask(pool, sd->vendor)) == 0)
			{
			  if (sd->vendor)
		            printf("- allow vendor change from '%s' (%s-%s.%s) to '%s' (%s-%s.%s)\n", id2str(pool, s->vendor), id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), id2str(pool, sd->vendor), id2str(pool, sd->name), id2str(pool, sd->evr), id2str(pool, sd->arch));
			  else
		            printf("- allow vendor change from '%s' (%s-%s.%s) to no vendor (%s-%s.%s)\n", id2str(pool, s->vendor), id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), id2str(pool, sd->name), id2str(pool, sd->evr), id2str(pool, sd->arch));
			  gotone = 1;
			}
		      if (!gotone)
		        printf("- allow replacement of %s-%s.%s with %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), id2str(pool, sd->name), id2str(pool, sd->evr), id2str(pool, sd->arch));
		    }
		  else
		    {
		      printf("- allow deinstallation of %s-%s.%s [%ld]\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch), (long int)(s - pool->solvables));
		    }
		}
	      else
		{
		  abort();
		}
	    }
	  printf("------------------------------------\n");
	}
      queue_free(&solution);
      queue_free(&problems);
      return;
    }

  printdecisions(solv);
  if (solv->suggestions.count)
    {
      printf("\nsuggested packages:\n");
      for (i = 0; i < solv->suggestions.count; i++)
	{
	  s = pool->solvables + solv->suggestions.elements[i];
	  printf("- %s-%s.%s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
	}
    }
}


// EOF
