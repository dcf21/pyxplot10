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

// Reports errors, warnings, informational messages and debugging logs to the user, prefixing each with the file and
// line of input being processed. Every part of Pyxplot reports problems through these functions, which also count the
// errors so that Pyxplot can exit with a non-zero status. Messages are assembled in static buffers, under a lock, so
// that threads evaluating functions in parallel can report safely.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"

#include "coreUtils/errorReport.h"
#include "settings/settingTypes.h"

// The length of the static buffers in which messages are assembled, leaving room for a null terminator
#define BLEN (LSTR_LENGTH - 1)

static char temp_stringA[BLEN + 1], temp_stringB[BLEN + 1], temp_stringC[BLEN + 1], temp_stringD[BLEN + 1],
        temp_stringE[BLEN + 1];

// Error messages may be reported from several threads at once, e.g. while functions are sampled in parallel. Only one
// thread may write a message at a time, since messages are assembled in static buffers. The lock is nestable, since
// the reporting functions call one another.
#ifdef _OPENMP
static omp_nest_lock_t reportLockObj;

//! reportLockInit - Initialise the lock which lets only one thread report a message at a time, when Pyxplot starts

static void reportLockInit() __attribute__((constructor));
static void reportLockInit() {
    omp_init_nest_lock(&reportLockObj);
}
#endif

//! reportLock - Wait until no other thread is reporting a message, and then stop other threads from doing so

static void reportLock() {
#ifdef _OPENMP
    omp_set_nest_lock(&reportLockObj);
#endif
}

//! reportUnlock - Let other threads report messages again

static void reportUnlock() {
#ifdef _OPENMP
    omp_unset_nest_lock(&reportLockObj);
#endif
}

//! ppl_error_setstreaminfo - Record the source of the input currently being processed, so that error messages can
//! report the filename and line number where they occurred
//! \param [in|out] context - The error-reporting context to update. It is also given a new unique source ID.
//! \param [in] linenumber - The line number currently being processed, or -1 if not processing a file
//! \param [in] filename - The name of the file currently being processed, or NULL to leave it unchanged

void ppl_error_setstreaminfo(pplerr_context *context, const int linenumber, const char *filename) {
    static long sourceId = 0;
    context->error_input_sourceId = sourceId++;
    context->error_input_linenumber = linenumber;
    // Record the filename, truncating it to fit the buffer
    if (filename != NULL) {
        strncpy(context->error_input_filename, filename, FNAME_LENGTH);
        context->error_input_filename[FNAME_LENGTH - 1] = '\0';
    }
    return;
}

//! errorUnlocked - The body of ppl_error(), which is called with the lock held that lets only one thread report at a time

