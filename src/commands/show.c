// show.c
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

// Implements the show command, which lists the current values of settings in the syntax of the set commands which would
// set them, along with arrows, labels, line styles, variables, functions and units. Each setting is shown if its name,
// or a word such as settings, matches one of the words given. In interactive sessions, settings which differ from their
// defaults are highlighted in color.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <ctype.h>

#include "commands/show.h"
#include "coreUtils/errorReport.h"

#include "coreUtils/dict.h"
#include "coreUtils/memAlloc.h"
#include "coreUtils/list.h"

#include "expressions/traceback_fns.h"

#include "parser/cmdList.h"
#include "parser/parser.h"

#include "settings/arrows_fns.h"
#include "settings/axes_fns.h"
#include "settings/epsColors.h"
#include "settings/labels_fns.h"
#include "settings/settings.h"
#include "settings/settingTypes.h"
#include "settings/textConstants.h"
#include "settings/withWords_fns.h"

#include "stringTools/asciidouble.h"

#include "userspace/context.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjFunc.h"
#include "userspace/pplObjPrint.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"

#include "canvasItems.h"
#include "pplConstants.h"


//! showHighlight - On interactive sessions, start highlighting a setting, in one colour if it has been changed by
//! the user and in another if it still has its default value
//! \param [in] c - The Pyxplot context
//! \param [in,out] out - The buffer to which the show command's output is being written
//! \param [in] outLen - The size of the buffer <out>
//! \param [in,out] i - The position in <out> at which to write
//! \param [in] interactive - True if the session is interactive
//! \param [in] unchanged - True if the setting still has its default value

static void showHighlight(ppl_context *c, char *out, const size_t outLen, int *i, const int interactive,
                          const int unchanged) {
    const int colour =
            (unchanged == 0) ? c->errcontext.session_default.color_wrn : c->errcontext.session_default.color_rep;
    if (interactive == 0) return;
    ppl_strAppendf(
            out, outLen, i, "%s",
            *(char **) ppl_fetchSettingName(&c->errcontext, colour, SW_TERMCOL_INT, SW_TERMCOL_TXT, sizeof(char *)));
}

//! showDehighlight - On interactive sessions, stop highlighting a setting
//! \param [in] c - The Pyxplot context
//! \param [in,out] out - The buffer to which the show command's output is being written
//! \param [in] outLen - The size of the buffer <out>
//! \param [in,out] i - The position in <out> at which to write
//! \param [in] interactive - True if the session is interactive

static void showDehighlight(ppl_context *c, char *out, const size_t outLen, int *i, const int interactive) {
    if (interactive == 0) return;
    ppl_strAppendf(out, outLen, i, "%s",
                   *(char **) ppl_fetchSettingName(&c->errcontext, SW_TERMCOL_NOR, SW_TERMCOL_INT, SW_TERMCOL_TXT,
                                                   sizeof(char *)));
}

//! ppl_directive_show3 - Write the value of a single setting, in the syntax of the set command that would set it
//! \param [in] c - The Pyxplot context
//! \param [out] out - Character buffer into which to write the setting
//! \param [in] outLen - The size of the buffer <out>
//! \param [in] itemSet - The text inserted after "set" to identify the plot item the setting applies to, if any,
//! e.g. "item 2"
//! \param [in] itemSetShow - If true, include <itemSet> in the output; otherwise pad it with spaces
//! \param [in] interactive - Flag indicating whether the session is interactive, in which case settings which differ
//! from their defaults are highlighted
//! \param [in] setting_name - The name of the setting, as used in the set command
//! \param [in] setting_value - The value of the setting. "on" and "off" are written as "set name" and "set noname".
//! \param [in] modified - Flag indicating whether the setting differs from its default value
//! \param [in] description - A description of the setting, written as a comment (may be NULL)

static void ppl_directive_show3(ppl_context *c, char *out, const size_t outLen, char *itemSet,
                                unsigned char itemSetShow, const int interactive, char *setting_name,
                                char *setting_value, const int modified, char *description) {
    int i = 0;

    // Highlight settings which differ from their defaults
    showHighlight(c, out, outLen, &i, interactive, modified);

    // Start with the word set, followed by the item number if the setting applies to a multiplot item
    ppl_snprintfTrunc(out + i, outLen - i, "set %*s", (int) strlen(itemSet), itemSetShow ? itemSet : "");
    i += strlen(out + i); // Start off with a set command

    // Boolean settings are written as "set name" or "set noname"; others as "set name value"
    if (strcmp(setting_value, "on") == 0) {
        ppl_snprintfTrunc(out + i, outLen - i, "%-41s", setting_name);
        i += strlen(out + i);
    } else if (strcmp(setting_value, "off") == 0) {
        // Insert "no" after the last space in the setting name, and pad the result to 41 characters
        const char *lastSpace = strrchr(setting_name, ' ');
        const int k = (lastSpace == NULL) ? -1 : (int) (lastSpace - setting_name);
        ppl_snprintfTrunc(out + i, outLen - i, "%.*sno%-*s", k + 1, setting_name, 38 - k, setting_name + k + 1);
        i += strlen(out + i);
    } else {
        ppl_snprintfTrunc(out + i, outLen - i, "%-16s %-24s", setting_name, setting_value);
        i += strlen(out + i);
    }

    // Put a description of the setting after it, as a comment
    if (description != NULL) {
        ppl_snprintfTrunc(out + i, outLen - i, " # %s.", description);
        i += strlen(out + i);
    } // Finally put a decriptive comment after the setting
    ppl_snprintfTrunc(out + i, outLen - i, "%s", "\n");
    i += strlen(out + i); // and a linefeed

    // Return to the normal text colour
    showDehighlight(c, out, outLen, &i, interactive);
    return;
}

//! showColorSetting - The value of a colour setting, which is either a named colour, or given by its components
typedef struct showColorSetting {
    int color; // A named colour (COLOR_*), or zero if the colour is given by its components
    int space; // The colour space of the components (SW_COLSPACE_*)
    double c1, c2, c3, c4; // The components
} showColorSetting;

//! showColor - Write a line of the output of the show command describing a colour setting
//! \param [in] c - The Pyxplot context
//! \param [out] out - The buffer into which to write the line
//! \param [in] outLen - The size of the buffer <out>
//! \param [in] itemSet - The text inserted after "set" to identify the plot item the setting applies to, if any
//! \param [in] interactive - True if the session is interactive
//! \param [out] buf - A buffer of LSTR_LENGTH bytes to use as workspace
//! \param [in] cur - The current value of the setting
//! \param [in] def - The default value of the setting
//! \param [in] name - The name of the setting
//! \param [in] description - A description of the setting

static void showColor(ppl_context *c, char *out, const size_t outLen, char *itemSet, const int interactive, char *buf,
                      const showColorSetting cur, const showColorSetting def, char *name, char *description) {
    if (cur.color > 0)
        ppl_snprintfTrunc(
                buf, LSTR_LENGTH, "%s",
                *(char **) ppl_fetchSettingName(&c->errcontext, cur.color, SW_COLOR_INT, SW_COLOR_STR, sizeof(char *)));
    else if (cur.space == SW_COLSPACE_RGB)
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "rgb(%s,%s,%s)", ppl_numericDisplayDefault(c, cur.c1, 0),
                          ppl_numericDisplayDefault(c, cur.c2, 1), ppl_numericDisplayDefault(c, cur.c3, 2));
    else if (cur.space == SW_COLSPACE_HSB)
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "hsb(%s,%s,%s)", ppl_numericDisplayDefault(c, cur.c1, 0),
                          ppl_numericDisplayDefault(c, cur.c2, 1), ppl_numericDisplayDefault(c, cur.c3, 2));
    else if (cur.space == SW_COLSPACE_CMYK)
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "cmyk(%s,%s,%s,%s)", ppl_numericDisplayDefault(c, cur.c1, 0),
                          ppl_numericDisplayDefault(c, cur.c2, 1), ppl_numericDisplayDefault(c, cur.c3, 2),
                          ppl_numericDisplayDefault(c, cur.c4, 3));
    ppl_directive_show3(c, out, outLen, itemSet, 1, interactive, name, buf,
                        (cur.color == def.color) && (cur.space == def.space) && (cur.c1 == def.c1) &&
                                (cur.c2 == def.c2) && (cur.c3 == def.c3) && (cur.c4 == def.c4),
                        description);
}

//! showTicks - Write a line of the output of the show command describing the tick settings of an axis
//! \param [in] c - The Pyxplot context
//! \param [out] out - The buffer into which to write the line
//! \param [in] outLen - The size of the buffer <out>
//! \param [in] itemSet - The text inserted after "set" to identify the plot item the setting applies to, if any
//! \param [in] interactive - True if the session is interactive
//! \param [in] name - The name of the setting, e.g. x1tics
//! \param [out] buf - A buffer of LSTR_LENGTH bytes to use as workspace
//! \param [out] buf2 - A buffer of FNAME_LENGTH bytes to use as workspace
//! \param [in] T - The tick settings
//! \param [in] TD - The default tick settings
//! \param [in,out] U - The physical unit of the axis; its value is overwritten while formatting tick positions
//! \param [in] UD - The default physical unit of the axis
//! \param [in] amLog - Whether the axis is logarithmic
//! \param [in] amLogD - Whether the axis is logarithmic by default
//! \param [in] axisLetter - The letter of the axis, e.g. 'x'
//! \param [in] axisNumber - The number of the axis, e.g. 1

static void showTicks(ppl_context *c, char *out, const size_t outLen, char *itemSet, const int interactive, char *name,
                      char *buf, char *buf2, const pplset_tics *T, const pplset_tics *TD, pplObj *U, const pplObj *UD,
                      const int amLog, const int amLogD, const char axisLetter, const int axisNumber) {
    ppl_snprintfTrunc(buf2, FNAME_LENGTH, "Sets where the %s ticks are placed along the %c%d axis, and how they appear",
                      (name[0] == 'm') ? "minor" : "major", axisLetter, axisNumber);
    ppl_snprintfTrunc(
            buf, LSTR_LENGTH, "%s ",
            *(char **) ppl_fetchSettingName(&c->errcontext, T->tickDir, SW_TICDIR_INT, SW_TICDIR_STR, sizeof(char *)));
    int m = strlen(buf);
    // Write the tick positions: automatic, at regular intervals, or as a list
    if ((!T->tickStepSet) && (T->tickList == NULL)) {
        ppl_strAppendf(buf, LSTR_LENGTH, &m, "autofreq");
    } else if (T->tickList == NULL) {
        // Ticks at regular intervals: min, step, max
        if (T->tickMinSet) {
            U->real = T->tickMin;
            ppl_strAppendf(buf, LSTR_LENGTH, &m, "%s", ppl_unitsNumericDisplay(c, U, 0, 0, 0));
        }
        // The step between ticks; on logarithmic axes, this is a dimensionless factor
        if (T->tickStepSet) {
            U->real = T->tickStep;
            ppl_strAppendf(buf, LSTR_LENGTH, &m, "%s%s", (T->tickMinSet) ? ", " : "",
                           (amLog == SW_BOOL_FALSE) ? ppl_unitsNumericDisplay(c, U, 0, 0, 0)
                                                    : ppl_numericDisplayDefault(c, T->tickStep, 0));
        }
        // The position of the last tick
        if (T->tickMaxSet) {
            U->real = T->tickMax;
            ppl_strAppendf(buf, LSTR_LENGTH, &m, ", %s", ppl_unitsNumericDisplay(c, U, 0, 0, 0));
        }
    } else {
        // A list of tick positions, each optionally with a label
        ppl_strAppendChar(buf, LSTR_LENGTH, &m, '(');
        // Write each tick position
        for (int n = 0; T->tickStrs[n] != NULL; n++) {
            U->real = T->tickList[n];
            ppl_strAppendf(buf, LSTR_LENGTH, &m, "%s%s", (n == 0) ? "" : ", ", ppl_unitsNumericDisplay(c, U, 0, 0, 0));
            // A tick label is marked as absent by a leading byte 0xFF
            if (T->tickStrs[n][0] != '\xFF') {
                ppl_strAppendChar(buf, LSTR_LENGTH, &m, ' ');
                ppl_strEscapify(T->tickStrs[n], buf + m, LSTR_LENGTH - m);
                m += strlen(buf + m);
            }
        }
        ppl_strAppendf(buf, LSTR_LENGTH, &m, ")");
    }
    ppl_directive_show3(c, out, outLen, itemSet, 1, interactive, name, buf,
                        (T->tickDir == TD->tickDir) && (pplaxis_cmpTics(c, T, TD, U, UD, amLog, amLogD)), buf2);
}

//! showColorRange - Write a line of the output of the show command describing the range of a colour axis
//! \param [in] c - The Pyxplot context
//! \param [out] out - The buffer into which to write the line
//! \param [in] outLen - The size of the buffer <out>
//! \param [in] itemSet - The text inserted after "set" to identify the plot item the setting applies to, if any
//! \param [in] interactive - True if the session is interactive
//! \param [out] buf - A buffer of LSTR_LENGTH bytes to use as workspace
//! \param [in] sg - The graph settings
//! \param [in] C - The index of the colour axis (0-3, for c1 to c4)

