/*
 * Project:  LevenbergMarquardtLeastSquaresFitting
 *
 * File:     lmmin.c
 *
 * Contents: Levenberg-Marquardt core implementation,
 *           and simplified user interface.
 *
 * Author:   Joachim Wuttke <j.wuttke@fz-juelich.de>
 *
 * Acknowledgments:
 *           This software is built on public domain work by
 *           Burton S. Garbow, Kenneth E. Hillstrom, Jorge J. Moré,
 *           Steve Moshier, and the authors of lapack.
 *           See ../CHANGELOG for contributors of corrections and refinements.
 *
 * Licence:  see ../COPYING (FreeBSD)
 * 
 * Homepage: joachimwuttke.de/lmfit
 */

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <float.h>
#include "lmmin.h"

#define MIN(a,b) (((a)<=(b)) ? (a) : (b))
#define MAX(a,b) (((a)>=(b)) ? (a) : (b))
#define SQR(x)   (x)*(x)

/* #define LMFIT_DEBUG_MESSAGES 1 */
/* #define LMFIT_DEBUG_MATRIX 1 */

/* function declarations (implemented below). */
void lm_lmpar( int n, double *r, int ldr, int *ipvt, double *diag,
               double *qtb, double delta, double *par, double *x,
               double *sdiag, double *aux, double *xdi );
void lm_qrfac( int m, int n, double *a, int pivot, int *ipvt,
               double *rdiag, double *acnorm, double *wa );
void lm_qrsolv( int n, double *r, int ldr, int *ipvt, double *diag,
                double *qtb, double *x, double *sdiag, double *wa );


/*****************************************************************************/
/*  Numeric constants                                                        */
/*****************************************************************************/

/* machine-dependent constants from float.h */
#define LM_MACHEP     DBL_EPSILON   /* resolution of arithmetic */
#define LM_DWARF      DBL_MIN       /* smallest nonzero number */
#define LM_SQRT_DWARF sqrt(DBL_MIN) /* square should not underflow */
#define LM_SQRT_GIANT sqrt(DBL_MAX) /* square should not overflow */
#define LM_USERTOL    30*LM_MACHEP  /* users are recommended to require this */

/* If the above values do not work, the following seem good for an x86:
 LM_MACHEP     .555e-16
 LM_DWARF      9.9e-324 
 LM_SQRT_DWARF 1.e-160   
 LM_SQRT_GIANT 1.e150 
 LM_USER_TOL   1.e-14
   The following values should work on any machine:
 LM_MACHEP     1.2e-16
 LM_DWARF      1.0e-38
 LM_SQRT_DWARF 3.834e-20
 LM_SQRT_GIANT 1.304e19
 LM_USER_TOL   1.e-14
*/

const lm_control_struct lm_control_double = {
    LM_USERTOL, LM_USERTOL, LM_USERTOL, LM_USERTOL, 100., 100, 1, 0, 1 };
const lm_control_struct lm_control_float = {
    1.e-7, 1.e-7, 1.e-7, 1.e-7, 100., 100, 1, 0, 1 };


/*****************************************************************************/
/*  Message texts (indexed by status.info)                                   */
/*****************************************************************************/

const char *lm_infmsg[] = {
    "success (sum of squares below underflow limit)",
    "success (the relative error in the sum of squares is at most tol)",
    "success (the relative error between x and the solution is at most tol)",
    "success (both errors are at most tol)",
    "trapped by degeneracy (increasing epsilon might help)",
    "timeout (number of calls to fcn has reached maxcall*(n+1))",
    "failure (ftol<tol: cannot reduce sum of squares any further)",
    "failure (xtol<tol: cannot improve approximate solution any further)",
    "failure (gtol<tol: cannot improve approximate solution any further)",
    "exception (not enough memory)",
    "fatal coding error (improper input parameters)",
    "exception (break requested within function evaluation)"
};

const char *lm_shortmsg[] = {
    "success (0)",
    "success (f)",
    "success (p)",
    "success (f,p)",
    "degenerate",
    "call limit",
    "failed (f)",
    "failed (p)",
    "failed (o)",
    "no memory",
    "invalid input",
    "user break"
};


/*****************************************************************************/
/*  lm_printout_std (default monitoring routine)                             */
/*****************************************************************************/

const char *lm_action_msg[] = {
    "trying step in gradient direction",
    "determining gradient",
    "starting minimization",
    "terminating minimization",
};

void lm_printout_std( int n_par, const double *par, int m_dat,
                      const void *data, const double *fvec,
                      int printflags, int iflag, int iter, int nfev)
/*
 *    This is the default monitoring routine.
 *
 *    It can also be used as a template for other ways of monitoring the fit:
 *    Copy this code to your application; rename the routine; modify it;
 *    and call lmmin with parameter printout = <your modified routine>.
 *
 *    Input parameters:
 *       data  : for soft control of printout behaviour, add control
 *                 variables to the data struct
 *       iflag : 0 (init) 1 (outer loop) 2(inner loop) 3(terminated)
 *       iter  : outer loop counter
 *       nfev  : number of calls to *evaluate
 */
{
    int i;

    if( !printflags )
        return;

    printf("lmmin monitor (nfev=%d, iter=%d)\n", nfev, iter );
    printf( "  %s\n", lm_action_msg[iflag] );

    if( printflags & 2 ){
        printf("  pars ");
        for (i = 0; i < n_par; ++i)
            printf(" %18.11g", par[i]);
        printf(" => norm %18.11g\n", lm_enorm(m_dat, fvec));
    }

    if ( (printflags & 8) || ((printflags & 4) && iflag == 3) ) {
        printf("  residuals (i -> fvec[i])");
        for (i = 0; i < m_dat; ++i)
            printf(" %3i %12g\n", i, fvec[i] );
        printf("\n");
    }
}

/*****************************************************************************/
/*  lm_printout_compact (compact monitoring for experts)                     */
/*****************************************************************************/

