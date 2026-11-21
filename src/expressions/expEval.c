// expEval.c
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

// The expression evaluator: a stack machine which executes the bytecode produced by expCompile.c. Each instruction
// pushes values (numbers, strings, variables), applies operators to the values on top of the stack, looks up members
// and methods, slices, calls functions, or jumps (for the short-circuit operators and, or and ?:). It also provides the
// casts between types which operators need, and the string substitution operator %. Every expression in Pyxplot is
// evaluated here.

#define _EXPEVAL_C 1

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#include <gsl/gsl_math.h>

#include "coreUtils/dict.h"
#include "coreUtils/errorReport.h"
#include "coreUtils/list.h"
#include "expressions/dollarOp.h"
#include "expressions/expEval.h"
#include "expressions/expEvalCalculus.h"
#include "expressions/expEvalOps.h"
#include "expressions/expEvalSlice.h"
#include "expressions/fnCall.h"
#include "expressions/traceback_fns.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "userspace/context.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"
#include "userspace/pplObjCmp.h"
#include "userspace/pplObjFunc.h"
#include "userspace/pplObjMethods.h"
#include "userspace/pplObjPrint.h"
#include "userspace/stack.h"
#include "userspace/unitsArithmetic.h"
#include "userspace/unitsDisp.h"

#include "datafile.h"
#include "pplConstants.h"


//! ppl_castToNumber - Convert an object into a number, in place, if possible. Booleans become 0 or 1, and strings
//! are parsed as numbers.
//! \param [in] context - The Pyxplot context
//! \param [in,out] x - The object to convert, which is overwritten with the number
//! \param [out] errText - Character buffer into which to write an error message, if the conversion fails
//! \param [in] errLen - The size of the buffer <errText>
//! \return Zero on success, or ERR_TYPE if the object could not be converted

int ppl_castToNumber(ppl_context *context, pplObj *x, char *errText, const size_t errLen) {
    // Booleans become 0 or 1, and strings are parsed as numbers; other types cannot be cast
    const int t = x->objType;
    const int rc = x->refCount; // The reference count is kept, since the object stays where it is (e.g. on the stack)
    double d = 0;
    if (t == PPLOBJ_NUM) return 0;
    // Cast according to the type of the object
    switch (t) {
        case PPLOBJ_BOOL:
            d = (x->real != 0);
            break;
        case PPLOBJ_STR: {
            int len;
            const char *str = (char *) x->auxil;
            d = ppl_getFloat(str, &len);
            if (len > 0) {
                while ((str[len] > '\0') && ((unsigned char) str[len] <= ' ')) len++; // Allow trailing whitespace
            }
            // The whole string must be a valid number
            if ((len < 0) || (str[len] != '\0')) {
                ppl_snprintfTrunc(errText, errLen,
                                  "Attempt to implicitly cast string to number failed: string is not a valid number.");
                return ERR_TYPE;
            }
            break;
        }
        default:
            ppl_snprintfTrunc(errText, errLen, "Cannot implicitly cast an object of type <%s> to a number.",
                              pplObjTypeNames[t]);
            return ERR_TYPE;
    }
    ppl_garbageObject(x);
    pplObjNum(x, 0, d, 0);
    x->refCount = rc;
    return 0;
}

//! ppl_castToNum - Convert an object on the evaluation stack into a number, in place, if possible, as
//! ppl_castToNumber() does. If the conversion fails, an error is added to the traceback.
//! \param [in] context - The Pyxplot context
//! \param [in,out] x - The object to convert, which is overwritten with the number
//! \param [in] expr - The expression being evaluated, used to report errors
//! \param [in] charpos - The position in <expr> of the operator or function being evaluated
//! \return Zero on success, or nonzero if the object could not be converted

int ppl_castToNum(ppl_context *context, pplObj *x, const pplExpr *expr, const int charpos) {
    const int errType = ppl_castToNumber(context, x, context->errStat.errBuff, sizeof(context->errStat.errBuff));
    if (errType == 0) return 0;
    ppl_tbAddExpr(context, expr, errType, charpos, "");
    return 1;
}

//! ppl_castToInt - Convert an object on the evaluation stack into a number, in place, and check that it is a real,
//! dimensionless value within the range of an int, as required by integer operators such as & and <<. If not, an
//! error is added to the traceback.
//! \param [in] context - The Pyxplot context
//! \param [in,out] x - The object to convert, which is overwritten with the number
//! \param [in] op - The name of the operator, used in error messages
//! \param [in] expr - The expression being evaluated, used to report errors
//! \param [in] charpos - The position in <expr> of the operator being evaluated
//! \return Zero on success, or nonzero if the object is not a suitable integer

int ppl_castToInt(ppl_context *context, pplObj *x, const char *op, const pplExpr *expr, const int charpos) {
    if (ppl_castToNum(context, x, expr, charpos)) return 1;
    // Integer operators need real, dimensionless numbers which fit in an int
    if (x->flagComplex) {
        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                          "The %s operator can only act on real numbers.", op);
        ppl_tbAddExpr(context, expr, ERR_RANGE, charpos, "");
        return 1;
    }
    // The number must be dimensionless
    if (!x->dimensionless) {
        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                          "The %s operator is an integer operator which can only act on dimensionless numbers: "
                          "supplied operand has units of <%s>.",
                          op, ppl_printUnit(context, x, NULL, NULL, 0, 1, 0));
        ppl_tbAddExpr(context, expr, ERR_UNIT, charpos, "");
        return 1;
    }
    // The number must fit in an int
    if ((x->real < INT_MIN) || (x->real > INT_MAX)) {
        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                          "The %s operator can only act on integers in the range %d to %d.", op, INT_MIN, INT_MAX);
        ppl_tbAddExpr(context, expr, ERR_RANGE, charpos, "");
        return 1;
    }
    return 0;
}

//! ppl_castToBool - Convert an object on the evaluation stack into a boolean, in place. Nonzero finite numbers,
//! non-empty strings, collections and open files are true; null, zombies and exceptions are false; other objects
//! are true.
//! \param [in] context - The Pyxplot context
//! \param [in,out] x - The object to convert, which is overwritten with the boolean

void ppl_castToBool(ppl_context *context, pplObj *x) {
    // Numbers are true unless zero, strings unless empty, and so on
    const int t = x->objType;
    const int rc = x->refCount; // The reference count is kept, since the object stays on the stack
    int s;
    if (t == PPLOBJ_BOOL) return;
    // Cast according to the type of the object
    switch (t) {
        case PPLOBJ_NUM:
            s = ((x->real != 0) || (x->imag != 0)) && gsl_finite(x->real) && gsl_finite(x->imag) &&
                    ((x->imag == 0) || (context->set->term_current.ComplexNumbers == SW_ONOFF_ON));
            break;
        case PPLOBJ_STR:
            s = ((char *) x->auxil)[0] != '\0';
            break;
        case PPLOBJ_ZOM:
        case PPLOBJ_EXC:
        case PPLOBJ_NULL:
            s = 0;
            break;
        case PPLOBJ_DICT:
            s = (((dict *) x->auxil)->length != 0);
            break;
        case PPLOBJ_LIST:
            s = (((list *) x->auxil)->length != 0);
            break;
        case PPLOBJ_VEC:
            s = (((pplVector *) x->auxil)->v->size != 0);
            break;
        case PPLOBJ_MAT:
            s = (((pplMatrix *) x->auxil)->m->size1 != 0) || (((pplMatrix *) x->auxil)->m->size2 != 0);
            break;
        case PPLOBJ_FILE:
            s = (((pplFile *) x->auxil)->open != 0);
            break;
        default:
            s = 1;
            break;
    }
    ppl_garbageObject(x);
    pplObjBool(x, 0, s);
    x->refCount = rc;
}

