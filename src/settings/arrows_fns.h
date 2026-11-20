// arrows_fns.h
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

#ifndef _PPLSET_ARROWS_FNS_H
#define _PPLSET_ARROWS_FNS_H 1

#include "coreUtils/dict.h"
#include "parser/parser.h"
#include "settings/arrows.h"
#include "userspace/context.h"
#include "userspace/unitsDisp.h"

void pplarrow_add(ppl_context *context, pplarrow_object **inlist, parserOutput *in, parserLine *pl, const int *ptab);
void pplarrow_remove(ppl_context *context, pplarrow_object **inlist, parserOutput *in, const parserLine *pl,
                     const int *ptab, int quiet);
void pplarrow_unset(ppl_context *context, pplarrow_object **inlist, parserOutput *in, parserLine *pl, const int *ptab);
unsigned char pplarrow_compare(ppl_context *context, pplarrow_object *a, pplarrow_object *b);
void pplarrow_list_copy(ppl_context *context, pplarrow_object **out, pplarrow_object **in);
void pplarrow_list_destroy(ppl_context *context, pplarrow_object **inlist);
void pplarrow_print(ppl_context *context, pplarrow_object *in, char *out, size_t outLen);

int pplarrow_getSystem(ppl_context *context, const parserOutput *in, const int *ptab, int index);
int pplarrow_getAxis(const parserOutput *in, const int *ptab, int index);
int pplarrow_checkAxis(ppl_context *context, const parserOutput *in, const parserLine *pl, const int *ptab, int index);
void pplarrow_checkDimensions(ppl_context *context, const parserOutput *in, const parserLine *pl, const int *ptab,
                              int index, int system);
void pplarrow_copyCoordinate(parserOutput *in, const int *ptab, int index, int system, pplObj *out);

#endif
