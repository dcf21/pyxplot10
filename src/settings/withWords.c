// withWords.c
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

// The withWords structure records the style options which follow the keyword "with" in commands such as plot and arrow,
// e.g. "with lines color red linewidth 2". This file reads them from parsed commands, merges styles in order of
// priority (including numbered styles set with "set style"), compares, copies and frees them, and writes them back out
// as text for the show and save commands.

#define _PPL_SETTINGS_C 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"

#include "pplConstants.h"
#include "coreUtils/errorReport.h"
#include "expressions/expCompile_fns.h"
#include "parser/cmdList.h"
#include "parser/parser.h"
#include "settings/colors.h"
#include "settings/epsColors.h"
#include "settings/settings.h"
#include "settings/settingTypes.h"
#include "settings/withWords_fns.h"

#include "userspace/context.h"
#include "userspace/unitsDisp.h"
#include "userspace/pplObj.h"

// -----------------------------------------------
// ROUTINES FOR MANIPULATING WITH_WORDS STRUCTURES
// -----------------------------------------------

//! ppl_withWordsZero - Reset a set of plot style modifiers (the words which follow "with" in a plot command) so that
//! none of them are set
//! \param [in] context - The Pyxplot context
//! \param [out] a - The plot style modifiers to reset

void ppl_withWordsZero(const ppl_context *context, withWords *a) {
    a->color = a->fillcolor = a->linespoints = a->linetype = a->pointtype = a->style = a->Col1234Space =
            a->FillCol1234Space = 0;
    a->linewidth = a->pointlinewidth = a->pointsize = 1.0;
    a->color1 = a->color2 = a->color3 = a->color4 = a->fillcolor1 = a->fillcolor2 = a->fillcolor3 = a->fillcolor4 = 0.0;
    a->EXPlinetype = a->EXPlinewidth = a->EXPpointlinewidth = a->EXPpointsize = a->EXPpointtype = NULL;
    a->EXPcolor = a->EXPfillcolor = NULL;
    a->USEcolor = a->USEfillcolor = a->USElinespoints = a->USElinetype = a->USElinewidth = a->USEpointlinewidth =
            a->USEpointsize = a->USEpointtype = a->USEstyle = a->USEcolor1234 = a->USEfillcolor1234 = 0;
    a->AUTOcolor = a->AUTOlinetype = a->AUTOpointtype = 0;
    return;
}

//! ppl_withWordsFromDict - Read the plot style modifiers specified after "with" in a parsed command
//! \param [in] context - The Pyxplot context
//! \param [in] in - The parser output for the command
//! \param [in] pl - The parsed command line, used to report errors
//! \param [in] ptab - Table mapping parser variable indices onto positions in <in->stk>
//! \param [in] stkbase - Offset to add to positions in <in->stk>
//! \param [out] out - The plot style modifiers which were read

