/*
 * vmlmb.c --
 *
 * Limited memory variable metric method with bound constraints for OptimPack
 * library.
 *
 *-----------------------------------------------------------------------------
 *
 * Copyright (c) 2002, 2015 Éric Thiébaut
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 *-----------------------------------------------------------------------------
 */

#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include "optimpack-private.h"

#define SAVE_MEMORY 1 /* save memory to use 2 + 2m vectors instead of 4 + 2m */

#define TRUE                          OPK_TRUE
#define FALSE                         OPK_FALSE
#define MAX(a, b)                     OPK_MAX(a, b)
#define MIN(a, b)                     OPK_MIN(a, b)
#define ROUND_UP(a, b)                OPK_ROUND_UP(a,b )
#define ADDRESS(type, base, offset)   OPK_ADDRESS(type, base, offset)

/*---------------------------------------------------------------------------*/
/* VARIABLE METRIC OPTIMIZER */

/* Default parameters for the line search.  These values are from original
   LBFGS algorithm by Jorge Nocedal but other values may be more suitable. */
static const double STPMIN = 1E-20;
static const double STPMAX = 1E+20;
static const double SFTOL = 1E-4;
static const double SGTOL = 0.9;

/* Default parameters for the global convergence. */
static const double GRTOL = 1E-6;
static const double GATOL = 0.0;

/* Other default parameters. */
static const double EPSILON = 1e-2;
static const double DELTA   = 5e-2;

struct _opk_vmlmb {
  opk_object_t base; /**< Base type (must be the first member). */
  double delta;      /**< Relative size for a small step. */
  double epsilon;    /**< Threshold for Zoutendijk test. */
  double grtol;      /**< Relative threshold for the norm or the projected
                          gradient (relative to GPINIT the norm of the initial
                          projected gradient) for convergence. */
  double gatol;      /**< Absolute threshold for the norm or the projected
                          gradient for convergence. */
  double ginit;      /**< Euclidean norm or the initial projected gradient. */
  double f0;         /**< Function value at x0. */
  double gpnorm;     /**< Euclidean norm of the projected gradient at the last
                          tested step. */
  double alpha;      /**< Current step length. */
  double stpmin;     /**< Relative mimimum step length. */
  double stpmax;     /**< Relative maximum step length. */

  opk_vspace_t* space;
  opk_lnsrch_t* lnsrch;
  opk_vector_t* x0; /**< Variables at the start of the line search. */
  opk_vector_t* g0; /**< Gradient at x0. */
  opk_vector_t* d;  /**< Anti-search direction; a iterate is computed
                         as: x1 = x0 - alpha*d with alpha > 0. */
  opk_vector_t* w;  /**< Vector whose elements are set to 1 for free variables
                         and 0 otherwise. */
  opk_vector_t* tmp; /**< Scratch work vector. */


  /* Limited memory BFGS approximation of the Hessian of the objective
     function. */
  double gamma;        /**< Scale factor to approximate inverse Hessian. */
  opk_vector_t** s;    /**< Storage for variable differences. */
  opk_vector_t** y;    /**< Storage for gradient differences. */
  double* beta;        /**< Workspace to save <d,s>/<s,y> */
  double* rho;         /**< Workspace to save 1/<s,y> */
  opk_index_t m;       /**< Maximum number of memorized steps. */
  opk_index_t mp;      /**< Actual number of memorized steps (0 <= mp <= m). */
  opk_index_t updates; /**< Number of BFGS updates since start. */

  opk_index_t evaluations; /**< Number of functions (and gradients)
                                evaluations. */
  opk_index_t iterations;  /**< Number of iterations (successful steps
                                taken). */
  opk_index_t restarts;    /**< Number of LBFGS recurrence restarts. */
  opk_task_t task;
  int reason; /**< some details about the error */
};

typedef enum {
  OPK_NO_PROBLEMS = 0,
  OPK_BAD_PRECONDITIONER,  /**< Preconditioner is not positive definite. */
  OPK_LINE_SEARCH_WARNING, /**< Warning in line search. */
  OPK_LINE_SEARCH_ERROR,   /**< Error in line search. */
  OPK_INFEASIBLE_BOUNDS,   /**< Bad bounds. */
  OPK_STEP_LIMITS_NOT_IMPLEMENTED,
  OPK_WOULD_BLOCK,
  OPK_INSUFICIENT_MEMORY
} opk_reason_t;

static double
max3(double a1, double a2, double a3)
{
  if (a3 >= a2) {
    return (a3 >= a1 ? a3 : a1);
  } else {
    return (a2 >= a1 ? a2 : a1);
  }
}

