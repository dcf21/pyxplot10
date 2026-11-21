// labels.c
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

// Stores the text labels which users add to graphs with the "set label" command: adding, removing and resetting them,
// comparing and copying lists of them, and describing them for the show and save commands. The labels are drawn by
// epsMaker/eps_plot_labelsarrows.c. Coordinates are read with helper functions shared with arrows.c.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <gsl/gsl_math.h>

#include "coreUtils/dict.h"
#include "coreUtils/list.h"
#include "coreUtils/errorReport.h"
#include "expressions/traceback_fns.h"
#include "parser/cmdList.h"
#include "parser/parser.h"
#include "settings/settingTypes.h"
#include "settings/arrows_fns.h"
#include "settings/labels_fns.h"
#include "settings/settings.h"
#include "settings/withWords_fns.h"
#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"
#include "userspace/context.h"
#include "userspace/pplObj_fns.h"
#include "userspace/unitsArithmetic.h"
#include "pplConstants.h"

// -------------------------------------------
// ROUTINES FOR MANIPULATING LABELS STRUCTURES
// -------------------------------------------


//! sameValue - Test whether two numerical values are equal, including having the same physical dimensions
//! \param [in] a - The first value
//! \param [in] b - The second value
//! \return True if the values are equal

static int sameValue(const pplObj *a, const pplObj *b) {
    return ppl_unitsDimEqual(a, b) && ppl_dblEqual(a->real, b->real);
}


//! ppllabel_add - Implement the "set label" command, adding a new text label to a list of labels, or replacing an
//! existing label with the same ID number
//! \param [in] context - The Pyxplot context
//! \param [in|out] inlist - Pointer to the head of the linked list of labels, sorted by ID number
//! \param [in] in - The parser output for the command, containing the label's ID, text, position and style
//! \param [in] pl - The parsed command line, used to report errors
//! \param [in] ptab - Table mapping parser variable indices onto positions in <in->stk>