//! expEval_stringSubs - Evaluate the % operator applied to a string, substituting values into printf-style format
//! tokens. The format string and the values to substitute are taken from the top of the stack, and replaced by the
//! resulting string.
//! \param [in] context - The Pyxplot context
//! \param [in] inExpr - The expression being evaluated
//! \param [in] charpos - The position of the % operator within the expression, used to report errors
//! \param [in] Nsubs - The number of values to substitute into the format string

static void expEval_stringSubs(ppl_context *context, const pplExpr *inExpr, const int charpos, int Nsubs) {
    const char allowedFormats[] = "cdieEfgGosSxX%"; // These tokens are allowed after a % format specifier
    char formatToken[512];
    pplObj *obj = &context->stack[context->stackPtr];
    int argP = -Nsubs;
    char *format = (char *) ((obj + argP - 1)->auxil);
    int outlen = 65536;
    int inP = 0;
    int outP = 0;
    int inP2, requiredArgs, l, arg1i = 0, arg2i = 0;
    char *out = (char *) malloc(outlen);

    // Failure to allocate memory is reported as an error
    if (out == NULL) {
        strcpy(context->errStat.errBuff, "Out of memory.");
        ppl_tbAddExpr(context, inExpr, ERR_MEMORY, charpos, "");
        return;
    }

    // Loop over format string looking for tokens
    for (; format[inP] != '\0'; inP++) {
        // If output buffer is nearly full, malloc some more
        if (outP > outlen - 16384) {
            char *outnew = (char *) realloc((void *) out, outlen += 65536);
            // Failure to allocate memory is reported as an error
            if (outnew == NULL) {
                free(out);
                out = NULL;
                strcpy(context->errStat.errBuff, "Out of memory.");
                ppl_tbAddExpr(context, inExpr, ERR_MEMORY, charpos, "");
                return;
            }
            out = outnew;
        }

        // Parse each %-token of the format string, as printf() does: flags, width, precision and conversion character.
        // The width and precision may be *, which takes an extra integer argument.
        if (format[inP] != '%') {
            out[outP++] = format[inP];
            continue;
        } // Characters other than % tokens are copied straight to output
        inP2 = inP + 1; // inP2 looks ahead to see experimentally if syntax is right
        requiredArgs = 1; // Normal %f like tokens require 1 argument
        if ((format[inP2] == '+') || (format[inP2] == '-') || (format[inP2] == ' ') || (format[inP2] == '#'))
            inP2++; // optional flag can be <+- #>
        // The width may be * or a number
        if (format[inP2] == '*') {
            inP2++;
            requiredArgs++;
        } else
            while ((format[inP2] >= '0') && (format[inP2] <= '9')) inP2++; // length can be * or some digits
        if (format[inP2] == '.') // precision starts with a . and is followed by * or by digits
        {
            inP2++;
            // The precision may be * or a number
            if (format[inP2] == '*') {
                inP2++;
                requiredArgs++;
            } else
                while ((format[inP2] >= '0') && (format[inP2] <= '9')) inP2++; // length can be * or some digits
        }

        // We do not allow user to specify optional length flag, which could potentially be <hlL>

        // The conversion character must be one which printf() accepts; %% takes no argument
        for (l = 0; allowedFormats[l] != '\0'; l++)
            if (format[inP2] == allowedFormats[l]) break;
        if (allowedFormats[l] == '%') requiredArgs = 0;
        // Text which is not a valid %-token is copied as it is
        if ((allowedFormats[l] == '\0') || ((allowedFormats[l] == '%') && (format[inP2 - 1] != '%'))) {
            out[outP++] = format[inP];
            continue;
        } // Have not got correct syntax for a format specifier
        // There must be enough arguments left for the token
        if (requiredArgs > -argP) {
            free(out);
            out = NULL;
            strcpy(context->errStat.errBuff, "Too few arguments supplied to string substitution operator");
            ppl_tbAddExpr(context, inExpr, ERR_RANGE, charpos, "");
            return;
        } // Have run out of substitution arguments

        // %% token simply produces a literal %
        if (allowedFormats[l] == '%') {
            strcpy(out + outP, "%"); // %% just produces a % character
        } else {
            ppl_strSlice(format, formatToken, inP, inP2 + 1);

            // If token required extra integer arguments, read these now
            if (requiredArgs > 1) {
                if (ppl_castToInt(context, obj + argP, "%", inExpr, charpos)) goto cast_fail;
                arg1i = (int) ((obj + argP)->real);
                argP++;
            }
            // A second * argument, for the precision
            if (requiredArgs > 2) {
                if (ppl_castToInt(context, obj + argP, "%", inExpr, charpos)) goto cast_fail;
                arg2i = (int) ((obj + argP)->real);
                argP++;
            }

            // Print a string
            if ((allowedFormats[l] == 'c') || (allowedFormats[l] == 's') || (allowedFormats[l] == 'S')) {
                const int tmpbufflen = 65536;
                char *tmpbuff = (char *) malloc(tmpbufflen);
                // Failure to allocate memory is reported as an error
                if (tmpbuff == NULL) {
                    free(out);
                    out = NULL;
                    strcpy(context->errStat.errBuff, "Out of memory.");
                    ppl_tbAddExpr(context, inExpr, ERR_MEMORY, charpos, "");
                    return;
                }
                pplObjPrint(context, obj + argP, NULL, tmpbuff, tmpbufflen, 0, 0);
                formatToken[inP2 - inP] = 's';
                if (requiredArgs == 1)
                    snprintf(out + outP, outlen - outP, formatToken, tmpbuff); // Print a string variable
                if (requiredArgs == 2) snprintf(out + outP, outlen - outP, formatToken, arg1i, tmpbuff);
                if (requiredArgs == 3) snprintf(out + outP, outlen - outP, formatToken, arg1i, arg2i, tmpbuff);
                out[outlen - 1] = '\0';
                free(tmpbuff);
                tmpbuff = NULL;
            } else {
                pplObj *o = obj + argP;
                if (ppl_castToNum(context, obj + argP, inExpr, charpos)) goto cast_fail; // Make object into a number
                if ((!gsl_finite(o->real)) ||
                    ((context->set->term_current.ComplexNumbers == SW_ONOFF_OFF) && (o->flagComplex != 0))) {
                    strcpy(out + outP, "nan");
                } else {
                    strcpy(out + outP,
                           ppl_unitsNumericDisplayWithFormat(context, o, 0, formatToken, allowedFormats[l],
                                                             outlen - outP, requiredArgs, arg1i, arg2i));
                }
            }
            argP++;
        }
        outP += strlen(out + outP);
        inP = inP2;
    }
    out[outP] = '\0';

    // Throw an error if too many arguments were supplied
    if (argP < 0) {
        free(out);
        out = NULL;
        ppl_snprintfTrunc(
                context->errStat.errBuff, sizeof(context->errStat.errBuff),
                "Too many arguments supplied to string substitution operator; %d supplied when %d were needed.", Nsubs,
                Nsubs + argP);
        ppl_tbAddExpr(context, inExpr, ERR_RANGE, charpos, "");
        return;
    }

    // Return output string
    pplObjStr(obj, 0, 1, out);
    context->stackPtr++; // Push the substituted string
    return;

// An argument could not be cast to the type the token needs
cast_fail:
    // Free the output buffer
    free(out);
    out = NULL;
    return;
}