static int
non_finite(double x)
{
  return (isnan(x) || isinf(x));
}

static void
finalize_vmlmb(opk_object_t* obj)
{
  opk_vmlmb_t* opt = (opk_vmlmb_t*)obj;
  opk_index_t k;

  /* Drop references to all objects (neither opt->x0 nor opt->g0 which are weak
     references to specific vectors in opt->s and opt->y). */
  OPK_DROP(opt->space);
  OPK_DROP(opt->lnsrch);
#if ! SAVE_MEMORY
  OPK_DROP(opt->x0);
  OPK_DROP(opt->g0);
#endif
  OPK_DROP(opt->d);
  OPK_DROP(opt->w);
  OPK_DROP(opt->tmp);
  if (opt->s != NULL) {
    for (k = 0; k < opt->m; ++k) {
      OPK_DROP(opt->s[k]);
    }
  }
  if (opt->y != NULL) {
    for (k = 0; k < opt->m; ++k) {
      OPK_DROP(opt->y[k]);
    }
  }
}

/* The following function returns the index where is stored the
   (updates-j)-th correction pair.  Argument j must be in the
   inclusive range [0:mp] with mp the actual number of saved
   corrections.  At any moment, `0 ≤ mp ≤ min(m,updates)`; thus
   `updates - j ≥ 0`. */
static opk_index_t
slot(const opk_vmlmb_t* opt, opk_index_t j)
{
  return (opt->updates - j)%opt->m;
}

opk_vector_t*
opk_get_vmlmb_s(opk_vmlmb_t* opt, opk_index_t k)
{
  return (0 <= k && k <= opt->mp ? opt->s[slot(opt, k)] : NULL);
}

opk_vector_t*
opk_get_vmlmb_y(opk_vmlmb_t* opt, opk_index_t k)
{
  return (0 <= k && k <= opt->mp ? opt->y[slot(opt, k)] : NULL);
}

static opk_task_t
optimizer_success(opk_vmlmb_t* opt, opk_task_t task)
{
  opt->reason = OPK_NO_PROBLEMS;
  opt->task = task;
  return task;
}

static opk_task_t
optimizer_failure(opk_vmlmb_t* opt, opk_reason_t reason)
{
  opt->reason = reason;
  opt->task = OPK_TASK_ERROR;
  return opt->task;
}

static opk_task_t
line_search_failure(opk_vmlmb_t* opt)
{
  opk_reason_t reason;
  if (opk_lnsrch_has_errors(opt->lnsrch)) {
    reason = OPK_LINE_SEARCH_ERROR;
  } else {
    reason = OPK_LINE_SEARCH_WARNING;
  }
  return optimizer_failure(opt, reason);
}

opk_vmlmb_t*
opk_new_vmlmb_optimizer_with_line_search(opk_vspace_t* space,
                                         opk_index_t m,
                                         opk_lnsrch_t* lnsrch)
{
  opk_vmlmb_t* opt;
  size_t s_offset, y_offset, beta_offset, rho_offset, size;
  opk_index_t k;

  /* Check the input arguments for errors. */
  if (space == NULL || lnsrch == NULL) {
    errno = EFAULT;
    return NULL;
  }
  if (space->size < 1 || m < 1) {
    errno = EINVAL;
    return NULL;
  }
  if (m > space->size) {
    m = space->size;
  }

  /* Allocate enough memory for the workspace and its arrays. */
  s_offset = ROUND_UP(sizeof(opk_vmlmb_t), sizeof(opk_vector_t*));
  y_offset = s_offset + m*sizeof(opk_vector_t*);
  beta_offset = ROUND_UP(y_offset + m*sizeof(opk_vector_t*), sizeof(double));
  rho_offset = beta_offset + m*sizeof(double);
  size = rho_offset + m*sizeof(double);
  opt = (opk_vmlmb_t*)opk_allocate_object(finalize_vmlmb, size);
  if (opt == NULL) {
    return NULL;
  }
  opt->s    = ADDRESS(opk_vector_t*, opt,    s_offset);
  opt->y    = ADDRESS(opk_vector_t*, opt,    y_offset);
  opt->beta = ADDRESS(double,        opt, beta_offset);
  opt->rho  = ADDRESS(double,        opt,  rho_offset);
  opt->m = m;
  for (k = 0; k < m; ++k) {
    opt->s[k] = opk_vcreate(space);
    if (opt->s[k] == NULL) {
      goto error;
    }
    opt->y[k] = opk_vcreate(space);
    if (opt->y[k] == NULL) {
      goto error;
    }
  }
  opt->space = OPK_HOLD_VSPACE(space);
  opt->lnsrch = OPK_HOLD_LNSRCH(lnsrch);
#if ! SAVE_MEMORY
  opt->x0 = opk_vcreate(space);
  if (opt->x0 == NULL) {
    goto error;
  }
  opt->g0 = opk_vcreate(space);
  if (opt->g0 == NULL) {
    goto error;
  }
#endif
  opt->d = opk_vcreate(space);
  if (opt->d == NULL) {
    goto error;
  }
  opt->w = opk_vcreate(space);
  if (opt->w == NULL) {
    goto error;
  }
  opt->grtol = GRTOL;
  opt->gatol = GATOL;
  opt->stpmin = STPMIN;
  opt->stpmax = STPMAX;
  opt->epsilon = EPSILON;
  opt->delta = DELTA;
  opt->gamma = 1.0;
  return opt;

 error:
  OPK_DROP(opt);
  return NULL;
}