static void showColorRange(ppl_context *c, char *out, const size_t outLen, char *itemSet, const int interactive,
                           char *buf, const pplset_graph *sg, const int C) {
    const pplset_graph *def = &c->set->graph_default;
    char name[16];
    ppl_snprintfTrunc(name, sizeof(name), "c%drange", C + 1);
    ppl_snprintfTrunc(
            buf, LSTR_LENGTH, "[%s:%s] %s %s",
            (sg->Cminauto[C] == SW_BOOL_TRUE) ? "*" : ppl_unitsNumericDisplay(c, (pplObj *) &(sg->Cmin[C]), 0, 0, 0),
            (sg->Cmaxauto[C] == SW_BOOL_TRUE) ? "*" : ppl_unitsNumericDisplay(c, (pplObj *) &(sg->Cmax[C]), 1, 0, 0),
            (sg->Crenorm[C] == SW_BOOL_TRUE) ? "renormalise" : "norenormalise",
            (sg->Creverse[C] == SW_BOOL_TRUE) ? "reverse" : "noreverse");
    ppl_directive_show3(
            c, out, outLen, itemSet, 1, interactive, name, buf,
            (def->Cminauto[C] == sg->Cminauto[C]) && (def->Cmaxauto[C] == sg->Cmaxauto[C]) &&
                    ((sg->Cminauto[C] == SW_BOOL_TRUE) ||
                     ((def->Cmin[C].real == sg->Cmin[C].real) && ppl_unitsDimEqual(&(def->Cmin[C]), &(sg->Cmin[C])))) &&
                    ((sg->Cmaxauto[C] == SW_BOOL_TRUE) ||
                     ((def->Cmax[C].real == sg->Cmax[C].real) && ppl_unitsDimEqual(&(def->Cmax[C]), &(sg->Cmax[C])))) &&
                    (def->Crenorm[C] == sg->Crenorm[C]) && (def->Creverse[C] == sg->Creverse[C]),
            "The range of values represented by different colors in the colormap plot style, and by "
            "contours in the contourmap plot style");
}

//! ppl_directive_show2 - Display the settings matching one word of a show command, e.g. "settings" or "xrange"
//! \param [in] c - The Pyxplot context
//! \param [in] word - The word of the show command, which may be an abbreviation of a setting name
//! \param [in] itemSet - The text inserted after "set" to identify the plot item the settings apply to, if any
//! \param [in] interactive - Flag indicating whether the session is interactive
//! \param [in] sg - The graph settings to display
//! \param [in] al - The list of arrows to display
//! \param [in] ll - The list of labels to display
//! \param [in] xa - The array of x axes to display
//! \param [in] ya - The array of y axes to display
//! \param [in] za - The array of z axes to display
//! \return One if any settings were displayed, zero if none matched, or one on memory allocation failure

