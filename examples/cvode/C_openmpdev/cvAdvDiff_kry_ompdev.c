/* -------------------------------------------------------------------
 * Programmer(s): Shelby Lockhart @ LLNL
 * -------------------------------------------------------------------
 * Acknowledgements: This example is based on cvAdvDiff_kry example
 *                   by Slaven Peles which is based on cvAdvDiff_bnd
 *                   example by Scott D. Cohen, Alan C.
 *                   Hindmarsh and Radu Serban @ LLNL
 * -------------------------------------------------------------------
 * SUNDIALS Copyright Start
 * Copyright (c) 2002-2025, Lawrence Livermore National Security
 * and Southern Methodist University.
 * All rights reserved.
 *
 * See the top-level LICENSE and NOTICE files for details.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * SUNDIALS Copyright End
 * -------------------------------------------------------------------
 * Example problem:
 *
 * The following is a simple example problem with a banded Jacobian,
 * with the program for its solution by CVODE.
 * The problem is the semi-discrete form of the advection-diffusion
 * equation in 2-D:
 *   du/dt = d^2 u / dx^2 + .5 du/dx + d^2 u / dy^2
 * on the rectangle 0 <= x <= 2, 0 <= y <= 1, and the time
 * interval 0 <= t <= 1. Homogeneous Dirichlet boundary conditions
 * are posed, and the initial condition is
 *   u(x,y,t=0) = x(2-x)y(1-y)exp(5xy).
 * The PDE is discretized on a uniform MX+2 by MY+2 grid with
 * central differencing, and with boundary values eliminated,
 * leaving an ODE system of size NEQ = MX*MY.
 * This program solves the problem with the BDF method, Newton
 * iteration with the CVBAND band linear solver, and a user-supplied
 * Jacobian routine.
 * It uses scalar relative and absolute tolerances.
 * Output is printed at t = .1, .2, ..., 1.
 * Run statistics (optional outputs) are printed at the end.
 * -----------------------------------------------------------------*/

#include <cvode/cvode.h> /* prototypes for CVODE fcts., consts. */
#include <math.h>
#include <nvector/nvector_openmpdev.h> /* OpenMPDEV N_Vector types, fcts., macros */
#include <stdio.h>
#include <stdlib.h>
#include <sundials/sundials_math.h>    /* definition of ABS and EXP   */
#include <sundials/sundials_types.h>   /* definition of type sunrealtype */
#include <sunlinsol/sunlinsol_spgmr.h> /* access to SPGMR SUNLinearSolver     */

#ifdef _OPENMP
#include <omp.h>
#endif

/* Real Constants */

#define ATOL  SUN_RCONST(1.0e-5) /* scalar absolute tolerance */
#define T0    SUN_RCONST(0.0)    /* initial time              */
#define T1    SUN_RCONST(0.1)    /* first output time         */
#define DTOUT SUN_RCONST(0.1)    /* output time increment     */
#define NOUT  10                 /* number of output times    */

#define ZERO SUN_RCONST(0.0)
#define HALF SUN_RCONST(0.5)
#define ONE  SUN_RCONST(1.0)
#define TWO  SUN_RCONST(2.0)
#define FIVE SUN_RCONST(5.0)

/* Type : _UserData (contains model and discretization parameters) */
typedef struct
{
  sunindextype MX, MY, NEQ;
  sunrealtype dx, dy, XMAX, YMAX;
  sunrealtype hdcoef, hacoef, vdcoef;
}* UserData;

/*typedef _UserData *UserData;*/

/* Problem setup and initialization functions */
static UserData SetUserData(int argc, char** argv);
static void SetIC(N_Vector u, UserData data);

/* Functions Called by the Solver */
static int f(sunrealtype t, N_Vector u, N_Vector udot, void* user_data);
static int jtv(N_Vector v, N_Vector Jv, sunrealtype t, N_Vector u, N_Vector fu,
               void* user_data, N_Vector tmp);

/* Private Helper Functions */
static void PrintHeader(sunrealtype reltol, sunrealtype abstol,
                        sunrealtype umax, UserData data);