opk_vmlmb_t*
opk_new_vmlmb_optimizer(opk_vspace_t* space, opk_index_t m)
{
  opk_lnsrch_t* lnsrch;
  opk_vmlmb_t* opt;

  lnsrch = opk_lnsrch_new_nonmonotone(1, SFTOL, 0.1, 0.9);
  if (lnsrch == NULL) {
    return NULL;
  }
  opt = opk_new_vmlmb_optimizer_with_line_search(space, m, lnsrch);
  OPK_DROP(lnsrch); /* the line search is now owned by the optimizer */
  return opt;
}

static opk_task_t
project_variables(opk_vmlmb_t* opt,
                  opk_vector_t* x,
                  const opk_bound_t* xl,
                  const opk_bound_t* xu)
{
  int status = opk_box_project_variables(x, x, xl, xu);
  if (status != OPK_SUCCESS) {
    return optimizer_failure(opt, OPK_INFEASIBLE_BOUNDS);
  } else {
    return optimizer_success(opt, OPK_TASK_COMPUTE_FG);
  }
}

opk_task_t
opk_start_vmlmb(opk_vmlmb_t* opt, opk_vector_t* x,
                const opk_bound_t* xl, const opk_bound_t* xu)
{
  opt->iterations = 0;
  opt->evaluations = 0;
  opt->restarts = 0;
  opt->updates = 0;
  opt->mp = 0;
  return project_variables(opt, x, xl, xu);
}

/*
 * To guess an optimal step:
 *
 * f(x + alpha*d) = f(x) + alpha*<g(x),d> + (alpha^2/2)*<d,B(x).d>
 * ==> bestalpha = -<g(x),d>/<d,B(x).d>
 * ==> min_alpha f(x + alpha*d) = f(x + bestalpha*d)
 *                              = f(x) - (1/2)*<g(x),d>^2/<d,B(x).d>
 *                              = f(x) - (1/2)*bestalpha*<g(x),d>
 * ==> bestalpha = 2*(f(x) - f(x + bestalpha*d))/<g(x),d>
 *               ~ 2*abs(f(x))/<g(x),d>
 */

#define S(k)     opt->s[k]
#define Y(k)     opt->y[k]
#define BETA(k)  opt->beta[k]
#define RHO(k)   opt->rho[k]
#define UPDATE(dst, alpha, x)            opk_vaxpby(dst, 1, dst, alpha, x)
#define COMBINE(dst, alpha, x, beta, y)  opk_vaxpby(dst, alpha, x, beta, y)
#define COPY(dst, src)                   opk_vcopy(dst, src)
#define SCALE(x, alpha)                  opk_vscale(x, alpha, x)
#define DOT(x, y)                        opk_vdot(x, y)
#define WDOT(x, y)                       opk_vdot3(opt->w, x, y)