const char lm_action_code[] = { 'S', 'G', 'I', 'F' };

void lm_printout_compact( int n_par, const double *par, int m_dat,
                          const void *data, const double *fvec,
                          int printflags, int iflag, int iter, int nfev)
/*
 *    This is the default monitoring routine.
 *
 *    It can also be used as a template for other ways of monitoring the fit:
 *    Copy this code to your application; rename the routine; modify it;
 *    and call lmmin with parameter printout = <your modified routine>.
 *
 *    Input parameters:
 *       data  : for soft control of printout behaviour, add control
 *                 variables to the data struct
 *       iflag : 0 (init) 1 (outer loop) 2(inner loop) 3(terminated)
 *       iter  : outer loop counter
 *       nfev  : number of calls to *evaluate
 */
{
    int i;

    if( !printflags )
        return;

    printf("lmmin:: %3d %2d %c", nfev, iter, lm_action_code[iflag] );

    if( printflags & 2 ){
        for (i = 0; i < n_par; ++i)
            printf(" %16.9g", par[i]);
        printf(" => %18.11g\n", lm_enorm(m_dat, fvec));
    }

    if ( (printflags & 8) || ((printflags & 4) && iflag == -1) ) {
        printf("  residuals");
        for (i = 0; i < m_dat; ++i)
            printf(" %10g", fvec[i] );
        printf("\n");
    }
}


/*****************************************************************************/
/*  lmmin (main minimization routine)                                        */
/*****************************************************************************/

void lmmin( int n, double *x, int m, const void *data, 
            void (*evaluate) (const double *par, int m_dat, const void *data,
                              double *fvec, int *info),
            const lm_control_struct *C,
            lm_status_struct *S,
            void (*printout) (int n_par, const double *par, int m_dat,
                              const void *data, const double *fvec,
                              int printflags, int iflag, int iter, int nfev) )