static void PrintOutput(sunrealtype t, sunrealtype umax, long int nst);
static void PrintFinalStats(void* cvode_mem);

/* Private function to check function return values */
static int check_retval(void* flagvalue, const char* funcname, int opt);

/*
 *-------------------------------
 * Main Program
 *-------------------------------
 */

int main(int argc, char** argv)
{
  SUNContext sunctx;
  sunrealtype reltol, abstol, t, tout, umax;
  N_Vector u;
  UserData data;
  SUNLinearSolver LS;
  void* cvode_mem;
  int iout, retval;
  long int nst;

  u         = NULL;
  data      = NULL;
  LS        = NULL;
  cvode_mem = NULL;

  /* Create the SUNDIALS context */
  retval = SUNContext_Create(SUN_COMM_NULL, &sunctx);
  if (check_retval(&retval, "SUNContext_Create", 1)) { return (1); }

  /* Set model parameters */
  data = SetUserData(argc, argv);
  if (check_retval((void*)data, "malloc", 2)) { return (1); }

  reltol = ZERO; /* Set the tolerances */
  abstol = ATOL;

  /* Create an OpenMPDEV vector with initial values */
  u = N_VNew_OpenMPDEV(data->NEQ, sunctx); /* Allocate u vector */
  if (check_retval((void*)u, "N_VNew_Cuda", 0)) { return (1); }

  SetIC(u, data); /* Initialize u vector */

  /* Call CVodeCreate to create the solver memory and specify the
   * Backward Differentiation Formula and the use of a Newton iteration */
  cvode_mem = CVodeCreate(CV_BDF, sunctx);
  if (check_retval((void*)cvode_mem, "CVodeCreate", 0)) { return (1); }

  /* Call CVodeInit to initialize the integrator memory and specify the
   * user's right hand side function in u'=f(t,u), the initial time T0, and
   * the initial dependent variable vector u. */
  retval = CVodeInit(cvode_mem, f, T0, u);
  if (check_retval(&retval, "CVodeInit", 1)) { return (1); }

  /* Call CVodeSStolerances to specify the scalar relative tolerance
   * and scalar absolute tolerance */
  retval = CVodeSStolerances(cvode_mem, reltol, abstol);
  if (check_retval(&retval, "CVodeSStolerances", 1)) { return (1); }

  /* Set the pointer to user-defined data */
  retval = CVodeSetUserData(cvode_mem, data);
  if (check_retval(&retval, "CVodeSetUserData", 1)) { return (1); }

  /* Create SPGMR solver without preconditioning
   * and the maximum Krylov dimension maxl */
  LS = SUNLinSol_SPGMR(u, SUN_PREC_NONE, 0, sunctx);
  if (check_retval(&retval, "SUNLinSol_SPGMR", 1)) { return (1); }

  /* Attach the linear solver */
  retval = CVodeSetLinearSolver(cvode_mem, LS, NULL);
  if (check_retval(&retval, "CVodeSetLinearSolver", 1)) { return (1); }

  /* Set the JAcobian-times-vector function */
  retval = CVodeSetJacTimes(cvode_mem, NULL, jtv);
  if (check_retval(&retval, "CVodeSetJacTimesVecFn", 1)) { return (1); }

  /* In loop over output points: call CVode, print results, test for errors */

  umax = N_VMaxNorm(u);
  PrintHeader(reltol, abstol, umax, data);
  for (iout = 1, tout = T1; iout <= NOUT; iout++, tout += DTOUT)
  {
    retval = CVode(cvode_mem, tout, u, &t, CV_NORMAL);
    if (check_retval(&retval, "CVode", 1)) { break; }
    umax   = N_VMaxNorm(u);
    retval = CVodeGetNumSteps(cvode_mem, &nst);
    check_retval(&retval, "CVodeGetNumSteps", 1);
    PrintOutput(t, umax, nst);
  }

  PrintFinalStats(cvode_mem); /* Print some final statistics   */

  N_VDestroy(u);         /* Free the u vector */
  CVodeFree(&cvode_mem); /* Free the integrator memory */
  free(data);            /* Free the user data */
  SUNContext_Free(&sunctx);

  return (0);
}