void ppl_withWordsFromDict(ppl_context *context, parserOutput *in, const parserLine *pl, const int *ptab,
                           const int stkbase, withWords *out) {
    pplObj *stk = in->stk + stkbase;
    int tempint, i, pos, got;
    double tempdbl;
    char *tempstr;
    pplExpr *tempexp;
    ppl_withWordsZero(context, out);

    // read color names
    ppl_colorFromDict(context, in, pl, ptab, stkbase, 0, &out->color, &out->Col1234Space, &out->EXPcolor, &out->color1,
                      &out->color2, &out->color3, &out->color4, &out->USEcolor, &out->USEcolor1234);
    ppl_colorFromDict(context, in, pl, ptab, stkbase, 1, &out->fillcolor, &out->FillCol1234Space, &out->EXPfillcolor,
                      &out->fillcolor1, &out->fillcolor2, &out->fillcolor3, &out->fillcolor4, &out->USEfillcolor,
                      &out->USEfillcolor1234);

    // Other settings
    pos = ptab[PARSE_INDEX_linetype];
    got = (pos >= 0) && (stk[pos].objType == PPLOBJ_NUM);
    // A line type given as a number is stored directly; one given as an expression (for each data point) is stored as
    // an expression. The other options below work in the same way.
    if (got) {
        tempint = (int) round(stk[pos].real);
        out->linetype = tempint;
        out->USElinetype = 1;
    }
    pos = ptab[PARSE_INDEX_linetype];
    got = (pos >= 0) && (stk[pos].objType == PPLOBJ_EXP);
    // Line type as an expression
    if (got) {
        tempexp = (pplExpr *) stk[pos].auxil;
        out->EXPlinetype = tempexp;
        tempexp->refCount++;
    }
    pos = ptab[PARSE_INDEX_linewidth];
    got = (pos >= 0) && (stk[pos].objType == PPLOBJ_NUM);
    // Line width as a number
    if (got) {
        tempdbl = stk[pos].real;
        out->linewidth = tempdbl;
        out->USElinewidth = 1;
    }
    pos = ptab[PARSE_INDEX_linewidth];
    got = (pos >= 0) && (stk[pos].objType == PPLOBJ_EXP);
    // Line width as an expression
    if (got) {
        tempexp = (pplExpr *) stk[pos].auxil;
        out->EXPlinewidth = tempexp;
        tempexp->refCount++;
    }
    pos = ptab[PARSE_INDEX_pointsize];
    got = (pos >= 0) && (stk[pos].objType == PPLOBJ_NUM);
    // Point size as a number
    if (got) {
        tempdbl = stk[pos].real;
        out->pointsize = tempdbl;
        out->USEpointsize = 1;
    }
    pos = ptab[PARSE_INDEX_pointsize];
    got = (pos >= 0) && (stk[pos].objType == PPLOBJ_EXP);
    // Point size as an expression
    if (got) {
        tempexp = (pplExpr *) stk[pos].auxil;
        out->EXPpointsize = tempexp;
        tempexp->refCount++;
    }
    pos = ptab[PARSE_INDEX_pointtype];
    got = (pos >= 0) && (stk[pos].objType == PPLOBJ_NUM);
    // Point type as a number
    if (got) {
        tempint = (int) round(stk[pos].real);
        out->pointtype = tempint;
        out->USEpointtype = 1;
    }
    pos = ptab[PARSE_INDEX_pointtype];
    got = (pos >= 0) && (stk[pos].objType == PPLOBJ_EXP);
    // Point type as an expression
    if (got) {
        tempexp = (pplExpr *) stk[pos].auxil;
        out->EXPpointtype = tempexp;
        tempexp->refCount++;
    }
    pos = ptab[PARSE_INDEX_style_number];
    got = (pos >= 0) && (stk[pos].objType == PPLOBJ_NUM);
    // The number of a style set with "set style"
    if (got) {
        tempint = (int) round(stk[pos].real);
        out->style = tempint;
        out->USEstyle = 1;
    }
    pos = ptab[PARSE_INDEX_pointlinewidth];
    got = (pos >= 0) && (stk[pos].objType == PPLOBJ_NUM);
    // Point line width as a number
    if (got) {
        tempdbl = stk[pos].real;
        out->pointlinewidth = tempdbl;
        out->USEpointlinewidth = 1;
    }
    pos = ptab[PARSE_INDEX_pointlinewidth];
    got = (pos >= 0) && (stk[pos].objType == PPLOBJ_EXP);
    // Point line width as an expression
    if (got) {
        tempexp = (pplExpr *) stk[pos].auxil;
        out->EXPpointlinewidth = tempexp;
        tempexp->refCount++;
    }
    pos = ptab[PARSE_INDEX_style];
    got = (pos >= 0) && (stk[pos].objType == PPLOBJ_STR);
    // The plot style, e.g. lines or points
    if (got) {
        tempstr = (char *) stk[pos].auxil;
        i = ppl_fetchSettingByName(&context->errcontext, tempstr, SW_STYLE_INT, SW_STYLE_STR);
        out->linespoints = i;
        out->USElinespoints = 1;
    }
    return;
}


//! ppl_withWordsCmp_zero - Test whether a set of plot style modifiers is empty
//! \param [in] context - The Pyxplot context
//! \param [in] a - The plot style modifiers to test
//! \return One if none of the modifiers are set, zero otherwise

