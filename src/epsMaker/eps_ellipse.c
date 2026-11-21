// eps_ellipse.c
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

// Draws ellipse canvas items, made with the ellipse command, into the postscript buffer during the final rendering pass
// of canvasDraw.c. An ellipse is drawn as a circle in a scaled and rotated coordinate system, which is then unscaled
// before stroking so that the line width is uniform.

#define _PPL_EPS_ELLIPSE 1

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "settings/withWords_fns.h"

#include "epsMaker/eps_comm.h"
#include "epsMaker/eps_core.h"
#include "epsMaker/eps_ellipse.h"
#include "epsMaker/eps_settings.h"

//! eps_ellps_RenderEPS - Render an ellipse canvas item to postscript
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn

void eps_ellps_RenderEPS(EPSComm *x) {
    // Variables for the shape and style of the ellipse
    int lt;
    unsigned char filled = 0, stroked = 0;
    double lw_scale;
    withWords ww;

    // Print label at top of postscript description of ellipse
    fprintf(x->epsbuffer, "%% Canvas item %d [ellipse]\n", x->current->id);
    eps_core_clear(x);

    // Calculate position of centre of elipse and its major/minor axes in TeX points
    const double xc = x->current->xpos * M_TO_PS;
    const double yc = x->current->ypos * M_TO_PS;
    const double a = x->current->xpos2 * M_TO_PS;
    const double b = x->current->ypos2 * M_TO_PS;
    const double r = x->current->rotation;

    // Expand any numbered styles which may appear in the with words we are passed
    ppl_withWordsMerge(x->c, &ww, &x->current->with_data, NULL, NULL, NULL, NULL, 1);

    // Set color of outline of ellipse
    eps_core_SetColor(x, &ww, 1);

    // Set linewidth and linetype of outline
    if (ww.USElinewidth) lw_scale = ww.linewidth;
    else lw_scale = x->current->settings.LineWidth;
    const double lw = EPS_DEFAULT_LINEWIDTH * lw_scale;

    // The line type is the one requested, or solid
    if (ww.USElinetype) lt = ww.linetype;
    else lt = 1;

    // Write the line width and type
    if (eps_core_isVisible(x)) eps_core_SetLinewidth(x, lw, lt, 0.0);

    // Set up postscript axes so that a unit circle produces the desired ellipse
    fprintf(x->epsbuffer, "gsave\n");
    fprintf(x->epsbuffer, "%.2f %.2f translate\n", xc, yc);
    fprintf(x->epsbuffer, "%.2f rotate\n", r * 180 / M_PI);
    fprintf(x->epsbuffer, "%.2f %.2f scale\n", a, b);

    // Set fill color of ellipse
    eps_core_SetFillColor(x, &ww);
    eps_core_SwitchTo_FillColor(x, 1);

    // Fill ellipse
    if (eps_core_isVisible(x)) {
        if (!x->current->arcset) fprintf(x->epsbuffer, "0 0 1 0 360 arc\nclosepath\nfill\n");
        else
            fprintf(x->epsbuffer, "0 0 1 %.2f %.2f arc\n0 0 lineto\nclosepath\nfill\n",
                    x->current->arcfrom * 180 / M_PI, x->current->arcto * 180 / M_PI);
        filled = 1;
    }

    // Set color of outline of ellipse
    eps_core_SetColor(x, &ww, 1);

    // Make path representing the outline of ellipse
    if (eps_core_isVisible(x)) {
        if (!x->current->arcset)
            fprintf(x->epsbuffer,
                    "0 0 1 0 360 arc\nclosepath\n"); // NB: Leave this path unstroked until we've done a grestore
        else
            fprintf(x->epsbuffer, "0 0 1 %.2f %.2f arc\n", x->current->arcfrom * 180 / M_PI,
                    x->current->arcto * 180 / M_PI);
        stroked = 1;
    }

    // Undo scaling of axes so that linewidths come out right
    const double a_inv = (a < 1e-8) ? 1e8 : 1.0 / a;
    // Avoid dividing by a semi-minor axis of zero
    const double b_inv = (b < 1e-8) ? 1e8 : 1.0 / b;
    fprintf(x->epsbuffer, "%f %f scale\n", a_inv, b_inv);

    // Now we can stroke the path of the ellipse
    if (eps_core_isVisible(x))
        fprintf(x->epsbuffer,
                "stroke\n"); // Stroke outline AFTER grestore, so that linewidth is not scaled.

    // Undo scaling of postscript axes
    fprintf(x->epsbuffer, "grestore\n");
    x->LastLinewidth = -1;
    x->LastLinetype = -1;
    x->LastPSColor[0] = '\0';

    // Factor ellipse into EPS file's bounding box
    if (filled || stroked) {
        eps_core_BoundingBox(x, xc - a * cos(r) - b * sin(r), yc - a * sin(-r) - b * cos(r), lw);
        eps_core_BoundingBox(x, xc - a * cos(r) + b * sin(r), yc - a * sin(-r) + b * cos(r), lw);
        eps_core_BoundingBox(x, xc + a * cos(r) - b * sin(r), yc + a * sin(-r) - b * cos(r), lw);
        eps_core_BoundingBox(x, xc + a * cos(r) + b * sin(r), yc + a * sin(-r) + b * cos(r), lw);
    }

    // Free with words
    ppl_withWordsDestroy(x->c, &ww);

    // Final newline at end of canvas item
    fprintf(x->epsbuffer, "\n");
    return;
}