/*
 *   This routine contains the core algorithm of our library.
 *
 *   It minimizes the sum of the squares of m nonlinear functions
 *   in n variables by a modified Levenberg-Marquardt algorithm.
 *   The function evaluation is done by the user-provided routine 'evaluate'.
 *   The Jacobian is then calculated by a forward-difference approximation.
 *
 *   Parameters:
 *
 *      m is a positive integer input variable set to the number
 *        of functions.
 *
 *      n is a positive integer input variable set to the number
 *        of variables; n must not exceed m.
 *
 *      x is an array of length n. On input x must contain an initial
 *        estimate of the solution vector. On OUTPUT x contains the
 *        final estimate of the solution vector.
 *
 *      fvec is an OUTPUT array of length m which contains
 *        the functions evaluated at the output x.
 *
 *      C->ftol is a nonnegative input variable. Termination occurs when
 *        both the actual and predicted relative reductions in the sum
 *        of squares are at most C->ftol. Therefore, C->ftol measures the
 *        relative error desired in the sum of squares.
 *
 *      C->xtol is a nonnegative input variable. Termination occurs when
 *        the relative error between two consecutive iterates is at
 *        most C->xtol. Therefore, C->xtol measures the relative error desired
 *        in the approximate solution.
 *
 *      C->gtol is a nonnegative input variable. Termination occurs when
 *        the cosine of the angle between fvec and any column of the
 *        jacobian is at most C->gtol in absolute value. Therefore, C->gtol
 *        measures the orthogonality desired between the function vector
 *        and the columns of the jacobian.
 *
 *      maxfev is a positive integer input variable. Termination
 *        occurs when the number of calls to lm_fcn is at least
 *        maxfev by the end of an iteration.
 *
 *      C->epsilon is an input variable used in choosing a step length for
 *        the forward-difference approximation. The relative errors in
 *        the functions are assumed to be of the order of C->epsilon.
 *
 *      diag is an array of length n. If C->scale_diag is true, diag is
 *        internally set. If C->scale_diag is false, diag must contain positive entries
 *        that serve as multiplicative scale C->stepbounds for the variables.
 *
 *      C->scale_diag is an integer input variable with boolean semantics.
 *        If true, the variables will be scaled internally.
 *        If false, the scaling is specified by the input diag.
 *
 *      C->stepbound is a positive input variable used in determining the
 *        initial step bound. This bound is set to the product of
 *        C->stepbound and the euclidean norm of diag*x if nonzero, or else
 *        to C->stepbound itself. In most cases C->stepbound should lie in the
 *        interval (0.1,100.0). Generally, the value 100.0 is recommended.
 *
 *      info is an integer OUTPUT variable that indicates the termination
 *        status of lmmin as follows:
 *
 *        info < 0  termination requested by user-supplied routine *evaluate;
 *
 *        info = 0  fnorm almost vanishing;
 *
 *        info = 1  both actual and predicted relative reductions
 *                  in the sum of squares are at most C->ftol;
 *
 *        info = 2  relative error between two consecutive iterates
 *                  is at most C->xtol;
 *
 *        info = 3  conditions for info = 1 and info = 2 both hold;
 *
 *        info = 4  the cosine of the angle between fvec and any
 *                  column of the jacobian is at most C->gtol in
 *                  absolute value;
 *
 *        info = 5  number of calls to lm_fcn has reached or
 *                  exceeded maxfev;
 *
 *        info = 6  C->ftol is too small: no further reduction in
 *                  the sum of squares is possible;
 *
 *        info = 7  C->xtol is too small: no further improvement in
 *                  the approximate solution x is possible;
 *
 *        info = 8  C->gtol is too small: fvec is orthogonal to the
 *                  columns of the jacobian to machine precision;
 *
 *        info =10  improper input parameters;
 *
 *      nfev is an OUTPUT variable set to the number of calls to the
 *        user-supplied routine *evaluate.
 *
 *      fjac is an OUTPUT m by n array. The upper n by n
 *        submatrix of fjac contains an upper triangular matrix r with
 *        diagonal elements of nonincreasing magnitude such that
 *
 *              pT*(jacT*jac)*p = rT*r
 *
 *              (NOTE: T stands for matrix transposition),
 *
 *        where p is a permutation matrix and jac is the final
 *        calculated jacobian. Column j of p is column ipvt(j)
 *        (see below) of the identity matrix. The lower trapezoidal
 *        part of fjac contains information generated during
 *        the computation of r.
 *
 *      ipvt is an integer OUTPUT array of length n. It defines a
 *        permutation matrix p such that jac*p = q*r, where jac is
 *        the final calculated jacobian, q is orthogonal (not stored),
 *        and r is upper triangular with diagonal elements of
 *        nonincreasing magnitude. Column j of p is column ipvt(j)
 *        of the identity matrix.
 *
 *      qtf is an OUTPUT array of length n which contains
 *        the first n elements of the vector (q transpose)*fvec.
 *
 *      wa1, wa2, and wa3 are work arrays of length n.
 *
 *      wa4 is a work array of length m, used among others to hold
 *        residuals from evaluate.
 *
 *      evaluate points to the subroutine which calculates the m nonlinear
 *        functions. Implementations should be written as follows:
 *
 *        void evaluate( double* par, int m, void *data,
 *                       double* fvec, int *info )
 *        {
 *           // for ( i=0; i<m; ++i )
 *           //     calculate fvec[i] for given parameters par;
 *           // to stop the minimization, 
 *           //     set *info to a negative integer.
 *        }
 *
 *      printout points to the subroutine which informs about fit progress.
 *        Call with printout=0 if no printout is desired.
 *        Call with printout=lm_printout_std to use the default implementation.
 *
 *      printflags is passed to printout.
 *
 *      data is an input pointer to an arbitrary structure that is passed to
 *        evaluate. Typically, it contains experimental data to be fitted.
 *
 */
{

    double *fvec, *diag, *fjac, *qtf, *wa1, *wa2, *wa3, *wa4;
    int *ipvt;
    int j, i;
    double actred, dirder, fnorm, fnorm1, gnorm, pnorm,
        prered, ratio, step, sum, temp, temp1, temp2, temp3;
    static double p1 = 0.1;
    static double p0001 = 1.0e-4;

    int maxfev = C->maxcall * (n+1);

    int    iter = 0;      /* outer loop counter */
    double par = 0;       /* Levenberg-Marquardt parameter */
    double delta = 0;
    double xnorm = 0;
    double eps = sqrt(MAX(C->epsilon, LM_MACHEP)); /* for forward differences */

    S->info = 0;   /* status code */
    S->nfev = 0;   /* function evaluation counter */

/*** check input parameters for errors. ***/

    if ( n <= 0 ) {
        fprintf( stderr, "lmmin: invalid number of parameters %i\n", n );
        S->info = 10; /* invalid parameter */
        return;
    }
    if (m < n) {
        fprintf( stderr, "lmmin: number of data points (%i) "
                 "smaller than number of parameters (%i)\n", m, n );
        S->info = 10;
        return;
    }
    if ((C->ftol < 0.) || (C->xtol < 0.) || (C->gtol < 0.)) {
        fprintf( stderr,
                 "lmmin: negative tolerance (at least one of %g %g %g)\n",
                 C->ftol, C->xtol, C->gtol );
        S->info = 10;
        return;
    }
    if (maxfev <= 0) {
        fprintf( stderr, "lmmin: nonpositive function evaluations limit %i\n",
                 maxfev );
        S->info = 10;
        return;
    }
    if (C->stepbound <= 0.) {
        fprintf( stderr, "lmmin: nonpositive stepbound %g\n", C->stepbound );
        S->info = 10;
        return;
    }

/*** allocate work space. ***/

    if ( (fvec = (double *) malloc(m * sizeof(double))) == NULL ||
         (diag = (double *) malloc(n * sizeof(double))) == NULL ||
         (qtf  = (double *) malloc(n * sizeof(double))) == NULL ||
         (fjac = (double *) malloc(n*m*sizeof(double))) == NULL ||
         (wa1  = (double *) malloc(n * sizeof(double))) == NULL ||
         (wa2  = (double *) malloc(n * sizeof(double))) == NULL ||
         (wa3  = (double *) malloc(n * sizeof(double))) == NULL ||
         (wa4  = (double *) malloc(m * sizeof(double))) == NULL ||
         (ipvt = (int *)    malloc(n * sizeof(int)   )) == NULL    ) {
        S->info = 9;
        return;
    }

    if (!C->scale_diag) {
        for (j = 0; j < n; j++) {
            diag[j] = 1.;
        }
    }

/*** evaluate function at starting point and calculate norm. ***/

    (*evaluate) (x, m, data, fvec, &(S->info));
    ++(S->nfev);
    if( printout )
        (*printout) (n, x, m, data, fvec, C->printflags, 0, 0, S->nfev);
    if (S->info < 0)
        return;
    fnorm = lm_enorm(m, fvec);
    if( fnorm <= LM_DWARF ){
        S->info = 0;
        return;
    }

/*** the outer loop. ***/

    do {
#ifdef LMFIT_DEBUG_MESSAGES
        printf("\n");
        printf("debug lmmin outer loop %d:%d fnorm=%.10e\n",
               iter, S->nfev, fnorm);
#endif

/*** outer: calculate the Jacobian. ***/

        for (j = 0; j < n; j++) {
            temp = x[j];
            step = MAX(eps*eps, eps * fabs(temp));
            x[j] += step; /* replace temporarily */
            S->info = 0;
            (*evaluate) (x, m, data, wa4, &(S->info));
            ++(S->nfev);
            if( printout )
                (*printout) (n, x, m, data,
                             wa4, C->printflags, 1, iter, S->nfev);
            if (S->info < 0)
                return; /* user requested break */
            for (i = 0; i < m; i++)
                fjac[j*m+i] = (wa4[i] - fvec[i]) / step;
            x[j] = temp; /* restore */
        }
#ifdef LMFIT_DEBUG_MATRIX
        /* print the entire matrix */
        for (i = 0; i < m; i++) {
            for (j = 0; j < n; j++)
                printf("%.5e ", fjac[j*m+i]);
            printf("\n");
        }
#endif

/*** outer: compute the qr factorization of the Jacobian. ***/

        lm_qrfac(m, n, fjac, C->pivot, ipvt, wa1, wa2, wa3);
        /* return values are ipvt, wa1=rdiag, wa2=acnorm */

        if (!iter) { 
            /* first iteration only */
            if (C->scale_diag) {
                /* diag := norms of the columns of the initial Jacobian */
                for (j = 0; j < n; j++) {
                    diag[j] = wa2[j];
                    if (wa2[j] == 0.)
                        diag[j] = 1.;
                }
            }
            /* use diag to scale x, then calculate the norm */
            for (j = 0; j < n; j++)
                wa3[j] = diag[j] * x[j];
            xnorm = lm_enorm(n, wa3);
            /* initialize the step bound delta. */
            delta = C->stepbound * xnorm;
            if (delta == 0.)
                delta = C->stepbound;
        } else {
            if (C->scale_diag) {
                for (j = 0; j < n; j++)
                    diag[j] = MAX( diag[j], wa2[j] );
            }
        }

/*** outer: form (q transpose)*fvec and store first n components in qtf. ***/

        for (i = 0; i < m; i++)
            wa4[i] = fvec[i];

        for (j = 0; j < n; j++) {
            temp3 = fjac[j*m+j];
            if (temp3 != 0.) {
                sum = 0;
                for (i = j; i < m; i++)
                    sum += fjac[j*m+i] * wa4[i];
                temp = -sum / temp3;
                for (i = j; i < m; i++)
                    wa4[i] += fjac[j*m+i] * temp;
            }
            fjac[j*m+j] = wa1[j];
            qtf[j] = wa4[j];
        }

/*** outer: compute norm of scaled gradient and test for convergence. ***/

        gnorm = 0;
        for (j = 0; j < n; j++) {
            if (wa2[ipvt[j]] == 0)
                continue;
            sum = 0.;
            for (i = 0; i <= j; i++)
                sum += fjac[j*m+i] * qtf[i];
            gnorm = MAX( gnorm, fabs( sum / wa2[ipvt[j]] / fnorm ) );
        }

        if (gnorm <= C->gtol) {
            S->info = 4;
            return;
        }

/*** the inner loop. ***/
        do {
#ifdef LMFIT_DEBUG_MESSAGES
            printf("\n");
            printf("debug lmmin inner loop %d:%d\n", iter, S->nfev);
#endif

/*** inner: determine the levenberg-marquardt parameter. ***/

            lm_lmpar( n, fjac, m, ipvt, diag, qtf, delta, &par,
                      wa1, wa2, wa4, wa3 );
            /* used return values are fjac (partly), par, wa1=x, wa3=diag*x */

            for (j = 0; j < n; j++)
                wa2[j] = x[j] - wa1[j]; /* new parameter vector ? */

            pnorm = lm_enorm(n, wa3);

            /* at first call, adjust the initial step bound. */

            if (S->nfev <= 1+n)
                delta = MIN(delta, pnorm);

/*** inner: evaluate the function at x + p and calculate its norm. ***/

            S->info = 0;
            (*evaluate) (wa2, m, data, wa4, &(S->info));
            ++(S->nfev);
            if( printout )
                (*printout) (n, wa2, m, data,
                             wa4, C->printflags, 2, iter, S->nfev);
            if (S->info < 0)
                return; /* user requested break. */

            fnorm1 = lm_enorm(m, wa4);
#ifdef LMFIT_DEBUG_MESSAGES
            printf("debug lmmin pnorm %.10e  fnorm1 %.10e  fnorm %.10e"
                   " delta=%.10e par=%.10e\n",
                   pnorm, fnorm1, fnorm, delta, par);
#endif

/*** inner: compute the scaled actual reduction. ***/

            if (p1 * fnorm1 < fnorm)
                actred = 1 - SQR(fnorm1 / fnorm);
            else
                actred = -1;

/*** inner: compute the scaled predicted reduction and 
     the scaled directional derivative. ***/

            for (j = 0; j < n; j++) {
                wa3[j] = 0;
                for (i = 0; i <= j; i++)
                    wa3[i] -= fjac[j*m+i] * wa1[ipvt[j]];
            }
            temp1 = lm_enorm(n, wa3) / fnorm;
            temp2 = sqrt(par) * pnorm / fnorm;
            prered = SQR(temp1) + 2 * SQR(temp2);
            dirder = -(SQR(temp1) + SQR(temp2));

/*** inner: compute the ratio of the actual to the predicted reduction. ***/

            ratio = prered != 0 ? actred / prered : 0;
#ifdef LMFIT_DEBUG_MESSAGES
            printf("debug lmmin actred=%.10e prered=%.10e ratio=%.10e"
                   " sq(1)=%.10e sq(2)=%.10e dd=%.10e\n",
                   actred, prered, prered != 0 ? ratio : 0.,
                   SQR(temp1), SQR(temp2), dirder);
#endif

/*** inner: update the step bound. ***/

            if (ratio <= 0.25) {
                if (actred >= 0.)
                    temp = 0.5;
                else
                    temp = 0.5 * dirder / (dirder + 0.5 * actred);
                if (p1 * fnorm1 >= fnorm || temp < p1)
                    temp = p1;
                delta = temp * MIN(delta, pnorm / p1);
                par /= temp;
            } else if (par == 0. || ratio >= 0.75) {
                delta = pnorm / 0.5;
                par *= 0.5;
            }

/*** inner: test for successful iteration. ***/

            if (ratio >= p0001) {
                /* yes, success: update x, fvec, and their norms. */
                for (j = 0; j < n; j++) {
                    x[j] = wa2[j];
                    wa2[j] = diag[j] * x[j];
                }
                for (i = 0; i < m; i++)
                    fvec[i] = wa4[i];
                xnorm = lm_enorm(n, wa2);
                fnorm = fnorm1;
                iter++;
            }
#ifdef LMFIT_DEBUG_MESSAGES
            else {
                printf("debug lmmin iteration considered unsuccessful\n");
            }
#endif

/*** inner: test for convergence. ***/

            if( fnorm<=LM_DWARF ){
                S->info = 0;
                return;
            }

            S->info = 0;
            if (fabs(actred) <= C->ftol && prered <= C->ftol && 0.5 * ratio <= 1)
                S->info = 1;
            if (delta <= C->xtol * xnorm)
                S->info += 2;
            if (S->info != 0)
                return;

/*** inner: tests for termination and stringent tolerances. ***/

            if (S->nfev >= maxfev){
                S->info = 5;
                return;
            }
            if (fabs(actred) <= LM_MACHEP &&
                prered <= LM_MACHEP && 0.5 * ratio <= 1){
                S->info = 6;
                return;
            }
            if (delta <= LM_MACHEP * xnorm){
                S->info = 7;
                return;
            }
            if (gnorm <= LM_MACHEP){
                S->info = 8;
                return;
            }

/*** inner: end of the loop. repeat if iteration unsuccessful. ***/

        } while (ratio < p0001);

/*** outer: end of the loop. ***/

    } while (1);

    if ( printout )
        (*printout)(
            n, x, m, data, fvec, C->printflags, 3, 0, S->nfev );
    S->fnorm = lm_enorm(m, fvec);
    if ( S->info < 0 )
        S->info = 11;

/*** clean up. ***/
    free(fvec);

} /*** lmmin. ***/


