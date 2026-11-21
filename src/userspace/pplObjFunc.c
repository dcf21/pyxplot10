// pplObjFunc.c
//
// The code in this file is part of Pyxplot
// <http://www.pyxplot.org.uk>
//
// Copyright (C) 2006-2026 Dominic Ford <coders@pyxplot.org.uk>
//               2008-2013 Ross Church
//
// $Id$
//
// Pyxplot is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 2 of the License, or (at your option) any later
// version.
//
// You should have received a copy of the GNU General Public License along with
// Pyxplot; if not, write to the Free Software Foundation, Inc., 51 Franklin
// Street, Fifth Floor, Boston, MA  02110-1301, USA

// ----------------------------------------------------------------------------

// Frees and copies the descriptors of Pyxplot's function objects. Functions may be built-in (system) functions,
// algebraic functions defined by the user, subroutines, or functions produced by commands such as interpolate,
// histogram and fft, each of which keeps its own data. A user-defined function may consist of a chain of definitions,
// each valid over a different range of its arguments.

#include "stdlib.h"
#include "string.h"

#ifdef HAVE_FFTW3
#include <fftw3.h>
#else
#include <fftw.h>
#endif

#include "expressions/expCompile_fns.h"
#include "parser/parser.h"
#include "userspace/pplObj.h"
#include "userspace/pplObjFunc_fns.h"

//! pplObjFuncDestroyChain - Free a chain of function descriptors, such as the multiple definitions of a function
//! over different ranges of its arguments
//! \param [in] f - The first function descriptor in the chain

void pplObjFuncDestroyChain(pplFunc *f) {
    // Free each definition in the chain
    while (f != NULL) {
        pplFunc *next = f->next;
        pplObjFuncDestroy(f);
        f = next;
    }
    return;
}

//! pplObjFuncDestroy - Free a function descriptor and the storage it owns. System functions are never freed.
//! \param [in] f - The function descriptor to free

void pplObjFuncDestroy(pplFunc *f) {
    // Built-in functions are never freed
    const int type = f->functionType;
    if ((type == PPL_FUNC_SYSTEM) || (type == PPL_FUNC_MAGIC)) return; // Never garbage collect system functions

    // Free the data held by each type of function
    switch (type) {
        case PPL_FUNC_USERDEF: {
            pplExpr *e = (pplExpr *) f->functionPtr;
            f->functionPtr = NULL;
            // A user-defined function holds the expression which defines it
            if (e != NULL) {
                pplExpr_free(e);
                e = NULL;
            }
            break;
        }
        case PPL_FUNC_SPLINE: {
            splineDescriptor *d = (splineDescriptor *) f->functionPtr;
            f->functionPtr = NULL;
            // An interpolated function holds the GSL spline and its accelerator, except stepwise interpolation, which
            // holds a plain array of data points
            if (d != NULL) {
                if (d->splineType[1] != 't') // Not stepwise interpolation
                {
                    // Free the GSL spline
                    if (d->splineObj != NULL) {
                        gsl_spline_free(d->splineObj);
                        d->splineObj = NULL;
                    }
                    // Free the GSL lookup accelerator
                    if (d->accelerator != NULL) {
                        gsl_interp_accel_free(d->accelerator);
                        d->accelerator = NULL;
                    }
                } else {
                    // Free the table of data points used for stepwise interpolation
                    free(d->splineObj);
                    d->splineObj = NULL;
                }
                free(d);
                d = NULL;
            }
            break;
        }
        case PPL_FUNC_INTERP2D:
        case PPL_FUNC_BMPDATA: {
            splineDescriptor *d = (splineDescriptor *) f->functionPtr;
            f->functionPtr = NULL;
            // Two-dimensional interpolations and bitmap images hold a plain array of data
            if (d != NULL) {
                // Free the array
                free(d->splineObj);
                d->splineObj = NULL;
                free(d);
                d = NULL;
            }
            break;
        }
        case PPL_FUNC_HISTOGRAM: {
            histogramDescriptor *h = (histogramDescriptor *) f->functionPtr;
            f->functionPtr = NULL;
            // A histogram holds the boundaries of its bins, and the value of each
            if (h != NULL) {
                // Free the bin boundaries
                free(h->bins);
                h->bins = NULL;
                // Free the bin values
                free(h->binvals);
                h->binvals = NULL;
                free(h);
                h = NULL;
            }
            break;
        }
        case PPL_FUNC_FFT: {
            FFTDescriptor *d = (FFTDescriptor *) f->functionPtr;
            f->functionPtr = NULL;
            // A Fourier transform holds its grid of output values, allocated by FFTW
            if (d != NULL) {
                // Free the grid
                if (d->datagrid != NULL) {
                    fftw_free(d->datagrid);
                    d->datagrid = NULL;
                }
                free(d);
                d = NULL;
            }
            break;
        }
        case PPL_FUNC_SUBROUTINE: {
            parserLine *s = (parserLine *) f->functionPtr;
            f->functionPtr = NULL;
            if (s != NULL) ppl_parserLineFree(s);
            break;
        }
    }

    // Free the names of the arguments, and the range of argument values over which this definition applies
    free(f->argList);
    f->argList = NULL;
    // Lower limits of the ranges of the arguments
    free(f->min);
    f->min = NULL;
    // Upper limits of the ranges of the arguments
    free(f->max);
    f->max = NULL;
    // Flags saying which lower limits apply
    free(f->minActive);
    f->minActive = NULL;
    // Flags saying which upper limits apply
    free(f->maxActive);
    f->maxActive = NULL;
    free(f);
    f = NULL;
    return;
}

