/* ====================================================
 * CUTEst interface for OptimPack
 *
 * Éric Thiébaut, November 12th, 2015
 *
 * (Based on CUTEr genc_main.c of D. Orban, Feb 3, 2003)
 * (CUTEst evolution, Nick Gould, Apr 2, 2014)
 * ====================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAXLINE 512
#define VERBOSE 0

#ifdef __cplusplus
extern "C" {   /* To prevent C++ compilers from mangling symbols */
#endif

#include "cutest.h"
#include "optimpack.h"

typedef enum {
  NLCG = 0, VMLM, VMLMB
} algorithm_t;

static void getinfo(integer n, integer m, doublereal *bl, doublereal *bu,
                    doublereal *cl, doublereal *cu, logical *equatn,
                    logical *linear, VarTypes *vartypes)
{
  int i;

  vartypes->nlin = 0;
  vartypes->neq = 0;
  vartypes->nbnds = 0;
  vartypes->nrange = 0;
  vartypes->nlower = 0;
  vartypes->nupper = 0;
  vartypes->nineq = 0;
  vartypes->nineq_lin = 0;
  vartypes->nineq_nlin = 0;
  vartypes->neq_lin = 0;
  vartypes->neq_nlin = 0;

  for (i = 0; i < n; ++i) {
    if (bl[i] > -CUTE_INF || bu[i] < CUTE_INF) {
      ++vartypes->nbnds;
    }
  }
  for (i = 0; i < m; ++i) {
    if (linear[i]) {
      ++vartypes->nlin;
    }
    if (equatn[i]) {
      ++vartypes->neq;
      if (linear[i]) {
        ++vartypes->neq_lin;
      } else {
        ++vartypes->neq_nlin;
      }
    } else {
      if (cl[i] > -CUTE_INF) {
        if (cu[i] < CUTE_INF) {
          vartypes->nrange++;
        } else {
          vartypes->nlower++;
          vartypes->nineq++;
        }
      } else {
        if (cu[i] < CUTE_INF) {
          ++vartypes->nupper;
          ++vartypes->nineq;
        }
      }
      if (!equatn[i] && linear[i]) {
        ++vartypes->nineq_lin;
      } else {
        ++vartypes->nineq_nlin;
      }
    }
  }
}

/* Return the (projected) gradient norm as in CG_DESCENT or ASA_CG */
static double
gradnorm(const opk_vector_t* x, const opk_vector_t* g, opk_vector_t* gp,
         const opk_bound_t* xl, const opk_bound_t* xu)
{
  if (gp != NULL) {
    /* compute ||Projection(x_k - g_k) - x_k||_infty */
    opk_vaxpby(gp, 1, x, -1, g);
    if (opk_box_project_variables(gp, gp, xl, xu) != OPK_SUCCESS) {
      printf("# Failed to project variables\n");
      exit(-1);
    }
    opk_vaxpby(gp, 1, gp, -1, x);
    return opk_vnorminf(gp);
  } else {
    return opk_vnorminf(g);
  }
}

/* global variables */
integer CUTEst_nvar;	    /* number of variables */
integer CUTEst_ncon;	    /* number of constraints */