/*****************************************************************************/
/*  lm_lmpar (determine Levenberg-Marquardt parameter)                       */
/*****************************************************************************/

void lm_lmpar(int n, double *r, int ldr, int *ipvt, double *diag,
              double *qtb, double delta, double *par, double *x,
              double *sdiag, double *aux, double *xdi)
{
/*     Given an m by n matrix a, an n by n nonsingular diagonal
 *     matrix d, an m-vector b, and a positive number delta,
 *     the problem is to determine a value for the parameter
 *     par such that if x solves the system
 *
 *          a*x = b  and  sqrt(par)*d*x = 0
 *
 *     in the least squares sense, and dxnorm is the euclidean
 *     norm of d*x, then either par=0 and (dxnorm-delta) < 0.1*delta,
 *     or par>0 and abs(dxnorm-delta) < 0.1*delta.
 *
 *     Using lm_qrsolv, this subroutine completes the solution of the problem
 *     if it is provided with the necessary information from the
 *     qr factorization, with column pivoting, of a. That is, if
 *     a*p = q*r, where p is a permutation matrix, q has orthogonal
 *     columns, and r is an upper triangular matrix with diagonal
 *     elements of nonincreasing magnitude, then lmpar expects
 *     the full upper triangle of r, the permutation matrix p,
 *     and the first n components of qT*b. On output
 *     lmpar also provides an upper triangular matrix s such that
 *
 *          pT*(aT*a + par*d*d)*p = sT*s.
 *
 *     s is employed within lmpar and may be of separate interest.
 *
 *     Only a few iterations are generally needed for convergence
 *     of the algorithm. If, however, the limit of 10 iterations
 *     is reached, then the output par will contain the best
 *     value obtained so far.
 *
 *     parameters:
 *
 *      n is a positive integer input variable set to the order of r.
 *
 *      r is an n by n array. on input the full upper triangle
 *        must contain the full upper triangle of the matrix r.
 *        on OUTPUT the full upper triangle is unaltered, and the
 *        strict lower triangle contains the strict upper triangle
 *        (transposed) of the upper triangular matrix s.
 *
 *      ldr is a positive integer input variable not less than n
 *        which specifies the leading dimension of the array r.
 *
 *      ipvt is an integer input array of length n which defines the
 *        permutation matrix p such that a*p = q*r. column j of p
 *        is column ipvt(j) of the identity matrix.
 *
 *      diag is an input array of length n which must contain the
 *        diagonal elements of the matrix d.
 *
 *      qtb is an input array of length n which must contain the first
 *        n elements of the vector (q transpose)*b.
 *
 *      delta is a positive input variable which specifies an upper
 *        bound on the euclidean norm of d*x.
 *
 *      par is a nonnegative variable. on input par contains an
 *        initial estimate of the levenberg-marquardt parameter.
 *        on OUTPUT par contains the final estimate.
 *
 *      x is an OUTPUT array of length n which contains the least
 *        squares solution of the system a*x = b, sqrt(par)*d*x = 0,
 *        for the output par.
 *
 *      sdiag is an array of length n which contains the
 *        diagonal elements of the upper triangular matrix s.
 *
 *      aux is a multi-purpose work array of length n.
 *
 *      xdi is a work array of length n. On OUTPUT: diag[j] * x[j].
 *
 */
    int i, iter, j, nsing;
    double dxnorm, fp, fp_old, gnorm, parc, parl, paru;
    double sum, temp;
    static double p1 = 0.1;

/*** lmpar: compute and store in x the gauss-newton direction. if the
     jacobian is rank-deficient, obtain a least squares solution. ***/

    nsing = n;
    for (j = 0; j < n; j++) {
        aux[j] = qtb[j];
        if (r[j * ldr + j] == 0 && nsing == n)
            nsing = j;
        if (nsing < n)
            aux[j] = 0;
    }
#ifdef LMFIT_DEBUG_MESSAGES
    printf("debug lmpar nsing=%d\n", nsing);
#endif
    for (j = nsing - 1; j >= 0; j--) {
        aux[j] = aux[j] / r[j + ldr * j];
        temp = aux[j];
        for (i = 0; i < j; i++)
            aux[i] -= r[j * ldr + i] * temp;
    }

    for (j = 0; j < n; j++)
        x[ipvt[j]] = aux[j];

/*** lmpar: initialize the iteration counter, evaluate the function at the
     origin, and test for acceptance of the gauss-newton direction. ***/

    iter = 0;
    for (j = 0; j < n; j++)
        xdi[j] = diag[j] * x[j];
    dxnorm = lm_enorm(n, xdi);
    fp = dxnorm - delta;
    if (fp <= p1 * delta) {
#ifdef LMFIT_DEBUG_MESSAGES
        printf("debug lmpar terminate (fp<p1*delta)\n");
#endif
        *par = 0;
        return;
    }

/*** lmpar: if the jacobian is not rank deficient, the newton
     step provides a lower bound, parl, for the 0. of
     the function. otherwise set this bound to 0.. ***/

    parl = 0;
    if (nsing >= n) {
        for (j = 0; j < n; j++)
            aux[j] = diag[ipvt[j]] * xdi[ipvt[j]] / dxnorm;

        for (j = 0; j < n; j++) {
            sum = 0.;
            for (i = 0; i < j; i++)
                sum += r[j * ldr + i] * aux[i];
            aux[j] = (aux[j] - sum) / r[j + ldr * j];
        }
        temp = lm_enorm(n, aux);
        parl = fp / delta / temp / temp;
    }

/*** lmpar: calculate an upper bound, paru, for the 0. of the function. ***/

    for (j = 0; j < n; j++) {
        sum = 0;
        for (i = 0; i <= j; i++)
            sum += r[j * ldr + i] * qtb[i];
        aux[j] = sum / diag[ipvt[j]];
    }
    gnorm = lm_enorm(n, aux);
    paru = gnorm / delta;
    if (paru == 0.)
        paru = LM_DWARF / MIN(delta, p1);

/*** lmpar: if the input par lies outside of the interval (parl,paru),
     set par to the closer endpoint. ***/

    *par = MAX(*par, parl);
    *par = MIN(*par, paru);
    if (*par == 0.)
        *par = gnorm / dxnorm;
#ifdef LMFIT_DEBUG_MESSAGES
    printf("debug lmpar parl %.4e  par %.4e  paru %.4e\n", parl, *par, paru);
#endif

/*** lmpar: iterate. ***/

    for (;; iter++) {

        /** evaluate the function at the current value of par. **/

        if (*par == 0.)
            *par = MAX(LM_DWARF, 0.001 * paru);
        temp = sqrt(*par);
        for (j = 0; j < n; j++)
            aux[j] = temp * diag[j];

        lm_qrsolv( n, r, ldr, ipvt, aux, qtb, x, sdiag, xdi );
        /* return values are r, x, sdiag */

        for (j = 0; j < n; j++)
            xdi[j] = diag[j] * x[j]; /* used as output */
        dxnorm = lm_enorm(n, xdi);
        fp_old = fp;
        fp = dxnorm - delta;
        
        /** if the function is small enough, accept the current value
            of par. Also test for the exceptional cases where parl
            is zero or the number of iterations has reached 10. **/

        if (fabs(fp) <= p1 * delta
            || (parl == 0. && fp <= fp_old && fp_old < 0.)
            || iter == 10)
            break; /* the only exit from the iteration. */
        
        /** compute the Newton correction. **/

        for (j = 0; j < n; j++)
            aux[j] = diag[ipvt[j]] * xdi[ipvt[j]] / dxnorm;

        for (j = 0; j < n; j++) {
            aux[j] = aux[j] / sdiag[j];
            for (i = j + 1; i < n; i++)
                aux[i] -= r[j * ldr + i] * aux[j];
        }
        temp = lm_enorm(n, aux);
        parc = fp / delta / temp / temp;

        /** depending on the sign of the function, update parl or paru. **/

        if (fp > 0)
            parl = MAX(parl, *par);
        else if (fp < 0)
            paru = MIN(paru, *par);
        /* the case fp==0 is precluded by the break condition  */
        
        /** compute an improved estimate for par. **/
        
        *par = MAX(parl, *par + parc);
        
    }

} /*** lm_lmpar. ***/


