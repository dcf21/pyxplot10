// arrows.c
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

// Stores the arrows which users add to graphs with the "set arrow" command: adding, removing and resetting them,
// comparing and copying lists of them, and describing them for the show and save commands. The arrows are drawn by
// epsMaker/eps_plot_labelsarrows.c. Some of these helpers, which read coordinates and their coordinate systems, are
// shared with labels.c.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <gsl/gsl_math.h>

#include "coreUtils/dict.h"
#include "coreUtils/list.h"
#include "coreUtils/errorReport.h"
#include "expressions/traceback_fns.h"
#include "parser/cmdList.h"
#include "settings/arrows_fns.h"
#include "settings/settingTypes.h"
#include "settings/withWords_fns.h"
#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"
#include "userspace/context.h"
#include "userspace/pplObj_fns.h"
#include "userspace/unitsArithmetic.h"
#include "pplConstants.h"

// -------------------------------------------
// ROUTINES FOR MANIPULATING ARROWS STRUCTURES
// -------------------------------------------


//! sameValue - Test whether two numerical values are equal, including having the same physical dimensions
//! \param [in] a - The first value
//! \param [in] b - The second value
//! \return True if the values are equal

static int sameValue(const pplObj *a, const pplObj *b) {
    return ppl_unitsDimEqual(a, b) && ppl_dblEqual(a->real, b->real);
}


//! pplarrow_getSystem - Read which coordinate system a coordinate of an arrow or label is specified in
//! \param [in] context - The Pyxplot context
//! \param [in] in - The parser output for the command
//! \param [in] ptab - The table of the positions of the command's parameters in <in>
//! \param [in] index - The index of the parameter giving the coordinate system, e.g. PARSE_INDEX_x0_system
//! \return The coordinate system, one of the SW_SYSTEM_* constants

int pplarrow_getSystem(ppl_context *context, const parserOutput *in, const int *ptab, const int index) {
    const pplObj *o = &in->stk[ptab[index]];
    if (o->objType != PPLOBJ_STR) return SW_SYSTEM_FIRST;
    return ppl_fetchSettingByName(&context->errcontext, (char *) o->auxil, SW_SYSTEM_INT, SW_SYSTEM_STR);
}

//! pplarrow_getAxis - Read the number of the axis against which a coordinate of an arrow or label is specified
//! \param [in] in - The parser output for the command
//! \param [in] ptab - The table of the positions of the command's parameters in <in>
//! \param [in] index - The index of the parameter giving the axis number, e.g. PARSE_INDEX_x0_axis
//! \return The axis number, or zero if none was given

int pplarrow_getAxis(const parserOutput *in, const int *ptab, const int index) {
    const pplObj *o = &in->stk[ptab[index]];
    if (o->objType != PPLOBJ_NUM) return 0;
    return (int) round(o->real);
}

//! pplarrow_checkAxis - Check that the axis number given for a coordinate of an arrow or label is in range
//! \param [in] context - The Pyxplot context
//! \param [in] in - The parser output for the command
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] ptab - The table of the positions of the command's parameters in <in>
//! \param [in] index - The index of the parameter giving the axis number, e.g. PARSE_INDEX_x0_axis
//! \return Zero if the axis number is valid (or not given), or nonzero if an error was reported

int pplarrow_checkAxis(ppl_context *context, const parserOutput *in, const parserLine *pl, const int *ptab,
                       const int index) {
    const pplObj *o = &in->stk[ptab[index]];
    // An axis number was given: check that it is in range
    if (o->objType == PPLOBJ_NUM) {
        const int i = (int) round(o->real);
        // Report an axis number out of range
        if ((i < 0) || (i >= MAX_AXES)) {
            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                              "Axis number %d is out of range; axis numbers must be in the range 0 - %d", i,
                              MAX_AXES - 1);
            ppl_tbAddLine(context, pl, ERR_RANGE, in->stkCharPos[ptab[index]], "");
            return 1;
        }
    }
    return 0;
}

//! pplarrow_checkDimensions - Check that a coordinate of an arrow or label is finite, and that, if it is specified in
//! the graph or page coordinate systems, it is either dimensionless or has dimensions of length. Any problems are
//! added to the traceback.
//! \param [in] context - The Pyxplot context
//! \param [in] in - The parser output for the command
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] ptab - The table of the positions of the command's parameters in <in>
//! \param [in] index - The index of the parameter giving the coordinate, e.g. PARSE_INDEX_x0
//! \param [in] system - The coordinate system of the coordinate, one of the SW_SYSTEM_* constants