int ppl_withWordsCmp_zero(const ppl_context *context, const withWords *a) {
    if (a->EXPlinetype != NULL) return 0;
    if (a->EXPlinewidth != NULL) return 0;
    if (a->EXPpointlinewidth != NULL) return 0;
    if (a->EXPpointsize != NULL) return 0;
    if (a->EXPpointtype != NULL) return 0;
    if (a->EXPcolor != NULL) return 0;
    if (a->EXPfillcolor != NULL) return 0;
    if (a->USEcolor) return 0;
    if (a->USEfillcolor) return 0;
    if (a->USElinespoints) return 0;
    if (a->USElinetype) return 0;
    if (a->USElinewidth) return 0;
    if (a->USEpointlinewidth) return 0;
    if (a->USEpointsize) return 0;
    if (a->USEpointtype) return 0;
    if (a->USEstyle) return 0;
    if (a->USEcolor1234) return 0;
    if (a->USEfillcolor1234) return 0;
    return 1;
}

//! ppl_withWordsCmp - Test whether two sets of plot style modifiers are identical
//! \param [in] context - The Pyxplot context
//! \param [in] a - The first set of modifiers
//! \param [in] b - The second set of modifiers
//! \return One if the modifiers are identical, zero otherwise

int ppl_withWordsCmp(ppl_context *context, const withWords *a, const withWords *b) {
    // Check that the range of items which are defined in both structures are the same
    if ((a->EXPcolor == NULL) != (b->EXPcolor == NULL)) return 0;
    if ((a->EXPfillcolor == NULL) != (b->EXPfillcolor == NULL)) return 0;
    if ((a->USElinespoints != b->USElinespoints)) return 0;
    if ((a->EXPlinetype == NULL) != (b->EXPlinetype == NULL)) return 0;
    if ((a->EXPlinetype == NULL) && (a->USElinetype != b->USElinetype)) return 0;
    if ((a->EXPlinewidth == NULL) != (b->EXPlinewidth == NULL)) return 0;
    if ((a->EXPlinewidth == NULL) && (a->USElinewidth != b->USElinewidth)) return 0;
    if ((a->EXPpointlinewidth == NULL) != (b->EXPpointlinewidth == NULL)) return 0;
    if ((a->EXPpointlinewidth == NULL) && (a->USEpointlinewidth != b->USEpointlinewidth)) return 0;
    if ((a->EXPpointsize == NULL) != (b->EXPpointsize == NULL)) return 0;
    if ((a->EXPpointsize == NULL) && (a->USEpointsize != b->USEpointsize)) return 0;
    if ((a->EXPpointtype == NULL) != (b->EXPpointtype == NULL)) return 0;
    if ((a->EXPpointtype == NULL) && (a->USEpointtype != b->USEpointtype)) return 0;
    if ((a->USEstyle != b->USEstyle)) return 0;

    // Check that the actual values are the same in both structures
    if (a->EXPcolor != NULL) {
        if ((strcmp(a->EXPcolor->ascii, b->EXPcolor->ascii) != 0)) return 0;
        if (a->Col1234Space != b->Col1234Space) return 0;
    } else if ((a->USEcolor1234) &&
               ((a->color1 != b->color1) || (a->color2 != b->color2) || (a->color3 != b->color3) ||
                (a->color4 != b->color4) || (a->Col1234Space != b->Col1234Space)))
        return 0;
    else if ((a->USEcolor) && ((a->color != b->color))) return 0;
    // Compare fill colors given as expressions, components or named colors
    if (a->EXPfillcolor != NULL) {
        if ((strcmp(a->EXPfillcolor->ascii, b->EXPfillcolor->ascii) != 0)) return 0;
        if (a->FillCol1234Space != b->FillCol1234Space) return 0;
    } else if ((a->USEfillcolor1234) &&
               ((a->fillcolor1 != b->fillcolor1) || (a->fillcolor2 != b->fillcolor2) ||
                (a->fillcolor3 != b->fillcolor3) || (a->fillcolor4 != b->fillcolor4) ||
                (a->FillCol1234Space != b->FillCol1234Space)))
        return 0;
    else if ((a->USEfillcolor) && ((a->fillcolor != b->fillcolor))) return 0;
    if ((a->USElinespoints) && ((a->linespoints != b->linespoints))) return 0;
    if ((a->EXPlinetype != NULL) && ((strcmp(a->EXPlinetype->ascii, b->EXPlinetype->ascii) != 0))) return 0;
    else if ((a->USElinetype) && ((a->linetype != b->linetype))) return 0;
    if ((a->EXPlinewidth != NULL) && ((strcmp(a->EXPlinewidth->ascii, b->EXPlinewidth->ascii) != 0))) return 0;
    else if ((a->USElinewidth) && ((a->linewidth != b->linewidth))) return 0;
    if ((a->EXPpointlinewidth != NULL) && ((strcmp(a->EXPpointlinewidth->ascii, b->EXPpointlinewidth->ascii) != 0)))
        return 0;
    else if ((a->USEpointlinewidth) && ((a->pointlinewidth != b->pointlinewidth))) return 0;
    if ((a->EXPpointsize != NULL) && ((strcmp(a->EXPpointsize->ascii, b->EXPpointsize->ascii) != 0))) return 0;
    else if ((a->USEpointsize) && ((a->pointsize != b->pointsize))) return 0;
    if ((a->EXPpointtype != NULL) && ((strcmp(a->EXPpointtype->ascii, b->EXPpointtype->ascii) != 0))) return 0;
    else if ((a->USEpointtype) && ((a->pointtype != b->pointtype))) return 0;
    if ((a->USEstyle) && ((a->style != b->style))) return 0;

    return 1; // We have not found any differences
}

