/*---------------------------------------------------------------
 * Programmer(s): Daniel R. Reynolds @ SMU
 *---------------------------------------------------------------
 * SUNDIALS Copyright Start
 * Copyright (c) 2002-2025, Lawrence Livermore National Security
 * and Southern Methodist University.
 * All rights reserved.
 *
 * See the top-level LICENSE and NOTICE files for details.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * SUNDIALS Copyright End
 *---------------------------------------------------------------
 * Implementation file for ARKODE's linear solver interface.
 *---------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sundials/sundials_math.h>
#include <sunmatrix/sunmatrix_band.h>
#include <sunmatrix/sunmatrix_dense.h>
#include <sunmatrix/sunmatrix_sparse.h>

#include "arkode_impl.h"
#include "arkode_ls_impl.h"

/* constants */
#define MIN_INC_MULT SUN_RCONST(1000.0)
#define MAX_DQITERS  3 /* max. # of attempts to recover in DQ J*v */
#define ZERO         SUN_RCONST(0.0)
#define PT25         SUN_RCONST(0.25)
#define ONE          SUN_RCONST(1.0)

/* Prototypes for internal functions */
static int arkLsLinSys(sunrealtype t, N_Vector y, N_Vector fy, SUNMatrix A,
                       SUNMatrix M, sunbooleantype jok, sunbooleantype* jcur,
                       sunrealtype gamma, void* arkode_mem, N_Vector tmp1,
                       N_Vector tmp2, N_Vector tmp3);

/*===============================================================
  Exported routines
  ===============================================================*/

/*---------------------------------------------------------------
  ARKodeSetLinearSolver specifies the linear solver.
  ---------------------------------------------------------------*/
int ARKodeSetLinearSolver(void* arkode_mem, SUNLinearSolver LS, SUNMatrix A)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;
  SUNLinearSolver_Type LSType;
  sunbooleantype iterative;   /* is the solver iterative?    */
  sunbooleantype matrixbased; /* is a matrix structure used? */

  /* Return immediately if either arkode_mem or LS inputs are NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Guard against use for time steppers that do not need an algebraic solver */
  if (!ark_mem->step_supports_implicit)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not require an algebraic solver");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  if (LS == NULL)
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "LS must be non-NULL");
    return (ARKLS_ILL_INPUT);
  }

  /* Test if solver is compatible with LS interface */
  if ((LS->ops->gettype == NULL) || (LS->ops->solve == NULL))
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "LS object is missing a required operation");
    return (ARKLS_ILL_INPUT);
  }

  /* Retrieve the LS type */
  LSType = SUNLinSolGetType(LS);

  /* Set flags based on LS type */
  iterative   = (LSType != SUNLINEARSOLVER_DIRECT);
  matrixbased = ((LSType != SUNLINEARSOLVER_ITERATIVE) &&
                 (LSType != SUNLINEARSOLVER_MATRIX_EMBEDDED));

  /* Test if vector is compatible with LS interface */
  if ((ark_mem->tempv1->ops->nvconst == NULL) ||
      (ark_mem->tempv1->ops->nvwrmsnorm == NULL))
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    MSG_LS_BAD_NVECTOR);
    return (ARKLS_ILL_INPUT);
  }

  /* Ensure that A is NULL when LS is matrix-embedded */
  if ((LSType == SUNLINEARSOLVER_MATRIX_EMBEDDED) && (A != NULL))
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__,
                    __FILE__, "Incompatible inputs: matrix-embedded LS requires NULL matrix");
    return (ARKLS_ILL_INPUT);
  }

  /* Check for compatible LS type, matrix and "atimes" support */
  if (iterative)
  {
    if (ark_mem->tempv1->ops->nvgetlength == NULL)
    {
      arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                      MSG_LS_BAD_NVECTOR);
      return (ARKLS_ILL_INPUT);
    }

    if (!matrixbased && (LSType != SUNLINEARSOLVER_MATRIX_EMBEDDED) &&
        (LS->ops->setatimes == NULL))
    {
      arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__,
                      __FILE__, "Incompatible inputs: iterative LS must support ATimes routine");
      return (ARKLS_ILL_INPUT);
    }

    if (matrixbased && (A == NULL))
    {
      arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                      "Incompatible inputs: matrix-iterative LS requires "
                      "non-NULL matrix");
      return (ARKLS_ILL_INPUT);
    }
  }
  else if (A == NULL)
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "Incompatible inputs: direct LS requires non-NULL matrix");
    return (ARKLS_ILL_INPUT);
  }

  /* Test whether time stepper module is supplied, with required routines */
  if ((ark_mem->step_attachlinsol == NULL) || (ark_mem->step_getlinmem == NULL) ||
      (ark_mem->step_getimplicitrhs == NULL) || (ark_mem->step_getgammas == NULL))
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "Missing time step module or associated routines");
    return (ARKLS_ILL_INPUT);
  }

  /* Allocate memory for ARKLsMemRec */
  arkls_mem = NULL;
  arkls_mem = (ARKLsMem)malloc(sizeof(struct ARKLsMemRec));
  if (arkls_mem == NULL)
  {
    arkProcessError(ark_mem, ARKLS_MEM_FAIL, __LINE__, __func__, __FILE__,
                    MSG_LS_MEM_FAIL);
    return (ARKLS_MEM_FAIL);
  }
  memset(arkls_mem, 0, sizeof(struct ARKLsMemRec));

  /* set SUNLinearSolver pointer */
  arkls_mem->LS = LS;

  /* Linear solver type information */
  arkls_mem->iterative   = iterative;
  arkls_mem->matrixbased = matrixbased;

  /* Set defaults for Jacobian-related fields */
  if (A != NULL)
  {
    arkls_mem->jacDQ  = SUNTRUE;
    arkls_mem->jac    = arkLsDQJac;
    arkls_mem->J_data = ark_mem;
  }
  else
  {
    arkls_mem->jacDQ  = SUNFALSE;
    arkls_mem->jac    = NULL;
    arkls_mem->J_data = NULL;
  }

  arkls_mem->jtimesDQ = SUNTRUE;
  arkls_mem->jtsetup  = NULL;
  arkls_mem->jtimes   = arkLsDQJtimes;
  arkls_mem->Jt_data  = ark_mem;
  arkls_mem->Jt_f     = ark_mem->step_getimplicitrhs(ark_mem);

  if (arkls_mem->Jt_f == NULL)
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "Time step module is missing implicit RHS fcn");
    free(arkls_mem);
    arkls_mem = NULL;
    return (ARKLS_ILL_INPUT);
  }

  arkls_mem->user_linsys = SUNFALSE;
  arkls_mem->linsys      = arkLsLinSys;
  arkls_mem->A_data      = ark_mem;

  /* Set defaults for preconditioner-related fields */
  arkls_mem->pset   = NULL;
  arkls_mem->psolve = NULL;
  arkls_mem->pfree  = NULL;
  arkls_mem->P_data = ark_mem->user_data;

  /* Initialize counters */
  arkLsInitializeCounters(arkls_mem);

  /* Set default values for the rest of the LS parameters */
  arkls_mem->msbj      = ARKLS_MSBJ;
  arkls_mem->jbad      = SUNTRUE;
  arkls_mem->eplifac   = ARKLS_EPLIN;
  arkls_mem->last_flag = ARKLS_SUCCESS;

  /* If LS supports ATimes, attach ARKLs routine */
  if (LS->ops->setatimes)
  {
    retval = SUNLinSolSetATimes(LS, ark_mem, arkLsATimes);
    if (retval != SUN_SUCCESS)
    {
      arkProcessError(ark_mem, ARKLS_SUNLS_FAIL, __LINE__, __func__, __FILE__,
                      "Error in calling SUNLinSolSetATimes");
      free(arkls_mem);
      arkls_mem = NULL;
      return (ARKLS_SUNLS_FAIL);
    }
  }

  /* If LS supports preconditioning, initialize pset/psol to NULL */
  if (LS->ops->setpreconditioner)
  {
    retval = SUNLinSolSetPreconditioner(LS, ark_mem, NULL, NULL);
    if (retval != SUN_SUCCESS)
    {
      arkProcessError(ark_mem, ARKLS_SUNLS_FAIL, __LINE__, __func__, __FILE__,
                      "Error in calling SUNLinSolSetPreconditioner");
      free(arkls_mem);
      arkls_mem = NULL;
      return (ARKLS_SUNLS_FAIL);
    }
  }

  /* When using a SUNMatrix object, store pointer to A and initialize savedJ */
  if (A != NULL)
  {
    arkls_mem->A      = A;
    arkls_mem->savedJ = NULL; /* allocated in arkLsInitialize */
  }

  /* Allocate memory for ytemp and x */
  if (!arkAllocVec(ark_mem, ark_mem->tempv1, &(arkls_mem->ytemp)))
  {
    arkProcessError(ark_mem, ARKLS_MEM_FAIL, __LINE__, __func__, __FILE__,
                    MSG_LS_MEM_FAIL);
    free(arkls_mem);
    arkls_mem = NULL;
    return (ARKLS_MEM_FAIL);
  }

  if (!arkAllocVec(ark_mem, ark_mem->tempv1, &(arkls_mem->x)))
  {
    arkProcessError(ark_mem, ARKLS_MEM_FAIL, __LINE__, __func__, __FILE__,
                    MSG_LS_MEM_FAIL);
    arkFreeVec(ark_mem, &(arkls_mem->ytemp));
    free(arkls_mem);
    arkls_mem = NULL;
    return (ARKLS_MEM_FAIL);
  }

  /* For iterative LS, compute default norm conversion factor */
  if (iterative)
  {
    arkls_mem->nrmfac = SUNRsqrt(N_VGetLength(arkls_mem->ytemp));
  }

  /* For matrix-based LS, enable solution scaling */
  if (matrixbased) { arkls_mem->scalesol = SUNTRUE; }
  else { arkls_mem->scalesol = SUNFALSE; }

  /* Attach ARKLs interface to time stepper module */
  retval = ark_mem->step_attachlinsol(ark_mem, arkLsInitialize, arkLsSetup,
                                      arkLsSolve, arkLsFree, LSType, arkls_mem);
  if (retval != ARK_SUCCESS)
  {
    arkProcessError(ark_mem, retval, __LINE__, __func__, __FILE__,
                    "Failed to attach to time stepper module");
    N_VDestroy(arkls_mem->x);
    N_VDestroy(arkls_mem->ytemp);
    free(arkls_mem);
    arkls_mem = NULL;
    return (retval);
  }

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeSetMassLinearSolver specifies the iterative mass-matrix
  linear solver and user-supplied routine to perform the
  mass-matrix-vector product.
  ---------------------------------------------------------------*/
int ARKodeSetMassLinearSolver(void* arkode_mem, SUNLinearSolver LS, SUNMatrix M,
                              sunbooleantype time_dep)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;
  SUNLinearSolver_Type LSType;
  sunbooleantype iterative;   /* is the solver iterative?    */
  sunbooleantype matrixbased; /* is a matrix structure used? */

  /* Return immediately if either arkode_mem or LS inputs are NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Guard against use for time steppers that do not support mass matrices */
  if (!ark_mem->step_supports_massmatrix)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not support non-identity mass matrices");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  if (LS == NULL)
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "LS must be non-NULL");
    return (ARKLS_ILL_INPUT);
  }

  /* Test if solver is compatible with LS interface */
  if ((LS->ops->gettype == NULL) || (LS->ops->solve == NULL))
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "LS object is missing a required operation");
    return (ARKLS_ILL_INPUT);
  }

  /* Retrieve the LS type */
  LSType = SUNLinSolGetType(LS);

  /* Set flags based on LS type */
  iterative   = (LSType != SUNLINEARSOLVER_DIRECT);
  matrixbased = ((LSType != SUNLINEARSOLVER_ITERATIVE) &&
                 (LSType != SUNLINEARSOLVER_MATRIX_EMBEDDED));

  /* Test if vector is compatible with LS interface */
  if ((ark_mem->tempv1->ops->nvconst == NULL) ||
      (ark_mem->tempv1->ops->nvwrmsnorm == NULL))
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    MSG_LS_BAD_NVECTOR);
    return (ARKLS_ILL_INPUT);
  }

  /* Ensure that M is NULL when LS is matrix-embedded */
  if ((LSType == SUNLINEARSOLVER_MATRIX_EMBEDDED) && (M != NULL))
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__,
                    __FILE__, "Incompatible inputs: matrix-embedded LS requires NULL matrix");
    return (ARKLS_ILL_INPUT);
  }

  /* Check for compatible LS type, matrix and "atimes" support */
  if (iterative)
  {
    if (ark_mem->tempv1->ops->nvgetlength == NULL)
    {
      arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                      MSG_LS_BAD_NVECTOR);
      return (ARKLS_ILL_INPUT);
    }

    if (!matrixbased && (LSType != SUNLINEARSOLVER_MATRIX_EMBEDDED) &&
        (LS->ops->setatimes == NULL))
    {
      arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__,
                      __FILE__, "Incompatible inputs: iterative LS must support ATimes routine");
      return (ARKLS_ILL_INPUT);
    }

    if (matrixbased && (M == NULL))
    {
      arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                      "Incompatible inputs: matrix-iterative LS requires "
                      "non-NULL matrix");
      return (ARKLS_ILL_INPUT);
    }
  }
  else if (M == NULL)
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "Incompatible inputs: direct LS requires non-NULL matrix");
    return (ARKLS_ILL_INPUT);
  }

  /* Test whether time stepper module is supplied, with required routines */
  if ((ark_mem->step_attachmasssol == NULL) || (ark_mem->step_getmassmem == NULL))
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "Missing time step module or associated routines");
    return (ARKLS_ILL_INPUT);
  }

  /* Allocate memory for ARKLsMemRec */
  arkls_mem = NULL;
  arkls_mem = (ARKLsMassMem)malloc(sizeof(struct ARKLsMassMemRec));
  if (arkls_mem == NULL)
  {
    arkProcessError(ark_mem, ARKLS_MEM_FAIL, __LINE__, __func__, __FILE__,
                    MSG_LS_MEM_FAIL);
    return (ARKLS_MEM_FAIL);
  }
  memset(arkls_mem, 0, sizeof(struct ARKLsMassMemRec));

  /* set SUNLinearSolver pointer */
  arkls_mem->LS = LS;

  /* Linear solver type information */
  arkls_mem->iterative   = iterative;
  arkls_mem->matrixbased = matrixbased;

  /* Set flag indicating time-dependence */
  arkls_mem->time_dependent = time_dep;

  /* Set mass-matrix routines to NULL */
  arkls_mem->mass    = NULL;
  arkls_mem->M_data  = NULL;
  arkls_mem->mtsetup = NULL;
  arkls_mem->mtimes  = NULL;
  arkls_mem->mt_data = NULL;

  /* Set defaults for preconditioner-related fields */
  arkls_mem->pset   = NULL;
  arkls_mem->psolve = NULL;
  arkls_mem->pfree  = NULL;
  arkls_mem->P_data = ark_mem->user_data;

  /* Initialize counters */
  arkLsInitializeMassCounters(arkls_mem);

  /* Set default values for the rest of the LS parameters */
  arkls_mem->eplifac   = ARKLS_EPLIN;
  arkls_mem->last_flag = ARKLS_SUCCESS;

  /* If LS supports ATimes, attach ARKLs routine */
  if (LS->ops->setatimes)
  {
    retval = SUNLinSolSetATimes(LS, ark_mem, NULL);
    if (retval != SUN_SUCCESS)
    {
      arkProcessError(ark_mem, ARKLS_SUNLS_FAIL, __LINE__, __func__, __FILE__,
                      "Error in calling SUNLinSolSetATimes");
      free(arkls_mem);
      arkls_mem = NULL;
      return (ARKLS_SUNLS_FAIL);
    }
  }

  /* If LS supports preconditioning, initialize pset/psol to NULL */
  if (LS->ops->setpreconditioner)
  {
    retval = SUNLinSolSetPreconditioner(LS, ark_mem, NULL, NULL);
    if (retval != SUN_SUCCESS)
    {
      arkProcessError(ark_mem, ARKLS_SUNLS_FAIL, __LINE__, __func__, __FILE__,
                      "Error in calling SUNLinSolSetPreconditioner");
      free(arkls_mem);
      arkls_mem = NULL;
      return (ARKLS_SUNLS_FAIL);
    }
  }

  /* When using a non-NULL SUNMatrix object, store pointer to M and, for direct
     linear solvers, create M_lu to store the factorization of M */
  if (M != NULL)
  {
    arkls_mem->M = M;
    if (!iterative)
    {
      arkls_mem->M_lu = SUNMatClone(M);
      if (arkls_mem->M_lu == NULL)
      {
        arkProcessError(ark_mem, ARKLS_MEM_FAIL, __LINE__, __func__, __FILE__,
                        MSG_LS_MEM_FAIL);
        free(arkls_mem);
        arkls_mem = NULL;
        return (ARKLS_MEM_FAIL);
      }
    }
    else { arkls_mem->M_lu = M; }
  }

  /* Allocate memory for x */
  if (!arkAllocVec(ark_mem, ark_mem->tempv1, &(arkls_mem->x)))
  {
    arkProcessError(ark_mem, ARKLS_MEM_FAIL, __LINE__, __func__, __FILE__,
                    MSG_LS_MEM_FAIL);
    if (!iterative) { SUNMatDestroy(arkls_mem->M_lu); }
    free(arkls_mem);
    arkls_mem = NULL;
    return (ARKLS_MEM_FAIL);
  }

  /* For iterative LS, compute default norm conversion factor */
  if (iterative) { arkls_mem->nrmfac = SUNRsqrt(N_VGetLength(arkls_mem->x)); }

  /* Attach ARKLs interface to time stepper module */
  retval = ark_mem->step_attachmasssol(ark_mem, arkLsMassInitialize,
                                       arkLsMassSetup, arkLsMTimes,
                                       arkLsMassSolve, arkLsMassFree, time_dep,
                                       LSType, arkls_mem);
  if (retval != ARK_SUCCESS)
  {
    arkProcessError(ark_mem, retval, __LINE__, __func__, __FILE__,
                    "Failed to attach to time stepper module");
    N_VDestroy(arkls_mem->x);
    if (!iterative) { SUNMatDestroy(arkls_mem->M_lu); }
    free(arkls_mem);
    arkls_mem = NULL;
    return (retval);
  }

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeSetJacFn specifies the Jacobian function.
  ---------------------------------------------------------------*/
