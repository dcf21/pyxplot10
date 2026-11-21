// expCompile.c
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

// The expression compiler. ppl_expTokenise() first classifies each character of an algebraic expression into tokens
// (numbers, strings, variable names, operators, brackets...), checking at each step that the token may follow the
// previous one, according to the table below; this finds syntax errors and their positions. The tokens are then
// compiled into bytecode for the stack machine in expEval.c, taking account of operator precedence. Every expression in
// Pyxplot passes through here, e.g. in commands, function definitions and using modifiers.

#define _EXPCOMPILE_C 1

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "coreUtils/errorReport.h"
#include "coreUtils/memAlloc.h"
#include "stringTools/asciidouble.h"
#include "userspace/context.h"
#include "expressions/expCompile_fns.h"

#include "pplConstants.h"

// expTokenise() -- Tokenise expressions

// A -- beginning of expression
// B -- a string literal
// C -- a string substitution operator
// D -- a list of string substitutions
// E -- a bracketed sub-expression
// F -- a unary post-lvalue operator
// G -- a variable name
// H -- a unary pre-lvalue operator
// I -- a unary value-based operator
// J -- a binary operator
// K -- a ternary operator
// L -- a numeric literal
// M -- a list literal
// N -- a dictionary literal
// O -- a dollar sign
// P -- a list of function args
// Q -- an array dereference []
// R -- the dot operator
// S -- assignment operator
// T -- dereference name
// U -- end of expression
// V -- parameter name after $

// A can be followed by .B..E.GHI..LMNO.......
// B can be followed by ..C......JK.....QR..U.
// C can be followed by .B.D..GHI..LMNO.......
// D can be followed by .........JK.....Q...U.
// E can be followed by .....F...JK.....QR..U.
// F can be followed by .........JK.........U.
// G can be followed by .....F...JK....PQRS.U.
// H can be followed by ....E.G...............
// I can be followed by .B..E.GH...LMNO.......
// J can be followed by .B..E.GHI..LMNO.......
// K can be followed by .B..E.GHI..LMNO.......
// L can be followed by .........JK.........U.
// M can be followed by .........JK.....QR..U.
// N can be followed by .........JK.....QR..U.
// O can be followed by ....E......L.........V
// P can be followed by .........JK....PQR..U.
// Q can be followed by .....F...JK....PQRS.U.
// R can be followed by ...................T..
// S can be followed by .B..E.GHI..LMNO.......
// T can be followed by .....F...JK....PQRS.U.
// V can be followed by .........JK.........U.

//! markupMatch - Test whether a string starts with a given piece of markup
//! \param [in] text - The string to test
//! \param [in] markup - The markup to look for
//! \return True if <text> starts with <markup>

static int markupMatch(const char *text, const char *markup) {
    return strncmp(text, markup, strlen(markup)) == 0;
}


//! tokeniserState - The state of ppl_expTokenise() as it works through an expression, writing a token code for
//! each character into <context->tokenBuff>. The buffer may be reallocated as it grows (including by recursive calls
//! which tokenise bracketed sub-expressions), so pointers into it are never kept; tokenOut() finds the current
//! position afresh.
typedef struct tokeniserState {
    ppl_context *context;
    int outOffset; // The position in <context->tokenBuff> of the token for the first character of the expression
    int outpos; // The number of tokens written so far
    int scanpos; // The position in the expression of the next character to tokenise
    char state; // The state of the tokeniser, i.e. the type of token currently being read (see the table above)
    unsigned char opcode; // The operator which the current token represents
    unsigned char precedence; // The precedence of that operator
} tokeniserState;

//! tokenOut - Find the position in the token buffer at which the next token will be written
//! \param [in] t - The tokeniser state
//! \return Pointer to the next token in <context->tokenBuff>

static pplTokenCode *tokenOut(const tokeniserState *t) {
    return t->context->tokenBuff + t->outOffset + t->outpos;
}

//! tokenBuffEnsure - Make sure that the token buffer has room for more than <n> further tokens, enlarging it if
//! necessary
//! \param [in,out] t - The tokeniser state
//! \param [in] n - The number of further tokens to make room for
//! \return Zero on success, or nonzero if memory ran out

static int tokenBuffEnsure(tokeniserState *t, const int n) {
    ppl_context *context = t->context;
    // Double the size of the buffer until there is room for n more tokens
    while (t->outOffset + t->outpos + n >= context->tokenBuffLen) {
        pplTokenCode *grown =
                (pplTokenCode *) realloc(context->tokenBuff, 2 * context->tokenBuffLen * sizeof(pplTokenCode));
        if (grown == NULL) return 1;
        context->tokenBuff = grown;
        context->tokenBuffLen *= 2;
    }
    return 0;
}

//! tokenNewState - Move the tokeniser into a new state, recording a token for each of the next <len> characters of
//! the expression
//! \param [in,out] t - The tokeniser state
//! \param [in] state - The new state
//! \param [in] opcode - The operator which the new token represents
//! \param [in] precedence - The precedence of that operator
//! \param [in] len - The number of characters which the new token spans
//! \return Zero on success, or nonzero if memory ran out

static int tokenNewState(tokeniserState *t, const char state, const unsigned char opcode,
                         const unsigned char precedence, const int len) {
    t->state = state;
    t->opcode = opcode;
    t->precedence = precedence;
    if (tokenBuffEnsure(t, len)) return 1;
    // Write a token code for each of the characters which the new token occupies
    for (int i = 0; i < len; i++) {
        pplTokenCode *o = tokenOut(t);
        o->state = state;
        o->opcode = opcode;
        o->precedence = precedence;
        t->outpos++;
    }
    t->scanpos += len;
    return 0;
}

//! tokenSameState - Record a token for the next character of the expression, which continues the current token
//! \param [in,out] t - The tokeniser state
//! \return Zero on success, or nonzero if memory ran out

static int tokenSameState(tokeniserState *t) {
    return tokenNewState(t, t->state, t->opcode, t->precedence, 1);
}

//! tokenFastForward - Skip over the tokens for the next <len> characters of the expression, which have already been
//! written by a recursive call to ppl_expTokenise()
//! \param [in,out] t - The tokeniser state
//! \param [in] len - The number of characters to skip
//! \return Zero on success, or nonzero if memory ran out

static int tokenFastForward(tokeniserState *t, const int len) {
    if (tokenBuffEnsure(t, len)) return 1;
    t->outpos += len;
    t->scanpos += len;
    return 0;
}

//! appendOr - When listing the alternatives which a syntax error message says were expected, write " or " between
//! each alternative and the next
//! \param [in,out] errText - The error message being written
//! \param [in,out] i - The position in <errText> at which to write; advanced past what is written
//! \param [in,out] j - Flag indicating whether any alternatives have been listed yet; set to true

static void appendOr(char *errText, int *i, int *j) {
    // Separate alternatives with "or", except before the first one
    if (*j != 0) {
        strcpy(errText + *i, " or ");
        *i += strlen(errText + *i);
    } else *j = 1;
}

