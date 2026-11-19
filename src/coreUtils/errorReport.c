// errorReport.c
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"

#include "coreUtils/errorReport.h"
#include "settings/settingTypes.h"

#define BLEN (LSTR_LENGTH - 1)

static char temp_stringA[BLEN + 1], temp_stringB[BLEN + 1], temp_stringC[BLEN + 1], temp_stringD[BLEN + 1],
        temp_stringE[BLEN + 1];

//! ppl_error_setstreaminfo - Record the source of the input currently being processed, so that error messages can
//! report the filename and line number where they occurred
//! \param [in|out] context - The error-reporting context to update. It is also given a new unique source ID.
//! \param [in] linenumber - The line number currently being processed, or -1 if not processing a file
//! \param [in] filename - The name of the file currently being processed, or NULL to leave it unchanged

void ppl_error_setstreaminfo(pplerr_context *context, const int linenumber, const char *filename) {
    static long sourceId = 0;
    context->error_input_sourceId = sourceId++;
    context->error_input_linenumber = linenumber;
    if (filename != NULL) {
        strncpy(context->error_input_filename, filename, FNAME_LENGTH);
        context->error_input_filename[FNAME_LENGTH - 1] = '\0';
    }
    return;
}

//! ppl_error - Report an error to the user on stderr, prefixed with the location in the input where it occurred and
//! the type of error. If the terminal supports colour, the message is coloured, and up to two character positions
//! within it can be highlighted in reverse video.
//! \param [in] context - The error-reporting context
//! \param [in] ErrType - The type of error, one of the ERR_* constants. ERR_PREFORMED messages are printed verbatim.
//! \param [in] HighlightPos1 - One-indexed position of the first character in <msg> to highlight, or -1 for none
//! \param [in] HighlightPos2 - One-indexed position of the second character in <msg> to highlight, or -1 for none
//! \param [in] msg - The error message (NULL is treated as an empty message)

void ppl_error(pplerr_context *context, const int ErrType, int HighlightPos1, int HighlightPos2, char *msg) {
    unsigned char ApplyHighlighting, reverse = 0;
    int i = 0, j;

    if (msg == NULL) msg = "";
    ApplyHighlighting = ((context->session_default.color == SW_ONOFF_ON) && (isatty(STDERR_FILENO) == 1));
    if (msg != temp_stringA) {
        ppl_snprintfTrunc(temp_stringA, BLEN, "%s", msg);
        temp_stringA[BLEN] = '\0';
        msg = temp_stringA;
    }

    temp_stringB[i] = temp_stringB[BLEN] = '\0';

    if (ErrType != ERR_PREFORMED) // Do not prepend anything to pre-formed errors
    {
        // When processing scripts, print error location
        if ((context->error_input_linenumber != -1) && (strcmp(context->error_input_filename, "") != 0)) {
            ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "%s:%d:", context->error_input_filename,
                              context->error_input_linenumber);
            i += strlen(temp_stringB + i);
            if ((ErrType != ERR_STACKED) && (i < BLEN - 2)) {
                temp_stringB[i++] = ' ';
                temp_stringB[i] = '\0';
            }
        }

        // Prepend error type
        switch (ErrType) {
            case ERR_INTERNAL:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Internal error: ");
                break;
            case ERR_MEMORY:
            case ERR_GENERIC:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Error: ");
                break;
            case ERR_SYNTAX:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Syntax error: ");
                break;
            case ERR_NUMERICAL:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Numerical error: ");
                break;
            case ERR_FILE:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "File error: ");
                break;
            case ERR_RANGE:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Range error: ");
                break;
            case ERR_UNIT:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Unit error: ");
                break;
            case ERR_OVERFLOW:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Overflow error: ");
                break;
            case ERR_NAMESPACE:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Namespace error: ");
                break;
            case ERR_TYPE:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Type error: ");
                break;
            case ERR_INTERRUPT:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Interrupt error: ");
                break;
            case ERR_DICTKEY:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Key error: ");
                break;
            case ERR_ASSERT:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Assertion error: ");
                break;
        }
        i += strlen(temp_stringB + i);
    }

    for (j = 0; msg[j] != '\0'; j++) {
        if (ApplyHighlighting) {
            if (j == HighlightPos1 - 1) {
                if (reverse == 0) {
                    ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "\x1b[7m");
                    i += strlen(temp_stringB + i);
                }
                reverse = 3;
                HighlightPos1 = -1;
            } else if (j == HighlightPos2 - 1) {
                if (reverse == 0) {
                    ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "\x1b[7m");
                    i += strlen(temp_stringB + i);
                }
                reverse = 3;
                HighlightPos2 = -1;
            } else if (reverse == 1) {
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "\x1b[27m");
                i += strlen(temp_stringB + i);
                reverse = 0;
            } else if (reverse > 1) reverse--;
        }
        if (i < BLEN) {
            if ((msg[j] == '\n') && (reverse > 0)) {
                temp_stringB[i++] = ' ';
                j--;
            } // If newline is highlighted, add spaces to end of line so that we can highlight them
            else
                temp_stringB[i++] = msg[j];
        }
    }
    if (ApplyHighlighting && (reverse != 0)) {
        ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "\x1b[27m");
        i += strlen(temp_stringB + i);
        reverse = 0;
    }
    if (i < BLEN) temp_stringB[i] = '\0';

    if (DEBUG) { ppl_log(context, temp_stringB); }

    // Print message in color or monochrome
    if (ApplyHighlighting)
        ppl_snprintfTrunc(temp_stringC, BLEN, "%s%s%s\n",
                          *(char **) ppl_fetchSettingName(context, context->session_default.color_err, SW_TERMCOL_INT,
                                                          (void *) SW_TERMCOL_TXT, sizeof(char *)),
                          temp_stringB,
                          *(char **) ppl_fetchSettingName(context, SW_TERMCOL_NOR, SW_TERMCOL_INT,
                                                          (void *) SW_TERMCOL_TXT, sizeof(char *)));
    else ppl_snprintfTrunc(temp_stringC, BLEN, "%s\n", temp_stringB);
    temp_stringC[BLEN] = '\0';
    fputs(temp_stringC, stderr);
    return;
}