void pplarrow_checkDimensions(ppl_context *context, const parserOutput *in, const parserLine *pl, const int *ptab,
                              const int index, const int system) {
    const pplObj *o = &in->stk[ptab[index]];
    int i;
    if (o->objType != PPLOBJ_NUM) return;
    if (((system == SW_SYSTEM_GRAPH) || (system == SW_SYSTEM_PAGE)) && (!o->dimensionless))
        for (i = 0; i < UNITS_MAX_BASEUNITS; i++)
            // Dimensional coordinates must have units of length: the exponent of every base unit must be zero, except
            // length, which must be one
            if (o->exponent[i] != (i == UNIT_LENGTH)) {
                ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                  "Coordinates specified in the graph and page systems must have dimensions of "
                                  "length. Received coordinate with dimensions of <%s>.",
                                  ppl_printUnit(context, o, NULL, NULL, 0, 1, 0));
                ppl_tbAddLine(context, pl, ERR_UNIT, in->stkCharPos[ptab[index]], "");
            }
    // Coordinates must be finite
    if (!gsl_finite(o->real)) {
        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                          "Coordinates specified are not finite.");
        ppl_tbAddLine(context, pl, ERR_NUMERICAL, in->stkCharPos[ptab[index]], "");
    }
}

//! pplarrow_copyCoordinate - Copy a coordinate of an arrow or label out of the parser output. Dimensionless
//! coordinates in the graph and page systems are taken to be in centimetres, and are converted to metres.
//! \param [in,out] in - The parser output for the command; the coordinate is converted in place
//! \param [in] ptab - The table of the positions of the command's parameters in <in>
//! \param [in] index - The index of the parameter giving the coordinate, e.g. PARSE_INDEX_x0
//! \param [in] system - The coordinate system of the coordinate, one of the SW_SYSTEM_* constants
//! \param [out] out - The coordinate, which is zero if none was given

void pplarrow_copyCoordinate(parserOutput *in, const int *ptab, const int index, const int system, pplObj *out) {
    pplObj tempobj;
    pplObj *o = &in->stk[ptab[index]];
    // Coordinates which were not given are zero
    if (o->objType != PPLOBJ_NUM) {
        o = &tempobj;
        pplObjNum(&tempobj, 0, 0, 0);
    }
    // Dimensionless coordinates in the graph and page systems are in centimetres; convert them to metres
    if (((system == SW_SYSTEM_GRAPH) || (system == SW_SYSTEM_PAGE)) && (o->dimensionless)) {
        o->dimensionless = 0;
        o->exponent[UNIT_LENGTH] = 1;
        o->real /= 100;
    }
    *out = *o;
}

//! pplarrow_add - Implement the "set arrow" command, adding a new arrow to a list of arrows, or replacing an
//! existing arrow with the same ID number
//! \param [in] context - The Pyxplot context
//! \param [in|out] inlist - Pointer to the head of the linked list of arrows, sorted by ID number
//! \param [in] in - The parser output for the command, containing the arrow's ID, coordinates and style
//! \param [in] pl - The parsed command line, used to report errors
//! \param [in] ptab - Table mapping parser variable indices onto positions in <in->stk>

