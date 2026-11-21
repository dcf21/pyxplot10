// parserCompile.c
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

// Pyxplot's command-line parser. Each line of input is matched against the syntax trees which parserInit.c builds from
// the grammar of every command, and compiled into a parserLine: a list of atoms, each of which places a literal or an
// expression into a slot of an output stack when the line is executed by parserExecute.c. It also handles commands
// which span several lines, such as { } blocks of code and inline data, the expansion of macros, the "expecting..."
// hints in syntax error messages, and tab completion.

#define _PARSERCOMPILE_C 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <sys/wait.h>

#ifdef HAVE_READLINE
#include <readline/readline.h>
#endif

#include "coreUtils/errorReport.h"

#include "expressions/expCompile_fns.h"
#include "expressions/traceback_fns.h"
#include "mathsTools/dcfmath.h"
#include "settings/epsColors.h"
#include "settings/settingTypes.h"
#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"
#include "parser/cmdList.h"
#include "parser/parser.h"
#include "userspace/context.h"
#include "userspace/garbageCollector.h"
#include "userspace/pplObj_fns.h"

//! parseTbAdd - Add an error in the parsing of a command line to the traceback. If the error arose within an
//! algebraic expression at or after the position of the error, the position within the expression is also recorded.
//! \param [in] c - The Pyxplot context
//! \param [in] srcLineN - The line number of the source of the command
//! \param [in] srcId - The ID number of the source of the command
//! \param [in] srcFname - The filename of the source of the command
//! \param [in] errType - The type of the error, e.g. ERR_SYNTAX
//! \param [in] errPos - The position of the error within the command line
//! \param [in] line - The command line

static void parseTbAdd(ppl_context *c, const int srcLineN, const long srcId, const char *srcFname, const int errType,
                       const int errPos, const char *line) {
    ppl_tbAdd(c, srcLineN, srcId, srcFname, 1, errType, errPos, line, "");
    if ((c->errStat.errPosExpr == -1) && (c->errStat.oldErrPosExpr == c->algebraErrPos) && (c->algebraErrPos >= 0) &&
        (c->algebraErrPos >= errPos)) {
        c->errStat.errPosExpr = c->algebraErrPos;
        c->errStat.sourceIdExpr = srcId;
    }
}

//! strDup - Copy the first N characters of a string into a newly malloced, null-terminated string
//! \param [in] in - The string to copy
//! \param [in] N - The number of characters to copy
//! \return The copy, or NULL if memory allocation failed

static char *strDup(const char *in, int N) {
    char *out = (char *) malloc(N + 1);
    if (out == NULL) return NULL;
    strncpy(out, in, N + 1);
    out[N] = '\0';
    return out;
}

//! ppl_parserAtomAdd - Append an atom to a compiled command line. Each atom places one value (a literal, or the
//! result of evaluating an expression) into a position in the output stack when the command is executed.
//! \param [in|out] in - The compiled command line
//! \param [in] stackOutPos - The position in the output stack at which to place the value
//! \param [in] linePos - The position of the value within the command line, used to report errors
//! \param [in] options - String of characters listing the types of value which are allowed, e.g. "d" for an integer
//! \param [in] expr - An expression to evaluate to obtain the value, or NULL if <literal> is given
//! \param [in] literal - A literal value to place in the stack, or NULL if <expr> is given

void ppl_parserAtomAdd(parserLine *in, const int stackOutPos, const int linePos, const char *options, pplExpr *expr,
                       const pplObj *literal) {
    parserAtom *output = (parserAtom *) malloc(sizeof(parserAtom));
    if (output == NULL) return;
    output->stackOutPos = stackOutPos;
    output->linePos = linePos;
    output->expr = expr;
    output->next = NULL;
    strncpy(output->options, options, 8);
    output->options[7] = '\0';
    if (literal == NULL) output->literal = NULL;
    // Literal values are copied into the atom, which owns the copy
    else {
        output->literal = (pplObj *) malloc(sizeof(pplObj));
        // Failure to allocate memory abandons the atom
        if (output->literal == NULL) {
            free(output);
            output = NULL;
            return;
        }
        memcpy(output->literal, literal, sizeof(pplObj));
        output->literal->refCount = 1;
        output->literal->amMalloced = 1;
    }
    // Append the atom to the end of the line's list of atoms
    if (in->lastAtom == NULL) {
        in->firstAtom = output;
    } else {
        in->lastAtom->next = output;
    }
    in->lastAtom = output;
}

//! parse_descend - Recursively match a command line against a node of the tree which describes the syntax of
//! Pyxplot's commands, writing atoms describing the values matched into a compiled command line
//! \param [in] c - The Pyxplot context
//! \param [in|out] s - The parser status, which tracks the progress of compiling multi-line blocks of commands
//! \param [in] writeOut - If true, write atoms into the compiled command line; otherwise only test for a match
//! \param [in] srcLineN - The line number of the source of the command, used to report errors
//! \param [in] srcId - The ID of the source of the command, used to report errors
//! \param [in] srcFname - The filename of the source of the command, used to report errors
//! \param [in] line - The command line being compiled
//! \param [in|out] linepos - The position in <line> reached so far
//! \param [in] node - The syntax node to match, or NULL to resume matching a sequence of nodes descended out of on a
//! previous line
//! \param [in|out] tabCompNo - When performing tab completion, the number of the completion requested; NULL otherwise
//! \param [in] tabCompStart - When performing tab completion, the position in <line> at which the word being
//! completed starts, or -1 otherwise
//! \param [out] tabCompTxt - Character buffer into which to write the tab completion, if requested
//! \param [in] level - The depth of <node> within the syntax tree
//! \param [in] blockDepth - The depth of nesting of code blocks
//! \param [out] match - Set to true if the command line matched the node
//! \return 0 if the command line did not match, 1 if it matched, 2 if a tab completion was found, or 3 if another
static int parse_descend(ppl_context *c, parserStatus *s, int writeOut, int srcLineN, long srcId, char *srcFname,
                         char *line, int *linepos, parserNode *node, int *tabCompNo, int tabCompStart, char *tabCompTxt,
                         int level, int blockDepth, int *match);

//! parse_repeat - Match the repetitions of a [ ] structure in the parser's description of a command
//! \param [in] node - The node of the [ ] structure
//! \param [in] first - Flag indicating whether no repetition has yet been matched
//! \param [in] oldStackOffset - The position in the stack of the parser output before the structure
//! Other parameters, and the return value, are as for parse_descend()

static int parse_repeat(ppl_context *c, parserStatus *s, const int writeOut, const int srcLineN, const long srcId,
                        char *srcFname, char *line, int *linepos, parserNode *node, int *tabCompNo,
                        const int tabCompStart, char *tabCompTxt, const int level, int blockDepth, int *match, int first,
                        const int oldStackOffset) {
    int status = 0, separator = 1;
    char sepStr[4];
    parserNode sepNode;
    sepStr[0] = node->varName[strlen(node->varName) - 1];
    sepStr[1] = '\0';
    if (isalnum(sepStr[0])) separator = 0;
    sepNode.type = PN_TYPE_ITEM;
    sepNode.listLen = 0;
    sepNode.acLevel = -1;
    sepNode.matchString = sepStr;
    sepNode.outString = sepStr;
    sepNode.varName = "X";
    sepNode.outStackPos = PARSE_arc_X;
    sepNode.firstChild = NULL;
    sepNode.nextSibling = NULL;

    // A [ ] structure matches its children repeatedly. Each repetition is first tried without writing output;
    // if it matches, it is matched again writing output into a new block of stack slots, the first slot of
    // which points to the next block.
    while (1) {
        const int lineposOld = *linepos;
        const int matchOld = *match;
        parserNode *nodeIter = node->firstChild;

        // Between repetitions there must be a separator, if the variable name ends in one, e.g. "," in
        // "var_names,". If it is absent, the repetitions have ended.
        if (separator && !first) {
            status = parse_descend(c, s, 0, srcLineN, srcId, srcFname, line, linepos, &sepNode, tabCompNo,
                                   tabCompStart, tabCompTxt, level + 1, blockDepth, match);
            if (status == 3) { status = 0; } // Separator shouldn't ask for more lines!
            // No separator: the repetition has ended, successfully
            if (status == 0) {
                *linepos = lineposOld;
                *match = matchOld;
                status = 1;
                goto repCleanup;
            }
            if (status == 2) goto repCleanup;
        }
        int lineposOld2 = *linepos;
        // Test, without writing any output, whether the line contains another repetition
        while (nodeIter != NULL) {
            status = parse_descend(c, s, 0, srcLineN, srcId, srcFname, line, linepos, nodeIter, tabCompNo,
                                   tabCompStart, tabCompTxt, level + 1, blockDepth, match);
            // A trial run which reaches the end of the line has failed
            if (status == 3) {
                status = 0;
                goto repCleanup;
            } // Trial run with writeOut=0 shouldn't ask for more lines!
            // Another repetition does not match
            if (status == 0) {
                // Failing to match ends the repetitions successfully if no first repetition is required (REP2),
                // or after any repetition without a separator; after a separator, another repetition must
                // follow
                if (first ? (node->type == PN_TYPE_REP2) : !separator) {
                    status = 1;
                    *linepos = lineposOld;
                    *match = matchOld;
                }
                goto repCleanup;
            }
            if (status == 2) goto repCleanup;
            nodeIter = nodeIter->nextSibling;
        }
        // Match the repetition again, writing output
        if ((s != NULL) && writeOut) {
            const int oldStackLen = s->pl[blockDepth]->stackLen;
            pplObj val;
            val.refCount = 1;
            pplObjNum(&val, 1, oldStackLen, 0);
            ppl_parserAtomAdd(
                    s->pl[blockDepth],
                    s->pl[blockDepth]->stackOffset +
                            ((s->pl[blockDepth]->stackOffset == oldStackOffset) ? node->outStackPos : 0),
                    *linepos, "", NULL, &val);
            s->pl[blockDepth]->stackLen += node->listLen;
            s->pl[blockDepth]->stackOffset = oldStackLen;
            nodeIter = node->firstChild;
            *linepos = lineposOld2;
            *match = matchOld;
            while (nodeIter != NULL) {
                // Add an atom to the parserLine to point to the beginning of structure
                // This is written to either stack location (offset + stackPos) if first iteration, or (offset + 0) subsequently
                // In any repeating structure, first stack position is pointer to next item
                status = parse_descend(c, s, writeOut, srcLineN, srcId, srcFname, line, linepos, nodeIter,
                                       tabCompNo, tabCompStart, tabCompTxt, level + 1, blockDepth, match);
                if (status == 3) return status;
                if (status != 1) goto repCleanup;
                nodeIter = nodeIter->nextSibling;
            }
        }
        first = 0;
    }
repCleanup:
    if (s != NULL) s->pl[blockDepth]->stackOffset = oldStackOffset;
    return status;
}