int ARKodeSetJacFn(void* arkode_mem, ARKLsJacFn jac)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Guard against use for time steppers that do not need an algebraic solver */
  if (!ark_mem->step_supports_implicit)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not require an algebraic solver");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* return with failure if jac cannot be used */
  if ((jac != NULL) && (arkls_mem->A == NULL))
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "Jacobian routine cannot be supplied for NULL SUNMatrix");
    return (ARKLS_ILL_INPUT);
  }

  /* set the Jacobian routine pointer, and update relevant flags */
  if (jac != NULL)
  {
    arkls_mem->jacDQ  = SUNFALSE;
    arkls_mem->jac    = jac;
    arkls_mem->J_data = ark_mem->user_data;
  }
  else
  {
    arkls_mem->jacDQ  = SUNTRUE;
    arkls_mem->jac    = arkLsDQJac;
    arkls_mem->J_data = ark_mem;
  }

  /* ensure the internal linear system function is used */
  arkls_mem->user_linsys = SUNFALSE;
  arkls_mem->linsys      = arkLsLinSys;
  arkls_mem->A_data      = ark_mem;

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeSetMassFn specifies the mass matrix function.
  ---------------------------------------------------------------*/
int ARKodeSetMassFn(void* arkode_mem, ARKLsMassFn mass)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Guard against use for time steppers that do not support mass matrices */
  if (!ark_mem->step_supports_massmatrix)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not support non-identity mass matrices");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* return with failure if mass cannot be used */
  if (mass == NULL)
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "Mass-matrix routine must be non-NULL");
    return (ARKLS_ILL_INPUT);
  }
  if (arkls_mem->M == NULL)
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__,
                    __FILE__, "Mass-matrix routine cannot be supplied for NULL SUNMatrix");
    return (ARKLS_ILL_INPUT);
  }

  /* set mass matrix routine pointer and return */
  arkls_mem->mass   = mass;
  arkls_mem->M_data = ark_mem->user_data;

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeSetEpsLin specifies the nonlinear -> linear tolerance
  scale factor.
  ---------------------------------------------------------------*/
int ARKodeSetEpsLin(void* arkode_mem, sunrealtype eplifac)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Guard against use for time steppers that do not need an algebraic solver */
  if (!ark_mem->step_supports_implicit)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not require an algebraic solver");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* store input and return */
  arkls_mem->eplifac = (eplifac <= ZERO) ? ARKLS_EPLIN : eplifac;

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeSetLSNormFactor sets or computes the factor to use when
  converting from the integrator tolerance (WRMS norm) to the
  linear solver tolerance (L2 norm).
  ---------------------------------------------------------------*/
int ARKodeSetLSNormFactor(void* arkode_mem, sunrealtype nrmfac)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Guard against use for time steppers that do not need an algebraic solver */
  if (!ark_mem->step_supports_implicit)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not require an algebraic solver");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* store input and return */
  if (nrmfac > ZERO)
  {
    /* set user-provided factor */
    arkls_mem->nrmfac = nrmfac;
  }
  else if (nrmfac < ZERO)
  {
    /* Ensure that vector support N_VDotProd */
    if (ark_mem->tempv1->ops->nvdotprod == NULL)
    {
      arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__,
                      __FILE__, "N_VDotProd unimplemented (required for ARKodeSetLSNormFactor)");
      return (ARKLS_ILL_INPUT);
    }

    /* compute factor for WRMS norm with dot product */
    N_VConst(ONE, ark_mem->tempv1);
    arkls_mem->nrmfac = SUNRsqrt(N_VDotProd(ark_mem->tempv1, ark_mem->tempv1));
  }
  else
  {
    /* compute default factor for WRMS norm from vector length */
    arkls_mem->nrmfac = SUNRsqrt(N_VGetLength(ark_mem->tempv1));
  }

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeSetJacEvalFrequency specifies the frequency for
  recomputing the Jacobian matrix and/or preconditioner.
  ---------------------------------------------------------------*/
int ARKodeSetJacEvalFrequency(void* arkode_mem, long int msbj)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Guard against use for time steppers that do not need an algebraic solver */
  if (!ark_mem->step_supports_implicit)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not require an algebraic solver");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* store input and return */
  arkls_mem->msbj = (msbj <= 0) ? ARKLS_MSBJ : msbj;

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeSetLinearSolutionScaling enables or disables scaling the
  linear solver solution to account for changes in gamma.
  ---------------------------------------------------------------*/
int ARKodeSetLinearSolutionScaling(void* arkode_mem, sunbooleantype onoff)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Guard against use for time steppers that do not need an algebraic solver */
  if (!ark_mem->step_supports_implicit)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not require an algebraic solver");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* check for valid solver type */
  if (!(arkls_mem->matrixbased)) { return (ARKLS_ILL_INPUT); }

  /* set solution scaling flag */
  arkls_mem->scalesol = onoff;

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeSetPreconditioner specifies the user-supplied
  preconditioner setup and solve routines.
  ---------------------------------------------------------------*/
int ARKodeSetPreconditioner(void* arkode_mem, ARKLsPrecSetupFn psetup,
                            ARKLsPrecSolveFn psolve)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  SUNPSetupFn arkls_psetup;
  SUNPSolveFn arkls_psolve;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Guard against use for time steppers that do not need an algebraic solver */
  if (!ark_mem->step_supports_implicit)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not require an algebraic solver");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* issue error if LS object does not allow user-supplied preconditioning */
  if (arkls_mem->LS->ops->setpreconditioner == NULL)
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "SUNLinearSolver object does not support user-supplied "
                    "preconditioning");
    return (ARKLS_ILL_INPUT);
  }

  /* store function pointers for user-supplied routines */
  arkls_mem->pset   = psetup;
  arkls_mem->psolve = psolve;

  /* notify linear solver to call ARKLs interface routines */
  arkls_psetup = (psetup == NULL) ? NULL : arkLsPSetup;
  arkls_psolve = (psolve == NULL) ? NULL : arkLsPSolve;
  retval = SUNLinSolSetPreconditioner(arkls_mem->LS, ark_mem, arkls_psetup,
                                      arkls_psolve);
  if (retval != SUN_SUCCESS)
  {
    arkProcessError(ark_mem, ARKLS_SUNLS_FAIL, __LINE__, __func__, __FILE__,
                    "Error in calling SUNLinSolSetPreconditioner");
    return (ARKLS_SUNLS_FAIL);
  }

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeSetJacTimes specifies the user-supplied Jacobian-vector
  product setup and multiply routines.
  ---------------------------------------------------------------*/
int ARKodeSetJacTimes(void* arkode_mem, ARKLsJacTimesSetupFn jtsetup,
                      ARKLsJacTimesVecFn jtimes)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Guard against use for time steppers that do not need an algebraic solver */
  if (!ark_mem->step_supports_implicit)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not require an algebraic solver");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* issue error if LS object does not allow user-supplied ATimes */
  if (arkls_mem->LS->ops->setatimes == NULL)
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__,
                    __FILE__, "SUNLinearSolver object does not support user-supplied ATimes routine");
    return (ARKLS_ILL_INPUT);
  }

  /* store function pointers for user-supplied routines in ARKLs
     interface (NULL jtimes implies use of DQ default) */
  if (jtimes != NULL)
  {
    arkls_mem->jtimesDQ = SUNFALSE;
    arkls_mem->jtsetup  = jtsetup;
    arkls_mem->jtimes   = jtimes;
    arkls_mem->Jt_data  = ark_mem->user_data;
  }
  else
  {
    arkls_mem->jtimesDQ = SUNTRUE;
    arkls_mem->jtsetup  = NULL;
    arkls_mem->jtimes   = arkLsDQJtimes;
    arkls_mem->Jt_data  = ark_mem;
    arkls_mem->Jt_f     = ark_mem->step_getimplicitrhs(ark_mem);

    if (arkls_mem->Jt_f == NULL)
    {
      arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                      "Time step module is missing implicit RHS fcn");
      return (ARKLS_ILL_INPUT);
    }
  }

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeSetJacTimesRhsFn specifies an alternative user-supplied
  ODE right-hand side function to use in the internal finite
  difference Jacobian-vector product.
  ---------------------------------------------------------------*/
int ARKodeSetJacTimesRhsFn(void* arkode_mem, ARKRhsFn jtimesRhsFn)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Guard against use for time steppers that do not need an algebraic solver */
  if (!ark_mem->step_supports_implicit)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not require an algebraic solver");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* check if using internal finite difference approximation */
  if (!(arkls_mem->jtimesDQ))
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__,
                    __FILE__, "Internal finite-difference Jacobian-vector product is disabled.");
    return (ARKLS_ILL_INPUT);
  }

  /* store function pointers for RHS function (NULL implies use ODE RHS) */
  if (jtimesRhsFn != NULL) { arkls_mem->Jt_f = jtimesRhsFn; }
  else
  {
    arkls_mem->Jt_f = ark_mem->step_getimplicitrhs(ark_mem);

    if (arkls_mem->Jt_f == NULL)
    {
      arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                      "Time step module is missing implicit RHS fcn");
      return (ARKLS_ILL_INPUT);
    }
  }

  return (ARKLS_SUCCESS);
}

/* ARKodeSetLinSysFn specifies the linear system setup function. */
int ARKodeSetLinSysFn(void* arkode_mem, ARKLsLinSysFn linsys)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Guard against use for time steppers that do not need an algebraic solver */
  if (!ark_mem->step_supports_implicit)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not require an algebraic solver");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARKLS_SUCCESS) { return (retval); }

  /* return with failure if linsys cannot be used */
  if ((linsys != NULL) && (arkls_mem->A == NULL))
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__,
                    __FILE__, "Linear system setup routine cannot be supplied for NULL SUNMatrix");
    return (ARKLS_ILL_INPUT);
  }

  /* set the linear system routine pointer, and update relevant flags */
  if (linsys != NULL)
  {
    arkls_mem->user_linsys = SUNTRUE;
    arkls_mem->linsys      = linsys;
    arkls_mem->A_data      = ark_mem->user_data;
  }
  else
  {
    arkls_mem->user_linsys = SUNFALSE;
    arkls_mem->linsys      = arkLsLinSys;
    arkls_mem->A_data      = ark_mem;
  }

  return (ARKLS_SUCCESS);
}

int ARKodeGetJac(void* arkode_mem, SUNMatrix* J)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return NULL for incompatible steppers */
  if (!ark_mem->step_supports_implicit)
  {
    *J = NULL;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARKLS_SUCCESS) { return retval; }

  /* set output and return */
  *J = arkls_mem->savedJ;
  return ARKLS_SUCCESS;
}

int ARKodeGetJacTime(void* arkode_mem, sunrealtype* t_J)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return an error for incompatible steppers */
  if (!ark_mem->step_supports_implicit)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not require an algebraic solver");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARKLS_SUCCESS) { return retval; }

  /* set output and return */
  *t_J = arkls_mem->tnlj;
  return ARKLS_SUCCESS;
}