//! ppl_fatal - Report a fatal internal error, and terminate the process
//! \param [in] context - The error-reporting context
//! \param [in] file - The source code file in which the error occurred
//! \param [in] line - The line number in the source code file where the error occurred
//! \param [in] msg - The error message (NULL is treated as an empty message)

void ppl_fatal(pplerr_context *context, char *file, int line, char *msg) {
    char introline[FNAME_LENGTH];
    if (msg == NULL) msg = "";
    if (msg != temp_stringE) {
        ppl_snprintfTrunc(temp_stringE, BLEN, "%s", msg);
        temp_stringE[BLEN - 1] = '\0';
        msg = temp_stringE;
    }
    ppl_snprintfTrunc(introline, FNAME_LENGTH, "Fatal Error encountered in %s at line %d: %s", file, line,
                      temp_stringE);
    introline[FNAME_LENGTH - 1] = '\0';
    ppl_error(context, ERR_PREFORMED, -1, -1, introline);
    if (DEBUG) ppl_log(context, "Terminating with error condition 1.");
    exit(1);
}

//! ppl_warning - Report a warning to the user on stderr, prefixed with the location in the input where it occurred
//! and the type of warning
//! \param [in] context - The error-reporting context
//! \param [in] ErrType - The type of warning, one of the ERR_* constants. ERR_PREFORMED messages are printed
//! verbatim.
//! \param [in] msg - The warning message (NULL is treated as an empty message)

void ppl_warning(pplerr_context *context, const int ErrType, char *msg) {
    int i = 0;

    if (msg == NULL) msg = "";
    if (msg != temp_stringA) {
        ppl_snprintfTrunc(temp_stringA, BLEN, "%s", msg);
        temp_stringA[BLEN - 1] = '\0';
        msg = temp_stringA;
    }

    temp_stringB[i] = temp_stringB[BLEN] = '\0';

    if (ErrType != ERR_PREFORMED) // Do not prepend anything to pre-formed errors
    {
        // When processing scripts, print error location
        if ((context->error_input_linenumber != -1) && (strcmp(context->error_input_filename, "") != 0)) {
            ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "%s:%d:", context->error_input_filename,
                              context->error_input_linenumber);
            i += strlen(temp_stringB + i);
            if ((ErrType != ERR_STACKED) && (i < BLEN - 2)) {
                temp_stringB[i++] = ' ';
                temp_stringB[i] = '\0';
            }
        }

        // Prepend error type
        switch (ErrType) {
            case ERR_INTERNAL:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Internal warning: ");
                break;
            case ERR_MEMORY:
            case ERR_GENERIC:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Warning: ");
                break;
            case ERR_SYNTAX:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Syntax warning: ");
                break;
            case ERR_NUMERICAL:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Numerical warning: ");
                break;
            case ERR_FILE:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "File warning: ");
                break;
            case ERR_RANGE:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Range warning: ");
                break;
            case ERR_UNIT:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Unit warning: ");
                break;
            case ERR_OVERFLOW:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Overflow warning: ");
                break;
            case ERR_NAMESPACE:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Namespace warning: ");
                break;
            case ERR_TYPE:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Type warning: ");
                break;
            case ERR_INTERRUPT:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Interrupt warning: ");
                break;
            case ERR_DICTKEY:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Key warning: ");
                break;
            case ERR_ASSERT:
                ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "Assertion warning: ");
                break;
        }
        i += strlen(temp_stringB + i);
    }

    ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "%s", msg);

    if (DEBUG) { ppl_log(context, temp_stringB); }

    // Print message in color or monochrome
    if ((context->session_default.color == SW_ONOFF_ON) && (isatty(STDERR_FILENO) == 1))
        ppl_snprintfTrunc(temp_stringC, BLEN, "%s%s%s\n",
                          *(char **) ppl_fetchSettingName(context, context->session_default.color_wrn, SW_TERMCOL_INT,
                                                          (void *) SW_TERMCOL_TXT, sizeof(char *)),
                          temp_stringB,
                          *(char **) ppl_fetchSettingName(context, SW_TERMCOL_NOR, SW_TERMCOL_INT,
                                                          (void *) SW_TERMCOL_TXT, sizeof(char *)));
    else ppl_snprintfTrunc(temp_stringC, BLEN, "%s\n", temp_stringB);
    temp_stringC[BLEN] = '\0';
    fputs(temp_stringC, stderr);
    return;
}