/*****************************************************************************/
/*  lm_qrfac (QR factorization, from lapack)                                 */
/*****************************************************************************/

void lm_qrfac(int m, int n, double *a, int pivot, int *ipvt,
              double *rdiag, double *acnorm, double *wa)
{
/*
 *     This subroutine uses Householder transformations with column
 *     pivoting (optional) to compute a qr factorization of the
 *     m by n matrix a. That is, qrfac determines an orthogonal
 *     matrix q, a permutation matrix p, and an upper trapezoidal
 *     matrix r with diagonal elements of nonincreasing magnitude,
 *     such that a*p = q*r. The Householder transformation for
 *     column k, k = 1,2,...,min(m,n), is of the form
 *
 *          i - (1/u(k))*u*uT
 *
 *     where u has zeroes in the first k-1 positions. The form of
 *     this transformation and the method of pivoting first
 *     appeared in the corresponding linpack subroutine.
 *
 *     Parameters:
 *
 *      m is a positive integer input variable set to the number
 *        of rows of a.
 *
 *      n is a positive integer input variable set to the number
 *        of columns of a.
 *
 *      a is an m by n array. On input a contains the matrix for
 *        which the qr factorization is to be computed. On OUTPUT
 *        the strict upper trapezoidal part of a contains the strict
 *        upper trapezoidal part of r, and the lower trapezoidal
 *        part of a contains a factored form of q (the non-trivial
 *        elements of the u vectors described above).
 *
 *      pivot is a logical input variable. If pivot is set true,
 *        then column pivoting is enforced.
 *
 *      ipvt is an integer OUTPUT array of length lipvt. This array
 *        defines the permutation matrix p such that a*p = q*r.
 *        Column j of p is column ipvt(j) of the identity matrix.
 *        If pivot is false, ipvt is not referenced.
 *
 *      rdiag is an OUTPUT array of length n which contains the
 *        diagonal elements of r.
 *
 *      acnorm is an OUTPUT array of length n which contains the
 *        norms of the corresponding columns of the input matrix a.
 *        If this information is not needed, then acnorm can coincide
 *        with rdiag.
 *
 *      wa is a work array of length n. If pivot is false, then wa
 *        can coincide with rdiag.
 *
 */
    int i, j, k, kmax, minmn;
    double ajnorm, sum, temp;

/*** qrfac: compute initial column norms and initialize several arrays. ***/

    for (j = 0; j < n; j++) {
        acnorm[j] = lm_enorm(m, &a[j*m]);
        rdiag[j] = acnorm[j];
        wa[j] = rdiag[j];
        if (pivot)
            ipvt[j] = j;
    }
#ifdef LMFIT_DEBUG_MESSAGES
    printf("debug qrfac\n");
#endif

/*** qrfac: reduce a to r with Householder transformations. ***/

    minmn = MIN(m, n);
    for (j = 0; j < minmn; j++) {
        if (!pivot)
            goto pivot_ok;

        /** bring the column of largest norm into the pivot position. **/

        kmax = j;
        for (k = j + 1; k < n; k++)
            if (rdiag[k] > rdiag[kmax])
                kmax = k;
        if (kmax == j)
            goto pivot_ok;

        for (i = 0; i < m; i++) {
            temp = a[j*m+i];
            a[j*m+i] = a[kmax*m+i];
            a[kmax*m+i] = temp;
        }
        rdiag[kmax] = rdiag[j];
        wa[kmax] = wa[j];
        k = ipvt[j];
        ipvt[j] = ipvt[kmax];
        ipvt[kmax] = k;

      pivot_ok:
        /** compute the Householder transformation to reduce the
            j-th column of a to a multiple of the j-th unit vector. **/

        ajnorm = lm_enorm(m-j, &a[j*m+j]);
        if (ajnorm == 0.) {
            rdiag[j] = 0;
            continue;
        }

        if (a[j*m+j] < 0.)
            ajnorm = -ajnorm;
        for (i = j; i < m; i++)
            a[j*m+i] /= ajnorm;
        a[j*m+j] += 1;

        /** apply the transformation to the remaining columns
            and update the norms. **/

        for (k = j + 1; k < n; k++) {
            sum = 0;

            for (i = j; i < m; i++)
                sum += a[j*m+i] * a[k*m+i];

            temp = sum / a[j + m * j];

            for (i = j; i < m; i++)
                a[k*m+i] -= temp * a[j*m+i];

            if (pivot && rdiag[k] != 0.) {
                temp = a[m * k + j] / rdiag[k];
                temp = MAX(0., 1 - temp * temp);
                rdiag[k] *= sqrt(temp);
                temp = rdiag[k] / wa[k];
                if ( 0.05 * SQR(temp) <= LM_MACHEP ) {
                    rdiag[k] = lm_enorm(m-j-1, &a[m*k+j+1]);
                    wa[k] = rdiag[k];
                }
            }
        }

        rdiag[j] = -ajnorm;
    }
}


