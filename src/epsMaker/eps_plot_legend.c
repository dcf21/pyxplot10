// eps_plot_legend.c
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

// Draws the legend (key) of a graph. GraphLegend_YieldUpText() collects each dataset's title, or makes one
// automatically from the plot command, to be typeset by LaTeX; GraphLegend_Render() then measures the typeset titles,
// arranges them into columns, which are justified to fill the space available, and places the legend at the requested
// position, beside the appropriate corner of the cube for three-dimensional plots, before drawing each entry's icon
// (eps_plot_styles.c) and title.

#define _PPL_EPS_PLOT_LEGEND_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <gsl/gsl_math.h>

#include "coreUtils/errorReport.h"
#include "coreUtils/list.h"
#include "coreUtils/memAlloc.h"

#include "settings/settings.h"
#include "settings/withWords_fns.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "userspace/context.h"
#include "userspace/unitsDisp.h"

#include "epsMaker/canvasDraw.h"
#include "epsMaker/eps_comm.h"
#include "epsMaker/eps_core.h"
#include "epsMaker/eps_plot_canvas.h"
#include "epsMaker/eps_plot_legend.h"
#include "epsMaker/eps_plot_styles.h"

#include "canvasItems.h"
#include "texify.h"

// Private routines for sorting 3D positions by depth and azimuth when working out where to put a legend on a 3D plot

//! SortByDepth - Comparison function for qsort(), used to sort the possible positions of a legend on a 3D plot into
//! order of depth
//! \param [in] x - Pointer to the first position
//! \param [in] y - Pointer to the second position
//! \return -1, 0 or 1 depending on the order of the positions

static int SortByDepth(const void *x, const void *y) {
    const double *xd = (const double *) x;
    const double *yd = (const double *) y;
    if (xd[2] > yd[2]) return 1;
    if (xd[2] < yd[2]) return -1;
    return 0;
}

static double SortByAzimuthXCentre, SortByAzimuthYCentre, SortByAzimuthTarget;

//! SortByAzimuthProximity - Comparison function for qsort(), used to sort the possible positions of a legend on a 3D
//! plot by how close their azimuth is to the viewing direction
//! \param [in] x - Pointer to the first position
//! \param [in] y - Pointer to the second position
//! \return -1, 0 or 1 depending on the order of the positions

static int SortByAzimuthProximity(const void *x, const void *y) {
    const double *xd = (const double *) x;
    const double *yd = (const double *) y;
    double ax, ay;
    ax = fabs(atan2(xd[0] - SortByAzimuthXCentre, xd[1] - SortByAzimuthYCentre) - SortByAzimuthTarget);
    ay = fabs(atan2(yd[0] - SortByAzimuthXCentre, yd[1] - SortByAzimuthYCentre) - SortByAzimuthTarget);
    while (ax > 2 * M_PI) ax -= 2 * M_PI;
    if (ax > M_PI) ax = 2 * M_PI - ax;
    while (ay > 2 * M_PI) ay -= 2 * M_PI;
    if (ay > M_PI) ay = 2 * M_PI - ay;
    if (ax > ay) return 1;
    if (ax < ay) return -1;
    return 0;
}

//! TriSwap - Swap two doubles
//! \param [in|out] a - The first double
//! \param [in|out] b - The second double

static void TriSwap(double *a, double *b) {
    double t[3];
    memcpy(t, a, 3 * sizeof(double));
    memcpy(a, b, 3 * sizeof(double));
    memcpy(b, t, 3 * sizeof(double));
    return;
}

// Graph legend API routines

//! eps_yieldTextItem - Add a string to the list of text items which are to be typeset by LaTeX for the canvas item
//! currently being drawn. Blank strings are skipped.
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] text - The string, which must persist until the canvas has been drawn; may be NULL
//! \return Zero on success, or nonzero if memory ran out (in which case an error is reported and <x->status> set)

int eps_yieldTextItem(EPSComm *x, char *text) {
    CanvasTextItem *i;
    if ((text == NULL) || (text[0] == '\0')) return 0;
    i = (CanvasTextItem *) ppl_memAlloc(sizeof(CanvasTextItem));
    // Failure to allocate memory is reported as an error
    if (i == NULL) {
        ppl_error(&x->c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (q).");
        *(x->status) = 1;
        return 1;
    }
    i->text = text;
    i->CanvasMultiplotID = x->current->id;
    ppl_listAppend(x->TextItems, i);
    x->NTextItems++;
    return 0;
}

//! eps_yieldTextItemCopy - Add a copy of a string to the list of text items which are to be typeset by LaTeX for the
//! canvas item currently being drawn
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] text - The string, which is copied
//! \return Zero on success, or nonzero if memory ran out (in which case an error is reported and <x->status> set)