//! line of input is required

static int parse_descend(ppl_context *c, parserStatus *s, const int writeOut, const int srcLineN, const long srcId,
                         char *srcFname, char *line, int *linepos, parserNode *node, int *tabCompNo,
                         const int tabCompStart, char *tabCompTxt, const int level, int blockDepth, int *match) {
    char msg[PPL_MSG_LENGTH];
    int status = 0; // 0=fail; 1=pass; 2=tab completion returned; 3=want another line.

    // If node is NULL, this means we're putting back together a hierarchy of SEQs that we earlier descended out of to look for more input statements
    if (node == NULL) {
        parserNode *nodeNext = s->stk[blockDepth][level + 1];
        node = s->stk[blockDepth][level];
        if (nodeNext != NULL) // We need to descend still further...
        {
            parserNode *nodeIter = node->firstChild;
            status = parse_descend(c, s, writeOut, srcLineN, srcId, srcFname, line, linepos, NULL, tabCompNo,
                                   tabCompStart, tabCompTxt, level + 1, blockDepth, match);
            if (status != 1) goto cleanup;

            // We came out of child node with a success return code. This means we carry on chomping along the SEQ
            // If we've come out of an OPT, treat it as a SEQ now. If we're in the middle of a REP, treat the rest of
            // the current repetition as a SEQ.
            if ((node->type == PN_TYPE_SEQ) || (node->type == PN_TYPE_OPT) || (node->type == PN_TYPE_REP) ||
                (node->type == PN_TYPE_REP2)) {
                while ((nodeIter != nodeNext) && (nodeIter != NULL))
                    nodeIter = nodeIter->nextSibling; // Work out where we were...
                if (nodeIter == nodeNext) nodeIter = nodeIter->nextSibling;
                // Match each of the remaining nodes in the sequence
                while (nodeIter != NULL) {
                    status = parse_descend(c, s, writeOut, srcLineN, srcId, srcFname, line, linepos, nodeIter,
                                           tabCompNo, tabCompStart, tabCompTxt, level + 1, blockDepth, match);
                    if (status != 1) goto cleanup;
                    nodeIter = nodeIter->nextSibling;
                }
                // See whether a REP repeats again
                if ((node->type == PN_TYPE_REP) || (node->type == PN_TYPE_REP2))
                    status = parse_repeat(c, s, writeOut, srcLineN, srcId, srcFname, line, linepos, node, tabCompNo,
                                          tabCompStart, tabCompTxt, level, blockDepth, match, 0,
                                          s->oldStackOffset[blockDepth][level]);
            } else {
                status = 0;
                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                  "Bad node type coming out of code block.");
                parseTbAdd(c, srcLineN, srcId, srcFname, ERR_INTERNAL, 0, line);
            }
            goto cleanup;
        }
        // We don't need to descend any further... we let ourselves loose on the code below!
    }

    // If this is a top-level attempt to parse a statement, make a new parserLine structure to hold the output of our attempt
    if ((s != NULL) && writeOut && (level == 0)) {
        parserLine *output = NULL;
        ppl_parserLineInit(&output, srcLineN, srcId, srcFname, line);
        // Failure to allocate memory is reported as an error
        if (output == NULL) {
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
            parseTbAdd(c, srcLineN, srcId, srcFname, ERR_MEMORY, 0, line);
            return 0;
        }
        output->stackLen = node->listLen;
        ppl_parserStatAdd(s, blockDepth, output);
    }

    // FFW over spaces
    while ((line[*linepos] != '\0') && ((unsigned char) line[*linepos] <= ' ')) (*linepos)++;

    // Deal with the node we've been given
    switch (node->type) {
        case PN_TYPE_CONFIRM: {
            *match = 1;
            status = 1;
            goto cleanup;
        }
        case PN_TYPE_DATABLK: {
            // When only testing for a match, a block of inline data always matches
            if ((s == NULL) || (!writeOut)) {
                status = 1;
            } else {
                int first = (s->blockDepth != blockDepth + 1);

                // first line -- must be at end of command line which includes '--'
                if (first) {
                    // The command which introduces inline data must end here
                    if (line[*linepos] != '\0') {
                        status = 0;
                        goto cleanup;
                    }
                }

                // Make parserLine structure to hold this line of text
                parserLine *output = NULL;
                ppl_parserLineInit(&output, srcLineN, srcId, srcFname, line);
                // Failure to allocate memory is reported as an error
                if (output == NULL) {
                    ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
                    ppl_tbAdd(c, srcLineN, srcId, srcFname, 0, ERR_MEMORY, 0, line, "inline data");
                    status = 0;
                    goto cleanup;
                }
                output->stackLen = 0;
                output->containsMacros = 1;

                // If s->blockDepth is not already set, this is the first line of data
                if (first) {
                    pplObj val;
                    val.refCount = 1;
                    pplObjBytecode(&val, 0, (void *) output);
                    ppl_parserAtomAdd(s->pl[blockDepth], s->pl[blockDepth]->stackOffset + node->outStackPos, *linepos,
                                      "", NULL, &val);
                    s->blockDepth = blockDepth + 1;
                    s->pl[s->blockDepth] = output;
                } else // Add this line of data to linked list of parserLines.
                {
                    ppl_parserStatAdd(s, s->blockDepth, output);
                }

                // See whether this line is the end of a datablock
                if (strncmp(line + *linepos, "END", 3) == 0) {
                    int i, endBlock = 1;
                    for (i = 3; line[*linepos + i] != '\0'; i++)
                        // Anything after END means that this line is data, not the end of the block
                        if ((unsigned char) line[*linepos + i] > ' ') {
                            endBlock = 0;
                            break;
                        }
                    // A line reading END ends one of the blocks of inline data which the command requires
                    if (endBlock) {
                        s->NinlineDatafiles[blockDepth]--;
                        first = 1;
                    }
                }

                // Return status 1 or 3 depending whether we're still looking for more data
                if (s->NinlineDatafiles[blockDepth] < 1) {
                    status = 1;
                    strcpy(s->prompt, "pyxplot");
                } else if (first) {
                    status = 3;
                    strcpy(s->prompt, "data>>>");
                } else {
                    status = 3;
                    strcpy(s->prompt, "data...");
                }
                *linepos += strlen(line + *linepos);
            }
            goto cleanup;
        }
        case PN_TYPE_CODEBLK: {
            // When only testing for a match, a code block matches if it opens with a {
            if ((s == NULL) || (!writeOut)) {
                // Skip over the {
                if (line[*linepos] == '{') {
                    status = (s != NULL);
                    (*linepos)++;
                } // Return fail when tab-completing, even if we've actually correctly matched a {; it won't make any useful suggestions hereafter
                else if ((s != NULL) &&
                         (s->eLlinePos <=
                          *linepos)) // Not tab completing, and we haven't matched a {: suggest { as an 'expecting' hint
                {
                    int j, k;
                    // If this point is further along the line than any previous failure, start a new list of what was
                    // expected here
                    if (s->eLlinePos < *linepos) {
                        s->eLlinePos = *linepos;
                        s->eLPos = 0;
                        s->expectingList[0] = '\0';
                    }
                    // Separate the alternatives in the list of what was expected
                    if (s->eLPos != 0) {
                        snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos, "%s", ", or ");
                        s->eLPos += strlen(s->expectingList + s->eLPos);
                    }
                    snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos, "\"{\"");
                    int l = strlen(s->expectingList + s->eLPos);
                    for (j = k = 0; ((j < s->eLPos) && !k); j++)
                        k = (strncmp(s->expectingList + j, s->expectingList + s->eLPos, l) ==
                             0); // Don't repeat ourselves
                    // Add "{" to the list of what was expected, unless it is already there; otherwise remove the
                    // separator just added
                    if (!k) {
                        s->eLPos += l;
                    } else {
                        if (s->eLPos != 0) { s->eLPos -= 5; }
                        s->expectingList[s->eLPos] = '\0';
                    }
                    status = 0;
                } else // We failed to match a {
                {
                    status = 0;
                }
            } else {
                char *cmd =
                        (char *) s->pl[blockDepth]->firstAtom->literal->auxil; // THIS ASSUMES DIRECTIVE IS FIRST ATOM
                const int first = (s->blockDepth < blockDepth + 1);

                // Test for opening brace
                if (first) {
                    s->waitingForBrace = 1;
                    s->outputPos[blockDepth] = s->pl[blockDepth]->stackOffset + node->outStackPos;
                    s->blockDepth = blockDepth + 1;
                    s->pl[s->blockDepth] = NULL;
                }
                // The opening { of the block may be on this line or the next
                if ((s->waitingForBrace) && (s->blockDepth == blockDepth + 1)) {
                    // Found the {: anything after it is the first line of the block, and if there is nothing, prompt
                    // for the next line
                    if (line[*linepos] == '{') {
                        s->waitingForBrace = 0;
                        (*linepos)++;
                        while ((line[*linepos] != '\0') && ((unsigned char) line[*linepos] <= ' '))
                            (*linepos)++; // FFW over spaces
                        if (line[*linepos] == '\0') goto codeblock_write_prompt;
                    } else if (line[*linepos] == '\0') {
                        goto codeblock_write_prompt;
                    } else {
                        status = 0;
                        // Something other than {: add "{" to the list of what was expected at this point
                        if ((s != NULL) && (s->eLlinePos <= *linepos)) {
                            int j, k;
                            // If this point is further along the line than any previous failure, start a new list of
                            // what was expected here
                            if (s->eLlinePos < *linepos) {
                                s->eLlinePos = *linepos;
                                s->eLPos = 0;
                                s->expectingList[0] = '\0';
                            }
                            // Separate the alternatives in the list of what was expected
                            if (s->eLPos != 0) {
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos, "%s",
                                         ", or ");
                                s->eLPos += strlen(s->expectingList + s->eLPos);
                            }
                            snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos, "\"{\"");
                            int l = strlen(s->expectingList + s->eLPos);
                            for (j = k = 0; ((j < s->eLPos) && !k); j++)
                                k = (strncmp(s->expectingList + j, s->expectingList + s->eLPos, l) ==
                                     0); // Don't repeat ourselves
                            // Add "{" to the list, unless it is already there; otherwise remove the separator just
                            // added
                            if (!k) {
                                s->eLPos += l;
                            } else {
                                if (s->eLPos != 0) { s->eLPos -= 5; }
                                s->expectingList[s->eLPos] = '\0';
                            }
                        }
                        goto cleanup;
                    }
                }

                // Test for closing brace
                if ((line[*linepos] == '}') && (s->blockDepth == blockDepth + 1)) {
                    status = 1;
                    (*linepos)++;
                    if (s->outputPos[blockDepth] >= 0) // check we've had at least one command
                    {
                        pplObj val;
                        val.refCount = 1;
                        pplObjBytecode(&val, 0, NULL);
                        ppl_parserAtomAdd(s->pl[blockDepth], s->outputPos[blockDepth], *linepos, "", NULL, &val);
                        s->outputPos[blockDepth] = -1;
                    }
                    strcpy(s->prompt, "pyxplot");
                    break;
                }

                // Compile line of text to bytecode
                ppl_parserCompile(c, s, srcLineN, srcId, srcFname, line + *linepos, 0, blockDepth + 1);
                // A syntax error within the block ends compilation
                if (c->errStat.status) {
                    status = 0;
                    goto cleanup;
                } // On error, stop
                if (s->blockDepth < blockDepth + 1) s->blockDepth = blockDepth + 1;

                // If s->outputPos is set, this is the first line of data
                if (s->outputPos[blockDepth] >= 0) {
                    pplObj val;
                    val.refCount = 1;
                    pplObjBytecode(&val, 0, (void *) s->pl[blockDepth + 1]);
                    ppl_parserAtomAdd(s->pl[blockDepth], s->outputPos[blockDepth], *linepos, "", NULL, &val);
                    s->outputPos[blockDepth] = -1;
                }

                // Write prompt
            codeblock_write_prompt:
                // If a command within the block is itself waiting for more lines, keep its prompt. Otherwise prompt
                // with the name of the command which opened this block, padded with > on its first line and .
                // thereafter.
                if (s->stk[blockDepth + 1][0] != NULL) {
                    status = 3;
                    goto cleanup;
                }
                if (strcmp(cmd, "do") == 0) strcpy(s->prompt, "do");
                else if (strcmp(cmd, "for") == 0) strcpy(s->prompt, "for");
                else if (strcmp(cmd, "foreach") == 0) strcpy(s->prompt, "foreac");
                else if (strcmp(cmd, "foreachdatum") == 0) strcpy(s->prompt, "foreac");
                else if (strcmp(cmd, "if") == 0) strcpy(s->prompt, "if");
                else if (strcmp(cmd, "subroutine") == 0) strcpy(s->prompt, "subrou");
                else if (strcmp(cmd, "while") == 0) strcpy(s->prompt, "while");
                else strcpy(s->prompt, "unknow");
                int i = strlen(s->prompt);
                for (; i < 7; i++) s->prompt[i] = first ? '>' : '.';
                s->prompt[i] = '\0';
                status = 3;
            }
            goto cleanup;
        }
        case PN_TYPE_ITEM: {
            int i;
            static int atNLastPos = -1; // Position of the last @n item matched
            if (*linepos < atNLastPos) atNLastPos = -1;

            // See if this item straddles the point from which we've been asked to make tab-completion suggestions; if so, return one.
            if ((tabCompTxt != NULL) && (tabCompStart <= *linepos)) {
                // Values, such as numbers and expressions, cannot be tab-completed, except filenames, which readline
                // completes
                if (node->matchString[0] == '%') {
                    if ((node->varName != NULL) &&
                        ((strcmp(node->varName, "filename") == 0) ||
                         (strcmp(node->varName, "directory") == 0))) { // Expecting a filename
                        // Readline asks for one completion at a time, numbered from zero; skip the earlier ones. A
                        // completion of "\n" tells the caller to complete a filename instead.
                        if ((*tabCompNo) != 0) {
                            status = 0;
                            (*tabCompNo)--;
                            goto item_cleanup;
                        }
                        status = 2;
                        strcpy(tabCompTxt, "\n");
                        (*tabCompNo)--;
                        goto item_cleanup;
                    }
                    goto finished_looking_for_tabcomp; // Expecting a float or string, which we don't tab complete... move along and look for something else
                }

                // A keyword can only be completed at the start of a word
                if ((*linepos > 0) && (isalnum(line[(*linepos) - 1])) && (atNLastPos != *linepos))
                    goto finished_looking_for_tabcomp; // 'plot a' cannot tab complete 'using' without space

                // Check that what has been typed so far matches the start of the keyword
                for (i = 0;
                     (((unsigned char) line[*linepos + i] > ' ') && ((unsigned char) node->matchString[i] > ' ')); i++)
                    // Matching is case-insensitive
                    if (toupper(line[*linepos + i]) != toupper(node->matchString[i])) {
                        status = 0;
                        goto item_cleanup; // We don't match the beginning of this string
                    }
                if ((node->acLevel == -1) && ((unsigned char) node->matchString[i] <= ' '))
                    goto finished_looking_for_tabcomp; // We've matched an @n string right to the end... move on
                // Skip completions earlier than the one requested; otherwise return this keyword
                if ((*tabCompNo) != 0) {
                    status = 0;
                    (*tabCompNo)--;
                    goto item_cleanup;
                }
                status = 2;
                // tabCompTxt is FNAME_LENGTH bytes long; the word being completed may be longer
                for (i = 0; (i < ((*linepos) - tabCompStart)) && (i < FNAME_LENGTH - 1); i++)
                    tabCompTxt[i] = line[tabCompStart + i];
                ppl_snprintfTrunc(tabCompTxt + i, FNAME_LENGTH - i, "%s",
                                  node->matchString); // Matchstring should match itself
                (*tabCompNo)--;
                goto item_cleanup;
            }
        finished_looking_for_tabcomp:

            // Treat items which are @n
            if (node->acLevel == -1) {
                int i;
                status = 1;
                for (i = 0; ((unsigned char) node->matchString[i] > ' '); i++)
                    // @n items must match their full text, which need not be followed by a space, e.g. the ( after a
                    // function name
                    if (toupper(line[*linepos + i]) != toupper(node->matchString[i])) {
                        status = 0;
                        break; // We don't match this string
                    }
                // Advance past the matched text, and store its output string unless the variable name is X, which means
                // it is not stored
                if (status == 1) {
                    *linepos += i; // We do match this string: advance by i spaces
                    atNLastPos = *linepos;
                    // Store the output string as a literal atom
                    if ((s != NULL) && writeOut && (strcmp(node->varName, "X") != 0)) {
                        pplObj val;
                        val.refCount = 1;
                        pplObjStr(&val, 0, 0, node->outString);
                        ppl_parserAtomAdd(s->pl[blockDepth], s->pl[blockDepth]->stackOffset + node->outStackPos,
                                          *linepos, "", NULL, &val);
                    }
                }
            }

            // Treat items which are not @n
            else if (node->matchString[0] != '%') // Items which do not begin % match themselves
            {
                const int i = ppl_strAutocomplete(line + *linepos, node->matchString, node->acLevel);
                // Advance past the matched keyword, which may be abbreviated, and store its output string unless the
                // variable name is X
                if (i < 0) {
                    status = 0;
                } else {
                    status = 1;
                    *linepos += i; // We match this string: advance by i spaces
                    atNLastPos = *linepos;
                    // Store the output string as a literal atom
                    if ((s != NULL) && writeOut && (strcmp(node->varName, "X") != 0)) {
                        pplObj val;
                        val.refCount = 1;
                        pplObjStr(&val, 0, 0, node->outString);
                        ppl_parserAtomAdd(s->pl[blockDepth], s->pl[blockDepth]->stackOffset + node->outStackPos,
                                          *linepos, "", NULL, &val);
                    }
                }
            } else // Item begins with a %, so substitute a value for this variable
            {
                pplExpr *expr;
                int explen = 0, errPos = -1, errType;
                const int dollarAllowed = ((node->matchString[1] == 'E') || (node->matchString[2] == 'E') ||
                                           (node->matchString[1] == 'F'));
                const int equalsAllowed = (node->matchString[1] != 'g');
                const int commaAllowed = (node->matchString[1] == 'F');
                const int vectorAllowed = (node->matchString[1] == 'P') || (node->matchString[1] == 'p');
                const int vectorLength = (node->matchString[1] == 'P') ? 3 : 2;
                int vectorCount = 0;
                int gotComma = 0;

                // Positions (%p and %P) may be given as comma-separated components, each of which is matched in a
                // further pass of this loop. Other types of value are matched in a single pass.
                do {
                    gotComma = 0;
                    // The letter after % gives the type of value expected, as listed at the top of
                    // buildScripts/parser_data.dat
                    switch (node->matchString[1]) {
                        case 's':
                        case 'S':
                        case 'r':
                        case 'v': {
                            if (node->matchString[1] == 'r') explen = strlen(line + *linepos);
                            // %s: a word made of letters and underscores
                            else if (node->matchString[1] == 's') {
                                for (explen = 0;
                                     ((isalpha(line[*linepos + explen])) || (line[*linepos + explen] == '_'));
                                     explen++);
                                // An empty word is not a match
                                if (explen == 0) {
                                    status = 0;
                                    goto item_cleanup;
                                }
                            } else if (node->matchString[1] == 'S') {
                                for (explen = 0;
                                     (((unsigned char) line[*linepos + explen] > ' ') &&
                                      (line[*linepos + explen] != '\'') && (line[*linepos + explen] != '\"'));
                                     explen++);
                                // An empty word is not a match
                                if (explen == 0) {
                                    status = 0;
                                    goto item_cleanup;
                                }
                            } else if (node->matchString[1] == 'v') {
                                // %v: a variable name
                                if (!isalpha(line[*linepos])) {
                                    status = 0;
                                    goto item_cleanup;
                                } // variable name must start with a letter
                                for (explen = 1;
                                     ((isalnum(line[*linepos + explen])) || (line[*linepos + explen] == '_'));
                                     explen++); // afterwards, numbers and _ allowed
                            }
                            // Store the matched text as a literal string
                            if ((s != NULL) && writeOut) {
                                char *str = strDup(line + *linepos, explen);
                                pplObj val;
                                // Failure to allocate memory abandons the match
                                if (str == NULL) {
                                    status = 0;
                                    goto item_cleanup;
                                }
                                val.refCount = 1;
                                pplObjStr(&val, 0, 1, str);
                                ppl_parserAtomAdd(s->pl[blockDepth], s->pl[blockDepth]->stackOffset + node->outStackPos,
                                                  *linepos, "", NULL, &val);
                            }
                            break;
                        }
                        case 'a': {
                            int xyz, anum = 1;
                            if ((line[*linepos] == 'x') || (line[*linepos] == 'X')) xyz = 0;
                            else if ((line[*linepos] == 'y') || (line[*linepos] == 'Y')) xyz = 1;
                            else if ((line[*linepos] == 'z') || (line[*linepos] == 'Z')) xyz = 2;
                            // %a: an axis name, which is x, y or z, optionally followed by a positive axis number, e.g.
                            // x2
                            else {
                                status = 0;
                                goto item_cleanup;
                            }
                            // Read the axis number, if any; it defaults to 1
                            if (ppl_validFloat(line + *linepos + 1, NULL)) {
                                double ad = ppl_getFloat(line + *linepos + 1, &explen);
                                // Axis numbers must be positive
                                if ((ad <= 0) || (ad >= INT_MAX)) {
                                    status = 0;
                                    goto item_cleanup;
                                }
                                anum = (int) floor(ad);
                                explen++;
                            } else {
                                explen = 1;
                            }
                            // Store the axis as a number, with the letter (0, 1 or 2 for x, y or z) in its first unit
                            // exponent
                            if ((s != NULL) && (writeOut)) {
                                char opt[8];
                                pplObj val;
                                val.refCount = 1;
                                pplObjNum(&val, 1, anum, 0);
                                val.exponent[0] = xyz;
                                ppl_snprintfTrunc(opt, sizeof(opt), "a%d", xyz);
                                ppl_parserAtomAdd(s->pl[blockDepth], s->pl[blockDepth]->stackOffset + node->outStackPos,
                                                  *linepos + 1, opt, NULL, &val);
                            }
                            break;
                        }
                        case 'c': {
                            char nambuff[64];
                            int i;
                            for (i = 0; ((i < 63) && ((isalnum(line[*linepos + i])) || (line[*linepos + i] == '(')));
                                 i++)
                                nambuff[i] = line[*linepos + i];
                            nambuff[i] = '\0';
                            int id = ppl_fetchSettingByName(&c->errcontext, nambuff, SW_COLOR_INT, SW_COLOR_STR);
                            // %c: a color, which may be the name of a color, or an expression which evaluates to a
                            // color
                            if (id > 0) {
                                pplObj val;
                                val.refCount = 1;
                                explen = i;
                                if ((s == NULL) || (!writeOut)) break;
                                pplObjColor(&val, 0, SW_COLSPACE_CMYK,
                                            *(double *) ppl_fetchSettingName(&c->errcontext, id, SW_COLOR_INT,
                                                                             (void *) SW_COLOR_CMYK_C, sizeof(double)),
                                            *(double *) ppl_fetchSettingName(&c->errcontext, id, SW_COLOR_INT,
                                                                             (void *) SW_COLOR_CMYK_M, sizeof(double)),
                                            *(double *) ppl_fetchSettingName(&c->errcontext, id, SW_COLOR_INT,
                                                                             (void *) SW_COLOR_CMYK_Y, sizeof(double)),
                                            *(double *) ppl_fetchSettingName(&c->errcontext, id, SW_COLOR_INT,
                                                                             (void *) SW_COLOR_CMYK_K, sizeof(double)));
                                val.exponent[2] = id;
                                ppl_parserAtomAdd(s->pl[blockDepth], s->pl[blockDepth]->stackOffset + node->outStackPos,
                                                  *linepos, "", NULL, &val);
                                break;
                            }
                            // If supplied string did not match a named color, treat it as an expression
                        }
                        default: {
                            ppl_expCompile(c, srcLineN, srcId, srcFname, line + *linepos, &explen, dollarAllowed,
                                           equalsAllowed, commaAllowed, &expr, &errPos, &errType, c->errStat.errBuff);
                            // The text could not be compiled as an expression
                            if (errPos >= 0) {
                                pplExpr_free(expr);
                                expr = NULL;
                                ppl_tbAdd(c, srcLineN, srcId, srcFname, 0, errType, errPos + *linepos, line, "");
                                c->algebraErrPos = errPos + *linepos;
                                status = 0;
                                goto item_cleanup;
                            }
                            // A position may continue with a comma and its next component
                            if (vectorAllowed && (vectorCount + 1 < vectorLength)) {
                                while ((line[*linepos + explen] > '\0') &&
                                       ((unsigned char) line[*linepos + explen] <= ' '))
                                    explen++; // ffw over whitespace
                                // Another component follows
                                if (line[*linepos + explen] == ',') {
                                    explen++;
                                    gotComma = 1;
                                } // vector with another comma-separated component
                            }
                            // When only testing for a match, the compiled expression is not needed. Expressions of most
                            // types are evaluated when the command is executed, and checked against the type letter.
                            if ((s == NULL) || (!writeOut)) {
                                pplExpr_free(expr);
                                expr = NULL;
                            } else if ((node->matchString[1] != 'e') && (node->matchString[1] != 'E') &&
                                       (node->matchString[1] != 'F') && (node->matchString[1] != 'g')) {
                                char *t = node->matchString + 1;
                                if (gotComma || (vectorCount > 0))
                                    t = "D"; // not a vector, rather comma separated distances
                                ppl_parserAtomAdd(s->pl[blockDepth],
                                                  s->pl[blockDepth]->stackOffset + node->outStackPos + vectorCount,
                                                  *linepos, t, expr, NULL);
                            } else {
                                // Expressions of types %e, %E, %F and %g are stored unevaluated, for the command to
                                // evaluate as it needs, e.g. once for each data point. A plot expression of '--'
                                // introduces a block of inline data, which must be counted.
                                int j, isInline = 1;
                                pplObj val;
                                val.refCount = 1;
                                if (!(strcmp(node->varName, "expression") == 0)) isInline = 0;
                                if (isInline && (strncmp(line + *linepos, "\"--\"", 4) != 0) &&
                                    (strncmp(line + *linepos, "'--'", 4) != 0))
                                    isInline = 0;
                                for (j = 4; j < explen; j++)
                                    if ((unsigned char) line[*linepos + j] > ' ') isInline = 0;
                                if (isInline) s->NinlineDatafiles[blockDepth]++;
                                pplObjExpression(&val, 0, (void *) expr);
                                ppl_parserAtomAdd(s->pl[blockDepth], s->pl[blockDepth]->stackOffset + node->outStackPos,
                                                  *linepos, "", NULL, &val);
                            }
                        } // default
                    } // switch
                    *linepos += explen;
                    if (gotComma) vectorCount++;
                } while (gotComma);
                status = 1;
            } // item beginning with %

            // If we're not running in tab-completion mode and have failed, return an "expecting" hint
        item_cleanup:
            // If the item failed to match, add what it expected to the list of what was expected at this point, for the
            // error message
            if ((s != NULL) && (status != 1) && (s->eLlinePos <= *linepos)) {
                int i, j, k, l;
                char varname[FNAME_LENGTH];
                // If this point is further along the line than any previous failure, start a new list of what was
                // expected here
                if (s->eLlinePos < *linepos) {
                    s->eLlinePos = *linepos;
                    s->eLPos = 0;
                    s->expectingList[0] = '\0';
                }
                if ((node->varName != NULL) && (node->varName[0] != '\0'))
                    ppl_snprintfTrunc(varname, sizeof(varname), " (%s)", node->varName);
                else varname[0] = '\0';
                // Add one description for a keyword, or one for each type letter after a %
                for (i = 1; 1; i++) {
                    // Separate the alternatives in the list of what was expected
                    if (s->eLPos != 0) {
                        snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos, "%s", ", or ");
                        s->eLPos += strlen(s->expectingList + s->eLPos);
                    }

                    // A keyword is described by itself, in quotes
                    if (node->matchString[0] != '%') {
                        snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos, "\"%s\"",
                                 node->matchString);
                    } else {
                        // All the type letters have been described: remove the trailing separator
                        if (node->matchString[i] == '\0') {
                            // Remove the five characters of the last ", or "
                            if (s->eLPos != 0) {
                                s->eLPos -= 5;
                                s->expectingList[s->eLPos] = '\0';
                            }
                            break;
                        }
                        // Describe each type of value in words
                        switch (node->matchString[i]) {
                            case 'a':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos,
                                         "an axis name%s", varname);
                                break;
                            case 'A':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos, "an angle%s",
                                         varname);
                                break;
                            case 'b':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos,
                                         "a boolean%s", varname);
                                break;
                            case 'c':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos, "a color%s",
                                         varname);
                                break;
                            case 'd':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos,
                                         "an integer%s", varname);
                                break;
                            case 'D':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos,
                                         "a distance%s", varname);
                                break;
                            case 'e':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos,
                                         "an algebraic expression%s", varname);
                                break;
                            case 'E':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos,
                                         "an algebraic expression%s", varname);
                                break;
                            case 'F':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos,
                                         "an algebraic expression%s", varname);
                                break;
                            case 'g':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos,
                                         "an algebraic expression%s", varname);
                                break;
                            case 'f':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos,
                                         "a real, dimensionless number%s", varname);
                                break;
                            case 'o':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos,
                                         "an expression%s", varname);
                                break;
                            case 'p':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos,
                                         "a position vector%s", varname);
                                break;
                            case 'P':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos,
                                         "a position vector%s", varname);
                                break;
                            case 'q':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos, "a string%s",
                                         varname);
                                break;
                            case 'r':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos,
                                         "a string of text%s", varname);
                                break;
                            case 's':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos,
                                         "an alphabetical word%s", varname);
                                break;
                            case 'S':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos, "a word%s",
                                         varname);
                                break;
                            case 'u':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos,
                                         "a physical quantity%s", varname);
                                break;
                            case 'v':
                                snprintf(s->expectingList + s->eLPos, sizeof(s->expectingList) - s->eLPos,
                                         "a variable name%s", varname);
                                break;
                        }
                    }
                    l = strlen(s->expectingList + s->eLPos);
                    for (j = k = 0; ((j < s->eLPos) && !k); j++)
                        k = (strncmp(s->expectingList + j, s->expectingList + s->eLPos, l) ==
                             0); // Don't repeat ourselves
                    // Keep the new description, unless it is already in the list; otherwise remove the separator just
                    // added
                    if (!k) {
                        s->eLPos += l;
                    } else {
                        if (s->eLPos != 0) { s->eLPos -= 5; }
                        s->expectingList[s->eLPos] = '\0';
                    }
                    if (node->matchString[0] != '%') break;
                }
            }
            goto cleanup;
        }
        case PN_TYPE_SEQ: {
            parserNode *nodeIter = node->firstChild;
            // A sequence matches if each of its children matches in turn
            while (nodeIter != NULL) {
                status = parse_descend(c, s, writeOut, srcLineN, srcId, srcFname, line, linepos, nodeIter, tabCompNo,
                                       tabCompStart, tabCompTxt, level + 1, blockDepth, match);
                if (status != 1) goto cleanup;
                nodeIter = nodeIter->nextSibling;
            }
            goto cleanup;
        }
        case PN_TYPE_REP:
        case PN_TYPE_REP2: {
            const int oldStackOffset = (s == NULL) ? 0 : s->pl[blockDepth]->stackOffset;
            if (s != NULL) s->oldStackOffset[blockDepth][level] = oldStackOffset;
            status = parse_repeat(c, s, writeOut, srcLineN, srcId, srcFname, line, linepos, node, tabCompNo,
                                  tabCompStart, tabCompTxt, level, blockDepth, match, 1, oldStackOffset);
            goto cleanup;
        }
        case PN_TYPE_OPT: {
            const int lineposOld = *linepos;
            const int matchOld = *match;
            parserNode *nodeIter = node->firstChild;
            int wo = 0;
            // A { } structure is optional: its children are first matched without writing output, and if they all
            // match, they are matched again writing output. If they do not, the structure is skipped.
            while (1) {
                // Match each child in turn
                while (nodeIter != NULL) {
                    status = parse_descend(c, s, wo, srcLineN, srcId, srcFname, line, linepos, nodeIter, tabCompNo,
                                           tabCompStart, tabCompTxt, level + 1, blockDepth, match);
                    // A child failed to match: skip the whole optional structure
                    if (status == 0) {
                        *linepos = lineposOld;
                        *match = matchOld;
                        status = 1;
                        goto cleanup;
                    }
                    if (status >= 2) goto cleanup;
                    nodeIter = nodeIter->nextSibling;
                }

                // The dry run succeeded, so do it again, for real
                if (wo == writeOut) goto cleanup;
                *linepos = lineposOld;
                *match = matchOld;
                nodeIter = node->firstChild;
                wo = writeOut;
            }
        }
        case PN_TYPE_PER: {
#define PER_MAXSIZE 64
            int repeating = 1;
            unsigned char excluded[PER_MAXSIZE];
            memset((void *) excluded, 0, PER_MAXSIZE);
            // A ( ) structure contains items which may appear in any order, each at most once. Repeatedly look for the
            // first item which matches, match it again writing output, and then exclude it from further passes, until
            // no more items match.
            while (repeating) {
                int count = 0;
                const int lineposOld = *linepos;
                const int matchOld = *match;
                parserNode *nodeIter = node->firstChild;
                int wo = 0, secondPass = 0;
                repeating = 0;
                // Try each item which has not already been matched
                while (nodeIter != NULL) {
                    // Skip items which have already been matched
                    if (excluded[count]) {
                        nodeIter = nodeIter->nextSibling;
                        count++;
                        continue;
                    }

                    // Match the item: first without writing output, and then, on the second pass, writing it
                    status = parse_descend(c, s, wo, srcLineN, srcId, srcFname, line, linepos, nodeIter, tabCompNo,
                                           tabCompStart, tabCompTxt, level + 1, blockDepth, match);
                    // After writing the item's output, exclude it, and look for another item
                    if (secondPass) {
                        excluded[count] = 1;
                        repeating = 1;
                        break;
                    }
                    if (status == 3) { status = 0; }
                    // The item does not match: try the next one
                    if (status == 0) {
                        *linepos = lineposOld;
                        *match = matchOld;
                        nodeIter = nodeIter->nextSibling;
                        count++;
                        continue;
                    }
                    if (status == 2) goto cleanup;
                    *linepos = lineposOld;
                    *match = matchOld;
                    wo = writeOut;
                    secondPass = 1;
                }
            }
            status = 1;
            goto cleanup;
        }
        case PN_TYPE_ORA: {
            const int lineposOld = *linepos;
            const int matchOld = *match;
            parserNode *nodeIter = node->firstChild;
            int wo = 0, secondPass = 0;
            // A < > structure matches the first of its alternatives which matches, which is matched first without
            // writing output, and then again writing output
            while (nodeIter != NULL) {
                status = parse_descend(c, s, wo, srcLineN, srcId, srcFname, line, linepos, nodeIter, tabCompNo,
                                       tabCompStart, tabCompTxt, level + 1, blockDepth, match);
                if (secondPass) goto cleanup;
                // An alternative which reaches the end of the line does not match
                if (status == 3) {
                    status = 0;
                    goto cleanup;
                }
                // This alternative does not match: try the next one
                if (status == 0) {
                    *linepos = lineposOld;
                    *match = matchOld;
                    nodeIter = nodeIter->nextSibling;
                    continue;
                }
                if (status == 2) goto cleanup;
                *linepos = lineposOld;
                *match = matchOld;
                wo = writeOut;
                secondPass = 1;
            }
            goto cleanup;
        }
        default: {
            ppl_snprintfTrunc(msg, sizeof(msg), "Hit an unexpected node type %d.", node->type);
            ppl_fatal(&c->errcontext, __FILE__, __LINE__, msg);
        }
    }

