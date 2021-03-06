/*
 Document-module: Satsolver
 =About Satsolver

 Satsolver is the module namespace for sat-solver bindings.

 sat-solver provides a repository data cache and a dependency solver
 for rpm-style dependencies based on a Satisfyability engine.

 See http://en.opensuse.org/Package_Management/Sat_Solver for details
 about the internals of sat-solver.

 Solving needs a Request containing a set of Jobs. Jobs install,
 update, remove, or lock Solvables (packages), names (a Solvable
 providing name), or Relations (+name+ +op+ +version.release+).

 Successful solving creates a Transaction, listing the Solvables to
 install, update, or remove in order to fulfill the Request while
 keeping the installed system consistent.

 Solver errors are reported as Problems. Each Problem has a
 description of what went wrong and a set of Solutions how to
 remediate the Problem.

 ==Working with sat-solver bindings
 
 The sat-solver bindings provide two main functionalities
 - An efficient cache of repository data
 - An ultra-fast dependency solver working on the cached data
 
 The core of the repository cache is represented by the _Pool_. It
 represents the context the solver works in. The Pool holds
 _Solvables_, representing (RPM-based) packages.
 
 Solvables have a
 name, a version and an architecture. Solvables usually have
 _Dependencies_, organized as sets of _Relation_s Solvables can also
 hold additional attribute data, typically everything from the RPM
 header, i.e. _vendor_, _download_ _size_, _install_ _size_, etc.

 Solvables within the Pool are grouped in Repositories. Filling the
 Pool by loading a .+solv+ file, representing a _Repository_, is the
 preferred way.
 
 In a nutshell:
 Pool _has_ _lots_ _of_ Repositories _have_ _lots_ _of_ Solvables
 _have_ _lots_ _of_ Attributes.
 
*/


%module satsolver
%feature("autodoc","1");

#if defined(SWIGRUBY)
%include <ruby.swg>
#endif

#define __type