int eps_yieldTextItemCopy(EPSComm *x, const char *text) {
    char *out = (char *) ppl_memAlloc(strlen(text) + 1);
    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        ppl_error(&x->c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (r).");
        *(x->status) = 1;
        return 1;
    }
    strcpy(out, text);
    return eps_yieldTextItem(x, out);
}

//! GraphLegend_YieldUpText - Collect the titles of the plot items in a graph's legend, generating automatic titles from
//! their filenames or functions where none was given, so that they can be typeset
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn

void GraphLegend_YieldUpText(EPSComm *x) {
    // Variables for building automatic titles
    canvas_plotdesc *pd;
    char *cptr, *buffer;
    int j, k, inlen;

    // There is nothing to do if the key is turned off
    if (x->current->settings.key != SW_ONOFF_ON) return;

    // Collect the title of each dataset in turn
    pd = x->current->plotitems;
    while (pd != NULL) // loop over all datasets
    {
        // Datasets with notitle have no entry in the legend
        if (pd->NoTitleSet) {
            pd = pd->next;
            continue;
        } // notitle set
        // Use the title given by the user, or make one which describes the plot command
        else if (pd->TitleSet) {
            pd->TitleFinal = pd->title;
            if (eps_yieldTextItem(x, pd->title)) return;
        } // title for dataset manually set
        else // generate automatic title for dataset
        {
            pd->TitleFinal = cptr = (char *) ppl_memAlloc(LSTR_LENGTH);
            buffer = (char *) ppl_memAlloc(LSTR_LENGTH);
            if (buffer == NULL) cptr = NULL;
            // Build the title from the parts of the plot command, converted into LaTeX
            if (cptr != NULL) {
                k = 0;
                // Parametric plots
                if (pd->parametric) {
                    ppl_strAppendf(cptr, LSTR_LENGTH, &k, "parametric");
                    k += strlen(cptr + k);
                }
                // The range of the parameter
                if (pd->TRangeSet) {
                    ppl_strAppendf(cptr, LSTR_LENGTH, &k, " [%s:%s]",
                                   ppl_unitsNumericDisplay(x->c, &pd->Tmin, 0, SW_DISPLAY_L, 0),
                                   ppl_unitsNumericDisplay(x->c, &pd->Tmax, 1, SW_DISPLAY_L, 0));
                    k += strlen(cptr + k);
                }
                // The functions plotted, or the name of the data file
                if ((pd->function) || (pd->vectors != NULL)) {
                    for (j = 0; j < pd->NFunctions; j++) // Print out the list of functions which we are plotting
                    {
                        ppl_strAppendChar(cptr, LSTR_LENGTH, &k, (j != 0) ? ':' : ' ');
                        inlen = strlen(pd->functions[j]->ascii);
                        ppl_texify_generic(x->c, pd->functions[j]->ascii, -1, &inlen, cptr + k, LSTR_LENGTH - k, NULL,
                                           NULL);
                        k += strlen(cptr + k);
                    }
                } else {
                    ppl_strAppendChar(cptr, LSTR_LENGTH, &k, ' ');
                    ppl_strEscapify((pd->filenameFinal == NULL) ? pd->filename : pd->filenameFinal, buffer,
                                    LSTR_LENGTH);
                    inlen = strlen(buffer);
                    ppl_texify_generic(x->c, buffer, -1, &inlen, cptr + k, LSTR_LENGTH - k, NULL, NULL);
                    k += strlen(cptr + k); // Filename of datafile we are plotting
                }
                if (pd->ContinuitySet) // Print continuous / discontinuous flag
                {
                    // The continuity of the data
                    if (pd->continuity == DATAFILE_DISCONTINUOUS) {
                        ppl_strAppendf(cptr, LSTR_LENGTH, &k, " discontinuous");
                        k += strlen(cptr + k);
                    } else {
                        ppl_strAppendf(cptr, LSTR_LENGTH, &k, " continuous");
                        k += strlen(cptr + k);
                    }
                }
                if (pd->axis1set || pd->axis2set || pd->axis3set) // Print axes to use
                {
                    ppl_strAppendf(cptr, LSTR_LENGTH, &k, "%s", " axes ");
                    k += strlen(cptr + k);
                    // The first axis
                    if (pd->axis1set) {
                        ppl_strAppendf(cptr, LSTR_LENGTH, &k, "$%c%d$", "xyzc"[pd->axis1xyz], pd->axis1);
                        k += strlen(cptr + k);
                    }
                    // The second axis
                    if (pd->axis2set) {
                        ppl_strAppendf(cptr, LSTR_LENGTH, &k, "$%c%d$", "xyzc"[pd->axis2xyz], pd->axis2);
                        k += strlen(cptr + k);
                    }
                    // The third axis
                    if (pd->axis3set) {
                        ppl_strAppendf(cptr, LSTR_LENGTH, &k, "$%c%d$", "xyzc"[pd->axis3xyz], pd->axis3);
                        k += strlen(cptr + k);
                    }
                }
                // The every modifier
                if (pd->EverySet > 0) {
                    ppl_strAppendf(cptr, LSTR_LENGTH, &k, " every $%ld$", pd->EveryList[0]);
                    k += strlen(cptr + k);
                } // Print out 'every' clause of plot command
                // Its second field
                if (pd->EverySet > 1) {
                    ppl_strAppendf(cptr, LSTR_LENGTH, &k, ":$%ld$", pd->EveryList[1]);
                    k += strlen(cptr + k);
                }
                // Its third field
                if (pd->EverySet > 2) {
                    ppl_strAppendf(cptr, LSTR_LENGTH, &k, ":$%ld$", pd->EveryList[2]);
                    k += strlen(cptr + k);
                }
                // Its fourth field
                if (pd->EverySet > 3) {
                    ppl_strAppendf(cptr, LSTR_LENGTH, &k, ":$%ld$", pd->EveryList[3]);
                    k += strlen(cptr + k);
                }
                // Its fifth field
                if (pd->EverySet > 4) {
                    ppl_strAppendf(cptr, LSTR_LENGTH, &k, ":$%ld$", pd->EveryList[4]);
                    k += strlen(cptr + k);
                }
                // Its sixth field
                if (pd->EverySet > 5) {
                    ppl_strAppendf(cptr, LSTR_LENGTH, &k, ":$%ld$", pd->EveryList[5]);
                    k += strlen(cptr + k);
                }
                // The index modifier
                if (pd->IndexSet) {
                    ppl_strAppendf(cptr, LSTR_LENGTH, &k, " index $%d$", pd->index);
                    k += strlen(cptr + k);
                } // Print index to use
                // The label modifier
                if (pd->label != NULL) {
                    ppl_strAppendf(cptr, LSTR_LENGTH, &k, " label ");
                    k += strlen(cptr + k);
                    inlen = strlen(pd->label->ascii);
                    ppl_texify_generic(x->c, pd->label->ascii, -1, &inlen, cptr + k, LSTR_LENGTH - k, NULL, NULL);
                    k += strlen(cptr + k);
                } // Print label string
                // The select modifier
                if (pd->SelectCriterion != NULL) {
                    ppl_strAppendf(cptr, LSTR_LENGTH, &k, "%s", " select ");
                    k += strlen(cptr + k);
                    inlen = strlen(pd->SelectCriterion->ascii);
                    ppl_texify_generic(x->c, pd->SelectCriterion->ascii, -1, &inlen, cptr + k, LSTR_LENGTH - k, NULL,
                                       NULL);
                    k += strlen(cptr + k);
                } // Print select criterion
                // The using modifier
                if ((pd->NUsing > 0) || (pd->UsingRowCols != DATAFILE_COL)) {
                    ppl_strAppendf(cptr, LSTR_LENGTH, &k, " using %s",
                                   (pd->UsingRowCols == DATAFILE_COL) ? "" : "rows");
                    k += strlen(cptr + k); // Print using list
                    // Each using item
                    for (j = 0; j < pd->NUsing; j++) {
                        ppl_strAppendChar(cptr, LSTR_LENGTH, &k, (j != 0) ? ':' : ' ');
                        inlen = strlen(pd->UsingList[j]->ascii);
                        ppl_texify_generic(x->c, pd->UsingList[j]->ascii, -1, &inlen, cptr + k, LSTR_LENGTH - k, NULL,
                                           NULL);
                        k += strlen(cptr + k);
                    }
                }
                cptr[k] = '\0';
                if (eps_yieldTextItem(x, cptr)) return;
            }
        }
        pd = pd->next;
    }
    return;
}