// Remember which node we are in if we are waiting for another line of input, so that matching can resume there;
// otherwise forget it
cleanup:
    // If we are not going back for another line in a CODEBLOCK, clear the parserNode stack
    if (s != NULL) {
        if (status != 3) s->stk[blockDepth][level] = NULL;
        else s->stk[blockDepth][level] = node;
    }

    // If we are at the top level of the hierarchy and we have failed to parse, throw a traceback now.
    // If we have succeeded in parsing, we should by now have consumed a whole line; if there is trailig text, that's an error
    if ((level == 0) && (status < 2) && (s != NULL)) {
        // The command must have used up the whole line, apart from trailing whitespace
        if (status == 1) {
            while ((line[*linepos] > '\0') && ((unsigned char) line[*linepos] <= ' ')) (*linepos)++;
            // Trailing text is a syntax error: add "the end of the command" to the list of what was expected
            if (line[*linepos] != '\0') {
                // Only describe the furthest point along the line reached by any attempt to match
                if (s->eLlinePos <= *linepos) {
                    // If this point is further along the line than any previous failure, start a new list of what was
                    // expected here
                    if (s->eLlinePos < *linepos) {
                        s->eLlinePos = *linepos;
                        s->eLPos = 0;
                        s->expectingList[0] = '\0';
                    }
                    // Separate the alternatives in the list of what was expected
                    if (s->eLPos > 0) {
                        snprintf(s->expectingList + s->eLPos, LSTR_LENGTH - s->eLPos, ", or ");
                        s->eLPos += strlen(s->expectingList + s->eLPos);
                    }
                    snprintf(s->expectingList + s->eLPos, LSTR_LENGTH - s->eLPos, "the end of the command");
                    s->eLPos += strlen(s->expectingList + s->eLPos);
                }
                status = 0;
            }
        }
        // Report a failure to match as a syntax error, listing what was expected at the furthest point reached
        if (status == 0) {
            ppl_snprintfTrunc(c->errStat.errBuff, LSTR_LENGTH, "At this point, was expecting %s.", s->expectingList);
            c->errStat.errBuff[LSTR_LENGTH - 1] = '\0';
            parseTbAdd(c, srcLineN, srcId, srcFname, ERR_SYNTAX, s->eLlinePos, line);
        }
    }

    // On success, clear any errors left behind by alternatives which failed to match. Unless we are waiting for another
    // line, reset the state of multi-line commands.
    if (status == 1) ppl_tbClear(c);
    if ((s != NULL) && (status != 3)) s->blockDepth = 0;
    if ((s != NULL) && (status != 3) && (level == 0)) s->NinlineDatafiles[blockDepth] = 0;
    //printf("%d %d %d %d\n",blockDepth,level,status,s->blockDepth);
    return status;
}

