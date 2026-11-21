// texify.c
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

// Converts Pyxplot expressions and strings into LaTeX, for the texify() function and for the automatic titles in graph
// legends. Expressions are tokenised with the expression compiler's tokeniser, so that each kind of token can be
// rendered appropriately: numbers with their units, string literals in roman type, variable names with Greek letters
// and subscripts, brackets of alternating shapes, and functions in the default namespace using the LaTeX templates in
// their definitions. Strings are escaped so that LaTeX prints them literally.

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

#include "coreUtils/memAlloc.h"
#include "userspace/stack.h"
#include "coreUtils/errorReport.h"
#include "mathsTools/dcfmath.h"
#include "expressions/expCompile_fns.h"
#include "expressions/expEval.h"
#include "settings/settingTypes.h"
#include "stringTools/strConstants.h"
#include "stringTools/asciidouble.h"
#include "userspace/context.h"
#include "userspace/pplObj_fns.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"
#include "pplConstants.h"
#include "texify.h"


//! texSetMode - Switch the LaTeX being written into plain text, math mode, or roman text within math mode
//! (\textrm{...}), writing whatever LaTeX is needed to close or open each mode
//! \param [out] out - The buffer into which LaTeX is being written
//! \param [in] outlen - The size of the buffer <out>
//! \param [in,out] k - The position in <out> at which to write; advanced past what is written
//! \param [in,out] mathMode - Flag indicating whether the LaTeX is currently in math mode
//! \param [in,out] textRm - Flag indicating whether the LaTeX is currently in a \textrm{...} block
//! \param [in] wantMath - True to enter math mode; false to return to plain text
//! \param [in] wantRm - True to enter a \textrm{...} block within math mode (requires <wantMath>)

static void texSetMode(char *out, const int outlen, int *k, int *mathMode, int *textRm, const int wantMath,
                       const int wantRm) {
    // Leaving plain text: open math mode
    if (wantMath && !*mathMode) {
        *mathMode = 1;
        snprintf(out + *k, outlen - *k, "$\\displaystyle ");
        *k += strlen(out + *k);
    }
    // Close or open a \textrm{...} block
    if (*textRm && !wantRm) {
        *textRm = 0;
        snprintf(out + *k, outlen - *k, "}");
        *k += strlen(out + *k);
    } else if (wantRm && !*textRm) {
        *textRm = 1;
        snprintf(out + *k, outlen - *k, "\\textrm{");
        *k += strlen(out + *k);
    }
    // Returning to plain text: close math mode
    if (!wantMath && *mathMode) {
        *mathMode = 0;
        snprintf(out + *k, outlen - *k, "$");
        *k += strlen(out + *k);
    }
}

static char *LatexVarNamesFr[] = {
        "alpha", "beta",  "gamma",  "delta", "epsilon", "zeta",  "eta",     "theta", "iota", "kappa", "lambda", "mu",
        "nu",    "xi",    "pi",     "rho",   "sigma",   "tau",   "upsilon", "phi",   "chi",  "psi",   "omega",  "Gamma",
        "Delta", "Theta", "Lambda", "Xi",    "Pi",      "Sigma", "Upsilon", "Phi",   "Psi",  "Omega", "aleph",  NULL};

static char *LatexVarNamesTo[] = {
        "\\alpha",   "\\beta",   "\\gamma", "\\delta",   "\\epsilon", "\\zeta",  "\\eta",   "\\theta", "\\iota",
        "\\kappa",   "\\lambda", "\\mu",    "\\nu",      "\\xi",      "\\pi",    "\\rho",   "\\sigma", "\\tau",
        "\\upsilon", "\\phi",    "\\chi",   "\\psi",     "\\omega",   "\\Gamma", "\\Delta", "\\Theta", "\\Lambda",
        "\\Xi",      "\\Pi",     "\\Sigma", "\\Upsilon", "\\Phi",     "\\Psi",   "\\Omega", "\\aleph", NULL};

//! ppl_texify_MakeGreek - Render a variable name in LaTeX, converting the names of Greek letters (e.g. alpha) into
//! LaTeX symbols, and underscores into subscripts
//! \param [in] in - The variable name
//! \param [out] out - Character buffer into which to write the LaTeX
//! \param [in] outlen - The size of the buffer <out>
//! \param [in|out] mathMode - Flag indicating whether the LaTeX output is currently in math mode
//! \param [in|out] textRm - Flag indicating whether the LaTeX output is currently in a \textrm block