void ppllabel_add(ppl_context *context, ppllabel_object **inlist, parserOutput *in, parserLine *pl, const int *ptab) {
    // The coordinate system of each coordinate, the label's text, rotation and gap, and the record of the label
    ppllabel_object *out;

    // Read the coordinate system of each coordinate
    const int system_x = pplarrow_getSystem(context, in, ptab, PARSE_INDEX_x_system);
    const int system_y = pplarrow_getSystem(context, in, ptab, PARSE_INDEX_y_system);
    const int system_z = pplarrow_getSystem(context, in, ptab, PARSE_INDEX_z_system);

    // Check the units of each coordinate
    pplarrow_checkDimensions(context, in, pl, ptab, PARSE_INDEX_x, system_x);
    pplarrow_checkDimensions(context, in, pl, ptab, PARSE_INDEX_y, system_y);
    pplarrow_checkDimensions(context, in, pl, ptab, PARSE_INDEX_z, system_z);

    // Check the axis number of each coordinate
    if (pplarrow_checkAxis(context, in, pl, ptab, PARSE_INDEX_x_axis)) return;
    if (pplarrow_checkAxis(context, in, pl, ptab, PARSE_INDEX_y_axis)) return;
    if (pplarrow_checkAxis(context, in, pl, ptab, PARSE_INDEX_z_axis)) return;

    char *tempstr = (char *) in->stk[ptab[PARSE_INDEX_label_text]].auxil; // Read label text
    char *label = (char *) malloc(strlen(tempstr) + 1);
    // Failure to allocate memory is reported as an error
    if (label == NULL) {
        ppl_error(&context->errcontext, ERR_MEMORY, -1, -1, "Out of memory");
        return;
    }
    strcpy(label, tempstr);

    const double ang = in->stk[ptab[PARSE_INDEX_rotation]].real; // Check for rotation modifier
    const int gotAng = (in->stk[ptab[PARSE_INDEX_rotation]].objType == PPLOBJ_NUM);
    const double gap = in->stk[ptab[PARSE_INDEX_gap]].real; // Check for gap modifier
    const int gotGap = (in->stk[ptab[PARSE_INDEX_gap]].objType == PPLOBJ_NUM);

    // See if fontsize is specified. Font sizes must be positive and finite; this is checked before any existing label
    // with this ID is changed.
    const int gotFontsize = (in->stk[ptab[PARSE_INDEX_fontsize]].objType == PPLOBJ_NUM);
    const double fontsize = in->stk[ptab[PARSE_INDEX_fontsize]].real;
    if (gotFontsize) {
        // Reject negative font sizes
        if (fontsize < 0) {
            ppl_error(&context->errcontext, ERR_RANGE, -1, -1,
                      "Fontsize specified in 'set label' command was less than zero.");
            free(label);
            return;
        }
        // Reject infinite font sizes
        if (!gsl_finite(fontsize)) {
            ppl_error(&context->errcontext, ERR_RANGE, -1, -1,
                      "Fontsize specified in 'set label' command was not finite.");
            free(label);
            return;
        }
    }

    // Look up ID number of the label we are adding and find appropriate place for it in label list
    const int i = (int) round(in->stk[ptab[PARSE_INDEX_label_id]].real);
    while ((*inlist != NULL) && ((*inlist)->id < i)) inlist = &((*inlist)->next);
    // Reuse an existing label with this ID, or insert a new one
    if ((*inlist != NULL) && ((*inlist)->id == i)) {
        out = *inlist;
        ppl_withWordsDestroy(context, &out->style);
        free(out->text);
        out->text = NULL;
    } else {
        out = (ppllabel_object *) malloc(sizeof(ppllabel_object));
        // Failure to allocate memory is reported as an error
        if (out == NULL) {
            ppl_error(&context->errcontext, ERR_MEMORY, -1, -1, "Out of memory");
            free(label);
            return;
        }
        out->id = i;
        out->next = *inlist;
        *inlist = out;
    }

    // Check for halign or valign modifiers
    tempstr = (char *) in->stk[ptab[PARSE_INDEX_halign]].auxil;
    int gotTempstr = (in->stk[ptab[PARSE_INDEX_halign]].objType == PPLOBJ_STR);
    if (gotTempstr) out->HAlign = ppl_fetchSettingByName(&context->errcontext, tempstr, SW_HALIGN_INT, SW_HALIGN_STR);
    else out->HAlign = context->set->graph_current.TextHAlign;
    tempstr = (char *) in->stk[ptab[PARSE_INDEX_valign]].auxil;
    gotTempstr = (in->stk[ptab[PARSE_INDEX_valign]].objType == PPLOBJ_STR);
    if (gotTempstr) out->VAlign = ppl_fetchSettingByName(&context->errcontext, tempstr, SW_VALIGN_INT, SW_VALIGN_STR);
    else out->VAlign = context->set->graph_current.TextVAlign;

    // The rotation and gap default to zero
    if (gotAng) out->rotation = ang;
    else out->rotation = 0.0;
    if (gotGap) out->gap = gap;
    else out->gap = 0.0;

    // The font size, if given
    out->fontsizeSet = gotFontsize;
    if (gotFontsize) out->fontsize = fontsize;

    // Store the label's style, text, and the coordinate system, axis number and value of each coordinate
    ppl_withWordsFromDict(context, in, pl, ptab, 0, &out->style);
    out->text = label;
    out->system_x = system_x;
    out->system_y = system_y;
    out->system_z = system_z;
    out->axis_x = pplarrow_getAxis(in, ptab, PARSE_INDEX_x_axis);
    out->axis_y = pplarrow_getAxis(in, ptab, PARSE_INDEX_y_axis);
    out->axis_z = pplarrow_getAxis(in, ptab, PARSE_INDEX_z_axis);
    pplarrow_copyCoordinate(in, ptab, PARSE_INDEX_x, system_x, &out->x);
    pplarrow_copyCoordinate(in, ptab, PARSE_INDEX_y, system_y, &out->y);
    pplarrow_copyCoordinate(in, ptab, PARSE_INDEX_z, system_z, &out->z);
    return;
}

