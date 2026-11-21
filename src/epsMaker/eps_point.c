// eps_point.c
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

// Draws point canvas items, made with the point command, into the postscript buffer during the final rendering pass of
// canvasDraw.c, using the same point symbols as plots. A point may have a text label, which is passed to LaTeX in the
// earlier text-collecting pass.

#define _PPL_EPS_POINT 1

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "coreUtils/memAlloc.h"

#include "epsMaker/canvasDraw.h"
#include "coreUtils/errorReport.h"
#include "settings/settings.h"
#include "settings/settingTypes.h"
#include "settings/withWords_fns.h"

#include "epsMaker/eps_comm.h"
#include "epsMaker/eps_core.h"
#include "epsMaker/eps_plot_styles.h"
#include "epsMaker/eps_point.h"
#include "epsMaker/eps_settings.h"
#include "epsMaker/eps_style.h"

//! eps_point_YieldUpText - Collect the text label of a point canvas item, so that it can be typeset
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn

void eps_point_YieldUpText(EPSComm *x) {
    CanvasTextItem *i;

    // Record where this item's text starts in the list of text items, then add its label, if any
    if (x->current->text != NULL) {
        x->current->FirstTextID = x->NTextItems;
        if (x->current->text[0] == '\0') return;

        // Add the label to the list of text to typeset
        i = (CanvasTextItem *) ppl_memAlloc(sizeof(CanvasTextItem));
        // Failure to allocate memory is reported as an error
        if (i == NULL) {
            ppl_error(&x->c->errcontext, ERR_MEMORY, -1, -1, "Out of memory");
            *(x->status) = 1;
            return;
        }
        i->text = x->current->text;
        i->CanvasMultiplotID = x->current->id;
        ppl_listAppend(x->TextItems, i);
        x->NTextItems++;
    }
    return;
}

//! eps_point_RenderEPS - Render a point canvas item to postscript
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn

void eps_point_RenderEPS(EPSComm *x) {
    // Variables for the style of the point
    int pt, lt;
    double lw, lw_scale, xpos, ypos;
    withWords ww, ww_default;
    int pageno;

    // This item's label is on the page of the typeset text which was recorded in eps_point_YieldUpText()
    pageno = x->LaTeXpageno = x->current->FirstTextID;
    x->LaTeXpageno++;

    // Print label at top of postscript description of box
    fprintf(x->epsbuffer, "%% Canvas item %d [point]\n", x->current->id);
    eps_core_clear(x);

    // Look up position of point
    xpos = x->current->xpos * M_TO_PS;
    ypos = x->current->ypos * M_TO_PS;

    // Expand any numbered styles which may appear in the with words we are passed
    eps_withwords_default(x, &ww_default, &x->current->settings, 0, 0, 0, 0, 0);
    ppl_withWordsMerge(x->c, &ww, &x->current->with_data, &ww_default, NULL, NULL, NULL, 1);

    // Display point
    eps_core_SetColor(x, &ww, 1);
    // Draw the point, unless it is invisible
    if (eps_core_isVisible(x)) {
        // Set linewidth and linetype of point
        if (ww.USElinewidth) lw_scale = ww.pointlinewidth;
        else lw_scale = x->current->settings.LineWidth;
        lw = EPS_DEFAULT_LINEWIDTH * lw_scale;
        lt = 1;
        eps_core_SetLinewidth(x, lw, lt, 0.0);
        fprintf(x->epsbuffer, "/ps { %f } def\n", ww.pointsize * EPS_DEFAULT_PS);

        // Point types cycle through the available symbols; note which are used, so that only those are defined in the
        // postscript prolog
        pt = (ww.pointtype - 1) % N_POINTTYPES;
        while (pt < 0) pt += N_POINTTYPES;
        x->PointTypesUsed[pt] = 1;
        fprintf(x->epsbuffer, "%.2f %.2f pt%d\n", xpos, ypos, pt + 1);
        eps_core_BoundingBox(x, xpos, ypos, 2 * ww.pointsize * eps_PointSize[pt] * EPS_DEFAULT_PS);

        // Label point if it is labelled
        if ((x->current->text != NULL) && (x->current->text[0] != '\0')) {
            const double xsp = (x->current->settings.TextVAlign == SW_VALIGN_CENT) ? 1.7 : 1.13;
            const double ysp = (x->current->settings.TextHAlign == SW_HALIGN_CENT) ? 1.7 : 1.13;
            canvas_EPSRenderTextItem(x, NULL, pageno,
                                     x->current->xpos -
                                             (x->current->settings.TextHAlign - SW_HALIGN_CENT) * ww.pointsize *
                                                     eps_PointSize[pt] * EPS_DEFAULT_PS / M_TO_PS * xsp,
                                     x->current->ypos +
                                             (x->current->settings.TextVAlign - SW_VALIGN_CENT) * ww.pointsize *
                                                     eps_PointSize[pt] * EPS_DEFAULT_PS / M_TO_PS * ysp,
                                     x->current->settings.TextHAlign, x->current->settings.TextVAlign, x->CurrentColor,
                                     x->current->settings.FontSize, 0.0, NULL, NULL);
        }
    }

    // Free with words
    ppl_withWordsDestroy(x->c, &ww);
    ppl_withWordsDestroy(x->c, &ww_default);

    // Final newline at end of canvas item
    fprintf(x->epsbuffer, "\n");
    return;
}