static void ppl_texify_MakeGreek(const char *in, char *out, const int outlen, int *mathMode, int *textRm) {
    // Variables for scanning the name, and the position of the end of the output before any final digits
    int i, ji = 0, jg, a, l, m, k = 0, kno_ = 0;

    // Variable names are written in math mode
    texSetMode(out, outlen, &k, mathMode, textRm, 1, 0);
    // Convert each part of the name, separated by underscores
    while ((isalnum(in[ji])) || (in[ji] == '_')) {
        a = 0;
        // Check whether the name starts with the name of a Greek letter
        for (i = 0; LatexVarNamesFr[i] != NULL; i++) {
            for (jg = 0; LatexVarNamesFr[i][jg] != '\0'; jg++)
                if (LatexVarNamesFr[i][jg] != in[ji + jg]) break;
            if (LatexVarNamesFr[i][jg] != '\0') continue;
            if (!isalnum(in[ji + jg])) // We have a Greek letter
            {
                snprintf(out + k, outlen - 1 - k, "%s", LatexVarNamesTo[i]);
                k += strlen(out + k);
                ji += jg;
                a = 1;
                break;
            } else if (isdigit(in[ji + jg])) {
                l = ji + jg;
                while (isdigit(in[ji + jg])) jg++;
                if (isalnum(in[ji + jg])) continue;
                snprintf(out + k, outlen - 1 - k, "%s_{", LatexVarNamesTo[i]); // We have Greek letter underscore number
                k += strlen(out + k);
                for (m = l; m < ji + jg; m++)
                    if (k < outlen - 1) out[k++] = in[m];
                if (k < outlen - 1) out[k++] = '}';
                ji += jg;
                a = 1;
                break;
            }
        }
        // Digits at the end of a name become a subscript
        if ((a == 0) && (ji > 1)) {
            for (jg = 0; isdigit(in[ji + jg]); jg++);
            // Rewrite the digits as a subscript
            if ((jg > 0) && (!isalnum(in[ji + jg])) && (in[ji + jg] != '_')) {
                k = kno_;
                snprintf(out + k, outlen - 1 - k, "_{"); // We have an underscore number at the end of variable name
                k += strlen(out + k);
                for (m = ji; m < ji + jg; m++)
                    if (k < outlen - 1) out[k++] = in[m];
                if (k < outlen - 1) out[k++] = '}';
                ji += jg;
                a = 1;
            }
        }
        // Copy any other characters as they are
        if (a == 0) {
            for (jg = 0; (isalnum(in[ji + jg])); jg++)
                if (k < outlen - 1) out[k++] = in[ji + jg];
            ji += jg;
        }
        kno_ = k;
        // Underscores are escaped
        if (in[ji] == '_') {
            if (k < outlen - 1) out[k++] = '\\';
            if (k < outlen - 1) out[k++] = '_';
            ji++;
        }
    }
    out[k++] = '\0';
    return;
}

//! ppl_texify_string - Render a text string in LaTeX, escaping any characters which LaTeX treats as special
//! \param [in] in - The text string
//! \param [out] out - Character buffer into which to write the LaTeX
//! \param [in] inlen - The number of characters of <in> to render, or -1 to render the whole string
//! \param [in] outlen - The size of the buffer <out>

void ppl_texify_string(const char *in, char *out, const int inlen, const int outlen) {
    // Variables for tracking whether quotes are opening or closing ones
    int i, j, DoubleQuoteLevel = 0, QuoteLevel = 0;

    // Escape each character which is special to LaTeX
    for (i = j = 0; ((in[i] != '\0') && ((inlen < 0) || (i < inlen))); i++) {
        // Stop with an ellipsis if the output is nearly full
        if (j > outlen - 16) {
            strcpy(out + j, "...");
            j += strlen(out + j);
            break;
        }
        // Escape the character
        if (in[i] == '\\') {
            strcpy(out + j, "$\\backslash$");
            j += strlen(out + j);
        } else if (in[i] == '_') {
            out[j++] = '\\';
            out[j++] = in[i];
        } else if (in[i] == '&') {
            out[j++] = '\\';
            out[j++] = in[i];
        } else if (in[i] == '%') {
            out[j++] = '\\';
            out[j++] = in[i];
        } else if (in[i] == '$') {
            out[j++] = '\\';
            out[j++] = in[i];
        } else if (in[i] == '{') {
            out[j++] = '\\';
            out[j++] = in[i];
        } else if (in[i] == '}') {
            out[j++] = '\\';
            out[j++] = in[i];
        } else if (in[i] == '#') {
            out[j++] = '\\';
            out[j++] = in[i];
        } else if (in[i] == '^') {
            strcpy(out + j, "\\verb|^|");
            j += strlen(out + j);
        } else if (in[i] == '~') {
            strcpy(out + j, "$\\sim$");
            j += strlen(out + j);
        } else if (in[i] == '<') {
            strcpy(out + j, "$<$");
            j += strlen(out + j);
        } else if (in[i] == '>') {
            strcpy(out + j, "$>$");
            j += strlen(out + j);
        } else if (in[i] == '|') {
            strcpy(out + j, "$|$");
            j += strlen(out + j);
        } else if ((in[i] == '\"') && ((i == 0) || (in[i - 1] != '\\'))) {
            out[j++] = DoubleQuoteLevel ? '\'' : '`';
            out[j++] = DoubleQuoteLevel ? '\'' : '`';
            DoubleQuoteLevel = !DoubleQuoteLevel;
        } else if ((in[i] == '\'') && ((i == 0) || (in[i - 1] != '\\'))) {
            out[j++] = QuoteLevel ? '\'' : '`';
            QuoteLevel = !QuoteLevel;
        } else {
            out[j++] = in[i];
        }
    }
    out[j++] = '\0';
    return;
}

//! markupMatch - Test whether a string starts with a given piece of markup
//! \param [in] text - The string to test
//! \param [in] markup - The markup to look for
//! \return True if <text> starts with <markup>

static int markupMatch(const char *text, const char *markup) {
    return strncmp(text, markup, strlen(markup)) == 0;
}