//! titledDataset - Find the next dataset on a graph which has a title, and so appears in the legend
//! \param [in] pd - The dataset at which to start looking
//! \param [in,out] iDataSet - The number of the dataset; incremented for each dataset examined, including those
//! skipped because they have no title
//! \return The first dataset at or after <pd> which has a title, or NULL if there are no more

static canvas_plotdesc *titledDataset(canvas_plotdesc *pd, int *iDataSet) {
    // Skip datasets with no title, counting every dataset
    for (; pd != NULL; pd = pd->next) {
        (*iDataSet)++;
        if ((!pd->NoTitleSet) && (pd->TitleFinal != NULL) && (pd->TitleFinal[0] != '\0')) return pd;
    }
    return NULL;
}

//! GraphLegend_ArrangeToHeight - Lay out all of the items in a graph's legend in columns, with a maximum column height
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] TrialHeight - The maximum height of each column, in postscript points
//! \param [out] AttainedHeight - Set to the height of the tallest column
//! \param [out] Ncolumns - Set to the number of columns used
//! \param [out] ColumnX - Array into which to write the x position of each column, followed by the right-hand edge of
//! the last column; it must have MAX_LEGEND_COLUMNS + 1 elements
//! \param [out] ColumnHeight - Array into which to write the height of each column
//! \param [out] ColumnNItems - Array into which to write the number of items in each column