//! ppl_parserAtomFree - Free a linked list of atoms, including any expressions and literals they contain
//! \param [in|out] in - Pointer to the first atom in the list, which is set to NULL

void ppl_parserAtomFree(parserAtom **in) {
    if (in == NULL) return;
    parserAtom *i = *in;
    // Free each atom, with its literal value or expression
    while (i != NULL) {
        parserAtom *next = i->next;
        if (i->literal != NULL) ppl_garbageObject(i->literal);
        // Free the atom's compiled expression
        if (i->expr != NULL) {
            pplExpr_free(i->expr);
            i->expr = NULL;
        }
        free(i);
        i = next;
    }
    *in = NULL;
    return;
}

//! ppl_parserStatInit - Create a new parser status structure, which tracks the progress of compiling multi-line
//! blocks of commands
//! \param [out] in - Set to point to the new parser status, or NULL if memory allocation failed
//! \param [in] pl - Pointer to the variable which will hold the root compiled command line

void ppl_parserStatInit(parserStatus **in, parserLine **pl) {
    *in = (parserStatus *) malloc(sizeof(parserStatus));
    if (*in == NULL) return;
    *pl = NULL;
    (*in)->rootpl = pl;
    ppl_parserStatReInit(*in);
    return;
}

//! ppl_parserStatReInit - Reset a parser status structure, freeing any partially-compiled block of commands
//! \param [in|out] in - The parser status to reset

