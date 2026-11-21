// parserInit.c
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

// Builds the parser's model of Pyxplot's command syntax when Pyxplot starts. The grammar of every command, generated
// into parser/cmdList.c from buildScripts/parser_data.dat, is read into a tree of parserNode structures for each
// command, which parserCompile.c then matches each line of input against.

#define _PARSERINIT_C 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coreUtils/errorReport.h"
#include "coreUtils/list.h"

#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"

#include "parser/cmdList.h"
#include "parser/parser.h"

#include "userspace/context.h"

// The syntax trees of all commands, sorted into 27 lists by the first letter of the command: one list for each of the
// letters a-z, and one for commands which begin with punctuation, such as ! and ?
list *pplParserCmdList[27];

//! ppl_parserStartNode - Open a new node in the syntax tree being built from the command list, and push it onto the
//! stack of open nodes
//! \param [in] c - The error-reporting context
//! \param [in|out] stk - The stack of open nodes
//! \param [in|out] i - The number of nodes on the stack
//! \param [in] type - The type of the new node, one of the PN_TYPE_* constants

static void ppl_parserStartNode(pplerr_context *c, parserNode **stk, int *i, const int type) {
    // Create the new node, with no children and no storage information yet
    parserNode *newNode = (parserNode *) malloc(sizeof(parserNode));
    // Failure to allocate memory is fatal
    if (newNode == NULL) {
        ppl_fatal(c, __FILE__, __LINE__, "Out of memory.");
        exit(1);
    }
    newNode->type = type;
    newNode->listLen = -1;
    newNode->acLevel = -1;
    newNode->outStackPos = -1;
    newNode->matchString = NULL;
    newNode->outString = NULL;
    newNode->varName = NULL;
    newNode->firstChild = newNode->nextSibling = NULL;

    // If this node is not the root, link it into the tree as the last child of the node at the top of the stack. Items
    // must sit inside a sequence (SEQ) node, so one is opened implicitly if the parent is, e.g., a list of
    // alternatives.
    if ((*i) >
        0) // If this new node is not the root context, check whether we need to insert an implicit sequence between *list* *seq* STUFF stuff stuff *endseq* *endlist*
    {
        parserNode **target = NULL;
        if ((stk[(*i) - 1]->type != PN_TYPE_SEQ) && (type != PN_TYPE_SEQ)) ppl_parserStartNode(c, stk, i, PN_TYPE_SEQ);
        target = &(stk[(*i) - 1]->firstChild); // Pointer to where first child of parent is linked
        while (*target != NULL)
            target = &((*target)->nextSibling); // If not first child of parent, traverse linked list to end
        *target =
                newNode; // Add new node into hierarchy, either as first child of parent, or into linked list of siblings
    }
    stk[(*i)++] = newNode; // Add new node structure to context stack
    return;
}

//! ppl_parserRollBack - Close the node at the top of the stack of open nodes in the syntax tree being built, reading
//! any storage information which follows its closing bracket in the command list. A fatal error is raised if the
//! brackets in the command list are incorrectly nested.
//! \param [in] c - The error-reporting context
//! \param [in|out] stk - The stack of open nodes
//! \param [in|out] i - The number of nodes on the stack
//! \param [in] type - The type of node being closed, one of the PN_TYPE_* constants
//! \param [in] cmdText - The text of the command list, starting at the closing bracket
//! \param [out] NcharsTaken - Set to the number of characters of <cmdText> which were read