static void errorUnlocked(pplerr_context *context, const int ErrType, int HighlightPos1, int HighlightPos2, char *msg) {
    // ApplyHighlighting is set if the terminal supports colour; reverse counts down the characters still to be shown in
    // reverse video; i is the length of the message assembled so far
    unsigned char reverse = 0;
    int i = 0;

    // Count the error, and decide whether to use colour
    if (msg == NULL) msg = "";
    context->errorCount++; // Any error makes Pyxplot exit with non-zero status
    unsigned char ApplyHighlighting = ((context->session_default.color == SW_ONOFF_ON) && (isatty(STDERR_FILENO) == 1));
    // Copy the message into a static buffer, unless it is already there
    if (msg != temp_stringA) {
        ppl_snprintfTrunc(temp_stringA, BLEN, "%s", msg);
        temp_stringA[BLEN] = '\0';
        msg = temp_stringA;
    }

    // The message is assembled in temp_stringB, starting with an empty string
    temp_stringB[i] = temp_stringB[BLEN] = '\0';

    if (ErrType != ERR_PREFORMED) // Do not prepend anything to pre-formed errors
    {
        // When processing scripts, print error location
        if ((context->error_input_linenumber != -1) && (strcmp(context->error_input_filename, "") != 0)) {
            ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "%s:%d:", context->error_input_filename,
                              context->error_input_linenumber);
            i += strlen(temp_stringB + i);
            // Stacked errors continue a message on the following lines, so are not followed by a space
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

    // Copy the message, highlighting the characters at HighlightPos1 and HighlightPos2. Each highlight covers three
    // characters, e.g. the arrow /|\ which points to the position of a syntax error.
    for (int j = 0; msg[j] != '\0'; j++) {
        // Switch reverse video on at each highlighted position, and off three characters later
        if (ApplyHighlighting) {
            // Start of the first highlight
            if (j == HighlightPos1 - 1) {
                // Switch reverse video on, unless it is already on
                if (reverse == 0) {
                    ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "\x1b[7m");
                    i += strlen(temp_stringB + i);
                }
                reverse = 3;
                HighlightPos1 = -1;
            } else if (j == HighlightPos2 - 1) {
                // Start of the second highlight: switch reverse video on, unless it is already on
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
        // Append the character, if there is room
        if (i < BLEN) {
            // A highlighted newline is shown as a space, before the newline itself is copied on the next pass (hence
            // j--)
            if ((msg[j] == '\n') && (reverse > 0)) {
                temp_stringB[i++] = ' ';
                j--;
            } // If newline is highlighted, add spaces to end of line so that we can highlight them
            else
                temp_stringB[i++] = msg[j];
        }
    }
    // Make sure that reverse video is switched off at the end of the message
    if (ApplyHighlighting && (reverse != 0)) {
        ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "\x1b[27m");
        i += strlen(temp_stringB + i);
        reverse = 0;
    }
    if (i < BLEN) temp_stringB[i] = '\0';

    // Copy the message to the debugging log
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

//! ppl_error - Report an error to the user on stderr, prefixed with the location in the input where it occurred and
//! the type of error. If the terminal supports colour, the message is coloured, and up to two character positions
//! within it can be highlighted in reverse video.
//! Messages may be reported from several threads at once; only one thread writes a message at a time.
//! \param [in] context - The error-reporting context
//! \param [in] ErrType - The type of error, one of the ERR_* constants. ERR_PREFORMED messages are printed verbatim.
//! \param [in] HighlightPos1 - One-indexed position of the first character in <msg> to highlight, or -1 for none
//! \param [in] HighlightPos2 - One-indexed position of the second character in <msg> to highlight, or -1 for none
//! \param [in] msg - The error message (NULL is treated as an empty message)

void ppl_error(pplerr_context *context, const int ErrType, int HighlightPos1, int HighlightPos2, char *msg) {
    reportLock();
    errorUnlocked(context, ErrType, HighlightPos1, HighlightPos2, msg);
    reportUnlock();
}

//! ppl_fatal - Report a fatal internal error, and terminate the process
//! \param [in] context - The error-reporting context
//! \param [in] file - The source code file in which the error occurred
//! \param [in] line - The line number in the source code file where the error occurred
//! \param [in] msg - The error message (NULL is treated as an empty message)

void ppl_fatal(pplerr_context *context, char *file, int line, char *msg) {
    char introline[FNAME_LENGTH];
    if (msg == NULL) msg = "";
    // Copy the message into a static buffer, unless it is already there
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

//! warningUnlocked - The body of ppl_warning(), which is called with the lock held that lets only one thread report at a time

static void warningUnlocked(pplerr_context *context, const int ErrType, char *msg) {
    // i is the length of the message assembled so far
    int i = 0;

    // Treat a missing message as an empty one
    if (msg == NULL) msg = "";
    // Copy the message into a static buffer, unless it is already there
    if (msg != temp_stringA) {
        ppl_snprintfTrunc(temp_stringA, BLEN, "%s", msg);
        temp_stringA[BLEN - 1] = '\0';
        msg = temp_stringA;
    }

    // The message is assembled in temp_stringB, starting with an empty string
    temp_stringB[i] = temp_stringB[BLEN] = '\0';

    if (ErrType != ERR_PREFORMED) // Do not prepend anything to pre-formed errors
    {
        // When processing scripts, print error location
        if ((context->error_input_linenumber != -1) && (strcmp(context->error_input_filename, "") != 0)) {
            ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "%s:%d:", context->error_input_filename,
                              context->error_input_linenumber);
            i += strlen(temp_stringB + i);
            // Stacked warnings continue a message on the following lines, so are not followed by a space
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

    // Append the message itself
    ppl_snprintfTrunc(temp_stringB + i, BLEN - i, "%s", msg);

    // Copy the message to the debugging log
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

//! ppl_warning - Report a warning to the user on stderr, prefixed with the location in the input where it occurred
//! and the type of warning
//! Messages may be reported from several threads at once; only one thread writes a message at a time.
//! \param [in] context - The error-reporting context
//! \param [in] ErrType - The type of warning, one of the ERR_* constants. ERR_PREFORMED messages are printed
//! verbatim.
//! \param [in] msg - The warning message (NULL is treated as an empty message)

void ppl_warning(pplerr_context *context, const int ErrType, char *msg) {
    reportLock();
    warningUnlocked(context, ErrType, msg);
    reportUnlock();
}

//! reportUnlocked - The body of ppl_report(), which is called with the lock held that lets only one thread report at a time

static void reportUnlocked(pplerr_context *context, char *msg) {
    if (msg == NULL) msg = "";
    // Copy the message into a static buffer, unless it is already there
    if (msg != temp_stringA) {
        ppl_snprintfTrunc(temp_stringA, BLEN, "%s", msg);
        temp_stringA[BLEN - 1] = '\0';
    }
    // Copy the message to the debugging log
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

//! ppl_report - Print a report-level message to the user on stdout, coloured if the terminal supports it
//! Messages may be reported from several threads at once; only one thread writes a message at a time.
//! \param [in] context - The error-reporting context
//! \param [in] msg - The message (NULL is treated as an empty message)

void ppl_report(pplerr_context *context, char *msg) {
    reportLock();
    reportUnlocked(context, msg);
    reportUnlock();
}

//! logUnlocked - The body of ppl_log(), which is called with the lock held that lets only one thread report at a time

static void logUnlocked(pplerr_context *context, char *msg) {
    // The log file, opened on the first call, and a latch which stops this function from calling itself
    static FILE *logfile = NULL;
    static int latch = 0;
    char linebuffer[LSTR_LENGTH];

    if (latch) return; // Do not allow recursive calls, which might be generated by the call to ppl_fatal below
    latch = 1;
    // Open the log file on the first call, naming it after the process ID so that each of Pyxplot's processes has its
    // own
    if (logfile == NULL) {
        char LogFName[128];
        snprintf(LogFName, 127, "pyxplot.%d.log", getpid());
        LogFName[127] = '\0';
        // Failure to open the log file is fatal
        if ((logfile = fopen(LogFName, "w")) == NULL) {
            ppl_fatal(context, __FILE__, __LINE__, "Could not open log file to write.");
            exit(1);
        }
        setvbuf(logfile, NULL, _IOLBF, 0); // Set log file to be line-buffered, so that log file is always up-to-date
    }

    // Treat a missing message as an empty one
    if (msg == NULL) msg = "";
    // Copy the message into a static buffer, unless it is already there
    if (msg != temp_stringD) {
        ppl_snprintfTrunc(temp_stringD, BLEN, "%s", msg);
        temp_stringD[BLEN - 1] = '\0';
    }
    fprintf(logfile, "[%s] [%s] %s\n", ppl_strStrip(ppl_friendlyTimestring(), linebuffer), context->error_source,
            temp_stringD);
    latch = 0;
    return;
}

//! ppl_log - Write a message to the log file <pyxplot.PID.log> in the current working directory, prefixed with a
//! timestamp and the name of the process that produced it
//! Messages may be reported from several threads at once; only one thread writes a message at a time.
//! \param [in] context - The error-reporting context
//! \param [in] msg - The message (NULL is treated as an empty message)

void ppl_log(pplerr_context *context, char *msg) {
    reportLock();
    logUnlocked(context, msg);
    reportUnlock();
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