/* main program */
int MAINENTRY(void)
{
  char *fname = "OUTSDIF.d"; /* CUTEst data file */
  integer funit = 42;        /* FORTRAN unit number for OUTSDIF.d */
  integer io_buffer = 11;    /* FORTRAN unit for internal i/o */
  integer iout = 6;          /* FORTRAN unit number for error output */
  integer ierr;              /* Exit flag from OPEN and CLOSE */
  integer status;            /* Exit flag from CUTEst tools */
  VarTypes vartypes;
  integer    ncon_dummy;
  doublereal *x, *g, *bl, *bu;
  doublereal *v = NULL, *cl = NULL, *cu = NULL;
  logical *equatn = NULL, *linear = NULL;
  char *pname, *vnames, *gnames, *cptr;
  char **Vnames, **Gnames; /* vnames and gnames as arrays of strings */
  integer e_order = 1, l_order = 0, v_order = 0;
  logical     constrained = FALSE_;
  doublereal  calls[7], cpu[2];
  integer     i, j;
  FILE       *spec;

  /* Variables for OptimPack methods. */
  opk_vspace_t* vspace;
  opk_vector_t* vx;
  opk_vector_t* vg;
  opk_vector_t* vgp;
  opk_vector_t* vbl;
  opk_vector_t* vbu;
  opk_nlcg_t*  nlcg  = NULL; /* non-linear conjugate gradient optimizer */
  opk_vmlm_t*  vmlm  = NULL; /* limited memory quasi-Newton optimizer */
  opk_vmlmb_t* vmlmb = NULL; /* idem with bound constraints */
  opk_vmlmn_t* vmlmn = NULL; /* idem with bound constraints */
  opk_lbfgs_t* lbfgs = NULL; /* LBFGS optimizer */
  opk_lnsrch_t* lnsrch = NULL; /* Line search method */
  opk_task_t task;
  opk_status_t final_status;
#define NLCG  1
#define VMLM  2
#define LBFGS 3
#define VMLMB 4
#define BLMVM 5
#define VMLMN 6
  int algorithm = VMLMB;
  unsigned int vmlmn_flags = 0;
  unsigned int nlcg_method = OPK_NLCG_DEFAULT;
  const char* algorithm_name;
  opk_bound_t* lower;
  opk_bound_t* upper;
  double f, gnorm, finalf, finalgnorm, gtest;
  double gatol = 1.0e-6;    /* required gradient absolute tolerance */
  double grtol = 0.0;       /* required gradient relative tolerance */
  double delta = 5e-2;
  double epsilon = 1e-2;
  double sftol = 1e-4;
  double sgtol = 0.9;
  double sxtol = 1e-15;
  double samin = 0.1;
  int delta_given = FALSE_;
  int epsilon_given = FALSE_;
  int maxiter = -1;
  int maxeval = -1;
  int verbose = 0;
  int evaluations, iterations, bounds;
  int mem = 5; /* number of saved (Y,S) pairs memorized by the quasi-Newton
                  method to approximate the Hessian, if zero non-linear
                  conjugate gradient is used */


  /* Open problem description file OUTSDIF.d */
  ierr = 0;
  FORTRAN_open( &funit, fname, &ierr );
  if (ierr) {
    printf("# Error opening file OUTSDIF.d.\nAborting.\n");
    exit(1);
  }

  /* Determine problem size */
  CUTEST_cdimen(&status, &funit, &CUTEst_nvar, &CUTEst_ncon);
  if (status != 0) {
    printf("# CUTEst error, status = %d, aborting\n", status);
    exit(status);
  }

  /* Determine whether to call constrained or unconstrained tools */
  constrained = (CUTEst_ncon > 0);

  /* Seems to be needed for some Solaris C compilers */
  ncon_dummy = CUTEst_ncon + 1;

  /* Reserve memory for variables, gradient, bounds, and multipliers and
     call appropriate initialization routine for CUTEst */
  x  = (double*)malloc(CUTEst_nvar*sizeof(double));
  g  = (double*)malloc(CUTEst_nvar*sizeof(double));
  bl = (double*)malloc(CUTEst_nvar*sizeof(double));
  bu = (double*)malloc(CUTEst_nvar*sizeof(double));
  if (constrained) {
    MALLOC( equatn, CUTEst_ncon, logical    );
    MALLOC( linear, CUTEst_ncon, logical    );
    MALLOC( v,      CUTEst_ncon, doublereal );
    MALLOC( cl,     CUTEst_ncon, doublereal );
    MALLOC( cu,     CUTEst_ncon, doublereal );
    CUTEST_csetup(&status, &funit, &iout, &io_buffer,
                  &CUTEst_nvar, &CUTEst_ncon, x, bl, bu,
                  v, cl, cu, equatn, linear,
                  &e_order, &l_order, &v_order);
  } else {
    CUTEST_usetup(&status, &funit, &iout, &io_buffer, &CUTEst_nvar, x, bl, bu);
  }
  if (status != 0) {
    printf("# CUTEst error, status = %d, aborting\n", status);
    exit(status);
  }

  /* Get problem, variables and constraints names */
  MALLOC(pname, FSTRING_LEN + 1, char);
  MALLOC(vnames, CUTEst_nvar * FSTRING_LEN, char);        /* For Fortran */
  MALLOC(Vnames, CUTEst_nvar, char *);               /* Array of strings */
  for (i = 0; i < CUTEst_nvar; i++) {
    MALLOC(Vnames[i], FSTRING_LEN + 1, char);
  }
  if (constrained) {
    MALLOC(gnames, CUTEst_ncon * FSTRING_LEN, char);   /* For Fortran */
    MALLOC(Gnames, CUTEst_ncon, char *);          /* Array of strings */
    for (i = 0; i < CUTEst_ncon; i++) {
      MALLOC(Gnames[i], FSTRING_LEN + 1, char);
    }
    CUTEST_cnames(&status, &CUTEst_nvar, &CUTEst_ncon,
                  pname, vnames, gnames);
  } else {
    CUTEST_unames(&status, &CUTEst_nvar, pname, vnames);
  }
  if (status != 0) {
    printf("# CUTEst error, status = %d, aborting\n", status);
    exit(status);
  }

  /* Make sure to null-terminate problem name */
  i = FSTRING_LEN;
  do {
     pname[i] = '\0';
  } while (--i >= 0 && pname[i] == ' ');

  /* Transfer variables and constraint names into arrays of
   * null-terminated strings.
   * If you know of a simpler way to do this portably, let me know!
   */
  for (i = 0; i < CUTEst_nvar; i++) {
    cptr = vnames + i * FSTRING_LEN;
    for (j = 0; j < FSTRING_LEN; j++) {
      Vnames[i][j] = *cptr;
      cptr++;
    }
    Vnames[i][FSTRING_LEN] = '\0';
  }

  for (i = 0; i < CUTEst_ncon; i++) {
    cptr = gnames + i * FSTRING_LEN;
    for (j = 0; j < FSTRING_LEN; j++) {
      Gnames[i][j] = *cptr;
      cptr++;
    }
    Gnames[i][FSTRING_LEN] = '\0';
  }

  /* Fortran strings no longer needed */
  FREE(vnames);
  if (constrained) FREE(gnames);

#if VERBOSE
  printf("# Variable names:\n");
  for (i = 0; i < CUTEst_nvar; i++) {
    printf("#  %s\n", Vnames[i]);
  }
#endif

  /* Free memory for variable names */
  for (i = 0; i < CUTEst_nvar; i++) {
    FREE(Vnames[i]);
  }
  FREE(Vnames);

#if VERBOSE
  if (constrained) {
    printf("# Constraint names:\n");
    for (i = 0; i < CUTEst_ncon; i++) {
      printf("#  %s\n", Gnames[i]);
    }
  }
#endif

  if (constrained) {
    /* Free memory for constraint names */
    for (i = 0; i < CUTEst_ncon; i++) {
      FREE(Gnames[i]);
    }
    FREE(Gnames);
  }

  if (constrained) {
    printf("# the problem %s has %i constraints\n",
            pname,  CUTEst_ncon);
    printf("# current OptimPack is for unconstrained optimization\n");
    exit(-1);
  }

  /* Obtain basic info on problem */
  getinfo(CUTEst_nvar, CUTEst_ncon, bl, bu, NULL, NULL, NULL, NULL, &vartypes);
  bounds = 0;
  for (i = 0; i < CUTEst_nvar; ++i) {
    /*if (i < 10) printf("# bl[%d] = %g / bu[%d] = %g\n", i, bl[i], i, bu[i]);*/
    if (bl[i] > -CUTE_INF) {
      bounds |= 1;
    }
    if (bu[i] < CUTE_INF) {
      bounds |= 2;
    }
  }


  /*printf("# Problem: %s (n = %i)\n", pname, CUTEst_nvar );*/

  /* MALLOC(vnames, CUTEst_nvar*FSTRING_LEN, char);
     CUTEST_unames( &status, &CUTEst_nvar, pname, vnames);
     if( status ) {
     printf("# CUTEst error, status = %d, aborting\n", status);
     exit(status);
     }
     FREE(vnames);
  */

  /* Set any parameter values here
     First read in the default parameter values */

  /* Parameter values are overwritten using any values stored in the
     OPTIMPACK.SPC file. The format of the file is parameter name at
     the start of the line followed by one or more spaces and then
     the parameter value.  */

  spec = fopen("OPTIMPACK.SPC", "r");
  if (spec != NULL) {
    char line[MAXLINE+1];
    char str[MAXLINE+1];
    int ival;
    double dval;
    int powell = FALSE_;
    unsigned int autostep = 0;
    nlcg_method = OPK_NLCG_POLAK_RIBIERE_POLYAK;
    while (fgets(line, MAXLINE, spec) != (char*)NULL) {
      /* determine the parameter and its value */
      line[MAXLINE] = 0;
      if (sscanf(line, " algorithm %s", str) == 1) {
        if (strcasecmp(str, "nlcg") == 0) {
          algorithm = NLCG;
        } else if (strcasecmp(str, "vmlm") == 0) {
          algorithm = VMLM;
        } else if (strcasecmp(str, "vmlmb") == 0) {
          algorithm = VMLMB;
        } else if (strcasecmp(str, "vmlmn") == 0) {
          algorithm = VMLMN;
        } else if (strcasecmp(str, "blmvm") == 0) {
          algorithm = VMLMN;
          vmlmn_flags = OPK_EMULATE_BLMVM;
        } else if (strcasecmp(str, "lbfgs") == 0) {
          algorithm = LBFGS;
        } else {
          printf("# Unknown algorithm\n");
          exit(-1);
        }
        continue;
      }
      if (sscanf(line, " linesearch %s", str) == 1) {
        if (strcasecmp(str, "quadratic") == 0) {
          lnsrch = opk_lnsrch_new_backtrack(sftol, samin);
        } else if (strcasecmp(str, "Armijo") == 0) {
          lnsrch = opk_lnsrch_new_backtrack(sftol, 0.5);
        } else if (strcasecmp(str, "cubic") == 0) {
          lnsrch = opk_lnsrch_new_csrch(sftol, sgtol, sxtol);
        } else {
          printf("# Unknown line search method\n");
          exit(-1);
        }
        continue;
      }
      if (sscanf(line, " Powell %d", &ival) == 1) {
        if (ival != 0) {
          powell = TRUE_;
        }
        continue;
      }
      if (sscanf(line, " ShannoPhua %d", &ival) == 1) {
        if (ival != 0) {
          autostep = OPK_NLCG_SHANNO_PHUA;
        }
        continue;
      }
      if (sscanf(line, " OrenSpedicato %d", &ival) == 1) {
        if (ival != 0) {
          autostep = OPK_NLCG_OREN_SPEDICATO;
        }
        continue;
      }
      if (sscanf(line, " BarzilaiBorwein %d", &ival) == 1) {
        if (ival != 0) {
          autostep = OPK_NLCG_BARZILAI_BORWEIN;
        }
        continue;
      }
      if (sscanf(line, " FletcherReeves %d", &ival) == 1) {
        if (ival != 0) {
          nlcg_method = OPK_NLCG_FLETCHER_REEVES;
        }
        continue;
      }
      if (sscanf(line, " HestenesStiefel %d", &ival) == 1) {
        if (ival != 0) {
          nlcg_method = OPK_NLCG_HESTENES_STIEFEL;
        }
        continue;
      }
      if (sscanf(line, " PolakRibierePolyak %d", &ival) == 1) {
        if (ival != 0) {
          nlcg_method = OPK_NLCG_POLAK_RIBIERE_POLYAK;
        }
        continue;
      }
      if (sscanf(line, " Fletcher %d", &ival) == 1) {
        if (ival != 0) {
          nlcg_method = OPK_NLCG_FLETCHER;
        }
        continue;
      }
      if (sscanf(line, " LiuStorey %d", &ival) == 1) {
        if (ival != 0) {
          nlcg_method = OPK_NLCG_LIU_STOREY;
        }
        continue;
      }
      if (sscanf(line, " DaiYuan %d", &ival) == 1) {
        if (ival != 0) {
          nlcg_method = OPK_NLCG_DAI_YUAN;
        }
        continue;
      }
      if (sscanf(line, " PerryShanno %d", &ival) == 1) {
        if (ival != 0) {
          nlcg_method = OPK_NLCG_PERRY_SHANNO;
        }
        continue;
      }
      if (sscanf(line, " HagerZhang %d", &ival) == 1) {
        if (ival != 0) {
          nlcg_method = OPK_NLCG_HAGER_ZHANG;
        }
        continue;
      }
      if (sscanf(line, " mem %d", &ival) == 1) {
        if (mem <= 0) {
          printf("# Illegal value for option MEM\n");
          exit(-1);
        }
        mem = ival;
        continue;
      }
      if (sscanf(line, " maxiter %d", &ival) == 1) {
        maxiter = ival;
        continue;
      }
      if (sscanf(line, " maxeval %d", &ival) == 1) {
        maxeval = ival;
        continue;
      }
      if (sscanf(line, " verb %d", &ival) == 1) {
        verbose = ival;
        continue;
      }
      if (sscanf(line, " delta %lf", &dval) == 1) {
        delta = dval;
        delta_given = TRUE_;
        continue;
      }
      if (sscanf(line, " epsilon %lf", &dval) == 1) {
        epsilon = dval;
        epsilon_given = TRUE_;
        continue;
      }
      if (sscanf(line, " gatol %lf", &dval) == 1) {
        gatol = dval;
        continue;
      }
      if (sscanf(line, " grtol %lf", &dval) == 1) {
        grtol = dval;
        continue;
      }
    }
    if (powell) {
      nlcg_method |= OPK_NLCG_POWELL;
    }
    if (autostep != 0) {
      nlcg_method |= autostep;
    }
    fclose(spec);
  }
  if (gatol < 0) {
    printf("# Bad value for GATOL\n");
    exit(-1);
  }
  if (grtol < 0) {
    printf("# Bad value for GRTOL\n");
    exit(-1);
  }

  /* Check whether the problem has constraints */
  if (bounds != 0 && mem == 0) {
    printf("# Use VMLMB or BLMVM for bound constrained optimization\n");
    exit(-1);
  }

  /* Build vector space and populate it with work vectors. */
  vspace = opk_new_simple_double_vector_space(CUTEst_nvar);
  if (vspace == NULL) {
    printf("# Failed to allocate vector space\n");
    exit(-1);
  }
  vx  = opk_wrap_simple_double_vector(vspace, x,  free, x);
  vg  = opk_wrap_simple_double_vector(vspace, g,  free, g);
  vbl = opk_wrap_simple_double_vector(vspace, bl, free, bl);
  vbu = opk_wrap_simple_double_vector(vspace, bu, free, bu);
  if (vx == NULL || vbl == NULL || vbu == NULL) {
    printf("# Failed to wrap vectors\n");
    exit(-1);
  }
  if ((bounds & 1) == 0) {
    lower = NULL;
  } else {
    lower = opk_new_bound(vspace, OPK_BOUND_VECTOR, vbl);
    if (lower == NULL) {
      printf("# Failed to wrap lower bounds\n");
      exit(-1);
    }
  }
  if ((bounds & 2) == 0) {
    upper = NULL;
  } else {
    upper = opk_new_bound(vspace, OPK_BOUND_VECTOR, vbu);
    if (upper == NULL) {
      printf("# Failed to wrap upper bounds\n");
      exit(-1);
    }
  }

  /* Create the optimizer */
  vgp = NULL;
  if (algorithm == VMLMB) {
    opk_vmlmb_options_t options;
    algorithm_name = "VMLMB-";
    if (lnsrch != NULL) {
      vmlmb = opk_new_vmlmb_optimizer_with_line_search(vspace, mem, lnsrch);
    } else {
      vmlmb = opk_new_vmlmb_optimizer(vspace, mem);
    }
    if (vmlmb == NULL) {
      printf("# Failed to create VMLMB optimizer\n");
      exit(-1);
    }
    vgp = opk_vcreate(vspace);
    if (vgp == NULL) {
      printf("# Failed to create projected gradient vector\n");
      exit(-1);
    }
    opk_get_vmlmb_options(&options, vmlmb);
    options.gatol = 0.0;
    options.grtol = 0.0;
    if (delta_given) {
      options.delta = delta;
    } else {
      delta = options.delta;
    }
    if (epsilon_given) {
      options.epsilon = epsilon;
    } else {
      epsilon = options.epsilon;
    }
    if (opk_set_vmlmb_options(vmlmb, &options) != OPK_SUCCESS) {
      printf("# Bad VMLMB options\n");
      exit(-1);
    }
    task = opk_start_vmlmb(vmlmb, vx, lower, upper);
  } else if (algorithm == VMLMN) {
    opk_vmlmn_options_t options;
    vmlmn = opk_new_vmlmn_optimizer(vspace, mem, vmlmn_flags,
                                    lower, upper, lnsrch);
    if (vmlmn == NULL) {
      printf("# Failed to create VMLMN optimizer\n");
      exit(-1);
    }
    algorithm_name = opk_get_vmlmn_method_name(vmlmn);
    vgp = opk_vcreate(vspace);
    if (vgp == NULL) {
      printf("# Failed to create projected gradient vector\n");
      exit(-1);
    }
    opk_get_vmlmn_options(&options, vmlmn);
    options.gatol = 0.0;
    options.grtol = 0.0;
    if (delta_given) {
      options.delta = delta;
    } else {
      delta = options.delta;
    }
    if (epsilon_given) {
      options.epsilon = epsilon;
    } else {
      epsilon = options.epsilon;
    }
    if (opk_set_vmlmn_options(vmlmn, &options) != OPK_SUCCESS) {
      printf("# Bad VMLMN options\n");
      exit(-1);
    }
    task = opk_start_vmlmn(vmlmn, vx);
  } else if (algorithm == VMLM) {
    opk_vmlm_options_t options;
    algorithm_name = "VMLM";
    if (bounds != 0) {
      printf("# Algorithm %s cannot be used with bounds\n",
             algorithm_name);
      exit(-1);
    }
    if (lnsrch != NULL) {
      vmlm = opk_new_vmlm_optimizer_with_line_search(vspace, mem, lnsrch);
    } else {
      vmlm = opk_new_vmlm_optimizer(vspace, mem);
    }
    if (vmlm == NULL) {
      printf("# Failed to create VMLM optimizer\n");
      exit(-1);
    }
    opk_get_vmlm_options(&options, vmlm);
    options.gatol = 0.0;
    options.grtol = 0.0;
    if (delta_given) {
      options.delta = delta;
    } else {
      delta = options.delta;
    }
    if (epsilon_given) {
      options.epsilon = epsilon;
    } else {
      epsilon = options.epsilon;
    }
    if (opk_set_vmlm_options(vmlm, &options) != OPK_SUCCESS) {
      printf("# Bad VMLM options\n");
      exit(-1);
    }
    task = opk_start_vmlm(vmlm, vx);
  } else if (algorithm == LBFGS) {
    opk_lbfgs_options_t options;
    algorithm_name = "LBFGS";
    if (bounds != 0) {
      printf("# Algorithm %s cannot be used with bounds\n",
             algorithm_name);
      exit(-1);
    }
    if (lnsrch != NULL) {
      lbfgs = opk_new_lbfgs_optimizer_with_line_search(vspace, mem, lnsrch);
    } else {
      lbfgs = opk_new_lbfgs_optimizer(vspace, mem);
    }
    if (lbfgs == NULL) {
      printf("# Failed to create LBFGS optimizer\n");
      exit(-1);
    }
    opk_get_lbfgs_options(&options, lbfgs);
    options.gatol = 0.0;
    options.grtol = 0.0;
    if (delta_given) {
      options.delta = delta;
    } else {
      delta = options.delta;
    }
    if (epsilon_given) {
      options.epsilon = epsilon;
    } else {
      epsilon = options.epsilon;
    }
    if (opk_set_lbfgs_options(lbfgs, &options) != OPK_SUCCESS) {
      printf("# Bad LBFGS options\n");
      exit(-1);
    }
    task = opk_start_lbfgs(lbfgs, vx);
  } else if (algorithm == NLCG) {
    opk_nlcg_options_t options;
    algorithm_name = "NLCG";
    if (bounds != 0) {
      printf("# Algorithm %s cannot be used with bounds\n",
             algorithm_name);
      exit(-1);
    }
    if (lnsrch != NULL) {
      nlcg = opk_new_nlcg_optimizer_with_line_search(vspace, nlcg_method, lnsrch);
    } else {
      nlcg = opk_new_nlcg_optimizer(vspace, nlcg_method);
    }
    if (nlcg == NULL) {
      printf("# Failed to create NLCG optimizer\n");
      exit(-1);
    }
    opk_get_nlcg_options(&options, nlcg);
    options.gatol = 0.0;
    options.grtol = 0.0;
    if (delta_given) {
      options.delta = delta;
    } else {
      delta = options.delta;
    }
    if (epsilon_given) {
      options.epsilon = epsilon;
    } else {
      epsilon = options.epsilon;
    }
    if (opk_set_nlcg_options(nlcg, &options) != OPK_SUCCESS) {
      printf("# Bad NLCG options\n");
      exit(-1);
    }
    task = opk_start_nlcg(nlcg, vx);
  } else {
    printf("# Bad algorithm\n");
    exit(-1);
  }

  /* Iteratively call the optimizer */
  evaluations = 0;
  iterations = 0;
  f = -1;
  gnorm = -1;
  finalf = -1;
  finalgnorm = -1;
  gtest = -1;
  if (verbose > 0) {
    char str[256];
    int len1, len2, off;
    strcpy(str, "--------------------------------------------------------------------");
    len1 = strlen(pname);
    len2 = strlen(str);
    off = (len2/2) - (len1/2);
    if (off < 3) {
      off = 3;
    }
    str[off] = '{';
    str[off+1] = ' ';
    strcpy(&str[off+2], pname);
    str[off+len1+2] = ' ';
    str[off+len1+3] = '}';
    printf("# %s\n", str);
    printf("# Iter.  Eval.  Reset            F               ||G||         Step\n");
    printf("# --------------------------------------------------------------------\n");
  }
  final_status = OPK_SUCCESS;
  while (TRUE_) {
    if (task == OPK_TASK_COMPUTE_FG) {
      logical grad = TRUE_;
      if (maxeval > 0 && evaluations > maxeval) {
        final_status = OPK_TOO_MANY_EVALUATIONS;
        task = OPK_TASK_FINAL_X; /* to force terminating */
      } else {
        CUTEST_uofg(&status, &CUTEst_nvar, x, &f, g, &grad);
        if ((status == 1) || (status == 2)) {
          printf("# CUTEst error, status = %d, aborting\n", status);
          exit(status);
        }
        gnorm = gradnorm(vx, vg, vgp, lower, upper); /* FIXME: this adds some overhead */
        ++evaluations;
        if (evaluations == 1) {
          gtest = gatol + grtol*gnorm;
        }
        if (gnorm <= gtest) {
          task = OPK_TASK_FINAL_X;
        }
        if (evaluations == 1 || f < finalf) {
          finalf = f;
          finalgnorm = gnorm;
        }
      }
    } else if (task != OPK_TASK_NEW_X) {
      /* Either algorithm converged, or an exception (error or warning)
         occured. */
      if (vmlmb != NULL) {
        final_status = opk_get_vmlmb_status(vmlmb);
      } else if (vmlmn != NULL) {
        final_status = opk_get_vmlmn_status(vmlmn);
      } else if (vmlm != NULL) {
        final_status = opk_get_vmlm_status(vmlm);
      } else if (lbfgs != NULL) {
        final_status = opk_get_lbfgs_status(lbfgs);
      }
      task = OPK_TASK_FINAL_X; /* to force terminating */
    }
    if (task == OPK_TASK_NEW_X || task == OPK_TASK_FINAL_X) {
      if (evaluations > 1) {
        ++iterations;
      }
      if (verbose > 0 && (task == OPK_TASK_FINAL_X ||
                          (iterations%verbose) == 0)) {
        double alpha;
        opk_index_t restarts;
        if (vmlmb != NULL) {
          alpha = opk_get_vmlmb_step(vmlmb);
          restarts = opk_get_vmlmb_restarts(vmlmb);
        } else if (vmlmn != NULL) {
          alpha = opk_get_vmlmn_step(vmlmn);
          restarts = opk_get_vmlmn_restarts(vmlmn);
        } else if (vmlm != NULL) {
          alpha = opk_get_vmlm_step(vmlm);
          restarts = opk_get_vmlm_restarts(vmlm);
        } else if (lbfgs != NULL) {
          alpha = opk_get_lbfgs_step(lbfgs);
          restarts = opk_get_lbfgs_restarts(lbfgs);
        } else {
          alpha = opk_get_nlcg_step(nlcg);
          restarts = 0;
        }
        printf("  %6d %6d %6ld %23.15e %11.3e %11.3e\n",
               iterations, evaluations, (long)restarts, f, gnorm, alpha);
      }
      if (task == OPK_TASK_FINAL_X) {
        break;
      }
      if (maxiter > 0 && iterations >= maxiter) {
        final_status = OPK_TOO_MANY_ITERATIONS;
        break;
      }
    }
    if (vmlmb != NULL) {
      task = opk_iterate_vmlmb(vmlmb, vx, f, vg, lower, upper);
    } else if (vmlmn != NULL) {
      task = opk_iterate_vmlmn(vmlmn, vx, f, vg);
    } else if (vmlm != NULL) {
      task = opk_iterate_vmlm(vmlm, vx, f, vg);
    } else if (lbfgs != NULL) {
      task = opk_iterate_lbfgs(lbfgs, vx, f, vg);
    } else {
      task = opk_iterate_nlcg(nlcg, vx, f, vg);
    }
  }
#if 0
  opk_vprint(stderr, " x", vx, 10);
  opk_vprint(stderr, " g", vg, 10);
  opk_vprint(stderr, "xl", vbl, 10);
  opk_vprint(stderr, "xu", vbu, 10);
#endif

  /* Get CUTEst statistics */
  CUTEST_creport(&status, calls, cpu);
  if (status != 0) {
    printf("# CUTEst error, status = %d, aborting\n", status);
    exit(status);
  }

  /* Print statistics */
  printf("# *********************** CUTEst statistics ************************\n");
  printf("#                Algorithm = OptimPack/%s ", algorithm_name);
  if (algorithm == VMLMB || algorithm == VMLM || algorithm == VMLMN) {
    printf("(mem=%d)\n", OPK_MIN(CUTEst_nvar, mem));
  } else {
    char temp[OPK_NLCG_DESCRIPTION_MAX_SIZE];
    opk_get_nlcg_description(nlcg, temp);
    printf("(%s)\n", temp);
  }
  printf("#                  Problem = %-s\n", pname);
  printf("#                Variables = %-10d\n", CUTEst_nvar);
  printf("#        Bound constraints = %-10d\n", vartypes.nbnds);
  printf("#               Iterations = %d\n", iterations);
  printf("# Objective function calls = %.0f\n", calls[0]);
  printf("# Objective gradient calls = %.0f\n", calls[1]);
  printf("#       Objective Hessians = %.0f\n", calls[2]);
  printf("#  Hessian-vector products = %.0f\n", calls[3]);
  printf("#                Exit code = %d (%s)\n", final_status,
         opk_get_reason(final_status));
  printf("#                  Final f = %-23.15e\n", finalf);
  printf("#              Final ||g|| = %-15.7e\n", finalgnorm);
  printf("#              Set up time = %#-14.6f seconds\n", cpu[0]);
  printf("#               Solve time = %#-14.6f seconds\n", cpu[1]);
  printf("#      Relative small step = %g\n", delta);
  printf("#        Descent threshold = %g\n", epsilon);
  printf("# ******************************************************************\n");

  ierr = 0;
  FORTRAN_close(&funit, &ierr);
  if (ierr != 0) {
    printf("# Error closing %s on unit %d.\n", fname, funit);
    printf("# Trying not to abort.\n");
  }

  /* Free workspace */
  FREE(pname);
  OPK_DROP(vx);
  OPK_DROP(vg);
  OPK_DROP(vgp);
  OPK_DROP(vbl);
  OPK_DROP(vbu);
  OPK_DROP(vspace);
  OPK_DROP(nlcg);

  CUTEST_uterminate(&status);

  return 0;
}

#ifdef __cplusplus
}    /* Closing brace for  extern "C"  block */
#endif
