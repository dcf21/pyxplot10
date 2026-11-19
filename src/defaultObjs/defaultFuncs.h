// defaultFuncs.h
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

#ifndef _PPL_DEFAULT_FUNCTIONS_H
#define _PPL_DEFAULT_FUNCTIONS_H 1

#include "coreUtils/dict.h"
#include "settings/settings.h"
#include "userspace/context.h"
#include "userspace/pplObj.h"
#include "userspace/pplObjFunc.h"

void ppl_addMagicFunction(dict *n, const char *name, int id, char *shortdesc, char *latex, char *desc);
pplFunc *ppl_addSystemFunc(dict *n, const char *name, int minArgs, int maxArgs, int numOnly, int notNan, int realOnly,
                           int dimlessOnly, void *fn, char *shortdesc, char *latex, char *desc);
void ppl_addSystemMethod(dict *n, char *name, int minArgs, int maxArgs, int numOnly, int notNan, int realOnly,
                         int dimlessOnly, void *fn, char *shortdesc, char *latex, char *desc);

void pplfunc_abs(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_acos(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_acosh(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_acot(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_acoth(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_acsc(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_acsch(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_airy_ai(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_airy_ai_diff(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_airy_bi(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_airy_bi_diff(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_arg(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_asec(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_asech(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_asin(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_asinh(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_atan(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_atanh(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_atan2(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_besseli(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_besselI(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_besselj(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_besselJ(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_besselk(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_besselK(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_bessely(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_besselY(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_beta(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_call(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_ceil(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_chr(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_classOf(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                     const char *errText);
void pplfunc_cmp(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                 const char *errText);
void pplfunc_cmyk(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                  const char *errText);
void pplfunc_conjugate(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_copy(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                  const char *errText);
void pplfunc_cos(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_cosh(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_cot(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_coth(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_cross(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_csc(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_csch(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_degrees(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_deepcopy(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                      const char *errText);
void pplfunc_ellK(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_ellE(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_ellP(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_erf(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_erfc(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_eval(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_exp(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_expm1(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_expint(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_factors(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_finite(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                    const char *errText);
void pplfunc_floor(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_gamma(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_gcd(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_globals(ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType, const char *errText);
void pplfunc_gray(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                  const char *errText);
void pplfunc_heaviside(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_hsb(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                 const char *errText);
void pplfunc_hyperg_0F1(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_hyperg_1F1(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_hyperg_2F0(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_hyperg_2F1(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_hyperg_U(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_hypot(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_imag(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_jacobi_cn(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_jacobi_dn(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_jacobi_sn(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_lambert_W0(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_lambert_W1(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_lcm(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_ldexp(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_legendreP(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_legendreQ(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_len(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_locals(ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType, const char *errText);
void pplfunc_log(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_log10(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_logn(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_lrange(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_max(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                 const char *errText);
void pplfunc_min(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                 const char *errText);
void pplfunc_mod(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_open(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_ord(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_ordinal(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_pow(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_prime(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_primefactors(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_radians(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_raise(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_range(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_rgb(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                 const char *errText);
void pplfunc_real(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_romanNum(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_root(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_round(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_sec(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_sech(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_sgn(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                 const char *errText);
void pplfunc_sin(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_sinc(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_sinh(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_sqrt(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_sum(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_tan(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_tanh(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_texify(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_texifyText(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_tophat(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_typeOf(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                    const char *errText);
void pplfunc_zernike(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_zernikeR(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_zeta(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);

#endif