static void ppl_parserRollBack(pplerr_context *c, parserNode **stk, int *i, int type, const char *cmdText,
                               int *NcharsTaken) {
    char msg[PPL_MSG_LENGTH];
    int j = 1; // Count characters taken; start at 1 for closing bracket
    int varNameStart = -1, varNameEnd = -1;

    // Close any implicit SEQ nodes, which have no closing bracket of their own
    while (((*i) > 0) && (stk[(*i) - 1]->type == PN_TYPE_SEQ))
        (*i)--; // Automatically close SEQs; they don't have closing grammar

    // Check that the bracket being closed matches the node which is open
    if ((*i) <= 0) {
        ppl_snprintfTrunc(
                msg, sizeof(msg),
                "Incorrect nesting of types in command specification -- attempt to close an unopened bracket.");
        ppl_fatal(c, __FILE__, __LINE__, msg);
    } else if (stk[(*i) - 1]->type != type) {
        ppl_snprintfTrunc(msg, sizeof(msg),
                          "Incorrect nesting of types in command specification -- expected %d, but got %d.", type,
                          stk[(*i) - 1]->type);
        ppl_fatal(c, __FILE__, __LINE__, msg);
    }

    // A closing ] may be followed by storage information, ]@varname@value@ACL@stackpos@listlen, saying where the
    // repeated items are stored
    if (cmdText[j] == '@') {
        // Only [ ] structures can carry storage information
        if (type != PN_TYPE_REP) {
            ppl_snprintfTrunc(msg, sizeof(msg), "Unexpected storage information after non [ ] data structure.");
            ppl_fatal(c, __FILE__, __LINE__, msg);
        }
        j++;
        // If the variable name begins with '0', zero repeats are allowed
        if (cmdText[j] == '0') {
            type = PN_TYPE_REP2;
            stk[(*i) - 1]->type = type;
            j++;
        } // If storage variable name begins '0', zero repeats are allowed; a REP2 item, not a REP item
        // Read the name of the variable in which the list of repeats is stored
        if (((unsigned char) cmdText[j] > ' ') && (cmdText[j] != '@')) {
            varNameStart = j;
            while (((unsigned char) cmdText[j] > ' ') && (cmdText[j] != '@')) j++; // FFW over variable name
            varNameEnd = j;
        }
        // Skip over the value field, which is unused for repeats
        if (cmdText[j++] != '@') {
            ppl_snprintfTrunc(msg, sizeof(msg), "Syntax error: expecting @ after ]@varname...");
            ppl_fatal(c, __FILE__, __LINE__, msg);
        }
        while (((unsigned char) cmdText[j] > ' ') && (cmdText[j] != '@')) j++;
        // Skip over the autocomplete level, which is unused for repeats
        if (cmdText[j++] != '@') {
            ppl_snprintfTrunc(msg, sizeof(msg), "Syntax error: expecting @ after ]@varname@value...");
            ppl_fatal(c, __FILE__, __LINE__, msg);
        }
        while (((unsigned char) cmdText[j] > ' ') && (cmdText[j] != '@')) j++;
        // Read the position on the output stack where the list of repeats is stored
        if (cmdText[j++] != '@') {
            ppl_snprintfTrunc(msg, sizeof(msg), "Syntax error: expecting @ after ]@varname@value@ACL...");
            ppl_fatal(c, __FILE__, __LINE__, msg);
        }
        int k = (int) ppl_getFloat(cmdText + j, NULL);
        while (((unsigned char) cmdText[j] > ' ') && (cmdText[j] != '@')) j++;
        stk[(*i) - 1]->outStackPos = k;
        // Read the length of the stack used by each repeated item
        if (cmdText[j++] != '@') {
            ppl_snprintfTrunc(msg, sizeof(msg), "Syntax error: expecting @ after ]@varname@value@ACL@stackpos...");
            ppl_fatal(c, __FILE__, __LINE__, msg);
        }
        int l = (int) ppl_getFloat(cmdText + j, NULL);
        while (((unsigned char) cmdText[j] > ' ') && (cmdText[j] != '@')) j++;
        stk[(*i) - 1]->listLen = l;
        while ((cmdText[j] != '\0') && (cmdText[j] != '\n') && ((unsigned char) cmdText[j] < ' '))
            j++; // FFW over trailing whitespace

        // Store a copy of the variable name in the node
        if (varNameStart > -1) {
            stk[(*i) - 1]->varName = (char *) malloc((varNameEnd - varNameStart + 1) * sizeof(char));
            // Failure to allocate memory is fatal
            if (stk[(*i) - 1]->varName == NULL) {
                ppl_fatal(c, __FILE__, __LINE__, "Out of memory whilst setting up Pyxplot's command line parser.");
                exit(1);
            }
            strncpy(stk[(*i) - 1]->varName, cmdText + varNameStart, varNameEnd - varNameStart);
            stk[(*i) - 1]->varName[varNameEnd - varNameStart] = '\0';
        }
    }
    (*i)--; // Close structure of type "type" and roll back stack.
    *NcharsTaken = j;
    return;
}

//! ppl_parserInit - Build the tree describing the syntax of each of Pyxplot's commands, from the command list
//! generated from buildScripts/parser_data.dat
//! \param [in] c - The Pyxplot context
//! \return Zero