void pplarrow_add(ppl_context *context, pplarrow_object **inlist, parserOutput *in, parserLine *pl, const int *ptab) {
    // The coordinate system of each end of the arrow, and the record of the arrow
    pplarrow_object *out;

    // Read the coordinate system of each coordinate of the two ends of the arrow
    int system_x0 = pplarrow_getSystem(context, in, ptab, PARSE_INDEX_x0_system);
    int system_y0 = pplarrow_getSystem(context, in, ptab, PARSE_INDEX_y0_system);
    int system_z0 = pplarrow_getSystem(context, in, ptab, PARSE_INDEX_z0_system);
    int system_x1 = pplarrow_getSystem(context, in, ptab, PARSE_INDEX_x1_system);
    int system_y1 = pplarrow_getSystem(context, in, ptab, PARSE_INDEX_y1_system);
    int system_z1 = pplarrow_getSystem(context, in, ptab, PARSE_INDEX_z1_system);

    // Check the units of each coordinate
    pplarrow_checkDimensions(context, in, pl, ptab, PARSE_INDEX_x0, system_x0);
    pplarrow_checkDimensions(context, in, pl, ptab, PARSE_INDEX_y0, system_y0);
    pplarrow_checkDimensions(context, in, pl, ptab, PARSE_INDEX_z0, system_z0);
    pplarrow_checkDimensions(context, in, pl, ptab, PARSE_INDEX_x1, system_x1);
    pplarrow_checkDimensions(context, in, pl, ptab, PARSE_INDEX_y1, system_y1);
    pplarrow_checkDimensions(context, in, pl, ptab, PARSE_INDEX_z1, system_z1);

    // Check the axis number of each coordinate
    if (pplarrow_checkAxis(context, in, pl, ptab, PARSE_INDEX_x0_axis)) return;
    if (pplarrow_checkAxis(context, in, pl, ptab, PARSE_INDEX_y0_axis)) return;
    if (pplarrow_checkAxis(context, in, pl, ptab, PARSE_INDEX_z0_axis)) return;
    if (pplarrow_checkAxis(context, in, pl, ptab, PARSE_INDEX_x1_axis)) return;
    if (pplarrow_checkAxis(context, in, pl, ptab, PARSE_INDEX_y1_axis)) return;
    if (pplarrow_checkAxis(context, in, pl, ptab, PARSE_INDEX_z1_axis)) return;

    // Find the arrow with this ID in the list, which is sorted by ID, or the place where it belongs
    int i = (int) round(in->stk[ptab[PARSE_INDEX_arrow_id]].real);
    while ((*inlist != NULL) && ((*inlist)->id < i)) inlist = &((*inlist)->next);
    // Reuse an existing arrow with this ID, or insert a new one
    if ((*inlist != NULL) && ((*inlist)->id == i)) {
        out = *inlist;
        ppl_withWordsDestroy(context, &out->style);
    } else {
        out = (pplarrow_object *) malloc(sizeof(pplarrow_object));
        // Failure to allocate memory is reported as an error
        if (out == NULL) {
            ppl_error(&context->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            return;
        }
        out->id = i;
        out->next = *inlist;
        *inlist = out;
    }

    // Check whether arrow head type has been specified
    char *tempstr = (char *) in->stk[ptab[PARSE_INDEX_arrow_style]].auxil;
    if (in->stk[ptab[PARSE_INDEX_arrow_style]].objType != PPLOBJ_STR) out->pplarrow_style = SW_ARROWTYPE_HEAD;
    else
        out->pplarrow_style = ppl_fetchSettingByName(&context->errcontext, tempstr, SW_ARROWTYPE_INT, SW_ARROWTYPE_STR);

    // Check what style keywords have been specified in the 'with' clause
    ppl_withWordsFromDict(context, in, pl, ptab, 0, &out->style);

    // Store the coordinate system, axis number and value of each coordinate
    out->system_x0 = system_x0;
    out->system_y0 = system_y0;
    out->system_z0 = system_z0;
    out->system_x1 = system_x1;
    out->system_y1 = system_y1;
    out->system_z1 = system_z1;

    // Store the axis number of each coordinate
    out->axis_x0 = pplarrow_getAxis(in, ptab, PARSE_INDEX_x0_axis);
    out->axis_y0 = pplarrow_getAxis(in, ptab, PARSE_INDEX_y0_axis);
    out->axis_z0 = pplarrow_getAxis(in, ptab, PARSE_INDEX_z0_axis);
    out->axis_x1 = pplarrow_getAxis(in, ptab, PARSE_INDEX_x1_axis);
    out->axis_y1 = pplarrow_getAxis(in, ptab, PARSE_INDEX_y1_axis);
    out->axis_z1 = pplarrow_getAxis(in, ptab, PARSE_INDEX_z1_axis);

    // Store the value of each coordinate
    pplarrow_copyCoordinate(in, ptab, PARSE_INDEX_x0, system_x0, &out->x0);
    pplarrow_copyCoordinate(in, ptab, PARSE_INDEX_y0, system_y0, &out->y0);
    pplarrow_copyCoordinate(in, ptab, PARSE_INDEX_z0, system_z0, &out->z0);
    pplarrow_copyCoordinate(in, ptab, PARSE_INDEX_x1, system_x1, &out->x1);
    pplarrow_copyCoordinate(in, ptab, PARSE_INDEX_y1, system_y1, &out->y1);
    pplarrow_copyCoordinate(in, ptab, PARSE_INDEX_z1, system_z1, &out->z1);
    return;
}

//! pplarrow_remove - Implement the "set noarrow" command, removing arrows from a list of arrows
//! \param [in] context - The Pyxplot context
//! \param [in|out] inlist - Pointer to the head of the linked list of arrows
//! \param [in] in - The parser output for the command, containing the list of arrow IDs to remove. If no IDs are
//! given, all arrows are removed.
//! \param [in] pl - The parsed command line
//! \param [in] ptab - Table mapping parser variable indices onto positions in <in->stk>
//! \param [in] quiet - If true, do not warn about arrow IDs which are not defined

void pplarrow_remove(ppl_context *context, pplarrow_object **inlist, parserOutput *in, const parserLine *pl,
                     const int *ptab, const int quiet) {
    char msg[PPL_MSG_LENGTH];

    // pos is the position in the parser's output stack of the list of arrow IDs
    pplarrow_object **first = inlist;
    int pos = ptab[PARSE_INDEX_0arrow_list];

    // With no IDs, remove every arrow
    if ((in->stk[pos].objType != PPLOBJ_NUM) || (in->stk[pos].real <= 0))
        pplarrow_list_destroy(context, inlist); // set noarrow with no number specified means all arrows deleted
    // Otherwise remove each listed arrow
    else {
        // Each item in the parser's list holds the position of the next item, or zero at the end of the list
        while (in->stk[pos].objType == PPLOBJ_NUM) {
            pos = (int) round(in->stk[pos].real);
            if (pos <= 0) break;
            int i = (int) round(in->stk[pos + ptab[PARSE_INDEX_arrow_id]].real);
            inlist = first;
            while ((*inlist != NULL) && ((*inlist)->id < i)) inlist = &((*inlist)->next);
            // Unlink and free the arrow, or warn that it does not exist
            if ((*inlist != NULL) && ((*inlist)->id == i)) {
                pplarrow_object *obj = *inlist;
                *inlist = (*inlist)->next;
                ppl_withWordsDestroy(context, &obj->style);
                free(obj);
                obj = NULL;
            } else if (!quiet) {
                ppl_snprintfTrunc(msg, sizeof(msg), "Arrow number %d is not defined", i);
                ppl_warning(&context->errcontext, ERR_GENERIC, msg);
            }
        }
    }
    return;
}

//! pplarrow_unset - Implement the "unset arrow" command, reverting arrows to their default definitions
//! \param [in] context - The Pyxplot context
//! \param [in|out] inlist - Pointer to the head of the linked list of arrows
//! \param [in] in - The parser output for the command, containing the list of arrow IDs to unset. If no IDs are
//! given, all arrows are reverted to their defaults.
//! \param [in] pl - The parsed command line
//! \param [in] ptab - Table mapping parser variable indices onto positions in <in->stk>

void pplarrow_unset(ppl_context *context, pplarrow_object **inlist, parserOutput *in, parserLine *pl, const int *ptab) {
    pplarrow_remove(context, inlist, in, pl, ptab, 1); // First of all, remove any arrows which we are unsetting
    pplarrow_object **first = inlist;
    // pos is the position in the parser's output stack of the list of arrow IDs
    int pos = ptab[PARSE_INDEX_0arrow_list];

    // With no IDs, replace all arrows with a copy of the default list
    if ((in->stk[pos].objType != PPLOBJ_NUM) || (in->stk[pos].real <= 0)) {
        pplarrow_list_destroy(context, inlist);
        pplarrow_list_copy(
                context, inlist,
                &context->set
                         ->pplarrow_list_default); // Check to see whether we are unsetting ALL arrows, and if so, use the copy command
    } else {
        // Otherwise restore the default definition of each listed arrow. Each item in the parser's list holds the
        // position of the next item, or zero at the end of the list.
        while (in->stk[pos].objType == PPLOBJ_NUM) {
            pos = (int) round(in->stk[pos].real);
            if (pos <= 0) break;
            int i = (int) round(in->stk[pos + ptab[PARSE_INDEX_arrow_id]].real);

            // Look for the arrow in the list of defaults
            pplarrow_object *obj = context->set->pplarrow_list_default;
            while ((obj != NULL) && (obj->id < i)) obj = (obj->next);
            // If it has a default definition, insert a copy of it into the list in the right place
            if ((obj != NULL) && (obj->id == i)) {
                inlist = first;
                while ((*inlist != NULL) && ((*inlist)->id < i)) inlist = &((*inlist)->next);
                pplarrow_object *new = (pplarrow_object *) malloc(sizeof(pplarrow_object));
                // Failure to allocate memory is reported as an error
                if (new == NULL) {
                    ppl_error(&context->errcontext, ERR_MEMORY, -1, -1, "Out of memory");
                    return;
                }
                *new = *obj;
                new->next = *inlist;
                ppl_withWordsCpy(context, &new->style, &obj->style);
                *inlist = new;
            }
        }
    }
    return;
}

//! pplarrow_compare - Test whether two arrow definitions are identical
//! \param [in] context - The Pyxplot context
//! \param [in] a - The first arrow
//! \param [in] b - The second arrow
//! \return One if the arrows are identical, zero otherwise

unsigned char pplarrow_compare(ppl_context *context, pplarrow_object *a, pplarrow_object *b) {
    if (a->id != b->id) return 0;
    if ((!sameValue(&a->x0, &b->x0)) || (!sameValue(&a->y0, &b->y0)) || (!sameValue(&a->z0, &b->z0))) return 0;
    if ((!sameValue(&a->x1, &b->x1)) || (!sameValue(&a->y1, &b->y1)) || (!sameValue(&a->z1, &b->z1))) return 0;
    if ((a->system_x0 != b->system_x0) || (a->system_y0 != b->system_y0) || (a->system_z0 != b->system_z0)) return 0;
    if ((a->system_x1 != b->system_x1) || (a->system_y1 != b->system_y1) || (a->system_z1 != b->system_z1)) return 0;
    if ((a->axis_x0 != b->axis_x0) || (a->axis_y0 != b->axis_y0) || (a->axis_z0 != b->axis_z0)) return 0;
    if ((a->axis_x1 != b->axis_x1) || (a->axis_y1 != b->axis_y1) || (a->axis_z1 != b->axis_z1)) return 0;
    if (!ppl_withWordsCmp(context, &a->style, &b->style)) return 0;
    return 1;
}

//! pplarrow_list_copy - Make a deep copy of a linked list of arrows
//! \param [in] context - The Pyxplot context
//! \param [out] out - Pointer to the head of the new list
//! \param [in] in - Pointer to the head of the list to copy

void pplarrow_list_copy(ppl_context *context, pplarrow_object **out, pplarrow_object **in) {
    *out = NULL;
    // Copy each arrow in turn, with its style, linking the copies together
    while (*in != NULL) {
        *out = (pplarrow_object *) malloc(sizeof(pplarrow_object));
        // Failure to allocate memory is reported as an error
        if (*out == NULL) {
            ppl_error(&context->errcontext, ERR_MEMORY, -1, -1, "Out of memory");
            return;
        }
        **out = **in;
        (*out)->next = NULL;
        ppl_withWordsCpy(context, &(*out)->style, &(*in)->style);
        in = &((*in)->next);
        out = &((*out)->next);
    }
    return;
}

//! pplarrow_list_destroy - Free a linked list of arrows
//! \param [in] context - The Pyxplot context
//! \param [in|out] inlist - Pointer to the head of the list to free, which is set to NULL

void pplarrow_list_destroy(ppl_context *context, pplarrow_object **inlist) {
    pplarrow_object *obj;

    // Free each arrow in turn, with its style
    pplarrow_object **first = inlist;
    // Unlink the first arrow, and free it
    while (*inlist != NULL) {
        obj = *inlist;
        *inlist = (*inlist)->next;
        ppl_withWordsDestroy(context, &obj->style);
        free(obj);
        obj = NULL;
    }
    *first = NULL;
    return;
}

//! pplarrow_print - Write a textual description of an arrow, in the syntax of the "set arrow" command
//! \param [in] context - The Pyxplot context
//! \param [in] in - The arrow to describe
//! \param [out] out - Character buffer into which to write the description
//! \param [in] outLen - The size of the buffer <out>

void pplarrow_print(ppl_context *context, pplarrow_object *in, char *out, const size_t outLen) {
    // Describe the start of the arrow: the coordinate system, axis number (if any) and value of each coordinate
    int i = 0;
    ppl_strAppendf(out, outLen, &i, "from %s",
                   *(char **) ppl_fetchSettingName(&context->errcontext, in->system_x0, SW_SYSTEM_INT,
                                                   (void *) SW_SYSTEM_STR, sizeof(char *)));
    i = strlen(out);
    // Coordinates given against a numbered axis include the axis number
    if (in->system_x0 == SW_SYSTEM_AXISN) {
        ppl_strAppendf(out, outLen, &i, " %d", in->axis_x0);
    }
    ppl_strAppendf(out, outLen, &i, " %s,", ppl_unitsNumericDisplay(context, &(in->x0), 0, 0, 0));
    ppl_strAppendf(out, outLen, &i, " %s",
                   *(char **) ppl_fetchSettingName(&context->errcontext, in->system_y0, SW_SYSTEM_INT,
                                                   (void *) SW_SYSTEM_STR, sizeof(char *)));
    // Coordinates given against a numbered axis include the axis number
    if (in->system_y0 == SW_SYSTEM_AXISN) {
        ppl_strAppendf(out, outLen, &i, " %d", in->axis_y0);
    }
    ppl_strAppendf(out, outLen, &i, " %s,", ppl_unitsNumericDisplay(context, &(in->y0), 0, 0, 0));
    ppl_strAppendf(out, outLen, &i, " %s",
                   *(char **) ppl_fetchSettingName(&context->errcontext, in->system_z0, SW_SYSTEM_INT,
                                                   (void *) SW_SYSTEM_STR, sizeof(char *)));
    // Coordinates given against a numbered axis include the axis number
    if (in->system_z0 == SW_SYSTEM_AXISN) {
        ppl_strAppendf(out, outLen, &i, " %d", in->axis_z0);
    }
    ppl_strAppendf(out, outLen, &i, " %s ", ppl_unitsNumericDisplay(context, &(in->z0), 0, 0, 0));
    ppl_strAppendf(out, outLen, &i, "to %s",
                   *(char **) ppl_fetchSettingName(&context->errcontext, in->system_x1, SW_SYSTEM_INT,
                                                   (void *) SW_SYSTEM_STR, sizeof(char *)));
    // Describe the end of the arrow in the same way. Coordinates given against a numbered axis include the axis number.
    if (in->system_x1 == SW_SYSTEM_AXISN) {
        ppl_strAppendf(out, outLen, &i, " %d", in->axis_x1);
    }
    ppl_strAppendf(out, outLen, &i, " %s,", ppl_unitsNumericDisplay(context, &(in->x1), 0, 0, 0));
    ppl_strAppendf(out, outLen, &i, " %s",
                   *(char **) ppl_fetchSettingName(&context->errcontext, in->system_y1, SW_SYSTEM_INT,
                                                   (void *) SW_SYSTEM_STR, sizeof(char *)));
    // Coordinates given against a numbered axis include the axis number
    if (in->system_y1 == SW_SYSTEM_AXISN) {
        ppl_strAppendf(out, outLen, &i, " %d", in->axis_y1);
    }
    ppl_strAppendf(out, outLen, &i, " %s,", ppl_unitsNumericDisplay(context, &(in->y1), 0, 0, 0));
    ppl_strAppendf(out, outLen, &i, " %s",
                   *(char **) ppl_fetchSettingName(&context->errcontext, in->system_z1, SW_SYSTEM_INT,
                                                   (void *) SW_SYSTEM_STR, sizeof(char *)));
    // Coordinates given against a numbered axis include the axis number. Then describe the arrow's head and style.
    if (in->system_z1 == SW_SYSTEM_AXISN) {
        ppl_strAppendf(out, outLen, &i, " %d", in->axis_z1);
    }
    ppl_strAppendf(out, outLen, &i, " %s", ppl_unitsNumericDisplay(context, &(in->z1), 0, 0, 0));
    ppl_strAppendf(out, outLen, &i, " with %s ",
                   *(char **) ppl_fetchSettingName(&context->errcontext, in->pplarrow_style, SW_ARROWTYPE_INT,
                                                   (void *) SW_ARROWTYPE_STR, sizeof(char *)));
    ppl_withWordsPrint(context, &in->style, out + i, outLen - i);
    return;
}