/*
 *-------------------------------------------
 * Problem setup and initialization functions
 *-------------------------------------------
 */

/* Set model and discretization parameters */

UserData SetUserData(int argc, char* argv[])
{
  const sunindextype MX  = 10;
  const sunindextype MY  = 5;
  const sunrealtype XMAX = SUN_RCONST(2.0); /* domain boundaries         */
  const sunrealtype YMAX = SUN_RCONST(1.0);

  /* Allocate user data structure */
  UserData ud = (UserData)malloc(sizeof *ud);
  if (check_retval((void*)ud, "AllocUserData", 2)) { return (NULL); }

  ud->MX     = MX;
  ud->MY     = MY;
  ud->NEQ    = MX * MY;
  ud->XMAX   = XMAX;
  ud->YMAX   = YMAX;
  ud->dx     = XMAX / (MX + 1); /* Set grid coefficients in data */
  ud->dy     = YMAX / (MY + 1);
  ud->hdcoef = ONE / (ud->dx * ud->dx);
  ud->hacoef = HALF / (TWO * ud->dx);
  ud->vdcoef = ONE / (ud->dy * ud->dy);

  return ud;
}

/* Set initial conditions in u vector */

static void SetIC(N_Vector u, UserData data)
{
  /* Extract needed constants from data */

  const sunrealtype dx   = data->dx;
  const sunrealtype dy   = data->dy;
  const sunrealtype xmax = data->XMAX;
  const sunrealtype ymax = data->YMAX;
  const sunindextype MY  = data->MY;
  const sunindextype NEQ = data->NEQ;

  /* Extract pointer to solution vector data on the host */
  sunrealtype* udata = N_VGetHostArrayPointer_OpenMPDEV(u);

  sunindextype i, j, tid;
  sunrealtype x, y;

  /* Load initial profile into u vector */

  for (tid = 0; tid < NEQ; tid++)
  {
    i = tid / MY;
    j = tid % MY;

    x = (i + 1) * dx;
    y = (j + 1) * dy;

    udata[tid] = x * (xmax - x) * y * (ymax - y) * SUNRexp(FIVE * x * y);
  }
  N_VCopyToDevice_OpenMPDEV(u);
}

/*
 *-------------------------------
 * Functions called by the solver
 *-------------------------------
 */

/* f routine. Compute f(t,u). */

static int f(sunrealtype t, N_Vector u, N_Vector udot, void* user_data)
{
  sunrealtype uij, udn, uup, ult, urt, hdiff, hadv, vdiff;
  sunindextype i, j, k;
  int dev;

  UserData data = (UserData)user_data;

  /* Extract needed constants from data */
  const sunindextype MX   = data->MX;
  const sunindextype MY   = data->MY;
  const sunrealtype hordc = data->hdcoef;
  const sunrealtype horac = data->hacoef;
  const sunrealtype verdc = data->vdcoef;

  /* Extract pointers to vector data */
  const sunrealtype* udata = N_VGetDeviceArrayPointer_OpenMPDEV(u);
  sunrealtype* dudata      = N_VGetDeviceArrayPointer_OpenMPDEV(udot);

  /* Get device */
  dev = omp_get_default_device();

  /* Loop over all grid points. */
#pragma omp target map(to : MY, MX, hordc, horac, verdc) \
  is_device_ptr(udata, dudata) device(dev)
#pragma omp teams distribute parallel for schedule(static, 1)
  for (k = 0; k < MY * MX; k++)
  {
    i = k / MY;
    j = k % MY;

    uij = udata[k];
    udn = (j == 0) ? ZERO : udata[k - 1];
    uup = (j == MY - 1) ? ZERO : udata[k + 1];
    ult = (i == 0) ? ZERO : udata[k - MY];
    urt = (i == MX - 1) ? ZERO : udata[k + MY];

    /* Set diffusion and advection terms and load into udot */

    hdiff     = hordc * (ult - TWO * uij + urt);
    hadv      = horac * (urt - ult);
    vdiff     = verdc * (uup - TWO * uij + udn);
    dudata[k] = hdiff + hadv + vdiff;
  }

  return (0);
}