int ppl_parserInit(ppl_context *c) {
    // inPos is the position reached in the text of the command list
    char msg[PPL_MSG_LENGTH];
    pplerr_context *e = &c->errcontext;
    int i;
    int inPos = 0;

    // Create an empty list of commands for each initial letter
    for (i = 0; i < 27; i++) {
        pplParserCmdList[i] = ppl_listInit(1);
        // Failure to allocate memory is fatal
        if (pplParserCmdList[i] == NULL) {
            ppl_fatal(e, __FILE__, __LINE__, "Out of memory.");
            exit(1);
        }
    }

    // Loop over instructions in command list
    while (1) {
        int stackPos = 0, cmdLetter, nc = 0;
        parserNode *defnStack[16];

        // Each command's definition is on one line, which begins with the length of the stack of variables which the
        // command produces. The first letter of the command decides which list it is stored in.
        while ((ppl_cmdList[inPos] != '\0') && ((unsigned char) ppl_cmdList[inPos] <= ' '))
            inPos++; // Ignore whitespace
        int rootStackLen =
                (int) ppl_getFloat(ppl_cmdList + inPos,
                                   &nc); // Each line of command list begins with the length of the root stack namespace
        inPos += nc;
        while ((ppl_cmdList[inPos] != '\0') && ((unsigned char) ppl_cmdList[inPos] <= ' '))
            inPos++; // Ignore whitespace
        if (ppl_cmdList[inPos] == '\0') break; // End of cmdList -- all instructions have now been read
        if ((ppl_cmdList[inPos] >= '@') && (ppl_cmdList[inPos + 1] >= 'a') && (ppl_cmdList[inPos + 1] <= 'z'))
            cmdLetter = (int) (ppl_cmdList[inPos + 1] - 'a');
        else cmdLetter = 26; // Begins with punctuation
        ppl_parserStartNode(e, defnStack, &stackPos, PN_TYPE_SEQ);
        defnStack[0]->listLen = rootStackLen;

        // Loop over words in instruction definition
        while (1) {
            while ((ppl_cmdList[inPos] != '\0') && (ppl_cmdList[inPos] != '\n') &&
                   ((unsigned char) ppl_cmdList[inPos] <= ' '))
                inPos++; // Ignore whitespace
            if ((ppl_cmdList[inPos] == '\0') || (ppl_cmdList[inPos] == '\n')) break; // Newline means end of instruction

            // Punctuation which descends into a new hierarchical data structure
            if (ppl_cmdList[inPos] ==
                '{') // {} grammar indicates an optional series of items, which go into a new structure.
            {
                ppl_parserStartNode(e, defnStack, &stackPos, PN_TYPE_OPT);
                inPos++;
            } else if (
                    ppl_cmdList[inPos] ==
                    '[') // [] grammar indicates a series of items which repeat 0 or more times. These go into a new structure.
            {
                ppl_parserStartNode(e, defnStack, &stackPos, PN_TYPE_REP);
                inPos++;
            } else if (
                    ppl_cmdList[inPos] ==
                    '(') // () grammar indicates items which can appear in any order, but each one not more than once. New structure.
            {
                ppl_parserStartNode(e, defnStack, &stackPos, PN_TYPE_PER);
                inPos++;
            } else if (ppl_cmdList[inPos] ==
                       '<') // <> grammar indicates a list of items of which only one should be matched. New structure.
            {
                ppl_parserStartNode(e, defnStack, &stackPos, PN_TYPE_ORA);
                inPos++;
            }

            // Punctuation which preceeds a new item in a sequence
            else if (ppl_cmdList[inPos] == '~') // ~ is used inside () to separate items
            {
                while ((stackPos >= 0) && (defnStack[stackPos - 1]->type == PN_TYPE_SEQ))
                    stackPos--; // Automatically close SEQs; they don't have closing grammar
                // A ~ must separate the items of a ( ) structure
                if ((stackPos < 0) || (defnStack[stackPos - 1]->type != PN_TYPE_PER)) {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Tilda should be used only in permutation structures (char pos %d).", inPos);
                    ppl_fatal(e, __FILE__, __LINE__, msg);
                }
                inPos++;
            } else if (ppl_cmdList[inPos] == '|') // | is used inside <> for either/or items
            {
                while ((stackPos >= 0) && (defnStack[stackPos - 1]->type == PN_TYPE_SEQ))
                    stackPos--; // Automatically close SEQs; they don't have closing grammar
                // A | must separate the alternatives of a < > structure
                if ((stackPos < 0) || (defnStack[stackPos - 1]->type != PN_TYPE_ORA)) {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Pipe alternatives should only be used inside ORA structures (char pos %d).",
                                      inPos);
                    ppl_fatal(e, __FILE__, __LINE__, msg);
                }
                inPos++;
            }

            // Punctuation which closes a hierarchical data structure
            else if (ppl_cmdList[inPos] == '>') // Match closing brackets for the above types
            {
                int j = 0;
                ppl_parserRollBack(e, defnStack, &stackPos, PN_TYPE_ORA, ppl_cmdList + inPos, &j);
                inPos += j;
            } else if (ppl_cmdList[inPos] == ')') {
                int j = 0;
                ppl_parserRollBack(e, defnStack, &stackPos, PN_TYPE_PER, ppl_cmdList + inPos, &j);
                inPos += j;
            } else if (ppl_cmdList[inPos] == ']') {
                int j = 0;
                ppl_parserRollBack(e, defnStack, &stackPos, PN_TYPE_REP, ppl_cmdList + inPos, &j);
                inPos += j;
            } else if (ppl_cmdList[inPos] == '}') {
                int j = 0;
                ppl_parserRollBack(e, defnStack, &stackPos, PN_TYPE_OPT, ppl_cmdList + inPos, &j);
                inPos += j;
            }

            // Non-punctuation items which match strings and expressions
            else if (ppl_cmdList[inPos] == '@') {
                parserNode *newNode;
                inPos++;
                if (defnStack[stackPos - 1]->type != PN_TYPE_SEQ)
                    ppl_parserStartNode(e, defnStack, &stackPos,
                                        PN_TYPE_SEQ); // Match words have to go in sequences, not, e.g. "ora" structures

                // Each item has the form @matchstr@varname@outstr@acLevel@stackpos@listlen: the text to match (or a
                // type of value, such as %e for an expression); the variable in which to store what was matched; the
                // string to store, if different from the matched text; the minimum number of characters for an
                // abbreviation to match; and where the variable goes on the output stack
                if ((newNode = (parserNode *) malloc(sizeof(parserNode))) == NULL) {
                    ppl_fatal(e, __FILE__, __LINE__, "Out of memory whilst setting up Pyxplot's command line parser.");
                    exit(1);
                }
                newNode->type = PN_TYPE_ITEM;
                newNode->firstChild = newNode->nextSibling = NULL;

                // Read the text to match. The special items DATABLOCK and CODEBLOCK match inline data and { } blocks of
                // code.
                int strStart = inPos;
                while (((unsigned char) ppl_cmdList[inPos] > ' ') && (ppl_cmdList[inPos] != '@'))
                    inPos++; // FFW over match string
                newNode->matchString = (char *) malloc((inPos - strStart + 1) * sizeof(char));
                // Failure to allocate memory is fatal
                if (newNode->matchString == NULL) {
                    ppl_fatal(e, __FILE__, __LINE__, "Out of memory whilst setting up Pyxplot's command line parser.");
                    exit(1);
                }
                strncpy(newNode->matchString, ppl_cmdList + strStart, inPos - strStart);
                newNode->matchString[inPos - strStart] = '\0';
                if (strcmp(newNode->matchString, "DATABLOCK") == 0) newNode->type = PN_TYPE_DATABLK;
                if (strcmp(newNode->matchString, "CODEBLOCK") == 0) newNode->type = PN_TYPE_CODEBLK;
                // The match string must be followed by an @
                if (ppl_cmdList[inPos++] != '@') {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Syntax error: expecting @ after @matchstr...");
                    ppl_fatal(e, __FILE__, __LINE__, msg);
                }

                // Read the name of the variable in which the match is stored
                strStart = inPos;
                while (((unsigned char) ppl_cmdList[inPos] > ' ') && (ppl_cmdList[inPos] != '@'))
                    inPos++; // FFW over variable name
                newNode->varName = (char *) malloc((inPos - strStart + 1) * sizeof(char));
                // Failure to allocate memory is fatal
                if (newNode->varName == NULL) {
                    ppl_fatal(e, __FILE__, __LINE__, "Out of memory whilst setting up Pyxplot's command line parser.");
                    exit(1);
                }
                strncpy(newNode->varName, ppl_cmdList + strStart, inPos - strStart);
                newNode->varName[inPos - strStart] = '\0';
                // The variable name must be followed by an @
                if (ppl_cmdList[inPos++] != '@') {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Syntax error: expecting @ after @matchstr@varname...");
                    ppl_fatal(e, __FILE__, __LINE__, msg);
                }

                // Read the string to store when this item is matched
                strStart = inPos;
                while (((unsigned char) ppl_cmdList[inPos] > ' ') && (ppl_cmdList[inPos] != '@'))
                    inPos++; // FFW over output string literal
                // If none is given, the matched text itself is stored
                if (strStart == inPos) {
                    newNode->outString =
                            newNode->matchString; // No outString is specified, assume it is the same as matchString
                } else {
                    newNode->outString = (char *) malloc((inPos - strStart + 1) * sizeof(char));
                    // Failure to allocate memory is fatal
                    if (newNode->outString == NULL) {
                        ppl_fatal(e, __FILE__, __LINE__,
                                  "Out of memory whilst setting up Pyxplot's command line parser.");
                        exit(1);
                    }
                    strncpy(newNode->outString, ppl_cmdList + strStart, inPos - strStart);
                    newNode->outString[inPos - strStart] = '\0';
                }
                // The string to store must be followed by an @
                if (ppl_cmdList[inPos++] != '@') {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Syntax error: expecting @ after @matchstr@varname@str...");
                    ppl_fatal(e, __FILE__, __LINE__, msg);
                }

                // Read the minimum number of characters which an abbreviation of the match string must have
                int k = (int) ppl_getFloat(ppl_cmdList + inPos, NULL);
                while (((unsigned char) ppl_cmdList[inPos] > ' ') && (ppl_cmdList[inPos] != '@')) inPos++;
                newNode->acLevel = k;
                // Read the position on the output stack where the match is stored
                if (ppl_cmdList[inPos++] != '@') {
                    ppl_snprintfTrunc(msg, sizeof(msg), "Syntax error: expecting @ after ]@varname@value@ACL...");
                    ppl_fatal(e, __FILE__, __LINE__, msg);
                }
                int l = (int) ppl_getFloat(ppl_cmdList + inPos, NULL);
                while (((unsigned char) ppl_cmdList[inPos] > ' ') && (ppl_cmdList[inPos] != '@')) inPos++;
                newNode->outStackPos = l;
                // Read the length of the output stack, for items which hold lists
                if (ppl_cmdList[inPos++] != '@') {
                    ppl_snprintfTrunc(msg, sizeof(msg),
                                      "Syntax error: expecting @ after ]@varname@value@ACL@stackpos...");
                    ppl_fatal(e, __FILE__, __LINE__, msg);
                }
                int m = (int) ppl_getFloat(ppl_cmdList + inPos, NULL);
                while (((unsigned char) ppl_cmdList[inPos] > ' ') && (ppl_cmdList[inPos] != '@')) inPos++;
                newNode->listLen = m;
                while ((ppl_cmdList[inPos] != '\0') && (ppl_cmdList[inPos] != '\n') &&
                       ((unsigned char) ppl_cmdList[inPos] < ' '))
                    inPos++; // FFW over trailing whitespace

                parserNode **target = &(defnStack[stackPos - 1]->firstChild); // Pointer to where first child of parent is linked
                while (*target != NULL)
                    target = &((*target)->nextSibling); // If not first child of parent, traverse linked list to end
                *target =
                        newNode; // Add new node into hierarchy, either as first child of parent, or into linked list of siblings
            } else if (ppl_cmdList[inPos] == '=') {
                parserNode *newNode;
                // An = marks the point beyond which a failure to match the rest of the command is a syntax error,
                // rather than a sign that a different command was meant
                if ((newNode = (parserNode *) calloc(1, sizeof(parserNode))) == NULL) {
                    ppl_fatal(e, __FILE__, __LINE__, "Out of memory whilst setting up Pyxplot's command line parser.");
                    exit(1);
                }
                newNode->type = PN_TYPE_CONFIRM;
                parserNode **target = &(defnStack[stackPos - 1]->firstChild); // Pointer to where first child of parent is linked
                while (*target != NULL)
                    target = &((*target)->nextSibling); // If not first child of parent, traverse linked list to end
                *target =
                        newNode; // Add new node into hierarchy, either as first child of parent, or into linked list of siblings
                inPos++;
            } else {
                ppl_snprintfTrunc(msg, sizeof(msg), "Syntax error: unexpected character '%c'", ppl_cmdList[inPos]);
                ppl_fatal(e, __FILE__, __LINE__, msg);
            }
        }
        ppl_listAppend(pplParserCmdList[cmdLetter], defnStack[0]);
    }
    return 0;
}
