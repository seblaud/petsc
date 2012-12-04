
#include <../src/snes/impls/ls/lsimpl.h>

/*
     Checks if J^T F = 0 which implies we've found a local minimum of the norm of the function,
    || F(u) ||_2 but not a zero, F(u) = 0. In the case when one cannot compute J^T F we use the fact that
    0 = (J^T F)^T W = F^T J W iff W not in the null space of J. Thanks for Jorge More
    for this trick. One assumes that the probability that W is in the null space of J is very, very small.
*/
#undef __FUNCT__
#define __FUNCT__ "SNESNEWTONLSCheckLocalMin_Private"
PetscErrorCode SNESNEWTONLSCheckLocalMin_Private(SNES snes,Mat A,Vec F,Vec W,PetscReal fnorm,PetscBool  *ismin)
{
  PetscReal      a1;
  PetscErrorCode ierr;
  PetscBool      hastranspose;

  PetscFunctionBegin;
  *ismin = PETSC_FALSE;
  ierr = MatHasOperation(A,MATOP_MULT_TRANSPOSE,&hastranspose);CHKERRQ(ierr);
  if (hastranspose) {
    /* Compute || J^T F|| */
    ierr = MatMultTranspose(A,F,W);CHKERRQ(ierr);
    ierr = VecNorm(W,NORM_2,&a1);CHKERRQ(ierr);
    ierr = PetscInfo1(snes,"|| J^T F|| %14.12e near zero implies found a local minimum\n",(double)(a1/fnorm));CHKERRQ(ierr);
    if (a1/fnorm < 1.e-4) *ismin = PETSC_TRUE;
  } else {
    Vec         work;
    PetscScalar result;
    PetscReal   wnorm;

    ierr = VecSetRandom(W,PETSC_NULL);CHKERRQ(ierr);
    ierr = VecNorm(W,NORM_2,&wnorm);CHKERRQ(ierr);
    ierr = VecDuplicate(W,&work);CHKERRQ(ierr);
    ierr = MatMult(A,W,work);CHKERRQ(ierr);
    ierr = VecDot(F,work,&result);CHKERRQ(ierr);
    ierr = VecDestroy(&work);CHKERRQ(ierr);
    a1   = PetscAbsScalar(result)/(fnorm*wnorm);
    ierr = PetscInfo1(snes,"(F^T J random)/(|| F ||*||random|| %14.12e near zero implies found a local minimum\n",(double)a1);CHKERRQ(ierr);
    if (a1 < 1.e-4) *ismin = PETSC_TRUE;
  }
  PetscFunctionReturn(0);
}

/*
     Checks if J^T(F - J*X) = 0
*/
#undef __FUNCT__
#define __FUNCT__ "SNESNEWTONLSCheckResidual_Private"
PetscErrorCode SNESNEWTONLSCheckResidual_Private(SNES snes,Mat A,Vec F,Vec X,Vec W1,Vec W2)
{
  PetscReal      a1,a2;
  PetscErrorCode ierr;
  PetscBool      hastranspose;

  PetscFunctionBegin;
  ierr = MatHasOperation(A,MATOP_MULT_TRANSPOSE,&hastranspose);CHKERRQ(ierr);
  if (hastranspose) {
    ierr = MatMult(A,X,W1);CHKERRQ(ierr);
    ierr = VecAXPY(W1,-1.0,F);CHKERRQ(ierr);

    /* Compute || J^T W|| */
    ierr = MatMultTranspose(A,W1,W2);CHKERRQ(ierr);
    ierr = VecNorm(W1,NORM_2,&a1);CHKERRQ(ierr);
    ierr = VecNorm(W2,NORM_2,&a2);CHKERRQ(ierr);
    if (a1 != 0.0) {
      ierr = PetscInfo1(snes,"||J^T(F-Ax)||/||F-AX|| %14.12e near zero implies inconsistent rhs\n",(double)(a2/a1));CHKERRQ(ierr);
    }
  }
  PetscFunctionReturn(0);
}