%{

/*=============================================================*/
/* HELPER CODE                                                 */
/*=============================================================*/

#if defined(SWIGPYTHON)
#define Swig_Null_p(x) (x == Py_None)
#define Swig_True Py_True
#define Swig_False Py_False
#define Swig_Null Py_None
#define Swig_Type PyObject*
#define Swig_Int(x) PyInt_FromLong(x)
#define Swig_String(x) PyString_FromString(x)
#define Swig_Array() PyList_New(0)
#define Swig_Append(x,y) PyList_Append(x,y)

/* And here goes 'Python is object oriented' down the drain ... */
#define Swig_Type_Type PyTypeObject *
#define Swig_Type_Null &PyBaseObject_Type
#define Swig_Type_Bool &PyBool_Type
#define Swig_Type_Int &PyInt_Type
#define Swig_Type_Long &PyLong_Type
#define Swig_Type_Float &PyFloat_Type
#define Swig_Type_String &PyString_Type
#define Swig_Type_Array &PyList_Type
#define Swig_Type_Number &PyLong_Type
#define Swig_Type_Directory &PyList_Type


#endif

#if defined(SWIGRUBY)
#define Swig_Null_p(x) NIL_P(x)
#define Swig_True Qtrue
#define Swig_False Qfalse
#define Swig_Null Qnil
#define Swig_Type VALUE
#define Swig_Int(x) INT2FIX(x)
#define Swig_String(x) rb_str_new2(x)
#define Swig_Array() rb_ary_new()
#define Swig_Append(x,y) rb_ary_push(x,y)
#define Swig_Type_Type VALUE
#define Swig_Type_Null Qnil
#define Swig_Type_Bool rb_cTrueClass
#define Swig_Type_Int rb_cInteger
#define Swig_Type_Long rb_cInteger
#define Swig_Type_Float rb_cFloat
#define Swig_Type_String rb_cString
#define Swig_Type_Array rb_cArray
#define Swig_Type_Number rb_cNumeric
#define Swig_Type_Directory rb_cDir
#include <ruby.h>
#include <rubyio.h>
#endif

#if defined(SWIGPERL)
SWIGINTERNINLINE SV *SWIG_From_long  SWIG_PERL_DECL_ARGS_1(long value);
SWIGINTERNINLINE SV *SWIG_FromCharPtr(const char *cptr);

#define Swig_Null_p(x) (x == NULL)
#define Swig_True (&PL_sv_yes)
#define Swig_False (&PL_sv_no)
#define Swig_Null NULL
#define Swig_Type SV *
#define Swig_Int(x) SWIG_From_long(x) /* should be SWIG_From_long(x), but Swig declares it too late. FIXME */
#define Swig_String(x) SWIG_FromCharPtr(x) /* SWIG_FromCharPtr(x), also */
#define Swig_Array(x) (SV *)newAV()
#define Swig_Append(x,y) av_push((AV *)x, y)
/* FIXME: perl types */
#define Swig_Type_Type SV *
#define Swig_Type_Null NULL
#define Swig_Type_Bool NULL
#define Swig_Type_Int NULL
#define Swig_Type_Long NULL
#define Swig_Type_Float NULL
#define Swig_Type_String NULL
#define Swig_Type_Array NULL
#define Swig_Type_Number NULL
#define Swig_Type_Directory NULL
#endif

/* satsolver core includes */
#include "policy.h"
#include "bitmap.h"
#include "evr.h"
#include "hash.h"
#include "poolarch.h"
#include "pool.h"
#include "poolid.h"
#include "poolid_private.h"
#include "pooltypes.h"
#include "queue.h"
#include "solvable.h"
#include "solver.h"
#include "repo.h"
#include "repo_solv.h"
#include "repo_rpmdb.h"
#include "transaction.h"

/* satsolver application layer includes */
#include "applayer.h"
#include "xsolvable.h"
#include "xrepokey.h"
#include "relation.h"
#include "dependency.h"
#include "job.h"
#include "request.h"
#include "decision.h"
#include "problem.h"
#include "solution.h"
#include "covenant.h"
#include "ruleinfo.h"
#include "step.h"


#if defined(SWIGRUBY)
/*
 * iterating over (x)solvables ('yield' in Ruby)
 * (used by Pool, Repo and Solver)
 */

static int
generic_xsolvables_iterate_callback( const XSolvable *xs, void *user_data )
{
  /* FIXME: how to pass 'break' back to the caller ? */
  rb_yield( SWIG_NewPointerObj((void*)xs, SWIGTYPE_p__Solvable, 0) );
  return 0;
}
#endif

/*
 * create Dataiterator
 * (used by dataiterator.i and pool.search)
 */

static Dataiterator *
swig_dataiterator_new(Pool *pool, Repo *repo, const char *match, int option, XSolvable *xs, const char *keyname)
{
    Dataiterator *di = calloc(1, sizeof( Dataiterator ));
    Solvable *s = 0;
    /* cope with pool or repo being NULL */
    if (!pool) {
      if (!repo) {
        /* raise exception (FIXME) */
      }
      pool = repo->pool;
    }
    if (xs) s = xsolvable_solvable(xs);
    dataiterator_init(di, pool, repo, s ? s - s->repo->pool->solvables : 0, keyname && pool ? str2id(pool, keyname, 0) : 0, match, option);
    return di;
}

/*
 * destroy Dataiterator
 */
void
swig_dataiterator_free(Dataiterator *di)
{
    dataiterator_free(di);
    free( di );
}

/* convert Dataiterator to target value
 * if *more != 0 on return, value is incomplete
 */

static Swig_Type
dataiterator_value( Dataiterator *di )
{
  Swig_Type value = Swig_Null;

  /*
   * !! keep the order of case statements according to knownid.h !!
   */

  switch( di->key->type )
    {
      case REPOKEY_TYPE_VOID:
        value = Swig_True;
      break;
      case REPOKEY_TYPE_CONSTANT:
      case REPOKEY_TYPE_NUM:
      case REPOKEY_TYPE_U32:
        value = Swig_Int( di->kv.num );
      break;
      case REPOKEY_TYPE_CONSTANTID:
        value = Swig_String( dep2str( di->repo->pool, di->kv.id ) );
      break;
      case REPOKEY_TYPE_ID:
        if (di->data && di->data->localpool)
	  value = Swig_String( stringpool_id2str( &di->data->spool, di->kv.id ) );
	else
	  value = Swig_String( id2str( di->repo->pool, di->kv.id ) );
      break;
      case REPOKEY_TYPE_DIR:
        fprintf(stderr, "REPOKEY_TYPE_DIR: unhandled\n");
        value = Swig_Null;
      break;
      case REPOKEY_TYPE_STR:
        value = Swig_String( di->kv.str );
      break;
      case REPOKEY_TYPE_IDARRAY:
      {
        Swig_Type result = Swig_Array();
        do {
	  if (di->data && di->data->localpool)
	    Swig_Append( result, Swig_String( stringpool_id2str(&di->data->spool, di->kv.id ) ) );
	  else
	    Swig_Append( result, Swig_String( id2str( di->repo->pool, di->kv.id ) ) );
	}
	while (dataiterator_step(di));
	value = result;
      }
      break;
      case REPOKEY_TYPE_REL_IDARRAY:
        fprintf(stderr, "REPOKEY_TYPE_REL_IDARRAY: unhandled\n");
        value = Swig_Null;
      break;
      case REPOKEY_TYPE_DIRSTRARRAY:
	if (di->data)
	  value = Swig_String( repodata_dir2str(di->data, di->kv.id, di->kv.str) );
	else
	  fprintf(stderr, "REPOKEY_TYPE_DIRSTRARRAY: without repodata\n");
	break;
      case REPOKEY_TYPE_DIRNUMNUMARRAY:
        value = Swig_Array();
	if (di->data)
	{
	  Swig_Append( value, Swig_String(repodata_dir2str(di->data, di->kv.id, 0)) );
	  Swig_Append( value, Swig_Int(di->kv.num) );
	  Swig_Append( value, Swig_Int(di->kv.num2) );
	}
	else
	  fprintf(stderr, "REPOKEY_TYPE_DIRNUMNUMARRAY: without repodata\n");
      break;
      case REPOKEY_TYPE_MD5:
      case REPOKEY_TYPE_SHA1:
      case REPOKEY_TYPE_SHA256:
	if (di->data)
	  value = Swig_String( repodata_chk2str(di->data, di->key->type, (unsigned char *)di->kv.str) );
	else
	  fprintf(stderr, "REPOKEY_TYPE_{MD5,SHA1,SHA256}: without repodata\n");
      break;
      case REPOKEY_TYPE_FIXARRAY:
      case REPOKEY_TYPE_FLEXARRAY:
	value = Swig_String( di->kv.eof == 0 ? "element" : "sentinel" );
      break;
      default:
        fprintf(stderr, "Unhandled type %d\n", di->key->type);
    }
  return value;
}


%}

