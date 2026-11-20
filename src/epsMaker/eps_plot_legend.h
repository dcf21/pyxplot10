// eps_plot_legend.h
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

#ifndef _PPL_EPS_PLOT_LEGEND_H
#define _PPL_EPS_PLOT_LEGEND_H 1

#include "epsMaker/eps_comm.h"
#include "epsMaker/eps_settings.h"

#define AB_ENLARGE_FACTOR 2.5
#define BB_ENLARGE_FACTOR 1.4
#define MARGIN_HSIZE      (0.012 * M_TO_PS)
#define MARGIN_HSIZE_LEFT (0.009 * M_TO_PS)

#define LEGEND_HGAP_MAXIMUM (0.006 * M_TO_PS)
#define LEGEND_VGAP_MAXIMUM (0.003 * M_TO_PS)
#define LEGEND_MARGIN       (0.002 * M_TO_PS)

#define MAX_LEGEND_COLUMNS 1024

// Adds all of the text items which will be needed to render this plot to the
// list x->TextItems. It is vital that they be added in the exact order in
// which they will be rendered.

int eps_yieldTextItem(EPSComm *x, char *text);
int eps_yieldTextItemCopy(EPSComm *x, const char *text);

void GraphLegend_YieldUpText(EPSComm *x);
void GraphLegend_Render(EPSComm *x, double width, double height, double zdepth);

#endif