//! ppllabel_remove - Implement the "set nolabel" command, removing labels from a list of labels
//! \param [in] context - The Pyxplot context
//! \param [in|out] inlist - Pointer to the head of the linked list of labels
//! \param [in] in - The parser output for the command, containing the list of label IDs to remove. If no IDs are
//! given, all labels are removed.
//! \param [in] pl - The parsed command line
//! \param [in] ptab - Table mapping parser variable indices onto positions in <in->stk>
//! \param [in] quiet - If true, do not warn about label IDs which are not defined

void ppllabel_remove(ppl_context *context, ppllabel_object **inlist, parserOutput *in, const parserLine *pl,
                     const int *ptab, const int quiet) {
    char msg[PPL_MSG_LENGTH];

    // pos is the position in the parser's output stack of the list of label IDs
    ppllabel_object **first = inlist;
    int pos = ptab[PARSE_INDEX_0label_list];

    // With no IDs, remove every label
    if ((in->stk[pos].objType != PPLOBJ_NUM) || (in->stk[pos].real <= 0))
        ppllabel_list_destroy(context, inlist); // set noarrow with no number specified means all arrows deleted
    // Otherwise remove each listed label
    else {
        // Each item in the parser's list holds the position of the next item, or zero at the end of the list
        while (in->stk[pos].objType == PPLOBJ_NUM) {
            pos = (int) round(in->stk[pos].real);
            if (pos <= 0) break;
            const int i = (int) round(in->stk[pos + ptab[PARSE_INDEX_label_id]].real);
            inlist = first;
            while ((*inlist != NULL) && ((*inlist)->id < i)) inlist = &((*inlist)->next);
            // Unlink and free the label, or warn that it does not exist
            if ((*inlist != NULL) && ((*inlist)->id == i)) {
                ppllabel_object *obj = *inlist;
                *inlist = (*inlist)->next;
                ppl_withWordsDestroy(context, &obj->style);
                free(obj->text);
                obj->text = NULL;
                free(obj);
                obj = NULL;
            } else if (!quiet) {
                ppl_snprintfTrunc(msg, sizeof(msg), "Label number %d is not defined", i);
                ppl_warning(&context->errcontext, ERR_GENERIC, msg);
            }
        }
    }
    return;
}

//! ppllabel_unset - Implement the "unset label" command, reverting labels to their default definitions
//! \param [in] context - The Pyxplot context
//! \param [in|out] inlist - Pointer to the head of the linked list of labels
//! \param [in] in - The parser output for the command, containing the list of label IDs to unset. If no IDs are
//! given, all labels are reverted to their defaults.
//! \param [in] pl - The parsed command line
//! \param [in] ptab - Table mapping parser variable indices onto positions in <in->stk>

void ppllabel_unset(ppl_context *context, ppllabel_object **inlist, parserOutput *in, parserLine *pl, const int *ptab) {
    ppllabel_remove(context, inlist, in, pl, ptab, 1); // First of all, remove any labels which we are unsetting
    ppllabel_object **first = inlist;
    // pos is the position in the parser's output stack of the list of label IDs
    int pos = ptab[PARSE_INDEX_0label_list];

    // With no IDs, replace all labels with a copy of the default list
    if ((in->stk[pos].objType != PPLOBJ_NUM) || (in->stk[pos].real <= 0)) {
        ppllabel_list_destroy(context, inlist);
        ppllabel_list_copy(
                context, inlist,
                &context->set
                         ->ppllabel_list_default); // Check to see whether we are unsetting ALL arrows, and if so, use the copy command
    } else {
        // Otherwise restore the default definition of each listed label. Each item in the parser's list holds the
        // position of the next item, or zero at the end of the list.
        while (in->stk[pos].objType == PPLOBJ_NUM) {
            pos = (int) round(in->stk[pos].real);
            if (pos <= 0) break;
            const int i = (int) round(in->stk[pos + ptab[PARSE_INDEX_label_id]].real);

            // Look for the label in the list of defaults
            ppllabel_object *obj = context->set->ppllabel_list_default;
            while ((obj != NULL) && (obj->id < i)) obj = (obj->next);
            // If it has a default definition, insert a copy of it into the list in the right place
            if ((obj != NULL) && (obj->id == i)) {
                inlist = first;
                while ((*inlist != NULL) && ((*inlist)->id < i)) inlist = &((*inlist)->next);
                ppllabel_object *new = (ppllabel_object *) malloc(sizeof(ppllabel_object));
                // Failure to allocate memory is reported as an error
                if (new == NULL) {
                    ppl_error(&context->errcontext, ERR_MEMORY, -1, -1, "Out of memory");
                    return;
                }
                *new = *obj;
                new->next = *inlist;
                new->text = (char *) malloc(strlen(obj->text) + 1);
                // Failure to allocate memory is reported as an error
                if (new->text == NULL) {
                    ppl_error(&context->errcontext, ERR_MEMORY, -1, -1, "Out of memory");
                    free(new);
                    new = NULL;
                    return;
                }
                strcpy(new->text, obj->text);
                ppl_withWordsCpy(context, &new->style, &obj->style);
                *inlist = new;
            }
        }
    }
    return;
}