int ARKodeGetJacNumSteps(void* arkode_mem, long int* nst_J)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_implicit)
  {
    *nst_J = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARKLS_SUCCESS) { return retval; }

  /* set output and return */
  *nst_J = arkls_mem->nstlj;
  return ARKLS_SUCCESS;
}

/*---------------------------------------------------------------
  ARKodeGetLinWorkSpace returns the length of workspace allocated for
  the ARKLS linear solver interface.
  ---------------------------------------------------------------*/
int ARKodeGetLinWorkSpace(void* arkode_mem, long int* lenrw, long int* leniw)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  sunindextype lrw1, liw1;
  long int lrw, liw;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_implicit)
  {
    *lenrw = *leniw = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* start with fixed sizes plus vector/matrix pointers */
  *lenrw = 3;
  *leniw = 30;

  /* add NVector sizes */
  if (arkls_mem->x->ops->nvspace)
  {
    N_VSpace(arkls_mem->x, &lrw1, &liw1);
    *lenrw += 2 * lrw1;
    *leniw += 2 * liw1;
  }

  /* add SUNMatrix size (only account for the one owned by Ls interface) */
  if (arkls_mem->savedJ)
  {
    if (arkls_mem->savedJ->ops->space)
    {
      retval = SUNMatSpace(arkls_mem->savedJ, &lrw, &liw);
      if (retval == 0)
      {
        *lenrw += lrw;
        *leniw += liw;
      }
    }
  }

  /* add LS sizes */
  if (arkls_mem->LS->ops->space)
  {
    retval = SUNLinSolSpace(arkls_mem->LS, &lrw, &liw);
    if (retval == SUN_SUCCESS)
    {
      *lenrw += lrw;
      *leniw += liw;
    }
  }

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumJacEvals returns the number of Jacobian evaluations
  ---------------------------------------------------------------*/
int ARKodeGetNumJacEvals(void* arkode_mem, long int* njevals)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_implicit)
  {
    *njevals = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *njevals = arkls_mem->nje;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumLinRhsEvals returns the number of calls to the ODE
  function needed for the DQ Jacobian approximation or J*v product
  approximation.
  ---------------------------------------------------------------*/
int ARKodeGetNumLinRhsEvals(void* arkode_mem, long int* nfevalsLS)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_implicit)
  {
    *nfevalsLS = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *nfevalsLS = arkls_mem->nfeDQ;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumPrecEvals returns the number of calls to the
  user- or ARKODE-supplied preconditioner setup routine.
  ---------------------------------------------------------------*/
int ARKodeGetNumPrecEvals(void* arkode_mem, long int* npevals)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_implicit)
  {
    *npevals = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *npevals = arkls_mem->npe;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumPrecSolves returns the number of calls to the
  user- or ARKODE-supplied preconditioner solve routine.
  ---------------------------------------------------------------*/
int ARKodeGetNumPrecSolves(void* arkode_mem, long int* npsolves)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_implicit)
  {
    *npsolves = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *npsolves = arkls_mem->nps;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumLinIters returns the number of linear iterations
  (if accessible from the LS object).
  ---------------------------------------------------------------*/
int ARKodeGetNumLinIters(void* arkode_mem, long int* nliters)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_implicit)
  {
    *nliters = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *nliters = arkls_mem->nli;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumLinConvFails returns the number of linear solver
  convergence failures (as reported by the LS object).
  ---------------------------------------------------------------*/
int ARKodeGetNumLinConvFails(void* arkode_mem, long int* nlcfails)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_implicit)
  {
    *nlcfails = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *nlcfails = arkls_mem->ncfl;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumJTSetupEvals returns the number of calls to the
  user-supplied Jacobian-vector product setup routine.
  ---------------------------------------------------------------*/
int ARKodeGetNumJTSetupEvals(void* arkode_mem, long int* njtsetups)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_implicit)
  {
    *njtsetups = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *njtsetups = arkls_mem->njtsetup;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumJtimesEvals returns the number of calls to the
  Jacobian-vector product multiply routine.
  ---------------------------------------------------------------*/
int ARKodeGetNumJtimesEvals(void* arkode_mem, long int* njvevals)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_implicit)
  {
    *njvevals = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMem structures */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *njvevals = arkls_mem->njtimes;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumMassMultSetups returns the number of calls to the
  mass matrix-vector setup routine.
  ---------------------------------------------------------------*/
int ARKodeGetNumMassMultSetups(void* arkode_mem, long int* nmvsetups)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_massmatrix)
  {
    *nmvsetups = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *nmvsetups = arkls_mem->nmvsetup;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetLastLinFlag returns the last flag set in a ARKLS
  function.
  ---------------------------------------------------------------*/
int ARKodeGetLastLinFlag(void* arkode_mem, long int* flag)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return success for incompatible steppers */
  if (!ark_mem->step_supports_implicit)
  {
    *flag = ARKLS_SUCCESS;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *flag = arkls_mem->last_flag;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetLinReturnFlagName translates from the integer error code
  returned by an ARKLs routine to the corresponding string
  equivalent for that flag
  ---------------------------------------------------------------*/
char* ARKodeGetLinReturnFlagName(long int flag)
{
  char* name = (char*)malloc(30 * sizeof(char));

  switch (flag)
  {
  case ARKLS_SUCCESS: sprintf(name, "ARKLS_SUCCESS"); break;
  case ARKLS_MEM_NULL: sprintf(name, "ARKLS_MEM_NULL"); break;
  case ARKLS_LMEM_NULL: sprintf(name, "ARKLS_LMEM_NULL"); break;
  case ARKLS_ILL_INPUT: sprintf(name, "ARKLS_ILL_INPUT"); break;
  case ARKLS_MEM_FAIL: sprintf(name, "ARKLS_MEM_FAIL"); break;
  case ARKLS_MASSMEM_NULL: sprintf(name, "ARKLS_MASSMEM_NULL"); break;
  case ARKLS_JACFUNC_UNRECVR: sprintf(name, "ARKLS_JACFUNC_UNRECVR"); break;
  case ARKLS_JACFUNC_RECVR: sprintf(name, "ARKLS_JACFUNC_RECVR"); break;
  case ARKLS_MASSFUNC_UNRECVR: sprintf(name, "ARKLS_MASSFUNC_UNRECVR"); break;
  case ARKLS_MASSFUNC_RECVR: sprintf(name, "ARKLS_MASSFUNC_RECVR"); break;
  case ARKLS_SUNMAT_FAIL: sprintf(name, "ARKLS_SUNMAT_FAIL"); break;
  case ARKLS_SUNLS_FAIL: sprintf(name, "ARKLS_SUNLS_FAIL"); break;
  default: sprintf(name, "NONE");
  }

  return (name);
}

/*---------------------------------------------------------------
  ARKodeSetMassEpsLin specifies the nonlinear -> linear tolerance
  scale factor for mass matrix linear systems.
  ---------------------------------------------------------------*/
int ARKodeSetMassEpsLin(void* arkode_mem, sunrealtype eplifac)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Guard against use for time steppers that do not support mass matrices */
  if (!ark_mem->step_supports_massmatrix)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not support non-identity mass matrices");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* store input and return */
  arkls_mem->eplifac = (eplifac <= ZERO) ? ARKLS_EPLIN : eplifac;

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeSetMassLSNormFactor sets or computes the factor to use when
  converting from the integrator tolerance (WRMS norm) to the
  linear solver tolerance (L2 norm).
  ---------------------------------------------------------------*/
int ARKodeSetMassLSNormFactor(void* arkode_mem, sunrealtype nrmfac)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Guard against use for time steppers that do not support mass matrices */
  if (!ark_mem->step_supports_massmatrix)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not support non-identity mass matrices");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  /* access ARKLsMem structures */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* store input and return */
  if (nrmfac > ZERO)
  {
    /* set user-provided factor */
    arkls_mem->nrmfac = nrmfac;
  }
  else if (nrmfac < ZERO)
  {
    /* Ensure that vector support N_VDotProd */
    if (ark_mem->tempv1->ops->nvdotprod == NULL)
    {
      arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__,
                      __FILE__, "N_VDotProd unimplemented (required for ARKodeSetMassLSNormFactor)");
      return (ARKLS_ILL_INPUT);
    }

    /* compute factor for WRMS norm with dot product */
    N_VConst(ONE, ark_mem->tempv1);
    arkls_mem->nrmfac = SUNRsqrt(N_VDotProd(ark_mem->tempv1, ark_mem->tempv1));
  }
  else
  {
    /* compute default factor for WRMS norm from vector length */
    arkls_mem->nrmfac = SUNRsqrt(N_VGetLength(ark_mem->tempv1));
  }

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeSetMassPreconditioner specifies the user-supplied
  preconditioner setup and solve routines.
  ---------------------------------------------------------------*/
int ARKodeSetMassPreconditioner(void* arkode_mem, ARKLsMassPrecSetupFn psetup,
                                ARKLsMassPrecSolveFn psolve)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  SUNPSetupFn arkls_mpsetup;
  SUNPSolveFn arkls_mpsolve;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Guard against use for time steppers that do not support mass matrices */
  if (!ark_mem->step_supports_massmatrix)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not support non-identity mass matrices");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* issue error if LS object does not allow user-supplied preconditioning */
  if (arkls_mem->LS->ops->setpreconditioner == NULL)
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "SUNLinearSolver object does not support user-supplied "
                    "preconditioning");
    return (ARKLS_ILL_INPUT);
  }

  /* store function pointers for user-supplied routines in ARKLs interface */
  arkls_mem->pset   = psetup;
  arkls_mem->psolve = psolve;

  /* notify linear solver to call ARKLs interface routines */
  arkls_mpsetup = (psetup == NULL) ? NULL : arkLsMPSetup;
  arkls_mpsolve = (psolve == NULL) ? NULL : arkLsMPSolve;
  retval = SUNLinSolSetPreconditioner(arkls_mem->LS, ark_mem, arkls_mpsetup,
                                      arkls_mpsolve);
  if (retval != SUN_SUCCESS)
  {
    arkProcessError(ark_mem, ARKLS_SUNLS_FAIL, __LINE__, __func__, __FILE__,
                    "Error in calling SUNLinSolSetPreconditioner");
    return (ARKLS_SUNLS_FAIL);
  }

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeSetMassTimes specifies the user-supplied mass
  matrix-vector product setup and multiply routines.
  ---------------------------------------------------------------*/
int ARKodeSetMassTimes(void* arkode_mem, ARKLsMassTimesSetupFn mtsetup,
                       ARKLsMassTimesVecFn mtimes, void* mtimes_data)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Guard against use for time steppers that do not support mass matrices */
  if (!ark_mem->step_supports_massmatrix)
  {
    arkProcessError(ark_mem, ARK_STEPPER_UNSUPPORTED, __LINE__, __func__,
                    __FILE__, "time-stepping module does not support non-identity mass matrices");
    return (ARK_STEPPER_UNSUPPORTED);
  }

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* issue error if mtimes function is unusable */
  if (mtimes == NULL)
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "non-NULL mtimes function must be supplied");
    return (ARKLS_ILL_INPUT);
  }

  /* issue error if LS object does not allow user-supplied ATimes */
  if (arkls_mem->LS->ops->setatimes == NULL)
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__,
                    __FILE__, "SUNLinearSolver object does not support user-supplied ATimes routine");
    return (ARKLS_ILL_INPUT);
  }

  /* store pointers for user-supplied routines and data structure
     in ARKLs interface */
  arkls_mem->mtsetup = mtsetup;
  arkls_mem->mtimes  = mtimes;
  arkls_mem->mt_data = mtimes_data;

  /* notify linear solver to call ARKLs interface routine */
  retval = SUNLinSolSetATimes(arkls_mem->LS, ark_mem, arkLsMTimes);
  if (retval != SUN_SUCCESS)
  {
    arkProcessError(ark_mem, ARKLS_SUNLS_FAIL, __LINE__, __func__, __FILE__,
                    "Error in calling SUNLinSolSetATimes");
    return (ARKLS_SUNLS_FAIL);
  }

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetMassWorkSpace
  ---------------------------------------------------------------*/
int ARKodeGetMassWorkSpace(void* arkode_mem, long int* lenrw, long int* leniw)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  sunindextype lrw1, liw1;
  long int lrw, liw;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_massmatrix)
  {
    *lenrw = *leniw = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* start with fixed sizes plus vector/matrix pointers */
  *lenrw = 2;
  *leniw = 23;

  /* add NVector sizes */
  if (ark_mem->tempv1->ops->nvspace)
  {
    N_VSpace(ark_mem->tempv1, &lrw1, &liw1);
    *lenrw += lrw1;
    *leniw += liw1;
  }

  /* add SUNMatrix size (only account for the one owned by Ls interface) */
  if (!(arkls_mem->iterative) && arkls_mem->M_lu)
  {
    if (arkls_mem->M_lu->ops->space)
    {
      retval = SUNMatSpace(arkls_mem->M_lu, &lrw, &liw);
      if (retval == 0)
      {
        *lenrw += lrw;
        *leniw += liw;
      }
    }
  }

  /* add LS sizes */
  if (arkls_mem->LS->ops->space)
  {
    retval = SUNLinSolSpace(arkls_mem->LS, &lrw, &liw);
    if (retval == SUN_SUCCESS)
    {
      *lenrw += lrw;
      *leniw += liw;
    }
  }

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumMassSetups returns the number of mass matrix
  solver 'setup' calls
  ---------------------------------------------------------------*/
int ARKodeGetNumMassSetups(void* arkode_mem, long int* nmsetups)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_massmatrix)
  {
    *nmsetups = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *nmsetups = arkls_mem->nmsetups;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumMassMult returns the number of calls to the user-
  supplied or internal mass matrix-vector product multiply routine.
  ---------------------------------------------------------------*/
int ARKodeGetNumMassMult(void* arkode_mem, long int* nmvevals)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_massmatrix)
  {
    *nmvevals = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *nmvevals = arkls_mem->nmtimes;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumMassSolves returns the number of mass matrix
  solver 'solve' calls
  ---------------------------------------------------------------*/
int ARKodeGetNumMassSolves(void* arkode_mem, long int* nmsolves)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_massmatrix)
  {
    *nmsolves = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *nmsolves = arkls_mem->nmsolves;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumMassPrecEvals returns the number of calls to the
  user- or ARKODE-supplied preconditioner setup routine.
  ---------------------------------------------------------------*/
int ARKodeGetNumMassPrecEvals(void* arkode_mem, long int* npevals)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_massmatrix)
  {
    *npevals = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *npevals = arkls_mem->npe;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumMassPrecSolves returns the number of calls to the
  user- or ARKODE-supplied preconditioner solve routine.
  ---------------------------------------------------------------*/
int ARKodeGetNumMassPrecSolves(void* arkode_mem, long int* npsolves)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_massmatrix)
  {
    *npsolves = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *npsolves = arkls_mem->nps;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumMassIters returns the number of mass matrix solver
  linear iterations (if accessible from the LS object).
  ---------------------------------------------------------------*/
int ARKodeGetNumMassIters(void* arkode_mem, long int* nmiters)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_massmatrix)
  {
    *nmiters = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *nmiters = arkls_mem->nli;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumMassConvFails returns the number of linear solver
  convergence failures (as reported by the LS object).
  ---------------------------------------------------------------*/
int ARKodeGetNumMassConvFails(void* arkode_mem, long int* nmcfails)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_massmatrix)
  {
    *nmcfails = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *nmcfails = arkls_mem->ncfl;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetCurrentMassMatrix returns the current mass matrix.
  ---------------------------------------------------------------*/
int ARKodeGetCurrentMassMatrix(void* arkode_mem, SUNMatrix* M)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return NULL for incompatible steppers */
  if (!ark_mem->step_supports_massmatrix)
  {
    *M = NULL;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *M = arkls_mem->M;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetNumMTSetups returns the number of calls to the
  user-supplied mass matrix-vector product setup routine.
  ---------------------------------------------------------------*/
int ARKodeGetNumMTSetups(void* arkode_mem, long int* nmtsetups)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return 0 for incompatible steppers */
  if (!ark_mem->step_supports_massmatrix)
  {
    *nmtsetups = 0;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output value and return */
  *nmtsetups = arkls_mem->nmtsetup;
  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  ARKodeGetLastMassFlag returns the last flag set in a ARKLS
  function.
  ---------------------------------------------------------------*/
int ARKodeGetLastMassFlag(void* arkode_mem, long int* flag)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* Return immediately if arkode_mem is NULL */
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARK_MEM_NULL, __LINE__, __func__, __FILE__,
                    MSG_ARK_NO_MEM);
    return (ARK_MEM_NULL);
  }
  ark_mem = (ARKodeMem)arkode_mem;

  /* Return ARKLS_SUCCESS for incompatible steppers */
  if (!ark_mem->step_supports_massmatrix)
  {
    *flag = ARKLS_SUCCESS;
    return (ARK_SUCCESS);
  }

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* set output and return */
  *flag = arkls_mem->last_flag;
  return (ARKLS_SUCCESS);
}

