// stack.h
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

// Functions for managing the stack of pplObj values used to evaluate expressions (context->stack)

#ifndef _STACK_H
#define _STACK_H 1

#include "userspace/context.h"

int ppl_stackPop(ppl_context *c);
void ppl_stackPushCopy(ppl_context *c, pplObj *o, int lval);
int ppl_stackClean(ppl_context *c, int level);
void ppl_stackRequest(ppl_context *c, int n);
void ppl_stackMustHave(ppl_context *c, int n);
void ppl_stackReinit(ppl_context *c);

#endif
