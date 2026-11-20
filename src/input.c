// input.c
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

#define _INPUT_C 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#include "coreUtils/errorReport.h"
#include "coreUtils/getPasswd.h"

#include "expressions/traceback_fns.h"

#include "parser/parser.h"

#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"

#include "settings/settings.h"
#include "settings/settingTypes.h"
#include "settings/textConstants.h"

#include "userspace/context.h"

#include "children.h"
#include "input.h"
#include "pplConstants.h"
#include "pyxplot.h"

//! ppl_inputInit - Allocate the buffer into which lines of input are read
//! \param [in|out] context - The Pyxplot context
//! \return One on success, or zero if memory allocation failed

int ppl_inputInit(ppl_context *context) {
    context->inputLineBufferLen = LSTR_LENGTH;
    context->inputLineBuffer = (char *) malloc(context->inputLineBufferLen);
    return (context->inputLineBuffer != NULL);
}

sigjmp_buf sigint_destination;
int sigint_longjmp = 0;

//! ppl_interactiveSigInt - SIGINT handler used during interactive sessions. While waiting for input from readline,
//! it jumps back to discard the current line; otherwise it sets <cancellationFlag> to interrupt the current command.
//! \param [in] signo - The signal number

void ppl_interactiveSigInt(const int signo) {
    if (sigint_longjmp) siglongjmp(sigint_destination, 1);
    fprintf(stdout, "\n");
    cancellationFlag = 1;
    return;
}

//! ppl_interactiveSession - Run an interactive session, reading commands from the terminal (through readline, if
//! available) and executing them until the user quits
//! \param [in] context - The Pyxplot context