/*  --------------------------------------------------------------------

     This file implements a truncated Newton method with a line search,
     for solving a system of nonlinear equations, using the KSP, Vec,
     and Mat interfaces for linear solvers, vectors, and matrices,
     respectively.

     The following basic routines are required for each nonlinear solver:
          SNESCreate_XXX()          - Creates a nonlinear solver context
          SNESSetFromOptions_XXX()  - Sets runtime options
          SNESSolve_XXX()           - Solves the nonlinear system
          SNESDestroy_XXX()         - Destroys the nonlinear solver context
     The suffix "_XXX" denotes a particular implementation, in this case
     we use _NEWTONLS (e.g., SNESCreate_NEWTONLS, SNESSolve_NEWTONLS) for solving
     systems of nonlinear equations with a line search (LS) method.
     These routines are actually called via the common user interface
     routines SNESCreate(), SNESSetFromOptions(), SNESSolve(), and
     SNESDestroy(), so the application code interface remains identical
     for all nonlinear solvers.

     Another key routine is:
          SNESSetUp_XXX()           - Prepares for the use of a nonlinear solver
     by setting data structures and options.   The interface routine SNESSetUp()
     is not usually called directly by the user, but instead is called by
     SNESSolve() if necessary.

     Additional basic routines are:
          SNESView_XXX()            - Prints details of runtime options that
                                      have actually been used.
     These are called by application codes via the interface routines
     SNESView().

     The various types of solvers (preconditioners, Krylov subspace methods,
     nonlinear solvers, timesteppers) are all organized similarly, so the
     above description applies to these categories also.

    -------------------------------------------------------------------- */