/*****************************************************************************/
/*  lm_qrsolv (linear least-squares)                                         */
/*****************************************************************************/

void lm_qrsolv(int n, double *r, int ldr, int *ipvt, double *diag,
               double *qtb, double *x, double *sdiag, double *wa)
{
/*
 *     Given an m by n matrix a, an n by n diagonal matrix d,
 *     and an m-vector b, the problem is to determine an x which
 *     solves the system
 *
 *          a*x = b  and  d*x = 0
 *
 *     in the least squares sense.
 *
 *     This subroutine completes the solution of the problem
 *     if it is provided with the necessary information from the
 *     qr factorization, with column pivoting, of a. That is, if
 *     a*p = q*r, where p is a permutation matrix, q has orthogonal
 *     columns, and r is an upper triangular matrix with diagonal
 *     elements of nonincreasing magnitude, then qrsolv expects
 *     the full upper triangle of r, the permutation matrix p,
 *     and the first n components of (q transpose)*b. The system
 *     a*x = b, d*x = 0, is then equivalent to
 *
 *          r*z = qT*b,  pT*d*p*z = 0,
 *
 *     where x = p*z. If this system does not have full rank,
 *     then a least squares solution is obtained. On output qrsolv
 *     also provides an upper triangular matrix s such that
 *
 *          pT *(aT *a + d*d)*p = sT *s.
 *
 *     s is computed within qrsolv and may be of separate interest.
 *
 *     Parameters
 *
 *      n is a positive integer input variable set to the order of r.
 *
 *      r is an n by n array. On input the full upper triangle
 *        must contain the full upper triangle of the matrix r.
 *        On OUTPUT the full upper triangle is unaltered, and the
 *        strict lower triangle contains the strict upper triangle
 *        (transposed) of the upper triangular matrix s.
 *
 *      ldr is a positive integer input variable not less than n
 *        which specifies the leading dimension of the array r.
 *
 *      ipvt is an integer input array of length n which defines the
 *        permutation matrix p such that a*p = q*r. Column j of p
 *        is column ipvt(j) of the identity matrix.
 *
 *      diag is an input array of length n which must contain the
 *        diagonal elements of the matrix d.
 *
 *      qtb is an input array of length n which must contain the first
 *        n elements of the vector (q transpose)*b.
 *
 *      x is an OUTPUT array of length n which contains the least
 *        squares solution of the system a*x = b, d*x = 0.
 *
 *      sdiag is an OUTPUT array of length n which contains the
 *        diagonal elements of the upper triangular matrix s.
 *
 *      wa is a work array of length n.
 *
 */
    int i, kk, j, k, nsing;
    double qtbpj, sum, temp;
    double _sin, _cos, _tan, _cot; /* local variables, not functions */

/*** qrsolv: copy r and (q transpose)*b to preserve input and initialize s.
     in particular, save the diagonal elements of r in x. ***/

    for (j = 0; j < n; j++) {
        for (i = j; i < n; i++)
            r[j * ldr + i] = r[i * ldr + j];
        x[j] = r[j * ldr + j];
        wa[j] = qtb[j];
    }
/*** qrsolv: eliminate the diagonal matrix d using a Givens rotation. ***/

    for (j = 0; j < n; j++) {

/*** qrsolv: prepare the row of d to be eliminated, locating the
     diagonal element using p from the qr factorization. ***/

        if (diag[ipvt[j]] == 0.)
            goto L90;
        for (k = j; k < n; k++)
            sdiag[k] = 0.;
        sdiag[j] = diag[ipvt[j]];

/*** qrsolv: the transformations to eliminate the row of d modify only 
     a single element of qT*b beyond the first n, which is initially 0. ***/

        qtbpj = 0.;
        for (k = j; k < n; k++) {

            /** determine a Givens rotation which eliminates the
                appropriate element in the current row of d. **/

            if (sdiag[k] == 0.)
                continue;
            kk = k + ldr * k;
            if (fabs(r[kk]) < fabs(sdiag[k])) {
                _cot = r[kk] / sdiag[k];
                _sin = 1 / sqrt(1 + SQR(_cot));
                _cos = _sin * _cot;
            } else {
                _tan = sdiag[k] / r[kk];
                _cos = 1 / sqrt(1 + SQR(_tan));
                _sin = _cos * _tan;
            }

            /** compute the modified diagonal element of r and
                the modified element of ((q transpose)*b,0). **/

            r[kk] = _cos * r[kk] + _sin * sdiag[k];
            temp = _cos * wa[k] + _sin * qtbpj;
            qtbpj = -_sin * wa[k] + _cos * qtbpj;
            wa[k] = temp;

            /** accumulate the tranformation in the row of s. **/

            for (i = k + 1; i < n; i++) {
                temp = _cos * r[k * ldr + i] + _sin * sdiag[i];
                sdiag[i] = -_sin * r[k * ldr + i] + _cos * sdiag[i];
                r[k * ldr + i] = temp;
            }
        }

      L90:
        /** store the diagonal element of s and restore
            the corresponding diagonal element of r. **/

        sdiag[j] = r[j * ldr + j];
        r[j * ldr + j] = x[j];
    }

/*** qrsolv: solve the triangular system for z. if the system is
     singular, then obtain a least squares solution. ***/

    nsing = n;
    for (j = 0; j < n; j++) {
        if (sdiag[j] == 0. && nsing == n)
            nsing = j;
        if (nsing < n)
            wa[j] = 0;
    }

    for (j = nsing - 1; j >= 0; j--) {
        sum = 0;
        for (i = j + 1; i < nsing; i++)
            sum += r[j * ldr + i] * wa[i];
        wa[j] = (wa[j] - sum) / sdiag[j];
    }

/*** qrsolv: permute the components of z back to components of x. ***/

    for (j = 0; j < n; j++)
        x[ipvt[j]] = wa[j];

} /*** lm_qrsolv. ***/