void ppl_interactiveSession(ppl_context *context) {
    int linenumber = 1;
    void (*sigint_old)(int) = NULL;
    parserLine *pl = NULL;
    parserStatus *ps = NULL;

    if (DEBUG) ppl_log(&context->errcontext, "Starting an interactive session.");

    ppl_parserStatInit(&ps, &pl);
    if ((ps == NULL) || (context->inputLineBuffer == NULL)) {
        ppl_error(&context->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
        return;
    }

    if ((isatty(STDIN_FILENO) == 1) && (context->errcontext.session_default.splash == SW_ONOFF_ON))
        ppl_report(&context->errcontext, ppltxt_welcome);

    context->shellExiting = 0;
    sigint_old = signal(SIGINT, ppl_interactiveSigInt); // Set up SIGINT handler
    while ((!context->shellExiting) && (!context->shellBroken) && (!context->shellContinued) &&
           (!context->shellReturned)) {
        pplcsp_checkForGvOutput(context);
        cancellationFlag = 0;
        if (isatty(STDIN_FILENO) == 1) {
            ppl_error_setstreaminfo(&context->errcontext, -1, "");
#ifdef HAVE_READLINE
            {
                int len;
                char *line_ptr, prompt[32];
                if (context->inputLineAddBuffer != NULL) {
                    strcpy(prompt, ".......> ");
                } else {
                    // Continuation prompt: the first seven characters of the prompt, padded with dots
                    int k;
                    for (k = 0; (k < 7) && (ps->prompt[k] != '\0'); k++) prompt[k] = ps->prompt[k];
                    for (; k < 7; k++) prompt[k] = '.';
                    strcpy(prompt + 7, "> ");
                }

                if (sigsetjmp(sigint_destination, 1) != 0) { printf("\n"); }
                sigint_longjmp = 1;
                line_ptr = readline(prompt);
                sigint_longjmp = 0;
                if (line_ptr == NULL) break;
                add_history(line_ptr);
                context->historyNLinesWritten++;
                len = strlen(line_ptr) + 1;
                if ((context->inputLineBufferLen > LSTR_LENGTH) && (len < LSTR_LENGTH)) {
                    free(context->inputLineBuffer);
                    context->inputLineBuffer = NULL;
                    ppl_inputInit(context);
                }
                if (len > context->inputLineBufferLen) {
                    context->inputLineBuffer = (char *) realloc(context->inputLineBuffer, len);
                    context->inputLineBufferLen = len;
                }
                if (context->inputLineBuffer == NULL) break;
                strcpy(context->inputLineBuffer, line_ptr);
                free(line_ptr);
                line_ptr = NULL;
            }
#else
            {
                char prompt[32];
                if (sigsetjmp(sigint_destination, 1) != 0) { printf("\n"); }
                sigint_longjmp = 1;
                if (context->inputLineAddBuffer != NULL) {
                    strcpy(prompt, ".......> ");
                } else {
                    // Continuation prompt: the first seven characters of the prompt, padded with dots
                    int k;
                    for (k = 0; (k < 7) && (ps->prompt[k] != '\0'); k++) prompt[k] = ps->prompt[k];
                    for (; k < 7; k++) prompt[k] = '.';
                    strcpy(prompt + 7, "> ");
                }
                printf("%s", prompt);
                if (fgets(context->inputLineBuffer, context->inputLineBufferLen - 1, stdin) == NULL) { break; }
                sigint_longjmp = 0;
                context->inputLineBuffer[context->inputLineBufferLen - 1] = '\0';
            }
#endif
        } else {
            ppl_error_setstreaminfo(&context->errcontext, linenumber, "piped input");
            if ((feof(stdin)) || (ferror(stdin))) break;
            ppl_file_readline(stdin, &context->inputLineBuffer, &context->inputLineBufferLen, 0);
            context->inputLineBuffer[context->inputLineBufferLen - 1] = '\0';
            linenumber++;
        }
        if ((!cancellationFlag) && (!context->shellExiting) && (!context->shellBroken) && (!context->shellContinued) &&
            (!context->shellReturned)) {
            ppl_processLine(context, ps, context->inputLineBuffer, isatty(STDIN_FILENO), 0);
        }
        cancellationFlag = 0;
        ppl_error_setstreaminfo(&context->errcontext, -1, "");
        pplcsp_killAllHelpers(context);
    }

    if (context->inputLineAddBuffer != NULL) {
        free(context->inputLineAddBuffer);
        context->inputLineAddBuffer = NULL;
    }
    context->shellExiting = 0;
    signal(SIGINT, sigint_old); // Restore old SIGINT handler
    if (isatty(STDIN_FILENO) == 1) {
        if (context->errcontext.session_default.splash == SW_ONOFF_ON)
            ppl_report(&context->errcontext, "\nGoodbye. Have a nice day.");
        else ppl_report(&context->errcontext, ""); // Make a new line
    }
    ppl_parserStatFree(&ps);
    return;
}

//! ppl_processScript - Execute the commands in a script file
//! \param [in] context - The Pyxplot context
//! \param [in] input - The filename of the script
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion

void ppl_processScript(ppl_context *context, char *input, const int iterDepth) {
    char msg[PPL_MSG_LENGTH];
    int linenumber = 1;
    int status;
    char full_filename[FNAME_LENGTH];
    char filename_description[FNAME_LENGTH];
    FILE *infile;
    parserLine *pl = NULL;
    parserStatus *ps = NULL;
    const int shellBreakableOld = context->shellBreakable;
    const int shellReturnableOld = context->shellReturnable;

    char *oldInputLineBuffer = context->inputLineBuffer;
    const int oldInputLineBufferLen = context->inputLineBufferLen;
    char *oldInputLineAddBuffer = context->inputLineAddBuffer;

    if (DEBUG) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Processing input from the script file '%s'.", input);
        ppl_log(&context->errcontext, msg);
    }
    ppl_unixExpandUserHomeDir(&context->errcontext, input, context->errcontext.session_default.cwd, full_filename,
                              sizeof(full_filename));
    ppl_snprintfTrunc(filename_description, sizeof(filename_description), "file '%s'", input);
    if ((infile = fopen(full_filename, "r")) == NULL) {
        ppl_snprintfTrunc(msg, sizeof(msg), "Could not find command file '%s'. Skipping on to next command file.",
                          full_filename);
        ppl_error(&context->errcontext, ERR_FILE, -1, -1, msg);
        goto restore;
    }

    ppl_inputInit(context);
    ppl_parserStatInit(&ps, &pl);
    if ((ps == NULL) || (context->inputLineBuffer == NULL)) {
        ppl_error(&context->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
        goto restore;
    }

    context->shellExiting = 0;
    context->shellBreakable = 0;
    context->shellReturnable = 0;
    while ((!context->shellExiting) && (!context->shellBroken) && (!context->shellContinued) &&
           (!context->shellReturned) && (!cancellationFlag)) {
        ppl_error_setstreaminfo(&context->errcontext, linenumber, filename_description);
        if ((feof(infile)) || (ferror(infile))) break;
        if (context->inputLineBufferLen > LSTR_LENGTH) {
            free(context->inputLineBuffer);
            context->inputLineBuffer = NULL;
            ppl_inputInit(context);
        }
        if (context->inputLineBuffer == NULL) {
            ppl_error(&context->errcontext, ERR_MEMORY, -1, -1, "Out of memory.");
            break;
        }
        ppl_file_readline(infile, &context->inputLineBuffer, &context->inputLineBufferLen, 0);
        linenumber++;
        status = ppl_processLine(context, ps, context->inputLineBuffer, 0, iterDepth);
        ppl_error_setstreaminfo(&context->errcontext, -1, "");
        pplcsp_killAllHelpers(context);
        if ((status > 0) || context->shellExiting ||
            cancellationFlag) // If an error occurs in a script, aborted processing it
        {
            if (!context->shellExiting) ppl_error(&context->errcontext, ERR_FILE, -1, -1, "Aborting.");
            if (context->inputLineAddBuffer != NULL) {
                free(context->inputLineAddBuffer);
                context->inputLineAddBuffer = NULL;
            }
            break;
        }
    }
    if ((!context->shellExiting) && (!context->shellBroken) && (!context->shellContinued) &&
        (!context->shellReturned) && (!cancellationFlag))
        if (context->inputLineAddBuffer != NULL) // Process last line of file if there is still text buffered
        {
            ppl_warning(&context->errcontext, ERR_SYNTAX,
                        "Line continuation character (\\) at the end of command script, with no line following it.");
            ppl_error_setstreaminfo(&context->errcontext, linenumber, filename_description);
            status = ppl_processLine(context, ps, "", 0, iterDepth);
            ppl_error_setstreaminfo(&context->errcontext, -1, "");
            if (status > 0) ppl_error(&context->errcontext, ERR_FILE, -1, -1, "Aborting.");
            if (context->inputLineAddBuffer != NULL) {
                free(context->inputLineAddBuffer);
                context->inputLineAddBuffer = NULL;
            }
        }
    context->shellExiting = 0;
    context->shellBreakable = shellBreakableOld;
    context->shellReturnable = shellReturnableOld;
    context->inputLineAddBuffer = NULL;
    fclose(infile);
    ppl_parserStatFree(&ps);
    pplcsp_checkForGvOutput(context);

restore:
    if (context->inputLineBuffer != NULL) {
        free(context->inputLineBuffer);
        context->inputLineBuffer = NULL;
    }
    context->inputLineBuffer = oldInputLineBuffer;
    context->inputLineBufferLen = oldInputLineBufferLen;
    context->inputLineAddBuffer = oldInputLineAddBuffer;
    return;
}

//! ppl_processLine - Execute a line of input, which may contain several statements separated by semicolons
//! \param [in] context - The Pyxplot context
//! \param [in|out] ps - The parser status, which tracks the progress of compiling multi-line blocks of commands
//! \param [in] in - The line of input
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! lineScanState - The state of a scan through a command line: whether it is inside a quoted string (with ' " or `
//! quotes), and how deeply nested in brackets it is
typedef struct lineScanState {
    char quoteChar; // The quote character of the string currently open, or '\0' if outside any string
    int bracketLevel; // The number of round brackets currently open
} lineScanState;

//! lineScanStep - Update the state of a scan through a command line, for the next character
//! \param [in,out] st - The state of the scan
//! \param [in] line - The command line
//! \param [in] i - The position in <line> of the next character
//! \return True if the character was a bracket or quotation mark, which changed the state

static int lineScanStep(lineScanState *st, const char *line, const int i) {
    const char ch = line[i];
    const int escaped = (i > 0) && (line[i - 1] == '\\');
    if (st->quoteChar == '\0') {
        if (ch == '(') st->bracketLevel++;
        else if (ch == ')') st->bracketLevel--;
        else if ((ch == '\'') || (ch == '\"') || (ch == '`')) st->quoteChar = ch;
        else return 0;
        return 1;
    }
    if ((ch == st->quoteChar) && !escaped) {
        st->quoteChar = '\0';
        return 1;
    }
    return 0;
}

//! \return Zero on success, or one if an error occurred

int ppl_processLine(ppl_context *context, parserStatus *ps, char *in, const int interactive, const int iterDepth) {
    int i, status = 0;
    char *inputLineBuffer = NULL;

    // Find last character of presently entered line. If it's a \, buffer line and collect another
    for (i = 0; in[i] != '\0'; i++);
    for (; ((i > 0) && ((unsigned char) in[i] <= ' ')); i--);
    if (in[i] == '\\') {
        if (context->inputLineAddBuffer == NULL) {
            context->inputLineAddBuffer = (char *) malloc(i + 1);
            if (context->inputLineAddBuffer == NULL) {
                ppl_error(&context->errcontext, ERR_MEMORY, -1, -1,
                          "Out of memory whilst trying to combine input lines.");
                return 1;
            }
            strncpy(context->inputLineAddBuffer, in, i);
            context->inputLineAddBuffer[i] = '\0';
        } else {
            int j = strlen(context->inputLineAddBuffer);
            context->inputLineAddBuffer = (char *) realloc((void *) context->inputLineAddBuffer, j + i + 1);
            if (context->inputLineAddBuffer == NULL) {
                ppl_error(&context->errcontext, ERR_MEMORY, -1, -1,
                          "Out of memory whilst trying to combine input lines.");
                return 1;
            }
            strncpy(context->inputLineAddBuffer + j, in, i);
            context->inputLineAddBuffer[j + i] = '\0';
        }
        return 0;
    }

    // Add previous backslashed lines to the beginning of this one
    if (context->inputLineAddBuffer != NULL) {
        int j = strlen(context->inputLineAddBuffer);
        context->inputLineAddBuffer = (char *) realloc((void *) context->inputLineAddBuffer, j + i + 2);
        if (context->inputLineAddBuffer == NULL) {
            ppl_error(&context->errcontext, ERR_MEMORY, -1, -1, "Out of memory whilst trying to combine input lines.");
            return 1;
        }
        strncpy(context->inputLineAddBuffer + j, in, i + 1);
        context->inputLineAddBuffer[j + i + 1] = '\0';
        inputLineBuffer = context->inputLineAddBuffer;
    } else {
        inputLineBuffer = in;
    }


    // Cut comments off the ends of lines
    {
        lineScanState st = {'\0', 0};
        for (i = 0; inputLineBuffer[i] != '\0'; i++) {
            if (lineScanStep(&st, inputLineBuffer, i)) continue;
            if ((st.quoteChar == '\0') && (inputLineBuffer[i] == '#')) break;
        }
    }
    inputLineBuffer[i] = '\0';

    // Loop over semicolon-separated line segments
    {
        lineScanState st = {'\0', 0};
        for (i = 0; inputLineBuffer[i] != '\0'; i++) {
            if (lineScanStep(&st, inputLineBuffer, i)) continue;
            if ((st.quoteChar == '\0') && (st.bracketLevel == 0) && (inputLineBuffer[i] == ';')) {
                inputLineBuffer[i] = '\0';
                status = ppl_ProcessStatement(context, ps, inputLineBuffer, interactive, iterDepth);
                if (status) break;
                inputLineBuffer = inputLineBuffer + i + 1;
                i = -1;
            }
        }
    }

    if (!status) status = ppl_ProcessStatement(context, ps, inputLineBuffer, interactive, iterDepth);
    if (context->inputLineAddBuffer != NULL) {
        free(context->inputLineAddBuffer);
        context->inputLineAddBuffer = NULL;
    }
    context->inputLineAddBuffer = NULL;
    return (status != 0);
}

//! ppl_ProcessStatement - Compile and execute a single statement. If it opens a block of commands, it is stored in
//! the parser status until the block is complete.
//! \param [in] context - The Pyxplot context
//! \param [in|out] ps - The parser status, which tracks the progress of compiling multi-line blocks of commands
//! \param [in] line - The statement
//! \param [in] interactive - Flag indicating whether the session is interactive, i.e. attached to a terminal
//! \param [in] iterDepth - The depth of recursion, used to prevent infinite recursion
//! \return Zero on success, or one if an error occurred

int ppl_ProcessStatement(ppl_context *context, parserStatus *ps, char *line, const int interactive,
                         const int iterDepth) {
    int stat = 0;

    ppl_tbClear(context);

    // If line is blank, ignore it
    {
        int i = 0, j = 0;
        for (i = 0; line[i] != '\0'; i++)
            if ((unsigned char) line[i] > ' ') {
                j = 1;
                break;
            }
        if (j == 0) return 0;
    }

    stat = ppl_parserCompile(context, ps, context->errcontext.error_input_linenumber,
                             context->errcontext.error_input_sourceId, context->errcontext.error_input_filename, line,
                             0, iterDepth);

    if ((!stat) && (!context->errStat.status) && (ps->blockDepth == 0)) {
        ppl_parserExecute(context, ps->pl[iterDepth], NULL, interactive, iterDepth);
    }

    if ((iterDepth > 0) && (ps->blockDepth == 0)) {
        ppl_parserLineFree(ps->pl[iterDepth]);
        ps->pl[iterDepth] = NULL;
    }

    if (stat || context->errStat.status) {
        if (iterDepth == 0) {
            ppl_tbWrite(context);
            ppl_parserStatReInit(ps);
        }
        return 1;
    }

    return 0;
}