//! pplObjFuncCpy - Make a deep copy of a user-defined function descriptor
//! \param [in] f - The function descriptor to copy
//! \return The copy, or NULL if <f> is not a user-defined function, or if memory allocation failed

pplFunc *pplObjFuncCpy(const pplFunc *f) {
    // Only user-defined functions can be copied
    pplFunc *o;
    int i, j;
    if (f->functionType != PPL_FUNC_USERDEF) return NULL;

    // Find the length of the list of argument names, which are stored one after another, each null-terminated
    const int Nargs = f->maxArgs;
    for (j = 0, i = 0; i < Nargs; i++)
        while (f->argList[j++] != '\0');

    // Copy the descriptor, including its flags; the pointers which it owns are copied separately below
    if ((o = (pplFunc *) malloc(sizeof(pplFunc))) == NULL) return NULL;
    *o = *f;
    o->refCount = 1;
    o->functionPtr = NULL;
    o->argList = NULL;
    o->min = o->max = NULL;
    o->minActive = o->maxActive = NULL;
    o->next = NULL;
    o->description = NULL;
    o->descriptionShort = NULL;
    o->LaTeX = NULL;

    // Copy the expression, the argument names and the ranges of the arguments. On failure, free whatever was copied.
    if (((f->functionPtr != NULL) &&
         ((o->functionPtr = (void *) pplExpr_cpy((pplExpr *) f->functionPtr)) == NULL)) ||
        ((o->argList = (char *) malloc(j)) == NULL) ||
        ((o->min = (pplObj *) malloc(Nargs * sizeof(pplObj))) == NULL) ||
        ((o->max = (pplObj *) malloc(Nargs * sizeof(pplObj))) == NULL) ||
        ((o->minActive = (unsigned char *) malloc(Nargs)) == NULL) ||
        ((o->maxActive = (unsigned char *) malloc(Nargs)) == NULL)) {
        pplObjFuncDestroy(o);
        return NULL;
    }
    memcpy(o->argList, f->argList, j);
    memcpy(o->min, f->min, Nargs * sizeof(pplObj));
    memcpy(o->max, f->max, Nargs * sizeof(pplObj));
    memcpy(o->minActive, f->minActive, Nargs);
    memcpy(o->maxActive, f->maxActive, Nargs);
    return o;
}