/*
   SNESSolve_NEWTONLS - Solves a nonlinear system with a truncated Newton
   method with a line search.

   Input Parameters:
.  snes - the SNES context

   Output Parameter:
.  outits - number of iterations until termination

   Application Interface Routine: SNESSolve()

   Notes:
   This implements essentially a truncated Newton method with a
   line search.  By default a cubic backtracking line search
   is employed, as described in the text "Numerical Methods for
   Unconstrained Optimization and Nonlinear Equations" by Dennis
   and Schnabel.
*/
#undef __FUNCT__
#define __FUNCT__ "SNESSolve_NEWTONLS"
PetscErrorCode SNESSolve_NEWTONLS(SNES snes)
{
  PetscErrorCode      ierr;
  PetscInt            maxits,i,lits;
  PetscBool           lssucceed;
  MatStructure        flg = DIFFERENT_NONZERO_PATTERN;
  PetscReal           fnorm,gnorm,xnorm,ynorm;
  Vec                 Y,X,F,G,W,FPC;
  KSPConvergedReason  kspreason;
  PetscBool           domainerror;
  SNESLineSearch      linesearch;
  SNESConvergedReason reason;

  PetscFunctionBegin;
  snes->numFailures            = 0;
  snes->numLinearSolveFailures = 0;
  snes->reason                 = SNES_CONVERGED_ITERATING;

  maxits	= snes->max_its;	/* maximum number of iterations */
  X		= snes->vec_sol;	/* solution vector */
  F		= snes->vec_func;	/* residual vector */
  Y		= snes->vec_sol_update; /* newton step */
  G		= snes->work[0];
  W		= snes->work[1];

  ierr = PetscObjectTakeAccess(snes);CHKERRQ(ierr);
  snes->iter = 0;
  snes->norm = 0.0;
  ierr = PetscObjectGrantAccess(snes);CHKERRQ(ierr);
  ierr = SNESGetSNESLineSearch(snes, &linesearch);CHKERRQ(ierr);
  if (!snes->vec_func_init_set) {
    ierr = SNESComputeFunction(snes,X,F);CHKERRQ(ierr);
    ierr = SNESGetFunctionDomainError(snes, &domainerror);CHKERRQ(ierr);
    if (domainerror) {
      snes->reason = SNES_DIVERGED_FUNCTION_DOMAIN;
      PetscFunctionReturn(0);
    }
  } else {
    snes->vec_func_init_set = PETSC_FALSE;
  }
  if (!snes->norm_init_set) {
    ierr = VecNormBegin(F,NORM_2,&fnorm);CHKERRQ(ierr);	/* fnorm <- ||F||  */
    ierr = VecNormEnd(F,NORM_2,&fnorm);CHKERRQ(ierr);
    if (PetscIsInfOrNanReal(fnorm)) SETERRQ(PETSC_COMM_SELF,PETSC_ERR_FP,"User provided compute function generated a Not-a-Number");
  } else {
    fnorm = snes->norm_init;
    snes->norm_init_set = PETSC_FALSE;
  }
  ierr = PetscObjectTakeAccess(snes);CHKERRQ(ierr);
  snes->norm = fnorm;
  ierr = PetscObjectGrantAccess(snes);CHKERRQ(ierr);
  SNESLogConvHistory(snes,fnorm,0);
  ierr = SNESMonitor(snes,0,fnorm);CHKERRQ(ierr);

  /* set parameter for default relative tolerance convergence test */
  snes->ttol = fnorm*snes->rtol;
  /* test convergence */
  ierr = (*snes->ops->converged)(snes,0,0.0,0.0,fnorm,&snes->reason,snes->cnvP);CHKERRQ(ierr);
  if (snes->reason) PetscFunctionReturn(0);

  for (i=0; i<maxits; i++) {

    /* Call general purpose update function */
    if (snes->ops->update) {
      ierr = (*snes->ops->update)(snes, snes->iter);CHKERRQ(ierr);
    }

    /* apply the nonlinear preconditioner if it's right preconditioned */
    if (snes->pc && snes->pcside == PC_RIGHT) {
      //ierr = SNESSetInitialFunction(snes->pc, F);CHKERRQ(ierr);
      //ierr = SNESSetInitialFunctionNorm(snes->pc, fnorm);CHKERRQ(ierr);
      ierr = SNESSolve(snes->pc, snes->vec_rhs, X);CHKERRQ(ierr);
      ierr = SNESGetConvergedReason(snes->pc,&reason);CHKERRQ(ierr);
      if (reason < 0  && reason != SNES_DIVERGED_MAX_IT) {
        snes->reason = SNES_DIVERGED_INNER;
        PetscFunctionReturn(0);
      }
      ierr = SNESGetFunction(snes->pc, &FPC, PETSC_NULL, PETSC_NULL);CHKERRQ(ierr);
      ierr = VecCopy(FPC, F);CHKERRQ(ierr);
      ierr = SNESGetFunctionNorm(snes->pc, &fnorm);CHKERRQ(ierr);
    }

    /* Solve J Y = F, where J is Jacobian matrix */
    ierr = SNESComputeJacobian(snes,X,&snes->jacobian,&snes->jacobian_pre,&flg);CHKERRQ(ierr);
    ierr = KSPSetOperators(snes->ksp,snes->jacobian,snes->jacobian_pre,flg);CHKERRQ(ierr);
    ierr = SNES_KSPSolve(snes,snes->ksp,F,Y);CHKERRQ(ierr);
    ierr = KSPGetConvergedReason(snes->ksp,&kspreason);CHKERRQ(ierr);
    if (kspreason < 0) {
      if (++snes->numLinearSolveFailures >= snes->maxLinearSolveFailures) {
        ierr = PetscInfo2(snes,"iter=%D, number linear solve failures %D greater than current SNES allowed, stopping solve\n",snes->iter,snes->numLinearSolveFailures);CHKERRQ(ierr);
        snes->reason = SNES_DIVERGED_LINEAR_SOLVE;
        break;
      }
    }
    ierr = KSPGetIterationNumber(snes->ksp,&lits);CHKERRQ(ierr);
    snes->linear_its += lits;
    ierr = PetscInfo2(snes,"iter=%D, linear solve iterations=%D\n",snes->iter,lits);CHKERRQ(ierr);

    if (PetscLogPrintInfo){
      ierr = SNESNEWTONLSCheckResidual_Private(snes,snes->jacobian,F,Y,G,W);CHKERRQ(ierr);
    }

    /* Compute a (scaled) negative update in the line search routine:
         X <- X - lambda*Y
       and evaluate F = function(X) (depends on the line search).
    */
    gnorm = fnorm;
    ierr = SNESLineSearchApply(linesearch, X, F, &fnorm, Y);CHKERRQ(ierr);
    ierr = SNESLineSearchGetSuccess(linesearch, &lssucceed);CHKERRQ(ierr);
    ierr = SNESLineSearchGetNorms(linesearch, &xnorm, &fnorm, &ynorm);CHKERRQ(ierr);
    ierr = PetscInfo4(snes,"fnorm=%18.16e, gnorm=%18.16e, ynorm=%18.16e, lssucceed=%d\n",(double)gnorm,(double)fnorm,(double)ynorm,(int)lssucceed);CHKERRQ(ierr);
    if (snes->reason == SNES_DIVERGED_FUNCTION_COUNT) break;
    ierr = SNESGetFunctionDomainError(snes, &domainerror);CHKERRQ(ierr);
    if (domainerror) {
      snes->reason = SNES_DIVERGED_FUNCTION_DOMAIN;
      PetscFunctionReturn(0);
    }
    if (!lssucceed) {
      if (snes->stol*xnorm > ynorm) {
        snes->reason = SNES_CONVERGED_SNORM_RELATIVE;
        PetscFunctionReturn(0);
      }
      if (++snes->numFailures >= snes->maxFailures) {
        PetscBool  ismin;
        snes->reason = SNES_DIVERGED_LINE_SEARCH;
        ierr = SNESNEWTONLSCheckLocalMin_Private(snes,snes->jacobian,F,W,fnorm,&ismin);CHKERRQ(ierr);
        if (ismin) snes->reason = SNES_DIVERGED_LOCAL_MIN;
        break;
      }
    }
    /* Monitor convergence */
    ierr = PetscObjectTakeAccess(snes);CHKERRQ(ierr);
    snes->iter = i+1;
    snes->norm = fnorm;
    ierr = PetscObjectGrantAccess(snes);CHKERRQ(ierr);
    SNESLogConvHistory(snes,snes->norm,lits);
    ierr = SNESMonitor(snes,snes->iter,snes->norm);CHKERRQ(ierr);
    /* Test for convergence */
    ierr = (*snes->ops->converged)(snes,snes->iter,xnorm,ynorm,fnorm,&snes->reason,snes->cnvP);CHKERRQ(ierr);
    if (snes->reason) break;
  }
  if (i == maxits) {
    ierr = PetscInfo1(snes,"Maximum number of iterations has been reached: %D\n",maxits);CHKERRQ(ierr);
    if (!snes->reason) snes->reason = SNES_DIVERGED_MAX_IT;
  }
  PetscFunctionReturn(0);
}
/* -------------------------------------------------------------------------- */
/*
   SNESSetUp_NEWTONLS - Sets up the internal data structures for the later use
   of the SNESNEWTONLS nonlinear solver.

   Input Parameter:
.  snes - the SNES context
.  x - the solution vector

   Application Interface Routine: SNESSetUp()

   Notes:
   For basic use of the SNES solvers, the user need not explicitly call
   SNESSetUp(), since these actions will automatically occur during
   the call to SNESSolve().
 */