/* Jacobian-times-vector routine. */

static int jtv(N_Vector v, N_Vector Jv, sunrealtype t, N_Vector u, N_Vector fu,
               void* user_data, N_Vector tmp)
{
  sunindextype i, j, k;
  int dev;

  UserData data = (UserData)user_data;

  /* Extract needed constants from data */
  const sunindextype MX   = data->MX;
  const sunindextype MY   = data->MY;
  const sunrealtype hordc = data->hdcoef;
  const sunrealtype horac = data->hacoef;
  const sunrealtype verdc = data->vdcoef;

  /* Extract pointers to vector data */
  const sunrealtype* vdata = N_VGetDeviceArrayPointer_OpenMPDEV(v);
  sunrealtype* Jvdata      = N_VGetDeviceArrayPointer_OpenMPDEV(Jv);

  N_VConst(ZERO, Jv);

  /* Get device */
  dev = omp_get_default_device();

#pragma omp target map(to : MX, MY, hordc, horac, verdc) \
  is_device_ptr(vdata, Jvdata) device(dev)
#pragma omp teams distribute parallel for schedule(static, 1)
  for (k = 0; k < MX * MY; k++)
  {
    i = k / MY;
    j = k % MY;

    /* set the kth element of Jv */
    Jvdata[k] = -TWO * (verdc + hordc) * vdata[k];
    if (i != 0) { Jvdata[k] += (hordc - horac) * vdata[k - MY]; }
    if (i != MX - 1) { Jvdata[k] += (hordc + horac) * vdata[k + MY]; }
    if (j != 0) { Jvdata[k] += verdc * vdata[k - 1]; }
    if (j != MY - 1) { Jvdata[k] += verdc * vdata[k + 1]; }
  }

  return (0);
}

/*
 *-------------------------------
 * Private helper functions
 *-------------------------------
 */

/* Print first lines of output (problem description) */

static void PrintHeader(sunrealtype reltol, sunrealtype abstol,
                        sunrealtype umax, UserData data)
{
  printf("\n2-D Advection-Diffusion Equation\n");
  printf("Mesh dimensions = %ld X %ld\n", (long int)data->MX, (long int)data->MY);
  printf("Total system size = %ld\n", (long int)data->NEQ);
#if defined(SUNDIALS_EXTENDED_PRECISION)
  printf("Tolerance parameters: reltol = %Lg   abstol = %Lg\n\n", reltol, abstol);
  printf("At t = %Lg      max.norm(u) =%14.6Le \n", T0, umax);
#elif defined(SUNDIALS_DOUBLE_PRECISION)
  printf("Tolerance parameters: reltol = %g   abstol = %g\n\n", reltol, abstol);
  printf("At t = %g      max.norm(u) =%14.6e \n", T0, umax);
#else
  printf("Tolerance parameters: reltol = %g   abstol = %g\n\n", reltol, abstol);
  printf("At t = %g      max.norm(u) =%14.6e \n", T0, umax);
#endif

  return;
}

/* Print current value */

static void PrintOutput(sunrealtype t, sunrealtype umax, long int nst)
{
#if defined(SUNDIALS_EXTENDED_PRECISION)
  printf("At t = %4.2Lf   max.norm(u) =%14.6Le   nst = %4ld\n", t, umax, nst);
#elif defined(SUNDIALS_DOUBLE_PRECISION)
  printf("At t = %4.2f   max.norm(u) =%14.6e   nst = %4ld\n", t, umax, nst);
#else
  printf("At t = %4.2f   max.norm(u) =%14.6e   nst = %4ld\n", t, umax, nst);
#endif

  return;
}

/* Get and print some final statistics */