//! ppl_withWordsMerge - Merge up to five sets of plot style modifiers, with modifiers set in earlier sets taking
//! priority over those set in later ones. Numbered plot styles ("style N") are expanded if requested.
//! \param [in] context - The Pyxplot context
//! \param [out] out - The merged set of modifiers
//! \param [in] a - The set of modifiers with the highest priority (may be NULL)
//! \param [in] b - The set of modifiers with the second-highest priority (may be NULL)
//! \param [in] c - The set of modifiers with the third-highest priority (may be NULL)
//! \param [in] d - The set of modifiers with the fourth-highest priority (may be NULL)
//! \param [in] e - The set of modifiers with the lowest priority (may be NULL)
//! \param [in] ExpandStyles - If true, substitute the definitions of numbered plot styles referenced with "style N"

void ppl_withWordsMerge(ppl_context *context, withWords *out, const withWords *a, const withWords *b,
                        const withWords *c, const withWords *d, const withWords *e, const unsigned char ExpandStyles) {
    int i;
    const withWords *InputArray[25] = {a, b, c, d, e};
    unsigned char BlockStyleSubstitution[25] = {0, 0, 0, 0, 0};
    const withWords *x;

    // The inputs are applied in order of increasing priority, from e up to a, so that options set in a override those
    // set in the others. A numbered style is expanded by inserting it into the list just below the input which refers
    // to it, so that options given explicitly override those of the style.
    ppl_withWordsZero(context, out);

    // Apply each input in turn
    for (i = 4; i >= 0; i--) {
        // Numbered styles which refer to one another in a loop would expand forever
        if (i > 24) {
            ppl_error(&context->errcontext, ERR_GENERIC, -1, -1,
                      "Iteration depth exceeded whilst substituting plot styles. Infinite plot style loop suspected.");
            return;
        } // i can reach 24 when recursion happens
        x = InputArray[i];
        if (x == NULL) continue;
        if ((x->USEstyle) && (!BlockStyleSubstitution[i])) // Substitute for numbered plot styles
        {
            // Insert the numbered style below this input, and process it next, before returning to this input
            if (ExpandStyles) {
                BlockStyleSubstitution[i] = 1; // Only do this once, to avoid infinite loop
                BlockStyleSubstitution[i + 1] = 0; // Allow recursive substitutions
                InputArray[i + 1] = &(context->set->plot_styles[x->style]);
                i += 2; // Recurse
                continue;
            } else {
                out->style = x->style;
                out->USEstyle = 1;
            }
        }
        // A color given by its components overrides any other color
        if (x->USEcolor1234) {
            out->color1 = x->color1;
            out->color2 = x->color2;
            out->color3 = x->color3;
            out->color4 = x->color4;
            out->Col1234Space = x->Col1234Space;
            out->USEcolor1234 = 1;
            out->USEcolor = 0;
            pplExpr_free(out->EXPcolor);
            out->EXPcolor = NULL;
            out->AUTOcolor = x->AUTOcolor;
        }
        // A named color, or palette color, overrides any other color
        if (x->USEcolor) {
            out->color = x->color;
            out->USEcolor = 1;
            out->USEcolor1234 = 0;
            pplExpr_free(out->EXPcolor);
            out->EXPcolor = NULL;
            out->AUTOcolor = x->AUTOcolor;
        }
        // A color given by an expression overrides any other color
        if (x->EXPcolor != NULL) {
            pplExpr_free(out->EXPcolor);
            out->EXPcolor = x->EXPcolor;
            out->EXPcolor->refCount++;
            out->USEcolor = 0;
            out->USEcolor1234 = 0;
            out->AUTOcolor = x->AUTOcolor;
        }
        // A fill color given by its components overrides any other fill color
        if (x->USEfillcolor1234) {
            out->fillcolor1 = x->fillcolor1;
            out->fillcolor2 = x->fillcolor2;
            out->fillcolor3 = x->fillcolor3;
            out->fillcolor4 = x->fillcolor4;
            out->FillCol1234Space = x->FillCol1234Space;
            out->USEfillcolor1234 = 1;
            out->USEfillcolor = 0;
            pplExpr_free(out->EXPfillcolor);
            out->EXPfillcolor = NULL;
        }
        // A named fill color overrides any other fill color
        if (x->USEfillcolor) {
            out->fillcolor = x->fillcolor;
            out->USEfillcolor = 1;
            out->USEfillcolor1234 = 0;
            pplExpr_free(out->EXPfillcolor);
            out->EXPfillcolor = NULL;
        }
        // A fill color given by an expression overrides any other fill color
        if (x->EXPfillcolor != NULL) {
            pplExpr_free(out->EXPfillcolor);
            out->EXPfillcolor = x->EXPfillcolor;
            out->EXPfillcolor->refCount++;
            out->USEfillcolor = 0;
            out->USEfillcolor1234 = 0;
        }
        // The plot style
        if (x->USElinespoints) {
            out->linespoints = x->linespoints;
            out->USElinespoints = 1;
        }
        // A line type given as a number overrides an expression
        if (x->USElinetype) {
            out->linetype = x->linetype;
            out->USElinetype = 1;
            pplExpr_free(out->EXPlinetype);
            out->EXPlinetype = NULL;
            out->AUTOlinetype = x->AUTOlinetype;
        }
        // A line type given as an expression
        if (x->EXPlinetype != NULL) {
            pplExpr_free(out->EXPlinetype);
            out->EXPlinetype = x->EXPlinetype;
            out->EXPlinetype->refCount++;
        }
        // A line width given as a number overrides an expression
        if (x->USElinewidth) {
            out->linewidth = x->linewidth;
            out->USElinewidth = 1;
            pplExpr_free(out->EXPlinewidth);
            out->EXPlinewidth = NULL;
        }
        // A line width given as an expression overrides a number
        if (x->EXPlinewidth != NULL) {
            pplExpr_free(out->EXPlinewidth);
            out->EXPlinewidth = x->EXPlinewidth;
            out->EXPlinewidth->refCount++;
            out->USElinewidth = 0;
        }
        // A point line width given as a number overrides an expression
        if (x->USEpointlinewidth) {
            out->pointlinewidth = x->pointlinewidth;
            out->USEpointlinewidth = 1;
            pplExpr_free(out->EXPpointlinewidth);
            out->EXPpointlinewidth = NULL;
        }
        // A point line width given as an expression overrides a number
        if (x->EXPpointlinewidth != NULL) {
            pplExpr_free(out->EXPpointlinewidth);
            out->EXPpointlinewidth = x->EXPpointlinewidth;
            out->EXPpointlinewidth->refCount++;
            out->USEpointlinewidth = 0;
        }
        // A point size given as a number overrides an expression
        if (x->USEpointsize) {
            out->pointsize = x->pointsize;
            out->USEpointsize = 1;
            pplExpr_free(out->EXPpointsize);
            out->EXPpointsize = NULL;
        }
        // A point size given as an expression overrides a number
        if (x->EXPpointsize != NULL) {
            pplExpr_free(out->EXPpointsize);
            out->EXPpointsize = x->EXPpointsize;
            out->EXPpointsize->refCount++;
            out->USEpointsize = 0;
        }
        // A point type given as a number overrides an expression
        if (x->USEpointtype) {
            out->pointtype = x->pointtype;
            out->USEpointtype = 1;
            pplExpr_free(out->EXPpointtype);
            out->EXPpointtype = NULL;
            out->AUTOpointtype = x->AUTOpointtype;
        }
        // A point type given as an expression
        if (x->EXPpointtype != NULL) {
            pplExpr_free(out->EXPpointtype);
            out->EXPpointtype = x->EXPpointtype;
            out->EXPpointtype->refCount++;
        }
    }
    return;
}