void GraphLegend_ArrangeToHeight(const EPSComm *x, const double TrialHeight, double *AttainedHeight, int *Ncolumns,
                                 double *ColumnX, double *ColumnHeight, int *ColumnNItems) {
    canvas_plotdesc *pd;
    int i, ColumnNo = 0;
    double ColumnYPos = 0.0, ColumnWidth = 0.0;

    *AttainedHeight = 0.0;
    for (i = 0; i < MAX_LEGEND_COLUMNS; i++) ColumnNItems[i] = 0;
    ColumnX[0] = 0.0;
    {
        int iDataSet = -1; // Counts over all datasets, including those skipped because they have no title
        for (pd = titledDataset(x->current->plotitems, &iDataSet); pd != NULL;
             pd = titledDataset(pd->next, &iDataSet)) {
            if ((ColumnYPos > 0) && (pd->TitleFinal_height > TrialHeight - ColumnYPos) &&
                (ColumnNo < MAX_LEGEND_COLUMNS - 1)) {
                ColumnHeight[ColumnNo] = ColumnYPos;
                ColumnX[ColumnNo + 1] = ColumnX[ColumnNo] + ColumnWidth;
                ColumnYPos = ColumnWidth = 0.0;
                ColumnNo++;
            }
            ColumnNItems[ColumnNo]++;
            pd->TitleFinal_col = ColumnNo;
            pd->TitleFinal_xpos = ColumnX[ColumnNo];
            pd->TitleFinal_ypos =
                    -ColumnYPos; // Minus sign since postscript measures height from bottom, and legend runs down page
            ColumnYPos += pd->TitleFinal_height;
            if (ColumnWidth < pd->TitleFinal_width) ColumnWidth = pd->TitleFinal_width;
            if (ColumnYPos > *AttainedHeight) *AttainedHeight = ColumnYPos;
        }
    }
    ColumnHeight[ColumnNo] = ColumnYPos;
    ColumnX[ColumnNo + 1] = ColumnX[ColumnNo] + ColumnWidth;
    *Ncolumns = ColumnNo + 1;
    return;
}

//! GraphLegend_Render - Draw a graph's legend, choosing its layout and position as requested by "set key"
//! \param [in] x - The state of the EPS/PS rendering process for the canvas currently being drawn
//! \param [in] width - The width of the graph, in postscript points
//! \param [in] height - The height of the graph, in postscript points
//! \param [in] zdepth - The depth of the graph, in postscript points, for 3D plots