opk_task_t
opk_iterate_vmlmb(opk_vmlmb_t* opt, opk_vector_t* x,
                  double f, opk_vector_t* g,
                  const opk_bound_t* xl, const opk_bound_t* xu)
{
  double gtd, yty, sty, alphamax;
  opk_index_t j, k;
  int status;

  switch (opt->task) {

  case OPK_TASK_COMPUTE_FG:

    /* Caller has computed the function value and the gradient at the current
       point. */
    ++opt->evaluations;

    /* Determine the set of free variables (FIXME: should return number of free
       variables, -1 in case of error). */
    status = opk_box_get_free_variables(opt->w, x, xl, xu, g, OPK_ASCENT);
    if (status != OPK_SUCCESS) {
      return optimizer_failure(opt, OPK_INFEASIBLE_BOUNDS);
    }

    /* Check for global convergence. */
    opt->gpnorm = sqrt(WDOT(g, g));
    if (opt->evaluations == 1) {
      opt->ginit = opt->gpnorm;
    }
    if (opt->gpnorm <= max3(0.0, opt->gatol, opt->grtol*opt->ginit)) {
      return optimizer_success(opt, OPK_TASK_FINAL_X);
    }

    if (opt->evaluations > 1) {
      /* A line search is in progress, check whether it has converged. */
      if (opk_lnsrch_use_deriv(opt->lnsrch)) {
        /* Compute effective step and directional derivative. */
        if (opt->tmp == NULL) {
          opt->tmp = opk_vcreate(opt->space);
          if (opt->tmp == NULL) {
            return optimizer_failure(opt, OPK_INSUFICIENT_MEMORY);
          }
        }
        opk_vaxpby(opt->tmp, 1, x, -1, opt->x0);
        gtd = opk_vdot(opt->tmp, g)/opt->alpha;
      } else {
        gtd = 0;
      }
      status = opk_lnsrch_iterate(opt->lnsrch, &opt->alpha, f, gtd);
      if (status == OPK_LNSRCH_SEARCH) {
        /* Line search has not yet converged. */
        goto next_step;
      }
      if (status != OPK_LNSRCH_CONVERGENCE &&
          status != OPK_LNSRCH_WARNING_ROUNDING_ERRORS_PREVENT_PROGRESS) {
        return line_search_failure(opt);
      }
      ++opt->iterations;
    }
    return optimizer_success(opt, OPK_TASK_NEW_X);


  case OPK_TASK_NEW_X:
  case OPK_TASK_FINAL_X:

    if (opt->iterations >= 1) {
      /* Update L-BFGS approximation of the Hessian. */
      k = slot(opt, 0);
      opk_vaxpby(S(k), 1, x, -1, opt->x0);
      opk_vaxpby(Y(k), 1, g, -1, opt->g0);
      ++opt->updates;
      if (opt->mp < opt->m) {
        ++opt->mp;
      }
    }

    /* Compute a search direction.  We take care of checking whether -D is a
       sufficient descent direction.  As shown by Zoutendijk, this is true if:
       cos(theta) = (D/|D|)'.(G/|G|) >= EPSILON > 0 where G is the gradient at
       X. */

    opt->gamma = 0;
    gtd = 0;
    if (opt->mp >= 1) {
      /* Apply the L-BFGS Strang's two-loop recursion to compute a search
         direction. */
      COPY(opt->d, g);
      for (j = 1; j <= opt->mp; ++j) {
        k = slot(opt, j);
        sty = WDOT(Y(k), S(k));
        if (sty <= 0) {
          RHO(k) = 0;
          continue;
        }
        RHO(k) = 1/sty;
        BETA(k) = RHO(k)*WDOT(opt->d, S(k));
        UPDATE(opt->d, -BETA(k), Y(k));
        if (opt->gamma == 0) {
          yty = WDOT(Y(k), Y(k));
          if (yty > 0) {
            opt->gamma = sty/yty;
          }
        }
      }
      if (opt->gamma > 0) {
        /* Apply initial inverse Hessian approximation. */
        if (opt->gamma != 1) {
          SCALE(opt->d, opt->gamma);
        }

        /* Proceed with the second loop of Strang's recursion. */
        for (j = opt->mp; j >= 1; --j) {
          k = slot(opt, j);
          if (RHO(k) > 0) {
            UPDATE(opt->d, BETA(k) - RHO(k)*WDOT(opt->d, Y(k)), S(k));
          }
        }

        /* Enforce search direction to belong to the subset of the free
           variables. */
        opk_vproduct(opt->d, opt->w, opt->d);

        /* Check whether the algorithm has produced a sufficient descent
           direction. */
        gtd = -DOT(opt->d, g);
        if (opt->epsilon > 0 &&
            opt->epsilon*opt->gpnorm*opk_vnorm2(opt->d) > -gtd) {
          /* Set GTD to zero to indicate that D is not a sufficient descent
             direction. */
          gtd = 0;
        } else {
          opt->alpha = 1.0;
        }
      }
    }
    if (gtd >= 0) {
      /* Use (projected) steepest projected ascent. */
      if (opt->mp > 0) {
        /* BFGS recursion did not produce a sufficient descent direction. */
        ++opt->restarts;
        opt->mp = 0;
      }
      opk_vproduct(opt->d, opt->w, g);
      gtd = -opt->gpnorm*opt->gpnorm;
      if (f != 0) {
        opt->alpha = 2*fabs(f/gtd);
      } else {
        double dnorm = opt->gpnorm;
        double xnorm = sqrt(WDOT(x, x));
        if (xnorm > 0) {
          opt->alpha = opt->delta*xnorm/dnorm;
        } else {
          opt->alpha = opt->delta/dnorm;
        }
      }
    }

    /* Shortcut the step length. */
    status = opk_box_get_step_limits(NULL, NULL, &alphamax,
                                     x, xl, xu, opt->d, OPK_ASCENT);
    if (status != OPK_SUCCESS) {
      return optimizer_failure(opt, OPK_STEP_LIMITS_NOT_IMPLEMENTED);
    }
    if (opt->alpha > alphamax) {
      opt->alpha = alphamax;
    }
    if (opt->alpha <= 0) {
      return optimizer_failure(opt, OPK_WOULD_BLOCK);
    }

    /* Save current point. */
#if SAVE_MEMORY
    k = slot(opt, 0);
    opt->x0 = S(k); /* weak reference */
    opt->g0 = Y(k); /* weak reference */
#endif
    opt->f0 = f;
    COPY(opt->x0, x);
    COPY(opt->g0, g);

    /* Start line search. */
    status = opk_lnsrch_start(opt->lnsrch, f, gtd, opt->alpha,
                              opt->stpmin, opt->stpmax);
    if (status != OPK_LNSRCH_SEARCH) {
      return line_search_failure(opt);
    }
    goto next_step;

  default:

    /* There must be something wrong. */
    return opt->task;

  }

 next_step:
  opk_vaxpby(x, 1, opt->x0, -opt->alpha, opt->d);
  return project_variables(opt, x, xl, xu);
}