//! ppl_expTokenise - Tokenise an algebraic expression, labelling each character with the type of token it belongs
//! to and the operator it represents. The result is written into <context->tokenBuff>.
//! \param [in] context - The Pyxplot context
//! \param [in] in - The expression to tokenise
//! \param [out] end - Set to the number of characters of <in> which form part of the expression
//! \param [in] dollarAllowed - If true, the $ operator may be used
//! \param [in] equalsAllowed - If true, assignment operators may be used
//! \param [in] allowCommaOperator - If true, the comma operator may be used
//! \param [in] collectCommas - If true, commas separate items in a list, rather than ending the expression
//! \param [in] isDict - If true, the expression is the contents of a dictionary literal, so colons separate keys
//! from values
//! \param [in] outOffset - Position in <context->tokenBuff> at which to start writing, for recursive calls
//! \param [out] outlen - Set to the number of tokens written
//! \param [out] errPos - Set to the position of any syntax error in <in>, or -1 if there was none
//! \param [out] errType - Set to the type of error, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void ppl_expTokenise(ppl_context *context, char *in, int *end, const int dollarAllowed, const int equalsAllowed,
                     const int allowCommaOperator, const int collectCommas, const int isDict, const int outOffset,
                     int *outlen, int *errPos, int *errType, char *errText) {
    const char *allowed[] = {"BEHILMNOG", "CJKQRU",    "BDHILMNOG", "JKQU",     "FJKQRU", "JKU",   "SFJKPQRU", "EG",
                             "BEHLMNOG",  "BEHILMNOG", "BEHILMNOG", "JKU",      "JKQRU",  "JKQRU", "ELV",      "JKPQRU",
                             "SFJKPQRU",  "T",         "BEHILMNOG", "SFJKPQRU", "",       "JKU"};
    int nCommaItems = 1, nDictItems = 0, tertiaryDepth = 0;
    char trialstate;
    tokeniserState t = {context, outOffset, 0, 0, 'A', 0, 0};
    int expOp = 0; // Was last named function one which acts on an expression -- e.g. unit() or int_dx()?
    int extraArg =
            0; // Was last named function one which has a variable name encoded in it -- i.e. int_dx() or diff_dx()?

    // At the start of a new top-level expression, shrink a token buffer which has grown very large
    if ((outOffset == 0) && (context->tokenBuffLen > ALGEBRA_MAXLEN)) {
        free(context->tokenBuff);
        context->tokenBuff = NULL;
        context->tokenBuffLen = 0;
    }

    // Allocate the token buffer if necessary
    if (context->tokenBuff == NULL) {
        context->tokenBuff = (pplTokenCode *) malloc(ALGEBRA_MAXLEN * sizeof(pplTokenCode));
        context->tokenBuffLen = ALGEBRA_MAXLEN;
    }

    // Failure to allocate memory is reported as an error
    if (context->tokenBuff == NULL) {
        *errPos = 0;
        *errType = ERR_MEMORY;
        ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s", "Out of memory");
        *end = -1;
        return;
    }

    // Read tokens until the end of the expression (state U). At each step, skip whitespace, and then try each of the
    // states which may follow the current one, in the order listed in the table, until one matches.
    while (t.state != 'U') {
        while ((in[t.scanpos] != '\0') && ((unsigned char) in[t.scanpos] <= ' ')) {
            if (tokenSameState(&t)) goto overflow;
        } // Sop up whitespace
        const int oldpos = t.scanpos;

        // Try each state which may follow the current one
        for (int trialpos = 0; ((trialstate = allowed[(int) (t.state - 'A')][trialpos]) != '\0'); trialpos++) {
            if ((expOp) && (trialstate != 'P'))
                continue; // int_dx and diff_dx MUST be followed by arguments; they have already put one on the stack.
            if (trialstate == 'B') // string literal
            {
                char quoteType;
                int raw = 0;
                // A string may have a prefix: r for a raw string, in which backslashes are not escapes
                if ((in[t.scanpos] == 'r') && ((in[t.scanpos + 1] == '\'') || (in[t.scanpos + 1] == '"'))) {
                    raw = 1;
                    if (tokenNewState(&t, trialstate, 0, 0, 1)) goto overflow;
                } // r'hello' is raw string
                // e for a string in which escape sequences such as
                // are expanded
                if ((in[t.scanpos] == 'e') && ((in[t.scanpos + 1] == '\'') || (in[t.scanpos + 1] == '"'))) {
                    raw = 0;
                    if (tokenNewState(&t, trialstate, 0, 0, 1)) goto overflow;
                } // e'hello' is string with extended escapes
                // A string literal, in single or double quotes, or in triple quotes, which may span several lines
                if ((in[t.scanpos] == (quoteType = '\'')) || (in[t.scanpos] == (quoteType = '"'))) {
                    int j = 1, tripleQuote = 0;
                    // Triple quotes
                    if ((in[t.scanpos + 1] == quoteType) && (in[t.scanpos + 2] == quoteType)) {
                        j = 3;
                        tripleQuote = 1;
                    }
                    for (; (in[t.scanpos + j] != '\0') &&
                         (((!tripleQuote) && (in[t.scanpos + j] != quoteType)) ||
                          (tripleQuote &&
                           ((in[t.scanpos + j] != quoteType) || (in[t.scanpos + j + 1] != quoteType) ||
                            (in[t.scanpos + j + 2] != quoteType))));
                         j++)
                        if ((!raw) && (in[t.scanpos + j] == '\\') && (in[t.scanpos + j + 1] != '\0')) j++;
                    // Found the closing quote
                    if (in[t.scanpos + j] == quoteType) {
                        // Triple quotes are closed by three quote characters
                        if (tripleQuote) {
                            j += 3;
                        } else {
                            j++;
                        }
                        if (tokenNewState(&t, trialstate, 0, 0, j)) goto overflow;
                    } else {
                        *errPos = t.scanpos;
                        *errType = ERR_SYNTAX;
                        ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s", "Mismatched quote.");
                        *end = -1;
                        *outlen = 0;
                        return;
                    }
                }
            } else if (trialstate == 'C') // string substitution operator
            {
                if (markupMatch(in + t.scanpos, "%")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_STRING_SUBST, 4, 1)) goto overflow;
                }
            } else if ((trialstate == 'D') // a list of string substitutions
                       || (trialstate == 'E') // a bracketed sub-expression
                       || (trialstate == 'P')) // a list of function arguments
            {
                // A bracketed sub-expression, or the arguments of a function call
                if (markupMatch(in + t.scanpos, "(")) {
                    int j, m, n = 1, cp[3], cpl = 3, extraStrArg = 0;
                    ppl_strBracketMatch(in + t.scanpos, '(', ')', cp, &cpl, &j, cpl); // Search for a ) to match the (
                    // The bracket must be closed
                    if (j <= 0) {
                        *errPos = t.scanpos;
                        *errType = ERR_SYNTAX;
                        ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s", "Mismatched ( )");
                        *end = -1;
                        *outlen = 0;
                        return;
                    }
                    if (tokenNewState(&t, trialstate, 0, 0, 1))
                        goto overflow; // Record the one character opening bracket
                    // Skip whitespace after the opening bracket
                    while ((in[t.scanpos] != '\0') && ((unsigned char) in[t.scanpos] <= ' ')) {
                        if (tokenSameState(&t)) goto overflow;
                        n++;
                    } // Sop up whitespace
                    if ((trialstate == 'P') && (expOp) && (in[t.scanpos] != '\'') &&
                        (in[t.scanpos] !=
                         '"')) // Function call acting on expression: save expression as string even though it's not quoted
                    {
                        int explen = cp[1] + (cpl != 1) - n;
                        if (expOp > 1) explen = j - n; // texify receives all arguments as a string
                        // ppl_strBracketMatch() should always find the closing bracket here
                        if (cpl < 1) {
                            *errPos = t.scanpos;
                            *errType = ERR_INTERNAL;
                            ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s",
                                              "ppl_strBracketMatch returned fewer than two results.");
                            *end = -1;
                            *outlen = 0;
                            return;
                        }
                        trialstate = 'B';
                        if (tokenNewState(&t, trialstate, 0, 0, explen)) goto overflow;
                        trialstate = 'P';
                        n += explen;
                        extraStrArg = (explen > 0);
                    }
                    const int k = t.scanpos;
                    const int l = t.outpos;
                    j -= n;
                    // Unless the brackets are empty, tokenise their contents recursively
                    if ((in[k] != ')') || (trialstate == 'E')) {
                        ppl_expTokenise(context, in + k, &m, dollarAllowed, 1, 1, (trialstate != 'E'), 0, l + outOffset,
                                        &n, errPos, errType,
                                        errText); // Hierarchically tokenise the inside of the brackets
                        // Report a syntax error within the brackets
                        if (*errPos >= 0) {
                            *errPos += k;
                            return;
                        }
                        // The contents must use everything up to the closing bracket
                        if (m != j) {
                            *errPos = m + k;
                            *errType = ERR_SYNTAX;
                            ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s",
                                              "Unexpected trailing matter at end of expression.");
                            *end = -1;
                            *outlen = 0;
                            return;
                        }
                    } else tokenOut(&t)->depth = 0;
                    if (tokenFastForward(&t, j)) goto overflow; // Fast-forward to closing bracket
                    if (extraStrArg) tokenOut(&t)->depth++;
                    if (extraArg) tokenOut(&t)->depth++;
                    if (tokenNewState(&t, trialstate, 0, 0, 1))
                        goto overflow; // Record the one character closing bracket
                    extraArg = 0;
                    expOp = 0;
                }
            } else if (trialstate == 'F') // a unary post-lvalue operator
            {
                // Post-decrement and post-increment operators, e.g. x++
                if (markupMatch(in + t.scanpos, "--")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_POST_DECREMENT, 3, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "++")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_POST_INCREMENT, 3, 2)) goto overflow;
                }
            } else if (trialstate == 'H') // a unary pre-lvalue operator
            {
                // Pre-decrement and pre-increment operators, e.g. ++x
                if (markupMatch(in + t.scanpos, "--")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_PRE_DECREMENT, 3, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "++")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_PRE_INCREMENT, 3, 2)) goto overflow;
                }
            } else if ((trialstate == 'G') || // a variable name
                       (trialstate == 'T') || // a dereference name
                       (trialstate == 'V')) // a $parameter name
            {
                // A variable or function name. The names unit(), int_dx() and diff_dx() are special, since their
                // arguments are not ordinary expressions.
                if (isalpha(in[t.scanpos])) {
                    const char tmp = trialstate;
                    if ((strncmp(in + t.scanpos, "unit", 4) == 0) && (!isalnum(in[t.scanpos + 4])) &&
                        (in[t.scanpos + 4] != '_')) // unit() function
                    {
                        expOp = 1;
                        if (tokenNewState(&t, trialstate, 0, 0, 4)) goto overflow;
                    } else if (strncmp(in + t.scanpos, "int_d", 5) == 0) // int_d() function
                    {
                        // int_d must be followed by the name of the dummy variable
                        if (!isalpha(in[t.scanpos + 5])) {
                            *errPos = t.scanpos;
                            *errType = ERR_SYNTAX;
                            ppl_snprintfTrunc(
                                    errText, PPL_MSG_LENGTH, "%s",
                                    "System function int_d should be followed by a variable name to integrate over.");
                            *end = -1;
                            *outlen = 0;
                            return;
                        }
                        if (tokenNewState(&t, trialstate, 0, 0, 5)) goto overflow;
                        trialstate = 'B';
                        if (tokenNewState(&t, trialstate, 0, 0, 1)) goto overflow;
                        while ((isalnum(in[t.scanpos])) || (in[t.scanpos] == '_')) {
                            if (tokenSameState(&t)) goto overflow;
                        }
                        trialstate = tmp;
                        if (tokenNewState(&t, trialstate, 0, 0, 0)) goto overflow;
                        expOp = 1;
                        extraArg = 1;
                    } else if (strncmp(in + t.scanpos, "diff_d", 6) == 0) // diff_d() function
                    {
                        // diff_d must be followed by the name of the dummy variable
                        if (!isalpha(in[t.scanpos + 6])) {
                            *errPos = t.scanpos;
                            *errType = ERR_SYNTAX;
                            ppl_snprintfTrunc(
                                    errText, PPL_MSG_LENGTH, "%s",
                                    "System function diff_d should be followed by a variable name to differentiate with respect to.");
                            *end = -1;
                            *outlen = 0;
                            return;
                        }
                        if (tokenNewState(&t, trialstate, 0, 0, 6)) goto overflow;
                        trialstate = 'B';
                        if (tokenNewState(&t, trialstate, 0, 0, 1)) goto overflow;
                        while ((isalnum(in[t.scanpos])) || (in[t.scanpos] == '_')) {
                            if (tokenSameState(&t)) goto overflow;
                        }
                        trialstate = tmp;
                        if (tokenNewState(&t, trialstate, 0, 0, 0)) goto overflow;
                        expOp = 1;
                        extraArg = 1;
                    } else {
                        if (tokenNewState(&t, trialstate, 0, 0, 1)) goto overflow;
                        while ((isalnum(in[t.scanpos])) || (in[t.scanpos] == '_')) {
                            if (tokenSameState(&t)) goto overflow;
                        }
                        expOp = 0;
                    }
                }
            } else if (trialstate == 'I') // a unary value-based operator
            {
                // Unary operators, which act on values: -, +, ~ (bitwise not) and ! (logical not)
                if (markupMatch(in + t.scanpos, "-")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_NEGATE, 3, 1)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "+")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_UNARY_PLUS, 3, 1)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "~")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_BITWISE_NOT, 3, 1)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "!")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_LOGICAL_NOT, 3, 1)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "not") && (!isalpha(in[t.scanpos + 3]))) {
                    if (tokenNewState(&t, trialstate, PPL_OP_LOGICAL_NOT, 3, 3)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "NOT") && (!isalpha(in[t.scanpos + 3]))) {
                    if (tokenNewState(&t, trialstate, PPL_OP_LOGICAL_NOT, 3, 3)) goto overflow;
                }
            } else if (trialstate == 'J') // a binary operator
            {
                // Binary operators, longest first, so that e.g. ** is not read as *. The numbers are their precedences,
                // smaller binding more tightly.
                if (markupMatch(in + t.scanpos, "**")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_POWER, 2, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "<<")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_SHIFT_LEFT, 7, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, ">>")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_SHIFT_RIGHT, 7, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "<=")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_LESS_EQUAL, 8, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, ">=")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_GREATER_EQUAL, 8, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "==")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_EQUAL, 9, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "<>")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_NOT_EQUAL, 10, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "!=")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_NOT_EQUAL, 10, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "&&")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_LOGICAL_AND, 14, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "and") && (!isalpha(in[t.scanpos + 3]))) {
                    if (tokenNewState(&t, trialstate, PPL_OP_LOGICAL_AND, 14, 3)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "AND") && (!isalpha(in[t.scanpos + 3]))) {
                    if (tokenNewState(&t, trialstate, PPL_OP_LOGICAL_AND, 14, 3)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "||")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_LOGICAL_OR, 15, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "or") && (!isalpha(in[t.scanpos + 2]))) {
                    if (tokenNewState(&t, trialstate, PPL_OP_LOGICAL_OR, 15, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "OR") && (!isalpha(in[t.scanpos + 2]))) {
                    if (tokenNewState(&t, trialstate, PPL_OP_LOGICAL_OR, 15, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "*")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_MULTIPLY, 5, 1)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "/")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_DIVIDE, 5, 1)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "%")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_MODULO, 5, 1)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "+")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_ADD, 6, 1)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "-")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_SUBTRACT, 6, 1)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "<")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_LESS, 8, 1)) goto overflow;
                } else if (markupMatch(in + t.scanpos, ">")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_GREATER, 8, 1)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "&")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_BITWISE_AND, 11, 1)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "^")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_BITWISE_XOR, 12, 1)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "|")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_BITWISE_OR, 13, 1)) goto overflow;
                } else if ((markupMatch(in + t.scanpos, ",") &&
                            (collectCommas || allowCommaOperator || (tertiaryDepth > 0)))) {
                    // A comma is either the comma operator, or separates the items of a list, dictionary or function
                    // arguments, which are counted
                    if ((!collectCommas) || (tertiaryDepth > 0)) {
                        if (tokenNewState(&t, trialstate, PPL_OP_COMMA, 18, 1)) goto overflow;
                    } else {
                        // In a dictionary, a comma must follow a key:value pair
                        if (isDict && (nCommaItems >= 2 - (nDictItems == 0))) {
                            *errPos = t.scanpos;
                            *errType = ERR_SYNTAX;
                            ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s",
                                              "Expecting : followed by value for dictionary key");
                            *end = -1;
                            *outlen = 0;
                            return;
                        }
                        if (tokenNewState(&t, trialstate, PPL_OP_COLLECT, 18, 1)) goto overflow;
                        nCommaItems++;
                    }
                }
            } else if (trialstate == 'K') // a ternary operator
            {
                // The ternary operator ?: ; tertiaryDepth counts the ? waiting for their :
                if (markupMatch(in + t.scanpos, "?")) {
                    if (tokenNewState(&t, trialstate, PPL_OP_TERNARY_QUESTION, 16, 1)) goto overflow;
                    tertiaryDepth++;
                } else if (markupMatch(in + t.scanpos, ":")) {
                    // In a dictionary, a : which is not part of a ?: separates a key from its value
                    if (isDict && (tertiaryDepth == 0)) {
                        // Each key must be followed by exactly one :
                        if (nCommaItems < 2 - (nDictItems == 0)) {
                            *errPos = t.scanpos;
                            *errType = ERR_SYNTAX;
                            ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s",
                                              "Expecting , to separate dictionary items");
                            *end = -1;
                            *outlen = 0;
                            return;
                        }
                        if (tokenNewState(&t, trialstate, PPL_OP_COLLECT, 18, 1)) goto overflow;
                        nCommaItems = 1;
                        nDictItems++;
                    } else {
                        // The : of a ternary operator
                        if (tertiaryDepth > 0) {
                            tertiaryDepth--;
                            if (tokenNewState(&t, trialstate, PPL_OP_TERNARY_COLON, 16, 1)) goto overflow;
                        }
                    }
                }
            } else if (trialstate == 'L') // a numeric literal
            {
                int j = 0;
                if (ppl_validFloat(in + t.scanpos, &j)) {
                    if (tokenNewState(&t, trialstate, 0, 0, j)) goto overflow;
                }
            } else if ((trialstate == 'M') || // a list literal
                       (trialstate == 'Q')) // an array dereference []
            {
                expOp = 0;
                // A list literal, or a slice or item lookup, e.g. a[1:3]
                if (markupMatch(in + t.scanpos, "[")) {
                    int j, m = 0, n = 1;
                    int slice = 0, minSet = 0, maxSet = 0;
                    ppl_strBracketMatch(in + t.scanpos, '[', ']', NULL, NULL, &j, 0); // Search for a ] to match the [
                    // The bracket must be closed
                    if (j <= 0) {
                        *errPos = t.scanpos;
                        *errType = ERR_SYNTAX;
                        ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s", "Mismatched [ ]");
                        *end = -1;
                        *outlen = 0;
                        return;
                    }
                    if (tokenNewState(&t, trialstate, 0, 0, 1))
                        goto overflow; // Record the one character opening bracket
                    if ((trialstate == 'Q') && (in[t.scanpos] == ':')) // slice of the form [:x]
                    {
                        if (tokenSameState(&t)) goto overflow;
                        n++;
                        slice = 1;
                        // Skip whitespace after the opening bracket
                        while ((in[t.scanpos] != '\0') && ((unsigned char) in[t.scanpos] <= ' ')) {
                            if (tokenSameState(&t)) goto overflow;
                            n++;
                        } // Sop up whitespace
                    }
                    int k = t.scanpos;
                    int l = t.outpos;
                    j -= n;
                    if ((in[k] != ']') ||
                        ((trialstate == 'Q') &&
                         (!slice))) // [] is allowed as a list literal, but not as an array dereference
                    {
                        if (slice) maxSet = 1;
                        ppl_expTokenise(context, in + k, &m, dollarAllowed, 1, 1, trialstate != 'Q', 0, l + outOffset,
                                        &n, errPos, errType,
                                        errText); // Hierarchically tokenise the inside of the brackets
                        // Report a syntax error within the brackets
                        if (*errPos >= 0) {
                            *errPos += k;
                            return;
                        }
                        // A : within [] makes this a slice, with an upper limit after the :
                        if ((trialstate == 'Q') && (!slice) && (in[k + m] == ':')) {
                            slice = minSet = 1;
                            if (tokenFastForward(&t, m)) goto overflow;
                            j -= m; // Fast-forward over first expression in range
                            trialstate = 'J';
                            if (tokenNewState(&t, trialstate, PPL_OP_COLLECT, 18, 1)) goto overflow;
                            j--; // Treat : separating ranges as a , operator (collect items on stack)
                            // Skip whitespace after the :
                            while ((in[t.scanpos] != '\0') && ((unsigned char) in[t.scanpos] <= ' ')) {
                                if (tokenSameState(&t)) goto overflow;
                                j--;
                            } // Sop up whitespace
                            trialstate = 'Q';
                            k = t.scanpos;
                            l = t.outpos;
                            m = 0;
                            // Tokenise the upper limit of the slice, if there is one
                            if (j > 0) {
                                maxSet = 1;
                                ppl_expTokenise(context, in + k, &m, dollarAllowed, 1, 1, trialstate != 'Q', 0,
                                                l + outOffset, &n, errPos, errType,
                                                errText); // Hierarchically tokenise the inside of the brackets
                                // Report a syntax error within the brackets
                                if (*errPos >= 0) {
                                    *errPos += k;
                                    return;
                                }
                            }
                        }
                        // The contents must use everything up to the closing bracket
                        if (m != j) {
                            *errPos = m + k;
                            *errType = ERR_SYNTAX;
                            ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s",
                                              "Unexpected trailing matter at end of expression");
                            *end = -1;
                            *outlen = 0;
                            return;
                        }
                    } else tokenOut(&t)->depth = 0;
                    if (tokenFastForward(&t, j)) goto overflow; // Fast-forward to closing bracket
                    // Record which limits of a slice were given in the opcode of its closing bracket
                    if (trialstate == 'Q') {
                        tokenOut(&t)->opcode = (minSet ? PPL_SLICE_MIN_SET : 0) | (maxSet ? PPL_SLICE_MAX_SET : 0) |
                                (slice ? 0 : PPL_SLICE_SINGLE_ITEM);
                        tokenOut(&t)->precedence = 0;
                    }
                    if (tokenNewState(&t, trialstate, tokenOut(&t)->opcode, tokenOut(&t)->precedence, 1))
                        goto overflow; // Record the one character closing bracket
                }
            } else if (trialstate == 'N') // a dictionary literal
            {
                // A dictionary literal
                if (markupMatch(in + t.scanpos, "{")) {
                    int j, m, n = 1;
                    ppl_strBracketMatch(in + t.scanpos, '{', '}', NULL, NULL, &j, 0); // Search for a } to match the {
                    // The brace must be closed
                    if (j <= 0) {
                        *errPos = t.scanpos;
                        *errType = ERR_SYNTAX;
                        ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s", "Mismatched { }");
                        *end = -1;
                        *outlen = 0;
                        return;
                    }
                    if (tokenNewState(&t, trialstate, 0, 0, 1))
                        goto overflow; // Record the one character opening bracket
                    // Skip whitespace after the opening brace
                    while ((in[t.scanpos] != '\0') && ((unsigned char) in[t.scanpos] <= ' ')) {
                        if (tokenSameState(&t)) goto overflow;
                        n++;
                    } // Sop up whitespace
                    const int k = t.scanpos;
                    const int l = t.outpos;
                    j -= n;
                    // Unless the dictionary is empty, tokenise its contents recursively
                    if (in[k] != '}') {
                        ppl_expTokenise(context, in + k, &m, dollarAllowed, 1, 1, 1, 1, l + outOffset, &n, errPos,
                                        errType, errText); // Hierarchically tokenise the inside of the brackets
                        // Report a syntax error within the braces
                        if (*errPos >= 0) {
                            *errPos += k;
                            return;
                        }
                        // The contents must use everything up to the closing brace
                        if (m != j) {
                            *errPos = m + k;
                            *errType = ERR_SYNTAX;
                            ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s",
                                              "Unexpected trailing matter at end of expression");
                            *end = -1;
                            *outlen = 0;
                            return;
                        }
                    } else tokenOut(&t)->depth = 0;
                    if (tokenFastForward(&t, j)) goto overflow; // Fast-forward to closing bracket
                    if (tokenNewState(&t, trialstate, 0, 0, 1))
                        goto overflow; // Record the one character closing bracket
                }
            } else if (trialstate == 'O') // a dollar sign
            {
                if ((dollarAllowed) && (markupMatch(in + t.scanpos, "$"))) {
                    if (tokenNewState(&t, trialstate, 0, 0, 1)) goto overflow;
                }
            } else if (trialstate == 'R') // the dot operator
            {
                if (markupMatch(in + t.scanpos, ".")) {
                    if (tokenNewState(&t, trialstate, 0, 0, 1)) goto overflow;
                }
            } else if (trialstate == 'S') // assignment operator
            {
                if ((markupMatch(in + t.scanpos, "=") && (in[t.scanpos + 1] != '=') && equalsAllowed)) {
                    if (tokenNewState(&t, trialstate, PPL_ASSIGN, 17, 1)) goto overflow;
                } // Match = but not ==
                // The fused assignment operators
                else if (markupMatch(in + t.scanpos, "+=")) {
                    if (tokenNewState(&t, trialstate, PPL_ASSIGN_ADD, 17, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "-=")) {
                    if (tokenNewState(&t, trialstate, PPL_ASSIGN_SUBTRACT, 17, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "*=")) {
                    if (tokenNewState(&t, trialstate, PPL_ASSIGN_MULTIPLY, 17, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "/=")) {
                    if (tokenNewState(&t, trialstate, PPL_ASSIGN_DIVIDE, 17, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "%=")) {
                    if (tokenNewState(&t, trialstate, PPL_ASSIGN_MODULO, 17, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "&=")) {
                    if (tokenNewState(&t, trialstate, PPL_ASSIGN_BITWISE_AND, 17, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "^=")) {
                    if (tokenNewState(&t, trialstate, PPL_ASSIGN_BITWISE_XOR, 17, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "|=")) {
                    if (tokenNewState(&t, trialstate, PPL_ASSIGN_BITWISE_OR, 17, 2)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "<<=")) {
                    if (tokenNewState(&t, trialstate, PPL_ASSIGN_SHIFT_LEFT, 17, 3)) goto overflow;
                } else if (markupMatch(in + t.scanpos, ">>=")) {
                    if (tokenNewState(&t, trialstate, PPL_ASSIGN_SHIFT_RIGHT, 17, 3)) goto overflow;
                } else if (markupMatch(in + t.scanpos, "**=")) {
                    if (tokenNewState(&t, trialstate, PPL_ASSIGN_POWER, 17, 3)) goto overflow;
                }
            } else if (trialstate == 'U') // end of expression
            {
                if (tokenNewState(&t, trialstate, 0, 0, 0)) goto overflow;
                break; // State U consumes no characters, and has no successor states, so stop looking for one
            }
            if (t.scanpos != oldpos) break;
        }
        if (t.scanpos == oldpos) break; // We've got stuck
    }

    // Every ? of a ternary operator must be matched by a :
    if ((t.state == 'U') && (tertiaryDepth > 0)) {
        *errPos = t.scanpos;
        *errType = ERR_SYNTAX;
        ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s", "Expecting : to complete the ternary operator a ? b : c");
        *end = -1;
        *outlen = 0;
        return;
    }

    // In a dictionary literal, the last key must be followed by a colon and a value, just as the others are (which is
    // checked when the comma after each item is reached). Empty dictionaries are not passed to the tokeniser.
    if ((t.state == 'U') && isDict && ((nDictItems == 0) || (nCommaItems >= 2))) {
        *errPos = t.scanpos;
        *errType = ERR_SYNTAX;
        ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s", "Expecting : followed by value for dictionary key");
        *end = -1;
        *outlen = 0;
        return;
    }

    if (t.state == 'U') // We reached t.state U... end of expression
    {
        *errPos = -1;
        *errType = 0;
        *errText = '\0';
        tokenOut(&t)->state = '@';
        tokenOut(&t)->opcode = 0;
        // Record in the end-of-expression token how many items a list, dictionary or argument list contained
        if (isDict) {
            tokenOut(&t)->depth = nDictItems;
        } else if (collectCommas) {
            tokenOut(&t)->depth = nCommaItems;
        } else {
            tokenOut(&t)->depth = 0;
        }
        t.outpos++;
        *end = t.scanpos;
        *outlen = t.outpos;
        return;
    } else {
        // Error; we didn't reach t.state U
        int i = 0, j = 0;
        int B = 0, F = 0, G = 0, J = 0, P = 0;
        *errPos = t.scanpos;
        *end = -1;
        *outlen = 0;
        // Now we need to construct an error string
        *errType = ERR_SYNTAX;
        ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s", "At this point, was expecting ");
        i = strlen(errText);
        // Describe each of the states which could have come next, in words
        for (int trialpos = 0; ((trialstate = allowed[(int) (t.state - 'A')][trialpos]) != '\0'); trialpos++) {
            if ((expOp) && (trialstate != 'P'))
                continue; // int_dx and diff_dx MUST be followed by arguments; they have already put one on the stack.

            // Literals of any kind are described just once
            if ((!B) &&
                ((trialstate == 'B') || (trialstate == 'L') || (trialstate == 'M') || (trialstate == 'N') ||
                 (trialstate == 'O'))) {
                appendOr(errText, &i, &j);
                strcpy(errText + i, "a literal value");
                i += strlen(errText + i);
                B = 1;
            } else if (trialstate == 'C') {
                appendOr(errText, &i, &j);
                strcpy(errText + i, "a string substitution operator");
                i += strlen(errText + i);
            } else if (trialstate == 'D') {
                appendOr(errText, &i, &j);
                strcpy(errText + i, "a list of string substitutions");
                i += strlen(errText + i);
            } else if (trialstate == 'E') {
                appendOr(errText, &i, &j);
                strcpy(errText + i, "a bracketed expression");
                i += strlen(errText + i);
            } else if ((!F) && ((trialstate == 'F') || (trialstate == 'H') || (trialstate == 'I'))) {
                appendOr(errText, &i, &j);
                strcpy(errText + i, "a unary operator");
                i += strlen(errText + i);
                F = 1;
            } else if ((!G) && ((trialstate == 'G') || (trialstate == 'T'))) {
                appendOr(errText, &i, &j);
                strcpy(errText + i, "a variable name");
                i += strlen(errText + i);
                G = 1;
            } else if ((!J) && ((trialstate == 'J') || (trialstate == 'K') || (trialstate == 'S'))) {
                appendOr(errText, &i, &j);
                strcpy(errText + i, "a binary/ternary operator");
                i += strlen(errText + i);
                J = 1;
            } else if (trialstate == 'P') {
                appendOr(errText, &i, &j);
                strcpy(errText + i, "a list of function arguments");
                i += strlen(errText + i);
            } else if ((!P) && ((trialstate == 'Q') || (trialstate == 'R'))) {
                appendOr(errText, &i, &j);
                strcpy(errText + i, "an object dereference");
                i += strlen(errText + i);
                P = 1;
            }
        }
        strcpy(errText + i, ".");
        i += strlen(errText + i);
        return;
    }

// The token buffer could not be enlarged to hold the expression
overflow:
    // The token buffer could not be enlarged to hold the expression
    *errPos = t.scanpos;
    *errType = ERR_OVERFLOW;
    ppl_snprintfTrunc(errText, PPL_MSG_LENGTH, "%s", "Algebraic expression too long");
    *end = -1;
}

//! ppl_tokenPrint - Print the tokenised form of an expression to stdout, for debugging
//! \param [in] context - The Pyxplot context, whose <tokenBuff> holds the tokens
//! \param [in] in - The expression which was tokenised
//! \param [in] len - The number of characters in the expression

void ppl_tokenPrint(const ppl_context *context, char *in, const int len) {
    pplTokenCode *tdat = context->tokenBuff;
    for (int i = 0; i < len; i++) printf("  %c", in[i]);
    printf("\n");
    for (int i = 0; i < len; i++) printf("  %c", tdat[i].state);
    printf("\n");
    for (int i = 0; i < len; i++) printf(" %02x", (int) tdat[i].opcode);
    printf("\n");
    for (int i = 0; i < len; i++) printf(" %02x", (int) tdat[i].precedence);
    printf("\n");
    for (int i = 0; i < len; i++) printf(" %02x", (int) tdat[i].depth);
    printf("\n");
    return;
}

// ----------------------------------------------------------------------------
// ppl_expCompile() -- compile an expression into reverse-Polish bytecode
// ----------------------------------------------------------------------------
//
// The compiler uses Dijkstra's shunting-yard algorithm. It reads the tokens produced by ppl_expTokenise() from left to
// right, and produces a list of instructions (pplExprInstruction) for a stack machine, in which every operator comes
// after its operands. For example, a+b*c compiles to:
//
//     lookup a; lookup b; lookup c; operator *; operator +; return
//
// Values (numbers, strings, variable names) are written straight to the output as soon as they are read. Operators
// cannot be written straight away, because their right-hand operand has not been read yet, so they wait on an
// operator stack of pplExprStackItem entries. Each time a new operator is read, any operators already waiting on the
// stack which bind more tightly are popped off and written to the output first (see popOperators()). Then the new
// operator is pushed onto the stack. Opening brackets are also pushed onto the stack, and act as barriers: a closing
// bracket pops every operator back to its matching opening bracket. At the end of the expression, every operator left
// on the stack is popped.
//
// Precedence numbers are taken from the tokeniser: a lower number means an operator binds more tightly. When a new
// operator has the same precedence as the one on top of the stack, a left-associative operator (e.g. a-b-c means
// (a-b)-c) pops it, while a right-associative operator (e.g. a**b**c means a**(b**c)) does not.
//
// Some operators need special treatment:
//  * && and || only evaluate their right-hand operand if needed. a&&b compiles to:
//        <a>; branch-if-false to END (keeping a on the stack); <b>; make-boolean; END:
//    The branch instruction is written when && is read, and its target is filled in when && is popped.
//  * a?b:c compiles to:
//        <a>; branch-if-false to ELSE (popping a); <b>; goto END; ELSE: <c>; END:
//    When ? is read, the conditional branch is written, and ? is pushed onto the stack. When : is read, everything
//    back to the ? is popped, the goto is written, and the conditional branch is pointed at the instruction after it.
//    Then : is pushed onto the stack, and the goto is pointed at the end of <c> when : is popped.
//  * In an assignment x=..., the variable x is written as a lookup when it is read, before the = is seen. When = is
//    read, that lookup is converted into a lookup of a pointer to x (see convertToPointer()).
//  * Function calls, list literals, dictionary literals and string substitutions such as "%d"%(x) collect several
//    comma-separated items. The tokeniser records how many there are against the closing bracket.

//! pplExprCompiler - The working state of the compiler while it compiles one expression
typedef struct pplExprCompiler {
    ppl_context *context; // The Pyxplot context, which owns the memory used for the operator stack
    pplExpr *expr; // The expression being compiled. Its <bytecode> and <strings> arrays grow as needed.
    int bytecodeAlloc; // The number of instructions for which space is allocated in <expr->bytecode>
    int stringsAlloc; // The number of bytes for which space is allocated in <expr->strings>
    int lastInstruction; // The index of the most recently written instruction, or -1 if there is none
    pplExprStackItem *stack; // The operator stack (this is <context->parserStack>)
    int stackLen; // The number of items currently on the operator stack
    int *errPos; // Set to the position of any error in the expression
    int *errType; // Set to the type of any error
    char *errText; // Buffer into which any error message is written
    int *end; // Set to -1 on error
} pplExprCompiler;

//! compileError - Report an error encountered while compiling an expression
//! \param [in] c - The compiler state
//! \param [in] pos - The position in the source text at which the error occurred
//! \param [in] type - The type of the error, e.g. ERR_SYNTAX
//! \param [in] text - The error message

static void compileError(pplExprCompiler *c, const int pos, const int type, const char *text) {
    *c->errPos = pos;
    *c->errType = type;
    strcpy(c->errText, text);
    *c->end = -1;
}

//! emitInstruction - Append a new instruction to the end of the bytecode, with an operand of zero
//! \param [in] c - The compiler state
//! \param [in] opcode - The instruction to append
//! \param [in] charpos - The position of the token in the source text which produced the instruction
//! \return Pointer to the new instruction, so that its operand can be set, or NULL if memory ran out. The pointer
//! is only valid until the next instruction is appended.

static pplExprInstruction *emitInstruction(pplExprCompiler *c, const pplExprOpcode opcode, const int charpos) {
    pplExpr *const expr = c->expr;
    // Double the space for bytecode when it is full
    if (expr->bcLen >= c->bytecodeAlloc) {
        pplExprInstruction *const grown =
                (pplExprInstruction *) realloc(expr->bytecode, 2 * c->bytecodeAlloc * sizeof(pplExprInstruction));
        // Failure to allocate memory is reported as an error
        if (grown == NULL) {
            compileError(c, 0, ERR_MEMORY, "Out of memory.");
            return NULL;
        }
        expr->bytecode = grown;
        c->bytecodeAlloc *= 2;
    }
    c->lastInstruction = expr->bcLen++;
    pplExprInstruction *instruction = &expr->bytecode[c->lastInstruction];
    memset(instruction, 0, sizeof(pplExprInstruction));
    instruction->opcode = opcode;
    instruction->charpos = charpos;
    return instruction;
}

//! reserveString - Make sure there is space for a new string of up to <maxLen> characters (plus a null terminator)
//! at the end of the expression's string table
//! \param [in] c - The compiler state
//! \param [in] maxLen - The maximum length of the string which will be written
//! \return Pointer to where the string should be written, or NULL if memory ran out. Once written, the string must
//! be added to the table with commitString().

static char *reserveString(pplExprCompiler *c, const int maxLen) {
    pplExpr *const expr = c->expr;
    // Grow the string table when it is full
    if (expr->stringsLen + maxLen + 1 > c->stringsAlloc) {
        const int newAlloc = 2 * (expr->stringsLen + maxLen + 1);
        char *const grown = (char *) realloc(expr->strings, newAlloc);
        // Failure to allocate memory is reported as an error
        if (grown == NULL) {
            compileError(c, 0, ERR_MEMORY, "Out of memory.");
            return NULL;
        }
        expr->strings = grown;
        c->stringsAlloc = newAlloc;
    }
    return expr->strings + expr->stringsLen;
}

//! commitString - Add a string written to the space returned by reserveString() to the expression's string table
//! \param [in] c - The compiler state
//! \param [in] len - The length of the string which was written (excluding any null terminator)
//! \return The offset of the string within the string table

static int commitString(pplExprCompiler *c, const int len) {
    pplExpr *const expr = c->expr;
    const int offset = expr->stringsLen;
    expr->strings[offset + len] = '\0';
    expr->stringsLen += len + 1;
    return offset;
}

//! pushStackItem - Push a new item onto the operator stack, enlarging the stack if necessary
//! \param [in] c - The compiler state
//! \param [in] type - The type of the item
//! \param [in] operatorCode - For operators, the operator code (PPL_OP_* or PPL_ASSIGN_*)
//! \param [in] precedence - For operators, the precedence of the operator
//! \param [in] charpos - The position of the token in the source text
//! \return Pointer to the new item, or NULL if memory ran out. The pointer is only valid until the next push.

static pplExprStackItem *pushStackItem(pplExprCompiler *c, const pplExprStackItemType type,
                                       const unsigned char operatorCode, const unsigned char precedence,
                                       const int charpos) {
    ppl_context *const context = c->context;
    // Double the size of the compiler's stack when it is full
    if (c->stackLen >= context->parserStackLen) {
        pplExprStackItem *const grown = (pplExprStackItem *) realloc(
                context->parserStack, 2 * context->parserStackLen * sizeof(pplExprStackItem));
        // Failure to allocate memory is reported as an error
        if (grown == NULL) {
            compileError(c, charpos, ERR_OVERFLOW, "Stack overflow whilst parsing algebraic expression.");
            return NULL;
        }
        context->parserStack = c->stack = grown;
        context->parserStackLen *= 2;
    }
    pplExprStackItem *item = &c->stack[c->stackLen++];
    item->type = type;
    item->operatorCode = operatorCode;
    item->precedence = precedence;
    item->charpos = charpos;
    item->branchInstruction = -1;
    return item;
}

//! emitStackItem - Write the instruction(s) for an operator which is being popped from the operator stack, now that
//! all of its operands have been written
//! \param [in] c - The compiler state
//! \param [in] item - The operator being popped
//! \return Zero on success, or nonzero if memory ran out

static int emitStackItem(pplExprCompiler *c, const pplExprStackItem *item) {
    // Write the instruction which applies an operator, or completes a bracket, taken from the compiler's stack
    pplExprInstruction *instruction = NULL;
    // The type of stack item determines the instruction
    switch (item->type) {
        case PPL_STACK_ASSIGNMENT:
            instruction = emitInstruction(c, PPL_BC_ASSIGN, item->charpos);
            if (instruction == NULL) return 1;
            instruction->operand.operatorCode = item->operatorCode;
            return 0;
        case PPL_STACK_DOLLAR:
            return emitInstruction(c, PPL_BC_DOLLAR, item->charpos) == NULL;
        case PPL_STACK_OPERATOR:
            break;
        default:
            return 0; // Brackets are never popped by this function
    }

    // Some operators need more than one instruction
    switch (item->operatorCode) {
        case PPL_OP_LOGICAL_AND:
        case PPL_OP_LOGICAL_OR:
            // Both operands have now been written. Convert the result to a boolean, and point the branch which
            // skips the right-hand operand at the instruction after this one.
            if (emitInstruction(c, PPL_BC_MAKE_BOOLEAN, item->charpos) == NULL) return 1;
            c->expr->bytecode[item->branchInstruction].operand.target = c->expr->bcLen;
            return 0;
        case PPL_OP_TERNARY_COLON:
            // The third operand of a?b:c has now been written. Point the goto which skips it at the next instruction.
            c->expr->bytecode[item->branchInstruction].operand.target = c->expr->bcLen;
            return 0;
        case PPL_OP_COLLECT:
            // Commas which separate the items of a list do nothing; the items are collected by the closing bracket
            return 0;
        case PPL_OP_STRING_SUBST:
            // A string substitution operator with a single value, e.g. "%d"%x. With a bracketed list of values, e.g.
            // "%d %d"%(x,y), the operator is removed from the stack when the closing bracket is read instead.
            instruction = emitInstruction(c, PPL_BC_STRING_SUBST, item->charpos);
            if (instruction == NULL) return 1;
            instruction->operand.count = 1;
            return 0;
        case PPL_OP_PRE_DECREMENT:
        case PPL_OP_PRE_INCREMENT:
            instruction = emitInstruction(c, PPL_BC_INCREMENT, item->charpos);
            if (instruction == NULL) return 1;
            instruction->operand.operatorCode = item->operatorCode;
            return 0;
        default:
            instruction = emitInstruction(c, PPL_BC_OPERATOR, item->charpos);
            if (instruction == NULL) return 1;
            instruction->operand.operatorCode = item->operatorCode;
            return 0;
    }
}

//! popOperators - Pop operators from the top of the operator stack, writing their instructions, until reaching an
//! operator which binds less tightly than a new operator, or an opening bracket, or the bottom of the stack
//! \param [in] c - The compiler state
//! \param [in] precedence - The precedence of the new operator. Use 255 to pop all operators back to the nearest
//! bracket.
//! \param [in] rightAssoc - True if the new operator is right-associative, in which case operators of equal
//! precedence are left on the stack
//! \return Zero on success, or nonzero if memory ran out

static int popOperators(pplExprCompiler *c, const int precedence, const int rightAssoc) {
    // The shunting-yard algorithm: before an operator is pushed, emit the operators on the stack which bind at least as
    // tightly (or, for right-associative operators, more tightly)
    while (c->stackLen > 0) {
        const pplExprStackItem *const top = &c->stack[c->stackLen - 1];
        int stackPrecedence = top->precedence;

        // Only operators can be popped; brackets and the ? of a ternary operator wait for their closing partner
        if ((top->type != PPL_STACK_OPERATOR) && (top->type != PPL_STACK_ASSIGNMENT) && (top->type != PPL_STACK_DOLLAR))
            break;

        // ** (precedence 2) binds more tightly than unary minus (precedence 3) on its left, so that -2**2 is -4.
        // But a unary minus on its right, as in 2**-1, must be applied first, so it is treated as binding more
        // tightly than ** in that case.
        if ((stackPrecedence == 2) && (precedence == 3)) stackPrecedence = 4;

        // Stop at an operator which binds less tightly
        if (rightAssoc ? (precedence <= stackPrecedence) : (precedence < stackPrecedence)) break;

        // Emit the operator, and pop it
        if (emitStackItem(c, top)) return 1;
        c->stackLen--;
    }
    return 0;
}

//! popBracket - Pop all operators back to the matching opening bracket, and then pop the bracket itself
//! \param [in] c - The compiler state
//! \param [in] type - The type of opening bracket expected
//! \param [in] tpos - The position of the closing bracket in the source text
//! \param [in] errorText - The error message to report if there is no matching opening bracket
//! \param [out] openPos - Set to the position of the opening bracket in the source text
//! \return Zero on success, or nonzero on error

static int popBracket(pplExprCompiler *c, const pplExprStackItemType type, const int tpos, const char *errorText,
                      int *openPos) {
    if (popOperators(c, 255, 0)) return 1;
    // The top of the stack must now be the matching opening bracket
    if ((c->stackLen < 1) || (c->stack[c->stackLen - 1].type != type)) {
        compileError(c, tpos, ERR_INTERNAL, errorText);
        return 1;
    }
    *openPos = c->stack[c->stackLen - 1].charpos;
    c->stackLen--;
    return 0;
}

//! convertToPointer - In an assignment such as x=3, the variable x was compiled into an instruction to look up its
//! value. Convert that instruction into one which looks up a pointer to x instead, so that x can be assigned to, and
//! need not exist already.
//! \param [in] c - The compiler state
//! \param [in] tpos - The position of the assignment operator in the source text
//! \return Zero on success, or nonzero on error

static int convertToPointer(pplExprCompiler *c, const int tpos) {
    // The instruction just written, which fetched the value being assigned to, is changed to fetch a pointer to it
    // instead
    // There must be such an instruction
    if (c->lastInstruction < 0) {
        compileError(c, tpos, ERR_INTERNAL, "Could not find variable name preceding assignment operator.");
        return 1;
    }
    pplExprInstruction *instruction = &c->expr->bytecode[c->lastInstruction];
    // Variable lookups, member lookups and item lookups each have a pointer form
    switch (instruction->opcode) {
        case PPL_BC_LOOKUP:
            instruction->opcode = PPL_BC_LOOKUP_POINTER;
            break;
        case PPL_BC_DEREFERENCE:
            instruction->opcode = PPL_BC_DEREFERENCE_POINTER;
            break;
        case PPL_BC_SLICE:
            // A slice of several items cannot be assigned to
            if ((instruction->operand.sliceFlags & PPL_SLICE_SINGLE_ITEM) == 0) {
                compileError(c, tpos, ERR_SYNTAX,
                             "Cannot apply an assignment operator to the output of a slice operator.");
                return 1;
            }
            instruction->opcode = PPL_BC_SLICE_POINTER;
            break;
        default:
            break;
    }
    return 0;
}

//! decodeEscape - Decode the character after a backslash in an escape sequence within a string literal
//! \param [in] next - The character after the backslash
//! \param [in] extended - If true, C-style escape sequences such as \n are recognised. Otherwise, only quote
//! characters and backslashes can be escaped.
//! \return The character represented by the escape sequence, or zero if it is not a recognised escape sequence

static char decodeEscape(const char next, const int extended) {
    if ((next == '\'') || (next == '\"') || (next == '\\')) return next;
    if (!extended) return '\0';
    // Map the character after a backslash to the character it stands for; most only apply to e'...' strings
    switch (next) {
        case '?':
            return '\?';
        case 'a':
            return '\a';
        case 'b':
            return '\b';
        case 'f':
            return '\f';
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\t';
        case 'v':
            return '\v';
        default:
            return '\0';
    }
}

//! compileStringLiteral - Write an instruction to push a string literal onto the stack. The literal may be quoted
//! with ' or ", or with triple quotes ''' or """, and may have the prefix r (raw: no escape sequences) or e
//! (extended escape sequences such as \n). Strings in some contexts, such as filenames, are also allowed to be
//! unquoted.
//! \param [in] c - The compiler state
//! \param [in] in - The source text of the expression
//! \param [in] tdata - The tokens of the expression
//! \param [in] tlen - The number of tokens
//! \param [in] maxLen - An upper limit on the length of any string within the expression
//! \param [in,out] tpos - The position of the start of the string. For unquoted strings, this is advanced to the end
//! of the string.
//! \return Zero on success, or nonzero on error

static int compileStringLiteral(pplExprCompiler *c, const char *in, const pplTokenCode *tdata, const int tlen,
                                const int maxLen, int *tpos) {
    enum { ESCAPES_NONE, ESCAPES_QUOTES, ESCAPES_EXTENDED } escapes = ESCAPES_QUOTES;
    const char state = tdata[*tpos].state;
    pplExprInstruction *const instruction = emitInstruction(c, PPL_BC_PUSH_STRING, *tpos);
    char *const out = reserveString(c, maxLen);
    int start = *tpos;
    char quote = in[start];
    int outLen = 0;

    // Failure to allocate memory abandons compilation
    if ((instruction == NULL) || (out == NULL)) return 1;

    // Prefixes r'...' and e'...'
    if (((quote == 'r') || (quote == 'e')) && ((in[start + 1] == '\'') || (in[start + 1] == '\"'))) {
        escapes = (quote == 'r') ? ESCAPES_NONE : ESCAPES_EXTENDED;
        start++;
        quote = in[start];
    }

    // Unquoted strings, e.g. the arguments of unit(), are copied as they are
    if ((quote != '\'') && (quote != '\"')) {
        // Unquoted string, which ends at the end of the token, excluding any trailing comma
        while ((*tpos < tlen) && (tdata[*tpos].state == state)) out[outLen++] = in[(*tpos)++];
        if ((outLen > 0) && (out[outLen - 1] == ',')) outLen--;
    } else {
        // Quoted string
        const int tripleQuote = (in[start + 1] == quote) && (in[start + 2] == quote);
        for (int i = start + (tripleQuote ? 3 : 1);
             tripleQuote ? ((in[i] != quote) || (in[i + 1] != quote) || (in[i + 2] != quote)) : (in[i] != quote); i++) {
            char decoded = in[i];
            // The tokeniser should have found the closing quote
            if (in[i] == '\0') {
                compileError(c, i, ERR_INTERNAL, "Unexpected end of string.");
                return 1;
            }
            // Decode escape sequences, unless the string is raw
            if ((in[i] == '\\') && (escapes != ESCAPES_NONE)) {
                // Escape sequences. Unrecognised ones are left as they are, including the backslash.
                const char escaped = decodeEscape(in[i + 1], escapes == ESCAPES_EXTENDED);
                // A recognised escape sequence replaces the backslash and the character after it; others are kept as
                // they are
                if (escaped != '\0') {
                    decoded = escaped;
                    i++; // Skip the character after the backslash
                }
            }
            out[outLen++] = decoded;
        }
    }
    instruction->operand.string = commitString(c, outLen);
    return 0;
}

//! compileName - Write an instruction for a variable name, a member name after a dot, or a column name after a $
//! \param [in] c - The compiler state
//! \param [in] opcode - The instruction to write: PPL_BC_LOOKUP for a variable name; PPL_BC_DEREFERENCE for a member
//! name; PPL_BC_PUSH_STRING for a column name
//! \param [in] in - The source text of the expression
//! \param [in] tdata - The tokens of the expression
//! \param [in] tlen - The number of tokens
//! \param [in] maxLen - An upper limit on the length of any string within the expression
//! \param [in,out] tpos - The position of the start of the name, which is advanced to the end of the name
//! \return Zero on success, or nonzero on error

static int compileName(pplExprCompiler *c, const pplExprOpcode opcode, const char *in, const pplTokenCode *tdata,
                       const int tlen, const int maxLen, int *tpos) {
    const char state = tdata[*tpos].state;
    pplExprInstruction *const instruction = emitInstruction(c, opcode, *tpos);
    char *const out = reserveString(c, maxLen);
    int outLen = 0;
    if ((instruction == NULL) || (out == NULL)) return 1;
    while ((*tpos < tlen) && (tdata[*tpos].state == state) && (isalnum(in[*tpos]) || (in[*tpos] == '_')))
        out[outLen++] = in[(*tpos)++];
    instruction->operand.string = commitString(c, outLen);
    return 0;
}

//! compileOperator - Process an operator token, popping any operators from the stack which bind more tightly, and
//! then pushing the new operator onto the stack
//! \param [in] c - The compiler state
//! \param [in] tokenState - The type of token: 'C' (string substitution %), 'H' (prefix ++ or --), 'I' (unary
//! operator), 'J' (binary operator), 'K' (? or :) or 'S' (assignment operator)
//! \param [in] token - The token
//! \param [in] tpos - The position of the operator in the source text
//! \return Zero on success, or nonzero on error

static int compileOperator(pplExprCompiler *c, const char tokenState, const pplTokenCode *token, const int tpos) {
    // Assignments, and operators such as **, are right-associative: a=b=c means a=(b=c)
    const unsigned char code = token->opcode;
    const int precedence = token->precedence;
    const int isAssignment = (tokenState == 'S');
    const int rightAssoc = ((code & PPL_OP_RIGHT_ASSOC) != 0) || isAssignment; // All assignments are right-assoc
    int branchInstruction = -1;
    pplExprStackItemType type;

    // Emit the operators which bind more tightly than this one
    if (popOperators(c, precedence, rightAssoc)) return 1;

    // Assignments, the ternary operator and the short-circuit operators need special treatment
    if (isAssignment) {
        // Turn the lookup of the variable being assigned into a pointer lookup now, while it is the last instruction
        if ((code == PPL_ASSIGN) && convertToPointer(c, tpos)) return 1;
    } else if ((code == PPL_OP_LOGICAL_AND) || (code == PPL_OP_LOGICAL_OR) || (code == PPL_OP_TERNARY_QUESTION)) {
        // a&&b: if a is false, skip b, leaving a on the stack as the result. Otherwise, pop a and evaluate b.
        // a||b: if a is true, skip b, leaving a on the stack as the result. Otherwise, pop a and evaluate b.
        // a?b:c: pop a. If it was false, skip b. The branch target is set when the matching : is read.
        pplExprInstruction *const instruction =
                emitInstruction(c, (code == PPL_OP_LOGICAL_OR) ? PPL_BC_BRANCH_IF_TRUE : PPL_BC_BRANCH_IF_FALSE, tpos);
        if (instruction == NULL) return 1;
        instruction->popOnBranch = (code == PPL_OP_TERNARY_QUESTION);
        branchInstruction = c->lastInstruction;
    } else if (code == PPL_OP_TERNARY_COLON) {
        // The second operand of a?b:c is complete. Pop everything back to the ?, then write a goto which skips the
        // third operand, and point the branch written by the ? at the start of the third operand.
        if (popOperators(c, 999, rightAssoc)) return 1;
        // The : must match a ?
        if ((c->stackLen < 1) || (c->stack[c->stackLen - 1].type != PPL_STACK_TERNARY)) {
            compileError(c, tpos, ERR_INTERNAL, "Could not match : to a ? in the ternary operator.");
            return 1;
        }
        const int questionBranch = c->stack[c->stackLen - 1].branchInstruction;
        c->stackLen--;
        if (emitInstruction(c, PPL_BC_GOTO, tpos) == NULL) return 1;
        branchInstruction = c->lastInstruction;
        c->expr->bytecode[questionBranch].operand.target = branchInstruction + 1;
    }

    // Push the new operator onto the stack, to wait for its right-hand operand
    if (isAssignment) type = PPL_STACK_ASSIGNMENT;
    else if (code == PPL_OP_TERNARY_QUESTION) type = PPL_STACK_TERNARY;
    else type = PPL_STACK_OPERATOR;
    pplExprStackItem *item = pushStackItem(c, type, code, precedence, tpos);
    if (item == NULL) return 1;
    item->branchInstruction = branchInstruction;
    return 0;
}

//! compileBracket - Process an opening or closing bracket
//! \param [in] c - The compiler state
//! \param [in] tokenState - The type of token: 'D' (list of values after the string substitution operator %), 'E'
//! (bracketed sub-expression), 'P' (function arguments), 'M' (list literal), 'N' (dictionary literal) or 'Q'
//! (subscript)
//! \param [in] token - The token
//! \param [in] bracket - The bracket character
//! \param [in] tpos - The position of the bracket in the source text
//! \return Zero on success, or nonzero on error

static int compileBracket(pplExprCompiler *c, const char tokenState, const pplTokenCode *token, const char bracket,
                          const int tpos) {
    pplExprStackItemType type;
    const char *mismatchError;
    pplExprInstruction *instruction;
    int openPos;

    // Which kind of opening bracket this closing bracket must match
    switch (tokenState) {
        case 'M':
            type = PPL_STACK_LIST_BRACKET;
            mismatchError = "Could not match ] to an [.";
            break;
        case 'N':
            type = PPL_STACK_DICT_BRACE;
            mismatchError = "Could not match } to an {.";
            break;
        case 'Q':
            type = PPL_STACK_SUBSCRIPT_BRACKET;
            mismatchError = "Could not match ] to an [.";
            break;
        default:
            type = PPL_STACK_PARENTHESIS;
            mismatchError = "Could not match ) to an (.";
            break;
    }

    // An opening bracket is pushed onto the stack, as a barrier which stops operators outside it being popped
    if ((bracket == '(') || (bracket == '[') || (bracket == '{')) return pushStackItem(c, type, 0, 0, tpos) == NULL;

    // A closing bracket completes the expression(s) inside it
    if (popBracket(c, type, tpos, mismatchError, &openPos)) return 1;

    // The tokeniser stores the number of comma-separated items inside the brackets against the closing bracket
    switch (tokenState) {
        case 'D':
            // A list of values for the string substitution operator. Apply the operator now.
            if ((c->stackLen < 1) || (c->stack[c->stackLen - 1].type != PPL_STACK_OPERATOR) ||
                (c->stack[c->stackLen - 1].operatorCode != PPL_OP_STRING_SUBST)) {
                compileError(c, tpos, ERR_INTERNAL, "Could not match string substituion () to a %.");
                return 1;
            }
            c->stackLen--;
            if ((instruction = emitInstruction(c, PPL_BC_STRING_SUBST, openPos)) == NULL) return 1;
            instruction->operand.count = (int) token->depth;
            return 0;
        case 'P':
            if ((instruction = emitInstruction(c, PPL_BC_CALL, openPos)) == NULL) return 1;
            instruction->operand.count = (int) token->depth;
            return 0;
        case 'M':
            if ((instruction = emitInstruction(c, PPL_BC_MAKE_LIST, openPos)) == NULL) return 1;
            instruction->operand.count = (int) token->depth;
            return 0;
        case 'N':
            if ((instruction = emitInstruction(c, PPL_BC_MAKE_DICT, openPos)) == NULL) return 1;
            instruction->operand.count = (int) token->depth;
            return 0;
        case 'Q':
            // The tokeniser stores the form of the subscript ([x], [x:y], [x:], [:y] or [:]) as its opcode
            if ((instruction = emitInstruction(c, PPL_BC_SLICE, openPos)) == NULL) return 1;
            instruction->operand.sliceFlags = token->opcode;
            return 0;
        default:
            return 0; // A bracketed sub-expression needs no instruction of its own
    }
}

//! optimiseBranches - Where a conditional branch jumps to a goto, or to another branch which is certain to be taken,
//! make it jump straight to the final destination. For example, in a&&b&&c, if a is false, the branch after a can
//! jump straight to the end of the expression.
//! \param [in] expr - The compiled expression to optimise

static void optimiseBranches(pplExpr *expr) {
    pplExprInstruction *const bytecode = expr->bytecode;
    int changed;
    // Make branches which jump to another branch jump straight to its destination, where that is equivalent, repeating
    // until nothing changes
    do {
        changed = 0;
        // Examine each branch instruction
        for (int i = 0; i < expr->bcLen; i++) {
            pplExprInstruction *const branch = &bytecode[i];
            const pplExprInstruction *destination;
            if ((branch->opcode != PPL_BC_BRANCH_IF_FALSE) && (branch->opcode != PPL_BC_BRANCH_IF_TRUE)) continue;
            destination = &bytecode[branch->operand.target];
            // If a branch which leaves its value on the stack lands on an identical branch, that branch will test
            // the same value, so will also be taken
            if ((destination->opcode == PPL_BC_GOTO) ||
                ((destination->opcode == branch->opcode) && !branch->popOnBranch && !destination->popOnBranch)) {
                branch->operand.target = destination->operand.target;
                changed = 1;
            }
        }
    } while (changed);
}

//! ppl_expCompile - Compile an algebraic expression into bytecode, using the shunting-yard algorithm
//! \param [in] context - The Pyxplot context
//! \param [in] srcLineN - The line number of the source of the expression, used to report errors
//! \param [in] srcId - The ID of the source of the expression, used to report errors
//! \param [in] srcFname - The filename of the source of the expression, used to report errors
//! \param [in] in - The expression to compile
//! \param [out] end - Set to the number of characters of <in> which form part of the expression, or -1 on error
//! \param [in] dollarAllowed - If true, the $ operator may be used
//! \param [in] equalsAllowed - If true, assignment operators may be used
//! \param [in] allowCommaOperator - If true, the comma operator may be used
//! \param [out] outExpr - Set to point to the compiled expression, which is allocated with malloc(). This is set even
//! if an error occurs, in which case the caller should free it with pplExpr_free().
//! \param [out] errPos - Set to the position of any syntax error in <in>, or -1 if there was none
//! \param [out] errType - Set to the type of error, if an error occurred
//! \param [out] errText - Character buffer into which to write an error message, if an error occurred

void ppl_expCompile(ppl_context *context, const int srcLineN, const long srcId, char *srcFname, char *in, int *end,
                    const int dollarAllowed, const int equalsAllowed, const int allowCommaOperator, pplExpr **outExpr,
                    int *errPos, int *errType, char *errText) {
    pplExprCompiler c;
    const pplTokenCode *tdata;
    int tpos;
    int tlen;

    // Set up the compiler's state
    c.context = context;
    c.errPos = errPos;
    c.errType = errType;
    c.errText = errText;
    c.end = end;
    c.lastInstruction = -1;
    c.stackLen = 0;

    // Allocate the output structure
    pplExpr *expr = *outExpr = c.expr = (pplExpr *) calloc(1, sizeof(pplExpr));
    // Failure to allocate memory is reported as an error
    if (expr == NULL) {
        compileError(&c, 0, ERR_MEMORY, "Out of memory.");
        return;
    }
    expr->refCount = 1;
    expr->srcId = srcId;
    expr->srcLineN = srcLineN;
    c.bytecodeAlloc = 64;
    c.stringsAlloc = 256;
    expr->srcFname = (char *) malloc(strlen(srcFname) + 1);
    expr->bytecode = (pplExprInstruction *) malloc(c.bytecodeAlloc * sizeof(pplExprInstruction));
    expr->strings = (char *) malloc(c.stringsAlloc);
    // Failure to allocate memory is reported as an error
    if ((expr->bytecode == NULL) || (expr->srcFname == NULL) || (expr->strings == NULL)) {
        compileError(&c, 0, ERR_MEMORY, "Out of memory.");
        return;
    }
    strcpy(expr->srcFname, srcFname);

    // First tokenise the expression
    ppl_expTokenise(context, in, end, dollarAllowed, equalsAllowed, allowCommaOperator, 0, 0, 0, &tlen, errPos, errType,
                    errText);
    if (*errPos >= 0) return;
    tdata = context->tokenBuff;
    //ppl_tokenPrint(context, in, tlen);

    // No string in the expression can be longer than the expression itself
    int maxStringLen = (int) strlen(in);
    if (tlen > maxStringLen) maxStringLen = tlen;

    // Set up the operator stack. This is kept in the context for reuse, but if a previous expression made it very
    // large, start again with a smaller one.
    if (context->parserStackLen > 1024) {
        free(context->parserStack);
        context->parserStack = NULL;
        context->parserStackLen = 0;
    }
    // The compiler's operator stack is kept in the context, and reused between expressions
    if (context->parserStack == NULL) {
        context->parserStack = (pplExprStackItem *) malloc(1024 * sizeof(pplExprStackItem));
        // Failure to allocate memory is reported as an error
        if (context->parserStack == NULL) {
            compileError(&c, 0, ERR_MEMORY, "Out of memory");
            return;
        }
        context->parserStackLen = 1024;
    }
    c.stack = context->parserStack;

    // Copy the source text of the expression into the expression object
    expr->ascii = (char *) malloc(*end + 1);
    // Keep a copy of the text of the expression, for error messages
    if (expr->ascii == NULL) {
        compileError(&c, 0, ERR_MEMORY, "Out of memory.");
        return;
    }
    strncpy(expr->ascii, in, *end);
    expr->ascii[*end] = '\0';

    // The shunting-yard algorithm: read the tokens one by one
    for (tpos = 0; tpos < tlen;) {
        const char state = tdata[tpos].state; // The type of token
        int status = 0;

        // Compile each token, according to its type
        switch (state) {
            case 'B': // A string literal
                status = compileStringLiteral(&c, in, tdata, tlen, maxStringLen, &tpos);
                break;
            case 'C': // The string substitution operator %
            case 'H': // A prefix ++ or -- operator
            case 'I': // A unary operator
            case 'J': // A binary operator
            case 'K': // The ? or : of a ternary operator
            case 'S': // An assignment operator
                status = compileOperator(&c, state, &tdata[tpos], tpos);
                break;
            case 'D': // Brackets around the list of values after the string substitution operator %
            case 'E': // Brackets around a sub-expression
            case 'P': // Brackets around a list of function arguments
            case 'M': // Brackets around a list literal
            case 'N': // Braces around a dictionary literal
            case 'Q': // Brackets around a subscript
                status = compileBracket(&c, state, &tdata[tpos], in[tpos], tpos);
                break;
            case 'F': // A postfix ++ or -- operator, which applies straight away to the preceding variable
            {
                const int rightAssoc = (tdata[tpos].opcode & PPL_OP_RIGHT_ASSOC) != 0;
                status = popOperators(&c, tdata[tpos].precedence, rightAssoc);
                if (status) break;
                pplExprInstruction *instruction = emitInstruction(&c, PPL_BC_INCREMENT, tpos);
                if (instruction == NULL) status = 1;
                else instruction->operand.operatorCode = tdata[tpos].opcode;
                break;
            }
            case 'G': // A variable name
                status = compileName(&c, PPL_BC_LOOKUP, in, tdata, tlen, maxStringLen, &tpos);
                break;
            case 'T': // The name of a member, after the dot operator
                status = compileName(&c, PPL_BC_DEREFERENCE, in, tdata, tlen, maxStringLen, &tpos);
                break;
            case 'V': // The name of a column, after the $ operator
                status = compileName(&c, PPL_BC_PUSH_STRING, in, tdata, tlen, maxStringLen, &tpos);
                break;
            case 'L': // A numeric literal
            {
                pplExprInstruction *const instruction = emitInstruction(&c, PPL_BC_PUSH_NUMBER, tpos);
                if (instruction == NULL) status = 1;
                else instruction->operand.number = ppl_getFloat(in + tpos, NULL);
                break;
            }
            case 'O': // The $ operator. It acts on the column number or name which follows, so wait for that.
                status = pushStackItem(&c, PPL_STACK_DOLLAR, 0, 0, tpos) == NULL;
                break;
            case 'R': // The dot operator. Nothing to do: the work is done by the member name which follows.
            default:
                break;
        }
        if (status) return;

        // Skip to the next token. Some tokens are made of several characters which all have the same state.
        while ((tpos < tlen) && (tdata[tpos].state == state)) {
            tpos++;
            if (tpos >= tlen) break;
            // Brackets are single-character tokens, even when an opening bracket is followed directly by a closing
            // bracket of the same type, e.g. f() or []
            if (((state == 'D') || (state == 'E') || (state == 'P')) && ((in[tpos] == '(') || (in[tpos] == ')'))) break;
            if (((state == 'M') || (state == 'Q')) && ((in[tpos] == '[') || (in[tpos] == ']'))) break;
            if ((state == 'N') && ((in[tpos] == '{') || (in[tpos] == '}'))) break;
        }
    }

    // At the end of the expression, pop all remaining operators
    if (popOperators(&c, 255, 0)) return;

    // If there are still brackets on the stack, they were never closed
    if (c.stackLen != 0) {
        compileError(&c, 0, ERR_INTERNAL, "Unexpected junk left on stack.");
        return;
    }

    // Finish with a return instruction
    if (emitInstruction(&c, PPL_BC_RETURN, tpos) == NULL) return;

    // Make branches which land on other branches go straight to their final destination
    optimiseBranches(expr);

    // Release any unused space in the output arrays, since compiled expressions may be kept for a long time
    {
        pplExprInstruction *const bytecode =
                (pplExprInstruction *) realloc(expr->bytecode, expr->bcLen * sizeof(pplExprInstruction));
        char *const strings = (char *) realloc(expr->strings, (expr->stringsLen > 0) ? expr->stringsLen : 1);
        if (bytecode != NULL) expr->bytecode = bytecode;
        if (strings != NULL) expr->strings = strings;
    }

    //ppl_reversePolishPrint(context, expr);
    return;
}

//! operatorSymbol - Return the textual form of an operator code, for debugging
//! \param [in] code - The operator code (PPL_OP_*)
//! \return The operator, e.g. "+"

static const char *operatorSymbol(const int code) {
    // The symbol of each operator, for debugging output
    switch (code) {
        case PPL_OP_NEGATE:
            return "-";
        case PPL_OP_UNARY_PLUS:
            return "+";
        case PPL_OP_BITWISE_NOT:
            return "~";
        case PPL_OP_LOGICAL_NOT:
            return "!";
        case PPL_OP_POWER:
            return "**";
        case PPL_OP_MULTIPLY:
            return "*";
        case PPL_OP_DIVIDE:
            return "/";
        case PPL_OP_MODULO:
            return "%";
        case PPL_OP_ADD:
            return "+";
        case PPL_OP_SUBTRACT:
            return "-";
        case PPL_OP_SHIFT_LEFT:
            return "<<";
        case PPL_OP_SHIFT_RIGHT:
            return ">>";
        case PPL_OP_LESS:
            return "<";
        case PPL_OP_LESS_EQUAL:
            return "<=";
        case PPL_OP_GREATER_EQUAL:
            return ">=";
        case PPL_OP_GREATER:
            return ">";
        case PPL_OP_EQUAL:
            return "==";
        case PPL_OP_NOT_EQUAL:
            return "!=";
        case PPL_OP_BITWISE_AND:
            return "&";
        case PPL_OP_BITWISE_XOR:
            return "^";
        case PPL_OP_BITWISE_OR:
            return "|";
        case PPL_OP_LOGICAL_AND:
            return "&& (error: should have been replaced by an if)";
        case PPL_OP_LOGICAL_OR:
            return "|| (error: should have been replaced by an if)";
        case PPL_OP_COMMA:
            return "swap-pop";
        case PPL_OP_TERNARY_QUESTION:
            return "? -- error: should have been compiled into a conditional";
        case PPL_OP_TERNARY_COLON:
            return "?: -- error: should have been compiled into a conditional";
        case PPL_OP_COLLECT:
            return "nop (collect -- error: should have been optimised out)";
        default:
            return "??? (error: unknown opcode)";
    }
}

//! assignmentSymbol - Return the textual form of an assignment operator code, for debugging
//! \param [in] code - The assignment operator code (PPL_ASSIGN_*)
//! \return The operator, e.g. "+="

static const char *assignmentSymbol(const int code) {
    // The symbol of each assignment operator, for debugging output
    switch (code) {
        case PPL_ASSIGN:
            return "=";
        case PPL_ASSIGN_ADD:
            return "+=";
        case PPL_ASSIGN_SUBTRACT:
            return "-=";
        case PPL_ASSIGN_MULTIPLY:
            return "*=";
        case PPL_ASSIGN_DIVIDE:
            return "/=";
        case PPL_ASSIGN_MODULO:
            return "%=";
        case PPL_ASSIGN_BITWISE_AND:
            return "&=";
        case PPL_ASSIGN_BITWISE_XOR:
            return "^=";
        case PPL_ASSIGN_BITWISE_OR:
            return "|=";
        case PPL_ASSIGN_SHIFT_LEFT:
            return "<<=";
        case PPL_ASSIGN_SHIFT_RIGHT:
            return ">>=";
        case PPL_ASSIGN_POWER:
            return "**=";
        default:
            return "???";
    }
}

//! ppl_reversePolishPrint - Print a textual representation of the reverse-Polish bytecode of a compiled expression,
//! for debugging
//! \param [in] context - The Pyxplot context
//! \param [in] expIn - The compiled expression to print

void ppl_reversePolishPrint(ppl_context *context, const pplExpr *expIn) {
    // Describe each instruction on a line of its own
    char msg[PPL_MSG_LENGTH];

    // Describe each instruction
    for (int j = 0; j < expIn->bcLen; j++) {
        const pplExprInstruction *const in = &expIn->bytecode[j];
        const char *string = expIn->strings + in->operand.string; // Only meaningful for instructions with strings
        const char *op = "???", *optype = "";
        char arg[1024] = "";
        // Name the instruction, and describe its operand
        switch (in->opcode) {
            case PPL_BC_RETURN:
                op = "return";
                break;
            case PPL_BC_PUSH_NUMBER:
                op = "push";
                optype = "numeric";
                ppl_snprintfTrunc(arg, sizeof(arg), "%.2e", in->operand.number);
                break;
            case PPL_BC_PUSH_STRING:
                op = "push";
                optype = "string";
                snprintf(arg, sizeof(arg), "\"%s\"", string);
                break;
            case PPL_BC_LOOKUP:
            case PPL_BC_LOOKUP_POINTER:
                op = "lookup";
                optype = (in->opcode == PPL_BC_LOOKUP) ? "value" : "pointer";
                snprintf(arg, sizeof(arg), "\"%s\"", string);
                break;
            case PPL_BC_DEREFERENCE:
            case PPL_BC_DEREFERENCE_POINTER:
                op = "deref";
                optype = (in->opcode == PPL_BC_DEREFERENCE) ? "value" : "pointer";
                snprintf(arg, sizeof(arg), "\"%s\"", string);
                break;
            case PPL_BC_SLICE: {
                const int f = in->operand.sliceFlags;
                const int range = f & (PPL_SLICE_MIN_SET | PPL_SLICE_MAX_SET);
                op = "slice";
                optype = "value";
                if (f & PPL_SLICE_SINGLE_ITEM) strcpy(arg, "[--]");
                else if (range == (PPL_SLICE_MIN_SET | PPL_SLICE_MAX_SET)) strcpy(arg, "[--:--]");
                else if (range == PPL_SLICE_MIN_SET) strcpy(arg, "[--:]");
                else if (range == PPL_SLICE_MAX_SET) strcpy(arg, "[:--]");
                else strcpy(arg, "[:]");
                break;
            }
            case PPL_BC_SLICE_POINTER:
                op = "slice";
                optype = "pointer";
                strcpy(arg, "[--]");
                break;
            case PPL_BC_MAKE_DICT:
            case PPL_BC_MAKE_LIST:
                op = "make";
                optype = (in->opcode == PPL_BC_MAKE_DICT) ? "dict" : "list";
                sprintf(arg, "%d items", in->operand.count);
                break;
            case PPL_BC_CALL:
                op = "call";
                ppl_snprintfTrunc(arg, sizeof(arg), "%d args", in->operand.count);
                break;
            case PPL_BC_OPERATOR: {
                const int arity = (in->operand.operatorCode >> 5) & 3;
                op = "op";
                optype = (arity == 1) ? "unary" : (arity == 2) ? "binary" : (arity == 3) ? "ternary" : "???";
                ppl_snprintfTrunc(arg, sizeof(arg), "%s", operatorSymbol(in->operand.operatorCode));
                break;
            }
            case PPL_BC_ASSIGN:
                op = "op";
                optype = "assign";
                strcpy(arg, assignmentSymbol(in->operand.operatorCode));
                break;
            case PPL_BC_INCREMENT: {
                const int t = in->operand.operatorCode;
                op = "op";
                optype = "unary";
                if (t == PPL_OP_POST_DECREMENT) strcpy(arg, "-- (post-eval)");
                else if (t == PPL_OP_POST_INCREMENT) strcpy(arg, "++ (post-eval)");
                else if (t == PPL_OP_PRE_DECREMENT) strcpy(arg, "-- (pre-eval)");
                else strcpy(arg, "++ (pre-eval)");
                break;
            }
            case PPL_BC_STRING_SUBST:
                op = "op";
                optype = "binary";
                ppl_snprintfTrunc(arg, sizeof(arg), "string subst (%d items)", in->operand.count);
                break;
            case PPL_BC_DOLLAR:
                op = "op";
                optype = "unary";
                strcpy(arg, "dollar -- column lookup");
                break;
            case PPL_BC_BRANCH_IF_FALSE:
            case PPL_BC_BRANCH_IF_TRUE: {
                const int ifTrue = (in->opcode == PPL_BC_BRANCH_IF_TRUE);
                op = "branch if";
                optype = ifTrue ? "true" : "false";
                ppl_snprintfTrunc(arg, sizeof(arg), "to %d (%s)", in->operand.target,
                                  in->popOnBranch  ? "pop conditional"
                                          : ifTrue ? "pop conditional; push TRUE on branch"
                                                   : "pop conditional; push FALSE on branch");
                break;
            }
            case PPL_BC_GOTO:
                op = "branch";
                optype = "goto";
                ppl_snprintfTrunc(arg, sizeof(arg), "%d", in->operand.target);
                break;
            case PPL_BC_MAKE_BOOLEAN:
                op = "make";
                optype = "boolean";
                break;
            default:
                strcpy(arg, "Illegal opcode");
                break;
        }
        ppl_snprintfTrunc(msg, sizeof(msg), "%4d %4d %10s %10s %s", j, in->charpos, op, optype, arg);
        ppl_report(&context->errcontext, msg);
    }
}

//! pplExpr_free - Release a reference to a compiled expression, freeing it when its reference count reaches zero
//! \param [in] inExpr - The compiled expression (may be NULL)

void pplExpr_free(pplExpr *inExpr) {
    if (inExpr == NULL) return;
    if (__sync_sub_and_fetch(&inExpr->refCount, 1) > 0) return;
    // When the last reference has gone, free the text, filename, bytecode and strings
    free(inExpr->ascii);
    inExpr->ascii = NULL;
    // The filename
    free(inExpr->srcFname);
    inExpr->srcFname = NULL;
    // The bytecode
    free(inExpr->bytecode);
    inExpr->bytecode = NULL;
    // The string table
    free(inExpr->strings);
    inExpr->strings = NULL;
    free(inExpr);
    inExpr = NULL;
    return;
}

//! pplExpr_cpyWith - Make a deep copy of a compiled expression, using a given memory allocator
//! \param [in] i - The compiled expression to copy (may be NULL)
//! \param [in] alloc - The memory allocator to use
//! \return The copy, or NULL if <i> is NULL or memory allocation failed

static pplExpr *pplExpr_cpyWith(const pplExpr *i, void *(*alloc)(size_t) ) {
    // Copy the structure, and then each of the malloced members it points to
    if (i == NULL) return NULL;
    pplExpr *o = (pplExpr *) alloc(sizeof(pplExpr));
    if (o == NULL) return NULL;
    memcpy(o, i, sizeof(pplExpr));
    o->refCount = 1;
    // The text
    if (i->ascii != NULL) {
        if ((o->ascii = alloc(strlen(i->ascii) + 1)) == NULL) return NULL;
        strcpy(o->ascii, i->ascii);
    }
    // The filename
    if (i->srcFname != NULL) {
        if ((o->srcFname = alloc(strlen(i->srcFname) + 1)) == NULL) return NULL;
        strcpy(o->srcFname, i->srcFname);
    }
    // The bytecode
    if (i->bytecode != NULL) {
        const size_t size = (i->bcLen > 0 ? i->bcLen : 1) * sizeof(pplExprInstruction);
        if ((o->bytecode = alloc(size)) == NULL) return NULL;
        memcpy(o->bytecode, i->bytecode, i->bcLen * sizeof(pplExprInstruction));
    }
    // The string table
    if (i->strings != NULL) {
        if ((o->strings = alloc(i->stringsLen > 0 ? i->stringsLen : 1)) == NULL) return NULL;
        memcpy(o->strings, i->strings, i->stringsLen);
    }
    return o;
}

//! pplExpr_cpy - Make a deep copy of a compiled expression, allocated with malloc()
//! \param [in] i - The compiled expression to copy (may be NULL)
//! \return The copy, or NULL if <i> is NULL or memory allocation failed

pplExpr *pplExpr_cpy(const pplExpr *i) {
    return pplExpr_cpyWith(i, malloc);
}

//! tmpAlloc - Allocate memory in the current ppl_memAlloc context, with the same signature as malloc()
//! \param [in] size - The number of bytes to allocate
//! \return Pointer to the allocated memory, or NULL on failure

static void *tmpAlloc(const size_t size) {
    return ppl_memAlloc((int) size);
}

//! pplExpr_tmpcpy - Make a temporary deep copy of a compiled expression, allocated in the current ppl_memAlloc
//! context. It is freed when that context is freed, and must not be passed to pplExpr_free().
//! \param [in] i - The compiled expression to copy (may be NULL)
//! \return The copy, or NULL if <i> is NULL or memory allocation failed

pplExpr *pplExpr_tmpcpy(const pplExpr *i) {
    return pplExpr_cpyWith(i, tmpAlloc);
}
