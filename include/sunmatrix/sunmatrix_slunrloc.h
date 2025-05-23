/*
 * ----------------------------------------------------------------------------
 * Programmer(s): Cody Balos @ LLNL
 * ----------------------------------------------------------------------------
 * SUNDIALS Copyright Start
 * Copyright (c) 2002-2025, Lawrence Livermore National Security
 * and Southern Methodist University.
 * All rights reserved.
 *
 * See the top-level LICENSE and NOTICE files for details.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * SUNDIALS Copyright End
 * ----------------------------------------------------------------------------
 * This is the header file for the SuperLU SLU_NR_loc SUNMatrix.
 * ----------------------------------------------------------------------------
 */

#ifndef _SUNMATRIX_SUPERLUNRLOC_H
#define _SUNMATRIX_SUPERLUNRLOC_H

#include <stdio.h>
#include <sundials/sundials_matrix.h>
#include <superlu_ddefs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
 *  PART 1: SUNMatrix wrapper for the SuperLU SuperMatrix structure with
 *          Stype = SLU_NR_loc, i.e. a SuperMatrix stored in a distributed
 *          CSR format.
 * ---------------------------------------------------------------------------*/

struct _SUNMatrixContent_SLUNRloc
{
  sunbooleantype own_data;
  gridinfo_t* grid;
  sunindextype* row_to_proc;
  pdgsmv_comm_t* gsmv_comm;
  SuperMatrix* A_super;
  SuperMatrix* ACS_super;
};

typedef struct _SUNMatrixContent_SLUNRloc* SUNMatrixContent_SLUNRloc;

/* ----------------------------------------------------------------------------
 * PART 2: Functions exported by SUNMatrix_SLUNRloc:
 * --------------------------------------------------------------------------*/

SUNDIALS_EXPORT SUNMatrix SUNMatrix_SLUNRloc(SuperMatrix* A_super,
                                             gridinfo_t* grid, SUNContext sunctx);
SUNDIALS_EXPORT void SUNMatrix_SLUNRloc_Print(SUNMatrix A, FILE* fp);

/* ----------------------------------------------------------------------------
 * Accessor Functions:
 * --------------------------------------------------------------------------*/

SUNDIALS_EXPORT SuperMatrix* SUNMatrix_SLUNRloc_SuperMatrix(SUNMatrix A);
SUNDIALS_EXPORT gridinfo_t* SUNMatrix_SLUNRloc_ProcessGrid(SUNMatrix A);
SUNDIALS_EXPORT sunbooleantype SUNMatrix_SLUNRloc_OwnData(SUNMatrix A);

/* -----------------------------------------------------------------------------
 * SuperLU implementations of various SUNMatrix operations:
 * ----------------------------------------------------------------------------*/

SUNDIALS_EXPORT SUNMatrix_ID SUNMatGetID_SLUNRloc(SUNMatrix A);
SUNDIALS_EXPORT SUNMatrix SUNMatClone_SLUNRloc(SUNMatrix A);
SUNDIALS_EXPORT void SUNMatDestroy_SLUNRloc(SUNMatrix A);
SUNDIALS_EXPORT SUNErrCode SUNMatZero_SLUNRloc(SUNMatrix A);
SUNDIALS_EXPORT SUNErrCode SUNMatCopy_SLUNRloc(SUNMatrix A, SUNMatrix B);
SUNDIALS_EXPORT SUNErrCode SUNMatScaleAdd_SLUNRloc(sunrealtype c, SUNMatrix A,
                                                   SUNMatrix B);
SUNDIALS_EXPORT SUNErrCode SUNMatScaleAddI_SLUNRloc(sunrealtype c, SUNMatrix A);
SUNDIALS_EXPORT SUNErrCode SUNMatMatvecSetup_SLUNRloc(SUNMatrix A);
SUNDIALS_EXPORT SUNErrCode SUNMatMatvec_SLUNRloc(SUNMatrix A, N_Vector x,
                                                 N_Vector y);
SUNDIALS_DEPRECATED_EXPORT_MSG(
  "Work space functions will be removed in version 8.0.0")
SUNErrCode SUNMatSpace_SLUNRloc(SUNMatrix A, long int* lenrw, long int* leniw);

#ifdef __cplusplus
}
#endif

#endif