opk_task_t
opk_get_vmlmb_task(opk_vmlmb_t* opt)
{
  return opt->task;
}

int
opk_get_vmlmb_reason(opk_vmlmb_t* opt)
{
  return opt->reason;
}

opk_index_t
opk_get_vmlmb_iterations(opk_vmlmb_t* opt)
{
  return opt->iterations;
}

opk_index_t
opk_get_vmlmb_evaluations(opk_vmlmb_t* opt)
{
  return opt->evaluations;
}

opk_index_t
opk_get_vmlmb_restarts(opk_vmlmb_t* opt)
{
  return opt->restarts;
}

double
opk_get_vmlmb_gatol(opk_vmlmb_t* opt)
{
  return (opt == NULL ? GATOL : opt->gatol);
}

int
opk_set_vmlmb_gatol(opk_vmlmb_t* opt, double gatol)
{
  if (opt == NULL) {
    errno = EFAULT;
    return OPK_FAILURE;
  }
  if (non_finite(gatol) || gatol < 0.0) {
    errno = EINVAL;
    return OPK_FAILURE;
  }
  opt->gatol = gatol;
  return OPK_SUCCESS;
}

double
opk_get_vmlmb_grtol(opk_vmlmb_t* opt)
{
  return (opt == NULL ? GRTOL : opt->grtol);
}

int
opk_set_vmlmb_grtol(opk_vmlmb_t* opt, double grtol)
{
  if (opt == NULL) {
    errno = EFAULT;
    return OPK_FAILURE;
  }
  if (non_finite(grtol) || grtol < 0.0) {
    errno = EINVAL;
    return OPK_FAILURE;
  }
  opt->grtol = grtol;
  return OPK_SUCCESS;
}

double
opk_get_vmlmb_step(opk_vmlmb_t* opt)
{
  return (opt == NULL ? -1.0 : opt->alpha);
}

double
opk_get_vmlmb_stpmin(opk_vmlmb_t* opt)
{
  return (opt == NULL ? STPMIN : opt->stpmin);
}

double
opk_get_vmlmb_stpmax(opk_vmlmb_t* opt)
{
  return (opt == NULL ? STPMAX : opt->stpmax);
}

int
opk_set_vmlmb_stpmin_and_stpmax(opk_vmlmb_t* opt,
                                double stpmin, double stpmax)
{
  if (opt == NULL) {
    errno = EFAULT;
    return OPK_FAILURE;
  }
  if (non_finite(stpmin) || non_finite(stpmax) ||
      stpmin < 0.0 || stpmax <= stpmin) {
    errno = EINVAL;
    return OPK_FAILURE;
  }
  opt->stpmin = stpmin;
  opt->stpmax = stpmax;
  return OPK_SUCCESS;
}

/*
 * Local Variables:
 * mode: C
 * tab-width: 8
 * c-basic-offset: 2
 * indent-tabs-mode: nil
 * fill-column: 79
 * coding: utf-8
 * ispell-local-dictionary: "american"
 * End:
 */