/*===============================================================
  ARKLS Private functions
  ===============================================================*/

/* arkLSSetUserData sets user_data pointers in arkLS */
int arkLSSetUserData(ARKodeMem ark_mem, void* user_data)
{
  ARKLsMem arkls_mem;
  int retval;

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARKLS_SUCCESS) { return (retval); }

  /* Set data for Jacobian */
  if (!arkls_mem->jacDQ) { arkls_mem->J_data = user_data; }

  /* Set data for Jtimes */
  if (!arkls_mem->jtimesDQ) { arkls_mem->Jt_data = user_data; }

  /* Set data for LinSys */
  if (arkls_mem->user_linsys) { arkls_mem->A_data = user_data; }

  /* Set data for Preconditioner */
  arkls_mem->P_data = user_data;

  return (ARKLS_SUCCESS);
}

/* arkLSMassSetUserData sets user_data pointers in arkLSMass */
int arkLSSetMassUserData(ARKodeMem ark_mem, void* user_data)
{
  ARKLsMassMem arkls_mem;
  int retval;

  /* access ARKLsMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARKLS_SUCCESS) { return (retval); }

  /* Set data for mass matrix */
  if (arkls_mem->mass != NULL) { arkls_mem->M_data = user_data; }

  /* Data for Mtimes is set in arkLSSetMassTimes */

  /* Set data for Preconditioner */
  arkls_mem->P_data = user_data;

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  arkLsATimes:

  This routine generates the matrix-vector product z = Av, where
  A = M - gamma*J. The product M*v is obtained either by calling
  the mtimes routine or by just using v (if M=I).  The product
  J*v is obtained by calling the jtimes routine. It is then scaled
  by -gamma and added to M*v to obtain A*v. The return value is
  the same as the values returned by jtimes and mtimes --
  0 if successful, nonzero otherwise.
  ---------------------------------------------------------------*/
int arkLsATimes(void* arkode_mem, N_Vector v, N_Vector z)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  void* ark_step_massmem;
  int retval;
  sunrealtype gamma, gamrat;
  sunbooleantype dgamma_fail, *jcur;

  /* access ARKodeMem and ARKLsMem structures */
  retval = arkLs_AccessARKODELMem(arkode_mem, __func__, &ark_mem, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* Access mass matrix solver (if it exists) */
  ark_step_massmem = NULL;
  if (ark_mem->step_getmassmem != NULL)
  {
    ark_step_massmem = ark_mem->step_getmassmem(arkode_mem);
  }

  /* get gamma values from time step module */
  retval = ark_mem->step_getgammas(arkode_mem, &gamma, &gamrat, &jcur,
                                   &dgamma_fail);
  if (retval != ARK_SUCCESS)
  {
    arkProcessError(ark_mem, retval, __LINE__, __func__, __FILE__,
                    "An error occurred in ark_step_getgammas");
    return (retval);
  }

  /* call Jacobian-times-vector product routine
     (either user-supplied or internal DQ) */
  retval = arkls_mem->jtimes(v, z, arkls_mem->tcur, arkls_mem->ycur,
                             arkls_mem->fcur, arkls_mem->Jt_data,
                             arkls_mem->ytemp);
  arkls_mem->njtimes++;
  if (retval != 0) { return (retval); }

  /* Compute mass matrix vector product and add to result */
  if (ark_step_massmem != NULL)
  {
    retval = arkLsMTimes(arkode_mem, v, arkls_mem->ytemp);
    if (retval != 0) { return (retval); }
    N_VLinearSum(ONE, arkls_mem->ytemp, -gamma, z, z);
  }
  else { N_VLinearSum(ONE, v, -gamma, z, z); }

  return (0);
}

/*---------------------------------------------------------------
  arkLsPSetup:

  This routine interfaces between the generic iterative linear
  solvers and the user's psetup routine.  It passes to psetup all
  required state information from arkode_mem.  Its return value
  is the same as that returned by psetup. Note that the generic
  iterative linear solvers guarantee that arkLsPSetup will only
  be called in the case that the user's psetup routine is non-NULL.
  ---------------------------------------------------------------*/
int arkLsPSetup(void* arkode_mem)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  sunrealtype gamma, gamrat;
  sunbooleantype dgamma_fail, *jcur;
  int retval;

  /* access ARKodeMem and ARKLsMem structures */
  retval = arkLs_AccessARKODELMem(arkode_mem, __func__, &ark_mem, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* get gamma values from time step module */
  retval = ark_mem->step_getgammas(arkode_mem, &gamma, &gamrat, &jcur,
                                   &dgamma_fail);
  if (retval != ARK_SUCCESS)
  {
    arkProcessError(ark_mem, retval, __LINE__, __func__, __FILE__,
                    "An error occurred in ark_step_getgammas");
    return (retval);
  }

  /* Call user pset routine to update preconditioner and possibly
     reset jcur (pass !jbad as update suggestion) */
  retval = arkls_mem->pset(arkls_mem->tcur, arkls_mem->ycur, arkls_mem->fcur,
                           !(arkls_mem->jbad), jcur, gamma, arkls_mem->P_data);
  return (retval);
}

/*---------------------------------------------------------------
  arkLsPSolve:

  This routine interfaces between the generic SUNLinSolSolve
  routine and the user's psolve routine.  It passes to psolve all
  required state information from arkode_mem.  Its return value
  is the same as that returned by psolve. Note that the generic
  SUNLinSol solver guarantees that arkLsPSolve will not be
  called in the case in which preconditioning is not done. This
  is the only case in which the user's psolve routine is allowed
  to be NULL.
  ---------------------------------------------------------------*/
int arkLsPSolve(void* arkode_mem, N_Vector r, N_Vector z, sunrealtype tol, int lr)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  sunrealtype gamma, gamrat;
  sunbooleantype dgamma_fail, *jcur;
  int retval;

  /* access ARKodeMem and ARKLsMem structures */
  retval = arkLs_AccessARKODELMem(arkode_mem, __func__, &ark_mem, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* get gamma values from time step module */
  retval = ark_mem->step_getgammas(arkode_mem, &gamma, &gamrat, &jcur,
                                   &dgamma_fail);
  if (retval != ARK_SUCCESS)
  {
    arkProcessError(ark_mem, retval, __LINE__, __func__, __FILE__,
                    "An error occurred in ark_step_getgammas");
    return (retval);
  }

  /* call the user-supplied psolve routine, and accumulate count */
  retval = arkls_mem->psolve(arkls_mem->tcur, arkls_mem->ycur, arkls_mem->fcur,
                             r, z, gamma, tol, lr, arkls_mem->P_data);
  arkls_mem->nps++;
  return (retval);
}

/*---------------------------------------------------------------
  arkLsMTimes:

  This routine generates the matrix-vector product z = Mv, where
  M is the system mass matrix, by calling the user-supplied mtimes
  routine. The return value is the same as the value returned
  by mtimes -- 0 if successful, nonzero otherwise.
  ---------------------------------------------------------------*/
int arkLsMTimes(void* arkode_mem, N_Vector v, N_Vector z)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* access ARKodeMem and ARKLsMassMem structures */
  retval = arkLs_AccessARKODEMassMem(arkode_mem, __func__, &ark_mem, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* perform multiply by either calling the user-supplied routine
     (default), or asking the SUNMatrix to do the multiply */
  if (arkls_mem->mtimes)
  {
    /* call user-supplied mtimes routine, increment counter and return */
    retval = arkls_mem->mtimes(v, z, ark_mem->tcur, arkls_mem->mt_data);
    if (retval == 0) { arkls_mem->nmtimes++; }
    else
    {
      arkProcessError(ark_mem, retval, __LINE__, __func__, __FILE__,
                      "Error in user mass matrix-vector product routine");
    }
    return (retval);
  }
  else if (arkls_mem->M)
  {
    /* try to ask SUNMatrix to do the multiply; increment counter and return */
    if (arkls_mem->M->ops->matvec)
    {
      retval = SUNMatMatvec(arkls_mem->M, v, z);
      if (retval == 0) { arkls_mem->nmtimes++; }
      else
      {
        arkProcessError(ark_mem, retval, __LINE__, __func__,
                        __FILE__, "Error in SUNMatrix mass matrix-vector product routine");
      }
      return (retval);
    }
  }

  /* if we made it here, then no matrix-vector product is available */
  arkProcessError(ark_mem, retval, __LINE__, __func__, __FILE__,
                  "Missing mass matrix-vector product routine");
  return (-1);
}

/*---------------------------------------------------------------
  arkLsMPSetup:

  This routine interfaces between the generic linear solver and
  the user's mass matrix psetup routine.  It passes to psetup all
  required state information from arkode_mem.  Its return value
  is the same as that returned by psetup.  Note that the generic
  linear solvers guarantee that arkLsMPSetup will only be
  called if the user's psetup routine is non-NULL.
  ---------------------------------------------------------------*/
int arkLsMPSetup(void* arkode_mem)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* access ARKodeMem and ARKLsMassMem structures */
  retval = arkLs_AccessARKODEMassMem(arkode_mem, __func__, &ark_mem, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* only proceed if the mass matrix is time-independent or if
     pset has not been called previously */
  if (!arkls_mem->time_dependent && arkls_mem->npe) { return (0); }

  /* call user-supplied pset routine and increment counter */
  retval = arkls_mem->pset(ark_mem->tcur, arkls_mem->P_data);
  arkls_mem->npe++;
  return (retval);
}

/*---------------------------------------------------------------
  arkLsMPSolve:

  This routine interfaces between the generic LS routine and the
  user's mass matrix psolve routine.  It passes to psolve all
  required state information from arkode_mem.  Its return value is
  the same as that returned by psolve. Note that the generic
  solver guarantees that arkLsMPSolve will not be called in the
  case in which preconditioning is not done. This is the only case
  in which the user's psolve routine is allowed to be NULL.
  ---------------------------------------------------------------*/
int arkLsMPSolve(void* arkode_mem, N_Vector r, N_Vector z, sunrealtype tol, int lr)
{
  ARKodeMem ark_mem;
  ARKLsMassMem arkls_mem;
  int retval;

  /* access ARKodeMem and ARKLsMassMem structures */
  retval = arkLs_AccessARKODEMassMem(arkode_mem, __func__, &ark_mem, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* call the user-supplied psolve routine, and accumulate count */
  retval = arkls_mem->psolve(ark_mem->tcur, r, z, tol, lr, arkls_mem->P_data);
  arkls_mem->nps++;
  return (retval);
}

/*---------------------------------------------------------------
  arkLsDQJac:

  This routine is a wrapper for the Dense and Band
  implementations of the difference quotient Jacobian
  approximation routines.
  ---------------------------------------------------------------*/
int arkLsDQJac(sunrealtype t, N_Vector y, N_Vector fy, SUNMatrix Jac,
               void* arkode_mem, N_Vector tmp1, N_Vector tmp2,
               SUNDIALS_MAYBE_UNUSED N_Vector tmp3)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  ARKRhsFn fi;
  int retval;

  /* access ARKodeMem and ARKLsMem structures */
  retval = arkLs_AccessARKODELMem(arkode_mem, __func__, &ark_mem, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* verify that Jac is non-NULL */
  if (Jac == NULL)
  {
    arkProcessError(ark_mem, ARKLS_LMEM_NULL, __LINE__, __func__, __FILE__,
                    "SUNMatrix is NULL");
    return (ARKLS_LMEM_NULL);
  }

  /* Access implicit RHS function */
  fi = ark_mem->step_getimplicitrhs((void*)ark_mem);
  if (fi == NULL)
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "Time step module is missing implicit RHS fcn");
    return (ARKLS_ILL_INPUT);
  }

  /* Verify that N_Vector supports required routines */
  if (ark_mem->tempv1->ops->nvcloneempty == NULL ||
      ark_mem->tempv1->ops->nvwrmsnorm == NULL ||
      ark_mem->tempv1->ops->nvlinearsum == NULL ||
      ark_mem->tempv1->ops->nvdestroy == NULL ||
      ark_mem->tempv1->ops->nvscale == NULL ||
      ark_mem->tempv1->ops->nvgetarraypointer == NULL ||
      ark_mem->tempv1->ops->nvsetarraypointer == NULL)
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    MSG_LS_BAD_NVECTOR);
    return (ARKLS_ILL_INPUT);
  }

  /* Call the matrix-structure-specific DQ approximation routine */
  if (SUNMatGetID(Jac) == SUNMATRIX_DENSE)
  {
    retval = arkLsDenseDQJac(t, y, fy, Jac, ark_mem, arkls_mem, fi, tmp1);
  }
  else if (SUNMatGetID(Jac) == SUNMATRIX_BAND)
  {
    retval = arkLsBandDQJac(t, y, fy, Jac, ark_mem, arkls_mem, fi, tmp1, tmp2);
  }
  else
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "arkLsDQJac not implemented for this SUNMatrix type!");
    retval = ARKLS_ILL_INPUT;
  }
  return (retval);
}