void ppl_parserStatReInit(parserStatus *in) {
    int i, j;
    parserLine **pl = in->rootpl;
    if (*pl != NULL) ppl_parserLineFree(*pl);
    *pl = NULL;
    for (i = 0; i < MAX_RECURSION_DEPTH; i++) in->pl[i] = NULL;
    for (i = 0; i < MAX_RECURSION_DEPTH; i++)
        for (j = 0; j < 16; j++) in->stk[i][j] = NULL;
    for (i = 0; i < MAX_RECURSION_DEPTH; i++) in->outputPos[i] = -1;
    strcpy(in->prompt, "pyxplot");
    in->blockDepth = 0;
    for (i = 0; i < MAX_RECURSION_DEPTH; i++) in->NinlineDatafiles[i] = 0;
    in->waitingForBrace = 0;
    in->eLPos = 0;
    in->eLlinePos = 0;
    in->expectingList[0] = '\0';
    return;
}

//! ppl_parserStatAdd - Append a compiled command line to the chain of command lines being compiled at a given depth
//! of nesting of code blocks
//! \param [in|out] in - The parser status
//! \param [in] bd - The depth of nesting of code blocks
//! \param [in] pl - The compiled command line to append

void ppl_parserStatAdd(parserStatus *in, const int bd, parserLine *pl) {
    // Append to the existing chain at this depth; or, if there is none, start one, which at depth zero is the root of
    // the whole compiled program
    if (in->pl[bd] != NULL) {
        pl->prev = in->pl[bd];
        in->pl[bd]->next = pl;
        in->pl[bd] = pl;
    } else {
        if (bd == 0) *(in->rootpl) = pl;
        pl->prev = NULL;
        in->pl[bd] = pl;
    }
    return;
}