#undef __FUNCT__
#define __FUNCT__ "SNESSetUp_NEWTONLS"
PetscErrorCode SNESSetUp_NEWTONLS(SNES snes)
{
  PetscErrorCode ierr;

  PetscFunctionBegin;
  ierr = SNESDefaultGetWork(snes,2);CHKERRQ(ierr);
  ierr = SNESSetUpMatrices(snes);CHKERRQ(ierr);

  PetscFunctionReturn(0);
}
/* -------------------------------------------------------------------------- */

#undef __FUNCT__
#define __FUNCT__ "SNESReset_NEWTONLS"
PetscErrorCode SNESReset_NEWTONLS(SNES snes)
{
  PetscFunctionBegin;
  PetscFunctionReturn(0);
}

/*
   SNESDestroy_NEWTONLS - Destroys the private SNES_NEWTONLS context that was created
   with SNESCreate_NEWTONLS().

   Input Parameter:
.  snes - the SNES context

   Application Interface Routine: SNESDestroy()
 */
#undef __FUNCT__
#define __FUNCT__ "SNESDestroy_NEWTONLS"
PetscErrorCode SNESDestroy_NEWTONLS(SNES snes)
{
  PetscErrorCode ierr;

  PetscFunctionBegin;
  ierr = SNESReset_NEWTONLS(snes);CHKERRQ(ierr);
  ierr = PetscFree(snes->data);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}
/* -------------------------------------------------------------------------- */

