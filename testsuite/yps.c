/*
 * yps - without gimmick
 * 
 * command line interface to solver
 * Usage:
 *   yps <system> <source> [ ... <source>] <name>
 *     to install a package <name>
 * 
 *   yps -e <system> <name>
 *     to erase a package <name>
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "pool.h"
#include "poolarch.h"
#include "source_solv.h"
#include "solver.h"

// find solvable by name
//   If source != NULL, find there (installed packages)
//   else find in pool (available packages)
//

static Solvable *
select_solvable(Pool *pool, Source *source, char *name)
{
  Id id;
  Queue plist;
  int i, end;
  Solvable *s;

  id = str2id(pool, name, 1);
  queueinit( &plist);
  i = source ? source->start : 1;
  end = source ? source->start + source->nsolvables : pool->nsolvables;
  for (; i < end; i++)
    {
      s = pool->solvables + i;
      if (s->arch == ARCH_SRC || s->arch == ARCH_NOSRC)
	continue;
      if (s->name == id)
	queuepush(&plist, i);
    }

  prune_best_version_arch(pool, &plist);

  if (plist.count == 0)
    {
      printf("unknown package '%s'\n", name);
      exit(1);
    }

  id = plist.elements[0];
  queuefree(&plist);

  return pool->solvables + id;
}


//-----------------------------------------------

int
main(int argc, char **argv)
{
  Pool *pool;		// available packages (multiple repos)
  FILE *fp;
  Source *system;	// installed packages (single repo, aka 'Source')
  Solvable *xs;
  Solver *solv;
  Source *channel;
  Queue job;
  Id id;
  int erase = 0;

  pool = pool_create();
  pool_setarch(pool, "i686");
  pool->verbose = 1;
  queueinit(&job);

  if (argc < 3)
    {
      fprintf(stderr, "Usage:\n  yps <system> <source> [ ... <source>] <name>\n");
      fprintf(stderr, "    to install a package <name>\n");
      fprintf(stderr, "\n  yps -e <system> <name>\n");
      fprintf(stderr, "    to erase a package <name>\n");
      exit(0);
    }

  // '-e' ?

  if (argc > 1 && !strcmp(argv[1], "-e"))
    {
      erase = 1;
      argc--;
      argv++;
    }

  // Load system file (installed packages)

  if ((fp = fopen(argv[1], "r")) == NULL)
    {
      perror(argv[1]);
      exit(1);
    }
  system = pool_addsource_solv(pool, fp, "system");
  channel = 0;
  fclose(fp);

  // Load further repo files (available packages)

  argc--;
  argv++;
  while (argc > 2)		       /* all but last arg are sources */
    {
      if ((fp = fopen(argv[1], "r")) == 0)
	{
	  perror(argv[1]);
	  exit(1);
	}
      channel = pool_addsource_solv(pool, fp, argv[1]);
      fclose(fp);
      argv++;
      argc--;
    }

  // setup job queue
  if (!argv[1][0])
    ;
  else if (!erase)
    {
      xs = select_solvable(pool, channel, argv[1]);
      queuepush(&job, SOLVER_INSTALL_SOLVABLE);
      queuepush(&job, xs - pool->solvables);
    }
  else
    {
      id = str2id(pool, argv[1], 1);
      queuepush(&job, SOLVER_ERASE_SOLVABLE_NAME);
      queuepush(&job, id);
    }

  pool_prepare(pool);

  pool->promoteepoch = 1;

  // start solving

  solv = solver_create(pool, system);

  solv->fixsystem = 0;
  solv->updatesystem = 0;
  solv->allowdowngrade = 0;
  solv->allowuninstall = 0;

  // Solve !

  solve(solv, &job);

  // clean up

  queuefree(&job);
  solver_free(solv);
  pool_free(pool);

  exit(0);
}

// EOF