//! ppl_parserStatFree - Free a parser status structure, including any partially-compiled block of commands
//! \param [in|out] in - Pointer to the parser status, which is set to NULL

void ppl_parserStatFree(parserStatus **in) {
    parserLine **pl = (*in)->rootpl;
    if (*pl != NULL) ppl_parserLineFree(*pl);
    *pl = NULL;
    free(*in);
    *in = NULL;
}

//! ppl_parserLineInit - Create a new, empty compiled command line
//! \param [out] in - Set to point to the new compiled command line, or NULL if memory allocation failed
//! \param [in] srcLineN - The line number of the source of the command, used to report errors
//! \param [in] srcId - The ID of the source of the command, used to report errors
//! \param [in] srcFname - The filename of the source of the command, used to report errors
//! \param [in] line - The text of the command, which is copied

void ppl_parserLineInit(parserLine **in, const int srcLineN, const long srcId, char *srcFname, char *line) {
    // The output is only returned once it has been fully initialised
    parserLine *output = NULL;
    *in = NULL;

    // Initialise output structure
    output = (parserLine *) malloc(sizeof(parserLine));
    if (output == NULL) return;
    output->linetxt = (char *) malloc(strlen(line) + 1);
    // Failure to allocate memory: free what has been allocated, and return NULL
    if (output->linetxt == NULL) {
        free(output);
        output = NULL;
        return;
    }
    strcpy(output->linetxt, line);
    output->srcFname = (char *) malloc(strlen(srcFname) + 1);
    // Failure to allocate memory: free what has been allocated, and return NULL
    if (output->srcFname == NULL) {
        free(output->linetxt);
        output->linetxt = NULL;
        free(output);
        output = NULL;
        return;
    }
    strcpy(output->srcFname, srcFname);
    output->firstAtom = NULL;
    output->lastAtom = NULL;
    output->next = NULL;
    output->prev = NULL;
    output->containsMacros = 0;
    output->refCount = 1;
    output->srcLineN = srcLineN;
    output->srcId = srcId;
    output->stackLen = 16;
    output->stackOffset = 0;

    *in = output;
    return;
}

//! ppl_parserLineFree - Free a chain of compiled command lines, including their atoms and any code blocks nested
//! within them
//! \param [in] in - The first compiled command line in the chain

void ppl_parserLineFree(parserLine *in) {
    parserLine *item = in;
    if (in == NULL) return;
    if (__sync_sub_and_fetch(&in->refCount, 1) > 0) return;
    // Free each line in the chain, with its atoms, text and filename
    while (item != NULL) {
        parserLine *next = item->next;
        ppl_parserAtomFree(&item->firstAtom);
        // Free the text of the line
        free(item->linetxt);
        item->linetxt = NULL;
        // Free the filename of the line's source
        free(item->srcFname);
        item->srcFname = NULL;
        free(item);
        item = next;
    }
    return;
}


//! quoteStateUpdate - Track whether a scan through a command line is inside a quoted string
//! \param [in,out] quoteChar - The quote character of the string currently open, or '\0' if outside any string
//! \param [in] line - The command line
//! \param [in] i - The position in <line> of the next character to scan
//! \return True if the character opened or closed a quoted string

static int quoteStateUpdate(char *quoteChar, const char *line, const int i) {
    const char ch = line[i];
    if ((ch != '\'') && (ch != '\"')) return 0;
    // Outside any string, a quote character opens a string
    if (*quoteChar == '\0') {
        *quoteChar = ch;
        return 1;
    }
    // Inside a string, the same quote character closes it, unless it is escaped with a backslash
    if ((*quoteChar == ch) && ((i == 0) || (line[i - 1] != '\\'))) {
        *quoteChar = '\0';
        return 1;
    }
    return 0;
}

//! lineContainsMacros - Test whether a command line contains macros to be substituted, i.e. `shell commands` or
//! @variable names, outside quoted strings
//! \param [in] line - The command line
//! \return True if the line contains macros

static int lineContainsMacros(const char *line) {
    char quoteChar = '\0';
    int i;
    // Look for ` and @ characters outside quoted strings
    for (i = 0; line[i] != '\0'; i++) {
        if (quoteStateUpdate(&quoteChar, line, i)) continue;
        if ((quoteChar == '\0') && ((line[i] == '`') || (line[i] == '@'))) return 1;
    }
    return 0;
}

//! appendCharGrowing - Append a character to a malloced buffer, enlarging the buffer as needed
//! \param [in] c - The Pyxplot context
//! \param [in,out] buf - Pointer to the buffer
//! \param [in,out] len - The size of the buffer
//! \param [in,out] pos - The position at which to append the character, which is advanced past it
//! \param [in] ch - The character to append
//! \return Zero on success, or nonzero if memory ran out (in which case an error message is in c->errStat.errBuff)

static int appendCharGrowing(ppl_context *c, char **buf, int *len, int *pos, const char ch) {
    // Grow the buffer when it is nearly full
    if (*pos > *len - 16) {
        char *grown = (char *) realloc(*buf, *len + LSTR_LENGTH);
        // Failure to allocate memory is reported as an error
        if (grown == NULL) {
            ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
            return 1;
        }
        *buf = grown;
        *len += LSTR_LENGTH;
    }
    (*buf)[(*pos)++] = ch;
    return 0;
}

