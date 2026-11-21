// canvasItems.c
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

// Implements the commands which add items to the multiplot canvas (arrow, line, box, circle, arc, ellipse, eps, image,
// piechart, plot, point, polygon and text), and those which manage it (list, delete, undelete, move, swap and clear).
// Each item is stored as a canvas_item in a list kept in order of item number, recording everything needed to draw it,
// including a copy of the settings in force when it was made; after each change, the canvas is redrawn by
// epsMaker/canvasDraw.c, and an item which fails to draw is removed again. ppl_canvas_item_textify() turns an item back
// into the command which would make it.

#define _CANVASITEMS_C 1

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glob.h>
#include <wordexp.h>

#include <gsl/gsl_math.h>

#include "canvasItems.h"
#include "userspace/stack.h"
#include "coreUtils/errorReport.h"
#include "datafile.h"

#include "coreUtils/dict.h"
#include "coreUtils/memAlloc.h"
#include "epsMaker/canvasDraw.h"
#include "epsMaker/eps_plot.h"
#include "epsMaker/eps_plot_styles.h"
#include "expressions/expCompile_fns.h"
#include "expressions/expEval.h"
#include "expressions/traceback_fns.h"
#include "mathsTools/dcfmath.h"
#include "settings/arrows_fns.h"
#include "settings/axes_fns.h"
#include "settings/labels_fns.h"
#include "settings/settings.h"
#include "settings/settingTypes.h"
#include "settings/withWords_fns.h"
#include "stringTools/asciidouble.h"
#include "parser/cmdList.h"
#include "parser/parser.h"
#include "userspace/context.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjPrint.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"


//! canvas_item_delete - Free an item on the multiplot canvas, including its settings, axes, arrows, labels, plot
//! items and any data stored with them
//! \param [in] c - The Pyxplot context
//! \param [in] ptr - The canvas item to free

static void canvas_item_delete(ppl_context *c, canvas_item *ptr) {
    // Variables for freeing the item
    int i;
    canvas_itemlist *canvas_items = c->canvas_items;
    canvas_plotrange *pr, *pr2;
    canvas_plotdesc *pd, *pd2;

    // Free the points of a polygon
    if (ptr->polygonPoints != NULL) {
        free(ptr->polygonPoints);
        ptr->polygonPoints = NULL;
    }
    // Free the item's text
    if (ptr->text != NULL) {
        free(ptr->text);
        ptr->text = NULL;
    }
    ppl_withWordsDestroy(c, &(ptr->settings.dataStyle));
    ppl_withWordsDestroy(c, &(ptr->settings.funcStyle));
    pplExpr_free(ptr->settings.ColMapExpr);
    ptr->settings.ColMapExpr = NULL;
    pplExpr_free(ptr->settings.MaskExpr);
    ptr->settings.MaskExpr = NULL;
    pplExpr_free(ptr->settings.c1format);
    ptr->settings.c1format = NULL;
    // Free the x axes
    if (ptr->XAxes != NULL) {
        for (i = 0; i < MAX_AXES; i++) pplaxis_destroy(c, &(ptr->XAxes[i]));
        free(ptr->XAxes);
        ptr->XAxes = NULL;
    }
    // Free the y axes
    if (ptr->YAxes != NULL) {
        for (i = 0; i < MAX_AXES; i++) pplaxis_destroy(c, &(ptr->YAxes[i]));
        free(ptr->YAxes);
        ptr->YAxes = NULL;
    }
    // Free the z axes
    if (ptr->ZAxes != NULL) {
        for (i = 0; i < MAX_AXES; i++) pplaxis_destroy(c, &(ptr->ZAxes[i]));
        free(ptr->ZAxes);
        ptr->ZAxes = NULL;
    }
    pplarrow_list_destroy(c, &(ptr->arrow_list));
    ppllabel_list_destroy(c, &(ptr->label_list));
    ppl_withWordsDestroy(c, &(ptr->with_data));

    // Delete range structures
    pr = ptr->plotranges;
    // Free the list of ranges given in a plot command
    while (pr != NULL) {
        pr2 = pr->next;
        free(pr);
        pr = pr2;
    }

    // Delete plot item descriptors
    pd = ptr->plotitems;
    // Free each dataset of a plot
    while (pd != NULL) {
        pd2 = pd->next;
        if (pd->functions != NULL)
            // Free the functions plotted
            for (i = 0; i < pd->NFunctions; i++) {
                pplExpr_free(pd->functions[i]);
                pd->functions[i] = NULL;
            }
        if (pd->UsingList != NULL)
            // Free the using expressions
            for (i = 0; i < pd->NUsing; i++) {
                pplExpr_free(pd->UsingList[i]);
                pd->UsingList[i] = NULL;
            }
        if (pd->vectors != NULL)
            for (i = 0; i < pd->NFunctions; i++) ppl_garbageObject(&pd->vectors[i]);
        // Free the filename
        if (pd->filename != NULL) {
            free(pd->filename);
            pd->filename = NULL;
        }
        // Free the list of functions
        if (pd->functions != NULL) {
            free(pd->functions);
            pd->functions = NULL;
        }
        // Free the list of vectors
        if (pd->vectors != NULL) {
            free(pd->vectors);
            pd->vectors = NULL;
        }
        // Free the label expression
        if (pd->label != NULL) {
            pplExpr_free(pd->label);
            pd->label = NULL;
        }
        // Free the select expression
        if (pd->SelectCriterion != NULL) {
            pplExpr_free(pd->SelectCriterion);
            pd->SelectCriterion = NULL;
        }
        // Free the title
        if (pd->title != NULL) {
            free(pd->title);
            pd->title = NULL;
        }
        // Free the list of using expressions
        if (pd->UsingList != NULL) {
            free(pd->UsingList);
            pd->UsingList = NULL;
        }
        ppl_withWordsDestroy(c, &pd->ww);
        ppl_withWordsDestroy(c, &pd->ww_final);
        free(pd);
        pd = pd2;
    }

    // Reseal doubly-linked list
    if (ptr->prev != NULL) ptr->prev->next = ptr->next;
    else canvas_items->first = ptr->next;
    if (ptr->next != NULL) ptr->next->prev = ptr->prev;
    else canvas_items->last = ptr->prev;

    // Free the item itself
    free(ptr);
    ptr = NULL;
    return;
}

//! canvas_itemlist_add - Add a new item to the multiplot canvas, or replace an existing item if the command
//! specifies an item number to edit. If multiplot mode is off, the canvas is cleared first. The user's current
//! settings are copied into the new item.
//! \param [in] c - The Pyxplot context
//! \param [in] command - The parser output stack of the command which creates the item
//! \param [in] type - The type of canvas item, one of the CANVAS_* constants
//! \param [out] output - Set to point to the new canvas item
//! \param [out] id - Set to the ID number of the new canvas item
//! \param [in] includeAxes - If true, also copy the user's current axes, arrows and labels into the new item
//! \return Zero on success, or one on failure

static int canvas_itemlist_add(ppl_context *c, pplObj *command, const int type, canvas_item **output, int *id,
                               const unsigned char includeAxes) {
    canvas_itemlist *canvas_items;
    canvas_item *ptr, *next, *prev, **insertpointA, **insertpointB;
    int i, editNo, gotEditNo;

    // If we're not in multiplot mode, clear the canvas now
    if (c->set->term_current.multiplot == SW_ONOFF_OFF) ppl_directive_clear(c, NULL, NULL, 0);
    canvas_items = c->canvas_items;

    // Ensure that multiplot canvas list is initialised before trying to use it
    if (canvas_items == NULL) {
        canvas_items = (canvas_itemlist *) malloc(sizeof(canvas_itemlist));
        // Failure to allocate memory is reported as an error
        if (canvas_items == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        }
        c->canvas_items = (void *) canvas_items;
        canvas_items->first = NULL;
        canvas_items->last = NULL;
    }

    // With no item number given, the item is added after the last one; otherwise it replaces the item with that number
    gotEditNo = (command[PARSE_arc_editno].objType == PPLOBJ_NUM);
    // Number the item after the last item on the canvas
    if (!gotEditNo) {
        int prevId;
        insertpointA = &(canvas_items->last);
        next = NULL;
        prev = (*insertpointA == NULL) ? NULL : canvas_items->last;
        insertpointB = (*insertpointA == NULL) ? &(canvas_items->first) : &(canvas_items->last->next);
        prevId = (*insertpointA == NULL) ? 0 : canvas_items->last->id;
        editNo = prevId + 1;
        // There is a fixed limit on the number of items
        if (editNo >= MULTIPLOT_MAXINDEX) {
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1, "The multiplot canvas is full.");
            return 1;
        }
    } else {
        // Item numbers index arrays of MULTIPLOT_MAXINDEX entries, so must be within range
        const double editNoReal = command[PARSE_arc_editno].real;
        // The item number must be within range
        if (!(editNoReal > -0.5) || !(editNoReal < MULTIPLOT_MAXINDEX - 0.5)) {
            char msg[PPL_MSG_LENGTH];
            ppl_snprintfTrunc(msg, sizeof(msg), "Multiplot item numbers must be in the range 0 to %d.",
                              MULTIPLOT_MAXINDEX - 1);
            ppl_error(&c->errcontext, ERR_RANGE, -1, -1, msg);
            return 1;
        }
        editNo = (int) round(editNoReal);
        insertpointB = &(canvas_items->first);
        prev = NULL;
        // Find where the item goes in the list, which is kept in order of item number
        while ((*insertpointB != NULL) && ((*insertpointB)->id < editNo)) {
            prev = *insertpointB;
            insertpointB = &((*insertpointB)->next);
        }
        next = *insertpointB;
        // Link the item at the end of the list, or before the next item
        if (next == NULL) {
            insertpointA = &(canvas_items->last);
        } else if (next->id == editNo) {
            next = next->next;
            if (next == NULL) insertpointA = &(canvas_items->last);
            else insertpointA = &(next->prev);
            canvas_item_delete(c, *insertpointB);
        } else {
            insertpointA = &(next->prev);
        }
    }
    ptr = (canvas_item *) malloc(sizeof(canvas_item));
    // Failure to allocate memory is reported as an error
    if (ptr == NULL) {
        ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
        return 1;
    }
    *insertpointA = *insertpointB = ptr;
    ptr->next = next; // Link doubly-linked list
    ptr->prev = prev;
    ptr->polygonPoints = NULL;
    ptr->text = NULL;
    ptr->plotitems = NULL;
    ptr->plotranges = NULL;
    ptr->id = editNo;
    ptr->type = type;
    ptr->deleted = 0;
    ppl_withWordsZero(c, &ptr->with_data);

    // Copy the user's current settings
    ptr->settings = c->set->graph_current;
    ptr->settings.ColMapExpr = pplExpr_cpy(c->set->graph_current.ColMapExpr);
    ptr->settings.MaskExpr = pplExpr_cpy(c->set->graph_current.MaskExpr);
    ptr->settings.c1format = pplExpr_cpy(c->set->graph_current.c1format);
    ppl_withWordsCpy(c, &ptr->settings.dataStyle, &c->set->graph_current.dataStyle);
    ppl_withWordsCpy(c, &ptr->settings.funcStyle, &c->set->graph_current.funcStyle);
    // Graphs have their own copies of the axes
    if (includeAxes) {
        ptr->XAxes = (pplset_axis *) malloc(MAX_AXES * sizeof(pplset_axis));
        ptr->YAxes = (pplset_axis *) malloc(MAX_AXES * sizeof(pplset_axis));
        ptr->ZAxes = (pplset_axis *) malloc(MAX_AXES * sizeof(pplset_axis));
        // Report a failure to allocate memory, freeing any axes which were allocated
        if ((ptr->XAxes == NULL) || (ptr->YAxes == NULL) || (ptr->ZAxes == NULL)) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (A).");
            // Free the x axes
            if (ptr->XAxes != NULL) {
                free(ptr->XAxes);
                ptr->XAxes = NULL;
            }
            // Free the y axes
            if (ptr->YAxes != NULL) {
                free(ptr->YAxes);
                ptr->YAxes = NULL;
            }
            // Free the z axes
            if (ptr->ZAxes != NULL) {
                free(ptr->ZAxes);
                ptr->ZAxes = NULL;
            }
            free(ptr);
            ptr = NULL;
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        } else {
            for (i = 0; i < MAX_AXES; i++) pplaxis_copy(c, &(ptr->XAxes[i]), &(c->set->XAxes[i]));
            for (i = 0; i < MAX_AXES; i++) pplaxis_copy(c, &(ptr->YAxes[i]), &(c->set->YAxes[i]));
            for (i = 0; i < MAX_AXES; i++) pplaxis_copy(c, &(ptr->ZAxes[i]), &(c->set->ZAxes[i]));
        }
        pplarrow_list_copy(c, &ptr->arrow_list, &c->set->pplarrow_list);
        ppllabel_list_copy(c, &ptr->label_list, &c->set->ppllabel_list);
    } else {
        ptr->XAxes = ptr->YAxes = ptr->ZAxes = NULL;
        ptr->arrow_list = NULL;
        ptr->label_list = NULL;
    }

    *output = ptr; // Return pointer to the newly-created canvas item
    *id = ptr->id;
    return 0;
}

//! ppl_directive_clear - Implement the clear command, which removes all items from the multiplot canvas. Also
//! called whenever the canvas is to be cleared.
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \return Zero

int ppl_directive_clear(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive) {
    canvas_itemlist *canvas_items = c->canvas_items;
    canvas_item *ptr, *next;

    // There is nothing to clear if the canvas has never been used
    if (canvas_items == NULL) return 0;
    ptr = canvas_items->first;
    // Free each item
    while (ptr != NULL) {
        next = ptr->next;
        ptr->prev = NULL;
        canvas_item_delete(c, ptr);
        ptr = next;
    }
    free(canvas_items);
    canvas_items = NULL;
    c->canvas_items = NULL;
    c->replotFocus = -1;
    return 0;
}

//! ppl_canvas_item_textify - Write the command which would need to be typed to produce a given canvas item
//! \param [in] c - The Pyxplot context
//! \param [in] ptr - The canvas item
//! \param [out] output - Character buffer into which to write the command
//! \param [in] outLen - The size of the buffer <output>
//! \return Pointer to <output>

