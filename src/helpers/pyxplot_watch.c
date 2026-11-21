// pyxplot_watch.c
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

// The pyxplot_watch program, which is built and installed alongside Pyxplot. It watches a list of Pyxplot script files,
// and runs Pyxplot on each of them whenever it changes, so that plots are regenerated as scripts are edited. It shares
// Pyxplot's string, dictionary, memory and error-reporting code, but none of its interpreter.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <glob.h>
#include <sys/stat.h>

#include "coreUtils/dict.h"
#include "coreUtils/memAlloc.h"
#include "coreUtils/errorReport.h"

#include "stringTools/asciidouble.h"
#include "stringTools/strConstants.h"

#include "settings/settingTypes.h"
#include "userspace/context.h"

// Required by the shared string-handling code, which checks it while reading files; pyxplot_watch never sets it
int cancellationFlag = 0;

//! RunPyxplotOnFile - Run Pyxplot on a script file which pyxplot_watch has found to have changed
//! \param [in] context - The error-reporting context
//! \param [in] fname - The filename of the script

void RunPyxplotOnFile(pplerr_context *context, char *fname) {
    char msg[PPL_MSG_LENGTH];
    char LineBuffer[LSTR_LENGTH];

    // Progress messages are only shown in an interactive terminal, unless the user asked for quiet output
    int interactive = ((isatty(STDIN_FILENO) == 1) && (context->session_default.splash == SW_ONOFF_ON));

    // Announce that the script is being run
    if (interactive) {
        ppl_snprintfTrunc(LineBuffer, sizeof(LineBuffer), "[%s] Running %s.",
                          ppl_strStrip(ppl_friendlyTimestring(), msg), fname);
        ppl_report(context, LineBuffer);
    }

    // Run Pyxplot on the script, passing on the user's choice of coloured output
    ppl_snprintfTrunc(LineBuffer, sizeof(LineBuffer), "%s -q %s %s", PPLBINARY,
                      (context->session_default.color == SW_ONOFF_ON) ? "-c" : "-m", fname);
    int status = system(LineBuffer);

    // In a non-interactive session, report scripts which failed; in an interactive session, Pyxplot's own error
    // messages suffice
    if (status && !interactive) {
        ppl_snprintfTrunc(LineBuffer, sizeof(LineBuffer), "[%s] Encountered problem in script file %s.",
                          ppl_strStrip(ppl_friendlyTimestring(), msg), fname);
        ppl_error(context, ERR_PREFORMED, -1, -1, LineBuffer);
    }

    // Announce that the script has finished
    if (interactive) {
        ppl_snprintfTrunc(LineBuffer, sizeof(LineBuffer), "[%s] Finished %s.",
                          ppl_strStrip(ppl_friendlyTimestring(), msg), fname);
        ppl_report(context, LineBuffer);
    }
    return;
}

//! main - Entry point for pyxplot_watch, which watches a list of script files and runs Pyxplot on each of them
//! whenever it is modified
//! \param [in] argc - The number of command-line arguments
//! \param [in] argv - The command-line arguments: options, and the filenames (which may contain wildcards) to watch
//! \return The exit status of the process