static int ppl_directive_show2(ppl_context *c, const char *word, char *itemSet, const int interactive, pplset_graph *sg,
                               pplarrow_object **al, ppllabel_object **ll, pplset_axis *xa, pplset_axis *ya,
                               pplset_axis *za) {
    char *bufp, *bufp2, temp1[32], temp2[32];
    int i = 0, p = 0, k, l, m;
    unsigned char unchanged;
    unit *ud = c->unit_database;
    const int outLen = 8 * LSTR_LENGTH;

    char *out = (char *) malloc(outLen); // Accumulate our whole output text here
    char *buf = (char *) malloc(LSTR_LENGTH); // Put the value of each setting in here
    char *buf2 = (char *) malloc(FNAME_LENGTH);

    // Report a failure to allocate the buffers, and free those that were allocated
    if ((out == NULL) || (buf == NULL) || (buf2 == NULL)) {
        ppl_error(&c->errcontext, ERR_MEMORY, -1, -1,
                  "Out of memory whilst trying to allocate buffers in show command.");
        // Free the output buffer
        free(out);
        out = NULL;
        // Free the value buffer
        free(buf);
        buf = NULL;
        // Free the description buffer
        free(buf2);
        buf2 = NULL;
        return 1;
    }

    // Each setting is shown if its name, or "settings", matches the word given; buffers accumulate the output and the
    // value of each setting
    out[0] = buf[0] = '\0';
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "axescolour", 1) >= 0) ||
        (ppl_strAutocomplete(word, "axescolor", 1) >= 0)) {
        showColor(c, out + i, outLen - i, itemSet, interactive, buf,
                  (showColorSetting) {sg->AxesColor, sg->AxesCol1234Space, sg->AxesColor1, sg->AxesColor2,
                                      sg->AxesColor3, sg->AxesColor4},
                  (showColorSetting) {c->set->graph_default.AxesColor, c->set->graph_default.AxesCol1234Space,
                                      c->set->graph_default.AxesColor1, c->set->graph_default.AxesColor2,
                                      c->set->graph_default.AxesColor3, c->set->graph_default.AxesColor4},
                  "axesColor", "The color used to draw graph axes");
        i += strlen(out + i);
        p = 1;
    }
    // Show the axisunitstyle setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "axisunitstyle", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, sg->AxisUnitStyle, SW_AXISUNITSTY_INT,
                                                          SW_AXISUNITSTY_STR, sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "axisUnitStyle", buf,
                            (c->set->graph_default.AxisUnitStyle == sg->AxisUnitStyle),
                            "Select how the physical units associated with axes are appended to axis labels");
        i += strlen(out + i);
        p = 1;
    }
    // Show the backup setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "backup", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.backup, SW_ONOFF_INT,
                                                          SW_ONOFF_STR, sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "backup", buf,
                            (c->set->term_default.backup == c->set->term_current.backup),
                            "Selects whether existing files are overwritten (nobackup) or moved (backup)");
        i += strlen(out + i);
        p = 1;
    }
    // Show the bar setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "bar", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s", ppl_numericDisplayDefault(c, sg->bar, 0));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "bar", buf,
                            (c->set->graph_default.bar == sg->bar),
                            "Sets the size of the strokes which mark the lower and upper limits of errorbars");
        i += strlen(out + i);
        p = 1;
    }
    // Show the binorigin setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "binorigin", 1) >= 0)) {
        bufp = "Used to control the exact position of the edges of the bins used by the histogram command";
        // An automatic bin origin, or a value
        if (c->set->term_current.BinOriginAuto) {
            ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "binOrigin", "auto",
                                c->set->term_current.BinOriginAuto == c->set->term_default.BinOriginAuto, bufp);
        } else {
            ppl_directive_show3(
                    c, out + i, outLen - i, itemSet, 0, interactive, "binOrigin",
                    ppl_unitsNumericDisplay(c, &(c->set->term_current.BinOrigin), 0, 0, 0),
                    (c->set->term_current.BinOriginAuto == c->set->term_default.BinOriginAuto) &&
                            ppl_dblEqual(c->set->term_default.BinOrigin.real, c->set->term_current.BinOrigin.real) &&
                            ppl_dblEqual(c->set->term_default.BinOrigin.imag, c->set->term_current.BinOrigin.imag) &&
                            ppl_unitsDimEqual(&c->set->term_default.BinOrigin, &c->set->term_current.BinOrigin),
                    bufp);
        }
        i += strlen(out + i);
        p = 1;
    }
    // Show the binwidth setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "binwidth", 1) >= 0)) {
        bufp = "Sets the width of bins used by the histogram command";
        // An automatic bin width, or a value
        if (c->set->term_current.BinWidthAuto) {
            ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "binWidth", "auto",
                                c->set->term_current.BinWidthAuto == c->set->term_default.BinWidthAuto, bufp);
        } else {
            ppl_directive_show3(
                    c, out + i, outLen - i, itemSet, 0, interactive, "binWidth",
                    ppl_unitsNumericDisplay(c, &(c->set->term_current.BinWidth), 0, 0, 0),
                    (c->set->term_current.BinWidthAuto == c->set->term_default.BinWidthAuto) &&
                            ppl_dblEqual(c->set->term_default.BinWidth.real, c->set->term_current.BinWidth.real) &&
                            ppl_dblEqual(c->set->term_default.BinWidth.imag, c->set->term_current.BinWidth.imag) &&
                            ppl_unitsDimEqual(&c->set->term_default.BinWidth, &c->set->term_current.BinWidth),
                    bufp);
        }
        i += strlen(out + i);
        p = 1;
    }
    // Show the boxfrom setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "boxfrom", 1) >= 0)) {
        bufp = "Sets the vertical level from which the bars of barcharts and histograms are drawn";
        // An automatic box origin, or a value
        if (sg->BoxFromAuto) {
            ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "boxFrom", "auto",
                                sg->BoxFromAuto == c->set->graph_default.BoxFromAuto, bufp);
        } else {
            ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "boxFrom",
                                ppl_unitsNumericDisplay(c, &(sg->BoxFrom), 0, 0, 0),
                                (sg->BoxFromAuto == c->set->graph_default.BoxFromAuto) &&
                                        ppl_dblEqual(c->set->graph_default.BoxFrom.real, sg->BoxFrom.real) &&
                                        ppl_dblEqual(c->set->graph_default.BoxFrom.imag, sg->BoxFrom.imag) &&
                                        ppl_unitsDimEqual(&c->set->graph_default.BoxFrom, &sg->BoxFrom),
                                bufp);
        }
        i += strlen(out + i);
        p = 1;
    }
    // Show the boxwidth setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "boxwidth", 1) >= 0)) {
        bufp = "Sets the width of bars on barcharts and histograms";
        // An automatic box width, or a value
        if (sg->BoxWidthAuto) {
            ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "boxWidth", "auto",
                                sg->BoxWidthAuto == c->set->graph_default.BoxWidthAuto, bufp);
        } else {
            ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "boxWidth",
                                ppl_unitsNumericDisplay(c, &(sg->BoxWidth), 0, 0, 0),
                                (sg->BoxWidthAuto == c->set->graph_default.BoxWidthAuto) &&
                                        ppl_dblEqual(c->set->graph_default.BoxWidth.real, sg->BoxWidth.real) &&
                                        ppl_dblEqual(c->set->graph_default.BoxWidth.imag, sg->BoxWidth.imag) &&
                                        ppl_unitsDimEqual(&c->set->graph_default.BoxWidth, &sg->BoxWidth),
                                bufp);
        }
        i += strlen(out + i);
        p = 1;
    }
    // Show the c1format setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "c1format", 1) >= 0)) {
        pplExpr *c1format_current = (pplExpr *) sg->c1format;
        const pplExpr *c1format_default = (pplExpr *) c->set->graph_default.c1format;
        if (sg->c1formatset) ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s ", c1format_current->ascii);
        else buf[0] = '\0';
        m = strlen(buf);
        ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, sg->c1TickLabelRotation, SW_TICLABDIR_INT,
                                                          SW_TICLABDIR_STR, sizeof(char *)));
        m += strlen(buf + m);
        // Rotated tick labels also have an angle
        if (sg->c1TickLabelRotation == SW_TICLABDIR_ROT) {
            pplObj valobj;
            valobj.refCount = 1;
            pplObjNum(&valobj, 0, sg->c1TickLabelRotate, 0);
            valobj.exponent[UNIT_ANGLE] = 1;
            valobj.dimensionless = 0;
            ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, " %s", ppl_unitsNumericDisplay(c, &valobj, 0, 0, 0));
        }
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "c1format", buf,
                            ((sg->c1TickLabelRotate == c->set->graph_default.c1TickLabelRotate) &&
                             (sg->c1TickLabelRotation == c->set->graph_default.c1TickLabelRotation) &&
                             (sg->c1formatset == c->set->graph_default.c1formatset) &&
                             ((!sg->c1formatset) || (strcmp(c1format_current->ascii, c1format_default->ascii) == 0))),
                            "Format string for the tick labels on the c1 axis");
        i += strlen(out + i);
        p = 1;
    }
    // Show the c1label setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "c1label", 1) >= 0)) {
        pplObj valobj;
        valobj.refCount = 1;
        ppl_strEscapify(sg->c1label, buf, LSTR_LENGTH);
        m = strlen(buf);
        pplObjNum(&valobj, 0, sg->c1LabelRotate, 0);
        valobj.exponent[UNIT_ANGLE] = 1;
        valobj.dimensionless = 0;
        ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, " rotate %s", ppl_unitsNumericDisplay(c, &valobj, 0, 0, 0));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "c1label", buf,
                            ((sg->c1LabelRotate == c->set->graph_default.c1LabelRotate) &&
                             (strcmp(sg->c1label, c->set->graph_default.c1label) == 0)),
                            "Textual label for the c1 axis");
        i += strlen(out + i);
        p = 1;
    }
    {
        int showMinor = 0;
        // Show the c1tics setting, if it, or all settings, were asked for
        if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "c1tics", 1) >= 0)) {
            ppl_snprintfTrunc(temp1, sizeof(temp1), "c1tics");
            showTicks(c, out + i, outLen - i, itemSet, interactive, temp1, buf, buf2, &(sg->ticsC),
                      &(c->set->graph_default.ticsC), &(sg->unitC), &(c->set->graph_default.unitC), sg->Clog[0],
                      c->set->graph_default.Clog[0], 'c', 1);
            i += strlen(out + i);
            p = 1;
            showMinor = 1; // If we've shown major tics, also show minor ticks too.
        }
        if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "mc1tics", 1) >= 0) ||
            showMinor) {
            ppl_snprintfTrunc(temp1, sizeof(temp1), "mc1tics");
            showTicks(c, out + i, outLen - i, itemSet, interactive, temp1, buf, buf2, &(sg->ticsCM),
                      &(c->set->graph_default.ticsCM), &(sg->unitC), &(c->set->graph_default.unitC), sg->Clog[0],
                      c->set->graph_default.Clog[0], 'c', 1);
            i += strlen(out + i);
            p = 1;
        }
    }
    // Show the calendarin setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "calendarin", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.CalendarIn,
                                                          SW_CALENDAR_INT, SW_CALENDAR_STR, sizeof(char *)));
        ppl_directive_show3(
                c, out + i, outLen - i, itemSet, 0, interactive, "calendarIn", buf,
                (c->set->term_current.CalendarIn == c->set->term_default.CalendarIn),
                "Selects the historical year in which the transition is made between Julian and Gregorian calendars when dates are being input");
        i += strlen(out + i);
        p = 1;
    }
    // Show the calendarout setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "calendarout", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.CalendarOut,
                                                          SW_CALENDAR_INT, SW_CALENDAR_STR, sizeof(char *)));
        ppl_directive_show3(
                c, out + i, outLen - i, itemSet, 0, interactive, "calendarOut", buf,
                (c->set->term_current.CalendarOut == c->set->term_default.CalendarOut),
                "Selects the historical year in which the transition is made between Julian and Gregorian calendars when displaying dates");
        i += strlen(out + i);
        p = 1;
    }
    // Show the clip setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "clip", 1) >= 0)) {
        ppl_snprintfTrunc(
                buf, LSTR_LENGTH, "%s",
                *(char **) ppl_fetchSettingName(&c->errcontext, sg->clip, SW_ONOFF_INT, SW_ONOFF_STR, sizeof(char *)));
        ppl_directive_show3(
                c, out + i, outLen - i, itemSet, 0, interactive, "clip", buf, (sg->clip == c->set->graph_default.clip),
                "Selects whether point symbols which extend over the axes of graphs are allowed to do so, or are clipped at the edges");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "colkey", 1) >= 0) ||
        (ppl_strAutocomplete(word, "colourkey", 1) >= 0) || (ppl_strAutocomplete(word, "colorkey", 1) >= 0)) {
        // The color key is off, or on in a given position
        if (sg->ColKey == SW_ONOFF_OFF) {
            ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                              *(char **) ppl_fetchSettingName(&c->errcontext, sg->ColKey, SW_ONOFF_INT, SW_ONOFF_STR,
                                                              sizeof(char *)));
        } else {
            ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                              *(char **) ppl_fetchSettingName(&c->errcontext, sg->ColKeyPos, SW_COLKEYPOS_INT,
                                                              SW_COLKEYPOS_STR, sizeof(char *)));
        }
        ppl_directive_show3(
                c, out + i, outLen - i, itemSet, 1, interactive, "colkey", buf,
                (c->set->graph_default.ColKey == sg->ColKey) &&
                        ((sg->ColKey == SW_ONOFF_OFF) || (c->set->graph_default.ColKeyPos == sg->ColKeyPos)),
                "Selects whether a color scale is included on colormap plots");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "colmap", 1) >= 0) ||
        (ppl_strAutocomplete(word, "colourmap", 1) >= 0) || (ppl_strAutocomplete(word, "colormap", 1) >= 0)) {
        const pplExpr *exp_default = (pplExpr *) c->set->graph_default.ColMapExpr;
        pplExpr *exp_current = (pplExpr *) sg->ColMapExpr;
        const pplExpr *maskdefault = (pplExpr *) c->set->graph_default.MaskExpr;
        pplExpr *maskcurrent = (pplExpr *) sg->MaskExpr;
        const int unchanged = ((exp_default == NULL) == (exp_current == NULL)) &&
                ((exp_default == NULL) || (strcmp(exp_default->ascii, exp_current->ascii) == 0)) &&
                ((maskdefault == NULL) == (maskcurrent == NULL)) &&
                ((maskdefault == NULL) || (strcmp(maskdefault->ascii, maskcurrent->ascii) == 0));
        if (exp_current != NULL) ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s", exp_current->ascii);
        else ppl_snprintfTrunc(buf, LSTR_LENGTH, "rgb(c1,c1,c1)");
        int k = strlen(buf);
        if (maskcurrent == NULL) ppl_snprintfTrunc(buf + k, LSTR_LENGTH - k, " nomask");
        else ppl_snprintfTrunc(buf + k, LSTR_LENGTH - k, " mask %s", maskcurrent->ascii);
        k += strlen(buf + k);
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "colmap", buf, unchanged,
                            "The mapping of ordinate value to color used by the colormap plot style");
        i += strlen(out + i);
        p = 1;
    }
    // Show the contours setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "contours", 1) >= 0)) {
        // Either a number of evenly spaced contours, or a list of contour levels
        if (sg->ContoursListLen < 0) {
            ppl_snprintfTrunc(buf, LSTR_LENGTH, "%d %slabel", sg->ContoursN,
                              (sg->ContoursLabel == SW_ONOFF_ON) ? "" : "no");
        } else {
            pplObj v = sg->ContoursUnit;
            ppl_snprintfTrunc(buf, LSTR_LENGTH, "(");
            int p = strlen(buf);
            // Write each contour level, separated by commas
            for (int q = 0; q < sg->ContoursListLen; q++) {
                // Separate the levels with commas
                if (q != 0) {
                    ppl_snprintfTrunc(buf + p, LSTR_LENGTH - p, ", ");
                    p += strlen(buf + p);
                }
                v.real = sg->ContoursList[q];
                ppl_snprintfTrunc(buf + p, LSTR_LENGTH - p, "%s", ppl_unitsNumericDisplay(c, &v, 0, 0, 0));
                p += strlen(buf + p);
            }
            ppl_snprintfTrunc(buf + p, LSTR_LENGTH - p, ") %slabel", (sg->ContoursLabel == SW_ONOFF_ON) ? "" : "no");
            p += strlen(buf + p);
        }
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "contour", buf,
                            (c->set->graph_default.ContoursN == sg->ContoursN) &&
                                    (c->set->graph_default.ContoursLabel == sg->ContoursLabel) &&
                                    (c->set->graph_default.ContoursListLen == sg->ContoursListLen) &&
                                    ppl_unitsDimEqual(&c->set->graph_default.ContoursUnit, &sg->ContoursUnit) &&
                                    ((sg->ContoursListLen < 0) ||
                                     (memcmp((void *) c->set->graph_default.ContoursList, (void *) sg->ContoursList,
                                             sg->ContoursListLen * sizeof(double)) == 0)),
                            "The number of contours drawn by the contourmap plot style");
        i += strlen(out + i);
        p = 1;
    }
    // Show the c1range setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "c1range", 1) >= 0)) {
        showColorRange(c, out + i, outLen - i, itemSet, interactive, buf, sg, 0);
        i += strlen(out + i);
        p = 1;
    }
    // Show the c2range setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "c2range", 1) >= 0)) {
        showColorRange(c, out + i, outLen - i, itemSet, interactive, buf, sg, 1);
        i += strlen(out + i);
        p = 1;
    }
    // Show the c3range setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "c3range", 1) >= 0)) {
        showColorRange(c, out + i, outLen - i, itemSet, interactive, buf, sg, 2);
        i += strlen(out + i);
        p = 1;
    }
    // Show the c4range setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "c4range", 1) >= 0)) {
        showColorRange(c, out + i, outLen - i, itemSet, interactive, buf, sg, 3);
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "logscale", 1) >= 0) ||
        (ppl_strAutocomplete(word, "linearscale", 1) >= 0)) {
        // Show the logscale setting of each of the four color axes
        for (int C = 0; C < 4; C++) {
            if (sg->Clog[C] == SW_BOOL_TRUE) bufp = "logscale";
            else bufp = "nologscale";
            ppl_snprintfTrunc(buf, LSTR_LENGTH, "c%d", C + 1);
            sprintf(buf2,
                    "Sets whether colors in the colormap plot style, and contours in the contourmap plot style, demark linear or logarithmic intervals");
            ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, bufp, buf,
                                (sg->Clog[C] == c->set->graph_default.Clog[C]), buf2);
            i += strlen(out + i);
            p = 1;
        }
    }
    // Show the display setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "display", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.display, SW_ONOFF_INT,
                                                          SW_ONOFF_STR, sizeof(char *)));
        ppl_directive_show3(
                c, out + i, outLen - i, itemSet, 0, interactive, "display", buf,
                (c->set->term_default.display == c->set->term_current.display),
                "Sets whether any output is produced; turn on to improve performance whilst setting up large multiplots");
        i += strlen(out + i);
        p = 1;
    }
    // Show the filters setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "filters", 1) >= 0)) {
        dictIterator *iter = ppl_dictIterateInit(c->set->filters);
        pplObj *item = NULL;
        char *key = NULL;
        // Show each input filter, which is a shell command through which files with names matching a pattern are read
        while ((item = (pplObj *) ppl_dictIterate(&iter, &key)) != NULL) {
            ppl_strEscapify(key, buf, LSTR_LENGTH);
            ppl_strEscapify((char *) item->auxil, buf2, FNAME_LENGTH);
            strcat(buf, " ");
            strcat(buf, buf2);
            ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "filter", buf, 1,
                                "Sets an input filter to be used when reading datafiles");
            i += strlen(out + i);
            p = 1;
        }
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "fontsize", 1) >= 0) ||
        (ppl_strAutocomplete(word, "fountsize", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s", ppl_numericDisplayDefault(c, sg->FontSize, 0));
        ppl_directive_show3(
                c, out + i, outLen - i, itemSet, 1, interactive, "fontSize", buf,
                (c->set->graph_default.FontSize == sg->FontSize),
                "Sets the font size of text output: 1.0 is the default, and other values multiply this default size");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "axes_", 1) >= 0) || (ppl_strAutocomplete(word, "axis", 1) >= 0) ||
        (ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "grid", 1) >= 0)) {
        ppl_snprintfTrunc(
                buf, LSTR_LENGTH, "%s",
                *(char **) ppl_fetchSettingName(&c->errcontext, sg->grid, SW_ONOFF_INT, SW_ONOFF_STR, sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "grid", buf,
                            (c->set->graph_default.grid == sg->grid), "Selects whether a grid is drawn on plots");
        i += strlen(out + i);
        p = 1;
        // If the grid is on, show which axes it is drawn from
        if (sg->grid == SW_ONOFF_ON) {
            bufp = buf;
            k = 1;
            // List the x axes from which grid lines are drawn
            for (int j = 0; j < MAX_AXES; j++) {
                // This axis has grid lines
                if (sg->GridAxisX[j] != 0) {
                    ppl_snprintfTrunc(bufp, LSTR_LENGTH - (bufp - buf), "x%d", j);
                    bufp += strlen(bufp);
                }
                if (sg->GridAxisX[j] != c->set->graph_default.GridAxisX[j]) k = 0;
            }
            if (bufp != buf)
                ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "grid", buf, k,
                                    "Sets the x axis with whose ticks gridlines are associated");
            i += strlen(out + i);

            // List the y axes from which grid lines are drawn
            bufp = buf;
            k = 1;
            // Loop over the y axes
            for (int j = 0; j < MAX_AXES; j++) {
                // This axis has grid lines
                if (sg->GridAxisY[j] != 0) {
                    ppl_snprintfTrunc(bufp, LSTR_LENGTH - (bufp - buf), "y%d", j);
                    bufp += strlen(bufp);
                }
                if (sg->GridAxisY[j] != c->set->graph_default.GridAxisY[j]) k = 0;
            }
            if (bufp != buf)
                ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "grid", buf, k,
                                    "Sets the y axis with whose ticks gridlines are associated");
            i += strlen(out + i);

            // List the z axes from which grid lines are drawn
            bufp = buf;
            k = 1;
            // Loop over the z axes
            for (int j = 0; j < MAX_AXES; j++) {
                // This axis has grid lines
                if (sg->GridAxisZ[j] != 0) {
                    ppl_snprintfTrunc(bufp, LSTR_LENGTH - (bufp - buf), "z%d", j);
                    bufp += strlen(bufp);
                }
                if (sg->GridAxisZ[j] != c->set->graph_default.GridAxisZ[j]) k = 0;
            }
            if (bufp != buf)
                ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "grid", buf, k,
                                    "Sets the z axis with whose ticks gridlines are associated");
            i += strlen(out + i);
        }
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "gridmajcolor", 1) >= 0) ||
        (ppl_strAutocomplete(word, "gridmajcolor", 1) >= 0)) {
        showColor(c, out + i, outLen - i, itemSet, interactive, buf,
                  (showColorSetting) {sg->GridMajColor, sg->GridMajCol1234Space, sg->GridMajColor1, sg->GridMajColor2,
                                      sg->GridMajColor3, sg->GridMajColor4},
                  (showColorSetting) {c->set->graph_default.GridMajColor, c->set->graph_default.GridMajCol1234Space,
                                      c->set->graph_default.GridMajColor1, c->set->graph_default.GridMajColor2,
                                      c->set->graph_default.GridMajColor3, c->set->graph_default.GridMajColor4},
                  "gridMajColor", "The color of the major gridlines on graphs");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "gridmincolour", 1) >= 0) ||
        (ppl_strAutocomplete(word, "gridmincolor", 1) >= 0)) {
        showColor(c, out + i, outLen - i, itemSet, interactive, buf,
                  (showColorSetting) {sg->GridMinColor, sg->GridMinCol1234Space, sg->GridMinColor1, sg->GridMinColor2,
                                      sg->GridMinColor3, sg->GridMinColor4},
                  (showColorSetting) {c->set->graph_default.GridMinColor, c->set->graph_default.GridMinCol1234Space,
                                      c->set->graph_default.GridMinColor1, c->set->graph_default.GridMinColor2,
                                      c->set->graph_default.GridMinColor3, c->set->graph_default.GridMinColor4},
                  "gridMinColor", "The color of the minor gridlines on graphs");
        i += strlen(out + i);
        p = 1;
    }

    // Show the key setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "key", 1) >= 0)) {
        ppl_snprintfTrunc(
                buf, LSTR_LENGTH, "%s",
                *(char **) ppl_fetchSettingName(&c->errcontext, sg->key, SW_ONOFF_INT, SW_ONOFF_STR, sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "key", buf,
                            (c->set->graph_default.key == sg->key), "Selects whether a legend is included on plots");
        i += strlen(out + i);
        p = 1;
    }
    if (((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "key", 1) >= 0)) &&
        (sg->key == SW_ONOFF_ON)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s %s , %s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, sg->KeyPos, SW_KEYPOS_INT, SW_KEYPOS_STR,
                                                          sizeof(char *)),
                          ppl_unitsNumericDisplay(c, &(sg->KeyXOff), 0, 0, 0),
                          ppl_unitsNumericDisplay(c, &(sg->KeyYOff), 1, 0, 0));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "key", buf,
                            ((c->set->graph_default.KeyPos == sg->KeyPos) &&
                             (c->set->graph_default.KeyXOff.real == sg->KeyXOff.real) &&
                             (c->set->graph_default.KeyYOff.real == sg->KeyYOff.real)),
                            "Selects where legends are orientated on graphs");
        i += strlen(out + i);
        p = 1;
    }
    // Show the keycolumns setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "keycolumns", 1) >= 0)) {
        if (sg->KeyColumns > 0) ppl_snprintfTrunc(buf, LSTR_LENGTH, "%d", sg->KeyColumns);
        else ppl_snprintfTrunc(buf, LSTR_LENGTH, "auto");
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "keyColumns", buf,
                            (c->set->graph_default.KeyColumns == sg->KeyColumns),
                            "Sets the number of columns into which legends on graphs are sorted");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "linewidth", 1) >= 0) ||
        (ppl_strAutocomplete(word, "lw", 2) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s", ppl_numericDisplayDefault(c, sg->LineWidth, 0));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "lineWidth", buf,
                            (c->set->graph_default.LineWidth == sg->LineWidth),
                            "Sets the widths of lines drawn on graphs");
        i += strlen(out + i);
        p = 1;
    }
    // Show the multiplot setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "multiplot", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.multiplot, SW_ONOFF_INT,
                                                          SW_ONOFF_STR, sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "multiplot", buf,
                            (c->set->term_default.multiplot == c->set->term_current.multiplot),
                            "Selects whether multiplot mode is currently active");
        i += strlen(out + i);
        p = 1;
    }
    // Show the numerics setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "numerics", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.ComplexNumbers,
                                                          SW_ONOFF_INT, SW_ONOFF_STR, sizeof(char *)));
        ppl_directive_show3(
                c, out + i, outLen - i, itemSet, 0, interactive, "numerics complex", buf,
                (c->set->term_default.ComplexNumbers == c->set->term_current.ComplexNumbers),
                "Selects whether numbers are allowed to have imagnary components; affects the behaviour of functions such as sqrt()");
        i += strlen(out + i);
        p = 1;
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.ExplicitErrors,
                                                          SW_ONOFF_INT, SW_ONOFF_STR, sizeof(char *)));
        ppl_directive_show3(
                c, out + i, outLen - i, itemSet, 0, interactive, "numerics errors explicit", buf,
                (c->set->term_default.ExplicitErrors == c->set->term_current.ExplicitErrors),
                "Selects whether numerical errors quietly produce not-a-number results, or throw explicit errors");
        i += strlen(out + i);
        p = 1;
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          ppl_numericDisplayDefault(c, c->set->term_current.SignificantFigures, 0));
        ppl_directive_show3(
                c, out + i, outLen - i, itemSet, 0, interactive, "numerics sigfig", buf,
                (c->set->term_default.SignificantFigures == c->set->term_current.SignificantFigures),
                "Sets the (minimum) number of significant figures to which decimal numbers are displayed by default");
        i += strlen(out + i);
        p = 1;
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.NumDisplay,
                                                          SW_DISPLAY_INT, SW_DISPLAY_STR, sizeof(char *)));
        ppl_directive_show3(
                c, out + i, outLen - i, itemSet, 0, interactive, "numerics display", buf,
                (c->set->term_default.NumDisplay == c->set->term_current.NumDisplay),
                "Selects how numerical results are displayed: in a natural textual way, in a way which can be copied into a terminal, or as LaTeX");
        i += strlen(out + i);
        p = 1;
    }
    // Show the origin setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "origin", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s , %s", ppl_unitsNumericDisplay(c, &(sg->OriginX), 0, 0, 0),
                          ppl_unitsNumericDisplay(c, &(sg->OriginY), 1, 0, 0));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "origin", buf,
                            ((c->set->graph_default.OriginX.real == sg->OriginX.real) &&
                             (c->set->graph_default.OriginY.real == sg->OriginY.real)),
                            "Selects where the bottom-left corners of graphs are located on multiplot pages");
        i += strlen(out + i);
        p = 1;
    }
    // Show the output setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "output", 1) >= 0)) {
        ppl_strEscapify(c->set->term_current.output, buf, LSTR_LENGTH);
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "output", buf,
                            (strcmp(c->set->term_default.output, c->set->term_current.output) == 0),
                            "Filename to which graphic output is sent");
        i += strlen(out + i);
        p = 1;
    }
    // Show the palette setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "palette", 1) >= 0)) {
        l = 0;
        for (int j = 0; j < PALETTE_LENGTH; j++) // Check whether the palette has been changed from its default setting
        {
            if ((c->set->palette_current[j] == -1) && (c->set->palette_default[j] == -1)) break;
            if ((c->set->palette_current[j] == c->set->palette_default[j]) &&
                ((c->set->palette_current[j] > 0) ||
                 ((c->set->paletteS_current[j] == c->set->paletteS_default[j]) &&
                  (c->set->palette1_current[j] == c->set->palette1_default[j]) &&
                  (c->set->palette2_current[j] == c->set->palette2_default[j]) &&
                  (c->set->palette3_current[j] == c->set->palette3_default[j]) &&
                  (c->set->palette4_current[j] == c->set->palette4_default[j]))))
                continue;
            l = 1;
            break;
        }
        // Write each color of the palette, by name or by its components
        for (int j = k = 0; c->set->palette_current[j] >= 0; j++) {
            // Separate the colors with commas
            if (j > 0) {
                ppl_snprintfTrunc(buf + k, LSTR_LENGTH - k, ", ");
                k += strlen(buf + k);
            }
            if (c->set->palette_current[j] > 0)
                ppl_snprintfTrunc(buf + k, LSTR_LENGTH - k, "%s",
                                  *(char **) ppl_fetchSettingName(&c->errcontext, c->set->palette_current[j],
                                                                  SW_COLOR_INT, SW_COLOR_STR, sizeof(char *)));
            else if (c->set->paletteS_current[j] == SW_COLSPACE_RGB)
                ppl_snprintfTrunc(buf + k, LSTR_LENGTH - k, "rgb(%s,%s,%s)",
                                  ppl_numericDisplayDefault(c, c->set->palette1_current[j], 0),
                                  ppl_numericDisplayDefault(c, c->set->palette2_current[j], 1),
                                  ppl_numericDisplayDefault(c, c->set->palette3_current[j], 2));
            else if (c->set->paletteS_current[j] == SW_COLSPACE_HSB)
                ppl_snprintfTrunc(buf + k, LSTR_LENGTH - k, "hsb(%s,%s,%s)",
                                  ppl_numericDisplayDefault(c, c->set->palette1_current[j], 0),
                                  ppl_numericDisplayDefault(c, c->set->palette2_current[j], 1),
                                  ppl_numericDisplayDefault(c, c->set->palette3_current[j], 2));
            else if (c->set->paletteS_current[j] == SW_COLSPACE_CMYK)
                ppl_snprintfTrunc(buf + k, LSTR_LENGTH - k, "cmyk(%s,%s,%s,%s)",
                                  ppl_numericDisplayDefault(c, c->set->palette1_current[j], 0),
                                  ppl_numericDisplayDefault(c, c->set->palette2_current[j], 1),
                                  ppl_numericDisplayDefault(c, c->set->palette3_current[j], 2),
                                  ppl_numericDisplayDefault(c, c->set->palette4_current[j], 3));
            k += strlen(buf + k);
        }
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "palette", buf, !l,
                            "The sequence of colors used to plot datasets on color graphs");
        i += strlen(out + i);
        p = 1;
    }
    // Show the papersize setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "papersize", 1) >= 0)) {
        // A named paper size, or a size given by the user
        if (ppl_strAutocomplete("user", c->set->term_current.PaperName, 1) < 0) {
            char docbuff[FNAME_LENGTH];
            ppl_snprintfTrunc(docbuff, sizeof(docbuff), "The current papersize for postscript output (%s, %s)",
                              ppl_unitsNumericDisplay(c, &(c->set->term_current.PaperWidth), 0, 0, 0),
                              ppl_unitsNumericDisplay(c, &(c->set->term_current.PaperHeight), 1, 0, 0));
            ppl_strEscapify(c->set->term_current.PaperName, buf, LSTR_LENGTH);
            ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "paperSize", buf,
                                (strcmp(c->set->term_default.PaperName, c->set->term_current.PaperName) == 0), docbuff);
            i += strlen(out + i);
            p = 1;
        } else {
            ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s, %s",
                              ppl_unitsNumericDisplay(c, &(c->set->term_current.PaperWidth), 0, 0, 0),
                              ppl_unitsNumericDisplay(c, &(c->set->term_current.PaperHeight), 1, 0, 0));
            ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "paperSize", buf,
                                ((c->set->term_default.PaperWidth.real == c->set->term_current.PaperWidth.real) &&
                                 (c->set->term_default.PaperHeight.real == c->set->term_current.PaperHeight.real)),
                                "The current papersize for postscript output");
            i += strlen(out + i);
            p = 1;
        }
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "pointlinewidth", 1) >= 0) ||
        (ppl_strAutocomplete(word, "plw", 3) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s", ppl_numericDisplayDefault(c, sg->PointLineWidth, 0));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "pointLineWidth", buf,
                            (c->set->graph_default.PointLineWidth == sg->PointLineWidth),
                            "The width of the strokes used to mark points on graphs");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "pointsize", 1) >= 0) ||
        (ppl_strAutocomplete(word, "ps", 2) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s", ppl_numericDisplayDefault(c, sg->PointSize, 0));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "pointSize", buf,
                            (c->set->graph_default.PointSize == sg->PointSize), "The size of points marked on graphs");
        i += strlen(out + i);
        p = 1;
    }
    // Show the preamble setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "preamble", 1) >= 0)) {
        ppl_strEscapify(c->set->term_current.LatexPreamble, buf, LSTR_LENGTH);
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "preamble", buf,
                            (strcmp(c->set->term_default.LatexPreamble, c->set->term_current.LatexPreamble) == 0),
                            "Configuration options sent to the LaTeX typesetting system");
        i += strlen(out + i);
        p = 1;
    }
    //  if ((ppl_strAutocomplete(word, "settings", 1)>=0) || (ppl_strAutocomplete(word, "projection", 1)>=0))
    //   {
    //    ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s", *(char **)ppl_fetchSettingName(&c->errcontext, sg->projection, SW_PROJ_INT, SW_PROJ_STR , sizeof(char *)));
    //    ppl_directive_show3(c, out+i, itemSet, 0, interactive, "projection", buf, (c->set->graph_default.projection==sg->projection), "The projection used when representing (x,y) data on a graph");
    //    i += strlen(out+i) ; p=1;
    //   }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "samples", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%d grid ", sg->samples);
        int k = strlen(buf);
        if (sg->SamplesXAuto == SW_BOOL_TRUE) ppl_snprintfTrunc(buf + k, LSTR_LENGTH - k, "* x ");
        else ppl_snprintfTrunc(buf + k, LSTR_LENGTH - k, "%d x ", sg->SamplesX);
        k += strlen(buf + k);
        if (sg->SamplesYAuto == SW_BOOL_TRUE) ppl_snprintfTrunc(buf + k, LSTR_LENGTH - k, "*");
        else ppl_snprintfTrunc(buf + k, LSTR_LENGTH - k, "%d", sg->SamplesY);
        k += strlen(buf + k);
        ppl_snprintfTrunc(buf + k, LSTR_LENGTH - k, " interpolate %s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, sg->Sample2DMethod, SW_SAMPLEMETHOD_INT,
                                                          SW_SAMPLEMETHOD_STR, sizeof(char *)));

        // Show the setting
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "samples", buf,
                            ((c->set->graph_default.samples == sg->samples) &&
                             (c->set->graph_default.SamplesXAuto == sg->SamplesXAuto) &&
                             (c->set->graph_default.SamplesYAuto == sg->SamplesYAuto) &&
                             ((sg->SamplesXAuto == SW_BOOL_TRUE) || (c->set->graph_default.SamplesX == sg->SamplesX)) &&
                             ((sg->SamplesYAuto == SW_BOOL_TRUE) || (c->set->graph_default.SamplesY == sg->SamplesY)) &&
                             (c->set->graph_default.Sample2DMethod == sg->Sample2DMethod)),
                            "The number of samples taken when functions are plotted");
        i += strlen(out + i);
        p = 1;
    }
    // Show the seed setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "seed", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%ld", c->set->term_current.RandomSeed);
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "seed", buf,
                            (c->set->term_default.RandomSeed == c->set->term_current.RandomSeed),
                            "The last seed set for the random number generator");
        i += strlen(out + i);
        p = 1;
    }
    // Show the size setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "size", 1) >= 0)) {
        if (sg->AutoAspect == SW_ONOFF_ON) ppl_snprintfTrunc(buf, LSTR_LENGTH, "auto");
        else ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s", ppl_numericDisplayDefault(c, sg->aspect, 0));
        ppl_directive_show3(
                c, out + i, outLen - i, itemSet, 1, interactive, "size ratio", buf,
                ((c->set->graph_default.aspect == sg->aspect) && (c->set->graph_default.AutoAspect == sg->AutoAspect)),
                "The y/x aspect-ratio of graphs");
        i += strlen(out + i);
        p = 1;
        if (sg->AutoZAspect == SW_ONOFF_ON) ppl_snprintfTrunc(buf, LSTR_LENGTH, "auto");
        else ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s", ppl_numericDisplayDefault(c, sg->zaspect, 0));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "size zratio", buf,
                            ((c->set->graph_default.zaspect == sg->zaspect) &&
                             (c->set->graph_default.AutoZAspect == sg->AutoZAspect)),
                            "The z/x aspect-ratio of 3d graphs");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "data", 1) >= 0) ||
        (ppl_strAutocomplete(word, "style", 1) >= 0)) {
        ppl_withWordsPrint(c, &sg->dataStyle, buf, LSTR_LENGTH);
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "data style", buf,
                            ppl_withWordsCmp(c, &c->set->graph_default.dataStyle, &sg->dataStyle),
                            "Default plot options for plotting datafiles");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "function", 1) >= 0) ||
        (ppl_strAutocomplete(word, "style", 1) >= 0)) {
        ppl_withWordsPrint(c, &sg->funcStyle, buf, LSTR_LENGTH);
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "function style", buf,
                            ppl_withWordsCmp(c, &c->set->graph_default.funcStyle, &sg->funcStyle),
                            "Default plot options for plotting functions");
        i += strlen(out + i);
        p = 1;
    }
    // Show the terminal setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "terminal", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.TermType,
                                                          SW_TERMTYPE_INT, SW_TERMTYPE_STR, sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "terminal", buf,
                            (c->set->term_default.TermType == c->set->term_current.TermType),
                            "The type of graphic output to be produced");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "terminal", 1) >= 0) ||
        (ppl_strAutocomplete(word, "antialias", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.TermAntiAlias,
                                                          SW_ONOFF_INT, SW_ONOFF_STR, sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "terminal antialias", buf,
                            (c->set->term_default.TermAntiAlias == c->set->term_current.TermAntiAlias),
                            "Selects whether anti-aliasing is applied to bitmap output");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "terminal", 1) >= 0) ||
        (ppl_strAutocomplete(word, "colour", 1) >= 0) || (ppl_strAutocomplete(word, "color", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.color, SW_ONOFF_INT,
                                                          SW_ONOFF_STR, sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "terminal color", buf,
                            (c->set->term_default.color == c->set->term_current.color),
                            "Selects whether output is color or monochrome");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "terminal", 1) >= 0) ||
        (ppl_strAutocomplete(word, "dpi", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s", ppl_numericDisplayDefault(c, c->set->term_current.dpi, 0));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "terminal dpi", buf,
                            (c->set->term_default.dpi == c->set->term_current.dpi),
                            "Sets the pixel resolution used when producing bitmap graphic output");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "terminal", 1) >= 0) ||
        (ppl_strAutocomplete(word, "enlargement", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.TermEnlarge,
                                                          SW_ONOFF_INT, SW_ONOFF_STR, sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "terminal enlarge", buf,
                            (c->set->term_default.TermEnlarge == c->set->term_current.TermEnlarge),
                            "Selects whether output photo-enlarged to fill the page");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "terminal", 1) >= 0) ||
        (ppl_strAutocomplete(word, "invert", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.TermInvert, SW_ONOFF_INT,
                                                          SW_ONOFF_STR, sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "terminal invert", buf,
                            (c->set->term_default.TermInvert == c->set->term_current.TermInvert),
                            "Selects whether the colors of bitmap output are inverted");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "terminal", 1) >= 0) ||
        (ppl_strAutocomplete(word, "landscape", 1) >= 0) || (ppl_strAutocomplete(word, "portrait", 1) >= 0)) {
        if (c->set->term_current.landscape == SW_ONOFF_ON) ppl_snprintfTrunc(buf, LSTR_LENGTH, "landscape");
        else ppl_snprintfTrunc(buf, LSTR_LENGTH, "portrait");
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "terminal", buf,
                            (c->set->term_default.landscape == c->set->term_current.landscape),
                            "Selects the orientation of output");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "terminal", 1) >= 0) ||
        (ppl_strAutocomplete(word, "transparent", 1) >= 0) || (ppl_strAutocomplete(word, "solid", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.TermTransparent,
                                                          SW_ONOFF_INT, SW_ONOFF_STR, sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "terminal transparent", buf,
                            (c->set->term_default.TermTransparent == c->set->term_current.TermTransparent),
                            "Selects whether gif and png output is transparent");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "textcolour", 1) >= 0) ||
        (ppl_strAutocomplete(word, "textcolor", 1) >= 0)) {
        showColor(c, out + i, outLen - i, itemSet, interactive, buf,
                  (showColorSetting) {sg->TextColor, sg->TextCol1234Space, sg->TextColor1, sg->TextColor2,
                                      sg->TextColor3, sg->TextColor4},
                  (showColorSetting) {c->set->graph_default.TextColor, c->set->graph_default.TextCol1234Space,
                                      c->set->graph_default.TextColor1, c->set->graph_default.TextColor2,
                                      c->set->graph_default.TextColor3, c->set->graph_default.TextColor4},
                  "textColor", "Selects the color of text labels");
        i += strlen(out + i);
        p = 1;
    }
    // Show the texthalign setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "texthalign", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, sg->TextHAlign, SW_HALIGN_INT, SW_HALIGN_STR,
                                                          sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "textHAlign", buf,
                            (c->set->graph_default.TextHAlign == sg->TextHAlign),
                            "Selects the horizontal alignment of text labels");
        i += strlen(out + i);
        p = 1;
    }
    // Show the textvalign setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "textvalign", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, sg->TextVAlign, SW_VALIGN_INT, SW_VALIGN_STR,
                                                          sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "textVAlign", buf,
                            (c->set->graph_default.TextVAlign == sg->TextVAlign),
                            "Selects the vertical alignment of text labels");
        i += strlen(out + i);
        p = 1;
    }
    // Show the timezone setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "timezone", 1) >= 0)) {
        ppl_strEscapify(c->set->term_current.timezone, buf, LSTR_LENGTH);
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "timezone", buf,
                            ((strcmp(c->set->graph_default.title, sg->title) == 0) &&
                             (c->set->graph_default.TitleXOff.real == sg->TitleXOff.real) &&
                             (c->set->graph_default.TitleYOff.real == sg->TitleYOff.real)),
                            "The default timezone used by date objects");
        i += strlen(out + i);
        p = 1;
    }
    // Show the title setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "title", 1) >= 0)) {
        ppl_strEscapify(sg->title, buf, LSTR_LENGTH);
        k = strlen(buf);
        ppl_snprintfTrunc(buf + k, LSTR_LENGTH - k, " %s , %s", ppl_unitsNumericDisplay(c, &(sg->TitleXOff), 0, 0, 0),
                          ppl_unitsNumericDisplay(c, &(sg->TitleYOff), 1, 0, 0));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "title", buf,
                            ((strcmp(c->set->graph_default.title, sg->title) == 0) &&
                             (c->set->graph_default.TitleXOff.real == sg->TitleXOff.real) &&
                             (c->set->graph_default.TitleYOff.real == sg->TitleYOff.real)),
                            "A title to be displayed above graphs");
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "trange", 1) >= 0) ||
        (ppl_strAutocomplete(word, "urange", 1) >= 0) || (ppl_strAutocomplete(word, "vrange", 1) >= 0))
        // Parametric plots use either a range of t, or ranges of u and v
        if (sg->USE_T_or_uv) {
            ppl_snprintfTrunc(buf, LSTR_LENGTH, "[%s:%s]", ppl_unitsNumericDisplay(c, &(sg->Tmin), 0, 0, 0),
                              ppl_unitsNumericDisplay(c, &(sg->Tmax), 1, 0, 0));
            ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "trange", buf,
                                (c->set->graph_default.USE_T_or_uv == sg->USE_T_or_uv) &&
                                        (c->set->graph_default.Tmin.real == sg->Tmin.real) &&
                                        ppl_unitsDimEqual(&(c->set->graph_default.Tmin), &(sg->Tmin)) &&
                                        (c->set->graph_default.Tmax.real == sg->Tmax.real) &&
                                        ppl_unitsDimEqual(&(c->set->graph_default.Tmax), &(sg->Tmax)),
                                "The range of input values used in constructing parametric function plots");
            i += strlen(out + i);
            p = 1;
        }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "logscale", 1) >= 0) ||
        (ppl_strAutocomplete(word, "linearscale", 1) >= 0)) {
        if (sg->Tlog == SW_BOOL_TRUE) bufp = "logscale";
        else bufp = "nologscale";
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "t");
        ppl_snprintfTrunc(buf2, FNAME_LENGTH, "Sets whether the t-axis scales linearly or logarithmically");
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, bufp, buf,
                            (sg->Tlog == c->set->graph_default.Tlog), buf2);
        i += strlen(out + i);
        p = 1;
    }
    // Show the units setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "units", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.UnitAngleDimless,
                                                          SW_ONOFF_INT, SW_ONOFF_STR, sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "unit angle dimensionless", buf,
                            (c->set->term_default.UnitAngleDimless == c->set->term_current.UnitAngleDimless),
                            "Selects whether angles are treated as dimensionless quantities");
        i += strlen(out + i);
        p = 1;
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.UnitDisplayAbbrev,
                                                          SW_ONOFF_INT, SW_ONOFF_STR, sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "unit display abbreviated", buf,
                            (c->set->term_default.UnitDisplayAbbrev == c->set->term_current.UnitDisplayAbbrev),
                            "Selects whether units are displayed in abbreviated form ('m' vs. 'metres')");
        i += strlen(out + i);
        p = 1;
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.UnitDisplayPrefix,
                                                          SW_ONOFF_INT, SW_ONOFF_STR, sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "unit display prefix", buf,
                            (c->set->term_default.UnitDisplayPrefix == c->set->term_current.UnitDisplayPrefix),
                            "Selects whether SI units are displayed with prefixes");
        i += strlen(out + i);
        p = 1;
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s",
                          *(char **) ppl_fetchSettingName(&c->errcontext, c->set->term_current.UnitScheme,
                                                          SW_UNITSCH_INT, SW_UNITSCH_STR, sizeof(char *)));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "unit scheme", buf,
                            (c->set->term_default.UnitScheme == c->set->term_current.UnitScheme),
                            "Selects the scheme (e.g. SI or Imperial) of preferred units");
        i += strlen(out + i);
        p = 1;
        for (int j = 0; j < c->unit_pos; j++)
            // Show the units which the user has chosen for particular quantities
            if (ud[j].userSel != 0) {
                ppl_snprintfTrunc(buf, LSTR_LENGTH, "unit of %s", ud[j].quantity);
                if (c->set->term_current.UnitDisplayAbbrev == SW_ONOFF_ON)
                    ppl_snprintfTrunc(buf2, FNAME_LENGTH, "%s%s", SIprefixes_abbrev[ud[j].userSelPrefix], ud[j].nameAs);
                else ppl_snprintfTrunc(buf2, FNAME_LENGTH, "%s%s", SIprefixes_full[ud[j].userSelPrefix], ud[j].nameFs);
                ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, buf, buf2, 0,
                                    "Selects a user-preferred unit for a particular quantity");
                i += strlen(out + i);
                p = 1;
            }

        // show preferred units
        {
            PreferredUnit *pu;
            listIterator *listiter = ppl_listIterateInit(c->unit_PreferredUnits);
            // Show each preferred unit, a product of powers of units
            while ((pu = (PreferredUnit *) ppl_listIterate(&listiter)) != NULL) {
                int pbuf = 0;
                buf[0] = '\0';
                // Write each unit in the product, with its prefix and power
                for (int ppu = 0; ppu < pu->NUnits; ppu++) {
                    if (ppu > 0) ppl_snprintfTrunc(buf + pbuf, LSTR_LENGTH - pbuf, "*");
                    pbuf += strlen(buf + pbuf);
                    if (c->set->term_current.UnitDisplayAbbrev == SW_ONOFF_ON)
                        ppl_snprintfTrunc(buf + pbuf, LSTR_LENGTH - pbuf, "%s%s",
                                          (pu->prefix[ppu] >= 1) ? SIprefixes_abbrev[pu->prefix[ppu]] : "",
                                          ud[pu->UnitID[ppu]].nameAs);
                    else
                        ppl_snprintfTrunc(buf + pbuf, LSTR_LENGTH - pbuf, "%s%s",
                                          (pu->prefix[ppu] >= 1) ? SIprefixes_full[pu->prefix[ppu]] : "",
                                          ud[pu->UnitID[ppu]].nameFs);
                    pbuf += strlen(buf + pbuf);
                    if (pu->exponent[ppu] != 1)
                        ppl_snprintfTrunc(buf + pbuf, LSTR_LENGTH - pbuf, "**%s",
                                          ppl_numericDisplayDefault(c, pu->exponent[ppu], 0));
                    pbuf += strlen(buf + pbuf);
                }
                ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "unit preferred", buf,
                                    !pu->modified, "Specifies a user-preferred physical unit");
                i += strlen(out + i);
                p = 1;
            }
        }
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "urange", 1) >= 0) ||
        (ppl_strAutocomplete(word, "trange", 1) >= 0))
        // The range of u, when it is in use
        if (!sg->USE_T_or_uv) {
            ppl_snprintfTrunc(buf, LSTR_LENGTH, "[%s:%s]", ppl_unitsNumericDisplay(c, &(sg->Umin), 0, 0, 0),
                              ppl_unitsNumericDisplay(c, &(sg->Umax), 1, 0, 0));
            ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "urange", buf,
                                (c->set->graph_default.USE_T_or_uv == sg->USE_T_or_uv) &&
                                        (c->set->graph_default.Umin.real == sg->Umin.real) &&
                                        ppl_unitsDimEqual(&(c->set->graph_default.Umin), &(sg->Umin)) &&
                                        (c->set->graph_default.Umax.real == sg->Umax.real) &&
                                        ppl_unitsDimEqual(&(c->set->graph_default.Umax), &(sg->Umax)),
                                "The range of input values used in constructing 2d parametric function plots");
            i += strlen(out + i);
            p = 1;
        }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "logscale", 1) >= 0) ||
        (ppl_strAutocomplete(word, "linearscale", 1) >= 0)) {
        if (sg->Ulog == SW_BOOL_TRUE) bufp = "logscale";
        else bufp = "nologscale";
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "u");
        ppl_snprintfTrunc(buf2, FNAME_LENGTH, "Sets whether the u-axis scales linearly or logarithmically");
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, bufp, buf,
                            (sg->Ulog == c->set->graph_default.Ulog), buf2);
        i += strlen(out + i);
        p = 1;
    }
    // Show the view setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "view", 1) >= 0)) {
        const int SF = c->set->term_current.SignificantFigures;
        const int TY = (c->set->term_current.NumDisplay == SW_DISPLAY_L);
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s,%s",
                          ppl_numericDisplay(sg->XYview.real / M_PI * 180, c->numdispBuff[0], SF, TY),
                          ppl_numericDisplay(sg->YZview.real / M_PI * 180, c->numdispBuff[1], SF, TY));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "view", buf,
                            (c->set->graph_default.XYview.real == sg->XYview.real) &&
                                    (c->set->graph_default.YZview.real == sg->YZview.real),
                            "The rotation angle of 3d graphs");
        i += strlen(out + i);
        p = 1;
    }
    // Show the viewer setting, if it, or all settings, were asked for
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "viewer", 1) >= 0)) {
        unsigned char changed = (c->set->term_current.viewer != c->set->term_default.viewer);
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "Selects the postscript viewer used by the X11 terminals%s%s%s",
                          (c->set->term_current.viewer != SW_VIEWER_CUSTOM) ? " (" : "",
                          (c->set->term_current.viewer == SW_VIEWER_GGV) ? "g" : "",
                          (c->set->term_current.viewer != SW_VIEWER_CUSTOM) ? "gv)" : "");
        if ((c->set->term_current.viewer == SW_VIEWER_CUSTOM) && (c->set->term_default.viewer == SW_VIEWER_CUSTOM))
            changed = (strcmp(c->set->term_current.ViewerCmd, c->set->term_default.ViewerCmd) != 0);
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, "viewer",
                            (c->set->term_current.viewer != SW_VIEWER_CUSTOM) ? "auto" : c->set->term_current.ViewerCmd,
                            !changed, buf);
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "vrange", 1) >= 0) ||
        (ppl_strAutocomplete(word, "trange", 1) >= 0))
        // The range of v, when it is in use
        if (!sg->USE_T_or_uv) {
            ppl_snprintfTrunc(buf, LSTR_LENGTH, "[%s:%s]", ppl_unitsNumericDisplay(c, &(sg->Vmin), 0, 0, 0),
                              ppl_unitsNumericDisplay(c, &(sg->Vmax), 1, 0, 0));
            ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "vrange", buf,
                                (c->set->graph_default.USE_T_or_uv == sg->USE_T_or_uv) &&
                                        (c->set->graph_default.Vmin.real == sg->Vmin.real) &&
                                        ppl_unitsDimEqual(&(c->set->graph_default.Vmin), &(sg->Vmin)) &&
                                        (c->set->graph_default.Vmax.real == sg->Vmax.real) &&
                                        ppl_unitsDimEqual(&(c->set->graph_default.Vmax), &(sg->Vmax)),
                                "The range of input values used in constructing 2d parametric function plots");
            i += strlen(out + i);
            p = 1;
        }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "logscale", 1) >= 0) ||
        (ppl_strAutocomplete(word, "linearscale", 1) >= 0)) {
        if (sg->Vlog == SW_BOOL_TRUE) bufp = "logscale";
        else bufp = "nologscale";
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "v");
        ppl_snprintfTrunc(buf2, FNAME_LENGTH, "Sets whether the t-axis scales linearly or logarithmically");
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, bufp, buf,
                            (sg->Vlog == c->set->graph_default.Vlog), buf2);
        i += strlen(out + i);
        p = 1;
    }
    if ((ppl_strAutocomplete(word, "settings", 1) >= 0) || (ppl_strAutocomplete(word, "width", 1) >= 0) ||
        (ppl_strAutocomplete(word, "size", 1) >= 0)) {
        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s", ppl_unitsNumericDisplay(c, &(sg->width), 0, 0, 0));
        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "width", buf,
                            (c->set->graph_default.width.real == sg->width.real), "The width of graphs");
        i += strlen(out + i);
        p = 1;
    }

    // Show axes
    l = 0;
    if ((ppl_strAutocomplete(word, "axes_", 1) >= 0) || (ppl_strAutocomplete(word, "axis", 1) >= 0)) l = 1;
    // Show the settings of each axis
    if (!((xa == NULL) || (ya == NULL) || (za == NULL))) {
        for (k = 0; k < 3; k++)
            // Loop over the x, y and z axes, and the axes of each direction
            for (int j = 0; j < MAX_AXES; j++) {
                pplset_axis *axisPtrDef, *axisPtr;
                // Point to the axis, and to its default settings
                switch (k) {
                    case 1: {
                        axisPtr = &(ya[j]);
                        axisPtrDef = &(c->set->YAxesDefault[j]);
                        break;
                    }
                    case 2: {
                        axisPtr = &(za[j]);
                        axisPtrDef = &(c->set->ZAxesDefault[j]);
                        break;
                    }
                    default: {
                        axisPtr = &(xa[j]);
                        axisPtrDef = &(c->set->XAxesDefault[j]);
                        break;
                    }
                }
                if (!axisPtr->enabled) // Do not show any information for inactive axes, except that they're disabled
                {
                    // A disabled axis is shown only if it is enabled by default
                    if (axisPtrDef->enabled) {
                        ppl_snprintfTrunc(temp1, sizeof(temp1), "%c%d", "xyzc"[k], j);
                        ppl_snprintfTrunc(buf2, FNAME_LENGTH, "Axis %s has been disabled", temp1);
                        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "noaxis", temp1, 0, buf2);
                        i += strlen(out + i);
                        p = 1;
                    }
                    continue;
                }

                // Show the axis if it was asked for by name, e.g. x2, or as x for x1, or if all axes were
                ppl_snprintfTrunc(temp1, sizeof(temp1), "%c%d", "xyzc"[k], j);
                ppl_snprintfTrunc(temp2, sizeof(temp2), "%c", "xyzc"[k]);
                if (l || (ppl_strAutocomplete(word, temp1, 1) >= 0) ||
                    ((j == 1) && (ppl_strAutocomplete(word, temp2, 1) >= 0))) {
                    ppl_snprintfTrunc(buf, LSTR_LENGTH, "%c%d ", "xyzc"[k], j);
                    m = strlen(buf);
                    ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, "%s ", (axisPtr->invisible ? "invisible" : "visible"));
                    m += strlen(buf + m);
                    if (k == 1)
                        ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, "%s ", (axisPtr->topbottom ? "right" : "left"));
                    else if (k == 2)
                        ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, "%s ", (axisPtr->topbottom ? "back" : "front"));
                    else ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, "%s ", (axisPtr->topbottom ? "top" : "bottom"));
                    m += strlen(buf + m);
                    ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, "%s ",
                                      *(char **) ppl_fetchSettingName(&c->errcontext, axisPtr->ArrowType,
                                                                      SW_AXISDISP_INT, SW_AXISDISP_STR,
                                                                      sizeof(char *)));
                    m += strlen(buf + m);
                    ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, "%s ", (axisPtr->atzero ? "atZero" : "notAtZero"));
                    m += strlen(buf + m);
                    ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, "%s ",
                                      *(char **) ppl_fetchSettingName(&c->errcontext, axisPtr->MirrorType,
                                                                      SW_AXISMIRROR_INT, SW_AXISMIRROR_STR,
                                                                      sizeof(char *)));
                    m += strlen(buf + m);
                    // A linked axis, the axis it is linked to, and the expression which maps between them
                    if (!axisPtr->linked) {
                        ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, "notLinked");
                        m += strlen(buf + m);
                    } else {
                        ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, "%s", "linked");
                        m += strlen(buf + m);
                        // An axis linked to an axis of another multiplot item
                        if (axisPtr->LinkedAxisCanvasID > 0) {
                            ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, " item %d", axisPtr->LinkedAxisCanvasID);
                            m += strlen(buf + m);
                        }
                        ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, " %c%d", "xyzc"[axisPtr->LinkedAxisToXYZ],
                                          axisPtr->LinkedAxisToNum);
                        m += strlen(buf + m);
                        // An expression which maps between the axes
                        if (axisPtr->linkusing != NULL) {
                            ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, " using %s",
                                              ((pplExpr *) axisPtr->linkusing)->ascii);
                            m += strlen(buf + m);
                        }
                    }
                    ppl_snprintfTrunc(buf2, FNAME_LENGTH, "Settings for the %c%d axis", "xyzc"[k], j);
                    ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, "axis", buf,
                                        (axisPtr->atzero == axisPtrDef->atzero) &&
                                                (axisPtr->enabled == axisPtrDef->enabled) &&
                                                (axisPtr->invisible == axisPtrDef->invisible) &&
                                                (axisPtr->linked == axisPtrDef->linked) &&
                                                (axisPtr->topbottom == axisPtrDef->topbottom) &&
                                                (axisPtr->ArrowType == axisPtrDef->ArrowType) &&
                                                (axisPtr->LinkedAxisCanvasID == axisPtrDef->LinkedAxisCanvasID) &&
                                                (axisPtr->LinkedAxisToXYZ == axisPtrDef->LinkedAxisToXYZ) &&
                                                (axisPtr->LinkedAxisToNum == axisPtrDef->LinkedAxisToNum) &&
                                                (axisPtr->MirrorType == axisPtrDef->MirrorType),
                                        buf2);
                    i += strlen(out + i);
                    p = 1;
                }

                // Show the format of the axis's tick labels
                ppl_snprintfTrunc(temp1, sizeof(temp1), "%c%dformat", "xyzc"[k], j);
                ppl_snprintfTrunc(temp2, sizeof(temp2), "%cformat", "xyzc"[k]);
                if (l || (ppl_strAutocomplete(word, temp1, 1) >= 0) ||
                    ((j == 1) && (ppl_strAutocomplete(word, temp2, 1) >= 0))) {
                    if (axisPtr->format != NULL)
                        ppl_snprintfTrunc(buf, LSTR_LENGTH, "%s ", ((pplExpr *) axisPtr->format)->ascii);
                    else buf[0] = '\0';
                    m = strlen(buf);
                    ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, "%s",
                                      *(char **) ppl_fetchSettingName(&c->errcontext, axisPtr->TickLabelRotation,
                                                                      SW_TICLABDIR_INT, SW_TICLABDIR_STR,
                                                                      sizeof(char *)));
                    m += strlen(buf + m);
                    // Rotated tick labels also have an angle
                    if (axisPtr->TickLabelRotation == SW_TICLABDIR_ROT) {
                        pplObj valobj;
                        valobj.refCount = 1;
                        pplObjNum(&valobj, 0, axisPtr->TickLabelRotate, 0);
                        valobj.exponent[UNIT_ANGLE] = 1;
                        valobj.dimensionless = 0;
                        ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, " %s",
                                          ppl_unitsNumericDisplay(c, &valobj, 0, 0, 0));
                    }
                    ppl_snprintfTrunc(buf2, FNAME_LENGTH, "Format string for the tick labels on the %c%d axis",
                                      "xyzc"[k], j);
                    ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, temp1, buf,
                                        ((axisPtr->TickLabelRotate == axisPtrDef->TickLabelRotate) &&
                                         (axisPtr->TickLabelRotation == axisPtrDef->TickLabelRotation) &&
                                         (((axisPtr->format == NULL) && (axisPtrDef->format == NULL)) ||
                                          ((axisPtr->format != NULL) && (axisPtrDef->format != NULL) &&
                                           (strcmp(((pplExpr *) axisPtr->format)->ascii,
                                                   ((pplExpr *) axisPtrDef->format)->ascii) == 0)))),
                                        buf2);
                    i += strlen(out + i);
                    p = 1;
                }

                // Show the label of the axis
                ppl_snprintfTrunc(temp1, sizeof(temp1), "%c%dlabel", "xyzc"[k], j);
                ppl_snprintfTrunc(temp2, sizeof(temp2), "%clabel", "xyzc"[k]);
                if (l || (ppl_strAutocomplete(word, temp1, 1) >= 0) ||
                    ((j == 1) && (ppl_strAutocomplete(word, temp2, 1) >= 0))) {
                    pplObj valobj;
                    valobj.refCount = 1;
                    ppl_strEscapify(axisPtr->label == NULL ? "" : axisPtr->label, buf, LSTR_LENGTH);
                    m = strlen(buf);
                    pplObjNum(&valobj, 0, axisPtr->LabelRotate, 0);
                    valobj.exponent[UNIT_ANGLE] = 1;
                    valobj.dimensionless = 0;
                    ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, " rotate %s",
                                      ppl_unitsNumericDisplay(c, &valobj, 0, 0, 0));
                    ppl_snprintfTrunc(buf2, FNAME_LENGTH, "Textual label for the %c%d axis", "xyzc"[k], j);
                    ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, temp1, buf,
                                        ((axisPtr->LabelRotate == axisPtrDef->LabelRotate) &&
                                         (((axisPtr->label == NULL) && (axisPtrDef->label == NULL)) ||
                                          ((axisPtr->label != NULL) && (axisPtrDef->label != NULL) &&
                                           (strcmp(axisPtr->label, axisPtrDef->label) == 0)))),
                                        buf2);
                    i += strlen(out + i);
                    p = 1;
                }

                // Show whether the axis is logarithmic
                if (l || (ppl_strAutocomplete(word, "logscale", 1) >= 0) ||
                    ((j == 1) && (ppl_strAutocomplete(word, "linearscale", 1) >= 0))) {
                    if (axisPtr->log == SW_BOOL_TRUE) bufp = "logscale";
                    else bufp = "nologscale";
                    ppl_snprintfTrunc(buf, LSTR_LENGTH, "%c%d", "xyzc"[k], j);
                    m = strlen(buf);
                    if (axisPtr->log == SW_BOOL_TRUE)
                        ppl_snprintfTrunc(buf + m, LSTR_LENGTH - m, " base %d", (int) axisPtr->tics.logBase);
                    ppl_snprintfTrunc(buf2, FNAME_LENGTH,
                                      "Sets whether the %c%d axis scales linearly or logarithmically", "xyzc"[k], j);
                    ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, bufp, buf,
                                        (axisPtr->log == axisPtrDef->log), buf2);
                    i += strlen(out + i);
                    p = 1;
                }

                // Show the range of the axis
                ppl_snprintfTrunc(temp1, sizeof(temp1), "%c%drange", "xyzc"[k], j);
                ppl_snprintfTrunc(temp2, sizeof(temp2), "%crange", "xyzc"[k]);
                if (l || (ppl_strAutocomplete(word, "autoscale", 1) >= 0) ||
                    (ppl_strAutocomplete(word, temp1, 1) >= 0) ||
                    ((j == 1) && (ppl_strAutocomplete(word, temp2, 1) >= 0))) {
                    axisPtr->unit.real = axisPtr->min;
                    if (axisPtr->MinSet == SW_BOOL_TRUE) bufp = ppl_unitsNumericDisplay(c, &(axisPtr->unit), 0, 0, 0);
                    else bufp = "*";
                    axisPtr->unit.real = axisPtr->max;
                    if (axisPtr->MaxSet == SW_BOOL_TRUE) bufp2 = ppl_unitsNumericDisplay(c, &(axisPtr->unit), 1, 0, 0);
                    else bufp2 = "*";
                    ppl_snprintfTrunc(buf, LSTR_LENGTH, "[%s:%s]%s", bufp, bufp2,
                                      axisPtr->RangeReversed ? " reversed" : "");
                    ppl_snprintfTrunc(buf2, FNAME_LENGTH, "Sets the range of the %c%d axis", "xyzc"[k], j);
                    ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, temp1, buf,
                                        (axisPtr->min == axisPtrDef->min) && (axisPtr->MinSet == axisPtrDef->MinSet) &&
                                                (axisPtr->max == axisPtrDef->max) &&
                                                (axisPtr->MaxSet == axisPtrDef->MaxSet),
                                        buf2);
                    i += strlen(out + i);
                    p = 1;
                }

                // Show the major ticks of the axis
                ppl_snprintfTrunc(temp1, sizeof(temp1), "%c%dtics", "xyzc"[k], j);
                ppl_snprintfTrunc(temp2, sizeof(temp2), "%ctics", "xyzc"[k]);
                m = 0;

                // Show them if they were asked for by name
                if (l || (ppl_strAutocomplete(word, temp1, 1) >= 0) ||
                    ((j == 1) && (ppl_strAutocomplete(word, temp2, 1) >= 0))) {
                    showTicks(c, out + i, outLen - i, itemSet, interactive, temp1, buf, buf2, &(axisPtr->tics),
                              &(axisPtrDef->tics), &(axisPtr->unit), &(axisPtrDef->unit), axisPtr->log, axisPtrDef->log,
                              "xyzc"[k], j);
                    i += strlen(out + i);
                    p = 1;
                    m = 1; // If we've shown major tics, also show minor ticks too.
                }

                // Show the minor ticks of the axis
                ppl_snprintfTrunc(temp1, sizeof(temp1), "m%c%dtics", "xyzc"[k], j);
                ppl_snprintfTrunc(temp2, sizeof(temp2), "m%ctics", "xyzc"[k]);
                if (l || m || (ppl_strAutocomplete(word, temp1, 1) >= 0) ||
                    ((j == 1) && (ppl_strAutocomplete(word, temp2, 1) >= 0))) {
                    showTicks(c, out + i, outLen - i, itemSet, interactive, temp1, buf, buf2, &(axisPtr->ticsM),
                              &(axisPtrDef->ticsM), &(axisPtr->unit), &(axisPtrDef->unit), axisPtr->log,
                              axisPtrDef->log, "xyzc"[k], j);
                    i += strlen(out + i);
                    p = 1;
                }
            } // loop over axes
    } // if axis data structures are not null

    // Showed numbered arrows
    if (ppl_strAutocomplete(word, "arrows", 1) >= 0) {
        // Show the arrows, if the item has any
        if (!((xa == NULL) || (ya == NULL) || (za == NULL))) {
            pplarrow_object *ai_default_prev = c->set->pplarrow_list_default;
            pplarrow_object *ai_default = c->set->pplarrow_list_default;
            showHighlight(c, out, outLen, &i, interactive, 1);
            ppl_snprintfTrunc(out + i, outLen - i, "\n# Numbered arrows:\n\n");
            i += strlen(out + i);
            p = 1;
            showDehighlight(c, out, outLen, &i, interactive);
            // Show each arrow, in order of number, interleaved with any default arrows which have been removed
            for (pplarrow_object *ai = *al; ai != NULL; ai = ai->next) {
                // Stop if the output buffer is nearly full
                if (i > outLen - 1024) {
                    ppl_snprintfTrunc(out + i, outLen - i, "...\n");
                    i += strlen(out + i);
                    break;
                }
                // Show any default arrows with lower numbers, which have been removed
                while ((ai_default != NULL) && (ai_default->id <= ai->id)) {
                    // This default arrow has been removed
                    if (ai_default->id < ai->id) {
                        ppl_snprintfTrunc(buf2, FNAME_LENGTH, "noarrow %6d", ai_default->id);
                        ppl_snprintfTrunc(buf, LSTR_LENGTH, "remove arrow %6d", ai_default->id);
                        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, buf2, "", 1, buf);
                        i += strlen(out + i);
                    }
                    ai_default_prev = ai_default;
                    ai_default = ai_default->next;
                }
                pplarrow_print(c, ai, buf, LSTR_LENGTH);
                ppl_snprintfTrunc(buf2, FNAME_LENGTH, "arrow %6d", ai->id);
                if ((unchanged = ((ai_default_prev != NULL) && (ai_default_prev->id == ai->id))) != 0)
                    unchanged = pplarrow_compare(c, ai, ai_default_prev);
                ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, buf2, buf, unchanged, buf2);
                i += strlen(out + i);
            }
            // Show any remaining default arrows, which have been removed
            while (ai_default != NULL) {
                ppl_snprintfTrunc(buf2, FNAME_LENGTH, "noarrow %6d", ai_default->id);
                ppl_snprintfTrunc(buf, LSTR_LENGTH, "remove arrow %6d", ai_default->id);
                ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, buf2, "", 1, buf);
                i += strlen(out + i);
                ai_default = ai_default->next;
            }
        }
    }

    // Show numbered text labels
    if (ppl_strAutocomplete(word, "labels", 1) >= 0) {
        // Show the text labels, if the item has any
        if (!((xa == NULL) || (ya == NULL) || (za == NULL))) {
            ppllabel_object *li_default_prev = c->set->ppllabel_list_default;
            ppllabel_object *li_default = c->set->ppllabel_list_default;
            showHighlight(c, out, outLen, &i, interactive, 1);
            ppl_snprintfTrunc(out + i, outLen - i, "\n# Numbered text labels:\n\n");
            i += strlen(out + i);
            p = 1;
            showDehighlight(c, out, outLen, &i, interactive);
            // Show each label, in order of number, interleaved with any default labels which have been removed
            for (ppllabel_object *li = *ll; li != NULL; li = li->next) {
                // Stop if the output buffer is nearly full
                if (i > outLen - 1024) {
                    ppl_snprintfTrunc(out + i, outLen - i, "...\n");
                    i += strlen(out + i);
                    break;
                }
                // Show any default labels with lower numbers, which have been removed
                while ((li_default != NULL) && (li_default->id <= li->id)) {
                    // This default label has been removed
                    if (li_default->id < li->id) {
                        ppl_snprintfTrunc(buf2, FNAME_LENGTH, "nolabel %6d", li_default->id);
                        ppl_snprintfTrunc(buf, LSTR_LENGTH, "remove label %6d", li_default->id);
                        ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, buf2, "", 1, buf);
                        i += strlen(out + i);
                    }
                    li_default_prev = li_default;
                    li_default = li_default->next;
                }
                ppllabel_print(c, li, buf, LSTR_LENGTH);
                ppl_snprintfTrunc(buf2, FNAME_LENGTH, "label %6d", li->id);
                if ((unchanged = ((li_default_prev != NULL) && (li_default_prev->id == li->id))) != 0)
                    unchanged = ppllabel_compare(c, li, li_default_prev);
                ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, buf2, buf, unchanged, buf2);
                i += strlen(out + i);
            }
            // Show any remaining default labels, which have been removed
            while (li_default != NULL) {
                ppl_snprintfTrunc(buf2, FNAME_LENGTH, "nolabel %6d", li_default->id);
                ppl_snprintfTrunc(buf, LSTR_LENGTH, "remove label %6d", li_default->id);
                ppl_directive_show3(c, out + i, outLen - i, itemSet, 1, interactive, buf2, "", 1, buf);
                i += strlen(out + i);
                li_default = li_default->next;
            }
        }
    }

    // Show numbered styles
    if ((ppl_strAutocomplete(word, "styles", 1) >= 0) || (ppl_strAutocomplete(word, "linestyles", 1) >= 0)) {
        showHighlight(c, out, outLen, &i, interactive, 1);
        ppl_snprintfTrunc(out + i, outLen - i, "\n# Numbered styles:\n\n");
        i += strlen(out + i);
        p = 1;
        showDehighlight(c, out, outLen, &i, interactive);
        // Show each line style which has been defined
        for (int j = 0; j < MAX_PLOTSTYLES; j++) {
            if (ppl_withWordsCmp_zero(c, &(c->set->plot_styles[j]))) continue;
            // Stop if the output buffer is nearly full
            if (i > outLen - 1024) {
                ppl_snprintfTrunc(out + i, outLen - i, "...\n");
                i += strlen(out + i);
                break;
            }
            ppl_withWordsPrint(c, &(c->set->plot_styles[j]), buf, LSTR_LENGTH);
            ppl_snprintfTrunc(buf2, FNAME_LENGTH, "style %4d", j);
            ppl_directive_show3(c, out + i, outLen - i, itemSet, 0, interactive, buf2, buf,
                                !ppl_withWordsCmp(c, &(c->set->plot_styles[j]), &(c->set->plot_styles_default[j])),
                                buf2);
            i += strlen(out + i);
        }
    }


    // Show variables
    if ((ppl_strAutocomplete(word, "variables", 1) >= 0) || (ppl_strAutocomplete(word, "vars", 1) >= 0) ||
        (ppl_strAutocomplete(word, "uservariables", 1) >= 0) || (ppl_strAutocomplete(word, "uservars", 1) >= 0)) {
        // Show the variables in the local namespace, then the global namespace, then the default namespace
        for (int l = c->ns_ptr; l >= 0; l = (l > 1) ? 1 : l - 1) {
            char *key;
            pplObj *item;
            dictIterator *di = ppl_dictIterateInit(c->namespaces[l]);
            if ((word[0] == 'u') && (l < 1)) break; // Don't show default variables as user variables
            showHighlight(c, out, outLen, &i, interactive, 1);
            if (l > 1) ppl_snprintfTrunc(out + i, outLen - i, "\n# Local variables:\n\n");
            else if (l == 1) ppl_snprintfTrunc(out + i, outLen - i, "\n# Global variables:\n\n");
            else ppl_snprintfTrunc(out + i, outLen - i, "\n# Default variables:\n\n");
            i += strlen(out + i);
            p = 1;
            showDehighlight(c, out, outLen, &i, interactive);
            // Show each variable, skipping functions and undefined or global markers
            while ((item = (pplObj *) ppl_dictIterate(&di, &key)) != NULL) {
                if ((item->objType == PPLOBJ_ZOM) || (item->objType == PPLOBJ_GLOB) || (item->objType == PPLOBJ_FUNC))
                    continue;
                // Stop if the output buffer is nearly full
                if (i > outLen - 1024) {
                    ppl_snprintfTrunc(out + i, outLen - i, "...\n");
                    i += strlen(out + i);
                    break;
                }
                showHighlight(c, out, outLen, &i, interactive, (l == 0));
                ppl_snprintfTrunc(out + i, outLen - i, "%s = ", key);
                i += strlen(out + i);
                pplObjPrint(c, item, key, out + i, 600, 0, 1);
                i += strlen(out + i);
                ppl_snprintfTrunc(out + i, outLen - i, "\n");
                i += strlen(out + i);
                showDehighlight(c, out, outLen, &i, interactive);
            }
        }
    }

    // Show system functions
    if ((ppl_strAutocomplete(word, "functions", 1) >= 0) || (ppl_strAutocomplete(word, "funcs", 1) >= 0)) {
        char *key;
        pplObj *item;
        dictIterator *di = ppl_dictIterateInit(c->namespaces[0]);
        showHighlight(c, out, outLen, &i, interactive, 1);
        ppl_snprintfTrunc(out + i, outLen - i, "\n# System-defined functions:\n\n");
        i += strlen(out + i);
        p = 1;
        showDehighlight(c, out, outLen, &i, interactive);
        // Show each system function
        while ((item = (pplObj *) ppl_dictIterate(&di, &key)) != NULL) {
            if (item->objType != PPLOBJ_FUNC) continue;
            // Stop if the output buffer is nearly full
            if (i > outLen - 1024) {
                ppl_snprintfTrunc(out + i, outLen - i, "...\n");
                i += strlen(out + i);
                break;
            }
            showHighlight(c, out, outLen, &i, interactive, 1);
            pplObjPrint(c, item, key, out + i, 600, 0, 1);
            i += strlen(out + i);
            ppl_snprintfTrunc(out + i, outLen - i, "\n");
            i += strlen(out + i);
            showDehighlight(c, out, outLen, &i, interactive);
        }
    }

    // Show user functions
    if ((ppl_strAutocomplete(word, "functions", 1) >= 0) || (ppl_strAutocomplete(word, "funcs", 1) >= 0) ||
        (ppl_strAutocomplete(word, "userfunctions", 1) >= 0) || (ppl_strAutocomplete(word, "userfuncs", 1) >= 0)) {
        // Show the functions in the local namespace, then the global namespace
        for (int l = c->ns_ptr; l > 0; l = (l > 1) ? 1 : l - 1) {
            char *key;
            pplObj *item;
            dictIterator *di = ppl_dictIterateInit(c->namespaces[l]);
            showHighlight(c, out, outLen, &i, interactive, 1);
            if (l > 1) ppl_snprintfTrunc(out + i, outLen - i, "\n# Local functions:\n\n");
            else ppl_snprintfTrunc(out + i, outLen - i, "\n# Global functions:\n\n");
            i += strlen(out + i);
            p = 1;
            showDehighlight(c, out, outLen, &i, interactive);
            // Show each function
            while ((item = (pplObj *) ppl_dictIterate(&di, &key)) != NULL) {
                if (item->objType != PPLOBJ_FUNC) continue;
                // Stop if the output buffer is nearly full
                if (i > outLen - 1024) {
                    ppl_snprintfTrunc(out + i, outLen - i, "...\n");
                    i += strlen(out + i);
                    break;
                }
                showHighlight(c, out, outLen, &i, interactive, 0);
                pplObjPrint(c, item, key, out + i, 600, 0, 1);
                i += strlen(out + i);
                ppl_snprintfTrunc(out + i, outLen - i, "\n");
                i += strlen(out + i);
                showDehighlight(c, out, outLen, &i, interactive);
            }
        }
    }

    // Show list of recognised units
    if (ppl_strAutocomplete(word, "units", 5) >= 0) {
        showHighlight(c, out, outLen, &i, interactive, 1);
        ppl_snprintfTrunc(out + i, outLen - i, "\n# Recognised physical units:\n\n");
        i += strlen(out + i);
        p = 1;
        showDehighlight(c, out, outLen, &i, interactive);
        l = -1;
        // List the units in alphabetical order, finding the next one each time by a search; l is the last unit listed
        do {
            m = -1;
            // Find the first unit whose name comes after unit l
            for (int j = 0; j < c->unit_pos; j++) {
                if ((l == -1) && (m == -1)) m = j;
                else if ((l == -1) && (ppl_strCmpNoCase(ud[j].nameFs, ud[m].nameFs) < 0)) m = j;
                else if ((l >= 0) && (m == -1) && (ppl_strCmpNoCase(ud[j].nameFs, ud[l].nameFs) > 0)) m = j;
                else if ((l >= 0) && (m >= 0) && (ppl_strCmpNoCase(ud[j].nameFs, ud[m].nameFs) < 0) &&
                         (ppl_strCmpNoCase(ud[j].nameFs, ud[l].nameFs) > 0))
                    m = j;
            }
            l = m;
            // Write each of the unit's names, and what it measures
            if (m != -1) {
                k = 0;
                showHighlight(c, out, outLen, &i, interactive, (ud[m].modified == 0));

#define SHOW_ALL_UNIT_NAMES 0

                // Write the unit's singular full name, followed by its other names, if they are different
                ppl_snprintfTrunc(out + i, outLen - i, "# The '%s', also known as", ud[m].nameFs);
                i += strlen(out + i);
                // The plural full name
                if ((SHOW_ALL_UNIT_NAMES) || (strcmp(ud[m].nameFp, ud[m].nameFs) != 0)) {
                    ppl_snprintfTrunc(out + i, outLen - i, " '%s' or", ud[m].nameFp);
                    i += strlen(out + i);
                    k = 1;
                }
                // The singular abbreviation
                if ((SHOW_ALL_UNIT_NAMES) || (strcmp(ud[m].nameAs, ud[m].nameFs) != 0)) {
                    ppl_snprintfTrunc(out + i, outLen - i, " '%s' or", ud[m].nameAs);
                    i += strlen(out + i);
                    k = 1;
                }
                if ((SHOW_ALL_UNIT_NAMES) ||
                    ((strcmp(ud[m].nameAp, ud[m].nameAs) != 0) && (strcmp(ud[m].nameAp, ud[m].nameFp) != 0))) {
                    ppl_snprintfTrunc(out + i, outLen - i, " '%s' or", ud[m].nameAp);
                    i += strlen(out + i);
                    k = 1;
                }

                // Alternative names
                if (ud[m].alt1 != NULL) {
                    ppl_snprintfTrunc(out + i, outLen - i, " '%s' or", ud[m].alt1);
                    i += strlen(out + i);
                    k = 1;
                }
                // Alternative names
                if (ud[m].alt2 != NULL) {
                    ppl_snprintfTrunc(out + i, outLen - i, " '%s' or", ud[m].alt2);
                    i += strlen(out + i);
                    k = 1;
                }
                // Alternative names
                if (ud[m].alt3 != NULL) {
                    ppl_snprintfTrunc(out + i, outLen - i, " '%s' or", ud[m].alt3);
                    i += strlen(out + i);
                    k = 1;
                }
                // Alternative names
                if (ud[m].alt4 != NULL) {
                    ppl_snprintfTrunc(out + i, outLen - i, " '%s' or", ud[m].alt4);
                    i += strlen(out + i);
                    k = 1;
                }
                // If there were no other names, remove " also known as"; otherwise replace the final " or" with a comma
                if (k == 0) {
                    i -= 15;
                } else {
                    i -= 3;
                    out[i++] = ',';
                }
                ppl_snprintfTrunc(out + i, outLen - i, " is a unit of %s", ud[m].quantity);
                i += strlen(out + i);
                // A comment about the unit
                if (ud[m].comment != NULL) {
                    ppl_snprintfTrunc(out + i, outLen - i, " (%s)", ud[m].comment);
                    i += strlen(out + i);
                }
                ppl_snprintfTrunc(out + i, outLen - i, ".\n");
                i += strlen(out + i);
                showDehighlight(c, out, outLen, &i, interactive);
            }
        } while (m != -1);
    }

    // Print the output, and free the buffers
    if (p != 0) ppl_report(&c->errcontext, out);
    free(out);
    out = NULL;
    free(buf);
    buf = NULL;
    free(buf2);
    buf2 = NULL;
    return p;
}