/*---------------------------------------------------------------
  arkLsDenseDQJac:

  This routine generates a dense difference quotient approximation
  to the Jacobian of f(t,y). It assumes a dense SUNMatrix input
  (stored column-wise, and that elements within each column are
  contiguous). The address of the jth column of J is obtained via
  the function SUNDenseMatrix_Column() and this pointer is
  associated with an N_Vector using the
  N_VGetArrayPointer/N_VSetArrayPointer functions.  Finally, the
  actual computation of the jth column of the Jacobian is done
  with a call to N_VLinearSum.
  ---------------------------------------------------------------*/
int arkLsDenseDQJac(sunrealtype t, N_Vector y, N_Vector fy, SUNMatrix Jac,
                    ARKodeMem ark_mem, ARKLsMem arkls_mem, ARKRhsFn fi,
                    N_Vector tmp1)
{
  sunrealtype fnorm, minInc, inc, inc_inv, yjsaved, srur, conj;
  sunrealtype *y_data, *ewt_data, *cns_data;
  N_Vector ftemp, jthCol;
  sunindextype j, N;
  int retval = 0;

  /* access matrix dimension */
  N = SUNDenseMatrix_Columns(Jac);

  /* Rename work vector for readability */
  ftemp = tmp1;

  /* Create an empty vector for matrix column calculations */
  jthCol = N_VCloneEmpty(tmp1);

  /* Obtain pointers to the data for various vectors */
  ewt_data = N_VGetArrayPointer(ark_mem->ewt);
  y_data   = N_VGetArrayPointer(y);
  cns_data = (ark_mem->constraintsSet) ? N_VGetArrayPointer(ark_mem->constraints)
                                       : NULL;

  /* Set minimum increment based on uround and norm of f */
  srur   = SUNRsqrt(ark_mem->uround);
  fnorm  = N_VWrmsNorm(fy, ark_mem->rwt);
  minInc = (fnorm != ZERO)
             ? (MIN_INC_MULT * SUNRabs(ark_mem->h) * ark_mem->uround * N * fnorm)
             : ONE;

  for (j = 0; j < N; j++)
  {
    /* Generate the jth col of J(tn,y) */
    N_VSetArrayPointer(SUNDenseMatrix_Column(Jac, j), jthCol);

    yjsaved = y_data[j];
    inc     = SUNMAX(srur * SUNRabs(yjsaved), minInc / ewt_data[j]);

    /* Adjust sign(inc) if y_j has an inequality constraint. */
    if (ark_mem->constraintsSet)
    {
      conj = cns_data[j];
      if (SUNRabs(conj) == ONE)
      {
        if ((yjsaved + inc) * conj < ZERO) { inc = -inc; }
      }
      else if (SUNRabs(conj) == TWO)
      {
        if ((yjsaved + inc) * conj <= ZERO) { inc = -inc; }
      }
    }

    y_data[j] += inc;

    retval = fi(t, y, ftemp, ark_mem->user_data);
    arkls_mem->nfeDQ++;
    if (retval != 0) { break; }

    y_data[j] = yjsaved;

    inc_inv = ONE / inc;
    N_VLinearSum(inc_inv, ftemp, -inc_inv, fy, jthCol);
  }

  /* Destroy jthCol vector */
  N_VSetArrayPointer(NULL, jthCol); /* SHOULDN'T BE NEEDED */
  N_VDestroy(jthCol);

  return (retval);
}

/*---------------------------------------------------------------
  arkLsBandDQJac:

  This routine generates a banded difference quotient approximation
  to the Jacobian of f(t,y).  It assumes a band SUNMatrix input
  (stored column-wise, and that elements within each column are
  contiguous). This makes it possible to get the address
  of a column of J via the function SUNBandMatrix_Column() and to
  write a simple for loop to set each of the elements of a column
  in succession.
  ---------------------------------------------------------------*/
int arkLsBandDQJac(sunrealtype t, N_Vector y, N_Vector fy, SUNMatrix Jac,
                   ARKodeMem ark_mem, ARKLsMem arkls_mem, ARKRhsFn fi,
                   N_Vector tmp1, N_Vector tmp2)
{
  N_Vector ftemp, ytemp;
  sunrealtype fnorm, minInc, inc, inc_inv, srur, conj;
  sunrealtype *col_j, *ewt_data, *fy_data, *ftemp_data, *y_data, *ytemp_data;
  sunrealtype* cns_data;
  sunindextype group, i, j, width, ngroups, i1, i2;
  sunindextype N, mupper, mlower;
  int retval = 0;

  /* access matrix dimensions */
  N      = SUNBandMatrix_Columns(Jac);
  mupper = SUNBandMatrix_UpperBandwidth(Jac);
  mlower = SUNBandMatrix_LowerBandwidth(Jac);

  /* Rename work vectors for use as temporary values of y and f */
  ftemp = tmp1;
  ytemp = tmp2;

  /* Obtain pointers to the data for ewt, fy, ftemp, y, ytemp */
  ewt_data   = N_VGetArrayPointer(ark_mem->ewt);
  fy_data    = N_VGetArrayPointer(fy);
  ftemp_data = N_VGetArrayPointer(ftemp);
  y_data     = N_VGetArrayPointer(y);
  ytemp_data = N_VGetArrayPointer(ytemp);
  cns_data = (ark_mem->constraintsSet) ? N_VGetArrayPointer(ark_mem->constraints)
                                       : NULL;

  /* Load ytemp with y = predicted y vector */
  N_VScale(ONE, y, ytemp);

  /* Set minimum increment based on uround and norm of f */
  srur   = SUNRsqrt(ark_mem->uround);
  fnorm  = N_VWrmsNorm(fy, ark_mem->rwt);
  minInc = (fnorm != ZERO)
             ? (MIN_INC_MULT * SUNRabs(ark_mem->h) * ark_mem->uround * N * fnorm)
             : ONE;

  /* Set bandwidth and number of column groups for band differencing */
  width   = mlower + mupper + 1;
  ngroups = SUNMIN(width, N);

  /* Loop over column groups. */
  for (group = 1; group <= ngroups; group++)
  {
    /* Increment all y_j in group */
    for (j = group - 1; j < N; j += width)
    {
      inc = SUNMAX(srur * SUNRabs(y_data[j]), minInc / ewt_data[j]);

      /* Adjust sign(inc) if yj has an inequality constraint. */
      if (ark_mem->constraintsSet)
      {
        conj = cns_data[j];
        if (SUNRabs(conj) == ONE)
        {
          if ((ytemp_data[j] + inc) * conj < ZERO) { inc = -inc; }
        }
        else if (SUNRabs(conj) == TWO)
        {
          if ((ytemp_data[j] + inc) * conj <= ZERO) { inc = -inc; }
        }
      }

      ytemp_data[j] += inc;
    }

    /* Evaluate f with incremented y */
    retval = fi(t, ytemp, ftemp, ark_mem->user_data);
    arkls_mem->nfeDQ++;
    if (retval != 0) { break; }

    /* Restore ytemp, then form and load difference quotients */
    for (j = group - 1; j < N; j += width)
    {
      ytemp_data[j] = y_data[j];
      col_j         = SUNBandMatrix_Column(Jac, j);
      inc           = SUNMAX(srur * SUNRabs(y_data[j]), minInc / ewt_data[j]);

      /* Adjust sign(inc) as before. */
      if (ark_mem->constraintsSet)
      {
        conj = cns_data[j];
        if (SUNRabs(conj) == ONE)
        {
          if ((ytemp_data[j] + inc) * conj < ZERO) { inc = -inc; }
        }
        else if (SUNRabs(conj) == TWO)
        {
          if ((ytemp_data[j] + inc) * conj <= ZERO) { inc = -inc; }
        }
      }

      inc_inv = ONE / inc;
      i1      = SUNMAX(0, j - mupper);
      i2      = SUNMIN(j + mlower, N - 1);
      for (i = i1; i <= i2; i++)
      {
        SM_COLUMN_ELEMENT_B(col_j, i, j) = inc_inv * (ftemp_data[i] - fy_data[i]);
      }
    }
  }

  return (retval);
}

/*---------------------------------------------------------------
  arkLsDQJtimes:

  This routine generates a difference quotient approximation to
  the Jacobian-vector product fi_y(t,y) * v. The approximation is
  Jv = [fi(y + v*sig) - fi(y)]/sig, where sig = 1 / ||v||_WRMS,
  i.e. the WRMS norm of v*sig is 1.
  ---------------------------------------------------------------*/
int arkLsDQJtimes(N_Vector v, N_Vector Jv, sunrealtype t, N_Vector y,
                  N_Vector fy, void* arkode_mem, N_Vector work)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  sunrealtype sig, siginv;
  int iter, retval;

  /* access ARKodeMem and ARKLsMem structures */
  retval = arkLs_AccessARKODELMem(arkode_mem, __func__, &ark_mem, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* Initialize perturbation to 1/||v|| */
  sig = ONE / N_VWrmsNorm(v, ark_mem->ewt);

  for (iter = 0; iter < MAX_DQITERS; iter++)
  {
    /* Set work = y + sig*v */
    N_VLinearSum(sig, v, ONE, y, work);

    /* Set Jv = f(tn, y+sig*v) */
    retval = arkls_mem->Jt_f(t, work, Jv, ark_mem->user_data);
    arkls_mem->nfeDQ++;
    if (retval == 0) { break; }
    if (retval < 0) { return (-1); }

    /* If fi failed recoverably, shrink sig and retry */
    sig *= PT25;
  }

  /* If retval still isn't 0, return with a recoverable failure */
  if (retval > 0) { return (+1); }

  /* Replace Jv by (Jv - fy)/sig */
  siginv = ONE / sig;
  N_VLinearSum(siginv, Jv, -siginv, fy, Jv);

  return (0);
}

/*-----------------------------------------------------------------
  arkLsLinSys

  Setup the linear system A = I - gamma J or A = M - gamma J
  -----------------------------------------------------------------*/
