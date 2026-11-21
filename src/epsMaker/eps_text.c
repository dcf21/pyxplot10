// eps_text.c
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

// Draws text canvas items, made with the text command, into the postscript buffer. All text in Pyxplot is typeset by
// LaTeX: eps_text_YieldUpText() adds the item's text to the list sent to LaTeX in the text-collecting pass of
// canvasDraw.c, and eps_text_RenderEPS() places the typeset result in the final rendering pass.

#define _PPL_EPS_TEXT 1

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "coreUtils/errorReport.h"
#include "coreUtils/memAlloc.h"
#include "settings/settingTypes.h"
#include "settings/withWords_fns.h"
#include "epsMaker/canvasDraw.h"
#include "epsMaker/eps_comm.h"
#include "epsMaker/eps_core.h"
#include "epsMaker/eps_text.h"
#include "epsMaker/eps_settings.h"

//! eps_text_YieldUpText - Collect the text of a text canvas item, so that it can be typeset
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn

void eps_text_YieldUpText(EPSComm *x) {

    // Record where this item's text starts in the list of text items
    x->current->FirstTextID = x->NTextItems;
    if (x->current->text[0] == '\0') return;

    // Add the text to the list of text to typeset
    CanvasTextItem *i = (CanvasTextItem *) ppl_memAlloc(sizeof(CanvasTextItem));
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
    return;
}

//! eps_text_RenderEPS - Render a text canvas item to postscript
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn

void eps_text_RenderEPS(EPSComm *x) {
    // Variables for the style and position of the text
    withWords def, merged;

    // This item's text is on the page of the typeset text which was recorded in eps_text_YieldUpText()
    const int pageno = x->LaTeXpageno = x->current->FirstTextID;
    x->LaTeXpageno++;

    // Empty text items draw nothing
    if (x->current->text[0] == '\0') return;

    // Write header at top of postscript
    fprintf(x->epsbuffer, "%% Canvas item %d [text label]\n", x->current->id);

    // Work out text color
    ppl_withWordsZero(x->c, &def);
    def.color = x->current->settings.TextColor;
    def.color1 = x->current->settings.TextColor1;
    def.color2 = x->current->settings.TextColor2;
    def.color3 = x->current->settings.TextColor3;
    def.color4 = x->current->settings.TextColor4;
    def.Col1234Space = x->current->settings.TextCol1234Space;
    def.USEcolor = (def.color != 0);
    def.USEcolor1234 = (def.color == 0);
    ppl_withWordsMerge(x->c, &merged, &x->current->with_data, &def, NULL, NULL, NULL, 1);
    eps_core_SetColor(x, &merged, 1);

    // Render text item to eps
    const double xgap = -(x->current->settings.TextHAlign - SW_HALIGN_CENT) * x->current->xpos2;
    const double ygap = (x->current->settings.TextVAlign - SW_VALIGN_CENT) * x->current->xpos2;

    // Rotate the gap with the text
    const double xgap2 = xgap * cos(x->current->rotation) - ygap * sin(x->current->rotation);
    const double ygap2 = xgap * sin(x->current->rotation) + ygap * cos(x->current->rotation);

    // Draw the typeset text
    canvas_EPSRenderTextItem(x, NULL, pageno, x->current->xpos + xgap2, x->current->ypos + ygap2,
                             x->current->settings.TextHAlign, x->current->settings.TextVAlign, x->CurrentColor,
                             x->current->settings.FontSize, x->current->rotation, NULL, NULL);

    // Free with words
    ppl_withWordsDestroy(x->c, &merged);

    // Final newline at end of canvas item
    fprintf(x->epsbuffer, "\n");
    return;
}