//! ppllabel_compare - Test whether two label definitions are identical
//! \param [in] context - The Pyxplot context
//! \param [in] a - The first label
//! \param [in] b - The second label
//! \return One if the labels are identical, zero otherwise

unsigned char ppllabel_compare(ppl_context *context, ppllabel_object *a, ppllabel_object *b) {
    if (a->id != b->id) return 0;
    if ((!sameValue(&a->x, &b->x)) || (!sameValue(&a->y, &b->y)) || (!sameValue(&a->z, &b->z))) return 0;
    if ((a->system_x != b->system_x) || (a->system_y != b->system_y) || (a->system_z != b->system_z)) return 0;
    if ((a->axis_x != b->axis_x) || (a->axis_y != b->axis_y) || (a->axis_z != b->axis_z)) return 0;
    if ((a->fontsizeSet != b->fontsizeSet) || ((a->fontsizeSet) && (a->fontsize != b->fontsize))) return 0;
    if (!ppl_withWordsCmp(context, &a->style, &b->style)) return 0;
    if (strcmp(a->text, b->text) != 0) return 0;
    return 1;
}

//! ppllabel_list_copy - Make a deep copy of a linked list of labels
//! \param [in] context - The Pyxplot context
//! \param [out] out - Pointer to the head of the new list
//! \param [in] in - Pointer to the head of the list to copy

void ppllabel_list_copy(ppl_context *context, ppllabel_object **out, ppllabel_object **in) {
    *out = NULL;
    while (*in != NULL) {
        *out = (ppllabel_object *) malloc(sizeof(ppllabel_object));
        if (*out == NULL) {
            ppl_error(&context->errcontext, ERR_MEMORY, -1, -1, "Out of memory");
            return;
        }
        **out = **in;
        (*out)->next = NULL;
        (*out)->text = (char *) malloc(strlen((*in)->text) + 1);
        // Failure to allocate memory is reported as an error
        if ((*out)->text == NULL) {
            ppl_error(&context->errcontext, ERR_MEMORY, -1, -1, "Out of memory");
            free(*out);
            *out = NULL;
            return;
        }
        strcpy((*out)->text, (*in)->text);
        ppl_withWordsCpy(context, &(*out)->style, &(*in)->style);
        in = &((*in)->next);
        out = &((*out)->next);
    }
    return;
}

//! ppllabel_list_destroy - Free a linked list of labels, including their text
//! \param [in] context - The Pyxplot context
//! \param [in|out] inlist - Pointer to the head of the list to free, which is set to NULL

void ppllabel_list_destroy(ppl_context *context, ppllabel_object **inlist) {

    // Free each label in turn, with its style and text
    ppllabel_object **first = inlist;
    // Unlink the first label, and free it
    while (*inlist != NULL) {
        ppllabel_object *obj = *inlist;
        *inlist = (*inlist)->next;
        ppl_withWordsDestroy(context, &obj->style);
        free(obj->text);
        obj->text = NULL;
        free(obj);
        obj = NULL;
    }
    *first = NULL;
    return;
}

//! ppllabel_print - Write a textual description of a label, in the syntax of the "set label" command
//! \param [in] context - The Pyxplot context
//! \param [in] in - The label to describe
//! \param [out] out - Character buffer into which to write the description
//! \param [in] outLen - The size of the buffer <out>