//! ppl_withWordsPrint - Write a textual description of a set of plot style modifiers, in the syntax used after
//! "with" in a plot command
//! \param [in] context - The Pyxplot context
//! \param [in] defn - The plot style modifiers to describe
//! \param [out] out - Character buffer into which to write the description
//! \param [in] outLen - The size of the buffer <out>

void ppl_withWordsPrint(ppl_context *context, const withWords *defn, char *out, const size_t outLen) {
    // Write each option which is set, in the syntax of the with clause
    int i = 0;
    out[i] = '\0';
    // The plot style
    if (defn->USElinespoints) {
        ppl_strAppendf(out, outLen, &i, "%s ",
                       *(char **) ppl_fetchSettingName(&context->errcontext, defn->linespoints, SW_STYLE_INT,
                                                       (void *) SW_STYLE_STR, sizeof(char *)));
        i += strlen(out + i);
    }
    // The color, as an expression, components, or a named color
    if (defn->EXPcolor != NULL) {
        ppl_strAppendf(out, outLen, &i, "color %s", defn->EXPcolor->ascii);
    } else if (defn->USEcolor1234) {
        if (defn->Col1234Space == SW_COLSPACE_RGB)
            ppl_strAppendf(out, outLen, &i, "color rgb(%s,%s,%s) ", ppl_numericDisplayDefault(context, defn->color1, 0),
                           ppl_numericDisplayDefault(context, defn->color2, 1),
                           ppl_numericDisplayDefault(context, defn->color3, 2));
        else if (defn->Col1234Space == SW_COLSPACE_HSB)
            ppl_strAppendf(out, outLen, &i, "color hsb(%s,%s,%s) ", ppl_numericDisplayDefault(context, defn->color1, 0),
                           ppl_numericDisplayDefault(context, defn->color2, 1),
                           ppl_numericDisplayDefault(context, defn->color3, 2));
        else if (defn->Col1234Space == SW_COLSPACE_CMYK)
            ppl_strAppendf(out, outLen, &i, "color cmyk(%s,%s,%s,%s) ",
                           ppl_numericDisplayDefault(context, defn->color1, 0),
                           ppl_numericDisplayDefault(context, defn->color2, 1),
                           ppl_numericDisplayDefault(context, defn->color3, 2),
                           ppl_numericDisplayDefault(context, defn->color4, 3));
    } else if (defn->USEcolor) {
        ppl_strAppendf(out, outLen, &i, "color %s ",
                       *(char **) ppl_fetchSettingName(&context->errcontext, defn->color, SW_COLOR_INT,
                                                       (void *) SW_COLOR_STR, sizeof(char *)));
    }
    i += strlen(out + i);
    // The fill color, as an expression, components, or a named color
    if (defn->EXPfillcolor != NULL) {
        ppl_strAppendf(out, outLen, &i, "fillcolor %s", defn->EXPfillcolor->ascii);
    } else if (defn->USEfillcolor1234) {
        if (defn->FillCol1234Space == SW_COLSPACE_RGB)
            ppl_strAppendf(out, outLen, &i, "fillcolor rgb(%s,%s,%s) ",
                           ppl_numericDisplayDefault(context, defn->fillcolor1, 0),
                           ppl_numericDisplayDefault(context, defn->fillcolor2, 1),
                           ppl_numericDisplayDefault(context, defn->fillcolor3, 2));
        else if (defn->FillCol1234Space == SW_COLSPACE_HSB)
            ppl_strAppendf(out, outLen, &i, "fillcolor hsb(%s,%s,%s) ",
                           ppl_numericDisplayDefault(context, defn->fillcolor1, 0),
                           ppl_numericDisplayDefault(context, defn->fillcolor2, 1),
                           ppl_numericDisplayDefault(context, defn->fillcolor3, 2));
        else if (defn->FillCol1234Space == SW_COLSPACE_CMYK)
            ppl_strAppendf(out, outLen, &i, "fillcolor cmyk(%s,%s,%s,%s) ",
                           ppl_numericDisplayDefault(context, defn->fillcolor1, 0),
                           ppl_numericDisplayDefault(context, defn->fillcolor2, 1),
                           ppl_numericDisplayDefault(context, defn->fillcolor3, 2),
                           ppl_numericDisplayDefault(context, defn->fillcolor4, 2));
    } else if (defn->USEfillcolor) {
        ppl_strAppendf(out, outLen, &i, "fillcolor %s ",
                       *(char **) ppl_fetchSettingName(&context->errcontext, defn->fillcolor, SW_COLOR_INT,
                                                       (void *) SW_COLOR_STR, sizeof(char *)));
    }
    i += strlen(out + i);
    // The line type
    if (defn->EXPlinetype != NULL) {
        ppl_strAppendf(out, outLen, &i, "linetype %s ", defn->EXPlinetype->ascii);
        i += strlen(out + i);
    } else if (defn->USElinetype) {
        ppl_strAppendf(out, outLen, &i, "linetype %d ", defn->linetype);
        i += strlen(out + i);
    }
    // The line width
    if (defn->EXPlinewidth != NULL) {
        ppl_strAppendf(out, outLen, &i, "linewidth %s ", defn->EXPlinewidth->ascii);
        i += strlen(out + i);
    } else if (defn->USElinewidth) {
        ppl_strAppendf(out, outLen, &i, "linewidth %s ", ppl_numericDisplayDefault(context, defn->linewidth, 0));
        i += strlen(out + i);
    }
    // The point line width
    if (defn->EXPpointlinewidth != NULL) {
        ppl_strAppendf(out, outLen, &i, "pointlinewidth %s ", defn->EXPpointlinewidth->ascii);
        i += strlen(out + i);
    } else if (defn->USEpointlinewidth) {
        ppl_strAppendf(out, outLen, &i, "pointlinewidth %s ",
                       ppl_numericDisplayDefault(context, defn->pointlinewidth, 0));
        i += strlen(out + i);
    }
    // The point size
    if (defn->EXPpointsize != NULL) {
        ppl_strAppendf(out, outLen, &i, "pointsize %s ", defn->EXPpointsize->ascii);
        i += strlen(out + i);
    } else if (defn->USEpointsize) {
        ppl_strAppendf(out, outLen, &i, "pointsize %s ", ppl_numericDisplayDefault(context, defn->pointsize, 0));
        i += strlen(out + i);
    }
    // The point type
    if (defn->EXPpointtype != NULL) {
        ppl_strAppendf(out, outLen, &i, "pointtype %s ", defn->EXPpointtype->ascii);
        i += strlen(out + i);
    } else if (defn->USEpointtype) {
        ppl_strAppendf(out, outLen, &i, "pointtype %d ", defn->pointtype);
        i += strlen(out + i);
    }
    // The number of the style
    if (defn->USEstyle) {
        ppl_strAppendf(out, outLen, &i, "style %d ", defn->style);
        i += strlen(out + i);
    }
    out[i] = '\0';
    return;
}