static int arkLsLinSys(sunrealtype t, N_Vector y, N_Vector fy, SUNMatrix A,
                       SUNMatrix M, sunbooleantype jok, sunbooleantype* jcur,
                       sunrealtype gamma, void* arkode_mem, N_Vector vtemp1,
                       N_Vector vtemp2, N_Vector vtemp3)
{
  ARKodeMem ark_mem;
  ARKLsMem arkls_mem;
  int retval;

  /* access ARKodeMem and ARKLsMem structures */
  retval = arkLs_AccessARKODELMem(arkode_mem, __func__, &ark_mem, &arkls_mem);
  if (retval != ARKLS_SUCCESS) { return (retval); }

  /* Check if Jacobian needs to be updated */
  if (jok)
  {
    /* Use saved copy of J */
    *jcur = SUNFALSE;

    /* Overwrite linear system matrix with saved J */
    retval = SUNMatCopy(arkls_mem->savedJ, A);
    if (retval)
    {
      arkProcessError(ark_mem, ARKLS_SUNMAT_FAIL, __LINE__, __func__, __FILE__,
                      MSG_LS_SUNMAT_FAILED);
      arkls_mem->last_flag = ARKLS_SUNMAT_FAIL;
      return (arkls_mem->last_flag);
    }
  }
  else
  {
    /* Call jac() routine to update J */
    *jcur = SUNTRUE;

    /* Clear the linear system matrix if necessary (direct linear solvers) */
    if (!(arkls_mem->iterative))
    {
      retval = SUNMatZero(A);
      if (retval)
      {
        arkProcessError(ark_mem, ARKLS_SUNMAT_FAIL, __LINE__, __func__,
                        __FILE__, MSG_LS_SUNMAT_FAILED);
        arkls_mem->last_flag = ARKLS_SUNMAT_FAIL;
        return (arkls_mem->last_flag);
      }
    }

    /* Compute new Jacobian matrix */
    retval = arkls_mem->jac(t, y, fy, A, arkls_mem->J_data, vtemp1, vtemp2,
                            vtemp3);
    if (retval < 0)
    {
      arkProcessError(ark_mem, ARKLS_JACFUNC_UNRECVR, __LINE__, __func__,
                      __FILE__, MSG_LS_JACFUNC_FAILED);
      arkls_mem->last_flag = ARKLS_JACFUNC_UNRECVR;
      return (-1);
    }
    if (retval > 0)
    {
      arkls_mem->last_flag = ARKLS_JACFUNC_RECVR;
      return (1);
    }

    /* Update saved copy of the Jacobian matrix */
    retval = SUNMatCopy(A, arkls_mem->savedJ);
    if (retval)
    {
      arkProcessError(ark_mem, ARKLS_SUNMAT_FAIL, __LINE__, __func__, __FILE__,
                      MSG_LS_SUNMAT_FAILED);
      arkls_mem->last_flag = ARKLS_SUNMAT_FAIL;
      return (arkls_mem->last_flag);
    }
  }

  /* Perform linear combination A = I - gamma*J or A = M - gamma*J */
  if (M == NULL) { retval = SUNMatScaleAddI(-gamma, A); }
  else { retval = SUNMatScaleAdd(-gamma, A, M); }

  /* Check matrix operation return value */
  if (retval)
  {
    arkProcessError(ark_mem, ARKLS_SUNMAT_FAIL, __LINE__, __func__, __FILE__,
                    MSG_LS_SUNMAT_FAILED);
    arkls_mem->last_flag = ARKLS_SUNMAT_FAIL;
    return (arkls_mem->last_flag);
  }

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  arkLsInitialize performs remaining initializations specific
  to the linear solver interface (and solver itself)
  ---------------------------------------------------------------*/
int arkLsInitialize(ARKodeMem ark_mem)
{
  ARKLsMem arkls_mem;
  ARKLsMassMem arkls_massmem;
  int retval;

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* access ARKLsMassMem (if applicable) */
  arkls_massmem = NULL;
  if (ark_mem->step_getmassmem != NULL)
  {
    if (ark_mem->step_getmassmem(ark_mem) != NULL)
    {
      retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_massmem);
      if (retval != ARK_SUCCESS) { return (retval); }
    }
  }

  /* Test for valid combinations of matrix & Jacobian routines: */
  if (arkls_mem->A != NULL)
  {
    /* Matrix-based case */

    if (!arkls_mem->user_linsys)
    {
      /* Internal linear system function, reset pointers (just in case) */
      arkls_mem->linsys = arkLsLinSys;
      arkls_mem->A_data = ark_mem;

      /* Check if an internal or user-supplied Jacobian function is used */
      if (arkls_mem->jacDQ)
      {
        /* Internal difference quotient Jacobian. Check that A is dense or band,
           otherwise return an error */
        retval = 0;
        if (arkls_mem->A->ops->getid)
        {
          if ((SUNMatGetID(arkls_mem->A) == SUNMATRIX_DENSE) ||
              (SUNMatGetID(arkls_mem->A) == SUNMATRIX_BAND))
          {
            arkls_mem->jac    = arkLsDQJac;
            arkls_mem->J_data = ark_mem;
          }
          else { retval++; }
        }
        else { retval++; }
        if (retval)
        {
          arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__,
                          __FILE__, "No Jacobian constructor available for SUNMatrix type");
          arkls_mem->last_flag = ARKLS_ILL_INPUT;
          return (ARKLS_ILL_INPUT);
        }
      }

      /* Allocate internally saved Jacobian if not already done */
      if (arkls_mem->savedJ == NULL)
      {
        arkls_mem->savedJ = SUNMatClone(arkls_mem->A);
        if (arkls_mem->savedJ == NULL)
        {
          arkProcessError(ark_mem, ARKLS_MEM_FAIL, __LINE__, __func__, __FILE__,
                          MSG_LS_MEM_FAIL);
          arkls_mem->last_flag = ARKLS_MEM_FAIL;
          return (ARKLS_MEM_FAIL);
        }
      }

    } /* end matrix-based case */
  }
  else
  {
    /* Matrix-free case: ensure 'jac' and 'linsys' function pointers are NULL */
    arkls_mem->jacDQ  = SUNFALSE;
    arkls_mem->jac    = NULL;
    arkls_mem->J_data = NULL;

    arkls_mem->user_linsys = SUNFALSE;
    arkls_mem->linsys      = NULL;
    arkls_mem->A_data      = NULL;
  }

  /* Test for valid combination of system matrix and mass matrix (if applicable) */
  if (arkls_massmem)
  {
    /* A and M must both be NULL or non-NULL */
    if ((arkls_mem->A == NULL) ^ (arkls_massmem->M == NULL))
    {
      arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__,
                      __FILE__, "Cannot combine NULL and non-NULL System and mass matrices");
      arkls_mem->last_flag = ARKLS_ILL_INPUT;
      return (ARKLS_ILL_INPUT);
    }

    /* If A is non-NULL, A and M must have matching types (if accessible) */
    if (arkls_mem->A)
    {
      retval = 0;
      if ((arkls_mem->A->ops->getid == NULL) ^
          (arkls_massmem->M->ops->getid == NULL))
      {
        retval++;
      }
      if (arkls_mem->A->ops->getid)
      {
        if (SUNMatGetID(arkls_mem->A) != SUNMatGetID(arkls_massmem->M))
        {
          retval++;
        }
      }
      if (retval)
      {
        arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                        "System and mass matrices have incompatible types");
        arkls_mem->last_flag = ARKLS_ILL_INPUT;
        return (ARKLS_ILL_INPUT);
      }
    }

    /* If either system or mass matrix solver is matrix-embedded, then both must be */
    if ((SUNLinSolGetType(arkls_mem->LS) == SUNLINEARSOLVER_MATRIX_EMBEDDED) &&
        (SUNLinSolGetType(arkls_massmem->LS) != SUNLINEARSOLVER_MATRIX_EMBEDDED))
    {
      arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__,
                      __FILE__, "mismatched matrix-embedded LS types (system and mass must match)");
      arkls_mem->last_flag = ARKLS_ILL_INPUT;
      return (ARKLS_ILL_INPUT);
    }
    if ((SUNLinSolGetType(arkls_mem->LS) != SUNLINEARSOLVER_MATRIX_EMBEDDED) &&
        (SUNLinSolGetType(arkls_massmem->LS) == SUNLINEARSOLVER_MATRIX_EMBEDDED))
    {
      arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__,
                      __FILE__, "mismatched matrix-embedded LS types (system and mass must match)");
      arkls_mem->last_flag = ARKLS_ILL_INPUT;
      return (ARKLS_ILL_INPUT);
    }
  }

  /* reset counters */
  arkLsInitializeCounters(arkls_mem);

  /* Set Jacobian-vector product related fields, based on jtimesDQ */
  if (arkls_mem->jtimesDQ)
  {
    arkls_mem->jtsetup = NULL;
    arkls_mem->jtimes  = arkLsDQJtimes;
    arkls_mem->Jt_data = ark_mem;
  }

  /* If A is NULL and psetup is not present, then arkLsSetup does
     not need to be called, so set the lsetup function to NULL (if possible) */
  if ((arkls_mem->A == NULL) && (arkls_mem->pset == NULL) &&
      (ark_mem->step_disablelsetup != NULL))
  {
    ark_mem->step_disablelsetup(ark_mem);
  }

  /* When using a matrix-embedded linear solver, disable lsetup call and solution scaling */
  if (SUNLinSolGetType(arkls_mem->LS) == SUNLINEARSOLVER_MATRIX_EMBEDDED)
  {
    ark_mem->step_disablelsetup(ark_mem);
    arkls_mem->scalesol = SUNFALSE;
  }

  /* Call LS initialize routine, and return result */
  arkls_mem->last_flag = SUNLinSolInitialize(arkls_mem->LS);
  return (arkls_mem->last_flag);
}

/*---------------------------------------------------------------
  arkLsSetup conditionally calls the LS 'setup' routine.

  When using a SUNMatrix object, this determines whether
  to update a Jacobian matrix (or use a stored version), based
  on heuristics regarding previous convergence issues, the number
  of time steps since it was last updated, etc.; it then creates
  the system matrix from this, the 'gamma' factor and the
  mass/identity matrix,
  A = M-gamma*J.

  This routine then calls the LS 'setup' routine with A.
  ---------------------------------------------------------------*/
int arkLsSetup(ARKodeMem ark_mem, int convfail, sunrealtype tpred,
               N_Vector ypred, N_Vector fpred, sunbooleantype* jcurPtr,
               N_Vector vtemp1, N_Vector vtemp2, N_Vector vtemp3)
{
  ARKLsMem arkls_mem     = NULL;
  void* ark_step_massmem = NULL;
  SUNMatrix M            = NULL;
  sunrealtype gamma, gamrat;
  sunbooleantype dgamma_fail, *jcur;
  int retval;

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* Immediately return when using matrix-embedded linear solver */
  if (SUNLinSolGetType(arkls_mem->LS) == SUNLINEARSOLVER_MATRIX_EMBEDDED)
  {
    arkls_mem->last_flag = ARKLS_SUCCESS;
    return (arkls_mem->last_flag);
  }

  /* Set ARKLs time and N_Vector pointers to current time,
     solution and rhs */
  arkls_mem->tcur = tpred;
  arkls_mem->ycur = ypred;
  arkls_mem->fcur = fpred;

  /* get gamma values from time step module */
  arkls_mem->last_flag = ark_mem->step_getgammas(ark_mem, &gamma, &gamrat,
                                                 &jcur, &dgamma_fail);
  if (arkls_mem->last_flag)
  {
    arkProcessError(ark_mem, arkls_mem->last_flag, __LINE__, __func__, __FILE__,
                    "An error occurred in ark_step_getgammas");
    return (arkls_mem->last_flag);
  }

  /* Use initsetup, gamma/gammap, and convfail to set J/P eval. flag jok;
     Note: the "ARK_FAIL_BAD_J" test is asking whether the nonlinear
     solver converged due to a bad system Jacobian AND our gamma was
     fine, indicating that the J and/or P were invalid */
  arkls_mem->jbad = (ark_mem->initsetup) ||
                    (ark_mem->nst >= arkls_mem->nstlj + arkls_mem->msbj) ||
                    ((convfail == ARK_FAIL_BAD_J) && (!dgamma_fail)) ||
                    (convfail == ARK_FAIL_OTHER);

  /* Check for mass matrix module and setup mass matrix */
  if (ark_mem->step_getmassmem)
  {
    ark_step_massmem = ark_mem->step_getmassmem(ark_mem);
  }

  if (ark_step_massmem)
  {
    /* Set shortcut to the mass matrix (NULL if matrix-free) */
    M = ((ARKLsMassMem)ark_step_massmem)->M;

    /* Setup mass matrix linear solver (including recomputation of mass matrix) */
    arkls_mem->last_flag = arkLsMassSetup(ark_mem, tpred, vtemp1, vtemp2, vtemp3);
    if (arkls_mem->last_flag)
    {
      arkProcessError(ark_mem, ARKLS_SUNMAT_FAIL, __LINE__, __func__, __FILE__,
                      "Error setting up mass-matrix linear solver");
      return (arkls_mem->last_flag);
    }
  }

  /* Setup the linear system if necessary */
  if (arkls_mem->A != NULL)
  {
    /* Update J if appropriate and evaluate A = I-gamma*J or A = M-gamma*J */
    retval = arkls_mem->linsys(tpred, ypred, fpred, arkls_mem->A, M,
                               !(arkls_mem->jbad), jcurPtr, gamma,
                               arkls_mem->A_data, vtemp1, vtemp2, vtemp3);

    /* Update J eval count and step when J was last updated */
    if (*jcurPtr)
    {
      arkls_mem->nje++;
      arkls_mem->nstlj = ark_mem->nst;
      arkls_mem->tnlj  = tpred;
    }

    /* Check linsys() return value and return if necessary */
    if (retval != ARKLS_SUCCESS)
    {
      if (arkls_mem->user_linsys)
      {
        if (retval < 0)
        {
          arkProcessError(ark_mem, ARKLS_JACFUNC_UNRECVR, __LINE__, __func__,
                          __FILE__, MSG_LS_JACFUNC_FAILED);
          arkls_mem->last_flag = ARKLS_JACFUNC_UNRECVR;
          return (-1);
        }
        else
        {
          arkls_mem->last_flag = ARKLS_JACFUNC_RECVR;
          return (1);
        }
      }
      else { return (retval); }
    }
  }
  else
  {
    /* Matrix-free case, set jcur to jbad */
    *jcurPtr = arkls_mem->jbad;
  }

  /* Call LS setup routine -- the LS may call arkLsPSetup, who will
     pass the heuristic suggestions above to the user code(s) */
  arkls_mem->last_flag = SUNLinSolSetup(arkls_mem->LS, arkls_mem->A);

  /* If the SUNMatrix was NULL, update heuristics flags */
  if (arkls_mem->A == NULL)
  {
    /* If user set jcur to SUNTRUE, increment npe and save nst value */
    if (*jcurPtr)
    {
      arkls_mem->npe++;
      arkls_mem->nstlj = ark_mem->nst;
      arkls_mem->tnlj  = tpred;
    }

    /* Update jcurPtr flag if we suggested an update */
    if (arkls_mem->jbad) { *jcurPtr = SUNTRUE; }
  }

  return (arkls_mem->last_flag);
}

/*---------------------------------------------------------------
  arkLsSolve: interfaces between ARKODE and the generic
  SUNLinearSolver object LS, by setting the appropriate tolerance
  and scaling vectors, calling the solver, and accumulating
  statistics from the solve for use/reporting by ARKODE.

  When using a non-NULL SUNMatrix, this will additionally scale
  the solution appropriately when gamrat != 1.
  ---------------------------------------------------------------*/