//! ppl_texify_generic - Render an algebraic expression in LaTeX, e.g. converting sqrt(x) into \sqrt{x}
//! \param [in] c - The Pyxplot context
//! \param [in] in - The algebraic expression
//! \param [in] inlen - The number of characters of <in> to render, or -1 to render the whole expression
//! \param [out] end - Set to the number of characters of <in> which form part of the expression
//! \param [out] out - Character buffer into which to write the LaTeX
//! \param [in] outlen - The size of the buffer <out>
//! \param [in|out] mm_in - Flag indicating whether the LaTeX output is currently in math mode, or NULL to start
//! outside math mode
//! \param [in|out] tr_in - Flag indicating whether the LaTeX output is currently in a \textrm block, or NULL to
//! start outside one

void ppl_texify_generic(ppl_context *c, char *in, const int inlen, int *end, char *out, const int outlen, int *mm_in,
                        int *tr_in) {
    char msg[PPL_MSG_LENGTH];
    pplTokenCode *tokenBuff;
    const int allowCommaOperator = (inlen < 0);
    const int equalsAllowed = 1;
    const int dollarAllowed = 1;
    int errPos = -1, errType, tlen;
    char errbuff[LSTR_LENGTH];
    int *stkpos, i, k, lastOpMultDiv = 0;

    // Math mode and roman type are tracked in the caller's variables if supplied, so that nested calls can share them
    int mm = 0, tr = 0;
    int *mathMode = (mm_in == NULL) ? &mm : mm_in;
    int *textRm = (tr_in == NULL) ? &tr : tr_in;

#define MAX_BRACKETS 4096
    int bracketLevel = 0, highestBracketLevel = 0, bracketOpenPos[MAX_BRACKETS];

    // First tokenise expression
    ppl_expTokenise(c, in, end, dollarAllowed, equalsAllowed, allowCommaOperator, 0, 0, 0, &tlen, &errPos, &errType,
                    errbuff);
    // Give up if the expression cannot be tokenised
    if (errPos >= 0) {
        *end = 0;
        return;
    }
    // Give up if there is nothing to convert
    if (tlen <= 0) {
        *end = 0;
        return;
    }

    // Backup tokenised version
    tokenBuff = (pplTokenCode *) malloc(tlen * sizeof(pplTokenCode));
    // Give up if memory cannot be allocated
    if (tokenBuff == NULL) {
        *end = 0;
        return;
    }
    memcpy(tokenBuff, c->tokenBuff, tlen * sizeof(pplTokenCode));
    if (inlen >= 0) tlen = ppl_min(tlen, inlen);

    // Malloc buffer for storing stack positions of numerical constants
    stkpos = (int *) malloc(tlen * sizeof(int));
    // Give up if memory cannot be allocated
    if (stkpos == NULL) {
        *end = 0;
        return;
    }
    for (i = 0; i < tlen; i++) stkpos[i] = -1;

    // Evaluate numerical constants
    k = c->stackPtr;
    for (i = 0; i < tlen; i++)
        // Each numeric literal is pushed onto the stack, and every character of it is noted as belonging to that stack
        // entry
        if (tokenBuff[i].state == (unsigned char) 'L') {
            ppl_stackMustHave(c, 4);
            // Give up if the stack is full
            if (c->stackFull) {
                free(stkpos);
                stkpos = NULL;
                free(tokenBuff);
                tokenBuff = NULL;
                strcpy(out, "stack overflow");
                return;
            }
            pplObjNum(&c->stack[k], 0, ppl_getFloat(in + i, NULL), 0);
            while ((tokenBuff[i].state == (unsigned char) 'L') && (i < tlen)) { stkpos[i++] = k; } // ffw over constant
            k++;
        }

    // Evaluate the unit() function (but not after a ., and only if followed by () )
    for (i = 0; i < tlen; i++)
        if ((tokenBuff[i].state == (unsigned char) 'G') &&
            ((i < 1) ||
             ((tokenBuff[i - 1].state != (unsigned char) 'R') && (tokenBuff[i - 1].state != (unsigned char) 'G')))) {
            int ei = i, upos, p, q = 0, errpos = -1;
            pplObj val;
            ppl_stackMustHave(c, 4);
            // Give up if the stack is full
            if (c->stackFull) {
                free(stkpos);
                stkpos = NULL;
                free(tokenBuff);
                tokenBuff = NULL;
                strcpy(out, "stack overflow");
                return;
            }
            while ((tokenBuff[ei].state == (unsigned char) 'G') && (ei < tlen)) { ei++; } // ffw over function name
            if (tokenBuff[ei].state != (unsigned char) 'P') continue; // we don't have function arguments
            if ((ei - i < 4) || (strncmp(in + i, "unit", 4) != 0)) continue; // function isn't called unit
            for (p = i + 4; p < ei; p++)
                if ((isalnum(in[p])) || (in[p] == '_')) break;
            if (p < ei) continue; // function has trailing matter
            upos = ei + 1; // position of unit name
            val.refCount = 1;
            val.amMalloced = 0;
            ppl_unitsStringEvaluate(c, in + upos, &val, &q, &errpos, msg);
            if (errpos >= 0) continue;
            if (in[upos + q] != ')') continue;
            ei = ei + (1 + q);
            if (tokenBuff[ei].state != (unsigned char) 'P') continue; // we don't have closing bracket
            ei++;
            while ((tokenBuff[ei].state == (unsigned char) 'P') && ((unsigned char) in[i] <= ' ') && (in[i] >= '\0') &&
                   (ei < tlen)) {
                ei++;
            } // ffw over function arguments
            memcpy(&c->stack[k], &val, sizeof(pplObj));

            // Clean up
            for (; i < ei; i++) { stkpos[i] = k; }
            k++;
        }

    // Evaluate minus signs if literal is right argument
    for (i = 0; i < tlen; i++)
        if ((tokenBuff[i].state == (unsigned char) 'I') && ((i < 1) || (tokenBuff[i - 1].state != (unsigned char) 'I')))
            // Fold a minus sign into the following literal
            if (in[i] == '-') {
                int ei = i, sp;
                while ((tokenBuff[ei].state == (unsigned char) 'I') && (ei < tlen)) { ei++; } // ffw over minus sign
                if ((ei >= tlen) || (stkpos[ei] < 0)) continue; // right argument is not literal or unit() function
                sp = stkpos[ei];
                c->stack[sp].real *= -1;
                if (c->stack[sp].flagComplex) c->stack[sp].imag *= -1;
                for (; i < ei; i++) { stkpos[i] = sp; }
            }

    // Evaluate ** operator if unit() is left argument
    for (i = 0; i < tlen; i++)
        if ((tokenBuff[i].state == (unsigned char) 'J') && (i > 0) && (tokenBuff[i - 1].state == (unsigned char) 'P') &&
            (stkpos[i - 1] >= 0))
            // Fold a unit raised to a literal power into a single value
            if ((in[i] == '*') && (in[i + 1] == '*')) {
                int ei = i, sp1, sp2, stat = 0, errpos = -1;
                pplObj val;
                while ((tokenBuff[ei].state == (unsigned char) 'J') && (ei < tlen)) {
                    ei++;
                } // ffw over binary operator
                if ((ei >= tlen) || (stkpos[ei] < 0)) continue; // right argument is not a literal
                sp1 = stkpos[i - 1];
                sp2 = stkpos[ei];
                val.refCount = 1;
                val.amMalloced = 0;
                pplObjNum(&val, 0, 0, 0);
                ppl_uaPow(c, &c->stack[sp1], &c->stack[sp2], &val, &stat, &errpos, msg);
                if ((stat) || (errpos >= 0)) continue;
                memcpy(&c->stack[sp1], &val, sizeof(pplObj));
                for (; i < ei; i++) { stkpos[i] = sp1; }
                for (; stkpos[i] == sp2; i++) { stkpos[i] = sp1; }
            }

    // Evaluate * and / operators if unit() is either argument
    for (i = 0; i < tlen; i++)
        if ((tokenBuff[i].state == (unsigned char) 'J') && (i > 0) && (stkpos[i - 1] >= 0))
            // Fold a literal multiplied or divided by a unit into a single value
            if (((in[i] == '*') && (in[i + 1] != '*')) || (in[i] == '/')) {
                int ei = i, sp1, sp2, stat = 0, errpos = -1, gotUnit;
                pplObj val;
                gotUnit = (tokenBuff[i - 1].state == (unsigned char) 'P');
                while ((tokenBuff[ei].state == (unsigned char) 'J') && (ei < tlen)) {
                    ei++;
                } // ffw over binary operator
                if ((ei >= tlen) || (stkpos[ei] < 0)) continue; // right argument is not a literal
                gotUnit = gotUnit || (tokenBuff[ei].state == (unsigned char) 'G');
                if (!gotUnit) continue; // neither side is a unit()
                sp1 = stkpos[i - 1];
                sp2 = stkpos[ei];
                val.refCount = 1;
                val.amMalloced = 0;
                pplObjNum(&val, 0, 0, 0);
                if (in[i] == '*') ppl_uaMul(c, &c->stack[sp1], &c->stack[sp2], &val, &stat, &errpos, msg);
                else ppl_uaDiv(c, &c->stack[sp1], &c->stack[sp2], &val, &stat, &errpos, msg);
                if ((stat) || (errpos >= 0)) continue;
                memcpy(&c->stack[sp1], &val, sizeof(pplObj));
                for (; i < ei; i++) { stkpos[i] = sp1; }
                for (; stkpos[i] == sp2; i++) { stkpos[i] = sp1; }
            }

    // Produce tex output
    for (i = k = 0; i < tlen;) {
        int opMultDiv = 0;
        const char o = tokenBuff[i].state; // Get tokenised markup state code
        texSetMode(out, outlen, &k, mathMode, textRm, 1, 0);
        // A literal or unit, written as a number with units
        if (stkpos[i] >= 0) {
            double real = 0, imag = 0;
            pplObj *o = &c->stack[stkpos[i]];
            char *u = NULL;
            if (lastOpMultDiv) u = ppl_printUnit(c, o, &real, &imag, 0, 1, SW_DISPLAY_L);
            // Values of one after a multiplication or division are written as their unit alone
            if ((real != 1) || (imag != 0)) {
                snprintf(out + k, outlen - k, "%s",
                         ppl_unitsNumericDisplay(c, o, 0, SW_DISPLAY_L, 0) + 1); // Chop off initial $
                k += strlen(out + k) - 1; // Chop off final $
                out[k] = '\0';
            } else {
                snprintf(out + k, outlen - k, "%s", u);
                k += strlen(out + k);
            }
            while ((i < tlen) && (stkpos[i] >= 0)) { i++; }
        } else if (o == 'B') // Process a string literal
        {
            int ei, l;
            const char quoteType = in[i];
            for (ei = i; ((ei < tlen) && (tokenBuff[ei].state == o)); ei++);
            l = ei - i;
            while ((l > 1) && (in[i + l - 1] >= '\0') && ((unsigned char) in[i + l - 1] <= ' '))
                l--; // Strip trailing spaces
            // Strip the quotes from quoted strings
            if ((quoteType == '\'') || (quoteType == '\"')) {
                l -= 2;
                i++;
            }
            texSetMode(out, outlen, &k, mathMode, textRm, 1, 1);
            if (quoteType == '\'') snprintf(out + k, outlen - k, "`");
            else if (quoteType == '\"') snprintf(out + k, outlen - k, "``");
            k += strlen(out + k);
            ppl_texify_string(in + i, out + k, l, outlen - k - 1);
            k += strlen(out + k);
            if (quoteType == '\'') snprintf(out + k, outlen - k, "'");
            else if (quoteType == '\"') snprintf(out + k, outlen - k, "''");
            k += strlen(out + k);
            texSetMode(out, outlen, &k, mathMode, textRm, 1, 0);
            i = ei;
        } else if (o == 'C') // string substitution operator
        {
            snprintf(out + k, outlen - k, "\\%%");
            k += strlen(out + k);
            while ((i < tlen) && (tokenBuff[i].state == o)) i++; // Skip to the end of the token
        } else if ((o == 'D') || (o == 'E') || (o == 'P')) // open or close brackets
        {
            // Opening brackets are written as \left( for now, and their positions noted, so that their shape can be
            // changed when the closing bracket is found
            if (in[i] == '(') {
                snprintf(out + k, outlen - k, "\\left( ");
                k += strlen(out + k);
                if ((bracketLevel >= 0) && (bracketLevel < MAX_BRACKETS)) bracketOpenPos[bracketLevel] = k;
                bracketLevel++;
                if (bracketLevel > highestBracketLevel) highestBracketLevel = bracketLevel;
            } else if (in[i] == ')') {
                snprintf(out + k, outlen - k, "\\right) ");
                k += strlen(out + k);
                bracketLevel--;
                // Nested brackets cycle through round, square and curly shapes, from the inside out
                if ((bracketLevel >= 0) && (bracketLevel < MAX_BRACKETS)) {
                    const int j = (highestBracketLevel - bracketLevel) % 3;
                    // Round brackets
                    if (j == 1) {
                        out[k - 2] = ')';
                        out[k - 1] = ' ';
                        out[bracketOpenPos[bracketLevel] - 2] = '(';
                        out[bracketOpenPos[bracketLevel] - 1] = ' ';
                    } else if (j == 2) {
                        out[k - 2] = ']';
                        out[k - 1] = ' ';
                        out[bracketOpenPos[bracketLevel] - 2] = '[';
                        out[bracketOpenPos[bracketLevel] - 1] = ' ';
                    } else {
                        out[k - 2] = '\\';
                        out[k - 1] = '}';
                        out[bracketOpenPos[bracketLevel] - 2] = '\\';
                        out[bracketOpenPos[bracketLevel] - 1] = '{';
                    }
                }
            }
            i++;
        } else if ((o == 'F') || (o == 'H')) // -- or ++ operators
        {
            // The decrement operator
            if (in[i] == '-') {
                texSetMode(out, outlen, &k, mathMode, textRm, 1, 1);
                snprintf(out + k, outlen - k, "{\\tiny \\raisebox{0.8pt}{\\kern-0.1pt --\\kern0.5pt --}}");
            }
            // The increment operator
            if (in[i] == '+') {
                texSetMode(out, outlen, &k, mathMode, textRm, 1, 1);
                snprintf(out + k, outlen - k, "{\\tiny \\raisebox{0.8pt}{\\kern-0.2pt +\\kern-0.2pt +}}");
            }
            k += strlen(out + k);
            i++;
            i++;
            while ((in[i] >= '\0') && ((unsigned char) in[i] <= ' ') && (tokenBuff[i].state == o)) {
                { i += 1; };
            }
        } else if ((o == 'G') || (o == 'T') || (o == 'V')) // variable name
        {
            int mathrm = (o == 'T') || (o == 'V');
            int ei = i;
            while ((ei < tlen) && (tokenBuff[ei].state == o)) { ei++; }
            if (((tokenBuff[ei].state == 'P') && in[ei] == '(') || (tokenBuff[ei].state == 'R')) mathrm = 1;
            // Functions in the default namespace which have LaTeX templates are written using them, unless they have
            // been redefined
            if ((o == 'G') && ((i < 1) || (tokenBuff[i - 1].state != 'R'))) {
                int j, bi, cp[16], ncp, cbp;
                char dummyVar[FNAME_LENGTH], dummyVarGreek[FNAME_LENGTH], *fname, *latex;
                dictItem *dptr;
                pplFunc *fnobj;
                void *ldptr;
                if ((ei >= tlen) || ((tokenBuff[ei].state != 'P') && (tokenBuff[ei].state != 'B')))
                    goto variableName; // function not called with () afterwards; int_d is followed by B, not P
                if ((tokenBuff[ei].state == 'P') && (in[ei] != '(')) goto variableName;
                ppl_dictLookupWithWildcard(c->namespaces[0], in + i, dummyVar, FNAME_LENGTH, &dptr);
                if (dptr == NULL) goto variableName; // function was not in the default namespace
                fname = dptr->key;
                ldptr = ppl_dictLookup(c->namespaces[c->ns_ptr], fname);
                if (ldptr != NULL) goto variableName; // function redefined locally
                ldptr = ppl_dictLookup(c->namespaces[1], fname);
                if (ldptr != NULL) goto variableName; // function redefined globally
                fnobj = (pplFunc *) ((pplObj *) dptr->data)->auxil;
                if (fnobj == NULL) goto variableName;
                latex = fnobj->LaTeX;
                if (latex == NULL) goto variableName; // no latex model supplied

                // Find ( at beginning of function arguments
                for (bi = i; isalnum(in[bi]) || (in[bi] == '_'); bi++);
                for (; (in[bi] > '\0') && ((unsigned char) in[bi] <= ' '); bi++);
                if (in[bi] != '(') goto variableName; // could not find (
                ppl_strBracketMatch(in + bi, '(', ')', cp, &ncp, &cbp, 16); // Search for a ) to match the (
                if (cbp <= 0) goto variableName; // could not find )
                if (ncp >= 16) goto variableName; // wrong number of arguments
                if (dummyVar[0] == '\0') {
                    if ((ncp < fnobj->minArgs) || (ncp > fnobj->maxArgs)) goto variableName;
                } // wrong number of arguments
                else {
                    if (ncp != fnobj->minArgs) goto variableName;
                }

                // Work through latex model
                ppl_texify_MakeGreek(dummyVar, dummyVarGreek, FNAME_LENGTH, mathMode, textRm);
                // Write the template, replacing @? with the dummy variable, @< and @> with brackets, @1 to @6 with the
                // arguments, and @0 with all of the arguments
                for (j = 0; latex[j] != '\0'; j++) {
                    if (latex[j] != '@') snprintf(out + k, outlen - k, "%c", latex[j]);
                    else if (latex[j + 1] == '?') snprintf(out + k, outlen - k, "%s", dummyVarGreek);
                    // An opening bracket
                    else if (latex[j + 1] == '<') {
                        snprintf(out + k, outlen - k, "\\left( ");
                        k += strlen(out + k);
                        if ((bracketLevel >= 0) && (bracketLevel < MAX_BRACKETS)) bracketOpenPos[bracketLevel] = k;
                        bracketLevel++;
                        if (bracketLevel > highestBracketLevel) highestBracketLevel = bracketLevel;
                    } else if (latex[j + 1] == '>') {
                        snprintf(out + k, outlen - k, "\\right) ");
                        k += strlen(out + k);
                        bracketLevel--;
                        // Nested brackets cycle through round, square and curly shapes, from the inside out
                        if ((bracketLevel >= 0) && (bracketLevel < MAX_BRACKETS)) {
                            const int j = (highestBracketLevel - bracketLevel) % 3;
                            // Round brackets
                            if (j == 1) {
                                out[k - 2] = ')';
                                out[k - 1] = ' ';
                                out[bracketOpenPos[bracketLevel] - 2] = '(';
                                out[bracketOpenPos[bracketLevel] - 1] = ' ';
                            } else if (j == 2) {
                                out[k - 2] = ']';
                                out[k - 1] = ' ';
                                out[bracketOpenPos[bracketLevel] - 2] = '[';
                                out[bracketOpenPos[bracketLevel] - 1] = ' ';
                            } else {
                                out[k - 2] = '\\';
                                out[k - 1] = '}';
                                out[bracketOpenPos[bracketLevel] - 2] = '\\';
                                out[bracketOpenPos[bracketLevel] - 1] = '{';
                            }
                        }
                    } else if ((latex[j + 1] >= '1') && (latex[j + 1] <= '6')) {
                        int l = (latex[j + 1] - '1'), end;
                        ppl_texify_generic(c, in + bi + cp[l] + 1, cp[l + 1] - cp[l] - 1, &end, out + k, outlen - k,
                                           mathMode, textRm);
                        k += strlen(out + k);
                    } else if (latex[j + 1] == '0') {
                        int a, end;
                        // Write all of the arguments, separated by commas
                        for (a = 0; a < ncp; a++) {
                            // Separate the arguments with commas
                            if (a != 0) {
                                snprintf(out + k, outlen - k, ",");
                                k += strlen(out + k);
                            }
                            ppl_texify_generic(c, in + bi + cp[a] + 1, cp[a + 1] - cp[a] - 1, &end, out + k, outlen - k,
                                               mathMode, textRm);
                            k += strlen(out + k);
                        }
                    }
                    k += strlen(out + k);
                    if (latex[j] == '@') j++; // FFW over two-byte code
                }

                // Carry on work after function
                i = bi + cbp + 1;
                goto variableDone; // The function call has been rendered; it is not also a variable name
            }

            // If variable name is not a recognised function name, print name as a variable name
        variableName:
            // Function names and the names of members are written in roman type
            if (mathrm) {
                snprintf(out + k, outlen - k, "\\mathrm{");
                k += strlen(out + k);
            }
            ppl_texify_MakeGreek(in + i, out + k, outlen - k, mathMode, textRm);
            k += strlen(out + k);
            // Close the roman type
            if (mathrm) {
                snprintf(out + k, outlen - k, "}");
                k += strlen(out + k);
            }
            while ((i < tlen) && (tokenBuff[i].state == o)) i++; // Skip to the end of the token
        variableDone:;
        } else if (o == 'I') // unary operator
        {
            // Write the unary operator
            if (markupMatch(in + i, "-")) {
                snprintf(out + k, outlen - k, "-");
                { i += 1; };
            } else if (markupMatch(in + i, "+")) {
                snprintf(out + k, outlen - k, "+");
                { i += 1; };
            } else if (markupMatch(in + i, "~")) {
                snprintf(out + k, outlen - k, "\\sim ");
                { i += 1; };
            } else if (markupMatch(in + i, "!")) {
                snprintf(out + k, outlen - k, "!");
                { i += 1; };
            } else if (markupMatch(in + i, "not")) {
                texSetMode(out, outlen, &k, mathMode, textRm, 1, 1);
                snprintf(out + k, outlen - k, " not ");
                { i += 3; };
            } else if (markupMatch(in + i, "NOT")) {
                texSetMode(out, outlen, &k, mathMode, textRm, 1, 1);
                snprintf(out + k, outlen - k, " not ");
                { i += 3; };
            }
            k += strlen(out + k);
            while ((in[i] >= '\0') && ((unsigned char) in[i] <= ' ') && (tokenBuff[i].state == o)) {
                { i += 1; };
            }
        } else if (o == 'J') // a binary operator
        {
            // Write the binary operator
            if (markupMatch(in + i, "**")) {
                snprintf(out + k, outlen - k, "\\,*\\kern-1.5pt *\\,\\,");
                { i += 2; };
            } else if (markupMatch(in + i, "<<")) {
                snprintf(out + k, outlen - k, "\\ll ");
                { i += 2; };
            } else if (markupMatch(in + i, ">>")) {
                snprintf(out + k, outlen - k, "\\gg ");
                { i += 2; };
            } else if (markupMatch(in + i, "<=")) {
                snprintf(out + k, outlen - k, "\\leq ");
                { i += 2; };
            } else if (markupMatch(in + i, ">=")) {
                snprintf(out + k, outlen - k, "\\geq ");
                { i += 2; };
            } else if (markupMatch(in + i, "==")) {
                snprintf(out + k, outlen - k, "==");
                { i += 2; };
            } else if (markupMatch(in + i, "<>")) {
                snprintf(out + k, outlen - k, "<>");
                { i += 2; };
            } else if (markupMatch(in + i, "!=")) {
                snprintf(out + k, outlen - k, "!=");
                { i += 2; };
            } else if (markupMatch(in + i, "&&")) {
                texSetMode(out, outlen, &k, mathMode, textRm, 1, 1);
                snprintf(out + k, outlen - k, "\\,\\&\\&\\,");
                { i += 2; };
            } else if (markupMatch(in + i, "and")) {
                texSetMode(out, outlen, &k, mathMode, textRm, 1, 1);
                snprintf(out + k, outlen - k, " and ");
                { i += 3; };
            } else if (markupMatch(in + i, "AND")) {
                texSetMode(out, outlen, &k, mathMode, textRm, 1, 1);
                snprintf(out + k, outlen - k, " and ");
                { i += 3; };
            } else if (markupMatch(in + i, "||")) {
                snprintf(out + k, outlen - k, "\\,||\\,");
                { i += 2; };
            } else if (markupMatch(in + i, "or")) {
                texSetMode(out, outlen, &k, mathMode, textRm, 1, 1);
                snprintf(out + k, outlen - k, " or ");
                { i += 2; };
            } else if (markupMatch(in + i, "OR")) {
                texSetMode(out, outlen, &k, mathMode, textRm, 1, 1);
                snprintf(out + k, outlen - k, " or ");
                { i += 2; };
            } else if (markupMatch(in + i, "*")) {
                snprintf(out + k, outlen - k, "\\times ");
                { i += 1; };
                opMultDiv = 1;
            } else if (markupMatch(in + i, "/")) {
                snprintf(out + k, outlen - k, "/");
                { i += 1; };
                opMultDiv = 1;
            } else if (markupMatch(in + i, "%")) {
                texSetMode(out, outlen, &k, mathMode, textRm, 1, 1);
                snprintf(out + k, outlen - k, "\\%%");
                { i += 1; };
            } else if (markupMatch(in + i, "+")) {
                snprintf(out + k, outlen - k, "+");
                { i += 1; };
            } else if (markupMatch(in + i, "-")) {
                snprintf(out + k, outlen - k, "-");
                { i += 1; };
            } else if (markupMatch(in + i, "<")) {
                snprintf(out + k, outlen - k, "<");
                { i += 1; };
            } else if (markupMatch(in + i, ">")) {
                snprintf(out + k, outlen - k, ">");
                { i += 1; };
            } else if (markupMatch(in + i, "&")) {
                texSetMode(out, outlen, &k, mathMode, textRm, 1, 1);
                snprintf(out + k, outlen - k, "\\,\\&\\,");
                { i += 1; };
            } else if (markupMatch(in + i, "^")) {
                snprintf(out + k, outlen - k, "\\verb|^|");
                { i += 1; };
            } else if (markupMatch(in + i, "|")) {
                snprintf(out + k, outlen - k, "\\,|\\,");
                { i += 1; };
            } else if (markupMatch(in + i, ",")) {
                snprintf(out + k, outlen - k, ",");
                { i += 1; };
            }
            k += strlen(out + k);
            while ((in[i] >= '\0') && ((unsigned char) in[i] <= ' ') && (tokenBuff[i].state == o)) {
                { i += 1; };
            }
        } else if (o == 'K') // a terniary operator
        {
            // Write the ternary operator
            if (markupMatch(in + i, "?")) {
                snprintf(out + k, outlen - k, "\\,?\\,");
                { i += 1; };
            } else if (markupMatch(in + i, ":")) {
                snprintf(out + k, outlen - k, ":");
                { i += 1; };
            }
            k += strlen(out + k);
            while ((in[i] >= '\0') && ((unsigned char) in[i] <= ' ') && (tokenBuff[i].state == o)) {
                { i += 1; };
            }
        } else if ((o == 'M') || (o == 'Q')) // list literal
        {
            if (in[i] == '[') snprintf(out + k, outlen - k, "\\left[");
            if (in[i] == ']') snprintf(out + k, outlen - k, "\\right]");
            k += strlen(out + k);
            i++;
            while ((in[i] >= '\0') && ((unsigned char) in[i] <= ' ') && (tokenBuff[i].state == o)) {
                { i += 1; };
            }
        } else if (o == 'N') // dictionary literal
        {
            if (in[i] == '{') snprintf(out + k, outlen - k, "\\left\\{");
            if (in[i] == '}') snprintf(out + k, outlen - k, "\\right\\}");
            k += strlen(out + k);
            i++;
            while ((in[i] >= '\0') && ((unsigned char) in[i] <= ' ') && (tokenBuff[i].state == o)) {
                { i += 1; };
            }
        } else if (o == 'O') // dollar operator
        {
            if (in[i] == '$') snprintf(out + k, outlen - k, "\\$");
            k += strlen(out + k);
            i++;
            while ((in[i] >= '\0') && ((unsigned char) in[i] <= ' ') && (tokenBuff[i].state == o)) {
                { i += 1; };
            }
        } else if (o == 'R') // dot operator
        {
            if (in[i] == '.') snprintf(out + k, outlen - k, ".");
            k += strlen(out + k);
            i++;
            while ((in[i] >= '\0') && ((unsigned char) in[i] <= ' ') && (tokenBuff[i].state == o)) {
                { i += 1; };
            }
        } else if (o == 'S') // assignment operator
        {
            // Write the assignment operator
            if (markupMatch(in + i, "=")) {
                snprintf(out + k, outlen - k, "=");
                { i += 1; };
            } else if (markupMatch(in + i, "+=")) {
                snprintf(out + k, outlen - k, "\\,\\,+\\kern-2.5pt =");
                { i += 2; };
            } else if (markupMatch(in + i, "-=")) {
                snprintf(out + k, outlen - k, "\\,\\,-\\kern-2.5pt =");
                { i += 2; };
            } else if (markupMatch(in + i, "*=")) {
                snprintf(out + k, outlen - k, "\\,\\,\\times\\kern-2.5pt =");
                { i += 2; };
            } else if (markupMatch(in + i, "/=")) {
                snprintf(out + k, outlen - k, "\\,/\\kern-1.5pt =");
                { i += 2; };
            } else if (markupMatch(in + i, "%=")) {
                snprintf(out + k, outlen - k, "\\,\\,\\textrm{\\%%}\\kern-2pt =");
                { i += 2; };
            } else if (markupMatch(in + i, "&=")) {
                snprintf(out + k, outlen - k, "\\,\\,\\textrm{\\&}\\kern-2pt =");
                { i += 2; };
            } else if (markupMatch(in + i, "^=")) {
                snprintf(out + k, outlen - k, "\\,\\,\\verb|^|\\kern-2pt =");
                { i += 2; };
            } else if (markupMatch(in + i, "|=")) {
                snprintf(out + k, outlen - k, "\\,\\,|\\kern-2.3pt =");
                { i += 2; };
            } else if (markupMatch(in + i, "<<=")) {
                snprintf(out + k, outlen - k, "\\ll=");
                { i += 3; };
            } else if (markupMatch(in + i, ">>=")) {
                snprintf(out + k, outlen - k, "\\gg=");
                { i += 3; };
            } else if (markupMatch(in + i, "**=")) {
                snprintf(out + k, outlen - k, "\\,*\\kern-1.5pt *\\kern-1.5pt=");
                { i += 3; };
            }
            k += strlen(out + k);
            while ((in[i] >= '\0') && ((unsigned char) in[i] <= ' ') && (tokenBuff[i].state == o)) {
                { i += 1; };
            }
        } else // unknown token
        {
            while ((i < tlen) && (tokenBuff[i].state == o)) i++; // Skip to the end of the token
        }
        lastOpMultDiv = opMultDiv;
    }

    // Close math mode at the end of a complete expression
    if (inlen < 0) { texSetMode(out, outlen, &k, mathMode, textRm, 0, 0); }
    out[k] = '\0';
    free(stkpos);
    stkpos = NULL;
    free(tokenBuff);
    tokenBuff = NULL;
    return;
}