/*
   SNESView_NEWTONLS - Prints info from the SNESNEWTONLS data structure.

   Input Parameters:
.  SNES - the SNES context
.  viewer - visualization context

   Application Interface Routine: SNESView()
*/
#undef __FUNCT__
#define __FUNCT__ "SNESView_NEWTONLS"
static PetscErrorCode SNESView_NEWTONLS(SNES snes,PetscViewer viewer)
{
  PetscErrorCode ierr;
  PetscBool      iascii;

  PetscFunctionBegin;
  ierr = PetscObjectTypeCompare((PetscObject)viewer,PETSCVIEWERASCII,&iascii);CHKERRQ(ierr);
  if (iascii) {
  }
  PetscFunctionReturn(0);
}

/* -------------------------------------------------------------------------- */
/*
   SNESSetFromOptions_NEWTONLS - Sets various parameters for the SNESNEWTONLS method.

   Input Parameter:
.  snes - the SNES context

   Application Interface Routine: SNESSetFromOptions()
*/
#undef __FUNCT__
#define __FUNCT__ "SNESSetFromOptions_NEWTONLS"
static PetscErrorCode SNESSetFromOptions_NEWTONLS(SNES snes)
{
  PetscErrorCode ierr;
  SNESLineSearch linesearch;

  PetscFunctionBegin;
  ierr = PetscOptionsHead("SNESNEWTONLS options");CHKERRQ(ierr);
  ierr = PetscOptionsTail();CHKERRQ(ierr);
  /* set the default line search type */
  if (!snes->linesearch) {
    ierr = SNESGetSNESLineSearch(snes, &linesearch);CHKERRQ(ierr);
    ierr = SNESLineSearchSetType(linesearch, SNESLINESEARCHBT);CHKERRQ(ierr);
  }
  PetscFunctionReturn(0);
}

/* -------------------------------------------------------------------------- */
/*MC
      SNESNEWTONLS - Newton based nonlinear solver that uses a line search

   Options Database:
+   -snes_linesearch_type <bt> - bt,basic.  Select line search type
.   -snes_linesearch_order <3> - 2, 3. Selects the order of the line search for bt
.   -snes_linesearch_norms <true> - Turns on/off computation of the norms for basic linesearch
.   -snes_linesearch_alpha <alpha> - Sets alpha used in determining if reduction in function norm is sufficient
.   -snes_linesearch_maxstep <maxstep> - Sets the maximum stepsize the line search will use (if the 2-norm(y) > maxstep then scale y to be y = (maxstep/2-norm(y)) *y)
.   -snes_linesearch_minlambda <minlambda>  - Sets the minimum lambda the line search will tolerate
.   -snes_linesearch_monitor - print information about progress of line searches
-   -snes_linesearch_damping - damping factor used for basic line search

    Notes: This is the default nonlinear solver in SNES

   Level: beginner

.seealso:  SNESCreate(), SNES, SNESSetType(), SNESNEWTONTR, SNESQN, SNESLineSearchSetType(), SNESLineSearchSetOrder()
           SNESLineSearchSetPostCheck(), SNESLineSearchSetPreCheck() SNESLineSearchSetComputeNorms()

M*/
EXTERN_C_BEGIN
#undef __FUNCT__
#define __FUNCT__ "SNESCreate_NEWTONLS"
PetscErrorCode  SNESCreate_NEWTONLS(SNES snes)
{
  PetscErrorCode ierr;
  SNES_NEWTONLS  *neP;

  PetscFunctionBegin;
  snes->ops->setup           = SNESSetUp_NEWTONLS;
  snes->ops->solve           = SNESSolve_NEWTONLS;
  snes->ops->destroy         = SNESDestroy_NEWTONLS;
  snes->ops->setfromoptions  = SNESSetFromOptions_NEWTONLS;
  snes->ops->view            = SNESView_NEWTONLS;
  snes->ops->reset           = SNESReset_NEWTONLS;

  snes->usesksp                      = PETSC_TRUE;
  snes->usespc                       = PETSC_FALSE;
  ierr                               = PetscNewLog(snes,SNES_NEWTONLS,&neP);CHKERRQ(ierr);
  snes->data                         = (void*)neP;
  PetscFunctionReturn(0);
}
EXTERN_C_END