//! assignToVariable - Complete the evaluation of an assignment operator, such as = or +=, by storing the value
//! assigned in the variable (or vector or matrix element) being assigned to. The stack holds [... o in tmp], where
//! <o> is the variable, <in> is the right-hand side of the operator, and <tmp> is the value to be assigned.
//! On success, <tmp> is moved into the stack slot of <o>, where it remains as the result of the operator.
//! \param [in] context - The Pyxplot context
//! \param [in,out] o - The stack slot holding the variable being assigned to; overwritten with the assigned value
//! \param [in] tmp - The stack slot holding the value to assign
//! \param [in] rhsType - The object type of the right-hand side of the operator
//! \param [in] inExpr - The expression being evaluated, used in error messages
//! \param [in] charpos - The position of the operator in the expression, used in error messages
//! \return Zero on success, or nonzero on failure (in which case an error has been added to the traceback)

static int assignToVariable(ppl_context *context, pplObj *o, pplObj *tmp, const int rhsType, const pplExpr *inExpr,
                            const int charpos) {
    tmp->refCount = 1;
    // Assign either to a variable, or to an element of a vector or matrix (which self_dval points to)
    if (o->self_dval == NULL) {
        // Assign to a variable, preserving its memory-management flags
        ppl_objReplace(o->self_lval, tmp);
    } else {
        // Assign to a vector or matrix element
        if ((rhsType != PPLOBJ_NUM) || (tmp->flagComplex)) {
            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                              "Vectors and matrices can only contain real numbers.");
            ppl_tbAddExpr(context, inExpr, ERR_TYPE, charpos, "");
            return 1;
        }
        // Elements of vectors and matrices must all have the same units
        if (!ppl_unitsDimEqual(o, tmp)) {
            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                              "Cannot insert element with dimensions <%s> into vector with dimensions <%s>.",
                              ppl_printUnit(context, tmp, NULL, NULL, 0, 1, 0),
                              ppl_printUnit(context, o, NULL, NULL, 1, 1, 0));
            ppl_tbAddExpr(context, inExpr, ERR_UNIT, charpos, "");
            return 1;
        }
        *o->self_dval = tmp->real;
    }
    ppl_garbageObject(o);
    memcpy(o, tmp, sizeof(pplObj)); // Stack: move the assigned value <tmp> down into the slot of the variable <o>...
    ppl_stackPop(context); // ... and pop the top slot, which held <tmp>, leaving [... value in]
    return 0;
}

//! ppl_expEval - Evaluate a compiled expression
//! \param [in] context - The Pyxplot context
//! \param [in] inExpr - The compiled expression to evaluate
//! \param [out] lastOpAssign - Set to true if the last operation in the expression was an assignment, in which case
//! its value is not normally displayed
//! \param [in] dollarAllowed - If true, the $ operator may be used
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return Pointer to the result on the stack, or NULL if an error occurred, in which case <context->errStat>
//! describes the error

