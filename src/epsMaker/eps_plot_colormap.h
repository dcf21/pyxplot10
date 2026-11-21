// eps_plot_colormap.h
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

#ifndef _PPL_EPS_PLOT_COLORMAP_H
#define _PPL_EPS_PLOT_COLORMAP_H 1

#include "settings/settings.h"

int eps_plot_colormap_findCRange(const pplset_graph *sg, int c, const dataBlock *blk, int Ncol, int XSize, int YSize,
                                 double *CMin, double *CMax, unsigned char *CLog);
void eps_plot_colormap_orientation(pplset_graph *sg, unsigned char ThreeDim, int xn, int yn, int zn, double origin_x,
                                   double origin_y, double width, double height, double zdepth, double *xo, double *yo,
                                   double *Lx, double *Ly, double *ThetaX, double *ThetaY);
void eps_plot_colormap_YieldText(EPSComm *x, const dataTable *data, pplset_graph *sg, canvas_plotdesc *pd);
int eps_plot_colormap(EPSComm *x, dataTable *data, unsigned char ThreeDim, int xn, int yn, int zn, pplset_graph *sg,
                      canvas_plotdesc *pd, int pdn, double origin_x, double origin_y, double width, double height,
                      double zdepth);
int eps_plot_colormap_DrawScales(EPSComm *x, double origin_x, double origin_y, double width, double height,
                                 double zdepth);

#endif