//! ppl_withWordsAppend - Append " with <style>" to a string, describing a set of style keywords in the syntax of the
//! with modifier, unless the style is empty
//! \param [in] context - The Pyxplot context
//! \param [in] defn - The style keywords to describe
//! \param [in,out] out - The buffer containing the string to append to
//! \param [in] outLen - The size of the buffer <out>
//! \param [in,out] pos - The position of the end of the string in <out>, which is advanced past the appended text

void ppl_withWordsAppend(ppl_context *context, const withWords *defn, char *out, const size_t outLen, int *pos) {
    char words[LSTR_LENGTH];
    ppl_withWordsPrint(context, defn, words, sizeof(words));
    if (words[0] != '\0') ppl_strAppendf(out, outLen, pos, " with %s", words);
}

//! ppl_withWordsDestroy - Free the expressions referenced by a set of plot style modifiers
//! \param [in] context - The Pyxplot context
//! \param [in|out] a - The plot style modifiers to clean up

void ppl_withWordsDestroy(const ppl_context *context, withWords *a) {
    // Release each of the expressions which the structure holds
    if (a->EXPlinetype != NULL) {
        pplExpr_free(a->EXPlinetype);
        a->EXPlinetype = NULL;
    }
    // Line width expression
    if (a->EXPlinewidth != NULL) {
        pplExpr_free(a->EXPlinewidth);
        a->EXPlinewidth = NULL;
    }
    // Point line width expression
    if (a->EXPpointlinewidth != NULL) {
        pplExpr_free(a->EXPpointlinewidth);
        a->EXPpointlinewidth = NULL;
    }
    // Point size expression
    if (a->EXPpointsize != NULL) {
        pplExpr_free(a->EXPpointsize);
        a->EXPpointsize = NULL;
    }
    // Point type expression
    if (a->EXPpointtype != NULL) {
        pplExpr_free(a->EXPpointtype);
        a->EXPpointtype = NULL;
    }
    // Color expression
    if (a->EXPcolor != NULL) {
        pplExpr_free(a->EXPcolor);
        a->EXPcolor = NULL;
    }
    // Fill color expression
    if (a->EXPfillcolor != NULL) {
        pplExpr_free(a->EXPfillcolor);
        a->EXPfillcolor = NULL;
    }
    return;
}