static void PrintFinalStats(void* cvode_mem)
{
  long lenrw, leniw;
  long lenrwLS, leniwLS;
  long int nst, nfe, nsetups, nni, ncfn, netf;
  long int nli, npe, nps, ncfl, nfeLS;
  int retval;

  retval = CVodeGetWorkSpace(cvode_mem, &lenrw, &leniw);
  check_retval(&retval, "CVodeGetWorkSpace", 1);
  retval = CVodeGetNumSteps(cvode_mem, &nst);
  check_retval(&retval, "CVodeGetNumSteps", 1);
  retval = CVodeGetNumRhsEvals(cvode_mem, &nfe);
  check_retval(&retval, "CVodeGetNumRhsEvals", 1);
  retval = CVodeGetNumLinSolvSetups(cvode_mem, &nsetups);
  check_retval(&retval, "CVodeGetNumLinSolvSetups", 1);
  retval = CVodeGetNumErrTestFails(cvode_mem, &netf);
  check_retval(&retval, "CVodeGetNumErrTestFails", 1);
  retval = CVodeGetNumNonlinSolvIters(cvode_mem, &nni);
  check_retval(&retval, "CVodeGetNumNonlinSolvIters", 1);
  retval = CVodeGetNumNonlinSolvConvFails(cvode_mem, &ncfn);
  check_retval(&retval, "CVodeGetNumNonlinSolvConvFails", 1);

  retval = CVodeGetLinWorkSpace(cvode_mem, &lenrwLS, &leniwLS);
  check_retval(&retval, "CVodeGetLinWorkSpace", 1);
  retval = CVodeGetNumLinIters(cvode_mem, &nli);
  check_retval(&retval, "CVodeGetNumLinIters", 1);
  retval = CVodeGetNumPrecEvals(cvode_mem, &npe);
  check_retval(&retval, "CVodeGetNumPrecEvals", 1);
  retval = CVodeGetNumPrecSolves(cvode_mem, &nps);
  check_retval(&retval, "CVodeGetNumPrecSolves", 1);
  retval = CVodeGetNumLinConvFails(cvode_mem, &ncfl);
  check_retval(&retval, "CVodeGetNumLinConvFails", 1);
  retval = CVodeGetNumLinRhsEvals(cvode_mem, &nfeLS);
  check_retval(&retval, "CVodeGetNumLinRhsEvals", 1);

  printf("\nFinal Statistics.. \n\n");
  printf("lenrw   = %5ld     leniw   = %5ld\n", lenrw, leniw);
  printf("lenrwLS = %5ld     leniwLS = %5ld\n", lenrwLS, leniwLS);
  printf("nst     = %5ld\n", nst);
  printf("nfe     = %5ld     nfeLS   = %5ld\n", nfe, nfeLS);
  printf("nni     = %5ld     nli     = %5ld\n", nni, nli);
  printf("nsetups = %5ld     netf    = %5ld\n", nsetups, netf);
  printf("npe     = %5ld     nps     = %5ld\n", npe, nps);
  printf("ncfn    = %5ld     ncfl    = %5ld\n\n", ncfn, ncfl);

  return;
}

/* Check function return value...
     opt == 0 means SUNDIALS function allocates memory so check if
              returned NULL pointer
     opt == 1 means SUNDIALS function returns a retval so check if
              retval >= 0
     opt == 2 means function allocates memory so check if returned
              NULL pointer */

static int check_retval(void* flagvalue, const char* funcname, int opt)
{
  int* errflag;

  /* Check if SUNDIALS function returned NULL pointer - no memory allocated */

  if (opt == 0 && flagvalue == NULL)
  {
    fprintf(stderr, "\nSUNDIALS_ERROR: %s() failed - returned NULL pointer\n\n",
            funcname);
    return (1);
  }

  /* Check if retval < 0 */

  else if (opt == 1)
  {
    errflag = (int*)flagvalue;
    if (*errflag < 0)
    {
      fprintf(stderr, "\nSUNDIALS_ERROR: %s() failed with retval = %d\n\n",
              funcname, *errflag);
      return (1);
    }
  }

  /* Check if function returned NULL pointer - no memory allocated */

  else if (opt == 2 && flagvalue == NULL)
  {
    fprintf(stderr, "\nMEMORY_ERROR: %s() failed - returned NULL pointer\n\n",
            funcname);
    return (1);
  }

  return (0);
}