//! ppl_report - Print a report-level message to the user on stdout, coloured if the terminal supports it
//! \param [in] context - The error-reporting context
//! \param [in] msg - The message (NULL is treated as an empty message)

void ppl_report(pplerr_context *context, char *msg) {
    if (msg == NULL) msg = "";
    if (msg != temp_stringA) {
        ppl_snprintfTrunc(temp_stringA, BLEN, "%s", msg);
        temp_stringA[BLEN - 1] = '\0';
    }
    if (DEBUG) {
        ppl_snprintfTrunc(temp_stringC, BLEN, "%s%s", "Reporting:\n", temp_stringA);
        temp_stringC[BLEN] = '\0';
        ppl_log(context, temp_stringC);
    }
    if ((context->session_default.color == SW_ONOFF_ON) && (isatty(STDOUT_FILENO) == 1))
        ppl_snprintfTrunc(temp_stringC, BLEN, "%s%s%s\n",
                          *(char **) ppl_fetchSettingName(context, context->session_default.color_rep, SW_TERMCOL_INT,
                                                          (void *) SW_TERMCOL_TXT, sizeof(char *)),
                          temp_stringA,
                          *(char **) ppl_fetchSettingName(context, SW_TERMCOL_NOR, SW_TERMCOL_INT,
                                                          (void *) SW_TERMCOL_TXT, sizeof(char *)));
    else ppl_snprintfTrunc(temp_stringC, BLEN, "%s\n", temp_stringA);
    temp_stringC[BLEN] = '\0';
    fputs(temp_stringC, stdout);
    return;
}

//! ppl_log - Write a message to the log file <pyxplot.PID.log> in the current working directory, prefixed with a
//! timestamp and the name of the process that produced it
//! \param [in] context - The error-reporting context
//! \param [in] msg - The message (NULL is treated as an empty message)

void ppl_log(pplerr_context *context, char *msg) {
    static FILE *logfile = NULL;
    static int latch = 0;
    char linebuffer[LSTR_LENGTH];

    if (latch) return; // Do not allow recursive calls, which might be generated by the call to ppl_fatal below
    latch = 1;
    if (logfile == NULL) {
        char LogFName[128];
        snprintf(LogFName, 127, "pyxplot.%d.log", getpid());
        LogFName[127] = '\0';
        if ((logfile = fopen(LogFName, "w")) == NULL) {
            ppl_fatal(context, __FILE__, __LINE__, "Could not open log file to write.");
            exit(1);
        }
        setvbuf(logfile, NULL, _IOLBF, 0); // Set log file to be line-buffered, so that log file is always up-to-date
    }

    if (msg == NULL) msg = "";
    if (msg != temp_stringD) {
        ppl_snprintfTrunc(temp_stringD, BLEN, "%s", msg);
        temp_stringD[BLEN - 1] = '\0';
    }
    fprintf(logfile, "[%s] [%s] %s\n", ppl_strStrip(ppl_friendlyTimestring(), linebuffer), context->error_source,
            temp_stringD);
    latch = 0;
    return;
}

//! ppl_errorCountTake - Decide whether to report an error, when only a limited number of errors are to be reported
//! (e.g. errors in the lines of a data file). If so, the count of errors remaining is decremented.
//! \param [in,out] errCount - The number of errors which may still be reported
//! \return True if the error should be reported

int ppl_errorCountTake(int *errCount) {
    if (*errCount <= 0) return 0;
    (*errCount)--;
    return 1;
}

//! ppl_errorCountCheck - After reporting an error with ppl_errorCountTake(), warn if no more errors will be reported
//! \param [in] context - The error-reporting context
//! \param [in] errCount - The number of errors which may still be reported
//! \param [in] filename - The name of the file in which the errors are occurring, or NULL
//! \param [in] lineNumber - The line number in the file, or -1 to omit it

void ppl_errorCountCheck(pplerr_context *context, const int errCount, const char *filename, const long lineNumber) {
    char msg[PPL_MSG_LENGTH];
    if (errCount != 0) return;
    if (filename == NULL) ppl_snprintfTrunc(msg, sizeof(msg), "Too many errors: no more errors will be shown.");
    else if (lineNumber < 0)
        ppl_snprintfTrunc(msg, sizeof(msg), "%s: Too many errors: no more errors will be shown.", filename);
    else
        ppl_snprintfTrunc(msg, sizeof(msg), "%s:%ld: Too many errors: no more errors will be shown.", filename,
                          lineNumber);
    ppl_warning(context, ERR_STACKED, msg);
}