//! ppl_withWordsCpy - Copy a set of plot style modifiers. Expressions are shared between the copies, with their
//! reference counts incremented.
//! \param [in] context - The Pyxplot context
//! \param [out] out - The copy
//! \param [in] in - The plot style modifiers to copy

void ppl_withWordsCpy(const ppl_context *context, withWords *out, const withWords *in) {
    *out = *in;
    // The copy shares the original's expressions, so each needs an extra reference
    if (in->EXPlinetype != NULL) {
        out->EXPlinetype = in->EXPlinetype;
        out->EXPlinetype->refCount++;
    }
    // Line width expression
    if (in->EXPlinewidth != NULL) {
        out->EXPlinewidth = in->EXPlinewidth;
        out->EXPlinewidth->refCount++;
    }
    // Point line width expression
    if (in->EXPpointlinewidth != NULL) {
        out->EXPpointlinewidth = in->EXPpointlinewidth;
        out->EXPpointlinewidth->refCount++;
    }
    // Point size expression
    if (in->EXPpointsize != NULL) {
        out->EXPpointsize = in->EXPpointsize;
        out->EXPpointsize->refCount++;
    }
    // Point type expression
    if (in->EXPpointtype != NULL) {
        out->EXPpointtype = in->EXPpointtype;
        out->EXPpointtype->refCount++;
    }
    // Color expression
    if (in->EXPcolor != NULL) {
        out->EXPcolor = in->EXPcolor;
        out->EXPcolor->refCount++;
    }
    // Fill color expression
    if (in->EXPfillcolor != NULL) {
        out->EXPfillcolor = in->EXPfillcolor;
        out->EXPfillcolor->refCount++;
    }
    return;
}