void ppllabel_print(ppl_context *context, ppllabel_object *in, char *out, const size_t outLen) {
    // Describe the label's text, and the coordinate system, axis number (if any) and value of each coordinate
    ppl_strEscapify(in->text, out, outLen);
    int i = strlen(out);
    ppl_strAppendf(out, outLen, &i, " at %s",
                   *(char **) ppl_fetchSettingName(&context->errcontext, in->system_x, SW_SYSTEM_INT,
                                                   (void *) SW_SYSTEM_STR, sizeof(char *)));
    // Coordinates given against a numbered axis include the axis number
    if (in->system_x == SW_SYSTEM_AXISN) {
        ppl_strAppendf(out, outLen, &i, " %d", in->axis_x);
    }
    ppl_strAppendf(out, outLen, &i, " %s,", ppl_unitsNumericDisplay(context, &(in->x), 0, 0, 0));
    ppl_strAppendf(out, outLen, &i, " %s",
                   *(char **) ppl_fetchSettingName(&context->errcontext, in->system_y, SW_SYSTEM_INT,
                                                   (void *) SW_SYSTEM_STR, sizeof(char *)));
    // Coordinates given against a numbered axis include the axis number
    if (in->system_y == SW_SYSTEM_AXISN) {
        ppl_strAppendf(out, outLen, &i, " %d", in->axis_y);
    }
    ppl_strAppendf(out, outLen, &i, " %s,", ppl_unitsNumericDisplay(context, &(in->y), 0, 0, 0));
    ppl_strAppendf(out, outLen, &i, " %s",
                   *(char **) ppl_fetchSettingName(&context->errcontext, in->system_z, SW_SYSTEM_INT,
                                                   (void *) SW_SYSTEM_STR, sizeof(char *)));
    // Coordinates given against a numbered axis include the axis number
    if (in->system_z == SW_SYSTEM_AXISN) {
        ppl_strAppendf(out, outLen, &i, " %d", in->axis_z);
    }
    ppl_strAppendf(out, outLen, &i, " %s", ppl_unitsNumericDisplay(context, &(in->z), 0, 0, 0));
    // Rotation, stored in radians, is shown in degrees
    if (in->rotation != 0.0) {
        ppl_strAppendf(out, outLen, &i, " rotate %s",
                       ppl_numericDisplay(in->rotation * 180 / M_PI, context->numdispBuff[0],
                                          context->set->term_current.SignificantFigures,
                                          (context->set->term_current.NumDisplay == SW_DISPLAY_L)));
    }
    // Horizontal alignment, if set
    if (in->HAlign > 0) {
        ppl_strAppendf(out, outLen, &i, " halign %s",
                       *(char **) ppl_fetchSettingName(&context->errcontext, in->HAlign, SW_HALIGN_INT,
                                                       (void *) SW_HALIGN_STR, sizeof(char *)));
    }
    // Vertical alignment, if set
    if (in->VAlign > 0) {
        ppl_strAppendf(out, outLen, &i, " valign %s",
                       *(char **) ppl_fetchSettingName(&context->errcontext, in->VAlign, SW_VALIGN_INT,
                                                       (void *) SW_VALIGN_STR, sizeof(char *)));
    }
    // The gap, stored in metres, is shown in centimetres
    if (in->gap != 0.0) {
        ppl_strAppendf(out, outLen, &i, " gap %s",
                       ppl_numericDisplay(in->gap * 100, context->numdispBuff[0],
                                          context->set->term_current.SignificantFigures,
                                          (context->set->term_current.NumDisplay == SW_DISPLAY_L)));
    }
    {
        char words[LSTR_LENGTH];
        ppl_withWordsPrint(context, &in->style, words, sizeof(words));
        if ((words[0] != '\0') || in->fontsizeSet) ppl_strAppendf(out, outLen, &i, " with %s", words);
    }
    i += strlen(out + i);
    // The font size, if set
    if (in->fontsizeSet) {
        ppl_strAppendf(out, outLen, &i, " fontsize %s",
                       ppl_numericDisplay(in->fontsize, context->numdispBuff[0],
                                          context->set->term_current.SignificantFigures,
                                          (context->set->term_current.NumDisplay == SW_DISPLAY_L)));
    }
    return;
}