/*=============================================================*/
/* BINDING CODE                                                */
/*=============================================================*/

%include exception.i

/*-------------------------------------------------------------*/
/* types and typemaps */

%typemap(newfree) char * {
  free($1);
}

#if defined(SWIGRUBY)

/*
 * FILE * to Ruby
 * (copied from /usr/share/swig/ruby/file.i)
 */
%typemap(in) FILE *READ_NOCHECK {
  OpenFile *fptr;

  Check_Type($input, T_FILE);
  GetOpenFile($input, fptr);
  /*rb_io_check_writable(fptr);*/
  $1 = GetReadFile(fptr);
  rb_read_check($1)
}

/* boolean input argument */
%typemap(in) (int bflag) {
   $1 = RTEST( $input );
}
#endif

/*
 * FILE * to Perl
 */
#if defined(SWIGPERL)
%typemap(in) FILE* {
    $1 = PerlIO_findFILE(IoIFP(sv_2io($input)));
}

/*
 * XSolvable array to Perl
 */
%typemap(out) XSolvable ** {
    int n, i;

    for (n = 0; $1[n];)
        n++;

    if (n > items)
        EXTEND(sp, n - items);

    for (i = 0; i < n; i++) {
        ST(argvi) = SWIG_NewPointerObj(SWIG_as_voidptr($1[i]), SWIGTYPE_p__Solvable, SWIG_OWNER | SWIG_SHADOW);
        argvi++;
    }
    free($1);
}
#endif

#if defined(SWIGPYTHON)
/*
 * XSolvable array to Python
 */
%typemap(out) XSolvable ** {
    int n, i;
    
    for (n = 0; $1[n];)
        n++;

    $result = PyList_New(n);

    for (i = 0; i < n; i++) {
        PyObject *item = SWIG_NewPointerObj(SWIG_as_voidptr($1[i]), SWIGTYPE_p__Solvable, 0);
        PyList_SetItem($result, i, item);
    }
    free($1);
}
#endif

typedef int Id;
typedef unsigned int Offset;

/*
 * just define empty structs to expose the types to SWIG
 */

%include "pool.i"
%include "repo.i"
%include "repodata.i"
%include "repokey.i"
%include "relation.i"
%include "dependency.i"
%include "solvable.i"
%include "job.i"
%include "request.i"
%include "decision.i"
%include "problem.i"
%include "solution.i"
%include "covenant.i"
%include "ruleinfo.i"
%include "solver.i"
%include "dataiterator.i"
%include "step.i"
%include "transaction.i"
