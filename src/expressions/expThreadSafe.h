// expThreadSafe.h
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

#ifndef _EXPTHREADSAFE_H
#define _EXPTHREADSAFE_H 1

#include "expressions/expCompile.h"
#include "userspace/context.h"

int ppl_expThreadSafe(ppl_context *c, const pplExpr *e, const char *const *locals, int nLocals);
int ppl_funcThreadSafe(ppl_context *c, const pplObj *fn);

ppl_context *ppl_contextWorkerNew(const ppl_context *parent);
int ppl_contextWorkerPrivatise(ppl_context *w, ppl_context *parent, pplExpr **exprs, int nExprs,
                               const char *const *locals, int nLocals);
int ppl_contextWorkerPrivatiseFunc(ppl_context *w, ppl_context *parent, const pplObj *fn, pplObj *out);
void ppl_contextWorkerFree(ppl_context *w);

#endif
