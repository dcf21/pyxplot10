// moduleOs.h
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

#ifndef _PPL_MODULE_OS_H
#define _PPL_MODULE_OS_H 1

#include "userspace/context.h"
#include "userspace/pplObj.h"

void pplfunc_osChdir(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osGetCwd(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osGetEgid(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                       const char *errText);
void pplfunc_osGetEuid(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                       const char *errText);
void pplfunc_osGetGid(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                      const char *errText);
void pplfunc_osGetPid(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                      const char *errText);
void pplfunc_osGetPgrp(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                       const char *errText);
void pplfunc_osGetPpid(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                       const char *errText);
void pplfunc_osGetUid(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                      const char *errText);
void pplfunc_osGetHome(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osGetHost(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osGetLogin(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osGetRealName(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osGlob(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osPopen(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osStat(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osSystem(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osTmpfile(const ppl_context *c, pplObj *in, int nArgs, const int *status, const int *errType,
                       const char *errText);
void pplfunc_osUname(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osPathExists(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osPathFilesize(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osPathATime(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osPathCTime(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osPathMTime(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osPathExpandUser(ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);
void pplfunc_osPathJoin(const ppl_context *c, pplObj *in, int nArgs, int *status, int *errType, char *errText);

#endif