//! ppl_directive_show - Implement the show command, which displays the values of Pyxplot's settings
//! \param [in] c - The Pyxplot context
//! \param [in] pl - The compiled command line, used to report errors
//! \param [in] in - The parser output for the command, containing the values of its parameters
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal

void ppl_directive_show(ppl_context *c, const parserLine *pl, const parserOutput *in, int interactive) {
    // Variables pointing to the settings to show, which belong to a multiplot item or to the current settings
    char msg[PPL_MSG_LENGTH];
    const pplObj *stk = in->stk;
    char itemSet[32];
    pplset_graph *sg;
    pplarrow_object **al;
    ppllabel_object **ll;
    pplset_axis *xa, *ya, *za;

    // Only highlight changed settings if the terminal supports color
    interactive = (interactive && (c->errcontext.session_default.color == SW_ONOFF_ON));

    // Show the current settings, or those of a multiplot item
    if (stk[PARSE_show_editno].objType != PPLOBJ_NUM) {
        sg = &c->set->graph_current;
        al = &c->set->pplarrow_list;
        ll = &c->set->ppllabel_list;
        xa = c->set->XAxes;
        ya = c->set->YAxes;
        za = c->set->ZAxes;
        itemSet[0] = '\0';
    } else {
        const canvas_itemlist *canvas_items = c->canvas_items;
        const int editNo = (int) round(stk[PARSE_show_editno].real);
        // The multiplot item number must be valid
        if ((editNo < 1) || (editNo > MULTIPLOT_MAXINDEX) || (canvas_items == NULL)) {
            ppl_snprintfTrunc(msg, sizeof(msg), "No multiplot item with index %d.", editNo);
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1, msg);
            return;
        }
        canvas_item *ptr = canvas_items->first;
        while ((ptr != NULL) && (ptr->id != editNo)) ptr = ptr->next;
        // Report a multiplot item which does not exist
        if (ptr == NULL) {
            ppl_snprintfTrunc(msg, sizeof(msg), "No multiplot item with index %d.", editNo);
            ppl_error(&c->errcontext, ERR_GENERIC, -1, -1, msg);
            return;
        }

        // Use the multiplot item's settings
        sg = &(ptr->settings);
        al = &(ptr->arrow_list);
        ll = &(ptr->label_list);
        xa = ptr->XAxes;
        ya = ptr->YAxes;
        za = ptr->ZAxes;
        ppl_snprintfTrunc(itemSet, sizeof(itemSet), "item %d ", editNo);
    }

    // With no words after show, print help on the show command
    int pos = PARSE_show_0setting_list;
    // No words were given
    if ((stk[pos].objType != PPLOBJ_NUM) || (stk[pos].real <= 0)) {
        ppl_error(&c->errcontext, ERR_PREFORMED, -1, -1, ppltxt_show);
    } else {
        char textBuffer[SSTR_LENGTH], *showWord = NULL;
        int p = 0, i = 0;
        if (interactive !=
            0) // On interactive sessions, highlight those settings which have been manually set by the user
        {
            sprintf(textBuffer + i, "%sSettings which have not been changed by the user are shown in %s.%s\n",
                    *(char **) ppl_fetchSettingName(&c->errcontext, c->errcontext.session_default.color_rep,
                                                    SW_TERMCOL_INT, SW_TERMCOL_TXT, sizeof(char *)),
                    *(char **) ppl_fetchSettingName(&c->errcontext, c->errcontext.session_default.color_rep,
                                                    SW_TERMCOL_INT, SW_TERMCOL_STR, sizeof(char *)),
                    *(char **) ppl_fetchSettingName(&c->errcontext, SW_TERMCOL_NOR, SW_TERMCOL_INT, SW_TERMCOL_TXT,
                                                    sizeof(char *)));
            i += strlen(textBuffer + i);
            sprintf(textBuffer + i, "%sSettings which have been changed by the user are shown in %s.%s\n",
                    *(char **) ppl_fetchSettingName(&c->errcontext, c->errcontext.session_default.color_wrn,
                                                    SW_TERMCOL_INT, SW_TERMCOL_TXT, sizeof(char *)),
                    *(char **) ppl_fetchSettingName(&c->errcontext, c->errcontext.session_default.color_wrn,
                                                    SW_TERMCOL_INT, SW_TERMCOL_STR, sizeof(char *)),
                    *(char **) ppl_fetchSettingName(&c->errcontext, SW_TERMCOL_NOR, SW_TERMCOL_INT, SW_TERMCOL_TXT,
                                                    sizeof(char *)));
            i += strlen(textBuffer + i);
            ppl_report(&c->errcontext, textBuffer);
        }
        // Show the settings named by each word; "all" shows everything
        while ((stk[pos].objType == PPLOBJ_NUM) && (stk[pos].real > 0)) {
            pos = (int) round(stk[pos].real);
            showWord = (char *) stk[pos + PARSE_show_setting_0setting_list].auxil;
            // "all" shows every group of settings
            if (ppl_strAutocomplete(showWord, "all", 1) >= 0) {
                ppl_directive_show2(c, "settings", itemSet, interactive, sg, al, ll, xa, ya, za);
                ppl_directive_show2(c, "axes_", itemSet, interactive, sg, al, ll, xa, ya, za);
                ppl_directive_show2(c, "arrows", itemSet, interactive, sg, al, ll, xa, ya, za);
                ppl_directive_show2(c, "labels", itemSet, interactive, sg, al, ll, xa, ya, za);
                ppl_directive_show2(c, "linestyles", itemSet, interactive, sg, al, ll, xa, ya, za);
                ppl_directive_show2(c, "variables", itemSet, interactive, sg, al, ll, xa, ya, za);
                ppl_directive_show2(c, "userfunctions", itemSet, interactive, sg, al, ll, xa, ya, za);
                //ppl_directive_show2(c, "units"         ,itemSet, interactive, sg, al, ll, xa, ya, za);
                p = 1;
            } else {
                p = (ppl_directive_show2(c, showWord, itemSet, interactive, sg, al, ll, xa, ya, za) || p);
            }
        }
        // Report a word which matched nothing
        if ((p == 0) && (showWord != NULL)) {
            snprintf(c->errStat.errBuff, LSTR_LENGTH, "Unrecognised show option '%s'.\n\n%s", showWord, ppltxt_show);
            ppl_tbAddLine(c, pl, ERR_SYNTAX, in->stkCharPos[pos + PARSE_show_setting_0setting_list], "");
        }
    }
    return;
}