char *ppl_canvas_item_textify(ppl_context *c, canvas_item *ptr, char *output, const size_t outLen) {
    // Write the command for the item's type, with all of its parameters
    int i, j;
    if (ptr->type == CANVAS_ARROW) // Produce textual representations of arrow commands
    {
        ppl_snprintfTrunc(
                output, outLen, "%s item %d from %s,%s to %s,%s with %s",
                (ptr->ArrowType == SW_ARROWTYPE_NOHEAD) ? "line" : "arrow", ptr->id,
                ppl_numericDisplay(ptr->xpos * 100, c->numdispBuff[0], c->set->term_current.SignificantFigures,
                                   (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                ppl_numericDisplay(ptr->ypos * 100, c->numdispBuff[1], c->set->term_current.SignificantFigures,
                                   (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                ppl_numericDisplay((ptr->xpos + ptr->xpos2) * 100, c->numdispBuff[2],
                                   c->set->term_current.SignificantFigures,
                                   (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                ppl_numericDisplay((ptr->ypos + ptr->ypos2) * 100, c->numdispBuff[3],
                                   c->set->term_current.SignificantFigures,
                                   (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                *(char **) ppl_fetchSettingName(&c->errcontext, ptr->ArrowType, SW_ARROWTYPE_INT,
                                                (void *) SW_ARROWTYPE_STR, sizeof(char *)));
        i = strlen(output);
        {
            char words[LSTR_LENGTH];
            ppl_withWordsPrint(c, &ptr->with_data, words, sizeof(words));
            if (words[0] != '\0') ppl_strAppendf(output, outLen, &i, " %s", words);
        }
    } else if (ptr->type == CANVAS_BOX) // Produce textual representations of box commands
    {
        ppl_snprintfTrunc(output, outLen, "box item %d ", ptr->id);
        i = strlen(output);
        ppl_strAppendf(output, outLen, &i, ptr->xpos2set ? "at %s,%s width %s height %s" : "from %s,%s to %s,%s",
                       ppl_numericDisplay(ptr->xpos * 100, c->numdispBuff[0], c->set->term_current.SignificantFigures,
                                          (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                       ppl_numericDisplay(ptr->ypos * 100, c->numdispBuff[1], c->set->term_current.SignificantFigures,
                                          (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                       ppl_numericDisplay((ptr->xpos * (!ptr->xpos2set) + ptr->xpos2) * 100, c->numdispBuff[2],
                                          c->set->term_current.SignificantFigures,
                                          (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                       ppl_numericDisplay((ptr->ypos * (!ptr->xpos2set) + ptr->ypos2) * 100, c->numdispBuff[3],
                                          c->set->term_current.SignificantFigures,
                                          (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        ppl_strAppendf(output, outLen, &i, " rotate %s",
                       ppl_numericDisplay(ptr->rotation * 180 / M_PI, c->numdispBuff[2],
                                          c->set->term_current.SignificantFigures,
                                          (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        ppl_withWordsAppend(c, &ptr->with_data, output, outLen, &i);
    } else if (ptr->type == CANVAS_CIRC) // Produce textual representations of circle commands
    {
        ppl_snprintfTrunc(
                output, outLen, "%s item %d at %s,%s radius %s", ptr->xfset ? "arc" : "circle", ptr->id,
                ppl_numericDisplay(ptr->xpos * 100, c->numdispBuff[0], c->set->term_current.SignificantFigures,
                                   (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                ppl_numericDisplay(ptr->ypos * 100, c->numdispBuff[1], c->set->term_current.SignificantFigures,
                                   (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                ppl_numericDisplay(ptr->xpos2 * 100, c->numdispBuff[2], c->set->term_current.SignificantFigures,
                                   (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i = strlen(output);
        // The position of the focus, if given
        if (ptr->xfset) {
            ppl_strAppendf(
                    output, outLen, &i, " from %s to %s",
                    ppl_numericDisplay(ptr->xf * 180 / M_PI, c->numdispBuff[0], c->set->term_current.SignificantFigures,
                                       (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                    ppl_numericDisplay(ptr->yf * 180 / M_PI, c->numdispBuff[1], c->set->term_current.SignificantFigures,
                                       (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
            i += strlen(output + i);
        }
        ppl_withWordsAppend(c, &ptr->with_data, output, outLen, &i);
    } else if (ptr->type == CANVAS_ELLPS) // Produce textual representations of ellipse commands
    {
        ppl_snprintfTrunc(output, outLen, "ellipse item %d", ptr->id);
        i = strlen(output);
        if (ptr->x1set)
            ppl_strAppendf(output, outLen, &i, " from %s,%s to %s,%s",
                           ppl_numericDisplay(ptr->x1 * 100, c->numdispBuff[0], c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                           ppl_numericDisplay(ptr->y1 * 100, c->numdispBuff[1], c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                           ppl_numericDisplay(ptr->x2 * 100, c->numdispBuff[2], c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                           ppl_numericDisplay(ptr->y2 * 100, c->numdispBuff[3], c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        if (ptr->xcset)
            ppl_strAppendf(output, outLen, &i, " centre %s,%s",
                           ppl_numericDisplay(ptr->xc * 100, c->numdispBuff[0], c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                           ppl_numericDisplay(ptr->yc * 100, c->numdispBuff[1], c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        if (ptr->xfset)
            ppl_strAppendf(output, outLen, &i, " focus %s,%s",
                           ppl_numericDisplay(ptr->xf * 100, c->numdispBuff[0], c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                           ppl_numericDisplay(ptr->yf * 100, c->numdispBuff[1], c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        if (ptr->aset)
            ppl_strAppendf(output, outLen, &i, " SemiMajorAxis %s",
                           ppl_numericDisplay(ptr->a * 100, c->numdispBuff[0], c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        if (ptr->bset)
            ppl_strAppendf(output, outLen, &i, " SemiMinorAxis %s",
                           ppl_numericDisplay(ptr->b * 100, c->numdispBuff[0], c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        if (ptr->eccset)
            ppl_strAppendf(output, outLen, &i, " eccentricity %s",
                           ppl_numericDisplay(ptr->ecc, c->numdispBuff[0], c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        if (ptr->slrset)
            ppl_strAppendf(output, outLen, &i, " SemiLatusRectum %s",
                           ppl_numericDisplay(ptr->slr * 100, c->numdispBuff[0],
                                              c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        if (ptr->arcset)
            ppl_strAppendf(output, outLen, &i, " arc from %s to %s",
                           ppl_numericDisplay(ptr->arcfrom * 180 / M_PI, c->numdispBuff[0],
                                              c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                           ppl_numericDisplay(ptr->arcto * 180 / M_PI, c->numdispBuff[1],
                                              c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        ppl_strAppendf(output, outLen, &i, " rotate %s",
                       ppl_numericDisplay(ptr->rotation * 180 / M_PI, c->numdispBuff[0],
                                          c->set->term_current.SignificantFigures,
                                          (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        ppl_withWordsAppend(c, &ptr->with_data, output, outLen, &i);
    } else if (ptr->type == CANVAS_EPS) // Produce textual representations of eps commands
    {
        ppl_snprintfTrunc(output, outLen, "eps item %d ", ptr->id);
        i = strlen(output);
        ppl_strEscapify(ptr->text, output + i, outLen - i);
        i += strlen(output + i);
        ppl_strAppendf(output, outLen, &i, " at %s,%s",
                       ppl_numericDisplay(ptr->xpos * 100, c->numdispBuff[0], c->set->term_current.SignificantFigures,
                                          (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                       ppl_numericDisplay(ptr->ypos * 100, c->numdispBuff[1], c->set->term_current.SignificantFigures,
                                          (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        if (ptr->xpos2set)
            ppl_strAppendf(output, outLen, &i, " width %s",
                           ppl_numericDisplay(ptr->xpos2 * 100, c->numdispBuff[0],
                                              c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        if (ptr->ypos2set)
            ppl_strAppendf(output, outLen, &i, " height %s",
                           ppl_numericDisplay(ptr->ypos2 * 100, c->numdispBuff[0],
                                              c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        ppl_strAppendf(output, outLen, &i, " rotate %s",
                       ppl_numericDisplay(ptr->rotation * 180 / M_PI, c->numdispBuff[2],
                                          c->set->term_current.SignificantFigures,
                                          (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        // The clip option
        if (ptr->clip) {
            ppl_strAppendf(output, outLen, &i, " clip");
            i += strlen(output + i);
        }
        // The calcbbox option
        if (ptr->calcbbox) {
            ppl_strAppendf(output, outLen, &i, " calcbbox");
            i += strlen(output + i);
        }
    } else if (ptr->type == CANVAS_IMAGE) // Produce textual representations of image commands
    {
        ppl_snprintfTrunc(output, outLen, "image item %d ", ptr->id);
        i = strlen(output);
        ppl_strEscapify(ptr->text, output + i, outLen - i);
        i += strlen(output + i);
        ppl_strAppendf(output, outLen, &i, " at %s,%s",
                       ppl_numericDisplay(ptr->xpos * 100, c->numdispBuff[0], c->set->term_current.SignificantFigures,
                                          (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                       ppl_numericDisplay(ptr->ypos * 100, c->numdispBuff[1], c->set->term_current.SignificantFigures,
                                          (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        // The smooth option
        if (ptr->smooth) {
            ppl_strAppendf(output, outLen, &i, " smooth");
            i += strlen(output + i);
        }
        // The notransparent option
        if (ptr->NoTransparency) {
            ppl_strAppendf(output, outLen, &i, " NoTransparency");
            i += strlen(output + i);
        }
        // A custom transparent color
        if (ptr->CustomTransparency) {
            ppl_strAppendf(output, outLen, &i, " transparent rgb%d:%d:%d", ptr->TransColR, ptr->TransColG,
                           ptr->TransColB);
            i += strlen(output + i);
        }
        if (ptr->xpos2set)
            ppl_strAppendf(output, outLen, &i, " width %s",
                           ppl_numericDisplay(ptr->xpos2 * 100, c->numdispBuff[0],
                                              c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        if (ptr->ypos2set)
            ppl_strAppendf(output, outLen, &i, " height %s",
                           ppl_numericDisplay(ptr->ypos2 * 100, c->numdispBuff[0],
                                              c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        ppl_strAppendf(output, outLen, &i, " rotate %s",
                       ppl_numericDisplay(ptr->rotation * 180 / M_PI, c->numdispBuff[2],
                                          c->set->term_current.SignificantFigures,
                                          (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
    } else if (ptr->type == CANVAS_PIE) // Produce textual representations of piechart commands
    {
        canvas_plotdesc *pd;

        // A piechart command, followed by the description of its data
        ppl_snprintfTrunc(output, outLen, "piechart item %d", ptr->id);
        i = strlen(output);
        pd = ptr->plotitems;
        // Describe the dataset, if there is one
        if (pd != NULL) {
            // Data from a file or vectors, or from functions
            if (!pd->function) {
                // The data file or vectors
                if ((pd->filename != NULL) || (pd->vectors != NULL)) {
                    ppl_strAppendChar(output, outLen, &i, ' ');
                    ppl_strEscapify(pd->filename, output + i, outLen - i);
                    i += strlen(output + i); // Filename of datafile we are plotting
                }
            } else
                for (j = 0; j < pd->NFunctions; j++) // Print out the list of functions which we are plotting
                {
                    ppl_strAppendChar(output, outLen, &i, (j != 0) ? ':' : ' ');
                    ppl_strAppendStripped(output, outLen, &i, pd->functions[j]->ascii);
                    i += strlen(output + i);
                }
            // The every modifier
            if (pd->EverySet > 0) {
                ppl_strAppendf(output, outLen, &i, " every %ld", pd->EveryList[0]);
                i += strlen(output + i);
            } // Print out 'every' clause of plot command
            // Its second field
            if (pd->EverySet > 1) {
                ppl_strAppendf(output, outLen, &i, ":%ld", pd->EveryList[1]);
                i += strlen(output + i);
            }
            // Its third field
            if (pd->EverySet > 2) {
                ppl_strAppendf(output, outLen, &i, ":%ld", pd->EveryList[2]);
                i += strlen(output + i);
            }
            // Its fourth field
            if (pd->EverySet > 3) {
                ppl_strAppendf(output, outLen, &i, ":%ld", pd->EveryList[3]);
                i += strlen(output + i);
            }
            // Its fifth field
            if (pd->EverySet > 4) {
                ppl_strAppendf(output, outLen, &i, ":%ld", pd->EveryList[4]);
                i += strlen(output + i);
            }
            // Its sixth field
            if (pd->EverySet > 5) {
                ppl_strAppendf(output, outLen, &i, ":%ld", pd->EveryList[5]);
                i += strlen(output + i);
            }
            // The format of the labels
            if (ptr->text == NULL) {
                ppl_strAppendf(output, outLen, &i, " format auto");
                i += strlen(output + i);
            } else {
                ppl_strAppendf(output, outLen, &i, " format %s", ptr->text);
                i += strlen(output + i);
            }
            // The index modifier
            if (pd->IndexSet) {
                ppl_strAppendf(output, outLen, &i, " index %d", pd->index);
                i += strlen(output + i);
            } // Print index to use
            // The label modifier, and the position of the labels
            if ((pd->label != NULL) || (ptr->ArrowType != SW_PIEKEYPOS_AUTO)) {
                ppl_strAppendf(output, outLen, &i, " label %s",
                               *(char **) ppl_fetchSettingName(&c->errcontext, ptr->ArrowType, SW_PIEKEYPOS_INT,
                                                               (void *) SW_PIEKEYPOS_STR, sizeof(char *)));
                i += strlen(output + i);
                if (pd->label != NULL)
                    ppl_strAppendf(output, outLen, &i, " %s", pd->label->ascii); // Print label string
                i += strlen(output + i);
            }
            // The select modifier
            if (pd->SelectCriterion != NULL) {
                ppl_strAppendf(output, outLen, &i, " select %s", pd->SelectCriterion->ascii);
                i += strlen(output + i);
            } // Print select criterion
            ppl_withWordsAppend(c, &pd->ww, output, outLen, &i);
            i += strlen(output + i);
            ppl_strAppendf(output, outLen, &i, " using %s", (pd->UsingRowCols == DATAFILE_COL) ? "columns" : "rows");
            i += strlen(output + i); // Print using list
            // Each using item
            for (j = 0; j < pd->NUsing; j++) {
                ppl_strAppendChar(output, outLen, &i, (j != 0) ? ':' : ' ');
                ppl_strAppendf(output, outLen, &i, "%s", pd->UsingList[j]->ascii);
                i += strlen(output + i);
            }
        }
    } else if (ptr->type == CANVAS_PLOT) // Produce textual representations of plot commands
    {
        canvas_plotrange *pr;
        canvas_plotdesc *pd;
        pplObj v;
        unsigned char pr_first = 1, pd_first = 1;

        // A plot command, followed by its ranges and the description of each dataset
        ppl_snprintfTrunc(output, outLen, "plot item %d", ptr->id);
        i = strlen(output);
        if (ptr->ThreeDim) ppl_strAppendf(output, outLen, &i, "%s", " 3d");
        i += strlen(output + i);
        pr = ptr->plotranges; // Print out plot ranges
        // Write each range
        while (pr != NULL) {
            // Separate the ranges with spaces
            if (pr_first) {
                ppl_strAppendChar(output, outLen, &i, ' ');
                pr_first = 0;
            }
            ppl_strAppendChar(output, outLen, &i, '[');
            if (pr->AutoMinSet) ppl_strAppendChar(output, outLen, &i, '*');
            // The minimum, or * if automatic
            if (pr->MinSet) {
                v = pr->unit;
                v.real = pr->min;
                ppl_strAppendf(output, outLen, &i, "%s", ppl_unitsNumericDisplay(c, &v, 0, 0, 0));
                i += strlen(output + i);
            }
            // A colon, if either limit is given
            if (pr->MinSet || pr->MaxSet || pr->AutoMinSet || pr->AutoMaxSet) {
                ppl_strAppendf(output, outLen, &i, "%s", ":");
                i += strlen(output + i);
            }
            if (pr->AutoMaxSet) ppl_strAppendChar(output, outLen, &i, '*');
            // The maximum, or * if automatic
            if (pr->MaxSet) {
                v = pr->unit;
                v.real = pr->max;
                ppl_strAppendf(output, outLen, &i, "%s", ppl_unitsNumericDisplay(c, &v, 0, 0, 0));
                i += strlen(output + i);
            }
            ppl_strAppendf(output, outLen, &i, "%s", "]");
            i += strlen(output + i);
            pr = pr->next;
        }
        pd = ptr->plotitems; // Print out plotted items one by one
        // Write each dataset, separated by commas
        while (pd != NULL) {
            // Separate the datasets with commas
            if (pd_first) {
                pd_first = 0;
            } else {
                ppl_strAppendChar(output, outLen, &i, ',');
            }
            // Parametric plots
            if (pd->parametric) {
                ppl_strAppendf(output, outLen, &i, " parametric");
                i += strlen(output + i);
            }
            // The range of t
            if (pd->TRangeSet) {
                ppl_strAppendf(output, outLen, &i, " [%s:%s]", ppl_unitsNumericDisplay(c, &pd->Tmin, 0, 0, 0),
                               ppl_unitsNumericDisplay(c, &pd->Tmax, 1, 0, 0));
                i += strlen(output + i);
            }
            // The range of v
            if (pd->VRangeSet) {
                ppl_strAppendf(output, outLen, &i, " [%s:%s]", ppl_unitsNumericDisplay(c, &pd->Vmin, 0, 0, 0),
                               ppl_unitsNumericDisplay(c, &pd->Vmax, 1, 0, 0));
                i += strlen(output + i);
            }
            // Data from a file or vectors, or from functions
            if ((!pd->function) || (pd->vectors != NULL)) {
                // The data file, or the vectors
                if (pd->filename != NULL) {
                    ppl_strAppendChar(output, outLen, &i, ' ');
                    ppl_strEscapify(pd->filename, output + i, outLen - i);
                    i += strlen(output + i); // Filename of datafile we are plotting
                }
            } else
                for (j = 0; j < pd->NFunctions; j++) // Print out the list of functions which we are plotting
                {
                    ppl_strAppendChar(output, outLen, &i, (j != 0) ? ':' : ' ');
                    ppl_strAppendStripped(output, outLen, &i, pd->functions[j]->ascii);
                    i += strlen(output + i);
                }
            if (pd->axis1set || pd->axis2set || pd->axis3set) // Print axes to use
            {
                ppl_strAppendf(output, outLen, &i, "%s", " axes ");
                i += strlen(output + i);
                // The first axis
                if (pd->axis1set) {
                    ppl_strAppendf(output, outLen, &i, "%c%d", "xyzc"[pd->axis1xyz], pd->axis1);
                    i += strlen(output + i);
                }
                // The second axis
                if (pd->axis2set) {
                    ppl_strAppendf(output, outLen, &i, "%c%d", "xyzc"[pd->axis2xyz], pd->axis2);
                    i += strlen(output + i);
                }
                // The third axis
                if (pd->axis3set) {
                    ppl_strAppendf(output, outLen, &i, "%c%d", "xyzc"[pd->axis3xyz], pd->axis3);
                    i += strlen(output + i);
                }
            }
            // The every modifier
            if (pd->EverySet > 0) {
                ppl_strAppendf(output, outLen, &i, " every %ld", pd->EveryList[0]);
                i += strlen(output + i);
            } // Print out 'every' clause of plot command
            // Its second field
            if (pd->EverySet > 1) {
                ppl_strAppendf(output, outLen, &i, ":%ld", pd->EveryList[1]);
                i += strlen(output + i);
            }
            // Its third field
            if (pd->EverySet > 2) {
                ppl_strAppendf(output, outLen, &i, ":%ld", pd->EveryList[2]);
                i += strlen(output + i);
            }
            // Its fourth field
            if (pd->EverySet > 3) {
                ppl_strAppendf(output, outLen, &i, ":%ld", pd->EveryList[3]);
                i += strlen(output + i);
            }
            // Its fifth field
            if (pd->EverySet > 4) {
                ppl_strAppendf(output, outLen, &i, ":%ld", pd->EveryList[4]);
                i += strlen(output + i);
            }
            // Its sixth field
            if (pd->EverySet > 5) {
                ppl_strAppendf(output, outLen, &i, ":%ld", pd->EveryList[5]);
                i += strlen(output + i);
            }
            // The index modifier
            if (pd->IndexSet) {
                ppl_strAppendf(output, outLen, &i, " index %d", pd->index);
                i += strlen(output + i);
            } // Print index to use
            // The label modifier
            if (pd->label != NULL) {
                ppl_strAppendf(output, outLen, &i, " label %s", pd->label->ascii);
                i += strlen(output + i);
            } // Print label string
            // The select modifier
            if (pd->SelectCriterion != NULL) {
                ppl_strAppendf(output, outLen, &i, " select %s", pd->SelectCriterion->ascii);
                i += strlen(output + i);
            } // Print select criterion
            if (pd->ContinuitySet) // Print continuous / discontinuous flag
            {
                // The continuity of the data
                if (pd->continuity == DATAFILE_DISCONTINUOUS) {
                    ppl_strAppendf(output, outLen, &i, " discontinuous");
                    i += strlen(output + i);
                } else {
                    ppl_strAppendf(output, outLen, &i, " continuous");
                    i += strlen(output + i);
                }
            }
            // The title, or notitle
            if (pd->NoTitleSet) {
                ppl_strAppendf(output, outLen, &i, "%s", " notitle");
                i += strlen(output + i);
            } // notitle is set
            // A title given by the user
            else if (pd->TitleSet) {
                ppl_strAppendf(output, outLen, &i, "%s", " title ");
                i += strlen(output + i);
                ppl_strEscapify(pd->title, output + i, outLen - i);
                i += strlen(output + i);
            }
            ppl_withWordsAppend(c, &pd->ww, output, outLen, &i);
            ppl_strAppendf(output, outLen, &i, " using %s", (pd->UsingRowCols == DATAFILE_COL) ? "columns" : "rows");
            i += strlen(output + i); // Print using list
            // Each using item
            for (j = 0; j < pd->NUsing; j++) {
                ppl_strAppendChar(output, outLen, &i, (j != 0) ? ':' : ' ');
                ppl_strAppendf(output, outLen, &i, "%s", pd->UsingList[j]->ascii);
                i += strlen(output + i);
            }
            pd = pd->next;
        }
    } else if (ptr->type == CANVAS_POINT) // Produce textual representations of point commands
    {
        ppl_snprintfTrunc(
                output, outLen, "point item %d at %s,%s", ptr->id,
                ppl_numericDisplay(ptr->xpos * 100, c->numdispBuff[0], c->set->term_current.SignificantFigures,
                                   (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                ppl_numericDisplay(ptr->ypos * 100, c->numdispBuff[1], c->set->term_current.SignificantFigures,
                                   (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i = strlen(output);
        // A point's label
        if (ptr->text != NULL) {
            ppl_strAppendf(output, outLen, &i, " label ");
            i += strlen(output + i);
            ppl_strEscapify(ptr->text, output + i, outLen - i);
            i += strlen(output + i);
        }
        ppl_withWordsAppend(c, &ptr->with_data, output, outLen, &i);
    } else if (ptr->type == CANVAS_POLYGON) // Produce textual representations of polygon commands
    {
        int p;
        ppl_snprintfTrunc(output, outLen, "polygon item %d [", ptr->id);
        i = strlen(output);
        // Each point of a polygon
        for (p = 0; p < ptr->NpolygonPoints; p++) {
            if (p > 0) ppl_strAppendChar(output, outLen, &i, ',');
            ppl_strAppendf(output, outLen, &i, "[%s,%s]",
                           ppl_numericDisplay(ptr->polygonPoints[2 * p] * 100, c->numdispBuff[0],
                                              c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                           ppl_numericDisplay(ptr->polygonPoints[2 * p + 1] * 100, c->numdispBuff[1],
                                              c->set->term_current.SignificantFigures,
                                              (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
            i += strlen(output + i);
        }
        ppl_strAppendf(output, outLen, &i, "]");
        i += strlen(output + i);
        ppl_withWordsAppend(c, &ptr->with_data, output, outLen, &i);
    } else if (ptr->type == CANVAS_TEXT) // Produce textual representations of text commands
    {
        ppl_snprintfTrunc(output, outLen, "text item %d ", ptr->id);
        i = strlen(output);
        ppl_strEscapify(ptr->text, output + i, outLen - i);
        i += strlen(output + i);
        ppl_strAppendf(output, outLen, &i, " at %s,%s rotate %s gap %s",
                       ppl_numericDisplay(ptr->xpos * 100, c->numdispBuff[0], c->set->term_current.SignificantFigures,
                                          (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                       ppl_numericDisplay(ptr->ypos * 100, c->numdispBuff[1], c->set->term_current.SignificantFigures,
                                          (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                       ppl_numericDisplay(ptr->rotation * 180 / M_PI, c->numdispBuff[2],
                                          c->set->term_current.SignificantFigures,
                                          (c->set->term_current.NumDisplay == SW_DISPLAY_L)),
                       ppl_numericDisplay(ptr->xpos2 * 100, c->numdispBuff[3], c->set->term_current.SignificantFigures,
                                          (c->set->term_current.NumDisplay == SW_DISPLAY_L)));
        i += strlen(output + i);
        ppl_withWordsAppend(c, &ptr->with_data, output, outLen, &i);
    } else {
        ppl_snprintfTrunc(output, outLen, "[unknown object]");
    } // Ooops.
    return output;
}

//! ppl_directive_list - Implement the list command, which lists the items on the multiplot canvas
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \return Zero

int ppl_directive_list(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive) {
    char msg[LSTR_LENGTH];
    canvas_itemlist *canvas_items = c->canvas_items;
    int i;
    canvas_item *ptr;

    // There is nothing to list if the canvas has never been used
    ppl_report(&c->errcontext, "# ID   Command");
    if (canvas_items == NULL) return 0;
    ptr = canvas_items->first;
    // List each item, marking those which have been deleted
    while (ptr != NULL) {
        ppl_snprintfTrunc(msg, sizeof(msg), "%5d  %s", ptr->id, (ptr->deleted) ? "[deleted] " : "");
        i = strlen(msg);
        ppl_canvas_item_textify(c, ptr, msg + i, sizeof(msg) - i);
        ppl_report(&c->errcontext, msg);
        ptr = ptr->next;
    }
    return 0;
}

//! canvas_delete - Mark an item on the multiplot canvas as deleted, so that it is no longer drawn. It can be restored
//! with the undelete command.
//! \param [in] c - The Pyxplot context
//! \param [in] id - The ID number of the item to delete
//! \return Zero on success, or one if there is no item with that ID number

static int canvas_delete(ppl_context *c, const int id) {
    char msg[PPL_MSG_LENGTH];
    canvas_itemlist *canvas_items = c->canvas_items;
    canvas_item *ptr = canvas_items->first;
    while ((ptr != NULL) && (ptr->id != id)) ptr = ptr->next;
    // Report an item which does not exist
    if (ptr == NULL) {
        ppl_snprintfTrunc(msg, sizeof(msg), "There is no multiplot item with ID %d.", id);
        ppl_warning(&c->errcontext, ERR_GENERIC, msg);
        return 1;
    } else {
        ptr->deleted = 1;
    }
    return 0;
}

//! ppl_directive_delete - Implement the delete command, which removes items from the multiplot canvas
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return Zero on success, or one on failure

int ppl_directive_delete(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                         const int iterDepth) {
    pplObj *stk = in->stk;
    canvas_itemlist *canvas_items = c->canvas_items;
    int pos;

    // Report a canvas which has never been used
    if (canvas_items == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "%s",
                          "There are currently no items on the multiplot canvas.");
        ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "");
        return 1;
    }

    // Delete each item named
    pos = PARSE_delete_deleteno;
    // Walk the linked list of item numbers on the parser stack
    while ((stk[pos].objType == PPLOBJ_NUM) && (stk[pos].real > 0)) {
        int id;
        pos = (int) round(stk[pos].real);
        id = (int) round(stk[pos + PARSE_delete_number_deleteno].real);
        canvas_delete(c, id);
    }

    // Redisplay the canvas as required
    if ((c->set->term_current.display == SW_ONOFF_ON) && (!cancellationFlag)) {
        unsigned char *unsuccessful_ops = (unsigned char *) malloc(MULTIPLOT_MAXINDEX);
        // Failure to allocate memory is reported as an error
        if (unsuccessful_ops == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        }
        ppl_canvas_draw(c, unsuccessful_ops, iterDepth);
        free(unsuccessful_ops);
        unsuccessful_ops = NULL;
    }
    return 0;
}

//! ppl_directive_undelete - Implement the undelete command, which restores items which have been deleted from the
//! multiplot canvas
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return Zero on success, or one on failure

int ppl_directive_undelete(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                           const int iterDepth) {
    char msg[PPL_MSG_LENGTH];
    pplObj *stk = in->stk;
    canvas_itemlist *canvas_items = c->canvas_items;

    // Variables for restoring items
    int pos;

    // Report a canvas which has never been used
    if (canvas_items == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "%s",
                          "There are currently no items on the multiplot canvas.");
        ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "");
        return 1;
    }

    // Restore each item named
    pos = PARSE_undelete_undeleteno;
    // Walk the linked list of item numbers on the parser stack
    while ((stk[pos].objType == PPLOBJ_NUM) && (stk[pos].real > 0)) {
        canvas_item *ptr;
        int id;
        pos = (int) round(stk[pos].real);
        id = (int) round(stk[pos + PARSE_undelete_number_undeleteno].real);
        ptr = canvas_items->first;
        while ((ptr != NULL) && (ptr->id != id)) ptr = ptr->next;
        // Report an item which does not exist
        if (ptr == NULL) {
            ppl_snprintfTrunc(msg, sizeof(msg), "There is no multiplot item with ID %d.", id);
            ppl_warning(&c->errcontext, ERR_GENERIC, msg);
            return 1;
        } else {
            ptr->deleted = 0;
        }
    }

    // Redisplay the canvas as required
    if ((c->set->term_current.display == SW_ONOFF_ON) && (!cancellationFlag)) {
        unsigned char *unsuccessful_ops = (unsigned char *) malloc(MULTIPLOT_MAXINDEX);
        // Failure to allocate memory is reported as an error
        if (unsuccessful_ops == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        }
        ppl_canvas_draw(c, unsuccessful_ops, iterDepth);
        free(unsuccessful_ops);
        unsuccessful_ops = NULL;
    }
    return 0;
}

//! ppl_directive_move - Implement the move command, which moves an item on the multiplot canvas to a new position,
//! optionally rotating it
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return Zero on success, or one on failure

int ppl_directive_move(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                       const int iterDepth) {
    char msg[PPL_MSG_LENGTH];
    pplObj *stk = in->stk;
    canvas_itemlist *canvas_items = c->canvas_items;
    int moveno, gotRotation;
    double x, y, rotation;
    unsigned char rotatable;
    canvas_item *ptr;

    // Report a canvas which has never been used
    if (canvas_items == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "%s",
                          "There are currently no items on the multiplot canvas.");
        ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "");
        return 1;
    }

    // Read the item number and its new position and rotation
    moveno = (int) round(stk[PARSE_move_moveno].real);
    x = stk[PARSE_move_p].real;
    y = stk[PARSE_move_p + 1].real;
    rotation = stk[PARSE_move_rotation].real;
    gotRotation = (stk[PARSE_move_rotation].objType == PPLOBJ_NUM);

    // Find the item
    ptr = canvas_items->first;
    while ((ptr != NULL) && (ptr->id != moveno)) ptr = ptr->next;
    // Report an item which does not exist
    if (ptr == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "There is no multiplot item with ID %d.",
                          moveno);
        ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "");
        return 1;
    }
    rotatable = ((ptr->type != CANVAS_ARROW) && (ptr->type != CANVAS_CIRC) && (ptr->type != CANVAS_PIE) &&
                 (ptr->type != CANVAS_PLOT) && (ptr->type != CANVAS_POINT));
    // Warn that items such as plots cannot be rotated
    if (gotRotation && !rotatable) {
        ppl_snprintfTrunc(msg, sizeof(msg), "It is not possible to rotate multiplot item %d.", moveno);
        ppl_warning(&c->errcontext, ERR_GENERIC, msg);
    }

    // Polygons are moved by moving each of their points; other items by their positions
    if (ptr->type == CANVAS_POLYGON) {
        int c;
        // Move the polygon so that its first point is at the new position, or rotate it about its first point
        if (!gotRotation) {
            const double ox = x - ptr->polygonPoints[0];
            const double oy = y - ptr->polygonPoints[1];
            // Shift each point
            for (c = 0; c < ptr->NpolygonPoints; c++) {
                ptr->polygonPoints[2 * c] += ox;
                ptr->polygonPoints[2 * c + 1] += oy;
            }
        } else {
            // Rotate each point about the first point, and place it relative to the new position. The first point's
            // original position is kept, since the first iteration moves it.
            const double x0 = ptr->polygonPoints[0];
            const double y0 = ptr->polygonPoints[1];
            for (c = 0; c < ptr->NpolygonPoints; c++) {
                // The offset of this point from the first point, before it is moved
                const double ox = ptr->polygonPoints[2 * c] - x0;
                const double oy = ptr->polygonPoints[2 * c + 1] - y0;
                ptr->polygonPoints[2 * c] = ox * cos(rotation) - oy * sin(rotation) + x;
                ptr->polygonPoints[2 * c + 1] = ox * sin(rotation) + oy * cos(rotation) + y;
            }
        }
    } else if ((ptr->type != CANVAS_PLOT) &&
               (ptr->type != CANVAS_PIE)) // Most canvas items are moved using the xpos and ypos fields
    {
        ptr->xpos = x;
        ptr->ypos = y;
        if (gotRotation && rotatable) ptr->rotation = rotation;
    } else // Plots are moved using the origin fields in settings_graph
    {
        ptr->settings.OriginX.real = x;
        ptr->settings.OriginY.real = y;
    }

    // Redisplay the canvas as required
    if ((c->set->term_current.display == SW_ONOFF_ON) && (!cancellationFlag)) {
        unsigned char *unsuccessful_ops = (unsigned char *) malloc(MULTIPLOT_MAXINDEX);
        // Failure to allocate memory is reported as an error
        if (unsuccessful_ops == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        }
        ppl_canvas_draw(c, unsuccessful_ops, iterDepth);
        free(unsuccessful_ops);
        unsuccessful_ops = NULL;
    }
    return 0;
}

//! ppl_directive_swap - Implement the swap command, which swaps the positions of two items in the multiplot canvas's
//! drawing order
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return Zero on success, or one on failure

int ppl_directive_swap(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                       const int iterDepth) {
    pplObj *stk = in->stk;
    canvas_itemlist *canvas_items = c->canvas_items;
    int item1, item2;
    canvas_item **ptr1, **ptr2, *temp;

    // Report a canvas which has never been used
    if (canvas_items == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "%s",
                          "There are currently no items on the multiplot canvas.");
        ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "");
        return 1;
    }

    // Read the ID numbers of the items to be swapped
    item1 = (int) round(stk[PARSE_swap_item1].real);
    item2 = (int) round(stk[PARSE_swap_item2].real);

    // Seek the first item to be swapped
    ptr1 = &canvas_items->first;
    while ((*ptr1 != NULL) && ((*ptr1)->id != item1)) ptr1 = &((*ptr1)->next);
    // Report an item which does not exist
    if (*ptr1 == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "There is no multiplot item with ID %d.",
                          item1);
        ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "");
        return 1;
    }

    // Seek the second item to be swapped
    ptr2 = &canvas_items->first;
    while ((*ptr2 != NULL) && ((*ptr2)->id != item2)) ptr2 = &((*ptr2)->next);
    // Report an item which does not exist
    if (*ptr2 == NULL) {
        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "There is no multiplot item with ID %d.",
                          item2);
        ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "");
        return 1;
    }

    // Do swap
    (*ptr1)->id = item2;
    (*ptr2)->id = item1;
    temp = *ptr1;
    *ptr1 = *ptr2;
    *ptr2 = temp;
    temp = (*ptr1)->next;
    (*ptr1)->next = (*ptr2)->next;
    (*ptr2)->next = temp;

    // Redisplay the canvas as required
    if ((c->set->term_current.display == SW_ONOFF_ON) && (!cancellationFlag)) {
        unsigned char *unsuccessful_ops = (unsigned char *) malloc(MULTIPLOT_MAXINDEX);
        // Failure to allocate memory is reported as an error
        if (unsuccessful_ops == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        }
        ppl_canvas_draw(c, unsuccessful_ops, iterDepth);
        free(unsuccessful_ops);
        unsuccessful_ops = NULL;
    }
    return 0;
}

//! ppl_directive_arrow - Implement the arrow and line commands, which add an arrow or line to the multiplot canvas
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return Zero on success, or one on failure

int ppl_directive_arrow(ppl_context *c, parserLine *pl, parserOutput *in, const int interactive, const int iterDepth) {
    // Variables for creating the item
    pplObj *stk = in->stk;
    canvas_item *ptr;
    int id, gotTempstr;
    double x1, x2, y1, y2;
    char *tempstr, *tempstr2;

    // Look up the start and end point of the arrow, and ensure that they are either dimensionless or in units of length
    x1 = stk[PARSE_arrow_p1].real;
    y1 = stk[PARSE_arrow_p1 + 1].real;
    x2 = stk[PARSE_arrow_p2].real;
    y2 = stk[PARSE_arrow_p2 + 1].real;

    // Add this arrow to the linked list which decribes the canvas
    if (canvas_itemlist_add(c, stk, CANVAS_ARROW, &ptr, &id, 0)) { return 1; }
    ptr->xpos = x1;
    ptr->ypos = y1;
    ptr->xpos2 = x2 - x1;
    ptr->ypos2 = y2 - y1;

    // Read in color and linewidth information, if available
    ppl_withWordsFromDict(c, in, pl, PARSE_TABLE_arrow_, 0, &ptr->with_data);

    // Work out whether this arrow is in the 'head', 'nohead' or 'twoway' style
    tempstr = (char *) stk[PARSE_arrow_arrow_style].auxil;
    gotTempstr = (stk[PARSE_arrow_arrow_style].objType == PPLOBJ_STR);
    tempstr2 = (char *) stk[PARSE_arrow_directive].auxil;
    if (gotTempstr)
        ptr->ArrowType = ppl_fetchSettingByName(&c->errcontext, tempstr, SW_ARROWTYPE_INT, SW_ARROWTYPE_STR);
    else ptr->ArrowType = (strcmp(tempstr2, "arrow") == 0) ? SW_ARROWTYPE_HEAD : SW_ARROWTYPE_NOHEAD;

    // Redisplay the canvas as required
    if ((c->set->term_current.display == SW_ONOFF_ON) && (!cancellationFlag)) {
        unsigned char *unsuccessful_ops = (unsigned char *) malloc(MULTIPLOT_MAXINDEX);
        int failed;
        // Failure to allocate memory is reported as an error
        if (unsuccessful_ops == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        }
        ppl_canvas_draw(c, unsuccessful_ops, iterDepth);
        failed = unsuccessful_ops[id];
        free(unsuccessful_ops);
        unsuccessful_ops = NULL;
        // If the new item could not be drawn, remove it again
        if (failed) {
            canvas_delete(c, id);
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1,
                      "Arrow has been removed from multiplot, because it generated an error.");
            return 1;
        }
    }
    return 0;
}

//! ppl_directive_box - Implement the box command, which adds a rectangle to the multiplot canvas
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return Zero on success, or one on failure

int ppl_directive_box(ppl_context *c, parserLine *pl, parserOutput *in, const int interactive, const int iterDepth) {
    // Variables for creating the item
    pplObj *stk = in->stk;
    canvas_item *ptr;
    int id, gotx2, gotWidth, gotHeight, gotAng;
    double x1, x2, x3, y1, y2, y3, ang, width, height;

    // Look up the positions of the two corners of the box
    x1 = stk[PARSE_box_p1].real;
    y1 = stk[PARSE_box_p1 + 1].real;
    x2 = stk[PARSE_box_p2].real;
    gotx2 = (stk[PARSE_box_p2].objType == PPLOBJ_NUM);
    y2 = stk[PARSE_box_p2 + 1].real;
    x3 = stk[PARSE_box_p3].real;
    y3 = stk[PARSE_box_p3 + 1].real;
    ang = stk[PARSE_box_rotation].real;
    gotAng = (stk[PARSE_box_rotation].objType == PPLOBJ_NUM);
    width = stk[PARSE_box_width].real;
    gotWidth = (stk[PARSE_box_width].objType == PPLOBJ_NUM);
    height = stk[PARSE_box_height].real;
    gotHeight = (stk[PARSE_box_height].objType == PPLOBJ_NUM);

    if ((!gotx2) && ((!gotWidth) || (!gotHeight))) // If box is specified in width/height format, both must be specified
    {
        ppl_snprintfTrunc(
                c->errStat.errBuff, sizeof(c->errStat.errBuff),
                "When a box is specified with given width and height, both width and height must be specified.");
        ppl_tbAddLine(c, pl, ERR_SYNTAX, 0, "");
        return 1;
    }

    // Add this box to the linked list which decribes the canvas
    if (canvas_itemlist_add(c, stk, CANVAS_BOX, &ptr, &id, 0)) { return 1; }
    ptr->xpos = gotx2 ? x1 : x3;
    ptr->ypos = gotx2 ? y1 : y3;
    if (gotx2) // Box is specified by two corners
    {
        ptr->xpos2 = x2 - ptr->xpos;
        ptr->ypos2 = y2 - ptr->ypos;
        ptr->xpos2set = 0; // Rotation should be about CENTRE of box
    } else // Box is specified with width and height
    {
        ptr->xpos2 = width;
        ptr->ypos2 = height;
        ptr->xpos2set = 1; // Rotation should be about fixed corner of box
    }
    if (gotAng) {
        ptr->rotation = ang;
    } // Rotation angle is zero if not specified
    else {
        ptr->rotation = 0.0;
    }

    // Read in color and linewidth information, if available
    ppl_withWordsFromDict(c, in, pl, PARSE_TABLE_box_, 0, &ptr->with_data);

    // Redisplay the canvas as required
    if ((c->set->term_current.display == SW_ONOFF_ON) && (!cancellationFlag)) {
        unsigned char *unsuccessful_ops = (unsigned char *) malloc(MULTIPLOT_MAXINDEX);
        int failed;
        // Failure to allocate memory is reported as an error
        if (unsuccessful_ops == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        }
        ppl_canvas_draw(c, unsuccessful_ops, iterDepth);
        failed = unsuccessful_ops[id];
        free(unsuccessful_ops);
        unsuccessful_ops = NULL;
        // If the new item could not be drawn, remove it again
        if (failed) {
            canvas_delete(c, id);
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1,
                      "Box has been removed from multiplot, because it generated an error.");
            return 1;
        }
    }
    return 0;
}

//! ppl_directive_circle - Implement the circle and arc commands, which add a circle or circular arc to the multiplot
//! canvas
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return Zero on success, or one on failure

int ppl_directive_circle(ppl_context *c, parserLine *pl, parserOutput *in, const int interactive, const int iterDepth) {
    // Variables for creating the item
    pplObj *stk = in->stk;
    canvas_item *ptr;
    int id, gota1, amArc;
    double x, y, r, a1, a2;
    char *tempstr;

    // Look up the position of the centre of the circle and its radius
    tempstr = (char *) stk[PARSE_circle_directive].auxil;
    amArc = (strcmp(tempstr, "arc") == 0);
    x = stk[PARSE_circle_p].real;
    y = stk[PARSE_circle_p + 1].real;
    r = stk[PARSE_circle_r].real;
    a1 = stk[PARSE_arc_angle1].real;
    gota1 = amArc && (stk[PARSE_arc_angle1].objType == PPLOBJ_NUM);
    a2 = stk[PARSE_arc_angle2].real;

    // Add this circle to the linked list which decribes the canvas
    if (canvas_itemlist_add(c, stk, CANVAS_CIRC, &ptr, &id, 0)) { return 1; }
    ptr->xpos = x;
    ptr->ypos = y;
    ptr->xpos2 = r;
    // An arc's start and end angles, if given
    if (gota1) {
        ptr->xfset = 1;
        ptr->xf = a1;
        ptr->yf = a2;
    } // arc command
    else {
        ptr->xfset = 0;
    } // circle command

    // Read in color and linewidth information, if available
    ppl_withWordsFromDict(c, in, pl, amArc ? PARSE_TABLE_arc_ : PARSE_TABLE_circle_, 0, &ptr->with_data);

    // Redisplay the canvas as required
    if ((c->set->term_current.display == SW_ONOFF_ON) && (!cancellationFlag)) {
        unsigned char *unsuccessful_ops = (unsigned char *) malloc(MULTIPLOT_MAXINDEX);
        int failed;
        // Failure to allocate memory is reported as an error
        if (unsuccessful_ops == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        }
        ppl_canvas_draw(c, unsuccessful_ops, iterDepth);
        failed = unsuccessful_ops[id];
        free(unsuccessful_ops);
        unsuccessful_ops = NULL;
        // If the new item could not be drawn, remove it again
        if (failed) {
            canvas_delete(c, id);
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1,
                      "Circle has been removed from multiplot, because it generated an error.");
            return 1;
        }
    }
    return 0;
}

//! ppl_directive_ellipse - Implement the ellipse command, which adds an ellipse to the multiplot canvas. The ellipse
//! may be specified by several different combinations of its centre, foci, axis lengths and eccentricity.
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return Zero on success, or one on failure

int ppl_directive_ellipse(ppl_context *c, parserLine *pl, parserOutput *in, const int interactive,
                          const int iterDepth) {
    pplObj *stk = in->stk;
    canvas_item *ptr;
    int e = 0, r = 0, p = 0, id;
    int gotAng, gotX1, gotXc, gotXf, gotA2, gotB2, gotA, gotB, gotSlr, gotLr, gotEcc, gotarc;
    double ang, x1, x2, y1, y2, xc, yc, xf, yf, a2, b2, a, b, slr, lr, ecc, arcfrom, arcto;
    double ratio;

    // Look up the input parameters which define the ellipse
    ang = stk[PARSE_ellipse_rotation].real;
    gotAng = (stk[PARSE_ellipse_rotation].objType == PPLOBJ_NUM);
    x1 = stk[PARSE_ellipse_p1].real;
    gotX1 = (stk[PARSE_ellipse_p1].objType == PPLOBJ_NUM);
    y1 = stk[PARSE_ellipse_p1 + 1].real;
    x2 = stk[PARSE_ellipse_p2].real;
    y2 = stk[PARSE_ellipse_p2 + 1].real;
    xc = stk[PARSE_ellipse_center].real;
    gotXc = (stk[PARSE_ellipse_center].objType == PPLOBJ_NUM);
    yc = stk[PARSE_ellipse_center + 1].real;
    xf = stk[PARSE_ellipse_focus].real;
    gotXf = (stk[PARSE_ellipse_focus].objType == PPLOBJ_NUM);
    yf = stk[PARSE_ellipse_focus + 1].real;
    a2 = stk[PARSE_ellipse_majoraxis].real;
    gotA2 = (stk[PARSE_ellipse_majoraxis].objType == PPLOBJ_NUM);
    b2 = stk[PARSE_ellipse_minoraxis].real;
    gotB2 = (stk[PARSE_ellipse_minoraxis].objType == PPLOBJ_NUM);
    a = stk[PARSE_ellipse_semimajoraxis].real;
    gotA = (stk[PARSE_ellipse_semimajoraxis].objType == PPLOBJ_NUM);
    b = stk[PARSE_ellipse_semiminoraxis].real;
    gotB = (stk[PARSE_ellipse_semiminoraxis].objType == PPLOBJ_NUM);
    slr = stk[PARSE_ellipse_slr].real;
    gotSlr = (stk[PARSE_ellipse_slr].objType == PPLOBJ_NUM);
    lr = stk[PARSE_ellipse_lr].real;
    gotLr = (stk[PARSE_ellipse_lr].objType == PPLOBJ_NUM);
    ecc = stk[PARSE_ellipse_eccentricity].real;
    gotEcc = (stk[PARSE_ellipse_eccentricity].objType == PPLOBJ_NUM);

    // Read the angles of an arc, if given
    arcfrom = stk[PARSE_ellipse_arcfrom].real;
    gotarc = (stk[PARSE_ellipse_arcfrom].objType == PPLOBJ_NUM);
    arcto = stk[PARSE_ellipse_arcto].real;

    // Count number of input parameters that have been supplied, to make sure we have a suitable set
    if (gotAng) {
        r++;
    } else {
        ang = 0.0;
    }
    if (gotXc) { p++; }
    if (gotXf) { p++; }
    if (gotA2) { e++; }
    if (gotB2) { e++; }
    if (gotA) { e++; }
    if (gotB) { e++; }
    // The eccentricity
    if (gotEcc) {
        e++;
        // The eccentricity must be between zero and one
        if ((ecc < 0.0) || (ecc >= 1.0)) {
            strcpy(c->errStat.errBuff, "Supplied eccentricity is not in the range 0 <= e < 1.");
            ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
            return 1;
        }
    }
    if (gotSlr) { e++; }
    if (gotLr) { e++; }

    // Major axis length is a drop-in replacement for the semi-major axis length
    if (gotA2) {
        a = a2 / 2;
        gotA = 1;
    }
    // A minor axis length is a drop-in replacement for the semi-minor axis length
    if (gotB2) {
        b = b2 / 2;
        gotB = 1;
    }
    // A latus rectum is a drop-in replacement for the semi-latus rectum
    if (gotLr) {
        slr = lr / 2;
        gotSlr = 1;
    }

    // Check that we have been supplied an appropriate set of inputs
    if ((!gotX1) && (((p == 2) && ((e != 1) || (r != 0))) || ((p < 2) && (e != 2)))) {
        strcpy(c->errStat.errBuff,
               "Ellipse command has received an inappropriate set of inputs. Must specify either the position of both the centre and focus of the ellipse, and one further piece of information out of the major axis length, the minor axis length, the eccentricity or the semi-latus rectum, or the position of one of these two points, the rotation angle of the major axis of the ellipse, and two further pieces of information.");
        ppl_tbAddLine(c, pl, ERR_GENERIC, 0, "");
        return 1;
    }

    // Convert inputs such that we have the position of the centre of the ellipse and major/minor axes
    if (gotX1) // User has specified two corners of the ellipse
    {
        xc = (x2 + x1) / 2.0;
        yc = (y2 + y1) / 2.0;
        a = fabs(x2 - x1) / 2.0;
        b = fabs(y2 - y1) / 2.0;
    } else if (p == 2) // User has specified both centre and focus of the ellipse, and one further piece of information
    {
        // Measure the angle of the major axis from the centre to the focus
        if (ppl_dblEqual(xc, xf) && ppl_dblEqual(yc, yf)) {
            ang = 0.0;
        } else {
            ang = atan2(yc - yf, xc - xf);
        }
        xc = xc;
        yc = yc;

        if (gotA) // Additional piece of information was major axis...
        {
            a = fabs(a);
            ecc = hypot(xc - xf, yc - yf) / a;
            // The eccentricity must be between zero and one
            if ((ecc < 0.0) || (ecc >= 1.0)) {
                strcpy(c->errStat.errBuff,
                       "Supplied semi-major axis length is shorter than the distance between the supplied focus and centre of the ellipse. No ellipse may have such parameters.");
                ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
                return 1;
            }
            // A circle has its focus at its centre
            if (ppl_dblEqual(ecc, 0.0)) {
                b = a;
            } else {
                b = a * sqrt(1.0 - pow(ecc, 2));
            }
        } else if (gotB) // minor axis...
        {
            b = fabs(b);
            a = hypot(hypot(xc - xf, yc - yf), b);
            // The semi-minor axis may not exceed the semi-major axis
            if (b > a) {
                strcpy(c->errStat.errBuff,
                       "Supplied minor axis length is longer than the implied major axis length of the ellipse.");
                ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
                return 1;
            }
            ecc = sqrt(1.0 - pow(b / a, 2.0));
        } else if (gotEcc) // eccentricity...
        {
            a = hypot(xc - xf, yc - yf) / ecc;
            // A circle has its focus at its centre
            if (ppl_dblEqual(ecc, 0.0)) {
                b = a;
            } else {
                b = a * sqrt(1.0 - pow(ecc, 2));
            }
        } else if (gotSlr) // or semi-latus rectum...
        {
            ratio = hypot(xc - xf, yc - yf) / slr;
            ecc = (sqrt(1 + 4 * pow(ratio, 2)) - 1.0) / (2 * ratio);
            // The eccentricity must be between zero and one
            if ((ecc < 0.0) || (ecc >= 1.0)) {
                strcpy(c->errStat.errBuff, "Eccentricity implied for ellipse is not in the range 0 <= e < 1.");
                ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
                return 1;
            }
            a = hypot(xc - xf, yc - yf) / ecc;
            b = a * sqrt(1.0 - pow(ecc, 2.0));
        } else {
            strcpy(c->errStat.errBuff, "Flow control error in ellipse command.");
            ppl_tbAddLine(c, pl, ERR_INTERNAL, 0, "");
            return 1;
        }
    } else // User has specified centre / focus of ellipse and two further pieces of information...
    {
        if (gotA && gotB) // major and minor axes...
        {
            a = fabs(a);
            b = fabs(b);
            // The semi-minor axis may not exceed the semi-major axis
            if (b > a) {
                strcpy(c->errStat.errBuff,
                       "Supplied minor axis length is longer than the supplied major axis length of the ellipse.");
                ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
                ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
                return 1;
            }
            ecc = sqrt(1.0 - pow(b / a, 2.0));
        } else if (gotA && gotEcc) // major axis and eccentricity...
        {
            a = fabs(a);
            b = a * sqrt(1.0 - pow(ecc, 2.0));
        } else if (gotA && gotSlr) // major axis and SLR...
        {
            a = fabs(a);
            // The semi-latus rectum may not exceed the semi-major axis
            if (fabs(slr) > a) {
                strcpy(c->errStat.errBuff,
                       "Supplied semi-latus rectum is longer than the supplied semi-major axis length of the ellipse. No ellipse may have such parameters.");
                ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
                return 1;
            }
            ecc = sqrt(1.0 - fabs(slr) / a);
            b = a * sqrt(1.0 - pow(ecc, 2.0));
        } else if (gotB && gotEcc) // minor axis and eccentricity...
        {
            b = fabs(b);
            a = b / sqrt(1.0 - pow(ecc, 2.0));
        } else if (gotB && gotSlr) // minor axis and SLR...
        {
            b = fabs(b);
            // The semi-latus rectum may not exceed the semi-minor axis
            if (fabs(slr) > b) {
                strcpy(c->errStat.errBuff,
                       "Supplied semi-latus rectum is longer than the supplied semi-minor axis length of the ellipse. No ellipse may have such parameters.");
                ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
                return 1;
            }
            ecc = sqrt(1.0 - pow(fabs(slr) / b, 2.0));
            a = b / sqrt(1.0 - pow(ecc, 2.0));
        } else if (gotEcc && gotSlr) // eccentricity and SLR...
        {
            a = fabs(slr) / (1.0 - pow(ecc, 2.0));
            b = a * sqrt(1.0 - pow(ecc, 2.0));
        } else {
            strcpy(c->errStat.errBuff, "Flow control error in ellipse command.");
            ppl_tbAddLine(c, pl, ERR_INTERNAL, 0, "");
            return 1;
        }

        if (gotXc) // User has specified the centre of the ellipse
        {
            xc = xc;
            yc = yc;
        } else if (!gotXf) // User has specified neither the centre nor a focus of the ellipse; use origin as centre
        {
            xc = c->set->graph_current.OriginX.real;
            yc = c->set->graph_current.OriginY.real;
        } else // User has specified the focus of the ellipse... convert to the centre by translating distance a * eccentricity
        {
            xc = xf + a * ecc * cos(ang);
            yc = yf + a * ecc * sin(ang);
        }
    }

    // Add this ellipse to the linked list which decribes the canvas
    if (canvas_itemlist_add(c, stk, CANVAS_ELLPS, &ptr, &id, 0)) { return 1; }
    ptr->xpos = xc;
    ptr->ypos = yc;
    ptr->xpos2 = a;
    ptr->ypos2 = b;
    ptr->rotation = ang;

    // Add the exact parameterisation which we have been given to canvas item, so that "list" command prints it out in the form originally supplied
    ptr->x1set = ptr->xcset = ptr->xfset = ptr->aset = ptr->bset = ptr->eccset = ptr->slrset = 0;
    ptr->x1 = ptr->y1 = ptr->x2 = ptr->y2 = ptr->xc = ptr->yc = ptr->xf = ptr->yf = ptr->ecc = ptr->slr = 0.0;
    ptr->arcset = 0;
    // Record which parameters were given, so that the item can be listed as it was typed
    if (gotX1) {
        ptr->x1set = 1;
        ptr->x1 = x1;
        ptr->y1 = y1;
        ptr->x2 = x2;
        ptr->y2 = y2;
    } else if (gotXc || !gotXf) {
        ptr->xcset = 1;
        ptr->xc = xc;
        ptr->yc = yc;
    }
    // The focus
    if (gotXf) {
        ptr->xfset = 1;
        ptr->xf = xf;
        ptr->yf = yf;
    }
    // The semi-major axis
    if (gotA) {
        ptr->aset = 1;
        ptr->a = a;
    }
    // The semi-minor axis
    if (gotB) {
        ptr->bset = 1;
        ptr->b = b;
    }
    // The eccentricity
    if (gotEcc) {
        ptr->eccset = 1;
        ptr->ecc = ecc;
    }
    // The semi-latus rectum
    if (gotSlr) {
        ptr->slrset = 1;
        ptr->slr = slr;
    }
    // The angles of the arc
    if (gotarc) {
        ptr->arcset = 1;
        ptr->arcfrom = arcfrom;
        ptr->arcto = arcto;
    }

    // Read in color and linewidth information, if available
    ppl_withWordsFromDict(c, in, pl, PARSE_TABLE_ellipse_, 0, &ptr->with_data);

    // Redisplay the canvas as required
    if ((c->set->term_current.display == SW_ONOFF_ON) && (!cancellationFlag)) {
        unsigned char *unsuccessful_ops = (unsigned char *) malloc(MULTIPLOT_MAXINDEX);
        int failed;
        // Failure to allocate memory is reported as an error
        if (unsuccessful_ops == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        }
        ppl_canvas_draw(c, unsuccessful_ops, iterDepth);
        failed = unsuccessful_ops[id];
        free(unsuccessful_ops);
        unsuccessful_ops = NULL;
        // If the new item could not be drawn, remove it again
        if (failed) {
            canvas_delete(c, id);
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1,
                      "Ellipse has been removed from multiplot, because it generated an error.");
            return 1;
        }
    }
    return 0;
}

//! ppl_directive_eps - Implement the eps command, which adds an encapsulated PostScript image to the multiplot canvas
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return Zero on success, or one on failure

int ppl_directive_eps(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                      const int iterDepth) {
    pplObj *stk = in->stk;
    canvas_item *ptr;
    int id;
    double x, y, ang, width, height;
    int gotX, gotY, gotAng, gotWidth, gotHeight, clip, calcbbox;
    char *text, *fname;

    // Read in positional information for this eps image
    x = stk[PARSE_eps_p].real;
    gotX = (stk[PARSE_eps_p].objType == PPLOBJ_NUM);
    y = stk[PARSE_eps_p + 1].real;
    gotY = (stk[PARSE_eps_p + 1].objType == PPLOBJ_NUM);
    ang = stk[PARSE_eps_rotation].real;
    gotAng = (stk[PARSE_eps_rotation].objType == PPLOBJ_NUM);
    width = stk[PARSE_eps_width].real;
    gotWidth = (stk[PARSE_eps_width].objType == PPLOBJ_NUM);
    height = stk[PARSE_eps_height].real;
    gotHeight = (stk[PARSE_eps_height].objType == PPLOBJ_NUM);
    clip = (stk[PARSE_eps_clip].objType != PPLOBJ_ZOM);
    calcbbox = (stk[PARSE_eps_calcbbox].objType != PPLOBJ_ZOM);
    fname = (char *) stk[PARSE_eps_filename].auxil;
    text = (char *) malloc(strlen(fname) + 1);
    // Failure to allocate memory is reported as an error
    if (text == NULL) {
        ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (F).");
        return 1;
    }
    strcpy(text, fname);

    // Add this eps image to the linked list which decribes the canvas
    if (canvas_itemlist_add(c, stk, CANVAS_EPS, &ptr, &id, 0)) {
        free(text);
        text = NULL;
        return 1;
    }

    // Store each property which was given
    if (gotX) {
        ptr->xpos = x;
    } else {
        ptr->xpos = c->set->graph_current.OriginX.real;
    }
    // The y position
    if (gotY) {
        ptr->ypos = y;
    } else {
        ptr->ypos = c->set->graph_current.OriginY.real;
    }
    // The rotation
    if (gotAng) {
        ptr->rotation = ang;
    } else {
        ptr->rotation = 0.0;
    }
    // The width
    if (gotWidth) {
        ptr->xpos2 = width;
        ptr->xpos2set = 1;
    } else {
        ptr->xpos2 = 0.0;
        ptr->xpos2set = 0;
    }
    // The height
    if (gotHeight) {
        ptr->ypos2 = height;
        ptr->ypos2set = 1;
    } else {
        ptr->ypos2 = 0.0;
        ptr->ypos2set = 0;
    }
    ptr->text = text;
    ptr->clip = clip;
    ptr->calcbbox = calcbbox;

    // Redisplay the canvas as required
    if ((c->set->term_current.display == SW_ONOFF_ON) && (!cancellationFlag)) {
        unsigned char *unsuccessful_ops = (unsigned char *) malloc(MULTIPLOT_MAXINDEX);
        int failed;
        // Failure to allocate memory is reported as an error
        if (unsuccessful_ops == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        }
        ppl_canvas_draw(c, unsuccessful_ops, iterDepth);
        failed = unsuccessful_ops[id];
        free(unsuccessful_ops);
        unsuccessful_ops = NULL;
        // If the new item could not be drawn, remove it again
        if (failed) {
            canvas_delete(c, id);
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1,
                      "EPS image has been removed from multiplot, because it generated an error.");
            return 1;
        }
    }
    return 0;
}

//! ppl_directive_point - Implement the point command, which adds a point, optionally labelled, to the multiplot
//! canvas
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return Zero on success, or one on failure

int ppl_directive_point(ppl_context *c, parserLine *pl, parserOutput *in, const int interactive, const int iterDepth) {
    // Variables for creating the item
    pplObj *stk = in->stk;
    canvas_item *ptr;
    int id;
    double x, y;

    // Look up the position of the point, and ensure that it is either dimensionless or in units of length
    x = stk[PARSE_point_p].real;
    y = stk[PARSE_point_p + 1].real;

    // Add this point to the linked list which decribes the canvas
    if (canvas_itemlist_add(c, stk, CANVAS_POINT, &ptr, &id, 0)) { return 1; }
    ptr->xpos = x;
    ptr->ypos = y;

    // Read in color and linewidth information, if available
    ppl_withWordsFromDict(c, in, pl, PARSE_TABLE_point_, 0, &ptr->with_data);

    // See whether this point is labelled
    if (stk[PARSE_point_label].objType == PPLOBJ_STR) {
        char *tempstr = (char *) stk[PARSE_point_label].auxil;
        char *text = (char *) malloc(strlen(tempstr) + 1);
        // Failure to allocate memory is reported as an error
        if (text == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (I).");
            return 1;
        }
        strcpy(text, tempstr);
        ptr->text = text;
    } else {
        ptr->text = NULL;
    }

    // Redisplay the canvas as required
    if ((c->set->term_current.display == SW_ONOFF_ON) && (!cancellationFlag)) {
        unsigned char *unsuccessful_ops = (unsigned char *) malloc(MULTIPLOT_MAXINDEX);
        int failed;
        // Failure to allocate memory is reported as an error
        if (unsuccessful_ops == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        }
        ppl_canvas_draw(c, unsuccessful_ops, iterDepth);
        failed = unsuccessful_ops[id];
        free(unsuccessful_ops);
        unsuccessful_ops = NULL;
        // If the new item could not be drawn, remove it again
        if (failed) {
            canvas_delete(c, id);
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1,
                      "Point has been removed from multiplot, because it generated an error.");
            return 1;
        }
    }
    return 0;
}

//! ppl_directive_polygon - Implement the polygon command, which adds a polygon to the multiplot canvas
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return Zero on success, or one on failure

int ppl_directive_polygon(ppl_context *c, parserLine *pl, parserOutput *in, const int interactive,
                          const int iterDepth) {
    char msg[PPL_MSG_LENGTH];
    pplObj *stk = in->stk;
    canvas_item *ptr;
    int id, Npts, i;
    double *ptList;
    list *ptListObj;

    // Count how many points have been supplied
    if (stk[PARSE_polygon_pointlist].objType != PPLOBJ_LIST) {
        ppl_snprintfTrunc(msg, sizeof(msg),
                          "List of points in polygon should have been a list; supplied object was of type <%s>.",
                          pplObjTypeNames[stk[PARSE_polygon_pointlist].objType]);
        ppl_error(&c->errcontext, ERR_TYPE, -1, -1, msg);
        return 1;
    }
    ptListObj = (list *) stk[PARSE_polygon_pointlist].auxil;
    Npts = ppl_listLen(ptListObj);
    ptList = (double *) malloc(2 * Npts * sizeof(double));
    // Failure to allocate memory is reported as an error
    if (ptList == NULL) {
        ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (J).");
        return 1;
    }
    // A polygon needs at least two points
    if (Npts < 2) {
        ppl_snprintfTrunc(msg, sizeof(msg), "A minimum of two points are required to specify the outline of a polygon.");
        goto fail;
    }
    // Read each point, which may be a vector or a list of two numbers, with units of length or dimensionless (in
    // centimetres)
    for (i = 0; i < Npts; i++) {
        double x, y;
        pplObj *item = (pplObj *) ppl_listGetItem(ptListObj, i);
        // A point given as a vector
        if (item->objType == PPLOBJ_VEC) {
            pplVector *v = (pplVector *) item->auxil;
            int l = v->v->size;
            double m = 1;
            if (item->dimensionless) m = 0.01;
            // Dimensionless positions are in centimetres; otherwise they must be lengths
            else {
                int p;
                for (p = 0; p < UNITS_MAX_BASEUNITS; p++)
                    // Check the units are those of length
                    if (item->exponent[p] != (p == UNIT_LENGTH)) {
                        p = -1;
                        break;
                    }
                // Report a position with the wrong units
                if (p < 0) {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                           "Point %d of polygon should have specified as a position vector; supplied position had wrong units.",
                           i + 1);
                    goto fail;
                }
            }
            // The vector must have two components
            if (l != 2) {
                ppl_snprintfTrunc(msg, sizeof(msg),
                       "Point %d of polygon should have specified as a two-component position vector; supplied vector had %d components.",
                       i + 1, l);
                goto fail;
            }
            x = gsl_vector_get(v->v, 0) * m;
            y = gsl_vector_get(v->v, 1) * m;
        } else if (item->objType == PPLOBJ_LIST) {
            int j, p;
            double m[2] = {1, 1}; // The scaling of each coordinate, which may differ if only one has units
            list *l = (list *) item->auxil;
            pplObj *xo[2];
            // A point given as a list must have two items
            if (ppl_listLen(l) != 2) {
                ppl_snprintfTrunc(msg, sizeof(msg),
                       "Point %d of polygon should have specified as a two-component position vector; supplied vector had %d components.",
                       i + 1, ppl_listLen(l));
                goto fail;
            }
            // Each coordinate must be a real number
            for (j = 0; j < 2; j++) {
                xo[j] = (pplObj *) ppl_listGetItem(l, j);
                // The coordinate must be a number
                if (xo[j]->objType != PPLOBJ_NUM) {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                           "Point %d of polygon should have specified as a position vector; supplied object had type <%s>.",
                           i + 1, pplObjTypeNames[xo[j]->objType]);
                    goto fail;
                }
                // The coordinate must be real
                if (xo[j]->flagComplex) {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                           "Point %d of polygon should have specified as a position vector; supplied object was complex.",
                           i + 1);
                    goto fail;
                }
                if (xo[j]->dimensionless) m[j] = 0.01;
                // Dimensionless coordinates are in centimetres; otherwise they must be lengths
                else {
                    for (p = 0; p < UNITS_MAX_BASEUNITS; p++)
                        // The exponent of every base unit must be zero, except length, which must be one
                        if (xo[j]->exponent[p] != (p == UNIT_LENGTH)) {
                            p = -1;
                            break;
                        }
                    // Report a coordinate with the wrong units
                    if (p < 0) {
                        ppl_snprintfTrunc(msg, sizeof(msg),
                               "Point %d of polygon should have specified as a position vector; supplied position had wrong units.",
                               i + 1);
                        goto fail;
                    }
                }
            }
            x = xo[0]->real * m[0];
            y = xo[1]->real * m[1];
        } else {
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "Point %d of polygon should have specified as a vector or a list; supplied object was of type <%s>.",
                    i + 1, pplObjTypeNames[item->objType]);
        fail:
            ppl_error(&c->errcontext, ERR_TYPE, -1, -1, msg);
            free(ptList);
            ptList = NULL;
            return 1;
        }
        ptList[2 * i] = x;
        ptList[2 * i + 1] = y;
    }

    // Add this polygon to the linked list which decribes the canvas
    if (canvas_itemlist_add(c, stk, CANVAS_POLYGON, &ptr, &id, 0)) { return 1; }
    ptr->NpolygonPoints = Npts;
    ptr->polygonPoints = ptList;

    // Read in color and linewidth information, if available
    ppl_withWordsFromDict(c, in, pl, PARSE_TABLE_polygon_, 0, &ptr->with_data);

    // Redisplay the canvas as required
    if ((c->set->term_current.display == SW_ONOFF_ON) && (!cancellationFlag)) {
        unsigned char *unsuccessful_ops = (unsigned char *) malloc(MULTIPLOT_MAXINDEX);
        int failed;
        // Failure to allocate memory is reported as an error
        if (unsuccessful_ops == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        }
        ppl_canvas_draw(c, unsuccessful_ops, iterDepth);
        failed = unsuccessful_ops[id];
        free(unsuccessful_ops);
        unsuccessful_ops = NULL;
        // If the new item could not be drawn, remove it again
        if (failed) {
            canvas_delete(c, id);
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1,
                      "Polygon has been removed from multiplot, because it generated an error.");
            return 1;
        }
    }
    return 0;
}

//! ppl_directive_text - Implement the text command, which adds a text label to the multiplot canvas
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return Zero on success, or one on failure

int ppl_directive_text(ppl_context *c, parserLine *pl, parserOutput *in, const int interactive, const int iterDepth) {
    // Variables for creating the item
    pplObj *stk = in->stk;
    canvas_item *ptr;
    int id, gotX, gotY, gotAng, gotGap, gotTempstr;
    double x, y, ang, gap;
    char *tempstr, *text;

    // Look up the position of the text item
    x = stk[PARSE_text_p].real;
    gotX = (stk[PARSE_text_p].objType == PPLOBJ_NUM);
    y = stk[PARSE_text_p + 1].real;
    gotY = (stk[PARSE_text_p + 1].objType == PPLOBJ_NUM);
    gap = stk[PARSE_text_gap].real;
    gotGap = (stk[PARSE_text_gap].objType == PPLOBJ_NUM);
    ang = stk[PARSE_text_rotation].real;
    gotAng = (stk[PARSE_text_rotation].objType == PPLOBJ_NUM);

    // Read the string which we are to render
    tempstr = (char *) stk[PARSE_text_string].auxil;
    text = (char *) malloc(strlen(tempstr) + 1);
    // Failure to allocate memory is reported as an error
    if (text == NULL) {
        ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (L).");
        return 1;
    }
    strcpy(text, tempstr);

    // Add the item to the canvas
    if (canvas_itemlist_add(c, stk, CANVAS_TEXT, &ptr, &id, 0)) {
        free(text);
        text = NULL;
        return 1;
    }

    // Check for halign or valign modifiers
    tempstr = (char *) stk[PARSE_text_halign].auxil;
    gotTempstr = (stk[PARSE_text_halign].objType == PPLOBJ_STR);
    if (gotTempstr)
        ptr->settings.TextHAlign = ppl_fetchSettingByName(&c->errcontext, tempstr, SW_HALIGN_INT, SW_HALIGN_STR);
    tempstr = (char *) stk[PARSE_text_valign].auxil;
    gotTempstr = (stk[PARSE_text_valign].objType == PPLOBJ_STR);
    if (gotTempstr)
        ptr->settings.TextVAlign = ppl_fetchSettingByName(&c->errcontext, tempstr, SW_VALIGN_INT, SW_VALIGN_STR);

    // Store each property which was given
    if (gotX) {
        ptr->xpos = x;
    } else {
        ptr->xpos = c->set->graph_current.OriginX.real;
    }
    // The y position
    if (gotY) {
        ptr->ypos = y;
    } else {
        ptr->ypos = c->set->graph_current.OriginY.real;
    }
    // The gap between the text and its position
    if (gotGap) {
        ptr->xpos2 = gap;
    } else {
        ptr->xpos2 = 0.0;
    }
    // The rotation
    if (gotAng) {
        ptr->rotation = ang;
    } else {
        ptr->rotation = 0.0;
    }
    ptr->text = text;

    // Read in color information, if available
    ppl_withWordsFromDict(c, in, pl, PARSE_TABLE_text_, 0, &ptr->with_data);

    // Redisplay the canvas as required
    if ((c->set->term_current.display == SW_ONOFF_ON) && (!cancellationFlag)) {
        unsigned char *unsuccessful_ops = (unsigned char *) malloc(MULTIPLOT_MAXINDEX);
        int failed;
        // Failure to allocate memory is reported as an error
        if (unsuccessful_ops == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        }
        ppl_canvas_draw(c, unsuccessful_ops, iterDepth);
        failed = unsuccessful_ops[id];
        free(unsuccessful_ops);
        unsuccessful_ops = NULL;
        // If the new item could not be drawn, remove it again
        if (failed) {
            canvas_delete(c, id);
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1,
                      "Text item has been removed from multiplot, because it generated an error.");
            return 1;
        }
    }
    return 0;
}

//! ppl_directive_image - Implement the image command, which adds a bitmap image to the multiplot canvas
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return Zero on success, or one on failure

int ppl_directive_image(ppl_context *c, const parserLine *pl, const parserOutput *in, const int interactive,
                        const int iterDepth) {
    pplObj *stk = in->stk;
    canvas_item *ptr;
    int id, transR = 0, transG = 0, transB = 0, smooth, noTrans, cTrans;
    double x, y, ang, width, height;
    int gotX, gotY, gotAng, gotWidth, gotHeight;
    char *text, *fname;

    // Read the parameters of the image
    x = stk[PARSE_image_p].real;
    gotX = (stk[PARSE_image_p].objType == PPLOBJ_NUM);
    y = stk[PARSE_image_p + 1].real;
    gotY = (stk[PARSE_image_p + 1].objType == PPLOBJ_NUM);
    ang = stk[PARSE_image_rotation].real;
    gotAng = (stk[PARSE_image_rotation].objType == PPLOBJ_NUM);
    width = stk[PARSE_image_width].real;
    gotWidth = (stk[PARSE_image_width].objType == PPLOBJ_NUM);
    height = stk[PARSE_image_height].real;
    gotHeight = (stk[PARSE_image_height].objType == PPLOBJ_NUM);
    smooth = (stk[PARSE_image_smooth].objType == PPLOBJ_STR);
    noTrans = (stk[PARSE_image_notrans].objType == PPLOBJ_STR);
    cTrans = (stk[PARSE_image_colorR].objType == PPLOBJ_NUM);
    // A transparent color, given by red, green and blue components between 0 and 1
    if (cTrans) {
        transR = (int) floor(stk[PARSE_image_colorR].real * 255);
        transR = ppl_max(ppl_min(transR, 255), 0);
        transG = (int) floor(stk[PARSE_image_colorG].real * 255);
        transG = ppl_max(ppl_min(transG, 255), 0);
        transB = (int) floor(stk[PARSE_image_colorB].real * 255);
        transB = ppl_max(ppl_min(transB, 255), 0);
    }
    fname = (char *) stk[PARSE_eps_filename].auxil;
    text = (char *) malloc(strlen(fname) + 1);
    // Failure to allocate memory is reported as an error
    if (text == NULL) {
        ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (N).");
        return 1;
    }
    strcpy(text, fname);

    // Add this eps image to the linked list which decribes the canvas
    if (canvas_itemlist_add(c, stk, CANVAS_IMAGE, &ptr, &id, 0)) {
        free(text);
        text = NULL;
        return 1;
    }

    // Store each property which was given
    if (gotX) {
        ptr->xpos = x;
    } else {
        ptr->xpos = c->set->graph_current.OriginX.real;
    }
    // The y position
    if (gotY) {
        ptr->ypos = y;
    } else {
        ptr->ypos = c->set->graph_current.OriginY.real;
    }
    // The rotation
    if (gotAng) {
        ptr->rotation = ang;
    } else {
        ptr->rotation = 0.0;
    }
    // The width
    if (gotWidth) {
        ptr->xpos2 = width;
        ptr->xpos2set = 1;
    } else {
        ptr->xpos2 = 0.0;
        ptr->xpos2set = 0;
    }
    // The height
    if (gotHeight) {
        ptr->ypos2 = height;
        ptr->ypos2set = 1;
    } else {
        ptr->ypos2 = 0.0;
        ptr->ypos2set = 0;
    }
    ptr->text = text;
    // Smooth the image when it is scaled
    if (smooth) {
        ptr->smooth = 1;
    } else {
        ptr->smooth = 0;
    }
    // Ignore any transparency in the image
    if (noTrans) {
        ptr->NoTransparency = 1;
    } else {
        ptr->NoTransparency = 0;
    }
    // Make a color transparent
    if (cTrans) {
        ptr->CustomTransparency = 1;
        ptr->TransColR = transR;
        ptr->TransColG = transG;
        ptr->TransColB = transB;
    } else {
        ptr->CustomTransparency = 0;
    }

    // Redisplay the canvas as required
    if ((c->set->term_current.display == SW_ONOFF_ON) && (!cancellationFlag)) {
        unsigned char *unsuccessful_ops = (unsigned char *) malloc(MULTIPLOT_MAXINDEX);
        int failed;
        // Failure to allocate memory is reported as an error
        if (unsuccessful_ops == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        }
        ppl_canvas_draw(c, unsuccessful_ops, iterDepth);
        failed = unsuccessful_ops[id];
        free(unsuccessful_ops);
        unsuccessful_ops = NULL;
        // If the new item could not be drawn, remove it again
        if (failed) {
            canvas_delete(c, id);
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1,
                      "Bitmap image has been removed from multiplot, because it generated an error.");
            return 1;
        }
    }
    return 0;
}


//! ppl_getPlotFname - Look up the Nth filename matching a wildcard given in a plot command, and store it in a plot
//! item descriptor
//! \param [in] c - The Pyxplot context
//! \param [in] in - The filename, which may contain wildcards. The special filenames "", "-" and "--" match only
//! once.
//! \param [in] wildcardMatchNumber - The number of the match to return, counting from zero
//! \param [out] out - The plot item descriptor in which to store the filename
//! \return Zero on success, or one if there are fewer than <wildcardMatchNumber>+1 matches or an error occurred

static int ppl_getPlotFname(ppl_context *c, const char *in, int wildcardMatchNumber, canvas_plotdesc *out) {
    // Variables for expanding the wildcard
    char msg[PPL_MSG_LENGTH];
    int i, C;
    char escaped[FNAME_LENGTH];
    wordexp_t wordExp;
    glob_t globData;
    out->function = 0;
    out->NFunctions = -1;
    out->functions = NULL;
    out->vectors = NULL;
    if (wildcardMatchNumber < 0) wildcardMatchNumber = 0;
    C = wildcardMatchNumber;
    if ((strcmp(in, "") == 0) || (strcmp(in, "-") == 0) || (strcmp(in, "--") == 0)) // special filenames match once only
    {
        if (wildcardMatchNumber > 0) return 1;
        out->filename = (char *) malloc(4);
        // Failure to allocate memory is reported as an error
        if (out->filename == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (P).");
            return 1;
        }
        strcpy(out->filename, in);
        return 0;
    }
    {
        int j, k;
        // Escape spaces in the filename before passing it to wordexp
        for (j = k = 0; ((in[j] != '\0') && (k < FNAME_LENGTH - 1));) {
            if (in[j] == ' ') escaped[k++] = '\\';
            escaped[k++] = in[j++];
        }
        escaped[k++] = '\0';
    }
    // Expand ~ and environment variables in the filename
    if ((wordexp(escaped, &wordExp, 0) != 0) || (wordExp.we_wordc <= 0)) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Could not open file '%s'.", escaped);
        ppl_error(&c->errcontext, ERR_FILE, -1, -1, msg);
        return 1;
    }
    // Find the requested match of the wildcards among the files matching each word of the expanded filename
    for (i = 0; i < wordExp.we_wordc; i++) {
        // A word which matches no files
        if ((glob(wordExp.we_wordv[i], 0, NULL, &globData) != 0) || ((i == 0) && (globData.gl_pathc == 0))) {
            // Report it on the first pass only
            if (wildcardMatchNumber == 0) {
                ppl_snprintfTrunc(msg, sizeof(msg), "Could not open file '%s'.", escaped);
                ppl_error(&c->errcontext, ERR_FILE, -1, -1, msg);
            }
            return 1;
        }
        // Count past the matches of this word
        if (C >= globData.gl_pathc) {
            C -= globData.gl_pathc;
            globfree(&globData);
            continue;
        }
        out->filename = (char *) malloc(strlen(globData.gl_pathv[C]) + 1);
        // Failure to allocate memory is reported as an error
        if (out->filename == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (Q).");
            return 1;
        }
        strcpy(out->filename, globData.gl_pathv[C]);
        globfree(&globData);
        wordfree(&wordExp);
        return 0;
    }
    wordfree(&wordExp);
    // Report that no file matched, on the first pass only
    if (wildcardMatchNumber == 0) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Could not open file '%s'.", escaped);
        ppl_error(&c->errcontext, ERR_FILE, -1, -1, msg);
    }
    return 1;
}

//! ppl_getPlotData - Read the description of one item in a plot or piechart command (a data file or list of
//! functions, with modifiers such as "using", "every" and "with"), and append a plot item descriptor to a canvas item
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command
//! \param [in|out] ci - The canvas item to which to append the plot item
//! \param [in] ptab - Table mapping parser variable indices onto positions in <in->stk>
//! \param [in] stkbase - Offset to add to positions in <in->stk> to find this plot item's parameters
//! \param [in] wildcardMatchNumber - For data files given as wildcards, the number of the matching file to use
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \param [in] dataSpool - Data supplied inline after the command, if any
//! \param [in] NExpectIn - The number of data columns expected, or zero or less to determine this from the plot
//! style
//! \return Zero on success, or one on failure

static int ppl_getPlotData(ppl_context *c, parserLine *pl, parserOutput *in, canvas_item *ci, const int *ptab,
                           int stkbase, const int wildcardMatchNumber, const int iterDepth, parserLine **dataSpool,
                           const int NExpectIn) {
    char msg[PPL_MSG_LENGTH];
    pplObj *stk = in->stk;
    canvas_plotdesc **plotItemPtr = &ci->plotitems;
    canvas_plotdesc *new = NULL;
    while (*plotItemPtr != NULL) plotItemPtr = &(*plotItemPtr)->next; // Find end of list of plot items

    // Malloc a structure to hold this plot item
    new = (canvas_plotdesc *) malloc(sizeof(canvas_plotdesc));
    // Failure to allocate memory is reported as an error
    if (new == NULL) {
        ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (R).");
        return 1;
    }
    memset((void *) new, 0, sizeof(canvas_plotdesc));
    new->filename = NULL;
    new->PersistentDataTable = NULL;
    ppl_withWordsZero(c, &new->ww);
    ppl_withWordsZero(c, &new->ww_final);

    // Test for expression list or filename
    {
        const int pos1 = ptab[PARSE_INDEX_expression_list] + stkbase;
        const int pos2 = ptab[PARSE_INDEX_filename] + stkbase;
        if ((pos1 > 0) && (stk[pos1].objType == PPLOBJ_NUM)) // we have been passed a list of expressions
        {
            const int stkLevelOld = c->stackPtr;
            int pos = pos1;
            int Nexprs = 0, i;
            pplExpr **exprList;
            pplObj *first;
            while (stk[pos].objType == PPLOBJ_NUM) // count number of expressions
            {
                pos = (int) round(stk[pos].real);
                if (pos <= 0) break;
                Nexprs++;
            }
            // There must be at least one expression
            if (Nexprs < 1) {
                ppl_error(&c->errcontext, ERR_SYNTAX, -1, -1, "Fewer than one expression was supplied to evaluate.");
                free(new);
                new = NULL;
                return 1;
            }
            exprList = (pplExpr **) ppl_memAlloc(Nexprs * sizeof(pplExpr *));
            // Failure to allocate memory is reported as an error
            if (exprList == NULL) {
                ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (S).");
                free(new);
                new = NULL;
                return 1;
            }
            // Collect the expressions from the linked list on the parser stack
            for (i = 0, pos = pos1; stk[pos].objType == PPLOBJ_NUM; i++) {
                pos = (int) round(stk[pos].real);
                if (pos <= 0) break;
                exprList[i] = (pplExpr *) stk[pos + ptab[PARSE_INDEX_expression]].auxil;
            }

            // A single expression which evaluates to a string is a filename; expressions which evaluate to vectors
            // supply vectors of data; others are functions to sample
            first = ppl_expEval(c, exprList[0], &i, 0, iterDepth);
            if ((!c->errStat.status) && (first->objType == PPLOBJ_STR) &&
                (Nexprs == 1)) // If we have a single expression that evaluates to a string, it's a filename
            {
                char *datafile = (char *) first->auxil;
                const int status = ppl_getPlotFname(c, datafile, wildcardMatchNumber, new);
                ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
                if (status) return 1;
            } else if ((!c->errStat.status) && (first->objType == PPLOBJ_VEC)) {
                const int stkLevelOld = c->stackPtr;
                pplObj *vecs;
                gsl_vector *v = ((pplVector *) first->auxil)->v;
                const int l = v->size;
                int j;
                vecs = (pplObj *) ppl_memAlloc(Nexprs * sizeof(pplObj));
                // Failure to allocate memory is reported as an error
                if (vecs == NULL) {
                    ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (T).");
                    free(new);
                    new = NULL;
                    return 1;
                }
                // Evaluate each expression, which must be a vector of the same length as the first
                for (i = 0; i < Nexprs; i++) {
                    int l2;
                    pplObj *obj = ppl_expEval(c, exprList[i], &j, 0, iterDepth);
                    // Report a failure to evaluate the expression
                    if (c->errStat.status) {
                        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                          "Could not evaluate vector expressions.");
                        ppl_tbAddLine(c, pl, ERR_NUMERICAL, 0, "");
                        ppl_tbWrite(c);
                        ppl_tbClear(c);
                        for (j = 0; j < i; j++) ppl_garbageObject(vecs + j);
                        ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
                        free(new);
                        new = NULL;
                        return 1;
                    }
                    // The expression must evaluate to a vector
                    if (obj->objType != PPLOBJ_VEC) {
                        ppl_snprintfTrunc(
                                msg, sizeof(msg),
                                "Vector data supplied to other columns, but columns %d evaluated to an object of type <%s>.",
                                i + 1, pplObjTypeNames[obj->objType]);
                        ppl_error(&c->errcontext, ERR_TYPE, -1, -1, msg);
                        for (j = 0; j < i; j++) ppl_garbageObject(vecs + j);
                        ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
                        free(new);
                        new = NULL;
                        return 1;
                    }
                    l2 = ((pplVector *) first->auxil)->v->size;
                    // All the vectors must have the same length
                    if (l != l2) {
                        ppl_snprintfTrunc(
                                msg, sizeof(msg),
                                "Data supplied as a list of vectors, but they have varying lengths, including %d (vector %d) and %d (vector %d).",
                                l, 1, l2, i + 1);
                        ppl_error(&c->errcontext, ERR_NUMERICAL, -1, -1, msg);
                        for (j = 0; j < i; j++) ppl_garbageObject(vecs + j);
                        ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack before giving up
                        free(new);
                        new = NULL;
                        return 1;
                    }
                    pplObjCpy(vecs + i, obj, 0, 0, 1);
                    vecs[i].refCount = 1;
                    ppl_stackClean(c, stkLevelOld); // Discard the expression's value, now that it has been copied
                }
                // Store the vectors, on the first pass only
                if (wildcardMatchNumber <= 0) {
                    new->function = 0;
                    new->NFunctions = Nexprs;
                    new->vectors = (pplObj *) malloc(Nexprs * sizeof(pplObj));
                    new->functions = (pplExpr **) malloc(Nexprs * sizeof(pplExpr *));
                    // Failure to allocate memory is reported as an error
                    if ((new->vectors == NULL) || (new->functions == NULL)) {
                        ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (U).");
                        for (i = 0; i < Nexprs; i++) ppl_garbageObject(vecs + i);
                        free(new);
                        new = NULL;
                        return 1;
                    }
                    memcpy(new->vectors, vecs, Nexprs * sizeof(pplObj));
                    for (i = 0; i < Nexprs; i++) new->functions[i] = pplExpr_cpy(exprList[i]);
                } else {
                    for (i = 0; i < Nexprs; i++) ppl_garbageObject(vecs + i);
                    free(new);
                    new = NULL;
                    return 1;
                }
            } else {
                ppl_stackClean(c, stkLevelOld); // Discard anything left on the stack
                ppl_tbClear(c);
                // Store copies of the functions, on the first pass only
                if (wildcardMatchNumber <= 0) {
                    new->function = 1;
                    new->NFunctions = Nexprs;
                    new->functions = (pplExpr **) malloc(Nexprs * sizeof(pplExpr *));
                    new->vectors = NULL;
                    // Failure to allocate memory is reported as an error
                    if (new->functions == NULL) {
                        ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (V).");
                        free(new);
                        new = NULL;
                        return 1;
                    }
                    for (i = 0; i < Nexprs; i++) new->functions[i] = pplExpr_cpy(exprList[i]);
                } else {
                    free(new);
                    new = NULL;
                    return 1;
                }
            }
        } else if ((pos2 > 0) && (stk[pos2].objType == PPLOBJ_STR)) // we have been passed a filename
        {
            char *filename = (char *) stk[pos2].auxil;
            const int status = ppl_getPlotFname(c, filename, wildcardMatchNumber, new);
            if (status) return 1;
        } else {
            ppl_snprintfTrunc(msg, sizeof(msg), "Could not find any expressions to evaluate.");
            ppl_error(&c->errcontext, ERR_INTERNAL, -1, -1, msg);
            free(new);
            new = NULL;
            return 1;
        }
    }

    // Read parametric flag
    new->parametric =
            ((ptab[PARSE_INDEX_parametric] > 0) && (stk[stkbase + ptab[PARSE_INDEX_parametric]].objType == PPLOBJ_STR));

    // Read T and V ranges
    new->TRangeSet = ((ptab[PARSE_INDEX_tmin] > 0) && (stk[stkbase + ptab[PARSE_INDEX_tmin]].objType != PPLOBJ_ZOM));
    new->VRangeSet = ((ptab[PARSE_INDEX_vmin] > 0) && (stk[stkbase + ptab[PARSE_INDEX_vmin]].objType != PPLOBJ_ZOM));

    // The range of t for a parametric plot, if given
    if (new->TRangeSet) {
        new->Tmin = stk[stkbase + ptab[PARSE_INDEX_tmin]];
        new->Tmax = stk[stkbase + ptab[PARSE_INDEX_tmax]];
        // The limits must be finite, and have the same units
        if (!gsl_finite(new->Tmin.real)) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Lower limit specified for parameter t is not finite.");
            ppl_error(&c->errcontext, ERR_NUMERICAL, -1, -1, msg);
            new->TRangeSet = 0;
        } else if (!gsl_finite(new->Tmax.real)) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Upper limit specified for parameter t is not finite.");
            ppl_error(&c->errcontext, ERR_NUMERICAL, -1, -1, msg);
            new->TRangeSet = 0;
        } else if (!ppl_unitsDimEqual(&new->Tmin, &new->Tmax)) {
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "Upper and lower limits specified for parameter t have conflicting physical units of <%s> and <%s>.",
                    ppl_printUnit(c, &new->Tmin, NULL, NULL, 0, 1, 0),
                    ppl_printUnit(c, &new->Tmax, NULL, NULL, 1, 1, 0));
            ppl_error(&c->errcontext, ERR_NUMERICAL, -1, -1, msg);
            new->TRangeSet = 0;
        }
    }

    // The range of v for a parametric plot, if given
    if (new->VRangeSet) {
        new->Vmin = stk[stkbase + ptab[PARSE_INDEX_vmin]];
        new->Vmax = stk[stkbase + ptab[PARSE_INDEX_vmax]];
        // The limits must be finite, and have the same units
        if (!gsl_finite(new->Vmin.real)) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Lower limit specified for parameter v is not finite.");
            ppl_error(&c->errcontext, ERR_NUMERICAL, -1, -1, msg);
            new->VRangeSet = 0;
        } else if (!gsl_finite(new->Vmax.real)) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Upper limit specified for parameter v is not finite.");
            ppl_error(&c->errcontext, ERR_NUMERICAL, -1, -1, msg);
            new->VRangeSet = 0;
        } else if (!ppl_unitsDimEqual(&new->Vmin, &new->Vmax)) {
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "Upper and lower limits specified for parameter v have conflicting physical units of <%s> and <%s>.",
                    ppl_printUnit(c, &new->Vmin, NULL, NULL, 0, 1, 0),
                    ppl_printUnit(c, &new->Vmax, NULL, NULL, 1, 1, 0));
            ppl_error(&c->errcontext, ERR_NUMERICAL, -1, -1, msg);
            new->VRangeSet = 0;
        }
    }

    // Read axes
    new->axis1set = new->axis2set = new->axis3set = 0;
    new->axis1 = new->axis2 = new->axis3 = 1;
    new->axis1xyz = 0;
    new->axis2xyz = 1;
    new->axis3xyz = 2;

    // The axes to plot against, if given
    if (ptab[PARSE_INDEX_axis_1] > 0) {
        const int got1 = (stk[stkbase + ptab[PARSE_INDEX_axis_1]].objType == PPLOBJ_NUM);
        const int xyz1 = got1 ? ((int) round(stk[stkbase + ptab[PARSE_INDEX_axis_1]].exponent[0])) : -1;
        const int n1 = got1 ? ((int) round(stk[stkbase + ptab[PARSE_INDEX_axis_1]].real)) : -1;
        const int got2 = (stk[stkbase + ptab[PARSE_INDEX_axis_2]].objType == PPLOBJ_NUM);
        const int xyz2 = got2 ? ((int) round(stk[stkbase + ptab[PARSE_INDEX_axis_2]].exponent[0])) : -1;
        const int n2 = got2 ? ((int) round(stk[stkbase + ptab[PARSE_INDEX_axis_2]].real)) : -1;
        const int got3 = (stk[stkbase + ptab[PARSE_INDEX_axis_3]].objType == PPLOBJ_NUM);
        const int xyz3 = got3 ? ((int) round(stk[stkbase + ptab[PARSE_INDEX_axis_3]].exponent[0])) : -1;
        const int n3 = got3 ? ((int) round(stk[stkbase + ptab[PARSE_INDEX_axis_3]].real)) : -1;
        // The first axis
        if (got1) {
            new->axis1set = 1;
            new->axis1xyz = xyz1;
            new->axis1 = n1;
        }
        // The second axis
        if (got2) {
            new->axis2set = 1;
            new->axis2xyz = xyz2;
            new->axis2 = n2;
        }
        // The third axis
        if (got3) {
            new->axis3set = 1;
            new->axis3xyz = xyz3;
            new->axis3 = n3;
        }
    }

    // Read using fields
    {
        int Nusing = 0, i = 0;
        int hadNonNullUsingItem = 0;
        int pos = ptab[PARSE_INDEX_using_list] + stkbase;
        const int o = ptab[PARSE_INDEX_using_item];

        // Count the using items
        if (o > 0) {
            // Walk the linked list of using items on the parser stack
            while (stk[pos].objType == PPLOBJ_NUM) {
                pos = (int) round(stk[pos].real);
                if (pos <= 0) break;
                // There is a fixed limit on the number of using items
                if (Nusing >= USING_ITEMS_MAX) {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Too many using items; maximum of %d are allowed.",
                                      USING_ITEMS_MAX);
                    ppl_error(&c->errcontext, ERR_SYNTAX, -1, -1, msg);
                    free(new);
                    new = NULL;
                    return 1;
                }
                if (stk[pos + o].objType == PPLOBJ_EXP) hadNonNullUsingItem = 1;
                Nusing++;
            }
            if ((!hadNonNullUsingItem) && (Nusing == 1))
                Nusing = 0; // If we've only had one using item, and it was blank, this is a parser abberation
            new->NUsing = Nusing;
            // Store copies of the using items, with room for extra items needed by style modifiers
            if (Nusing < 1) {
                new->UsingList = NULL;
            } else {
                new->UsingList = (pplExpr **) malloc(
                        (Nusing + 8) *
                        sizeof(pplExpr *)); // we may add some more using items later; leave room for eight
                // Failure to allocate memory is reported as an error
                if (new->UsingList == NULL) {
                    ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (W).");
                    free(new);
                    new = NULL;
                    return 1;
                }
                pos = ptab[PARSE_INDEX_using_list] + stkbase;
                // Copy each using item
                while (stk[pos].objType == PPLOBJ_NUM) {
                    pos = (int) round(stk[pos].real);
                    if (pos <= 0) break;
                    if (stk[pos + o].objType == PPLOBJ_EXP)
                        new->UsingList[i] = pplExpr_cpy((pplExpr *) stk[pos + o].auxil);
                    else new->UsingList[i] = NULL;
                    i++;
                }
            }
        }
    }

    // Read data label
    new->label = NULL;
    // The label expression, if given
    if (ptab[PARSE_INDEX_label] > 0) {
        pplObj *o = &stk[stkbase + ptab[PARSE_INDEX_label]];
        // The label must be an expression
        if (o->objType != PPLOBJ_EXP) {
            new->label = NULL;
        } else {
            new->label = pplExpr_cpy((pplExpr *) o->auxil);
        }
    }

    // Read continuous flag
    new->ContinuitySet = 0;
    new->continuity = DATAFILE_CONTINUOUS;
    // Whether the data are continuous or discontinuous, if specified
    if (ptab[PARSE_INDEX_continuous] > 0) {
        // Continuous data
        if (stk[stkbase + ptab[PARSE_INDEX_continuous]].objType == PPLOBJ_STR) {
            new->ContinuitySet = 1;
            new->continuity = DATAFILE_CONTINUOUS;
        }
        // Discontinuous data
        if (stk[stkbase + ptab[PARSE_INDEX_discontinuous]].objType == PPLOBJ_STR) {
            new->ContinuitySet = 1;
            new->continuity = DATAFILE_DISCONTINUOUS;
        }
    }

    // Read title/notitle setting
    new->NoTitleSet = new->TitleSet = 0;
    new->title = NULL;
    // The title of the dataset, or notitle
    if (ptab[PARSE_INDEX_title] > 0) {
        pplObj *o1 = &stk[stkbase + ptab[PARSE_INDEX_notitle]];
        pplObj *o2 = &stk[stkbase + ptab[PARSE_INDEX_title]];
        // notitle suppresses the dataset's entry in the legend
        if (o1->objType == PPLOBJ_STR) {
            new->NoTitleSet = 1;
            new->TitleSet = 0;
            new->title = NULL;
        } else if (o2->objType == PPLOBJ_STR) {
            new->NoTitleSet = 0;
            new->title = (char *) malloc(strlen((char *) o2->auxil) + 1);
            new->TitleSet = (new->title != NULL);
            if (new->TitleSet) strcpy(new->title, (char *) o2->auxil);
        }
    }

    // Read index field
    {
        const int p1 = ptab[PARSE_INDEX_index];
        const int p2 = ptab[PARSE_INDEX_use_rows];
        const int p3 = ptab[PARSE_INDEX_use_columns];
        // The index modifier
        if ((p1 > 0) && (stk[stkbase + p1].objType == PPLOBJ_NUM)) {
            new->IndexSet = 1;
            new->index = (int) round(stk[stkbase + p1].real);
        } else {
            new->IndexSet = 0;
            new->index = -1;
        }
        // Read the data in rows rather than columns
        if ((p2 > 0) && (stk[stkbase + p2].objType == PPLOBJ_STR)) {
            new->UsingRowCols = DATAFILE_ROW;
        } else if ((p3 > 0) && (stk[stkbase + p3].objType == PPLOBJ_STR)) {
            new->UsingRowCols = DATAFILE_COL;
        } else {
            new->UsingRowCols = DATAFILE_COL;
        }
    }

    // Read every fields
    new->EverySet = 0;
    new->EveryList[0] = new->EveryList[1] = 1;
    new->EveryList[2] = new->EveryList[3] = new->EveryList[4] = new->EveryList[5] = -1;
    // The every modifier
    if (ptab[PARSE_INDEX_every_list] > 0) {
        int Nevery = 0;
        int pos = ptab[PARSE_INDEX_every_list] + stkbase;
        const int o = ptab[PARSE_INDEX_every_item];
        // Read each item of the every modifier
        while (stk[pos].objType == PPLOBJ_NUM) {
            long x;
            pos = (int) round(stk[pos].real);
            if (pos <= 0) break;
            // There may be no more than six items
            if (Nevery >= 6) {
                ppl_warning(&c->errcontext, ERR_SYNTAX,
                            "More than six numbers supplied to the every modifier -- trailing entries ignored.");
                break;
            }
            x = (long) round(stk[pos + o].real);
            if (x > new->EveryList[Nevery]) new->EveryList[Nevery] = x;
            new->EverySet = ++Nevery;
        }
    }

    // Read select criterion
    {
        const int p = ptab[PARSE_INDEX_select_criterion];
        if ((p > 0) && (stk[stkbase + p].objType == PPLOBJ_EXP))
            new->SelectCriterion = pplExpr_cpy((pplExpr *) stk[stkbase + p].auxil);
        else new->SelectCriterion = NULL;
    }

    // Read withWords
    ppl_withWordsFromDict(c, in, pl, ptab, stkbase, &new->ww);

    // If reading data from pipe, read and store it now
    if ((new->filename != NULL) && ((strcmp(new->filename, "-") == 0) || (strcmp(new->filename, "--") == 0))) {
        int status = 0, errCount = DATAFILE_NERRS, nObjs = 0;
        pplExpr **UsingList;
        int NUsing = new->NUsing;
        unsigned char autoUsingList = 0;
        const int linespoints = new->ww.USElinespoints
                ? new->ww.linespoints
                : (ci->settings.dataStyle.USElinespoints ? ci->settings.dataStyle.linespoints : SW_STYLE_POINTS);
        int NExpect =
                (NExpectIn > 0) ? NExpectIn : eps_plot_styles_NDataColumns(&c->errcontext, linespoints, ci->ThreeDim);
        char *errbuff = (char *) malloc(LSTR_LENGTH);

        // Check that the number of using items suits the plot style
        UsingList = (pplExpr **) malloc((USING_ITEMS_MAX + 8) * sizeof(pplExpr *));
        // Failure to allocate memory is reported as an error
        if ((errbuff == NULL) || (UsingList == NULL)) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (X).");
            free(new);
            new = NULL;
            return 1;
        }

        if (NUsing > 0) memcpy(UsingList, new->UsingList, NUsing * sizeof(pplExpr *)); // UsingList may be NULL
        new->ww.linespoints =
                linespoints; // Fix plot style, so that number of expected columns doesn't later change with DataStyle
        new->ww.USElinespoints = 1;

        // Color maps can take 3,4,5 or 6 columns of data
        if (linespoints == SW_STYLE_COLORMAP) {
            const int listlen = NUsing;
            if ((listlen >= 3) && (listlen <= 6)) NExpect = listlen;
        }

        // Add any using items needed by style modifiers
        if (eps_plot_AddUsingItemsForWithWords(c, &new->ww, &NExpect, &autoUsingList, &UsingList, &NUsing, &nObjs,
                                               errbuff)) {
            free(new);
            new = NULL;
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1, errbuff);
            free(errbuff);
            errbuff = NULL;
            free(UsingList);
            UsingList = NULL;
            return 1;
        } // Add extra using items for, e.g. "linewidth $3".
        // The number of using items must match the number of columns the style needs
        if (NExpect != NUsing) {
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "The supplied using ... clause contains the wrong number of items. We need %d columns of data, but %d have been supplied.",
                    NExpect, NUsing);
            ppl_error(&c->errcontext, ERR_SYNTAX, -1, -1, msg);
            free(errbuff);
            errbuff = NULL;
            free(UsingList);
            UsingList = NULL;
            return 1;
        }
        ppldata_fromFile(c, &new->PersistentDataTable, new->filename, 0, NULL, dataSpool, new->index, UsingList,
                         autoUsingList, NExpect, nObjs, new->label, new->SelectCriterion, NULL, new->UsingRowCols,
                         new->EveryList, new->continuity, 1, &status, errbuff, &errCount, iterDepth);
        free(errbuff);
        errbuff = NULL;
        free(UsingList);
        UsingList = NULL;
    }


    // Store plot item
    *plotItemPtr = new;
    return 0;
}

//! ppl_directive_piechart - Implement the piechart command, which adds a pie chart to the multiplot canvas
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return Zero on success, or one on failure

int ppl_directive_piechart(ppl_context *c, parserLine *pl, parserOutput *in, const int interactive,
                           const int iterDepth) {
    pplObj *stk = in->stk;
    parserLine *spool = NULL, **dataSpool = &spool;
    canvas_item *ptr;
    int id, status;

    // Add this piechart to the linked list which decribes the canvas
    if (canvas_itemlist_add(c, stk, CANVAS_PIE, &ptr, &id, 0)) { return 1; }

    // Look up format string
    if (stk[PARSE_piechart_format_string].objType == PPLOBJ_EXP)
        ptr->format = pplExpr_cpy((pplExpr *) stk[PARSE_piechart_format_string].auxil);
    else ptr->format = NULL;

    // Look up label position
    if (stk[PARSE_piechart_piekeypos].objType == PPLOBJ_STR) {
        char *i = (char *) stk[PARSE_piechart_piekeypos].auxil;
        ptr->ArrowType = ppl_fetchSettingByName(&c->errcontext, i, SW_PIEKEYPOS_INT, SW_PIEKEYPOS_STR);
    } else {
        ptr->ArrowType = SW_PIEKEYPOS_AUTO;
    }

    // Look up data spool
    if (stk[PARSE_piechart_data].objType == PPLOBJ_BYT) {
        spool = (parserLine *) stk[PARSE_piechart_data].auxil;
        if (spool != NULL) spool = spool->next; // first line is command line
    }

    // Fetch options in common with the plot command
    status = ppl_getPlotData(c, pl, in, ptr, PARSE_TABLE_piechart_, 0, 0, iterDepth, dataSpool, 1);
    // Give up if the data description could not be read
    if (status) {
        canvas_delete(c, id);
        return 1;
    }

    // Redisplay the canvas as required
    if ((c->set->term_current.display == SW_ONOFF_ON) && (!cancellationFlag)) {
        unsigned char *unsuccessful_ops = (unsigned char *) malloc(MULTIPLOT_MAXINDEX);
        int failed;
        // Failure to allocate memory is reported as an error
        if (unsuccessful_ops == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        }
        ppl_canvas_draw(c, unsuccessful_ops, iterDepth);
        failed = unsuccessful_ops[id];
        free(unsuccessful_ops);
        unsuccessful_ops = NULL;
        // If the new item could not be drawn, remove it again
        if (failed) {
            canvas_delete(c, id);
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1,
                      "Piechart has been removed from multiplot, because it generated an error.");
            return 1;
        }
    }
    return 0;
}

//! ppl_directive_plot - Implement the plot and replot commands, which add a graph to the multiplot canvas, or add
//! further plot items to the most recent graph
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \param [in] replot - If true, implement the replot command rather than the plot command
//! \return Zero on success, or one on failure

int ppl_directive_plot(ppl_context *c, parserLine *pl, parserOutput *in, const int interactive, const int iterDepth,
                       const int replot) {
    char msg[PPL_MSG_LENGTH];
    pplObj *stk = in->stk;
    canvas_item *ptr = NULL;
    parserLine *spool = NULL, **dataSpool = &spool;
    canvas_itemlist *canvas_items = c->canvas_items;
    int id;

    // If replotting, find a plot item to append plot items onto
    if (replot) {
        const int gotEditNo = (stk[PARSE_replot_editno].objType == PPLOBJ_NUM);
        const int editNo = gotEditNo ? ((int) round(stk[PARSE_replot_editno].real)) : c->replotFocus;
        // replot adds to the previous plot, or to the plot with the item number given
        if (canvas_items != NULL) {
            ptr = canvas_items->first;
            while ((ptr != NULL) && (ptr->id != editNo)) ptr = ptr->next;
        }
        // Report that there is no plot to add to
        if (ptr == NULL) {
            ppl_snprintfTrunc(msg, sizeof(msg), "No plot found to replot.");
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1, msg);
            return 1;
        }
        id = editNo;
    } else {
        // Add this plot to the linked list which decribes the canvas
        if (canvas_itemlist_add(c, stk, CANVAS_PLOT, &ptr, &id, 0)) { return 1; }
    }
    c->replotFocus = id;

    // Copy graph settings and axes to this plot structure. Do this every time that the replot command is called
    ppl_withWordsDestroy(c, &ptr->settings.dataStyle); // First free the old set of settings which we'd stored
    ppl_withWordsDestroy(c, &ptr->settings.funcStyle);
    pplExpr_free(ptr->settings.ColMapExpr);
    ptr->settings.ColMapExpr = NULL;
    pplExpr_free(ptr->settings.MaskExpr);
    ptr->settings.MaskExpr = NULL;
    pplExpr_free(ptr->settings.c1format);
    ptr->settings.c1format = NULL;
    // Free the old x axes
    if (ptr->XAxes != NULL) {
        int i;
        for (i = 0; i < MAX_AXES; i++) pplaxis_destroy(c, &(ptr->XAxes[i]));
        free(ptr->XAxes);
        ptr->XAxes = NULL;
    }
    // Free the old y axes
    if (ptr->YAxes != NULL) {
        int i;
        for (i = 0; i < MAX_AXES; i++) pplaxis_destroy(c, &(ptr->YAxes[i]));
        free(ptr->YAxes);
        ptr->YAxes = NULL;
    }
    // Free the old z axes
    if (ptr->ZAxes != NULL) {
        int i;
        for (i = 0; i < MAX_AXES; i++) pplaxis_destroy(c, &(ptr->ZAxes[i]));
        free(ptr->ZAxes);
        ptr->ZAxes = NULL;
    }
    pplarrow_list_destroy(c, &ptr->arrow_list);
    ppllabel_list_destroy(c, &ptr->label_list);
    ptr->settings = c->set->graph_current;
    ptr->settings.ColMapExpr = pplExpr_cpy(c->set->graph_current.ColMapExpr);
    ptr->settings.MaskExpr = pplExpr_cpy(c->set->graph_current.MaskExpr);
    ptr->settings.c1format = pplExpr_cpy(c->set->graph_current.c1format);
    ppl_withWordsCpy(c, &ptr->settings.dataStyle, &c->set->graph_current.dataStyle);
    ppl_withWordsCpy(c, &ptr->settings.funcStyle, &c->set->graph_current.funcStyle);
    ptr->XAxes = (pplset_axis *) malloc(MAX_AXES * sizeof(pplset_axis));
    ptr->YAxes = (pplset_axis *) malloc(MAX_AXES * sizeof(pplset_axis));
    ptr->ZAxes = (pplset_axis *) malloc(MAX_AXES * sizeof(pplset_axis));
    // Report a failure to allocate memory, freeing any axes which were allocated
    if ((ptr->XAxes == NULL) || (ptr->YAxes == NULL) || (ptr->ZAxes == NULL)) {
        ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (0).");
        // Free the x axes
        if (ptr->XAxes != NULL) {
            free(ptr->XAxes);
            ptr->XAxes = NULL;
        }
        // Free the y axes
        if (ptr->YAxes != NULL) {
            free(ptr->YAxes);
            ptr->YAxes = NULL;
        }
        // Free the z axes
        if (ptr->ZAxes != NULL) {
            free(ptr->ZAxes);
            ptr->ZAxes = NULL;
        }
        return 1;
    } else {
        int i;
        for (i = 0; i < MAX_AXES; i++) pplaxis_copy(c, &(ptr->XAxes[i]), &(c->set->XAxes[i]));
        for (i = 0; i < MAX_AXES; i++) pplaxis_copy(c, &(ptr->YAxes[i]), &(c->set->YAxes[i]));
        for (i = 0; i < MAX_AXES; i++) pplaxis_copy(c, &(ptr->ZAxes[i]), &(c->set->ZAxes[i]));
    }
    pplarrow_list_copy(c, &ptr->arrow_list, &c->set->pplarrow_list);
    ppllabel_list_copy(c, &ptr->label_list, &c->set->ppllabel_list);

    // Check whether 3d or 2d plot
    if (!replot) {
        ptr->ThreeDim = (stk[PARSE_replot_threedim].objType == PPLOBJ_STR); // Set 3d flag
    }

    // Read data range
    {
        int nr = 0;
        int pos = PARSE_replot_0range_list;
        const int o1 = PARSE_replot_min_0range_list;
        const int o2 = PARSE_replot_max_0range_list;
        const int o3 = PARSE_replot_minauto_0range_list;
        const int o4 = PARSE_replot_maxauto_0range_list;
        canvas_plotrange **rangePtr = &ptr->plotranges;
        // Read each range, e.g. [0:1], adding it to those of any plot being replotted
        while (stk[pos].objType == PPLOBJ_NUM) {
            pos = (int) round(stk[pos].real);
            if (pos <= 0) break;
            // There is a fixed limit on the number of ranges
            if (nr >= USING_ITEMS_MAX) {
                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                  "Too many ranges specified; a maximum of %d are allowed.", USING_ITEMS_MAX);
                ppl_tbAddLine(c, pl, ERR_SYNTAX, 0, "");
                canvas_delete(c, id);
                return 1;
            }
            // Make a new range, if the plot does not already have one in this position
            if (*rangePtr == NULL) {
                *rangePtr = (canvas_plotrange *) malloc(sizeof(canvas_plotrange));
                if (*rangePtr == NULL) {
                    ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory (1).");
                    canvas_delete(c, id);
                    return 1;
                }
                pplObjNum(&(*rangePtr)->unit, 0, 0, 0);
                (*rangePtr)->min = (*rangePtr)->max = 0.0;
                (*rangePtr)->MinSet = (*rangePtr)->MaxSet = (*rangePtr)->AutoMinSet = (*rangePtr)->AutoMaxSet = 0;
                (*rangePtr)->next = NULL;
            }
            {
                pplObj *min = &stk[pos + o1];
                const int gotMin =
                        (min->objType == PPLOBJ_NUM) || (min->objType == PPLOBJ_DATE) || (min->objType == PPLOBJ_BOOL);
                pplObj *max = &stk[pos + o2];
                const int gotMax =
                        (max->objType == PPLOBJ_NUM) || (max->objType == PPLOBJ_DATE) || (max->objType == PPLOBJ_BOOL);
                const int minAuto = (stk[pos + o3].objType == PPLOBJ_STR);
                const int maxAuto = (stk[pos + o4].objType == PPLOBJ_STR);
                // The limits must have the same type
                if (gotMin && gotMax && (min->objType != max->objType)) {
                    ppl_snprintfTrunc(
                            c->errStat.errBuff, sizeof(c->errStat.errBuff),
                            "Minimum and maximum limits specified in range %d have conflicting types of <%s> and <%s>.",
                            nr + 1, pplObjTypeNames[min->objType], pplObjTypeNames[max->objType]);
                    ppl_tbAddLine(c, pl, ERR_TYPE, in->stkCharPos[pos + PARSE_replot_min_0range_list], "");
                    canvas_delete(c, id);
                    return 1;
                }
                // The limits must have the same units
                if (gotMin && gotMax && !ppl_unitsDimEqual(min, max)) {
                    ppl_snprintfTrunc(
                            c->errStat.errBuff, sizeof(c->errStat.errBuff),
                            "Minimum and maximum limits specified in range %d have conflicting units of <%s> and <%s>.",
                            nr + 1, ppl_printUnit(c, min, NULL, NULL, 0, 0, 0),
                            ppl_printUnit(c, max, NULL, NULL, 0, 0, 0));
                    ppl_tbAddLine(c, pl, ERR_UNIT, in->stkCharPos[pos + PARSE_replot_min_0range_list], "");
                    canvas_delete(c, id);
                    return 1;
                }

                // A minimum alone must have the same type as any existing maximum
                if (gotMin && (!gotMax) && (!maxAuto) && ((*rangePtr)->MaxSet) &&
                    (min->objType != (*rangePtr)->unit.objType)) {
                    ppl_snprintfTrunc(
                            c->errStat.errBuff, sizeof(c->errStat.errBuff),
                            "Minimum and maximum limits specified in range %d have conflicting types of <%s> and <%s>.",
                            nr + 1, pplObjTypeNames[min->objType], pplObjTypeNames[(*rangePtr)->unit.objType]);
                    ppl_tbAddLine(c, pl, ERR_TYPE, in->stkCharPos[pos + PARSE_replot_min_0range_list], "");
                    canvas_delete(c, id);
                    return 1;
                }
                if (gotMin && (!gotMax) && (!maxAuto) && ((*rangePtr)->MaxSet) &&
                    (!ppl_unitsDimEqual(min, &(*rangePtr)->unit))) {
                    ppl_snprintfTrunc(
                            c->errStat.errBuff, sizeof(c->errStat.errBuff),
                            "Minimum and maximum limits specified in range %d have conflicting units of <%s> and <%s>.",
                            nr + 1, ppl_printUnit(c, min, NULL, NULL, 0, 0, 0),
                            ppl_printUnit(c, &(*rangePtr)->unit, NULL, NULL, 0, 0, 0));
                    ppl_tbAddLine(c, pl, ERR_UNIT, in->stkCharPos[pos + PARSE_replot_min_0range_list], "");
                    canvas_delete(c, id);
                    return 1;
                }

                // A maximum alone must have the same type as any existing minimum
                if (gotMax && (!gotMin) && (!minAuto) && ((*rangePtr)->MinSet) &&
                    (max->objType != (*rangePtr)->unit.objType)) {
                    ppl_snprintfTrunc(
                            c->errStat.errBuff, sizeof(c->errStat.errBuff),
                            "Minimum and maximum limits specified in range %d have conflicting types of <%s> and <%s>.",
                            nr + 1, pplObjTypeNames[(*rangePtr)->unit.objType], pplObjTypeNames[max->objType]);
                    ppl_tbAddLine(c, pl, ERR_TYPE, in->stkCharPos[pos + PARSE_replot_min_0range_list], "");
                    canvas_delete(c, id);
                    return 1;
                }
                if (gotMax && (!gotMin) && (!minAuto) && ((*rangePtr)->MinSet) &&
                    (!ppl_unitsDimEqual(max, &(*rangePtr)->unit))) {
                    ppl_snprintfTrunc(
                            c->errStat.errBuff, sizeof(c->errStat.errBuff),
                            "Minimum and maximum limits specified in range %d have conflicting units of <%s> and <%s>.",
                            nr + 1, ppl_printUnit(c, &(*rangePtr)->unit, NULL, NULL, 0, 0, 0),
                            ppl_printUnit(c, max, NULL, NULL, 0, 0, 0));
                    ppl_tbAddLine(c, pl, ERR_UNIT, in->stkCharPos[pos + PARSE_replot_min_0range_list], "");
                    canvas_delete(c, id);
                    return 1;
                }

                // An automatic minimum
                if (minAuto) {
                    (*rangePtr)->AutoMinSet = 1;
                    (*rangePtr)->MinSet = 0;
                    (*rangePtr)->min = 0.0;
                }
                // An automatic maximum
                if (maxAuto) {
                    (*rangePtr)->AutoMaxSet = 1;
                    (*rangePtr)->MaxSet = 0;
                    (*rangePtr)->max = 0.0;
                }
                // A minimum given
                if (gotMin) {
                    (*rangePtr)->AutoMinSet = 0;
                    (*rangePtr)->MinSet = 1;
                    (*rangePtr)->min = min->real;
                    (*rangePtr)->unit = *min;
                    (*rangePtr)->unit.real = 1.0;
                }
                // A maximum given
                if (gotMax) {
                    (*rangePtr)->AutoMaxSet = 0;
                    (*rangePtr)->MaxSet = 1;
                    (*rangePtr)->max = max->real;
                    (*rangePtr)->unit = *max;
                    (*rangePtr)->unit.real = 1.0;
                }
            }
            rangePtr = &(*rangePtr)->next;
            nr++;
        }
    }

    // Look up data spool
    if (stk[PARSE_replot_data].objType == PPLOBJ_BYT) {
        spool = (parserLine *) stk[PARSE_replot_data].auxil;
        if (spool != NULL) spool = spool->next; // first line is command line
    }

    // Read list of plotted items
    {
        int pos = PARSE_replot_0plot_list;
        // Walk the linked list of datasets on the parser stack
        while (stk[pos].objType == PPLOBJ_NUM) {
            int w;
            pos = (int) round(stk[pos].real);
            if (pos <= 0) break;

            // Check that axes are correctly specified
            {
                const int got1 = (stk[pos + PARSE_replot_axis_1_0plot_list].objType == PPLOBJ_NUM);
                const int xyz1 = got1 ? ((int) round(stk[pos + PARSE_replot_axis_1_0plot_list].exponent[0])) : -1;
                //int n1 = got1?((int)round(stk[pos+PARSE_replot_axis_1_0plot_list].real       )):-1;
                const int got2 = (stk[pos + PARSE_replot_axis_2_0plot_list].objType == PPLOBJ_NUM);
                const int xyz2 = got2 ? ((int) round(stk[pos + PARSE_replot_axis_2_0plot_list].exponent[0])) : -1;
                //int n2 = got2?((int)round(stk[pos+PARSE_replot_axis_2_0plot_list].real       )):-1;
                const int got3 = (stk[pos + PARSE_replot_axis_3_0plot_list].objType == PPLOBJ_NUM);
                const int xyz3 = got3 ? ((int) round(stk[pos + PARSE_replot_axis_3_0plot_list].exponent[0])) : -1;
                //int n3 = got3?((int)round(stk[pos+PARSE_replot_axis_3_0plot_list].real       )):-1;
                if (got1 || got2 || got3) {
                    msg[0] = '\0';
                    if ((!ptr->ThreeDim) && (!(got1 && got2 && !got3)))
                        ppl_snprintfTrunc(
                                msg, sizeof(msg),
                                "The axes clause in the plot command must contain two perpendicular axes to produce a two-dimensional plot.");
                    else if (ptr->ThreeDim && (!(got1 && got2 && got3)))
                        ppl_snprintfTrunc(
                                msg, sizeof(msg),
                                "The axes clause in the plot command must contain three perpendicular axes to produce a three-dimensional plot.");
                    else if (((!ptr->ThreeDim) && (((xyz1 != 0) && (xyz2 != 0)) || ((xyz1 != 1) && (xyz2 != 1)))) ||
                             ((ptr->ThreeDim) && ((xyz1 == xyz2) || (xyz2 == xyz3))))
                        ppl_snprintfTrunc(msg, sizeof(msg),
                                          "The axes clause in the plot command may not list multiple parallel axes.");
                    // Report any problems with the axes clause
                    if (msg[0] != '\0') {
                        ppl_error(&c->errcontext, ERR_NUMERICAL, -1, -1, msg);
                        canvas_delete(c, id);
                        return 1;
                    }
                }
            }

            // Loop over wildcard matches
            for (w = 0;; w++) {
                const int status =
                        ppl_getPlotData(c, pl, in, ptr, PARSE_TABLE_replot_, pos, w, iterDepth, dataSpool, -1);
                // Give up if the description cannot be read; a later wildcard match which fails just means there are no
                // more files
                if (status) {
                    // The first match failed
                    if (w == 0) {
                        canvas_delete(c, id);
                        return 1;
                    } else {
                        break;
                    }
                }
            }
        }
    }

    // Redisplay the canvas as required
    if ((c->set->term_current.display == SW_ONOFF_ON) && (!cancellationFlag)) {
        unsigned char *unsuccessful_ops = (unsigned char *) malloc(MULTIPLOT_MAXINDEX);
        int failed;
        // Failure to allocate memory is reported as an error
        if (unsuccessful_ops == NULL) {
            ppl_error(&c->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return 1;
        }
        ppl_canvas_draw(c, unsuccessful_ops, iterDepth);
        failed = unsuccessful_ops[id];
        free(unsuccessful_ops);
        unsuccessful_ops = NULL;
        // If the new item could not be drawn, remove it again
        if (failed) {
            canvas_delete(c, id);
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1,
                      "Plot has been removed from multiplot, because it generated an error.");
            return 1;
        }
    }
    return 0;
}