void GraphLegend_Render(EPSComm *x, const double width, const double height, const double zdepth) {
    // Variables for arranging the legend
    double fs = x->current->settings.FontSize, CombinedHeight = 0.0, MinimumHeight = 0.0;
    double xoff = 0, yoff = 0;
    // ColumnX has one more element than there are columns, holding the right-hand edge of the last column
    double ColumnX[MAX_LEGEND_COLUMNS + 1], ColumnHeight[MAX_LEGEND_COLUMNS];
    double BestHeight, AttainedHeight, TrialHeight;
    int Ncolumns, ColumnNItems[MAX_LEGEND_COLUMNS];
    double height1, height2, bb_top, bb_bottom, ab_left, ab_right, ab_top, ab_bottom;
    unsigned char hfixed = 0, vfixed = 0;
    canvas_plotdesc *pd;
    int pageno, j, kp = x->current->settings.KeyPos;
    postscriptPage *dviPage;
    withWords ww;

    // There is nothing to do if the key is turned off
    if (x->current->settings.key != SW_ONOFF_ON) return;

    // Loop over all legend items to calculate their individual heights and widths, as well as the combined height of all of them
    pageno = x->LaTeXpageno = x->current->LegendTextID;
    {
        int iDataSet = -1; // Counts over all datasets, including those skipped because they have no title
        for (pd = titledDataset(x->current->plotitems, &iDataSet); pd != NULL;
             pd = titledDataset(pd->next, &iDataSet)) {
            // Fetch dimensions of requested page of postscript
            if (x->dvi == NULL) {
                pd->TitleFinal_width = 0;
                pd->TitleFinal_height = 0;
                pageno++;
                continue;
            }
            dviPage = (postscriptPage *) ppl_listGetItem(x->dvi->output->pages, pageno + 1);
            // If LaTeX did not typeset the title, give it zero size
            if (dviPage == NULL) {
                pd->TitleFinal_width = 0;
                pd->TitleFinal_height = 0;
                pageno++;
                continue;
            } // Such doom will trigger errors later
            //bb_left   = dviPage->boundingBox[0];
            bb_bottom = dviPage->boundingBox[1];
            //bb_right  = dviPage->boundingBox[2];
            bb_top = dviPage->boundingBox[3];
            ab_left = dviPage->textSizeBox[0];
            ab_bottom = dviPage->textSizeBox[1];
            ab_right = dviPage->textSizeBox[2];
            ab_top = dviPage->textSizeBox[3];
            height1 = fabs(ab_top - ab_bottom) * AB_ENLARGE_FACTOR;
            height2 = fabs(bb_top - bb_bottom) * BB_ENLARGE_FACTOR;
            pd->TitleFinal_height = ((height2 < height1) ? height2 : height1) * fs;
            pd->TitleFinal_width = ((ab_right - ab_left) + MARGIN_HSIZE) * fs;
            CombinedHeight += pd->TitleFinal_height;
            if (MinimumHeight < pd->TitleFinal_height) MinimumHeight = pd->TitleFinal_height;
            pageno++;
        }
    }

    // If number of columns is manually specified, repeatedly reduce height of legend until the desired number of columns is exceeded.
    if (x->current->settings.KeyColumns > 0) {
        BestHeight = TrialHeight = CombinedHeight + 2;
        // Reduce the height until there are too many columns, or the titles no longer fit
        while (TrialHeight > MinimumHeight) {
            GraphLegend_ArrangeToHeight(x, TrialHeight, &AttainedHeight, &Ncolumns, ColumnX, ColumnHeight,
                                        ColumnNItems);
            if (Ncolumns > x->current->settings.KeyColumns) break;
            if (AttainedHeight > TrialHeight) break;
            BestHeight = TrialHeight;
            TrialHeight = AttainedHeight - 1;
        }
    }

    // In ABOVE and BELOW alignment modes, repeatedly reduce height of legend until its width exceeds that of the plot
    else if ((kp == SW_KEYPOS_ABOVE) || (kp == SW_KEYPOS_BELOW)) {
        hfixed = 1;
        BestHeight = TrialHeight = CombinedHeight + 2;
        // Reduce the height until the legend is wider than the plot, or the titles no longer fit
        while (TrialHeight > MinimumHeight) {
            GraphLegend_ArrangeToHeight(x, TrialHeight, &AttainedHeight, &Ncolumns, ColumnX, ColumnHeight,
                                        ColumnNItems);
            if (ColumnX[Ncolumns] > width - 2 * LEGEND_MARGIN) break;
            if (AttainedHeight > TrialHeight) break;
            BestHeight = TrialHeight;
            TrialHeight = AttainedHeight - 1;
        }
    }

    // In all other modes, make maximum height of legend equal height of plot.
    else {
        vfixed = 1;
        BestHeight = height - 2 * LEGEND_MARGIN;
    }

    // Adjust spacing between legend items to justify them as necessary
    GraphLegend_ArrangeToHeight(x, BestHeight, &AttainedHeight, &Ncolumns, ColumnX, ColumnHeight, ColumnNItems);
    TrialHeight = vfixed ? (height - 2 * LEGEND_MARGIN) : AttainedHeight; // Vertical justification
    for (j = 0; j < Ncolumns; j++)
        // Do not stretch columns of fewer than two items, or columns which would need gaps larger than the maximum
        if ((ColumnNItems[j] < 2) || (ColumnHeight[j] < TrialHeight - LEGEND_VGAP_MAXIMUM * (ColumnNItems[j] - 1))) {
            TrialHeight = AttainedHeight;
            break;
        }
    for (j = 0; j < Ncolumns; j++)
        // Spread out the items in each column which can be stretched to the full height
        if (ColumnHeight[j] >= TrialHeight - LEGEND_VGAP_MAXIMUM * (ColumnNItems[j] - 1)) {
            double GapPerItem = (TrialHeight - ColumnHeight[j]) / (ColumnNItems[j] - 1), gap = 0.0;
            // Unless the gap is undefined, move each item in the column down by the gaps above it
            if (gsl_finite(GapPerItem)) {
                {
                    int iDataSet = -1; // Counts over all datasets, including those skipped because they have no title
                    for (pd = titledDataset(x->current->plotitems, &iDataSet); pd != NULL;
                         pd = titledDataset(pd->next, &iDataSet)) {
                        // Only items in this column
                        if (pd->TitleFinal_col == j) {
                            pd->TitleFinal_ypos -= gap;
                            gap += GapPerItem;
                        }
                    }
                }
            }
        }
    AttainedHeight = TrialHeight;
    // Horizonal justification
    if (hfixed && (ColumnX[Ncolumns] < width - 2 * LEGEND_MARGIN) && (Ncolumns > 1) &&
        (ColumnX[Ncolumns] >= width - 2 * LEGEND_MARGIN - LEGEND_HGAP_MAXIMUM * (Ncolumns - 1))) {
        const double GapPerColumn = (width - 2 * LEGEND_MARGIN - ColumnX[Ncolumns]) / Ncolumns;
        // Unless the gap is undefined, spread out the columns
        if (gsl_finite(GapPerColumn)) {
            {
                int iDataSet = -1; // Counts over all datasets, including those skipped because they have no title
                for (pd = titledDataset(x->current->plotitems, &iDataSet); pd != NULL;
                     pd = titledDataset(pd->next, &iDataSet)) {
                    pd->TitleFinal_xpos += pd->TitleFinal_col * GapPerColumn;
                }
            }
            for (j = 0; j < Ncolumns; j++) ColumnX[j] += j * GapPerColumn;
            ColumnX[Ncolumns] += (Ncolumns - 1) * GapPerColumn;
        }
    }

    // Translate legend to desired place on canvas (2D case)
    if (!x->current->ThreeDim) {
        // Place the legend in the requested position relative to the plot
        switch (kp) {
            case SW_KEYPOS_TR:
                xoff = width - ColumnX[Ncolumns] - LEGEND_MARGIN;
                yoff = height - LEGEND_MARGIN;
                break;
            case SW_KEYPOS_TM:
                xoff = width / 2 - ColumnX[Ncolumns] / 2;
                yoff = height - LEGEND_MARGIN;
                break;
            case SW_KEYPOS_TL:
                xoff = LEGEND_MARGIN;
                yoff = height - LEGEND_MARGIN;
                break;
            case SW_KEYPOS_MR:
                xoff = width - ColumnX[Ncolumns] - LEGEND_MARGIN;
                yoff = height / 2 + AttainedHeight / 2;
                break;
            case SW_KEYPOS_MM:
                xoff = width / 2 - ColumnX[Ncolumns] / 2;
                yoff = height / 2 + AttainedHeight / 2;
                break;
            case SW_KEYPOS_ML:
                xoff = LEGEND_MARGIN;
                yoff = height / 2 + AttainedHeight / 2;
                break;
            case SW_KEYPOS_BR:
                xoff = width - ColumnX[Ncolumns] - LEGEND_MARGIN;
                yoff = AttainedHeight + LEGEND_MARGIN;
                break;
            case SW_KEYPOS_BM:
                xoff = width / 2 - ColumnX[Ncolumns] / 2;
                yoff = AttainedHeight + LEGEND_MARGIN;
                break;
            case SW_KEYPOS_BL:
                xoff = LEGEND_MARGIN;
                yoff = AttainedHeight + LEGEND_MARGIN;
                break;
            case SW_KEYPOS_ABOVE:
                xoff = width / 2 - ColumnX[Ncolumns] / 2;
                yoff = AttainedHeight + LEGEND_MARGIN + x->current->PlotTopMargin -
                        x->current->settings.OriginY.real * M_TO_PS;
                break;
            case SW_KEYPOS_BELOW:
                xoff = width / 2 - ColumnX[Ncolumns] / 2;
                yoff = -LEGEND_MARGIN + x->current->PlotBottomMargin - x->current->settings.OriginY.real * M_TO_PS;
                break;
            case SW_KEYPOS_OUTSIDE:
                xoff = LEGEND_MARGIN;
                yoff = height - LEGEND_MARGIN;
                xoff += x->current->PlotRightMargin - x->current->settings.OriginX.real * M_TO_PS;
                break;
        }
        xoff += x->current->settings.OriginX.real * M_TO_PS;
        yoff += x->current->settings.OriginY.real * M_TO_PS;
    }

    // Translate legend to desired place on canvas (3D case)
    else {
        SortByAzimuthTarget = -999;

        // Place the legend in the requested position relative to the cube, noting the direction in which it lies from
        // the centre of the plot
        switch (kp) {
            case SW_KEYPOS_TR:
                SortByAzimuthTarget = 1 * M_PI / 4;
                xoff = LEGEND_MARGIN + x->current->PlotRightMargin;
                yoff = AttainedHeight + LEGEND_MARGIN;
                break;
            case SW_KEYPOS_TM:
                SortByAzimuthTarget = 0 * M_PI / 4;
                xoff = -ColumnX[Ncolumns] / 2;
                yoff = AttainedHeight + LEGEND_MARGIN + x->current->PlotTopMargin;
                break;
            case SW_KEYPOS_TL:
                SortByAzimuthTarget = -1 * M_PI / 4;
                xoff = -ColumnX[Ncolumns] - LEGEND_MARGIN + x->current->PlotLeftMargin;
                yoff = AttainedHeight + LEGEND_MARGIN;
                break;
            case SW_KEYPOS_MR:
                SortByAzimuthTarget = 2 * M_PI / 4;
                xoff = LEGEND_MARGIN + x->current->PlotRightMargin;
                yoff = AttainedHeight / 2;
                break;
            case SW_KEYPOS_MM:
                xoff = -ColumnX[Ncolumns] / 2 + x->current->settings.OriginX.real * M_TO_PS;
                yoff = AttainedHeight / 2 + x->current->settings.OriginY.real * M_TO_PS;
                break;
            case SW_KEYPOS_ML:
                SortByAzimuthTarget = -2 * M_PI / 4;
                xoff = -ColumnX[Ncolumns] - LEGEND_MARGIN + x->current->PlotLeftMargin;
                yoff = AttainedHeight / 2;
                break;
            case SW_KEYPOS_BR:
                SortByAzimuthTarget = 3 * M_PI / 4;
                xoff = LEGEND_MARGIN + x->current->PlotRightMargin;
                yoff = -LEGEND_MARGIN;
                break;
            case SW_KEYPOS_BM:
                SortByAzimuthTarget = 4 * M_PI / 4;
                xoff = -ColumnX[Ncolumns] / 2;
                yoff = -LEGEND_MARGIN + x->current->PlotBottomMargin;
                break;
            case SW_KEYPOS_BL:
                SortByAzimuthTarget = -3 * M_PI / 4;
                xoff = -ColumnX[Ncolumns] - LEGEND_MARGIN + x->current->PlotLeftMargin;
                yoff = -LEGEND_MARGIN;
                break;

            // Positions above and below the plot
            case SW_KEYPOS_ABOVE:
                xoff = -ColumnX[Ncolumns] / 2 + x->current->settings.OriginX.real * M_TO_PS;
                yoff = AttainedHeight + LEGEND_MARGIN + x->current->PlotTopMargin;
                break;
            case SW_KEYPOS_BELOW:
                xoff = -ColumnX[Ncolumns] / 2 + x->current->settings.OriginX.real * M_TO_PS;
                yoff = -LEGEND_MARGIN + x->current->PlotBottomMargin;
                break;
            case SW_KEYPOS_OUTSIDE:
                xoff = LEGEND_MARGIN + x->current->PlotRightMargin;
                yoff = -LEGEND_MARGIN + x->current->PlotTopMargin;
                break;
        }

        // Find the vertex of the graph closest to the target azimuth
        if (SortByAzimuthTarget > -100) {
            int i;
            double xap, yap, zap, data[3 * 8];
            const double origin_x = x->current->settings.OriginX.real * M_TO_PS;
            const double origin_y = x->current->settings.OriginY.real * M_TO_PS;
            // Project each corner of the cube onto the page
            for (i = 0; i < 8; i++) {
                xap = ((i & 1) != 0);
                yap = ((i & 2) != 0);
                zap = ((i & 4) != 0);
                eps_plot_ThreeDimProject(xap, yap, zap, &x->current->settings, origin_x, origin_y, width, height,
                                         zdepth, data + 3 * i, data + 3 * i + 1, data + 3 * i + 2);
            }
            SortByAzimuthXCentre = origin_x;
            SortByAzimuthYCentre = origin_y;
            qsort((void *) (data), 8, 3 * sizeof(double), SortByDepth);
            if ((data[3 * 0 + 2] == data[3 * 1 + 2]) &&
                (hypot(data[3 * 0 + 0] - origin_x, data[3 * 0 + 1] - origin_y) >
                 hypot(data[3 * 1 + 0] - origin_x, data[3 * 1 + 1] - origin_y)))
                TriSwap(data + 3 * 0, data + 3 * 1);
            if ((data[3 * 7 + 2] == data[3 * 6 + 2]) &&
                (hypot(data[3 * 7 + 0] - origin_x, data[3 * 7 + 1] - origin_y) >
                 hypot(data[3 * 6 + 0] - origin_x, data[3 * 6 + 1] - origin_y)))
                TriSwap(data + 3 * 7, data + 3 * 6);
            qsort((void *) (data + 3), 6, 3 * sizeof(double), SortByAzimuthProximity);
            if ((kp == SW_KEYPOS_TM) || (kp == SW_KEYPOS_BM)) xoff += data[3 * 1 + 0];
            else yoff += data[3 * 1 + 1];
        }
    }

    // Apply any offset requested by the user, and move each entry to its place
    xoff += x->current->settings.KeyXOff.real * M_TO_PS;
    yoff += x->current->settings.KeyYOff.real * M_TO_PS;
    {
        int iDataSet = -1; // Counts over all datasets, including those skipped because they have no title
        for (pd = titledDataset(x->current->plotitems, &iDataSet); pd != NULL;
             pd = titledDataset(pd->next, &iDataSet)) {
            pd->TitleFinal_xpos += xoff;
            pd->TitleFinal_ypos += yoff;
        }
    }

    // Finally loop over all datasets to display legend items
    {
        int iDataSet = -1; // Counts over all datasets, including those skipped because they have no title
        for (pd = titledDataset(x->current->plotitems, &iDataSet); pd != NULL;
             pd = titledDataset(pd->next, &iDataSet)) {
            int xyzaxis[3];
            pplset_axis *a1, *a2, *a3, *axissets[3];
            dataTable *data = x->current->plotdata[iDataSet];
            // Datasets with no data have no icon
            if ((data == NULL) || (data->Nrows < 1)) {
                x->LaTeXpageno++;
                continue;
            } // No data present: skip this dataset's entry, but draw the others
            axissets[0] = x->current->XAxes;
            axissets[1] = x->current->YAxes;
            axissets[2] = x->current->ZAxes;
            a1 = &axissets[pd->axis1xyz][pd->axis1];
            a2 = &axissets[pd->axis2xyz][pd->axis2];
            a3 = &axissets[pd->axis3xyz][pd->axis3];
            xyzaxis[pd->axis1xyz] = 0;
            xyzaxis[pd->axis2xyz] = 1;
            xyzaxis[pd->axis3xyz] = 2;
            eps_plot_LegendIcon(x, iDataSet, pd, pd->TitleFinal_xpos + MARGIN_HSIZE_LEFT / 2,
                                pd->TitleFinal_ypos - pd->TitleFinal_height / 2, MARGIN_HSIZE_LEFT, a1, a2, a3,
                                xyzaxis[0], xyzaxis[1], xyzaxis[2]);
            pageno = x->LaTeXpageno++;
            ppl_withWordsZero(x->c, &ww);
            // Write the titles in the color set with "set textcolor"
            if (x->current->settings.TextColor > 0) {
                ww.color = x->current->settings.TextColor;
                ww.USEcolor = 1;
            } else {
                ww.Col1234Space = x->current->settings.TextCol1234Space;
                ww.color1 = x->current->settings.TextColor1;
                ww.color2 = x->current->settings.TextColor2;
                ww.color3 = x->current->settings.TextColor3;
                ww.color4 = x->current->settings.TextColor4;
                ww.USEcolor1234 = 1;
            }
            eps_core_SetColor(x, &ww, 1);
            if (eps_core_isVisible(x))
                canvas_EPSRenderTextItem(x, NULL, pageno, (pd->TitleFinal_xpos + MARGIN_HSIZE_LEFT) / M_TO_PS,
                                         (pd->TitleFinal_ypos - pd->TitleFinal_height / 2) / M_TO_PS, SW_HALIGN_LEFT,
                                         SW_VALIGN_CENT, x->CurrentColor, fs, 0.0, NULL, NULL);
        }
    }
    return;
}