int arkLsSolve(ARKodeMem ark_mem, N_Vector b, sunrealtype tnow, N_Vector ynow,
               N_Vector fnow, sunrealtype eRNrm, int mnewt)
{
  sunrealtype bnorm;
  ARKLsMem arkls_mem;
  sunrealtype gamma, gamrat, delta, deltar, rwt_mean;
  sunbooleantype dgamma_fail, *jcur;
  int nli_inc, retval;

  /* used when logging is enabled */
  SUNDIALS_MAYBE_UNUSED long int nps_inc;
  SUNDIALS_MAYBE_UNUSED sunrealtype resnorm;

  /* access ARKLsMem structure */
  retval = arkLs_AccessLMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* Set scalar tcur and vectors ycur and fcur for use by the
     Atimes and Psolve interface routines */
  arkls_mem->tcur = tnow;
  arkls_mem->ycur = ynow;
  arkls_mem->fcur = fnow;

  /* If the linear solver is iterative:
     test norm(b), if small, return x = 0 or x = b;
     set linear solver tolerance (in left/right scaled 2-norm) */
  if (arkls_mem->iterative)
  {
    deltar = arkls_mem->eplifac * eRNrm;
    bnorm  = N_VWrmsNorm(b, ark_mem->rwt);

    SUNLogInfo(ARK_LOGGER, "begin-linear-solve",
               "iterative = 1, b-norm = %.16g, b-tol = %.16g, res-tol = %.16g",
               bnorm, deltar, deltar * arkls_mem->nrmfac);

    if (bnorm <= deltar)
    {
      if (mnewt > 0) { N_VConst(ZERO, b); }
      arkls_mem->last_flag = ARKLS_SUCCESS;

      SUNLogInfo(ARK_LOGGER, "end-linear-solve", "status = success small rhs",
                 "");

      return (arkls_mem->last_flag);
    }
    /* Adjust tolerance for 2-norm */
    delta = deltar * arkls_mem->nrmfac;
  }
  else
  {
    delta = bnorm = ZERO;

    SUNLogInfo(ARK_LOGGER, "begin-linear-solve", "iterative = 0");
  }

  /* Set scaling vectors for LS to use (if applicable) */
  if (arkls_mem->LS->ops->setscalingvectors)
  {
    retval = SUNLinSolSetScalingVectors(arkls_mem->LS, ark_mem->rwt,
                                        ark_mem->ewt);
    if (retval != SUN_SUCCESS)
    {
      arkProcessError(ark_mem, ARKLS_SUNLS_FAIL, __LINE__, __func__, __FILE__,
                      "Error in call to SUNLinSolSetScalingVectors");
      arkls_mem->last_flag = ARKLS_SUNLS_FAIL;

      SUNLogInfo(ARK_LOGGER, "end-linear-solve",
                 "status = failed set scaling vectors");

      return (arkls_mem->last_flag);
    }

    /* If solver is iterative and does not support scaling vectors, update the
     tolerance in an attempt to account for ewt/rwt vectors.  We make the
     following assumptions:
       1. rwt_i = rwt_mean, for i=0,...,n-1 (i.e. the residual units are identical)
       2. the linear solver uses a basic 2-norm to measure convergence
     Hence (using the notation from sunlinsol_spgmr.h, with S = diag(rwt)),
           || bbar - Abar xbar ||_2 < tol
       <=> || S b - S A x ||_2 < tol
       <=> || S (b - A x) ||_2 < tol
       <=> \sum_{i=0}^{n-1} (rwt_i (b - A x)_i)^2 < tol^2
       <=> rwt_mean^2 \sum_{i=0}^{n-1} (b - A x_i)^2 < tol^2
       <=> \sum_{i=0}^{n-1} (b - A x_i)^2 < tol^2 / rwt_mean^2
       <=> || b - A x ||_2 < tol / rwt_mean
     So we compute rwt_mean = ||rwt||_RMS and scale the desired tolerance accordingly. */
  }
  else if (arkls_mem->iterative)
  {
    N_VConst(ONE, arkls_mem->x);
    rwt_mean = N_VWrmsNorm(ark_mem->rwt, arkls_mem->x);
    delta /= rwt_mean;
  }

  /* Set initial guess x = 0 to LS */
  N_VConst(ZERO, arkls_mem->x);

  /* Set zero initial guess flag */
  retval = SUNLinSolSetZeroGuess(arkls_mem->LS, SUNTRUE);
  if (retval != SUN_SUCCESS)
  {
    SUNLogInfo(ARK_LOGGER, "end-linear-solve", "status = failed set zero guess",
               "");
    return (-1);
  }

  /* Store previous nps value in nps_inc */
  nps_inc = arkls_mem->nps;

  /* If a user-provided jtsetup routine is supplied, call that here */
  if (arkls_mem->jtsetup)
  {
    arkls_mem->last_flag = arkls_mem->jtsetup(tnow, ynow, fnow,
                                              arkls_mem->Jt_data);
    arkls_mem->njtsetup++;
    if (arkls_mem->last_flag)
    {
      arkProcessError(ark_mem, arkls_mem->last_flag, __LINE__, __func__,
                      __FILE__, MSG_LS_JTSETUP_FAILED);

      SUNLogInfo(ARK_LOGGER, "end-linear-solve", "status = failed J-times setup");
      return (arkls_mem->last_flag);
    }
  }

  /* Call solver, and copy x to b */
  retval = SUNLinSolSolve(arkls_mem->LS, arkls_mem->A, arkls_mem->x, b, delta);
  N_VScale(ONE, arkls_mem->x, b);

  /* If using a direct or matrix-iterative solver, scale the correction to
     account for change in gamma (this is only beneficial if M==I) */
  if (arkls_mem->scalesol)
  {
    arkls_mem->last_flag = ark_mem->step_getgammas(ark_mem, &gamma, &gamrat,
                                                   &jcur, &dgamma_fail);
    if (arkls_mem->last_flag != ARK_SUCCESS)
    {
      arkProcessError(ark_mem, arkls_mem->last_flag, __LINE__, __func__,
                      __FILE__, "An error occurred in ark_step_getgammas");
      return (arkls_mem->last_flag);
    }
    if (gamrat != ONE) { N_VScale(TWO / (ONE + gamrat), b, b); }
  }

  /* Retrieve statistics from iterative linear solvers */
  resnorm = ZERO;
  nli_inc = 0;
  if (arkls_mem->iterative)
  {
    if (arkls_mem->LS->ops->resnorm)
    {
      resnorm = SUNLinSolResNorm(arkls_mem->LS);
    }
    if (arkls_mem->LS->ops->numiters)
    {
      nli_inc = SUNLinSolNumIters(arkls_mem->LS);
    }
  }

  /* Increment counters nli and ncfl */
  arkls_mem->nli += nli_inc;
  if (retval != SUN_SUCCESS) { arkls_mem->ncfl++; }

  /* Interpret solver return value  */
  arkls_mem->last_flag = retval;

  SUNLogInfoIf(retval == SUN_SUCCESS, ARK_LOGGER, "end-linear-solve",
               "status = success, iters = %i, p-solves = %i, resnorm = %.16g",
               nli_inc, (int)(arkls_mem->nps - nps_inc), resnorm);
  SUNLogInfoIf(retval != SUN_SUCCESS, ARK_LOGGER,
               "end-linear-solve", "status = failed, retval = %i, iters = %i, p-solves = %i, resnorm = %.16g",
               retval, nli_inc, (int)(arkls_mem->nps - nps_inc), resnorm);

  switch (retval)
  {
  case SUN_SUCCESS: return (0); break;
  case SUNLS_RES_REDUCED:
    /* allow reduction but not solution on first nonlinear iteration,
       otherwise return with a recoverable failure */
    if (mnewt == 0) { return (0); }
    else { return (1); }
    break;
  case SUNLS_CONV_FAIL:
  case SUNLS_ATIMES_FAIL_REC:
  case SUNLS_PSOLVE_FAIL_REC:
  case SUNLS_PACKAGE_FAIL_REC:
  case SUNLS_QRFACT_FAIL:
  case SUNLS_LUFACT_FAIL: return (1); break;
  case SUN_ERR_ARG_CORRUPT:
  case SUN_ERR_ARG_INCOMPATIBLE:
  case SUN_ERR_MEM_FAIL:
  case SUNLS_GS_FAIL:
  case SUNLS_QRSOL_FAIL: return (-1); break;
  case SUN_ERR_EXT_FAIL:
    arkProcessError(ark_mem, SUN_ERR_EXT_FAIL, __LINE__, __func__, __FILE__,
                    "Failure in SUNLinSol external package");
    return (-1);
    break;
  case SUNLS_ATIMES_FAIL_UNREC:
    arkProcessError(ark_mem, SUNLS_ATIMES_FAIL_UNREC, __LINE__, __func__,
                    __FILE__, MSG_LS_JTIMES_FAILED);
    return (-1);
    break;
  case SUNLS_PSOLVE_FAIL_UNREC:
    arkProcessError(ark_mem, SUNLS_PSOLVE_FAIL_UNREC, __LINE__, __func__,
                    __FILE__, MSG_LS_PSOLVE_FAILED);
    return (-1);
    break;
  }

  return (0);
}

/*---------------------------------------------------------------
  arkLsFree frees memory associates with the ARKLs system
  solver interface.
  ---------------------------------------------------------------*/
int arkLsFree(ARKodeMem ark_mem)
{
  ARKLsMem arkls_mem;
  void* ark_step_lmem;

  /* Return immediately if ARKodeMem, ARKLsMem are NULL */
  if (ark_mem == NULL) { return (ARKLS_SUCCESS); }
  ark_step_lmem = ark_mem->step_getlinmem(ark_mem);
  if (ark_step_lmem == NULL) { return (ARKLS_SUCCESS); }
  arkls_mem = (ARKLsMem)ark_step_lmem;

  /* Free N_Vector memory */
  if (arkls_mem->ytemp)
  {
    N_VDestroy(arkls_mem->ytemp);
    arkls_mem->ytemp = NULL;
  }
  if (arkls_mem->x)
  {
    N_VDestroy(arkls_mem->x);
    arkls_mem->x = NULL;
  }

  /* Free savedJ memory */
  if (arkls_mem->savedJ)
  {
    SUNMatDestroy(arkls_mem->savedJ);
    arkls_mem->savedJ = NULL;
  }

  /* Nullify other N_Vector pointers */
  arkls_mem->ycur = NULL;
  arkls_mem->fcur = NULL;

  /* Nullify other SUNMatrix pointer */
  arkls_mem->A = NULL;

  /* Free preconditioner memory (if applicable) */
  if (arkls_mem->pfree) { arkls_mem->pfree(ark_mem); }

  /* free ARKLs interface structure */
  free(arkls_mem);

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  arkLsMassInitialize performs remaining initializations specific
  to the mass matrix solver interface (and solver itself)
  ---------------------------------------------------------------*/
int arkLsMassInitialize(ARKodeMem ark_mem)
{
  ARKLsMassMem arkls_mem;
  int retval;

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* reset counters */
  arkLsInitializeMassCounters(arkls_mem);

  /* perform checks for matrix-based mass system */
  if (arkls_mem->M != NULL)
  {
    /* check for user-provided mass matrix constructor */
    if (arkls_mem->mass == NULL)
    {
      arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                      "Missing user-provided mass-matrix routine");
      arkls_mem->last_flag = ARKLS_ILL_INPUT;
      return (arkls_mem->last_flag);
    }
    /* check that someone can perform matrix-vector product */
    if ((arkls_mem->mtimes == NULL) && (arkls_mem->M->ops->matvec == NULL))
    {
      arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                      "No available mass matrix-vector product routine");
      arkls_mem->last_flag = ARKLS_ILL_INPUT;
      return (arkls_mem->last_flag);
    }
  }

  /* perform checks for matrix-free mass system */
  if ((arkls_mem->M == NULL) && (arkls_mem->mtimes == NULL) &&
      (SUNLinSolGetType(arkls_mem->LS) != SUNLINEARSOLVER_MATRIX_EMBEDDED))
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "Missing user-provided mass matrix-vector product routine");
    arkls_mem->last_flag = ARKLS_ILL_INPUT;
    return (arkls_mem->last_flag);
  }

  /* ensure that a mass matrix solver exists */
  if (arkls_mem->LS == NULL)
  {
    arkProcessError(ark_mem, ARKLS_ILL_INPUT, __LINE__, __func__, __FILE__,
                    "Missing SUNLinearSolver object");
    arkls_mem->last_flag = ARKLS_ILL_INPUT;
    return (arkls_mem->last_flag);
  }

  /* if M is NULL and neither pset or mtsetup are present, then
     arkLsMassSetup does not need to be called, so set the
     msetup function to NULL */
  if ((arkls_mem->M == NULL) && (arkls_mem->pset == NULL) &&
      (arkls_mem->mtsetup == NULL) && (ark_mem->step_disablemsetup != NULL))
  {
    ark_mem->step_disablemsetup(ark_mem);
  }

  /* When using a matrix-embedded linear solver, disable lsetup call */
  if (SUNLinSolGetType(arkls_mem->LS) == SUNLINEARSOLVER_MATRIX_EMBEDDED)
  {
    ark_mem->step_disablemsetup(ark_mem);
  }

  /* Call LS initialize routine */
  arkls_mem->last_flag = SUNLinSolInitialize(arkls_mem->LS);
  return (arkls_mem->last_flag);
}

/*---------------------------------------------------------------
  arkLsMassSetup calls the LS 'setup' routine.
  ---------------------------------------------------------------*/
int arkLsMassSetup(ARKodeMem ark_mem, sunrealtype t, N_Vector vtemp1,
                   N_Vector vtemp2, N_Vector vtemp3)
{
  ARKLsMassMem arkls_mem;
  sunbooleantype call_mtsetup, call_mvsetup, call_lssetup;
  int retval;

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* Immediately return when using matrix-embedded linear solver */
  if (SUNLinSolGetType(arkls_mem->LS) == SUNLINEARSOLVER_MATRIX_EMBEDDED)
  {
    arkls_mem->last_flag = ARKLS_SUCCESS;
    return (arkls_mem->last_flag);
  }

  /* if the most recent setup essentially matches the current time,
     just return with success */
  if (SUNRabs(arkls_mem->msetuptime - t) < FUZZ_FACTOR * ark_mem->uround)
  {
    arkls_mem->last_flag = ARKLS_SUCCESS;
    return (arkls_mem->last_flag);
  }

  /* Determine whether to call user-provided mtsetup routine */
  call_mtsetup = SUNFALSE;
  if ((arkls_mem->mtsetup) &&
      (arkls_mem->time_dependent || (arkls_mem->nmtsetup == 0)))
  {
    call_mtsetup = SUNTRUE;
  }

  /* call user-provided mtsetup routine if applicable */
  if (call_mtsetup)
  {
    arkls_mem->last_flag = arkls_mem->mtsetup(t, arkls_mem->mt_data);
    arkls_mem->nmtsetup++;
    arkls_mem->msetuptime = t;
    if (arkls_mem->last_flag != 0)
    {
      arkProcessError(ark_mem, arkls_mem->last_flag, __LINE__, __func__,
                      __FILE__, MSG_LS_MTSETUP_FAILED);
      return (arkls_mem->last_flag);
    }
  }

  /* Perform user-facing setup based on whether this is matrix-free */
  if (arkls_mem->M == NULL)
  {
    /*** matrix-free -- only call LS setup if preconditioner setup exists ***/
    call_lssetup = (arkls_mem->pset != NULL);
    /*** matrix-free -- dont call matvec setup ***/
    call_mvsetup = SUNFALSE;
  }
  else
  {
    /*** matrix-based ***/

    /* If mass matrix is not time dependent, and if it has been set up
       previously, then just reuse existing matrix and factorization */
    if (!arkls_mem->time_dependent && (arkls_mem->nmsetups > 0))
    {
      arkls_mem->last_flag = ARKLS_SUCCESS;
      return (arkls_mem->last_flag);
    }

    /* Clear the mass matrix if necessary (direct linear solvers) */
    if (!(arkls_mem->iterative))
    {
      retval = SUNMatZero(arkls_mem->M);
      if (retval)
      {
        arkProcessError(ark_mem, ARKLS_SUNMAT_FAIL, __LINE__, __func__,
                        __FILE__, MSG_LS_SUNMAT_FAILED);
        arkls_mem->last_flag = ARKLS_SUNMAT_FAIL;
        return (arkls_mem->last_flag);
      }
    }

    /* Call user-supplied routine to fill the mass matrix */
    retval = arkls_mem->mass(t, arkls_mem->M, arkls_mem->M_data, vtemp1, vtemp2,
                             vtemp3);
    arkls_mem->msetuptime = t;
    if (retval < 0)
    {
      arkProcessError(ark_mem, ARKLS_MASSFUNC_UNRECVR, __LINE__, __func__,
                      __FILE__, MSG_LS_MASSFUNC_FAILED);
      arkls_mem->last_flag = ARKLS_MASSFUNC_UNRECVR;
      return (-1);
    }
    if (retval > 0)
    {
      arkls_mem->last_flag = ARKLS_MASSFUNC_RECVR;
      return (1);
    }

    /* Copy M into M_lu for factorization (direct linear solvers) */
    if (!(arkls_mem->iterative))
    {
      retval = SUNMatCopy(arkls_mem->M, arkls_mem->M_lu);
      if (retval)
      {
        arkProcessError(ark_mem, ARKLS_SUNMAT_FAIL, __LINE__, __func__,
                        __FILE__, MSG_LS_SUNMAT_FAILED);
        arkls_mem->last_flag = ARKLS_SUNMAT_FAIL;
        return (arkls_mem->last_flag);
      }
    }

    /* signal call to matvec setup routine only if the user didn't provide
       mtimes and the SUNMatrix implements the matvecsetup routine */
    if ((!arkls_mem->mtimes) && (arkls_mem->M->ops->matvecsetup))
    {
      call_mvsetup = SUNTRUE;
    }
    else { call_mvsetup = SUNFALSE; }

    /* signal call to LS setup routine */
    call_lssetup = SUNTRUE;
  }

  /* Call matvec setup routine if applicable */
  if (call_mvsetup)
  {
    retval = SUNMatMatvecSetup(arkls_mem->M);
    arkls_mem->nmvsetup++;
    if (retval)
    {
      arkProcessError(ark_mem, ARKLS_SUNMAT_FAIL, __LINE__, __func__, __FILE__,
                      MSG_LS_SUNMAT_FAILED);
      arkls_mem->last_flag = ARKLS_SUNMAT_FAIL;
      return (arkls_mem->last_flag);
    }
  }

  /* Call LS setup routine if applicable, and return */
  if (call_lssetup)
  {
    arkls_mem->last_flag = SUNLinSolSetup(arkls_mem->LS, arkls_mem->M_lu);
    arkls_mem->nmsetups++;
  }

  return (arkls_mem->last_flag);
}