pplObj *ppl_expEval(ppl_context *context, pplExpr *inExpr, int *lastOpAssign, const int dollarAllowed,
                    const int iterDepth) {
    int j = 0;
    int initialStackPtr = 0;
    int charpos = 0;
    const pplExprInstruction *const bytecode = inExpr->bytecode;

    // Guard against runaway recursion, e.g. a function which calls itself
    if (iterDepth > MAX_RECURSION_DEPTH) {
        strcpy(context->errStat.errBuff, "Maximum recursion depth exceeded.");
        ppl_tbAddExpr(context, inExpr, ERR_OVERFLOW, charpos, "");
        return NULL;
    }

    // If at bottom iteration depth, clean up stack now if there is any left-over junk
    if (iterDepth == 0) { ppl_stackReinit(context); }
    initialStackPtr = context->stackPtr;
    *lastOpAssign = 0;

    // Execute instructions until a RETURN. j is the index of the current instruction; stk points to the slot above the
    // top of the stack.
    while (1) {
        const pplExprInstruction *const instruction = &bytecode[j]; // The instruction to execute
        const pplExprOpcode opcode = instruction->opcode;
        int next = j + 1; // The index of the next instruction to execute; changed by instructions which jump
        charpos = instruction->charpos; // Position of the token in the source text, for error reporting

        // Stop if the user has pressed Ctrl-C
        if (cancellationFlag) {
            strcpy(context->errStat.errBuff, "Operation cancelled.");
            charpos = 0;
            ppl_tbAddExpr(context, inExpr, ERR_INTERRUPT, charpos, "");
            goto cleanup_on_error;
        }
        if (initialStackPtr == 0) {
            ppl_stackRequest(context, 4);
        } // allowed to realloc stack
        else {
            ppl_stackMustHave(context, 4);
        } // not allowed to realloc stack; will change pointers in calling layers
        // The stack is full, and cannot be grown here
        if (context->stackFull) {
            strcpy(context->errStat.errBuff, "Stack overflow.");
            charpos = 0;
            ppl_tbAddExpr(context, inExpr, ERR_MEMORY, charpos, "");
            goto cleanup_on_error;
        }
        pplObj *stk = &context->stack[context->stackPtr];

        // Execute the instruction
        switch (opcode) {
            case PPL_BC_RETURN:
                break;
            case PPL_BC_PUSH_NUMBER:
                *lastOpAssign = 0;
                pplObjNum(stk, 0, instruction->operand.number, 0);
                stk->refCount = 1;
                context->stackPtr++; // Push the number
                break;
            case PPL_BC_PUSH_STRING: {
                const char *const string = inExpr->strings + instruction->operand.string;
                const int l = strlen(string);
                char *out;
                *lastOpAssign = 0;
                if ((out = (char *) malloc(l + 1)) == NULL) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff), "Out of memory.");
                    ppl_tbAddExpr(context, inExpr, ERR_MEMORY, charpos, "");
                    goto cleanup_on_error;
                }
                strcpy(out, string);
                pplObjStr(stk, 0, 1, out);
                stk->refCount = 1;
                context->stackPtr++; // Push the string
                break;
            }
            case PPL_BC_LOOKUP: {
                int i, got = 0;
                char *key = inExpr->strings + instruction->operand.string;
                *lastOpAssign = 0;
                for (i = context->ns_ptr; i >= 0; i = (i > 1) ? 1 : i - 1) {
                    pplObj *obj = (pplObj *) ppl_dictLookup(context->namespaces[i], key);
                    if (obj == NULL) continue;
                    if ((obj->objType == PPLOBJ_GLOB) || (obj->objType == PPLOBJ_ZOM)) continue;
                    pplObjCpy(stk, obj, 1, 0, 1);
                    stk->immutable = stk->immutable || context->namespaces[i]->immutable;
                    stk->refCount = 1;
                    context->stackPtr++; // Push the value of the variable
                    got = 1;
                    break;
                }
                // The variable was not found in any namespace
                if (!got) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "No such variable '%s'.", key);
                    ppl_tbAddExpr(context, inExpr, ERR_NAMESPACE, charpos, "");
                    goto cleanup_on_error;
                }
                break;
            }
            case PPL_BC_LOOKUP_POINTER: {
                int i;
                char *key = inExpr->strings + instruction->operand.string;
                *lastOpAssign = 0;
                for (i = context->ns_ptr;; i = 1) {
                    pplObj *obj = (pplObj *) ppl_dictLookup(context->namespaces[i], key);
                    // Look up the variable in the local namespace, creating it if it does not exist, so that it can be
                    // assigned to. A variable declared global sends us on to the global namespace (i = 1).
                    if ((obj == NULL) && (!context->namespaces[i]->immutable)) {
                        pplObjZom(stk, 1); // Create a temporary zombie for now
                        stk->refCount = 1;
                        ppl_dictAppendCpy(context->namespaces[i], key, stk, sizeof(pplObj));
                        stk->amMalloced = 0;
                        obj = (pplObj *) ppl_dictLookup(context->namespaces[i], key);
                        // Failure to allocate memory is reported as an error
                        if (obj == NULL) {
                            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                              "Out of memory.");
                            ppl_tbAddExpr(context, inExpr, ERR_MEMORY, charpos, "");
                            goto cleanup_on_error;
                        }
                    }
                    // Variables in immutable namespaces cannot be assigned to
                    if ((obj == NULL) || ((context->namespaces[i]->immutable) && (obj->objType != PPLOBJ_GLOB))) {
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                          "Cannot modify variable in immutable namespace.");
                        ppl_tbAddExpr(context, inExpr, ERR_NAMESPACE, charpos, "");
                        goto cleanup_on_error;
                    }
                    // A global declaration: look in the global namespace instead
                    if (obj->objType == PPLOBJ_GLOB) {
                        // A global declaration in the global namespace itself would loop forever
                        if (i < 2) {
                            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                              "Variable declared global in global namespace.");
                            ppl_tbAddExpr(context, inExpr, ERR_NAMESPACE, charpos, "");
                            goto cleanup_on_error;
                        }
                        continue;
                    }
                    pplObjCpy(stk, obj, 1, 0, 1);
                    stk->refCount = 1;
                    context->stackPtr++; // Push a pointer to the variable
                    break;
                }
                break;
            }
            case PPL_BC_DEREFERENCE: {
                char *key = inExpr->strings + instruction->operand.string;
                pplObj *in = stk - 1;
                pplObj *in_cpy = NULL;
                pplObj *iter = in;
                int t = iter->objType;
                int imm = iter->immutable;
                int found = 0;
                *lastOpAssign = 0;
                // Make copy of object that will be self_this (in, which we therefore don't garbage below)
                in_cpy = (pplObj *) malloc(sizeof(pplObj));
                // Failure to allocate memory is reported as an error
                if (in_cpy == NULL) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff), "Out of memory.");
                    ppl_tbAddExpr(context, inExpr, ERR_MEMORY, charpos, "");
                    goto cleanup_on_error;
                }
                memcpy(in_cpy, in, sizeof(pplObj));
                in_cpy->refCount = 1;
                in_cpy->amMalloced = 1;
                // Loop through module / class instance and its prototypes looking for named method
                for (; (t == PPLOBJ_MOD) || (t == PPLOBJ_USER); iter = iter->objPrototype, t = iter->objType) {
                    dict *d = (dict *) (iter->auxil);
                    pplObj *obj = (pplObj *) ppl_dictLookup(d, key);
                    imm = imm || iter->immutable || d->immutable;
                    // Members not found in this object are looked for in its prototype. Members inherited from a
                    // prototype are immutable, so that they are not changed through an instance.
                    if ((obj == NULL) || (obj->objType == PPLOBJ_ZOM) || (obj->objType == PPLOBJ_GLOB)) {
                        imm = 1;
                        continue;
                    } // Named methods of prototypes are immutable
                    pplObjCpy(stk - 1, obj, 1, 0, 1);
                    (stk - 1)->immutable = (stk - 1)->immutable || imm;
                    (stk - 1)->refCount = 1;
                    found = 1;
                    break;
                }
                if (!found) // If module or class instance doesn't have named method, see if type has named method
                {
                    dict *d = pplObjMethods[in->objType];
                    pplObj *obj = (pplObj *) ppl_dictLookup(d, key);
                    // Neither the object nor its type has a member with this name
                    if (obj == NULL) {
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                          "No such method '%s'.", key);
                        ppl_tbAddExpr(context, inExpr, ERR_NAMESPACE, charpos, "");
                        free(in_cpy);
                        in_cpy = NULL;
                        goto cleanup_on_error;
                    }
                    pplObjCpy(stk - 1, obj, 1, 0, 1);
                    (stk - 1)->immutable = 1;
                    (stk - 1)->refCount = 1;
                }
                in->self_this = in_cpy;
                break;
            }
            case PPL_BC_DEREFERENCE_POINTER: {
                char *key = inExpr->strings + instruction->operand.string;
                const int t = (stk - 1)->objType;
                dict *d = (dict *) ((stk - 1)->auxil);
                *lastOpAssign = 0;
                if ((stk - 1)->immutable) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "Cannot modify variable in immutable namespace.");
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, charpos, "");
                    goto cleanup_on_error;
                }
                // Only members of modules and class instances can be assigned to
                if ((t == PPLOBJ_MOD) || (t == PPLOBJ_USER)) {
                    pplObj *obj = (pplObj *) ppl_dictLookup(d, key);
                    // Members of immutable modules cannot be assigned to
                    if (d->immutable) {
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                          "Cannot modify variable in immutable namespace.");
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, charpos, "");
                        goto cleanup_on_error;
                    }
                    // Create the member if it does not exist, so that it can be assigned to
                    if (obj == NULL) {
                        pplObjZom(stk, 0); // Create a temporary zombie for now
                        stk->refCount = 1;
                        ppl_dictAppendCpy(d, key, stk, sizeof(pplObj));
                        obj = (pplObj *) ppl_dictLookup(d, key);
                        // Failure to allocate memory is reported as an error
                        if (obj == NULL) {
                            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                              "Out of memory.");
                            ppl_tbAddExpr(context, inExpr, ERR_MEMORY, charpos, "");
                            goto cleanup_on_error;
                        }
                    }
                    // Pop the object whose member is being assigned to, and push a pointer to the member in its place
                    ppl_stackPop(context);
                    pplObjCpy(stk - 1, obj, 1, 0, 1);
                    (stk - 1)->refCount = 1;
                    context->stackPtr++; // Push the pointer to the member
                    break;
                } else {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "Cannot assign methods or variables to this object.");
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, charpos, "");
                    goto cleanup_on_error;
                }
                break;
            }
            case PPL_BC_SLICE:
            case PPL_BC_SLICE_POINTER: {
                int status = 0, errType = -1;
                const int getPtr = (opcode == PPL_BC_SLICE_POINTER);
                const int mode = instruction->operand.sliceFlags;
                const int range = (mode & PPL_SLICE_SINGLE_ITEM) == 0;
                const int maxset = (mode & PPL_SLICE_MAX_SET) != 0;
                const int minset = (mode & PPL_SLICE_MIN_SET) != 0;
                int Nrange = 1, i, min = 0, max = 0;
                *lastOpAssign = 0;
                if (range) {
                    Nrange = minset + maxset;
                    // Read the limits of the range from the stack; each must be a real, dimensionless integer
                    for (i = 1; i <= Nrange; i++) {
                        int *out = ((i == 1) && maxset) ? &max : &min;
                        // The limit must be a number
                        if ((stk - i)->objType != PPLOBJ_NUM) {
                            ppl_snprintfTrunc(
                                    context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                    "Range limits when slicing must be numerical values; supplied limit has type <%s>.",
                                    pplObjTypeNames[(stk - 1)->objType]);
                            ppl_tbAddExpr(context, inExpr, ERR_TYPE, charpos, "");
                            goto cleanup_on_error;
                        }
                        // The limit must be dimensionless
                        if (!(stk - i)->dimensionless) {
                            ppl_snprintfTrunc(
                                    context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                    "Range limits when slicing must be dimensionless numbers; supplied limit has units of <%s>.",
                                    ppl_printUnit(context, stk - i, NULL, NULL, 0, 1, 0));
                            ppl_tbAddExpr(context, inExpr, ERR_NUMERICAL, charpos, "");
                            goto cleanup_on_error;
                        }
                        // The limit must be real
                        if ((stk - i)->flagComplex) {
                            ppl_snprintfTrunc(
                                    context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                    "Range limits when slicing must be real numbers; supplied limit is complex.");
                            ppl_tbAddExpr(context, inExpr, ERR_NUMERICAL, charpos, "");
                            goto cleanup_on_error;
                        }
                        if ((!gsl_finite((stk - i)->real)) || ((stk - i)->real < INT_MIN) ||
                            ((stk - i)->real > INT_MAX)) {
                            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                              "Range limits when slicing must be in the range %d to %d.", INT_MIN,
                                              INT_MAX);
                            ppl_tbAddExpr(context, inExpr, ERR_RANGE, charpos, "");
                            goto cleanup_on_error;
                        }
                        *out = (int) (stk - i)->real;
                    }
                }
                if (!range) ppl_sliceItem(context, getPtr, &status, &errType, context->errStat.errBuff);
                else ppl_sliceRange(context, minset, min, maxset, max, &status, &errType, context->errStat.errBuff);
                // Report a failure to slice
                if (status) {
                    ppl_tbAddExpr(context, inExpr, errType, charpos, "");
                    goto cleanup_on_error;
                }
                break;
            }
            case PPL_BC_MAKE_DICT: {
                int k;
                const int len = instruction->operand.count;
                dict *d = ppl_dictInit(1);
                *lastOpAssign = 0;
                if (d == NULL) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff), "Out of memory.");
                    ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, charpos, "");
                    goto cleanup_on_error;
                }
                // The keys and values of the dictionary are on the stack, alternately
                if (context->stackPtr < 2 * len) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "Attempt to make dictionary with too few items on the stack.");
                    ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, charpos, "");
                    goto cleanup_on_error;
                }
                for (k = 0; k < len; k++)
                    // Every key must be a string
                    if ((stk - 2 * (len - k))->objType != PPLOBJ_STR) {
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                          "Dictionary keys must be strings; supplied key has type <%s>.",
                                          pplObjTypeNames[(stk - 2 * (len - k))->objType]);
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, charpos, "");
                        goto cleanup_on_error;
                    }
                // Copy each key and value into the new dictionary
                for (k = 0; k < len; k++) {
                    pplObj v;
                    pplObjCpy(&v, stk - 2 * (len - k) + 1, 0, 1, 1);
                    v.refCount = 1;
                    ppl_dictAppendCpy(d, (char *) ((stk - 2 * (len - k))->auxil), (void *) &v, sizeof(pplObj));
                }
                for (k = 0; k < 2 * len; k++) {
                    ppl_stackPop(context);
                } // Pop the keys and values, now copied into the dictionary
                pplObjDict(&context->stack[context->stackPtr], 0, 1, d);
                context->stack[context->stackPtr].refCount = 1;
                context->stackPtr++; // Push the new dictionary
                break;
            }
            case PPL_BC_MAKE_LIST: {
                int k;
                const int len = instruction->operand.count;
                list *l = ppl_listInit(1);
                *lastOpAssign = 0;
                if (l == NULL) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff), "Out of memory.");
                    ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, charpos, "");
                    goto cleanup_on_error;
                }
                // The items of the list are on the stack
                if (context->stackPtr < len) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "Attempt to make list with too few items on the stack.");
                    ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, charpos, "");
                    goto cleanup_on_error;
                }
                // Copy each item into the new list
                for (k = 0; k < len; k++) {
                    pplObj v;
                    pplObjCpy(&v, stk - len + k, 0, 1, 1);
                    v.refCount = 1;
                    ppl_listAppendCpy(l, (void *) &v, sizeof(pplObj));
                }
                for (k = 0; k < len; k++) { ppl_stackPop(context); } // Pop the items, now copied into the list
                pplObjList(&context->stack[context->stackPtr], 0, 1, l);
                context->stack[context->stackPtr].refCount = 1;
                context->stackPtr++; // Push the new list
                break;
            }
            case PPL_BC_CALL: {
                const int nArgs = instruction->operand.count;
                *lastOpAssign = 0;
                ppl_fnCall(context, inExpr, charpos, nArgs, dollarAllowed, iterDepth);
                if (context->errStat.status) goto cleanup_on_error;
                break;
            }
            case PPL_BC_OPERATOR: {
                const int t = instruction->operand.operatorCode;
                *lastOpAssign = 0;
                pplObjNum(stk, 0, 0, 0);
                stk->refCount = 1;
                // Apply the operator
                switch (t) {
                    case PPL_OP_NEGATE: {
                        if (ppl_castToNum(context, stk - 1, inExpr, charpos)) goto cast_fail;
                        (stk - 1)->real *= -1;
                        if ((stk - 1)->flagComplex) (stk - 1)->imag *= -1;
                        break;
                    }
                    case PPL_OP_UNARY_PLUS: {
                        if (ppl_castToNum(context, stk - 1, inExpr, charpos)) goto cast_fail;
                        break;
                    }
                    case PPL_OP_BITWISE_NOT: {
                        if (ppl_castToInt(context, stk - 1, "~", inExpr, charpos)) goto cast_fail;
                        (stk - 1)->real = ~(int) ((stk - 1)->real);
                        break;
                    }
                    case PPL_OP_LOGICAL_NOT: {
                        ppl_castToBool(context, stk - 1);
                        (stk - 1)->real = !(stk - 1)->real;
                        break;
                    }
                    case PPL_OP_POWER: {
                        int status = 0, errType = -1;
                        if (ppl_castToNum(context, stk - 1, inExpr, charpos)) goto cast_fail;
                        if (ppl_castToNum(context, stk - 2, inExpr, charpos)) goto cast_fail;
                        ppl_uaPow(context, stk - 2, stk - 1, stk, &status, &errType, context->errStat.errBuff);
                        // Report an arithmetic error
                        if (status) {
                            ppl_tbAddExpr(context, inExpr, errType, charpos, "");
                            goto cleanup_on_error;
                        }
                        // Pop both operands, then move the result, which was computed in the free slot above them, into their place
                        ppl_stackPop(context);
                        ppl_stackPop(context);
                        memcpy(stk - 2, stk, sizeof(pplObj));
                        context->stackPtr++; // ... which is now on top of the stack
                        break;
                    }
                    case PPL_OP_MULTIPLY: {
                        int status = 0, errType = -1;
                        ppl_opMul(context, stk - 2, stk - 1, stk, 1, &status, &errType, context->errStat.errBuff);
                        // Report an arithmetic error
                        if (status) {
                            ppl_tbAddExpr(context, inExpr, errType, charpos, "");
                            goto cleanup_on_error;
                        }
                        // Pop both operands, then move the result, which was computed in the free slot above them, into their place
                        ppl_stackPop(context);
                        ppl_stackPop(context);
                        memcpy(stk - 2, stk, sizeof(pplObj));
                        context->stackPtr++; // ... which is now on top of the stack
                        break;
                    }
                    case PPL_OP_DIVIDE: {
                        int status = 0, errType = -1;
                        ppl_opDiv(context, stk - 2, stk - 1, stk, 1, &status, &errType, context->errStat.errBuff);
                        // Report an arithmetic error
                        if (status) {
                            ppl_tbAddExpr(context, inExpr, errType, charpos, "");
                            goto cleanup_on_error;
                        }
                        // Pop both operands, then move the result, which was computed in the free slot above them, into their place
                        ppl_stackPop(context);
                        ppl_stackPop(context);
                        memcpy(stk - 2, stk, sizeof(pplObj));
                        context->stackPtr++; // ... which is now on top of the stack
                        break;
                    }
                    case PPL_OP_MODULO: {
                        int status = 0, errType = -1;
                        if (ppl_castToNum(context, stk - 1, inExpr, charpos)) goto cast_fail;
                        if (ppl_castToNum(context, stk - 2, inExpr, charpos)) goto cast_fail;
                        ppl_uaMod(context, stk - 2, stk - 1, stk, &status, &errType, context->errStat.errBuff);
                        // Report an arithmetic error
                        if (status) {
                            ppl_tbAddExpr(context, inExpr, errType, charpos, "");
                            goto cleanup_on_error;
                        }
                        // Pop both operands, then move the result, which was computed in the free slot above them, into their place
                        ppl_stackPop(context);
                        ppl_stackPop(context);
                        memcpy(stk - 2, stk, sizeof(pplObj));
                        context->stackPtr++; // ... which is now on top of the stack
                        break;
                    }
                    case PPL_OP_ADD: {
                        int status = 0, errType = -1;
                        ppl_opAdd(context, stk - 2, stk - 1, stk, 1, &status, &errType, context->errStat.errBuff);
                        // Report an arithmetic error
                        if (status) {
                            ppl_tbAddExpr(context, inExpr, errType, charpos, "");
                            goto cleanup_on_error;
                        }
                        // Pop both operands, then move the result, which was computed in the free slot above them, into their place
                        ppl_stackPop(context);
                        ppl_stackPop(context);
                        memcpy(stk - 2, stk, sizeof(pplObj));
                        context->stackPtr++; // ... which is now on top of the stack
                        break;
                    }
                    case PPL_OP_SUBTRACT: {
                        int status = 0, errType = -1;
                        ppl_opSub(context, stk - 2, stk - 1, stk, 1, &status, &errType, context->errStat.errBuff);
                        // Report an arithmetic error
                        if (status) {
                            ppl_tbAddExpr(context, inExpr, errType, charpos, "");
                            goto cleanup_on_error;
                        }
                        // Pop both operands, then move the result, which was computed in the free slot above them, into their place
                        ppl_stackPop(context);
                        ppl_stackPop(context);
                        memcpy(stk - 2, stk, sizeof(pplObj));
                        context->stackPtr++; // ... which is now on top of the stack
                        break;
                    }
                    case PPL_OP_SHIFT_LEFT: {
                        if (ppl_castToInt(context, stk - 1, "<<", inExpr, charpos)) goto cast_fail;
                        if (ppl_castToInt(context, stk - 2, "<<", inExpr, charpos)) goto cast_fail;
                        (stk - 2)->real = ((int) (stk - 2)->real) << ((int) (stk - 1)->real);
                        ppl_stackPop(context); // Pop the right-hand operand; the result has replaced the left-hand one
                        break;
                    }
                    case PPL_OP_SHIFT_RIGHT: {
                        if (ppl_castToInt(context, stk - 1, ">>", inExpr, charpos)) goto cast_fail;
                        if (ppl_castToInt(context, stk - 2, ">>", inExpr, charpos)) goto cast_fail;
                        (stk - 2)->real = ((int) (stk - 2)->real) >> ((int) (stk - 1)->real);
                        ppl_stackPop(context); // Pop the right-hand operand; the result has replaced the left-hand one
                        break;
                    }
                    case PPL_OP_BITWISE_AND: {
                        if (ppl_castToInt(context, stk - 1, "&", inExpr, charpos)) goto cast_fail;
                        if (ppl_castToInt(context, stk - 2, "&", inExpr, charpos)) goto cast_fail;
                        (stk - 2)->real = ((int) (stk - 2)->real) & ((int) (stk - 1)->real);
                        ppl_stackPop(context); // Pop the right-hand operand; the result has replaced the left-hand one
                        break;
                    }
                    case PPL_OP_BITWISE_XOR: {
                        if (ppl_castToInt(context, stk - 1, "^", inExpr, charpos)) goto cast_fail;
                        if (ppl_castToInt(context, stk - 2, "^", inExpr, charpos)) goto cast_fail;
                        (stk - 2)->real = ((int) (stk - 2)->real) ^ ((int) (stk - 1)->real);
                        ppl_stackPop(context); // Pop the right-hand operand; the result has replaced the left-hand one
                        break;
                    }
                    case PPL_OP_BITWISE_OR: {
                        if (ppl_castToInt(context, stk - 1, "|", inExpr, charpos)) goto cast_fail;
                        if (ppl_castToInt(context, stk - 2, "|", inExpr, charpos)) goto cast_fail;
                        (stk - 2)->real = ((int) (stk - 2)->real) | ((int) (stk - 1)->real);
                        ppl_stackPop(context); // Pop the right-hand operand; the result has replaced the left-hand one
                        break;
                    }
                    case PPL_OP_COMMA: // a, b: discard a and keep b
                    {
                        ppl_garbageObject(stk - 2);
                        memcpy(stk - 2, stk - 1, sizeof(pplObj));
                        context->stackPtr--; // Discard the left operand, keeping the right one as the result
                        break;
                    }
                    case PPL_OP_LESS:
                    case PPL_OP_LESS_EQUAL:
                    case PPL_OP_GREATER_EQUAL:
                    case PPL_OP_GREATER:
                    case PPL_OP_EQUAL:
                    case PPL_OP_NOT_EQUAL: {
                        int stat = 0, errType = -1,
                            cmp = pplObjCmp(context, stk - 2, stk - 1, &stat, &errType, context->errStat.errBuff, 0);
                        // Report objects which cannot be compared
                        if (stat) {
                            ppl_tbAddExpr(context, inExpr, errType, charpos, "");
                            goto cleanup_on_error;
                        }
                        if (t == PPL_OP_LESS) stat = (cmp == -1);
                        else if (t == PPL_OP_LESS_EQUAL) stat = (cmp == -1) || (cmp == 0);
                        else if (t == PPL_OP_GREATER_EQUAL) stat = (cmp == 1) || (cmp == 0);
                        else if (t == PPL_OP_GREATER) stat = (cmp == 1);
                        else if (t == PPL_OP_EQUAL) stat = (cmp == 0);
                        else stat = (cmp != 0);
                        // Pop both operands, and push the boolean result of the comparison in their place
                        ppl_stackPop(context);
                        ppl_stackPop(context);
                        pplObjBool(stk - 2, 0, stat);
                        (stk - 2)->refCount = 1;
                        context->stackPtr++; // Push the boolean
                        break;
                    }
                    default:
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                          "Unknown operator with id=%d.", t);
                        ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, charpos, "");
                        goto cleanup_on_error;
                }
                break;
            }
            case PPL_BC_ASSIGN: {
                const int t = instruction->operand.operatorCode;
                pplObj *o = stk - 2;
                pplObj *in = stk - 1;
                pplObj *tmp = stk;
                const int t2 = in->objType;
                int status = 0, errType = -1;
                *lastOpAssign = 1;
                if (context->stackPtr < 2) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "Too few items on stack for assignment operator.");
                    ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, charpos, "");
                    goto cleanup_on_error;
                }
                // Only variables can be assigned to
                if (o->self_lval == NULL) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "Assignment operators can only be applied to variables.");
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, charpos, "");
                    goto cleanup_on_error;
                }
                // Immutable variables cannot be assigned to
                if (o->immutable) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "Cannot assign to an immutable object.");
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, charpos, "");
                    goto cleanup_on_error;
                }

                // Plain assignment copies the value; the fused operators, such as +=, compute the new value first
                if (t == PPL_ASSIGN) {
                    pplObjCpy(tmp, in, 0, 0, 1);
                    tmp->immutable = 0;
                    if (assignToVariable(context, o, tmp, t2, inExpr, charpos)) goto cleanup_on_error;
                } else if (t == PPL_ASSIGN_ADD) {
                    ppl_opAdd(context, o, in, tmp, 0, &status, &errType, context->errStat.errBuff);
                    // Report an arithmetic error
                    if (status) {
                        ppl_tbAddExpr(context, inExpr, errType, charpos, "");
                        goto cleanup_on_error;
                    }
                    if (assignToVariable(context, o, tmp, t2, inExpr, charpos)) goto cleanup_on_error;
                } else if (t == PPL_ASSIGN_SUBTRACT) {
                    ppl_opSub(context, o, in, tmp, 0, &status, &errType, context->errStat.errBuff);
                    // Report an arithmetic error
                    if (status) {
                        ppl_tbAddExpr(context, inExpr, errType, charpos, "");
                        goto cleanup_on_error;
                    }
                    if (assignToVariable(context, o, tmp, t2, inExpr, charpos)) goto cleanup_on_error;
                } else if (t == PPL_ASSIGN_MULTIPLY) {
                    ppl_opMul(context, o, in, tmp, 0, &status, &errType, context->errStat.errBuff);
                    // Report an arithmetic error
                    if (status) {
                        ppl_tbAddExpr(context, inExpr, errType, charpos, "");
                        goto cleanup_on_error;
                    }
                    if (assignToVariable(context, o, tmp, t2, inExpr, charpos)) goto cleanup_on_error;
                } else if (t == PPL_ASSIGN_DIVIDE) {
                    ppl_opDiv(context, o, in, tmp, 0, &status, &errType, context->errStat.errBuff);
                    // Report an arithmetic error
                    if (status) {
                        ppl_tbAddExpr(context, inExpr, errType, charpos, "");
                        goto cleanup_on_error;
                    }
                    if (assignToVariable(context, o, tmp, t2, inExpr, charpos)) goto cleanup_on_error;
                } else {
                    // The remaining fused operators, e.g. %= and &=, only act on numbers
                    if (o->objType != PPLOBJ_NUM) {
                        ppl_snprintfTrunc(
                                context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                "The fused operator-assignment operators can only be applied to numeric variables.");
                        ppl_tbAddExpr(context, inExpr, ERR_TYPE, charpos, "");
                        goto cleanup_on_error;
                    }
                    if (ppl_castToNum(context, in, inExpr, charpos)) goto cast_fail;
                    pplObjNum(tmp, 0, 0, 0);
                    // Compute the new value
                    switch (t) {
                        case PPL_ASSIGN_MODULO:
                            ppl_uaMod(context, o, in, tmp, &status, &errType, context->errStat.errBuff);
                            break;
                        case PPL_ASSIGN_BITWISE_AND:
                            if (ppl_castToInt(context, in, "&=", inExpr, charpos)) goto cast_fail;
                            if (ppl_castToInt(context, o, "&=", inExpr, charpos)) goto cast_fail;
                            tmp->real = ((int) o->real) & ((int) in->real);
                            break;
                        case PPL_ASSIGN_BITWISE_XOR:
                            if (ppl_castToInt(context, in, "^=", inExpr, charpos)) goto cast_fail;
                            if (ppl_castToInt(context, o, "^=", inExpr, charpos)) goto cast_fail;
                            tmp->real = ((int) o->real) ^ ((int) in->real);
                            break;
                        case PPL_ASSIGN_BITWISE_OR:
                            if (ppl_castToInt(context, in, "|=", inExpr, charpos)) goto cast_fail;
                            if (ppl_castToInt(context, o, "|=", inExpr, charpos)) goto cast_fail;
                            tmp->real = ((int) o->real) | ((int) in->real);
                            break;
                        case PPL_ASSIGN_SHIFT_LEFT:
                            if (ppl_castToInt(context, in, "<<=", inExpr, charpos)) goto cast_fail;
                            if (ppl_castToInt(context, o, "<<=", inExpr, charpos)) goto cast_fail;
                            tmp->real = ((int) o->real) << ((int) in->real);
                            break;
                        case PPL_ASSIGN_SHIFT_RIGHT:
                            if (ppl_castToInt(context, in, ">>=", inExpr, charpos)) goto cast_fail;
                            if (ppl_castToInt(context, o, ">>=", inExpr, charpos)) goto cast_fail;
                            tmp->real = ((int) o->real) >> ((int) in->real);
                            break;
                        case PPL_ASSIGN_POWER:
                            ppl_uaPow(context, o, in, tmp, &status, &errType, context->errStat.errBuff);
                            break;
                        default:
                            ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                              "Unknown fused operator-assignment operator with id=%d.", t);
                            ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, charpos, "");
                            goto cleanup_on_error;
                    }
                    // Report an arithmetic error
                    if (status) {
                        ppl_tbAddExpr(context, inExpr, errType, charpos, "");
                        goto cleanup_on_error;
                    }
                    if (assignToVariable(context, o, tmp, t2, inExpr, charpos)) goto cleanup_on_error;
                    break;
                }
                break;
            }
            case PPL_BC_INCREMENT: {
                const int t = instruction->operand.operatorCode;
                pplObj *o = stk - 1;
                *lastOpAssign = 1;
                if (context->stackPtr < 1) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "Too few items on stack for -- or ++ operator.");
                    ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, charpos, "");
                    goto cleanup_on_error;
                }
                // Only numeric, mutable variables can be incremented or decremented
                if (o->self_lval == NULL) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "The -- and ++ operators can only be applied to variables.");
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, charpos, "");
                    goto cleanup_on_error;
                }
                // The variable must be a number
                if (o->objType != PPLOBJ_NUM) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "The -- and ++ operators can only be applied to numeric variables.");
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, charpos, "");
                    goto cleanup_on_error;
                }
                // The variable must not be immutable
                if (o->immutable) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "Cannot modify an immutable object.");
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, charpos, "");
                    goto cleanup_on_error;
                }
                // Change the variable, or the element of a vector or matrix, in place
                switch (t) {
                    case PPL_OP_POST_DECREMENT:
                    case PPL_OP_PRE_DECREMENT:
                        if (o->self_dval != NULL) (*o->self_dval)--;
                        else o->self_lval->real--;
                        if (t == PPL_OP_PRE_DECREMENT)
                            o->real = (o->self_dval != NULL) ? *o->self_dval : o->self_lval->real;
                        break;
                    case PPL_OP_POST_INCREMENT:
                    case PPL_OP_PRE_INCREMENT:
                        if (o->self_dval != NULL) (*o->self_dval)++;
                        else o->self_lval->real++;
                        if (t == PPL_OP_PRE_INCREMENT)
                            o->real = (o->self_dval != NULL) ? *o->self_dval : o->self_lval->real;
                        break;
                    default:
                        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                          "Unknown increment/decrement operator with id=%d.", t);
                        ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, charpos, "");
                        goto cleanup_on_error;
                }
                break;
            }
            case PPL_BC_STRING_SUBST: {
                const int Nsubs = instruction->operand.count;
                int i;
                // The format string is on the stack, followed by the values to substitute into it
                if (context->stackPtr < Nsubs + 1) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "Too few items on stack for string substitution operator.");
                    ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, charpos, "");
                    goto cleanup_on_error;
                }
                // The format must be a string
                if ((stk - Nsubs - 1)->objType != PPLOBJ_STR) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "Attempt to apply string substitution operator to a non-string.");
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, charpos, "");
                    goto cleanup_on_error;
                }
                expEval_stringSubs(context, inExpr, charpos, Nsubs);
                if (context->errStat.status) { goto cast_fail; }
                stk->refCount = 1;
                // The result was written to the slot above the top of the stack. Pop the format string and the Nsubs
                // values substituted into it, then move the result down into their place.
                context->stackPtr--;
                for (i = 0; i < Nsubs + 1; i++) { ppl_stackPop(context); }
                memcpy(&context->stack[context->stackPtr], stk, sizeof(pplObj));
                context->stackPtr++; // Push the result
                break;
            }
            case PPL_BC_DOLLAR: {
                // The column to fetch is on the top of the stack
                if (context->stackPtr < 1) {
                    ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                      "Too few items on stack for dollar operator.");
                    ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, charpos, "");
                    goto cleanup_on_error;
                }
                int t = context->stack[context->stackPtr - 1].objType;
                // A number is a column number; a string is a column heading
                if (t == PPLOBJ_NUM) {
                    double num = context->stack[context->stackPtr - 1].real;
                    // The column number must be in range
                    if ((num < -4) || (num > MAX_DATACOLS) || (!gsl_finite(num))) {
                        ppl_snprintfTrunc(
                                context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                "The $ column reference operator was passed an illegal column number; must be in range -3 to %d.",
                                MAX_DATACOLS);
                        ppl_tbAddExpr(context, inExpr, ERR_RANGE, charpos, "");
                        goto cleanup_on_error;
                    }
                    int n = (int) round(num);
                    ppl_dollarOp_fetchColByNum(context, inExpr, charpos, n);
                    if (context->errStat.status) goto cast_fail;
                } else if (t == PPLOBJ_STR) {
                    char *name = (char *) context->stack[context->stackPtr - 1].auxil;
                    const int malloced = context->stack[context->stackPtr - 1].auxilMalloced;
                    ppl_dollarOp_fetchColByName(context, inExpr, charpos, name);
                    // Free the copy of the column name
                    if (malloced) {
                        free(name);
                        name = NULL;
                    }
                } else {
                    ppl_snprintfTrunc(
                            context->errStat.errBuff, sizeof(context->errStat.errBuff),
                            "The $ column reference operator can only act on column numbers and (string) names. Object supplied was of type <%s>,",
                            pplObjTypeNames[t]);
                    ppl_tbAddExpr(context, inExpr, ERR_TYPE, charpos, "");
                    goto cleanup_on_error;
                }
                break;
            }
            case PPL_BC_BRANCH_IF_FALSE: {
                *lastOpAssign = 0;
                ppl_castToBool(context, stk - 1);
                int branch = (stk - 1)->real == 0;
                // Jump if the value is false
                if (branch) {
                    // Jump, either leaving the tested value on the stack as the result (&& and ||), or popping it (?:)
                    next = instruction->operand.target;
                    if (instruction->popOnBranch) { ppl_stackPop(context); }
                } else {
                    ppl_stackPop(context); // Not branching: pop the tested value and carry on
                }
                break;
            }
            case PPL_BC_BRANCH_IF_TRUE: {
                *lastOpAssign = 0;
                ppl_castToBool(context, stk - 1);
                int branch = (stk - 1)->real != 0;
                // Jump if the value is true
                if (branch) {
                    // Jump, either leaving the tested value on the stack as the result (&& and ||), or popping it (?:)
                    next = instruction->operand.target;
                    if (instruction->popOnBranch) { ppl_stackPop(context); }
                } else {
                    ppl_stackPop(context); // Not branching: pop the tested value and carry on
                }
                break;
            }
            case PPL_BC_GOTO: {
                *lastOpAssign = 0;
                next = instruction->operand.target;
                break;
            }
            case PPL_BC_MAKE_BOOLEAN: {
                *lastOpAssign = 0;
                ppl_castToBool(context, stk - 1);
                break;
            }
            default:
                ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                                  "Illegal bytecode opcode passed to expEval.");
                charpos = 0;
                ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, charpos, "");
                goto cleanup_on_error;
        }
        if (opcode == PPL_BC_RETURN) break;
        j = next;
    }
    // The expression must have left its value on the stack
    if (context->stackPtr <= 0) {
        ppl_snprintfTrunc(context->errStat.errBuff, sizeof(context->errStat.errBuff),
                          "Unexpected empty stack at end of evaluation.");
        charpos = 0;
        ppl_tbAddExpr(context, inExpr, ERR_INTERNAL, charpos, "");
        goto cleanup_on_error;
    }
    if (context->stackPtr != initialStackPtr + 1) {
        ppl_warning(&context->errcontext, ERR_INTERNAL, "Unexpected junk on stack in expEval.");
    }
    return &context->stack[context->stackPtr - 1];

// On error, discard everything this evaluation has pushed, and return NULL
cast_fail:
cleanup_on_error:
    ppl_stackClean(context, initialStackPtr); // Discard everything which this evaluation pushed onto the stack
    return NULL;
}