/*****************************************************************************/
/*  lm_enorm (Euclidean norm)                                                */
/*****************************************************************************/

double lm_enorm(int n, const double *x)
{
/*     Given an n-vector x, this function calculates the
 *     euclidean norm of x.
 *
 *     The euclidean norm is computed by accumulating the sum of
 *     squares in three different sums. The sums of squares for the
 *     small and large components are scaled so that no overflows
 *     occur. Non-destructive underflows are permitted. Underflows
 *     and overflows do not occur in the computation of the unscaled
 *     sum of squares for the intermediate components.
 *     The definitions of small, intermediate and large components
 *     depend on two constants, LM_SQRT_DWARF and LM_SQRT_GIANT. The main
 *     restrictions on these constants are that LM_SQRT_DWARF**2 not
 *     underflow and LM_SQRT_GIANT**2 not overflow.
 *
 *     Parameters
 *
 *      n is a positive integer input variable.
 *
 *      x is an input array of length n.
 */
    int i;
    double agiant, s1, s2, s3, xabs, x1max, x3max, temp;

    s1 = 0;
    s2 = 0;
    s3 = 0;
    x1max = 0;
    x3max = 0;
    agiant = LM_SQRT_GIANT / n;

    /** sum squares. **/

    for (i = 0; i < n; i++) {
        xabs = fabs(x[i]);
        if (xabs > LM_SQRT_DWARF) {
            if ( xabs < agiant ) {
                s2 += xabs * xabs;
            } else if ( xabs > x1max ) {
                temp = x1max / xabs;
                s1 = 1 + s1 * SQR(temp);
                x1max = xabs;
            } else {
                temp = xabs / x1max;
                s1 += SQR(temp);
            }
        } else if ( xabs > x3max ) {
            temp = x3max / xabs;
            s3 = 1 + s3 * SQR(temp);
            x3max = xabs;
        } else if (xabs != 0.) {
            temp = xabs / x3max;
            s3 += SQR(temp);
        }
    }

    /** calculation of norm. **/

    if (s1 != 0)
        return x1max * sqrt(s1 + (s2 / x1max) / x1max);
    else if (s2 != 0)
        if (s2 >= x3max)
            return sqrt(s2 * (1 + (x3max / s2) * (x3max * s3)));
        else
            return sqrt(x3max * ((s2 / x3max) + (x3max * s3)));
    else
        return x3max * sqrt(s3);

} /*** lm_enorm. ***/