/*---------------------------------------------------------------
  arkLsMassSolve: interfaces between ARKODE and the generic
  SUNLinearSolver object LS, by setting the appropriate tolerance
  and scaling vectors, calling the solver, and accumulating
  statistics from the solve for use/reporting by ARKODE.
  ---------------------------------------------------------------*/
int arkLsMassSolve(ARKodeMem ark_mem, N_Vector b, sunrealtype nlscoef)
{
  sunrealtype delta, rwt_mean;
  ARKLsMassMem arkls_mem;
  int nli_inc, retval;

  /* used when logging is enabled */
  SUNDIALS_MAYBE_UNUSED long int nps_inc;
  SUNDIALS_MAYBE_UNUSED sunrealtype resnorm;

  /* access ARKLsMassMem structure */
  retval = arkLs_AccessMassMem(ark_mem, __func__, &arkls_mem);
  if (retval != ARK_SUCCESS) { return (retval); }

  /* Set input tolerance for iterative solvers (in 2-norm) */
  if (arkls_mem->iterative)
  {
    delta = arkls_mem->eplifac * nlscoef * arkls_mem->nrmfac;

    SUNLogInfo(ARK_LOGGER, "begin-mass-linear-solve",
               "iterative = 1, res-tol = %.16g", delta);
  }
  else
  {
    delta = ZERO;

    SUNLogInfo(ARK_LOGGER, "begin-mass-linear-solve", "iterative = 0");
  }

  /* Set initial guess x = 0 for LS */
  N_VConst(ZERO, arkls_mem->x);

  /* Set scaling vectors for LS to use (if applicable) */
  if (arkls_mem->LS->ops->setscalingvectors)
  {
    retval = SUNLinSolSetScalingVectors(arkls_mem->LS, ark_mem->rwt,
                                        ark_mem->ewt);
    if (retval != SUN_SUCCESS)
    {
      arkProcessError(ark_mem, ARKLS_SUNLS_FAIL, __LINE__, __func__, __FILE__,
                      "Error in call to SUNLinSolSetScalingVectors");
      arkls_mem->last_flag = ARKLS_SUNLS_FAIL;

      SUNLogInfo(ARK_LOGGER, "end-mass-linear-solve",
                 "status = failed set scaling vectors");
      return (arkls_mem->last_flag);
    }

    /* If solver is iterative and does not support scaling vectors, update the
     tolerance in an attempt to account for rwt vector.  We make the
     following assumptions:
       1. rwt_i = rwt_mean, for i=0,...,n-1 (i.e. the solution units are identical)
       2. the linear solver uses a basic 2-norm to measure convergence
     Hence (using the notation from sunlinsol_spgmr.h, with S = diag(rwt)),
           || bbar - Abar xbar ||_2 < tol
       <=> || S b - S A x ||_2 < tol
       <=> || S (b - A x) ||_2 < tol
       <=> \sum_{i=0}^{n-1} (rwt_i (b - A x)_i)^2 < tol^2
       <=> rwt_mean^2 \sum_{i=0}^{n-1} (b - A x_i)^2 < tol^2
       <=> \sum_{i=0}^{n-1} (b - A x_i)^2 < tol^2 / rwt_mean^2
       <=> || b - A x ||_2 < tol / rwt_mean
     So we compute rwt_mean = ||rwt||_RMS and scale the desired tolerance accordingly. */
  }
  else if (arkls_mem->iterative)
  {
    N_VConst(ONE, arkls_mem->x);
    rwt_mean = N_VWrmsNorm(ark_mem->rwt, arkls_mem->x);
    delta /= rwt_mean;
  }

  /* Set initial guess x = 0 for LS */
  N_VConst(ZERO, arkls_mem->x);

  /* Set zero initial guess flag */
  retval = SUNLinSolSetZeroGuess(arkls_mem->LS, SUNTRUE);
  if (retval != SUN_SUCCESS)
  {
    SUNLogInfo(ARK_LOGGER, "end-mass-linear-solve",
               "status = failed set zero guess");
    return (-1);
  }

  /* Store previous nps value in nps_inc */
  nps_inc = arkls_mem->nps;

  /* Call solver, copy x to b, and increment mass solver counter */
  retval = SUNLinSolSolve(arkls_mem->LS, arkls_mem->M_lu, arkls_mem->x, b, delta);
  N_VScale(ONE, arkls_mem->x, b);
  arkls_mem->nmsolves++;

  /* Retrieve statistics from iterative linear solvers */
  resnorm = ZERO;
  nli_inc = 0;
  if (arkls_mem->iterative)
  {
    if (arkls_mem->LS->ops->resnorm)
    {
      resnorm = SUNLinSolResNorm(arkls_mem->LS);
    }
    if (arkls_mem->LS->ops->numiters)
    {
      nli_inc = SUNLinSolNumIters(arkls_mem->LS);
    }
  }

  /* Increment counters nli and ncfl */
  arkls_mem->nli += nli_inc;
  if (retval != SUN_SUCCESS) { arkls_mem->ncfl++; }

  SUNLogInfoIf(retval == SUN_SUCCESS, ARK_LOGGER, "end-mass-linear-solve",
               "status = success, iters = %i, p-solves = %i, res-norm = %.16g",
               nli_inc, (int)(arkls_mem->nps - nps_inc), resnorm);
  SUNLogInfoIf(retval != SUN_SUCCESS, ARK_LOGGER,
               "end-mass-linear-solve", "status = failed, retval = %i, iters = %i, p-solves = %i, res-norm = %.16g",
               retval, nli_inc, (int)(arkls_mem->nps - nps_inc), resnorm);

  /* Interpret solver return value  */
  arkls_mem->last_flag = retval;

  switch (retval)
  {
  case SUN_SUCCESS: return (0); break;
  case SUNLS_RES_REDUCED:
  case SUNLS_CONV_FAIL:
  case SUNLS_ATIMES_FAIL_REC:
  case SUNLS_PSOLVE_FAIL_REC:
  case SUNLS_PACKAGE_FAIL_REC:
  case SUNLS_QRFACT_FAIL:
  case SUNLS_LUFACT_FAIL: return (1); break;
  case SUN_ERR_ARG_CORRUPT:
  case SUN_ERR_ARG_INCOMPATIBLE:
  case SUN_ERR_MEM_FAIL:
  case SUNLS_GS_FAIL:
  case SUNLS_QRSOL_FAIL: return (-1); break;
  case SUN_ERR_EXT_FAIL:
    arkProcessError(ark_mem, SUN_ERR_EXT_FAIL, __LINE__, __func__, __FILE__,
                    "Failure in SUNLinSol external package");
    return (-1);
    break;
  case SUNLS_ATIMES_FAIL_UNREC:
    arkProcessError(ark_mem, SUNLS_ATIMES_FAIL_UNREC, __LINE__, __func__,
                    __FILE__, MSG_LS_MTIMES_FAILED);
    return (-1);
    break;
  case SUNLS_PSOLVE_FAIL_UNREC:
    arkProcessError(ark_mem, SUNLS_PSOLVE_FAIL_UNREC, __LINE__, __func__,
                    __FILE__, MSG_LS_PSOLVE_FAILED);
    return (-1);
    break;
  }

  return (0);
}

/*---------------------------------------------------------------
  arkLsMassFree frees memory associates with the ARKLs mass
  matrix solver interface.
  ---------------------------------------------------------------*/
int arkLsMassFree(ARKodeMem ark_mem)
{
  ARKLsMassMem arkls_mem;
  void* ark_step_massmem;

  /* Return immediately if ARKodeMem, ARKLsMassMem are NULL */
  if (ark_mem == NULL) { return (ARKLS_SUCCESS); }
  ark_step_massmem = ark_mem->step_getmassmem(ark_mem);
  if (ark_step_massmem == NULL) { return (ARKLS_SUCCESS); }
  arkls_mem = (ARKLsMassMem)ark_step_massmem;

  /* detach ARKLs interface routines from LS object (ignore return values) */
  if (arkls_mem->LS)
  {
    if (arkls_mem->LS->ops)
    {
      if (arkls_mem->LS->ops->setatimes)
      {
        SUNLinSolSetATimes(arkls_mem->LS, NULL, NULL);
      }

      if (arkls_mem->LS->ops->setpreconditioner)
      {
        SUNLinSolSetPreconditioner(arkls_mem->LS, NULL, NULL, NULL);
      }
    }
  }

  /* Free N_Vector memory */
  if (arkls_mem->x)
  {
    N_VDestroy(arkls_mem->x);
    arkls_mem->x = NULL;
  }

  /* Free M_lu memory (direct linear solvers) */
  if (!(arkls_mem->iterative) && arkls_mem->M_lu)
  {
    SUNMatDestroy(arkls_mem->M_lu);
  }
  arkls_mem->M_lu = NULL;

  /* Nullify other N_Vector pointers */
  arkls_mem->ycur = NULL;

  /* Nullify other SUNMatrix pointer */
  arkls_mem->M = NULL;

  /* Free preconditioner memory (if applicable) */
  if (arkls_mem->pfree) { arkls_mem->pfree(ark_mem); }

  /* free ARKLs interface structure */
  free(arkls_mem);

  return (ARKLS_SUCCESS);
}

/*---------------------------------------------------------------
  arkLsInitializeCounters and arkLsInitializeMassCounters:

  These routines reset all counters from an ARKLsMem or
  ARKLsMassMem structure.
  ---------------------------------------------------------------*/
int arkLsInitializeCounters(ARKLsMem arkls_mem)
{
  arkls_mem->nje      = 0;
  arkls_mem->nfeDQ    = 0;
  arkls_mem->nstlj    = 0;
  arkls_mem->npe      = 0;
  arkls_mem->nli      = 0;
  arkls_mem->nps      = 0;
  arkls_mem->ncfl     = 0;
  arkls_mem->njtsetup = 0;
  arkls_mem->njtimes  = 0;
  return (0);
}

int arkLsInitializeMassCounters(ARKLsMassMem arkls_mem)
{
  arkls_mem->nmsetups   = 0;
  arkls_mem->nmsolves   = 0;
  arkls_mem->nmtsetup   = 0;
  arkls_mem->nmtimes    = 0;
  arkls_mem->nmvsetup   = 0;
  arkls_mem->npe        = 0;
  arkls_mem->nli        = 0;
  arkls_mem->nps        = 0;
  arkls_mem->ncfl       = 0;
  arkls_mem->msetuptime = -SUN_BIG_REAL;
  return (0);
}

/*---------------------------------------------------------------
  arkLs_AccessARKODELMem, arkLs_AccessLMem,
  arkLs_AccessARKODEMassMem and arkLs_AccessMassMem:

  Shortcut routines to unpack ark_mem, ls_mem and mass_mem
  structures from void* pointer and ark_mem structure.  If any
  is missing it returns ARKLS_MEM_NULL, ARKLS_LMEM_NULL or
  ARKLS_MASSMEM_NULL.
  ---------------------------------------------------------------*/
int arkLs_AccessARKODELMem(void* arkode_mem, const char* fname,
                           ARKodeMem* ark_mem, ARKLsMem* arkls_mem)
{
  void* ark_step_lmem;
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARKLS_MEM_NULL, __LINE__, fname, __FILE__,
                    MSG_LS_ARKMEM_NULL);
    return (ARKLS_MEM_NULL);
  }
  *ark_mem      = (ARKodeMem)arkode_mem;
  ark_step_lmem = (*ark_mem)->step_getlinmem(*ark_mem);
  if (ark_step_lmem == NULL)
  {
    arkProcessError(*ark_mem, ARKLS_LMEM_NULL, __LINE__, fname, __FILE__,
                    MSG_LS_LMEM_NULL);
    return (ARKLS_LMEM_NULL);
  }
  *arkls_mem = (ARKLsMem)ark_step_lmem;
  return (ARKLS_SUCCESS);
}

int arkLs_AccessLMem(ARKodeMem ark_mem, const char* fname, ARKLsMem* arkls_mem)
{
  void* ark_step_lmem;
  ark_step_lmem = ark_mem->step_getlinmem(ark_mem);
  if (ark_step_lmem == NULL)
  {
    arkProcessError(ark_mem, ARKLS_LMEM_NULL, __LINE__, fname, __FILE__,
                    MSG_LS_LMEM_NULL);
    return (ARKLS_LMEM_NULL);
  }
  *arkls_mem = (ARKLsMem)ark_step_lmem;
  return (ARKLS_SUCCESS);
}

int arkLs_AccessARKODEMassMem(void* arkode_mem, const char* fname,
                              ARKodeMem* ark_mem, ARKLsMassMem* arkls_mem)
{
  void* ark_step_massmem;
  if (arkode_mem == NULL)
  {
    arkProcessError(NULL, ARKLS_MEM_NULL, __LINE__, fname, __FILE__,
                    MSG_LS_ARKMEM_NULL);
    return (ARKLS_MEM_NULL);
  }
  *ark_mem         = (ARKodeMem)arkode_mem;
  ark_step_massmem = (*ark_mem)->step_getmassmem(*ark_mem);
  if (ark_step_massmem == NULL)
  {
    arkProcessError(*ark_mem, ARKLS_MASSMEM_NULL, __LINE__, fname, __FILE__,
                    MSG_LS_MASSMEM_NULL);
    return (ARKLS_MASSMEM_NULL);
  }
  *arkls_mem = (ARKLsMassMem)ark_step_massmem;
  return (ARKLS_SUCCESS);
}

int arkLs_AccessMassMem(ARKodeMem ark_mem, const char* fname,
                        ARKLsMassMem* arkls_mem)
{
  void* ark_step_massmem;
  ark_step_massmem = ark_mem->step_getmassmem(ark_mem);
  if (ark_step_massmem == NULL)
  {
    arkProcessError(ark_mem, ARKLS_MASSMEM_NULL, __LINE__, fname, __FILE__,
                    MSG_LS_MASSMEM_NULL);
    return (ARKLS_MASSMEM_NULL);
  }
  *arkls_mem = (ARKLsMassMem)ark_step_massmem;
  return (ARKLS_SUCCESS);
}

/*===============================================================
  EOF
  ===============================================================*/