int main(const int argc, char **argv) {
    // Buffers for messages, the welcome text, the help text and the version string
    char msg[PPL_MSG_LENGTH];
    struct timespec waitperiod, waitedperiod; // A time.h timespec specifier; used for sleeping
    char init_string[LSTR_LENGTH], help_string[LSTR_LENGTH], version_string[FNAME_LENGTH],
            version_string_underline[FNAME_LENGTH];
    glob_t GlobData;

    // A dictionary of the stat() information of each file, recording when each was last modified
    struct stat StatInfo;

    // Initialise sub-modules
    pplerr_context *context = malloc(sizeof(pplerr_context));
    // Set up the error-reporting context, with no input file being processed
    if (context == NULL) {
        printf("malloc fail.");
        return 1;
    }
    context->error_input_linenumber = -1;
    context->errorCount = 0;
    context->error_input_filename[0] = '\0';
    strcpy(context->error_source, "main     ");

    // Start the memory management system, and create the dictionary of file modification times
    if (DEBUG) ppl_log(context, "Initialising Pyxplot Watch.");
    ppl_memAlloc_MemoryInit(context, &ppl_error, &ppl_log);
    dict *StatInfodict = ppl_dictInit(1);

    // Set up default session settings
    context->session_default.splash = SW_ONOFF_ON;
    context->session_default.color = SW_ONOFF_ON;
    context->session_default.color_rep = SW_TERMCOL_GRN;
    context->session_default.color_wrn = SW_TERMCOL_BRN;
    context->session_default.color_err = SW_TERMCOL_RED;

    // Make help and version strings
    ppl_snprintfTrunc(version_string, sizeof(version_string), "\nPyxplot Watch %s\n", VERSION);

    // The text shown by the --help switch
    ppl_snprintfTrunc(help_string, sizeof(help_string), "%s\
%s\n\
\n\
Usage: pyxplot_watch <options> <filelist>\n\
  -h, --help:       Display this help.\n\
  -v, --version:    Display version number.\n\
  -q, --quiet:      Turn off initial welcome message.\n\
  -V, --verbose:    Turn on initial welcome message.\n\
  -c, --color:     Use colored highlighting of output.\n\
  -m, --monochrome: Turn off colored highlighting.\n\
\n\
A brief introduction to Pyxplot can be obtained by typing 'man pyxplot'; the\n\
full Users' Guide can be found in the file:\n\
%s%spyxplot.pdf\n\
\n\
For the latest information on Pyxplot development, see the project website:\n\
<http://www.pyxplot.org.uk>\n",
                      version_string, ppl_strUnderline(version_string, version_string_underline), DOCDIR, PATHLINK);

    // The welcome text shown on startup
    ppl_snprintfTrunc(init_string, sizeof(init_string), "\n\
                        _       _      PYXPLOT WATCH\n\
 _ __  _   ___  ___ __ | | ___ | |_    Version %s\n\
| '_ \\| | | \\ \\/ / '_ \\| |/ _ \\| __|   %s\n\
| |_) | |_| |>  <| |_) | | (_) | |_\n\
| .__/ \\__, /_/\\_\\ .__/|_|\\___/ \\__|   Copyright (C) 2006-2026 Dominic Ford\n\
|_|    |___/     |_|                                 2008-2013 Ross Church\n\
\n\
For documentation and more information, see <http://www.pyxplot.org.uk>.\n\
",
                      VERSION, DATE);

    // Scan command-line options for any switches
    int HaveFilenames = 0;
    // Process each command-line argument in turn
    for (int i = 1; i < argc; i++) {
        if (strlen(argv[i]) == 0) continue;
        // Arguments which are not switches are the filenames to watch
        if (argv[i][0] != '-') {
            HaveFilenames = 1;
            continue;
        }
        if (strcmp(argv[i], "-q") == 0) context->session_default.splash = SW_ONOFF_OFF;
        else if (strcmp(argv[i], "-quiet") == 0) context->session_default.splash = SW_ONOFF_OFF;
        else if (strcmp(argv[i], "--quiet") == 0) context->session_default.splash = SW_ONOFF_OFF;
        else if (strcmp(argv[i], "-V") == 0) context->session_default.splash = SW_ONOFF_ON;
        else if (strcmp(argv[i], "-verbose") == 0) context->session_default.splash = SW_ONOFF_ON;
        else if (strcmp(argv[i], "--verbose") == 0) context->session_default.splash = SW_ONOFF_ON;
        else if (strcmp(argv[i], "-c") == 0) context->session_default.color = SW_ONOFF_ON;
        else if (strcmp(argv[i], "-colour") == 0) context->session_default.color = SW_ONOFF_ON;
        else if (strcmp(argv[i], "--colour") == 0) context->session_default.color = SW_ONOFF_ON;
        else if (strcmp(argv[i], "-color") == 0) context->session_default.color = SW_ONOFF_ON;
        else if (strcmp(argv[i], "--color") == 0) context->session_default.color = SW_ONOFF_ON;
        else if (strcmp(argv[i], "-m") == 0) context->session_default.color = SW_ONOFF_OFF;
        else if (strcmp(argv[i], "-mono") == 0) context->session_default.color = SW_ONOFF_OFF;
        else if (strcmp(argv[i], "--mono") == 0) context->session_default.color = SW_ONOFF_OFF;
        else if (strcmp(argv[i], "-monochrome") == 0) context->session_default.color = SW_ONOFF_OFF;
        else if (strcmp(argv[i], "--monochrome") == 0) context->session_default.color = SW_ONOFF_OFF;
        else if ((strcmp(argv[i], "-v") == 0) || (strcmp(argv[i], "-version") == 0) ||
                 (strcmp(argv[i], "--version") == 0)) {
            ppl_report(context, version_string);
            if (DEBUG) ppl_log(context, "Reported version number as requested.");
            ppl_memAlloc_FreeAll(0);
            ppl_memAlloc_MemoryStop();
            return 0;
        } else if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "-help") == 0) ||
                   (strcmp(argv[i], "--help") == 0)) {
            ppl_report(context, help_string);
            if (DEBUG) ppl_log(context, "Reported help text as requested.");
            ppl_memAlloc_FreeAll(0);
            ppl_memAlloc_MemoryStop();
            return 0;
        } else {
            ppl_snprintfTrunc(
                    msg, sizeof(msg),
                    "Received switch '%s' which was not recognised. Type 'pyxplot_watch -help' for a list of available command-line options.",
                    argv[i]);
            ppl_error(context, ERR_PREFORMED, -1, -1, msg);
            if (DEBUG) ppl_log(context, "Received unexpected command-line switch.");
            ppl_memAlloc_FreeAll(0);
            ppl_memAlloc_MemoryStop();
            return 1;
        }
    }

    // Produce splash text
    if ((isatty(STDIN_FILENO) == 1) && (context->session_default.splash == SW_ONOFF_ON))
        ppl_report(context, init_string);

    // Check that we have some filenames to watch
    if (!HaveFilenames) {
        ppl_error(
                context, ERR_PREFORMED, -1, -1,
                "No filenames were supplied to watch. Pyxplot Watch's command-line syntax is:\n\npyxplot_watch [options] filename_list\n\nAs Pyxplot Watch has no work to do, it is exiting...");
        exit(1);
    }

    // Scan command-line options and glob all filenames
    while (1) {
        int DoneWork = 0;
        // Expand each filename argument, which may contain wildcards
        for (int i = 1; i < argc; i++) {
            if ((argv[i][0] == '\0') || (argv[i][0] == '-')) continue;
            if (glob(argv[i], 0, NULL, &GlobData) != 0) continue;

            // Run Pyxplot on each matching file which is new, or has been modified since we last ran it, pausing
            // briefly after each one
            for (int j = 0; j < GlobData.gl_pathc; j++) {
                if (stat(GlobData.gl_pathv[j], &StatInfo) != 0) continue; // Stat didn't work...
                struct stat *StatInfoPtr = (struct stat *) ppl_dictLookup(StatInfodict, GlobData.gl_pathv[j]);
                if ((StatInfoPtr != NULL) && (StatInfoPtr->st_mtime >= StatInfo.st_mtime))
                    continue; // This file has not been modified lately
                ppl_dictAppendCpy(StatInfodict, GlobData.gl_pathv[j], (void *) &StatInfo, sizeof(struct stat));
                DoneWork = 1;
                RunPyxplotOnFile(context, GlobData.gl_pathv[j]);
                waitperiod.tv_sec = 0;
                waitperiod.tv_nsec = 100000000;
                nanosleep(&waitperiod, &waitedperiod);
            }

            // Free the list of filenames matching this argument
            globfree(&GlobData);
        }
        // If no files needed running, wait a second before looking again
        if (!DoneWork) {
            waitperiod.tv_sec = 1;
            waitperiod.tv_nsec = 0;
            nanosleep(&waitperiod, &waitedperiod);
        }
    }

    // Never get here...
    free(context);
    context = NULL;
    return 0;
}