//! ppl_parserCompile - Compile a command line into a sequence of atoms which can be executed by ppl_parserExecute().
//! Lines which open code blocks, e.g. for loops, are accumulated in the parser status until the block is closed.
//! \param [in] c - The Pyxplot context
//! \param [in|out] s - The parser status, which tracks the progress of compiling multi-line blocks of commands
//! \param [in] srcLineN - The line number of the source of the command, used to report errors
//! \param [in] srcId - The ID of the source of the command, used to report errors
//! \param [in] srcFname - The filename of the source of the command, used to report errors
//! \param [in] line - The command line to compile
//! \param [in] expandMacros - If true, expand macros and backtick substitutions now; otherwise, if the line contains
//! any, store it to be compiled when it is executed
//! \param [in] blockDepth - The depth of nesting of code blocks
//! \return Zero on success, or one if an error occurred, in which case <c->errStat> describes it

int ppl_parserCompile(ppl_context *c, parserStatus *s, const int srcLineN, const long srcId, char *srcFname, char *line,
                      const int expandMacros, const int blockDepth) {
    char msg[PPL_MSG_LENGTH];
    listIterator *cmdIter = NULL;
    int i, cln, containsMacros = 0;
    int obLen = LSTR_LENGTH, obPos;
    char *outbuff = NULL;

    // Forget the positions of any previous errors, and guard against runaway recursion
    c->algebraErrPos = c->errStat.oldErrPosExpr = c->errStat.oldErrPosCmd = -1;
    // Too many levels of nested blocks of code
    if (blockDepth > MAX_RECURSION_DEPTH) {
        strcpy(c->errStat.errBuff, "Maximum recursion depth exceeded.");
        parseTbAdd(c, srcLineN, srcId, srcFname, ERR_OVERFLOW, 0, line);
        ppl_parserStatReInit(s);
        return 1;
    }

    // Deal with macros and ` ` substitutions
    if (!expandMacros) {
        int fail = 0;
        containsMacros = lineContainsMacros(line);
        if (containsMacros && (s != NULL) &&
            !fail) // If we're not expanding macros at this stage, flag this parserLine as containing macros, and exit
        {
            const int bd = ppl_max(blockDepth, s->blockDepth);
            // A macro cannot be stored for later on a line which also opens a block of code, since the line must be
            // parsed now to find the {
            if (s->waitingForBrace) {
                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                  "Cannot process a macro on the same line as the opening brace of a loop.");
                parseTbAdd(c, srcLineN, srcId, srcFname, ERR_SYNTAX, 0, line);
                ppl_parserStatReInit(s);
                return 1;
            }
            parserLine *output = NULL;
            if ((s->stk[blockDepth][0] == NULL) && (blockDepth == 0)) ppl_parserStatReInit(s);
            ppl_parserLineInit(&output, srcLineN, srcId, srcFname, line);
            // Failure to allocate memory is reported as an error
            if (output == NULL) {
                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
                parseTbAdd(c, srcLineN, srcId, srcFname, ERR_MEMORY, 0, line);
                ppl_parserStatReInit(s);
                return 1;
            }
            output->stackLen = 0;
            output->containsMacros = 1;
            ppl_parserStatAdd(s, bd, output);
            // If this is the first line inside a block of code, link the block into the command which opened it
            if ((bd > 0) && (s->outputPos[bd - 1] >= 0)) {
                pplObj val;
                val.refCount = 1;
                pplObjBytecode(&val, 0, (void *) s->pl[bd]);
                ppl_parserAtomAdd(s->pl[bd - 1], s->outputPos[bd - 1], 0, "", NULL, &val);
                s->outputPos[bd - 1] = -1;
            }
            return 0;
        }
    } else {
        char *lineOriginal = line;
        int l = 0, fail = 0;
        ;
        for (l = 0; (l < 16) && !fail; l++) // Repeatedly test for and substitute macros; nested macros are allowed
        {
            containsMacros = lineContainsMacros(line);
            if (fail || !containsMacros) break;
            {
                outbuff = (char *) malloc(obLen);
                obPos = 0;
                // Copy the line into outbuff, substituting for each macro
                if (outbuff != NULL) {
                    char quoteChar = '\0';
                    int i;
                    // Scan the line one character at a time
                    for (i = 0; line[i] != '\0'; i++) {
                        // Quotation marks are copied to the output as they are
                        if (quoteStateUpdate(&quoteChar, line, i)) {
                            // Copy the quote character
                            if (appendCharGrowing(c, &outbuff, &obLen, &obPos, line[i])) {
                                fail = 1;
                                break;
                            }
                            continue;
                        }

                        // First, substitute for ` ` expressions
                        if ((quoteChar == '\0') && (line[i] == '`')) {
                            int is = ++i;
                            char *key = NULL;
                            FILE *substPipe;
                            for (; ((line[i] != '\0') && (line[i] != '`')); i++); // Find end of ` ` expression
                            // An unclosed ` is an error
                            if (line[i] != '`') {
                                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Mismatched `");
                                fail = 1;
                                break;
                            }
                            key = (char *) malloc(i - is + 1); // Put macro name into a string
                            // Failure to allocate memory is reported as an error
                            if (key == NULL) {
                                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
                                fail = 1;
                                break;
                            }
                            strncpy(key, line + is, i - is);
                            key[i - is] = '\0';
                            // Log the shell command
                            if (DEBUG) {
                                ppl_snprintfTrunc(msg, sizeof(msg), "Shell substitution with command '%s'.", key);
                                ppl_log(&c->errcontext, msg);
                            }
                            // Run the shell command
                            if ((substPipe = popen(key, "r")) == NULL) {
                                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                                  "Could not spawl shell substitution command '%s'.", key);
                                fail = 1;
                                goto shellSubstErr;
                            }
                            // Copy the command's output into the line, one character at a time, replacing newlines with
                            // spaces
                            while ((!feof(substPipe)) && (!ferror(substPipe))) {
                                if (fscanf(substPipe, "%c", outbuff + obPos) == EOF) break;
                                if (outbuff[obPos] == '\n') outbuff[obPos] = ' ';
                                if (outbuff[obPos] != '\0') obPos++;
                                // Grow the output buffer when it is nearly full
                                if (obPos > obLen - 16) {
                                    char *grown = (char *) realloc(outbuff, obLen + LSTR_LENGTH);
                                    // Failure to allocate memory is reported as an error; the old buffer remains valid
                                    if (grown == NULL) {
                                        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                                          "Out of memory.");
                                        fail = 1;
                                        break;
                                    }
                                    outbuff = grown;
                                    obLen += LSTR_LENGTH;
                                }
                            }
                            int status = pclose(substPipe);
                            if (fail) goto shellSubstErr;

                            // A shell command which fails is an error
                            if (WIFEXITED(status)) {
                                int ec = WEXITSTATUS(status);
                                // A non-zero exit status is an error
                                if (ec) {
                                    ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                                      "Command failure during ` ` substitution (exit code %d).", ec);
                                    fail = 1;
                                    goto shellSubstErr;
                                }
                            } else if (WIFSIGNALED(status)) {
                                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                                  "Command failure during ` ` substitution (terminated by signal %d).",
                                                  WTERMSIG(status));
                                fail = 1;
                                goto shellSubstErr;
                            } else {
                                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                                  "Command failure during ` ` substitution (fail happened).");
                                fail = 1;
                                goto shellSubstErr;
                            }

                        // Free the command, and stop if it failed
                        shellSubstErr:
                            // Free the text of the shell command
                            free(key);
                            key = NULL;
                            if (fail) break;
                        }

                        // Second, substitute for macros
                        else if ((quoteChar == '\0') && (line[i] == '@')) {
                            int is = ++i, j, got = 0;
                            char *key = NULL;
                            for (; (isalnum(line[i]) || (line[i] == '_')); i++); // Find end of macro name
                            key = (char *) malloc(i - is + 1); // Put macro name into a string
                            // Failure to allocate memory is reported as an error
                            if (key == NULL) {
                                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
                                fail = 1;
                                break;
                            }
                            strncpy(key, line + is, i - is);
                            key[i - is] = '\0';
                            // Look up the macro's name as a variable: first in the local namespace, and then in the
                            // global one. Its value must be a string, which is copied into the line.
                            for (j = 1; j >= 0; j--) {
                                pplObj *obj = (pplObj *) ppl_dictLookup(c->namespaces[j], key);
                                if (obj == NULL) continue;
                                if ((obj->objType == PPLOBJ_GLOB) || (obj->objType == PPLOBJ_ZOM)) continue;
                                // Only string variables can be expanded as macros
                                if (obj->objType != PPLOBJ_STR) {
                                    ppl_snprintfTrunc(
                                            c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                            "Attempt to expand a macro, \"%s\", which is not a string variable.", key);
                                    got = -1;
                                    break;
                                }
                                // Grow the output buffer if the string will not fit
                                if (obPos + strlen((char *) obj->auxil) > obLen - 16) {
                                    const int newLen = obLen + strlen((char *) obj->auxil) + LSTR_LENGTH;
                                    char *grown = (char *) realloc(outbuff, newLen);
                                    // Failure to allocate memory abandons the substitution; the old buffer remains
                                    // valid
                                    if (grown == NULL) {
                                        ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                                          "Out of memory.");
                                        got = -2;
                                        break;
                                    }
                                    outbuff = grown;
                                    obLen = newLen;
                                }
                                strcpy(outbuff + obPos, (char *) obj->auxil);
                                obPos += strlen(outbuff + obPos);
                                got = 1;
                                break;
                            }
                            // The variable was not found
                            if (got == 0) {
                                ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff),
                                                  "Undefined macro, \"%s\".", key);
                            }
                            free(key);
                            key = NULL;
                            // Stop on failure; otherwise step back, since the loop will advance past the character
                            // after the macro name
                            if (got <= 0) {
                                fail = 1;
                                break;
                            }
                            i--;
                        } else if (appendCharGrowing(c, &outbuff, &obLen, &obPos, line[i])) {
                            // Any other character is copied to the output
                            fail = 1;
                            break;
                        }
                    } // Loop over characters
                    outbuff[obPos++] = '\0';
                } else {
                    // Failure to allocate memory is reported as an error
                    ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Out of memory.");
                    fail = 1;
                }

                // Clean up after macro substitution
                if ((!fail) && (outbuff != NULL)) {
                    // The substituted line replaces the line from any previous round of substitution
                    if (line != lineOriginal) {
                        free(line);
                        line = NULL;
                    }
                    line = outbuff;
                } else if (outbuff != NULL) {
                    free(outbuff);
                    outbuff = NULL;
                }
            }
        }
        // Report a failed substitution as a syntax error
        if (fail) {
            parseTbAdd(c, srcLineN, srcId, srcFname, ERR_SYNTAX, 0, line);
            ppl_parserStatReInit(s);
            return 1;
        }
    }

    // If we are returning to add more code into a codeblock, do that now
    if (s->stk[blockDepth][0] != NULL) {
        int match = 0, linepos = 0;
        s->eLPos = s->eLlinePos = 0;
        s->expectingList[0] = '\0';
        parse_descend(c, s, 1, srcLineN, srcId, srcFname, line, &linepos, NULL, NULL, -1, NULL, 0, blockDepth, &match);
        // On error, abandon the partially-compiled block of code
        if (c->errStat.status) {
            // Free the line after macro substitution
            free(outbuff);
            outbuff = NULL;
            ppl_parserStatReInit(s);
            return 1;
        }
        return 0;
    }

    // If we're not adding more code to a codeBlock, and this is a top-level call, clean the stack
    if (blockDepth == 0) ppl_parserStatReInit(s);

    // Fetch first non-whitespace character of command string
    for (i = 0; ((line[i] >= '\0') && ((unsigned char) line[i] <= ' ')); i++);
    if ((line[i] >= 'a') && (line[i] <= 'z')) cln = (int) (line[i] - 'a');
    else if ((line[i] >= 'A') && (line[i] <= 'Z')) cln = (int) (line[i] - 'A');
    else cln = 26;

    // Cycle through possible command matches
    cmdIter = ppl_listIterateInit(pplParserCmdList[cln]);
    // Try each command whose definition begins with this letter, followed by each command which begins with
    // punctuation, until one matches
    while (1) {
        int match = 0, linepos = 0;

        // Move on to the list of commands which begin with punctuation
        if (cmdIter ==
            NULL) // Once we've finished cycling through commands that start with punctuation, cycle through ones that start with first letter
        {
            if (cln == 26) break;
            cmdIter = ppl_listIterateInit(pplParserCmdList[cln = 26]);
            if (cmdIter == NULL) break;
        }

        // Fetch next item from list of possible commands and try to parse it
        parserNode *item = (parserNode *) ppl_listIterate(&cmdIter);
        match = linepos = 0;
        parse_descend(c, s, 1, srcLineN, srcId, srcFname, line, &linepos, item, NULL, -1, NULL, 0, blockDepth, &match);

        // If this command did not match as far as the '=' token, clear stack and traceback
        if (match == 0) {
            parserLine *new = s->pl[blockDepth];
            parserLine *old = new->prev;
            ppl_tbClear(c);
            c->errStat.oldErrPosExpr = c->errStat.oldErrPosCmd = c->algebraErrPos = -1;
            ppl_parserLineFree(new);
            s->pl[blockDepth] = old;
            if (old != NULL) old->next = NULL;
            if ((blockDepth == 0) && (old == NULL)) *(s->rootpl) = NULL;
            s->eLPos = s->eLlinePos = 0;
            s->expectingList[0] = '\0';
            continue;
        }

        // If we did match as far as '=', but got an error traceback, free the stack and return to the user
        if (c->errStat.status) {
            // Free the line after macro substitution
            free(outbuff);
            outbuff = NULL;
            ppl_parserStatReInit(s);
            return 1;
        }

        // Success!
        free(outbuff);
        outbuff = NULL;
        return 0;
    }

    // We have not found a match to this command
    ppl_snprintfTrunc(c->errStat.errBuff, sizeof(c->errStat.errBuff), "Unrecognised command.");
    parseTbAdd(c, srcLineN, srcId, srcFname, ERR_SYNTAX, 0, line);
    // Free the line after macro substitution
    free(outbuff);
    outbuff = NULL;
    ppl_parserStatReInit(s);
    return 1;
}

#ifdef HAVE_READLINE
static ppl_context *rootContext = NULL;

//! ppl_parseAutocompleteSetContext - Set the Pyxplot context used by readline's tab completion callbacks
//! \param [in] c - The Pyxplot context

void ppl_parseAutocompleteSetContext(ppl_context *c) {
    rootContext = c;
    return;
}

//! ppl_parseAutocomplete - Readline generator function, which returns successive tab completions for the command
//! line being typed
//! \param [in] dummy - The word being completed (unused; the whole line is read from readline's buffer)
//! \param [in] status - Zero or more to return the Nth completion; a negative value -1-N to reset the generator to
//! complete the word starting at position N of the line
//! \return The next completion, in a newly malloced string, or NULL when there are no more

char *ppl_parseAutocomplete(const char *dummy, const int status) {
    // State kept between calls: the number of the next completion to return, the position of the word being completed,
    // and the line being completed
    static int matchNum = 0, start = 0;
    static char *linebuff = NULL, *line = NULL;
    char *output = NULL;

    // We are called once with negative status to set up static varaibles, before readline calls us with status>=0
    if (status < 0) {
        start = -status - 1;
        matchNum = -1;
        // Free any buffer left over from the previous completion
        if (line != NULL) {
            free(linebuff);
            linebuff = NULL;
        }
        if (rootContext->inputLineAddBuffer ==
            NULL) // If we're on the second line of a continued line, add InputLineAddBuffer to beginning of line
        {
            line = rl_line_buffer;
        } else {
            int i = strlen(rl_line_buffer) + strlen(rootContext->inputLineAddBuffer);
            if ((linebuff = (char *) malloc(i + 1)) == NULL) return NULL;
            strcpy(linebuff, rootContext->inputLineAddBuffer);
            strcpy(linebuff + strlen(linebuff), rl_line_buffer);
            linebuff[i] = '\0';
            line = linebuff;
            start += strlen(rootContext->inputLineAddBuffer);
        }
    }

    // Look for possible tab completions. Do this in a while loop, since if we get a %s filename completion, we search again
    while (1) {
        int cln = 0;
        listIterator *cmdIter = NULL;
        int number = matchNum++;
        char tabCompTxt[FNAME_LENGTH] = "\0";
        if (number < 0) number = 0; // Return first item twice

        // Loop over all of the possible commands that could be intended
        while (1) {
            int match = 0, linepos = 0;

            if (cmdIter == NULL) // Once we've finished cycling through each list, move onto the next one
            {
                if (cln == 27) break;
                cmdIter = ppl_listIterateInit(pplParserCmdList[cln++]);
                if (cmdIter == NULL) continue;
            }

            // Fetch next item from list of possible commands and try to parse it
            parserNode *item = (parserNode *) ppl_listIterate(&cmdIter);
            match = linepos = 0;
            parse_descend(rootContext, NULL, 0, -1, -1, "", line, &linepos, item, &number, start, tabCompTxt, 0, 0,
                          &match);

            if (tabCompTxt[0] == '\n') // Special case: use Readline's filename tab completion
            {
                if (status <
                    0) // On initial pass through, it's not too late to tell wrapper to use readline's own completer
                {
                    if ((output = (char *) malloc(strlen(tabCompTxt) + 1)) == NULL) return NULL;
                    strcpy(output, tabCompTxt);
                    return output;
                } else { // On subsequent pass-throughs, it's too late...
                    break;
                }
            }
            if (tabCompTxt[0] != '\0') // We have a new completion option; do not iterate through more commands
            {
                if ((output = (char *) malloc(strlen(tabCompTxt) + 1)) == NULL) return NULL;
                strcpy(output, tabCompTxt);
                return output;
            }
        }
        if (tabCompTxt[0] == '\n')
            continue; // We've been asked to match filenames, but have already made other Pyxplot syntax suggestions... skip on and return next match
        else break;
    }
    return NULL; // Tell readline we have no more matches to offer
}

//! ppl_rl_completion - Readline attempted-completion callback, which offers completions of Pyxplot command words,
//! falling back to filename completion within quoted strings
//! \param [in] text - The word being completed
//! \param [in] start - The position of the start of the word within the line
//! \param [in] end - The position of the end of the word within the line
//! \return Array of completions, or NULL to request readline's default filename completion

char **ppl_rl_completion(const char *text, const int start, const int end) {

    // Words inside quoted strings are filenames
    if ((start > 0) && ((rl_line_buffer[start - 1] == '\"') || (rl_line_buffer[start - 1] == '\'')))
        return NULL; // Do filename completion

    char *firstItem = ppl_parseAutocomplete(text, -1 - start); // Setup parse_autocomplete to know what string it's working on

    // A first completion of "\n" means that a filename is expected here
    if (firstItem != NULL) {
        if (firstItem[0] == '\n') // We are trying to match a %s:filename field, so turn on filename completion
        {
            free(firstItem);
            firstItem = NULL;
            rl_attempted_completion_over = 1; // NULL means that readline's default filename completer is activated
            return NULL;
        } else {
            free(firstItem);
            firstItem = NULL;
        }
    }

    // Gather all the completions from ppl_parseAutocomplete()
    char **matches = rl_completion_matches(text, ppl_parseAutocomplete);
    rl_attempted_completion_over = 1; // Make sure that filenames are not completion options
    return matches;
}
#endif
